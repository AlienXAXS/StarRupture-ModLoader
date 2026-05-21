#include "pch.h"
#include "engine_init.h"
#include "logging/logger.h"
#include "logging/log.h"
#include "memory_scanner/scanner.h"
#include "hooks/memory/engine_allocator.h"
#include "hooks/game/mass_spawner_activate/mass_spawner_activate.h"
#include "hooks/game/mass_spawner_deactivate/mass_spawner_deactivate.h"
#include "hooks/game/mass_do_spawning/mass_do_spawning.h"
#include <vector>
#include <algorithm>
#include "../scan_patterns.h"

// ---------------------------------------------------------------------------
// SEH crash diagnostics
//
// Wraps a call to the original FEngineLoop::Init in a structured exception
// handler so we can log the faulting address and register state before the
// process terminates.  Must be a standalone function with no C++ objects
// that have destructors in scope (MSVC C2712 restriction).
// ---------------------------------------------------------------------------

// POD context captured inside the SEH filter -- no destructor, safe inside __try.
struct EngineCrashContext
{
	DWORD    code;
	void*    address;
	ULONG64  rip, rsp, rbp;
	ULONG64  rcx, rdx, r8, r9;
	ULONG64  r10, r11;
	bool     captured;
};

static LONG CrashFilter(EXCEPTION_POINTERS* ep, EngineCrashContext* ctx)
{
	ctx->code    = ep->ExceptionRecord->ExceptionCode;
	ctx->address = ep->ExceptionRecord->ExceptionAddress;
	ctx->rip = ep->ContextRecord->Rip;
	ctx->rsp = ep->ContextRecord->Rsp;
	ctx->rbp = ep->ContextRecord->Rbp;
	ctx->rcx = ep->ContextRecord->Rcx;
	ctx->rdx = ep->ContextRecord->Rdx;
	ctx->r8  = ep->ContextRecord->R8;
	ctx->r9  = ep->ContextRecord->R9;
	ctx->r10 = ep->ContextRecord->R10;
	ctx->r11 = ep->ContextRecord->R11;
	ctx->captured = true;
	return EXCEPTION_EXECUTE_HANDLER;
}

// Returns the original's return value, or -1 on exception.
// ctx->captured will be true if an exception was caught.
static int32_t CallEngineLoopInitSEH(Hooks::EngineInit::FEngineLoop_Init_t fn, void* thisPtr,
                                     EngineCrashContext* ctx)
{
	__try
	{
		return fn(thisPtr);
	}
	__except (CrashFilter(GetExceptionInformation(), ctx))
	{
		return -1;
	}
}

// Same wrapper for the UGameEngine::Init fallback hook (bool return).
static bool CallGameEngineInitSEH(Hooks::EngineInit::UGameEngine_Init_t fn, void* thisPtr,
                                  void* inEngineLoop, EngineCrashContext* ctx)
{
	__try
	{
		return fn(thisPtr, inEngineLoop);
	}
	__except (CrashFilter(GetExceptionInformation(), ctx))
	{
		return false;
	}
}

namespace Hooks::EngineInit
{
	// Hook objects for multiple initialization points
	static Hook g_engineLoopHook;
	static Hook g_gameEngineHook;

	// Original function pointers
	static FEngineLoop_Init_t g_engineLoopOriginal = nullptr;
	static UGameEngine_Init_t g_gameEngineOriginal = nullptr;

	static bool g_engineInitialized = false;
	static long g_callCount = 0;

	// Sync handles set before Install() is called.
	static HANDLE g_engineReadyEventHandle = nullptr;
	static HANDLE g_pluginsLoadedEventHandle = nullptr;
	static HANDLE g_pluginsReadyEventHandle = nullptr;

	// Signalled at the very end of each detour (after NotifyEngineReady returns)
	// so the UE4SS loader thread wakes up only once the hook call-stack has
	// fully unwound and the engine is in a stable, quiescent state.
	static HANDLE g_ue4ssReadyEventHandle = nullptr;

	// Callback for plugins to receive engine init events
	static std::vector<PluginEngineInitCallback> g_pluginCallbacks;

	// Signal the init thread that the engine is ready, then block the main
	// thread until all plugins have been initialised.
	static void WaitForPluginsToLoad()
	{
		if (g_engineReadyEventHandle)
			SetEvent(g_engineReadyEventHandle);

		if (g_pluginsReadyEventHandle)
		{
			LogToFile::Info("[EngineInit] Main thread sleeping -- waiting for plugins to initialise");
			const DWORD result = WaitForSingleObject(g_pluginsReadyEventHandle, 120000);
			if (result == WAIT_TIMEOUT)
				LogToFile::Warn("[EngineInit] Timed out waiting for plugins -- resuming main thread anyway");
			else
				LogToFile::Info("[EngineInit] Plugins ready -- main thread resuming");
		}
	}

	// Shared notification function - called by any successful hook
	static void NotifyEngineReady(const wchar_t* hookSource)
	{
		// Only notify once, regardless of which hook fires first
		if (g_engineInitialized)
		{
			return;
		}

		g_engineInitialized = true;

		ModLoaderLogger::LogInfo(L"[EngineInit] *** ENGINE READY *** (via %s)", hookSource);

		// Resolve the engine allocator (FMemory::Malloc / FMemory::Free) now
		// that the engine is fully initialised.  This must happen before plugin
		// callbacks fire so they can use IPluginHooks::EngineAlloc / EngineFree.
		if (!EngineAllocator::Resolve())
		{
			ModLoaderLogger::LogWarn(L"[EngineInit] Engine allocator resolution failed - "
				L"plugins will not be able to use EngineAlloc/EngineFree");
		}

		if (!g_pluginCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[EngineInit] Notifying %zu plugin(s)...", g_pluginCallbacks.size());

			for (size_t i = 0; i < g_pluginCallbacks.size(); ++i)
			{
				if (!g_pluginCallbacks[i])
					continue;

				// Safe invocation of plugin callbacks
				ModLoaderLogger::LogTrace(L"[EngineInit]   Calling plugin callback #%zu", i + 1);

				try
				{
					g_pluginCallbacks[i]();
				}
				catch (const std::exception& e)
				{
					ModLoaderLogger::LogError(L"[EngineInit] Exception in callback: %S", e.what());
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[EngineInit] Unknown exception in callback");
				}
			}

			ModLoaderLogger::LogDebug(L"[EngineInit] All plugin callbacks completed");
		}
	}

	void TriggerEngineReadyFallback(const wchar_t* reason)
	{
		NotifyEngineReady(reason);
		WaitForPluginsToLoad();
	}

	// Hook 1: FEngineLoop::Init (primary)
	static int32_t __fastcall FEngineLoop_Init_Detour(void* thisPtr)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogInfo(L"[EngineInit] FEngineLoop::Init called (#%ld)", callNum);
		ModLoaderLogger::LogDebug(L"[EngineInit]   FEngineLoop=%p, Thread=%lu", thisPtr, GetCurrentThreadId());

		// Call original under SEH so any crash is logged with full register context
		int32_t result = 0;
		if (g_engineLoopOriginal)
		{
			ModLoaderLogger::LogDebug(L"[EngineInit]   Calling original FEngineLoop::Init...");

			EngineCrashContext crashCtx{};
			result = CallEngineLoopInitSEH(g_engineLoopOriginal, thisPtr, &crashCtx);

			if (crashCtx.captured)
			{
				HMODULE mainMod = GetModuleHandleW(nullptr);
				auto base = reinterpret_cast<ULONG64>(mainMod);
				ModLoaderLogger::LogError(L"[EngineInit] *** CRASH INSIDE FEngineLoop::Init ***");
				ModLoaderLogger::LogError(L"[EngineInit]   Exception : 0x%08lX", crashCtx.code);
				ModLoaderLogger::LogError(L"[EngineInit]   Fault addr: %p  (exe+0x%llX)",
				                          crashCtx.address,
				                          reinterpret_cast<ULONG64>(crashCtx.address) - base);
				ModLoaderLogger::LogError(L"[EngineInit]   RIP=0x%016llX  (exe+0x%llX)",
				                          crashCtx.rip, crashCtx.rip - base);
				ModLoaderLogger::LogError(L"[EngineInit]   RSP=0x%016llX  RBP=0x%016llX",
				                          crashCtx.rsp, crashCtx.rbp);
				ModLoaderLogger::LogError(L"[EngineInit]   RCX=0x%016llX  RDX=0x%016llX",
				                          crashCtx.rcx, crashCtx.rdx);
				ModLoaderLogger::LogError(L"[EngineInit]   R8 =0x%016llX  R9 =0x%016llX",
				                          crashCtx.r8,  crashCtx.r9);
				ModLoaderLogger::LogError(L"[EngineInit]   R10=0x%016llX  R11=0x%016llX",
				                          crashCtx.r10, crashCtx.r11);
			}
			else
			{
				ModLoaderLogger::LogDebug(L"[EngineInit]   Original returned: %d", result);
			}
		}
		else
		{
			ModLoaderLogger::LogError(L"[EngineInit] Original function pointer is null!");
		}

		// Notify plugins that engine is ready, then sleep the main thread until
		// the init thread has finished initialising all plugins.
		NotifyEngineReady(L"FEngineLoop::Init");
		WaitForPluginsToLoad();

		// Signal UE4SS loader thread: hook call-stack is fully unwound after
		// this point, so it is safe to call LoadLibraryW(ue4ss.dll).
		if (g_ue4ssReadyEventHandle)
			SetEvent(g_ue4ssReadyEventHandle);

		ModLoaderLogger::LogDebug(L"[EngineInit] FEngineLoop::Init complete (#%ld)", callNum);
		return result;
	}

	// Hook 2: UGameEngine::Init (fallback)
	static bool __fastcall UGameEngine_Init_Detour(void* thisPtr, void* InEngineLoop)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogInfo(L"[EngineInit] UGameEngine::Init called (#%ld)", callNum);
		ModLoaderLogger::LogDebug(L"[EngineInit]   GameEngine=%p, EngineLoop=%p, Thread=%lu",
		                          thisPtr, InEngineLoop, GetCurrentThreadId());

		// Call original under SEH so any crash deep inside (e.g. during UEngine::Browse
		// or WorldBeginPlay subsystem hooks) is caught and logged with full register context.
		bool result = false;
		if (g_gameEngineOriginal)
		{
			ModLoaderLogger::LogDebug(L"[EngineInit]   Calling original UGameEngine::Init...");

			EngineCrashContext crashCtx{};
			result = CallGameEngineInitSEH(g_gameEngineOriginal, thisPtr, InEngineLoop, &crashCtx);

			if (crashCtx.captured)
			{
				HMODULE mainMod = GetModuleHandleW(nullptr);
				auto base = reinterpret_cast<ULONG64>(mainMod);
				ModLoaderLogger::LogError(L"[EngineInit] *** CRASH INSIDE UGameEngine::Init ***");
				ModLoaderLogger::LogError(L"[EngineInit]   Exception : 0x%08lX", crashCtx.code);
				ModLoaderLogger::LogError(L"[EngineInit]   Fault addr: %p  (exe+0x%llX)",
				                          crashCtx.address,
				                          reinterpret_cast<ULONG64>(crashCtx.address) - base);
				ModLoaderLogger::LogError(L"[EngineInit]   RIP=0x%016llX  (exe+0x%llX)",
				                          crashCtx.rip, crashCtx.rip - base);
				ModLoaderLogger::LogError(L"[EngineInit]   RSP=0x%016llX  RBP=0x%016llX",
				                          crashCtx.rsp, crashCtx.rbp);
				ModLoaderLogger::LogError(L"[EngineInit]   RCX=0x%016llX  RDX=0x%016llX",
				                          crashCtx.rcx, crashCtx.rdx);
				ModLoaderLogger::LogError(L"[EngineInit]   R8 =0x%016llX  R9 =0x%016llX",
				                          crashCtx.r8,  crashCtx.r9);
				ModLoaderLogger::LogError(L"[EngineInit]   R10=0x%016llX  R11=0x%016llX",
				                          crashCtx.r10, crashCtx.r11);
			}
			else
			{
				ModLoaderLogger::LogDebug(L"[EngineInit]   Original returned: %s", result ? L"true" : L"false");
			}
		}
		else
		{
			ModLoaderLogger::LogError(L"[EngineInit] Original function pointer is null!");
		}

		// Notify plugins that engine is ready, then sleep the main thread until
		// the init thread has finished initialising all plugins.
		NotifyEngineReady(L"UGameEngine::Init");
		WaitForPluginsToLoad();

		// Signal UE4SS loader thread: hook call-stack is fully unwound after
		// this point, so it is safe to call LoadLibraryW(ue4ss.dll).
		if (g_ue4ssReadyEventHandle)
			SetEvent(g_ue4ssReadyEventHandle);

		ModLoaderLogger::LogDebug(L"[EngineInit] UGameEngine::Init complete (#%ld)", callNum);
		return result;
	}

	void SetSyncEvents(HANDLE engineReadyEvent, HANDLE pluginsLoadedEvent)
	{
		g_engineReadyEventHandle = engineReadyEvent;
		g_pluginsLoadedEventHandle = pluginsLoadedEvent;
	}

	void SetUE4SSReadyEvent(HANDLE ue4ssReadyEvent)
	{
		g_ue4ssReadyEventHandle = ue4ssReadyEvent;
	}

	void SetPluginsReadyEvent(HANDLE pluginsReadyEvent)
	{
		g_pluginsReadyEventHandle = pluginsReadyEvent;
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[EngineInit] Installing engine initialization hooks...");

		bool anyHookSucceeded = false;

		// Try Hook 1: FEngineLoop::Init (primary)
		{
			const char* pattern = ScanPatterns::FEngineLoop_Init;


			ModLoaderLogger::LogInfo(L"[EngineInit] Scanning for FEngineLoop::Init...");
			ModLoaderLogger::LogDebug(L"[EngineInit]   Pattern: %S", pattern);

			uintptr_t addr = Scanner::FindPatternInMainModule("FEngineLoop::Init", pattern);

			if (addr)
			{
				HMODULE mainModule = GetModuleHandleW(nullptr);
				auto base = reinterpret_cast<uintptr_t>(mainModule);

				ModLoaderLogger::LogDebug(L"[EngineInit] [OK] FEngineLoop::Init found at 0x%llX (base+0x%llX)",
				                          static_cast<unsigned long long>(addr),
				                          static_cast<unsigned long long>(addr - base));

				bool hookOk = g_engineLoopHook.Install(
					addr,
					reinterpret_cast<void*>(&FEngineLoop_Init_Detour),
					reinterpret_cast<void**>(&g_engineLoopOriginal));

				if (hookOk)
				{
					ModLoaderLogger::LogInfo(L"[EngineInit] [OK] FEngineLoop::Init hook installed successfully");
					anyHookSucceeded = true;
				}
				else
				{
					ModLoaderLogger::LogWarn(L"[EngineInit] [FAIL] FEngineLoop::Init hook installation failed");
				}
			}
			else
			{
				ModLoaderLogger::LogWarn(
					L"[EngineInit] [FAIL] FEngineLoop::Init pattern not found - will try fallback");
			}
		}

		// Try Hook 2: UGameEngine::Init (fallback)
		{
			const char* pattern = ScanPatterns::UGameEngine_Init;


			ModLoaderLogger::LogInfo(L"[EngineInit] Scanning for UGameEngine::Init (fallback)...");
			ModLoaderLogger::LogDebug(L"[EngineInit]   Pattern: %S", pattern);

			uintptr_t addr = Scanner::FindPatternInMainModule("UGameEngine::Init", pattern);

			if (addr)
			{
				HMODULE mainModule = GetModuleHandleW(nullptr);
				auto base = reinterpret_cast<uintptr_t>(mainModule);

				ModLoaderLogger::LogDebug(L"[EngineInit] [OK] UGameEngine::Init found at 0x%llX (base+0x%llX)",
				                          static_cast<unsigned long long>(addr),
				                          static_cast<unsigned long long>(addr - base));

				bool hookOk = g_gameEngineHook.Install(
					addr,
					reinterpret_cast<void*>(&UGameEngine_Init_Detour),
					reinterpret_cast<void**>(&g_gameEngineOriginal));

				if (hookOk)
				{
					ModLoaderLogger::LogInfo(L"[EngineInit] [OK] UGameEngine::Init hook installed successfully");
					anyHookSucceeded = true;
				}
				else
				{
					ModLoaderLogger::LogWarn(L"[EngineInit] [FAIL] UGameEngine::Init hook installation failed");
				}
			}
			else
			{
				ModLoaderLogger::LogWarn(L"[EngineInit] [FAIL] UGameEngine::Init pattern not found");
			}
		}

		// Final status
		if (anyHookSucceeded)
		{
			ModLoaderLogger::LogInfo(
				L"[EngineInit] At least one engine init hook installed - engine ready detection active");
		}
		else
		{
			ModLoaderLogger::LogError(L"[EngineInit] CRITICAL: No engine init hooks could be installed!");
			ModLoaderLogger::LogError(L"[EngineInit] Plugins requiring engine init callbacks will NOT work!");
		}

		return anyHookSucceeded;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[EngineInit] Removing engine init hooks...");

		g_engineLoopHook.Remove();
		g_gameEngineHook.Remove();

		// Clear plugin callbacks
		g_pluginCallbacks.clear();

		ModLoaderLogger::LogInfo(L"[EngineInit] All hooks removed");
	}

	bool IsEngineInitialized()
	{
		return g_engineInitialized;
	}

	void RegisterPluginCallback(PluginEngineInitCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[EngineInit] RegisterPluginCallback: null callback provided");
			return;
		}

		g_pluginCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[EngineInit] Plugin callback registered (%zu total)", g_pluginCallbacks.size());

		// If the engine is already initialized, invoke the callback immediately
		// so that late-registering plugins (loaded after the hook fires) still
		// receive the notification.
		if (g_engineInitialized)
		{
			ModLoaderLogger::LogDebug(L"[EngineInit] Engine already initialized - invoking callback immediately");
			try
			{
				callback();
			}
			catch (const std::exception& e)
			{
				ModLoaderLogger::LogError(L"[EngineInit] Exception in late callback: %S", e.what());
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[EngineInit] Unknown exception in late callback");
			}
		}
	}

	void UnregisterPluginCallback(PluginEngineInitCallback callback)
	{
		auto it = std::find(g_pluginCallbacks.begin(), g_pluginCallbacks.end(), callback);
		if (it != g_pluginCallbacks.end())
		{
			g_pluginCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[EngineInit] Plugin callback unregistered (%zu remaining)",
			                          g_pluginCallbacks.size());
		}
	}

	void SetEngineInitCallback(void (*callback)())
	{
		// Legacy compatibility - just use RegisterPluginCallback
		if (callback)
		{
			ModLoaderLogger::LogWarn(L"[EngineInit] SetEngineInitCallback is deprecated, use RegisterPluginCallback");
			RegisterPluginCallback(callback);
		}
	}

	uintptr_t GetOriginalPtrEngineLoopInit()
	{
		return reinterpret_cast<uintptr_t>(g_engineLoopOriginal);
	}

	uintptr_t GetOriginalPtrGameEngineInit()
	{
		return reinterpret_cast<uintptr_t>(g_gameEngineOriginal);
	}
}
