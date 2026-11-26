#include "pch.h"
#include "world_end_play.h"
#include "logging/logger.h"
#include "plugins/plugin_call_gate.h"
#include "plugins/plugin_guard.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "plugins/reload_policy.h"
#include "../SDK.hpp"
#include "Engine_classes.hpp"
#include <vector>
#include <algorithm>

namespace Hooks::WorldEndPlay
{
	// UWorld::EndPlay(EEndPlayReason::Type)
	// x64 __fastcall member function: RCX = this (UWorld*), RDX = reason (uint8 enum)
	using OnWorldEndPlay_t = char(__fastcall*)(void* worldThis, int endPlayReason);

	static Hook g_hook;
	static OnWorldEndPlay_t g_original = nullptr;
	static long g_callCount = 0;

	static std::vector<PluginWorldEndPlayCallback> g_beforeCallbacks;
	static std::vector<PluginWorldEndPlayCallback> g_afterCallbacks;

	// SEH wrapper — must be in its own function with no C++ objects to satisfy C2712.
	// A plugin's callback is contained (and its plugin switched off) by
	// PluginGuard; this outer one still catches a fault in the loader's own.
	static void InvokeCallbackSEH(PluginWorldEndPlayCallback cb, SDK::UWorld* world, const char* name, size_t idx, const wchar_t* phase)
	{
		__try
		{
			PluginGuard::Invoke(cb, "world end-play callback", world, name);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			ModLoaderLogger::LogError(L"[WorldEndPlay] SEH exception in %s callback #%zu (code=0x%08X)", phase, idx + 1, GetExceptionCode());
		}
	}

	// What the filter keeps of a fault in the original UWorld::EndPlay, for the
	// log lines the handler writes once the stack has unwound. Plain data (C2712).
	// Registers and addresses only: nothing here is dereferenced.
	static constexpr size_t kFaultFrames = 8;
	struct EndPlayFault
	{
		uint32_t  code;
		uintptr_t address;
		bool      hasAccess;      // an access violation says what it touched
		uintptr_t accessKind;     // 0 read, 1 write, 8 execute
		uintptr_t accessAddress;
		CONTEXT   context;
		uintptr_t frames[kFaultFrames];
		size_t    frameCount;
	};

	// A call through a null pointer faults with RIP 0 and the caller's return
	// address on top of the stack, which the unwind tables cannot see. Moves
	// `ctx` to the caller. Holds no C++ objects (C2712).
	static bool StepOutOfNullCall(CONTEXT& ctx)
	{
		__try
		{
			ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
			ctx.Rsp += sizeof(DWORD64);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;   // a stack too broken to read
		}
	}

	// Runs on the faulting thread before anything is unwound, so the stack
	// walk sees the real frames. The fault is swallowed, as it always was.
	static LONG CaptureEndPlayFault(EXCEPTION_POINTERS* ep, EndPlayFault* out)
	{
		const EXCEPTION_RECORD& rec = *ep->ExceptionRecord;
		out->code          = rec.ExceptionCode;
		out->address       = reinterpret_cast<uintptr_t>(rec.ExceptionAddress);
		out->hasAccess     = (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2);
		out->accessKind    = out->hasAccess ? rec.ExceptionInformation[0] : 0;
		out->accessAddress = out->hasAccess ? rec.ExceptionInformation[1] : 0;
		out->context       = *ep->ContextRecord;
		if (rec.ExceptionCode == EXCEPTION_STACK_OVERFLOW)
		{
			out->frames[0]  = out->address;   // no stack to spare for a walk
			out->frameCount = 1;
		}
		else if (!out->context.Rip)
		{
			out->frames[0]  = 0;
			out->frameCount = 1;
			CONTEXT caller = out->context;
			if (StepOutOfNullCall(caller))
				out->frameCount += PluginGuard::Detail::WalkFrames(caller, out->frames + 1, kFaultFrames - 1);
		}
		else
		{
			out->frameCount = PluginGuard::Detail::WalkFrames(out->context, out->frames, kFaultFrames);
		}
		return EXCEPTION_EXECUTE_HANDLER;
	}

	// "<module>+0x<offset>", or "<unknown>+0x<address>" outside every module.
	static void FormatModuleOffset(uintptr_t address, wchar_t* out, size_t len)
	{
		const HMODULE module = PluginGuard::ModuleOf(reinterpret_cast<const void*>(address));
		if (!module)
		{
			swprintf_s(out, len, L"<unknown>+0x%llX", static_cast<unsigned long long>(address));
			return;
		}
		swprintf_s(out, len, L"%S+0x%llX", GetCallerModuleName(reinterpret_cast<void*>(address)).c_str(),
		           static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
	}

	// Three log lines for one swallowed fault: what and where, the registers, the
	// stack. A fault here has no other trace. Kept out of CallOriginalSEH's frame,
	// so the call that does not fault pays nothing for the buffers.
	__declspec(noinline) static void LogEndPlayFault(const EndPlayFault& f)
	{
		wchar_t where[MAX_PATH + 32];
		FormatModuleOffset(f.address, where, std::size(where));

		wchar_t access[64] = {};
		if (f.hasAccess)
			swprintf_s(access, L" (%s address 0x%llX)", f.accessKind == 0 ? L"reading" : f.accessKind == 1 ? L"writing" : L"executing",
			           static_cast<unsigned long long>(f.accessAddress));

		ModLoaderLogger::LogError(L"[WorldEndPlay] SEH exception in original UWorld::EndPlay (code=0x%08X) at %s%s",
		                          f.code, where, access);

		const CONTEXT& c = f.context;
		ModLoaderLogger::LogError(
			L"[WorldEndPlay]   registers: RIP=%llX RSP=%llX RBP=%llX RAX=%llX RBX=%llX RCX=%llX RDX=%llX RSI=%llX RDI=%llX "
			L"R8=%llX R9=%llX R10=%llX R11=%llX R12=%llX R13=%llX R14=%llX R15=%llX",
			c.Rip, c.Rsp, c.Rbp, c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi,
			c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15);

		wchar_t stack[1024] = {};   // the logger's own limit for one line
		for (size_t i = 0; i < f.frameCount; ++i)
		{
			wchar_t frame[MAX_PATH + 32];
			FormatModuleOffset(f.frames[i], frame, std::size(frame));
			wchar_t entry[MAX_PATH + 40];
			swprintf_s(entry, L" #%zu %s", i, frame);
			wcsncat_s(stack, entry, _TRUNCATE);
		}
		ModLoaderLogger::LogError(L"[WorldEndPlay]   stack:%s", stack);
	}

	// SEH wrapper for original function — must be in its own function with no C++ objects to satisfy C2712.
	static void CallOriginalSEH(void* worldThis, int endPlayReason)
	{
		EndPlayFault fault;   // filled by the filter, only when something faults
		__try
		{
			g_original(worldThis, endPlayReason);
		}
		__except (CaptureEndPlayFault(GetExceptionInformation(), &fault))
		{
			LogEndPlayFault(fault);
		}
	}

	static char __fastcall Detour(void* worldThis, int endPlayReason)
	{
		long callNum = InterlockedIncrement(&g_callCount);

		ModLoaderLogger::LogDebug(L"[WorldEndPlay] World end play detected (#%ld) world=%p Thread=%lu",
		                          callNum, worldThis, GetCurrentThreadId());

		auto* inWorld = static_cast<SDK::UWorld*>(worldThis);
		std::string worldName;
		if (inWorld)
		{
			worldName = inWorld->GetName();
			ModLoaderLogger::LogDebug(L"[WorldEndPlay]   World: %S", worldName.c_str());
		}
		else
		{
			ModLoaderLogger::LogWarn(L"[WorldEndPlay]   worldThis is null");
		}

		// The gameplay world is ending: plugins whose settings say "main menu only"
		// may be loaded and unloaded again (plugins/reload_policy.h).
		ReloadPolicy::NoteWorldEnded(worldName.c_str());

		// --- Before callbacks ---
		if (!g_beforeCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] Notifying %zu before callback(s)...", g_beforeCallbacks.size());
			const PluginCallGate::Scope gate;   // before the callbacks are read
			for (size_t i = 0; i < g_beforeCallbacks.size(); ++i)
			{
				if (g_beforeCallbacks[i])
					InvokeCallbackSEH(g_beforeCallbacks[i], inWorld, worldName.c_str(), i, L"before");
			}
		}

		// --- Call original ---
		ModLoaderLogger::LogDebug(L"[WorldEndPlay]   Calling original UWorld::EndPlay (reason=%d)...", endPlayReason);
		if (g_original)
		{
			CallOriginalSEH(inWorld, endPlayReason);
			ModLoaderLogger::LogDebug(L"[WorldEndPlay]   Original returned");
		}
		else
		{
			ModLoaderLogger::LogError(L"[WorldEndPlay] Original function pointer is null!");
		}

		// --- After callbacks ---
		if (!g_afterCallbacks.empty())
		{
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] Notifying %zu after callback(s)...", g_afterCallbacks.size());
			const PluginCallGate::Scope gate;   // before the callbacks are read
			for (size_t i = 0; i < g_afterCallbacks.size(); ++i)
			{
				if (g_afterCallbacks[i])
					InvokeCallbackSEH(g_afterCallbacks[i], inWorld, worldName.c_str(), i, L"after");
			}
		}

		ModLoaderLogger::LogDebug(L"[WorldEndPlay] EndPlay complete (#%ld)", callNum);
		return 0; // Return value is ignored by the caller, so we can return anything.
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[WorldEndPlay] Installing hook...");

		uintptr_t addr = Scanner::FindPatternInMainModule(
			"UWorld::EndPlay",
			ScanPatterns::UWorld_EndPlay);

		if (!addr)
		{
			ModLoaderLogger::LogError(L"[WorldEndPlay] Pattern scan failed");
			return false;
		}

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[WorldEndPlay] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[WorldEndPlay] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[WorldEndPlay] Removing hook...");
		g_hook.Remove();
		g_beforeCallbacks.clear();
		g_afterCallbacks.clear();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	long GetCallCount()
	{
		return g_callCount;
	}

	void RegisterBeforeCallback(PluginWorldEndPlayCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[WorldEndPlay] RegisterBeforeCallback: null callback provided");
			return;
		}

		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[WorldEndPlay] First before callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[WorldEndPlay] Failed to install hook for before callback!");
				return;
			}
		}

		g_beforeCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[WorldEndPlay] Before callback registered (%zu total)", g_beforeCallbacks.size());
	}

	void UnregisterBeforeCallback(PluginWorldEndPlayCallback callback)
	{
		auto it = std::find(g_beforeCallbacks.begin(), g_beforeCallbacks.end(), callback);
		if (it != g_beforeCallbacks.end())
		{
			g_beforeCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] Before callback unregistered (%zu remaining)",
			                          g_beforeCallbacks.size());
		}
	}

	void RegisterAfterCallback(PluginWorldEndPlayCallback callback)
	{
		if (!callback)
		{
			ModLoaderLogger::LogWarn(L"[WorldEndPlay] RegisterAfterCallback: null callback provided");
			return;
		}

		if (!g_hook.installed)
		{
			ModLoaderLogger::LogInfo(L"[WorldEndPlay] First after callback registered - installing hook now...");
			if (!Install())
			{
				ModLoaderLogger::LogError(L"[WorldEndPlay] Failed to install hook for after callback!");
				return;
			}
		}

		g_afterCallbacks.push_back(callback);
		ModLoaderLogger::LogDebug(L"[WorldEndPlay] After callback registered (%zu total)", g_afterCallbacks.size());
	}

	void UnregisterAfterCallback(PluginWorldEndPlayCallback callback)
	{
		auto it = std::find(g_afterCallbacks.begin(), g_afterCallbacks.end(), callback);
		if (it != g_afterCallbacks.end())
		{
			g_afterCallbacks.erase(it);
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] After callback unregistered (%zu remaining)",
			                          g_afterCallbacks.size());
		}
	}

	uintptr_t GetOriginalPtr()
	{
		return reinterpret_cast<uintptr_t>(g_original);
	}

	// Drops every callback that lives in `module`: a plugin being unloaded,
	// whatever it did not unregister itself. Game thread, before FreeLibrary.
	void ForgetModule(HMODULE module)
	{
		const size_t dropped = PluginGuard::ForgetModule(g_beforeCallbacks, module)
		                      + PluginGuard::ForgetModule(g_afterCallbacks, module);
		if (dropped)
			ModLoaderLogger::LogDebug(L"[WorldEndPlay] Dropped %zu callback(s) of an unloading plugin", dropped);
	}
}
