#include "pch.h"
#include "engine_tick.h"
#include "tick_profiler.h"
#include "stack_sampler.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "../world_begin_play/world_begin_play.h"
#include "utils/game_thread_dispatch.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace Hooks::EngineTick
{
	// UGameEngine::Tick(UGameEngine* this, float DeltaSeconds, bool bIdleMode)
	using UGameEngine_Tick_t = void(__fastcall*)(void* thisPtr, float deltaSeconds, bool bIdleMode);

	static Hook g_hook;
	static UGameEngine_Tick_t g_original = nullptr;

	// Registered plugin callbacks
	static std::vector<PluginEngineTickCallback> g_pluginCallbacks;

	// ---------------------------------------------------------------------
	// Stutter detection: times the original tick call and logs outliers
	// against a slow-moving average, so spikes show up in modloader.log
	// without needing an external profiler attached.
	//
	// Only runs while ChimeraMain (the actual gameplay world) is ticking --
	// menus and loading screens have their own tick cost profile and would
	// just pollute the average / trigger false positives.
	// ---------------------------------------------------------------------
	static LARGE_INTEGER g_qpcFrequency{};
	static uint64_t g_tickIndex = 0;
	static double g_avgTickMs = 0.0;
	static bool g_inChimeraMain = false;
	static bool g_stutterLoggingEnabled = false;
	static bool g_stackSamplingEnabled = false;

	static constexpr uint64_t kWarmupTicks = 60;      // ticks to skip while the average stabilizes
	static constexpr double kEmaAlpha = 0.05;         // smoothing factor for the rolling average
	static constexpr double kStutterMultiplier = 2.5; // flag ticks this many times over average
	static constexpr double kStutterMinMs = 25.0;     // ...but never below this floor

	static void OnAnyWorldBeginPlay(SDK::UWorld* /*world*/, const char* worldName)
	{
		const bool nowInChimeraMain = worldName && strstr(worldName, "ChimeraMain") != nullptr;

		// Only reset when (re-)entering ChimeraMain from some other state, so we
		// don't wipe the running average on every menu/loading-screen world change
		// (irrelevant, since stats aren't collected outside ChimeraMain anyway),
		// and don't reset again if ChimeraMain fires more than once while already active.
		if (nowInChimeraMain && !g_inChimeraMain)
		{
			g_tickIndex = 0;
			g_avgTickMs = 0.0;
			TickProfiler::Reset();
		}

		g_inChimeraMain = nowInChimeraMain;
	}

	static void __fastcall Detour(void* thisPtr, float deltaSeconds, bool bIdleMode)
	{
		// Snapshot once -- g_stutterLoggingEnabled can be flipped from the UI/render
		// thread at any moment. Reading it separately at tick-start and tick-end could
		// observe different values within the same tick, skipping the start capture
		// (tickStart/g_qpcFrequency left uninitialized) while still running the
		// tick-end math, producing an inf/NaN that poisons the average forever.
		const bool profileThisTick = g_inChimeraMain && g_stutterLoggingEnabled;
		// Stack sampling is opt-in and heavier (suspends/resumes the game
		// thread repeatedly) -- only runs when the user has explicitly
		// turned it on, on top of plain stutter logging.
		const bool sampleThisTick = profileThisTick && g_stackSamplingEnabled;

		if (profileThisTick && g_qpcFrequency.QuadPart == 0)
			QueryPerformanceFrequency(&g_qpcFrequency);

		LARGE_INTEGER tickStart{}, engineStart{}, engineEnd{}, dispatchStart{}, dispatchEnd{};
		if (profileThisTick)
		{
			QueryPerformanceCounter(&tickStart);
			engineStart = tickStart;
		}

		if (sampleThisTick)
			StackSampler::BeginWindow();

		// Call original first so the engine tick completes before we notify plugins
		if (g_original)
		{
			g_original(thisPtr, deltaSeconds, bIdleMode);
		}

		if (sampleThisTick)
			StackSampler::EndWindow();

		if (profileThisTick)
		{
			QueryPerformanceCounter(&engineEnd);
			dispatchStart = engineEnd;
		}

		// Drain any tasks posted to the game thread from background threads
		GameThreadDispatch::Drain();

		if (profileThisTick)
			QueryPerformanceCounter(&dispatchEnd);

		// Per-plugin-callback timing, reused across ticks to avoid reallocating;
		// only populated (and only meaningful) while profiling this tick.
		static std::vector<double> s_callbackMs;
		if (profileThisTick)
			s_callbackMs.assign(g_pluginCallbacks.size(), 0.0);

		for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
		{
			if (g_pluginCallbacks[i])
			{
				LARGE_INTEGER cbStart{}, cbEnd{};
				if (profileThisTick)
					QueryPerformanceCounter(&cbStart);

				try
				{
					g_pluginCallbacks[i](deltaSeconds);
				}
				catch (...)
				{
					// Swallow exceptions to avoid crashing the main loop.
					// Log only once per callback to avoid spam.
					ModLoaderLogger::LogError(L"[EngineTick::Detour] Exception caught in callback %zu", i + 1);
				}

				if (profileThisTick)
				{
					QueryPerformanceCounter(&cbEnd);
					s_callbackMs[i] = (cbEnd.QuadPart - cbStart.QuadPart) * 1000.0 / static_cast<double>(g_qpcFrequency.QuadPart);
				}
			}
			else
			{
				ModLoaderLogger::LogWarn(L"[EngineTick::Detour] Callback %zu is NULL", i + 1);
			}
		}

		if (profileThisTick)
		{
			LARGE_INTEGER tickEnd{};
			QueryPerformanceCounter(&tickEnd);

			const double freq       = static_cast<double>(g_qpcFrequency.QuadPart);
			const double tickMs     = (tickEnd.QuadPart     - tickStart.QuadPart)     * 1000.0 / freq;
			const double engineMs   = (engineEnd.QuadPart   - engineStart.QuadPart)   * 1000.0 / freq;
			const double dispatchMs = (dispatchEnd.QuadPart - dispatchStart.QuadPart) * 1000.0 / freq;

			++g_tickIndex;

			bool isStutter = false;
			double avgAtCapture;

			// Also re-seed if a prior corrupted value ever slipped through, so the
			// detector self-heals within one tick instead of staying poisoned.
			if (g_tickIndex == 1 || !std::isfinite(g_avgTickMs))
			{
				g_avgTickMs = tickMs;
				avgAtCapture = g_avgTickMs;
			}
			else
			{
				avgAtCapture = g_avgTickMs;

				if (g_tickIndex > kWarmupTicks)
				{
					const double threshold = std::max(kStutterMinMs, g_avgTickMs * kStutterMultiplier);
					if (tickMs > threshold)
					{
						isStutter = true;
						ModLoaderLogger::LogWarn(L"[EngineTick] Stutter on tick %llu: %.2fms (avg %.2fms, delta %.4fs)",
						                          static_cast<unsigned long long>(g_tickIndex), tickMs, g_avgTickMs, deltaSeconds);
					}
				}

				g_avgTickMs += (tickMs - g_avgTickMs) * kEmaAlpha;
			}

			// Only worth resolving per-callback module names / building the
			// call tree when the tick is actually going into the stutter
			// list -- skipped on every normal tick.
			std::vector<TickProfiler::SegmentTiming> callbackTimings;
			std::vector<TickProfiler::CallTreeNode> callTree;
			size_t sampleCount = 0;
			if (isStutter)
			{
				callbackTimings.reserve(g_pluginCallbacks.size());
				for (size_t i = 0; i < g_pluginCallbacks.size() && i < s_callbackMs.size(); ++i)
				{
					if (!g_pluginCallbacks[i])
						continue;

					callbackTimings.push_back({
						Hooks::GetCallerModuleName(reinterpret_cast<void*>(g_pluginCallbacks[i])),
						s_callbackMs[i] });
				}

				if (sampleThisTick)
				{
					callTree = StackSampler::GetLastWindowCallTree();
					sampleCount = StackSampler::GetLastWindowSampleCount();
				}
			}

			TickProfiler::RecordTick(g_tickIndex, tickMs, avgAtCapture, deltaSeconds,
			                          isStutter, engineMs, dispatchMs, callbackTimings,
			                          callTree, sampleCount);
		}
	}

	bool Install()
	{
		if (g_hook.installed)
			return true;

		ModLoaderLogger::LogInfo(L"[EngineTick] Installing UGameEngine::Tick hook...");

		const char* pattern = ScanPatterns::UGameEngine_Tick;

		ModLoaderLogger::LogDebug(L"[EngineTick]   Pattern: %S", pattern);

		uintptr_t addr = Scanner::FindPatternInMainModule("UGameEngine::Tick", pattern);

		if (!addr)
		{
			ModLoaderLogger::LogError(L"[EngineTick] UGameEngine::Tick pattern not found");
			return false;
		}

		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto base = reinterpret_cast<uintptr_t>(mainModule);

		ModLoaderLogger::LogInfo(L"[EngineTick] UGameEngine::Tick found at 0x%llX (base+0x%llX)",
		                         static_cast<unsigned long long>(addr),
		                         static_cast<unsigned long long>(addr - base));

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
		{
			ModLoaderLogger::LogInfo(L"[EngineTick] Hook installed successfully");
			Hooks::WorldBeginPlay::RegisterAnyWorldCallback(&OnAnyWorldBeginPlay);
		}
		else
			ModLoaderLogger::LogError(L"[EngineTick] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[EngineTick] Removing hook...");
		g_hook.Remove();
		g_pluginCallbacks.clear();
		g_tickIndex = 0;
		g_avgTickMs = 0.0;
		g_inChimeraMain = false;
		TickProfiler::Reset();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	void RegisterPluginCallback(PluginEngineTickCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[EngineTick] RegisterPluginCallback: null callback provided");
			return;
		}

		// Lazily install the hook on first registration
		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[EngineTick] First callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[EngineTick] Failed to install hook for engine tick callback!");
				return;
			}
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[EngineTick] Plugin callback registered (%zu total)", g_pluginCallbacks.size());
	}

	void UnregisterPluginCallback(PluginEngineTickCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[EngineTick] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}

	void SetStutterLoggingEnabled(bool enabled)
	{
		g_stutterLoggingEnabled = enabled;
	}

	bool IsStutterLoggingEnabled()
	{
		return g_stutterLoggingEnabled;
	}

	void SetStackSamplingEnabled(bool enabled)
	{
		g_stackSamplingEnabled = enabled;
	}

	bool IsStackSamplingEnabled()
	{
		return g_stackSamplingEnabled;
	}

	uintptr_t GetOriginalPtr()
	{
		return reinterpret_cast<uintptr_t>(g_original);
	}
}
