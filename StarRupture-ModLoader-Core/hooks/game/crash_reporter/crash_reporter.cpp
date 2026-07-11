#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "crash_reporter.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "../../symbol_resolver.h"
#include <DbgHelp.h>
#pragma comment(lib, "DbgHelp.lib")

namespace Hooks::CrashReporter
{
	// anonymous_namespace_::ReportCrashUsingCrashReportClient(FWindowsPlatformCrashContext* InContext,
	//   _EXCEPTION_POINTERS* ExceptionInfo, EErrorReportUI ReportUI)
	using ReportCrashUsingCrashReportClient_t = int64_t(__fastcall*)(void* inContext, void* exceptionInfo, int32_t reportUI);

	static Hook g_hook;
	static ReportCrashUsingCrashReportClient_t g_original = nullptr;

	// Return address of the one call site inside FCrashReportingThread::HandleCrashInternal
	// that reports a real fatal crash. 0 until resolved in Install() -- if it can never be
	// resolved, the detour always falls through to the original (fail safe: never suppress
	// ensure()/hang reports it can't positively identify as fatal).
	static uintptr_t g_fatalCrashReturnAddr = 0;

	// Walks the crashing thread's call stack from the captured CONTEXT and logs one line per
	// frame: module name, symbol name (demangled Class::Func if PDBs are available, otherwise
	// just the raw address), and both a symbol-relative and module-relative offset.
	//
	// StackWalk64 is given a copy of the CONTEXT captured at the moment of the fault (from
	// ExceptionInfo->ContextRecord) rather than a live thread handle -- on x64 the unwind is
	// driven entirely by that CONTEXT plus the module's .pdata (SymFunctionTableAccess64), so
	// GetCurrentThread() is passed as a formality only.
	static void LogStackTrace(const CONTEXT* contextRecord)
	{
		HANDLE process = GetCurrentProcess();

		// Shared with engine_tick's stack sampler -- see symbol_resolver.h.
		// DbgHelp is not thread-safe, so every call into it (SymInitialize,
		// StackWalk64, SymFromAddrW, and the callbacks StackWalk64 invokes
		// internally) must be serialized through the same process-wide lock.
		Hooks::SymbolResolver::EnsureInitialized();
		std::lock_guard<std::mutex> dbgHelpLock(Hooks::SymbolResolver::GetMutex());

		CONTEXT ctx = *contextRecord; // StackWalk64 mutates this as it unwinds

		STACKFRAME64 frame{};
		frame.AddrPC.Offset = ctx.Rip;
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrFrame.Offset = ctx.Rbp;
		frame.AddrFrame.Mode = AddrModeFlat;
		frame.AddrStack.Offset = ctx.Rsp;
		frame.AddrStack.Mode = AddrModeFlat;

		ModLoaderLogger::LogError(L"[CrashReporter]   Stack trace:");

		constexpr int kMaxFrames = 64;
		for (int i = 0; i < kMaxFrames; ++i)
		{
			if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(),
					&frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
				break;

			DWORD64 addr = frame.AddrPC.Offset;
			if (!addr)
				break;

			wchar_t moduleName[MAX_PATH] = L"<unknown>";
			DWORD64 moduleBase = SymGetModuleBase64(process, addr);
			if (moduleBase)
			{
				wchar_t path[MAX_PATH]{};
				if (GetModuleFileNameW(reinterpret_cast<HMODULE>(moduleBase), path, MAX_PATH))
				{
					const wchar_t* slash = wcsrchr(path, L'\\');
					wcscpy_s(moduleName, slash ? slash + 1 : path);
				}
			}
			DWORD64 modDisplacement = moduleBase ? (addr - moduleBase) : 0;

			uint8_t symBuffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)]{};
			auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(symBuffer);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
			symbol->MaxNameLen = MAX_SYM_NAME;

			DWORD64 symDisplacement = 0;
			if (SymFromAddrW(process, addr, &symDisplacement, symbol))
			{
				ModLoaderLogger::LogError(L"[CrashReporter]     #%02d 0x%016llX  %s!%s + 0x%llX  (module+0x%llX)",
					i, addr, moduleName, symbol->Name, symDisplacement, modDisplacement);
			}
			else
			{
				ModLoaderLogger::LogError(L"[CrashReporter]     #%02d 0x%016llX  %s + 0x%llX",
					i, addr, moduleName, modDisplacement);
			}
		}
	}

	static int64_t __fastcall Detour(void* inContext, void* exceptionInfo, int32_t reportUI)
	{
		if (!g_fatalCrashReturnAddr || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_fatalCrashReturnAddr)
		{
			// Not the fatal-crash call site (ensure() or hang report) -- unchanged behavior.
			return g_original ? g_original(inContext, exceptionInfo, reportUI) : 0;
		}

		ModLoaderLogger::LogError(L"[CrashReporter] *** FATAL ENGINE CRASH *** -- suppressing CrashReportClient upload");

		// ExceptionInfo is a plain Win32 _EXCEPTION_POINTERS* (not an engine-internal type),
		// so we can log the fault details directly into ModLoader.log without needing the
		// engine's own FWindowsPlatformCrashContext serialization.
		auto* exceptionPointers = static_cast<EXCEPTION_POINTERS*>(exceptionInfo);
		if (exceptionPointers && exceptionPointers->ExceptionRecord && exceptionPointers->ContextRecord)
		{
			const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
			const CONTEXT* ctx = exceptionPointers->ContextRecord;

			HMODULE mainMod = GetModuleHandleW(nullptr);
			auto base = reinterpret_cast<ULONG64>(mainMod);
			auto faultAddr = reinterpret_cast<ULONG64>(record->ExceptionAddress);

			ModLoaderLogger::LogError(L"[CrashReporter]   Exception : 0x%08lX", record->ExceptionCode);
			ModLoaderLogger::LogError(L"[CrashReporter]   Fault addr: 0x%016llX  (exe+0x%llX)", faultAddr, faultAddr - base);
			ModLoaderLogger::LogError(L"[CrashReporter]   RIP=0x%016llX  (exe+0x%llX)", ctx->Rip, ctx->Rip - base);
			ModLoaderLogger::LogError(L"[CrashReporter]   RSP=0x%016llX  RBP=0x%016llX", ctx->Rsp, ctx->Rbp);
			ModLoaderLogger::LogError(L"[CrashReporter]   RCX=0x%016llX  RDX=0x%016llX", ctx->Rcx, ctx->Rdx);
			ModLoaderLogger::LogError(L"[CrashReporter]   R8 =0x%016llX  R9 =0x%016llX", ctx->R8, ctx->R9);
			ModLoaderLogger::LogError(L"[CrashReporter]   R10=0x%016llX  R11=0x%016llX", ctx->R10, ctx->R11);

			LogStackTrace(ctx);
		}
		else
		{
			ModLoaderLogger::LogError(L"[CrashReporter]   ExceptionInfo unavailable -- no fault details to log");
		}

		MessageBoxW(nullptr,
			L"StarRupture has crashed.\n\n"
			L"Automatic crash reporting to the developers is disabled because the ModLoader is installed.\n"
			L"This crash report has not been sent anywhere.\n\n"
			L"Details were written to ModLoader\\Logs\\ModLoader.log.",
			L"StarRupture ModLoader",
			MB_OK | MB_ICONERROR | MB_TASKMODAL);

		return 0;
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[CrashReporter] Installing hook...");

		uintptr_t targetAddr = Scanner::FindPatternInMainModule(
			"ReportCrashUsingCrashReportClient", ScanPatterns::ReportCrashUsingCrashReportClient);
		if (!targetAddr)
		{
			ModLoaderLogger::LogError(L"[CrashReporter] Pattern scan failed -- hook not installed");
			return false;
		}

		uintptr_t callSiteAddr = Scanner::FindPatternInMainModule(
			"HandleCrashInternal_FatalReportCallSite", ScanPatterns::HandleCrashInternal_FatalReportCallSite);
		if (callSiteAddr)
		{
			g_fatalCrashReturnAddr = callSiteAddr + ScanPatterns::HandleCrashInternal_FatalReportCallSite_Length;
		}
		else
		{
			ModLoaderLogger::LogWarn(L"[CrashReporter] Fatal-crash call site not found -- "
				L"hook will install but never trigger (ensures/hangs/crashes all keep default behavior)");
		}

		bool hookOk = g_hook.Install(
			targetAddr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[CrashReporter] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[CrashReporter] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[CrashReporter] Removing hook...");
		g_hook.Remove();
		g_fatalCrashReturnAddr = 0;
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	uintptr_t GetOriginalPtr()
	{
		return reinterpret_cast<uintptr_t>(g_original);
	}
}

#endif // MODLOADER_CLIENT_BUILD
