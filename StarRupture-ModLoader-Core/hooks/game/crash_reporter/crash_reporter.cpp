#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "crash_reporter.h"
#include "crash_dialog.h"
#include "logging/logger.h"
#include <string>
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "../../symbol_resolver.h"
#include "utils/pak_list.h"
#include <DbgHelp.h>
#include <tlhelp32.h>
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

	// Formats one line into the crash-details buffer shown in the crash dialog
	// (CRLF endings -- edit controls don't render bare '\n') and mirrors it to
	// the modloader log.
	static void EmitCrashLine(std::wstring& details, const wchar_t* format, ...)
	{
		wchar_t line[1024]{};
		va_list args;
		va_start(args, format);
		_vsnwprintf_s(line, _TRUNCATE, format, args);
		va_end(args);

		ModLoaderLogger::LogError(L"[CrashReporter]   %s", line);
		details += line;
		details += L"\r\n";
	}

	// Walks the crashing thread's call stack from the captured CONTEXT and logs one line per
	// frame: module name, symbol name (demangled Class::Func if PDBs are available, otherwise
	// just the raw address), and both a symbol-relative and module-relative offset.
	//
	// StackWalk64 is given a copy of the CONTEXT captured at the moment of the fault (from
	// ExceptionInfo->ContextRecord) rather than a live thread handle -- on x64 the unwind is
	// driven entirely by that CONTEXT plus the module's .pdata (SymFunctionTableAccess64), so
	// GetCurrentThread() is passed as a formality only.
	static void LogStackTrace(const CONTEXT* contextRecord, std::wstring& details)
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

		EmitCrashLine(details, L"Stack trace:");

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
				EmitCrashLine(details, L"  #%02d 0x%016llX  %s!%s + 0x%llX  (module+0x%llX)",
					i, addr, moduleName, symbol->Name, symDisplacement, modDisplacement);
			}
			else
			{
				EmitCrashLine(details, L"  #%02d 0x%016llX  %s + 0x%llX",
					i, addr, moduleName, modDisplacement);
			}
		}
	}

	// Suspends every thread in the process except the calling one. Called just
	// before showing the crash dialog: the engine keeps plenty of threads
	// alive after a fatal crash (hang detector / heartbeat watchdog, render,
	// task workers), and the hang detector in particular will terminate the
	// process after ~30s if the game thread stops ticking -- which would rip
	// the crash dialog away with no user interaction.
	//
	// Everything the dialog needs (details string, log lines) must be built
	// BEFORE calling this: a suspended thread may hold the heap lock, so
	// allocations afterwards are best-effort. The process never resumes these
	// threads -- the caller terminates the process when the dialog closes.
	static void SuspendAllOtherThreads()
	{
		// Log BEFORE suspending: the logger takes a critical section, and once
		// other threads are frozen one of them could be holding it forever.
		ModLoaderLogger::LogError(L"[CrashReporter]   Suspending all other threads before showing the crash dialog");

		HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (snapshot == INVALID_HANDLE_VALUE)
			return;

		const DWORD currentPid = GetCurrentProcessId();
		const DWORD currentTid = GetCurrentThreadId();

		THREADENTRY32 entry{};
		entry.dwSize = sizeof(entry);
		int suspended = 0;

		if (Thread32First(snapshot, &entry))
		{
			do
			{
				if (entry.th32OwnerProcessID != currentPid || entry.th32ThreadID == currentTid)
					continue;

				HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
				if (thread)
				{
					if (SuspendThread(thread) != static_cast<DWORD>(-1))
						++suspended;
					CloseHandle(thread);
				}
			} while (Thread32Next(snapshot, &entry));
		}

		CloseHandle(snapshot);
		(void)suspended;
	}

	static int64_t __fastcall Detour(void* inContext, void* exceptionInfo, int32_t reportUI)
	{
		if (!g_fatalCrashReturnAddr || reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_fatalCrashReturnAddr)
		{
			// Not the fatal-crash call site (ensure() or hang report) -- unchanged behavior.
			return g_original ? g_original(inContext, exceptionInfo, reportUI) : 0;
		}

		ModLoaderLogger::LogError(L"[CrashReporter] *** FATAL ENGINE CRASH *** -- suppressing CrashReportClient upload");

		// Every EmitCrashLine call below both logs to ModLoader.log and appends
		// to this buffer, which becomes the copyable text in the crash dialog.
		std::wstring details;

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

			EmitCrashLine(details, L"Exception : 0x%08lX", record->ExceptionCode);
			EmitCrashLine(details, L"Fault addr: 0x%016llX  (exe+0x%llX)", faultAddr, faultAddr - base);
			EmitCrashLine(details, L"RIP=0x%016llX  (exe+0x%llX)", ctx->Rip, ctx->Rip - base);
			EmitCrashLine(details, L"RSP=0x%016llX  RBP=0x%016llX", ctx->Rsp, ctx->Rbp);
			EmitCrashLine(details, L"RCX=0x%016llX  RDX=0x%016llX", ctx->Rcx, ctx->Rdx);
			EmitCrashLine(details, L"R8 =0x%016llX  R9 =0x%016llX", ctx->R8, ctx->R9);
			EmitCrashLine(details, L"R10=0x%016llX  R11=0x%016llX", ctx->R10, ctx->R11);
			details += L"\r\n";

			LogStackTrace(ctx, details);
		}
		else
		{
			EmitCrashLine(details, L"ExceptionInfo unavailable -- no fault details to log");
		}

		// Append the pak inventory captured at startup -- an out-of-date pak
		// mod is the most common real cause of crashes blamed on the modloader.
		{
			const std::wstring& paks = PakList::GetSummary();
			if (!paks.empty())
			{
				details += L"\r\n";
				details += paks;
			}
		}

		// Freeze the rest of the process so the engine's hang detector (which
		// terminates the process ~30s after the game thread stops ticking)
		// can't rip the dialog away. All logging and string building must be
		// done by this point -- see SuspendAllOtherThreads.
		SuspendAllOtherThreads();

		CrashDialog::Show(CrashDialog::Mode::FatalCrash, details);

		// The game is unrecoverable and every other thread is suspended --
		// returning into the engine's crash handling would deadlock. End the
		// process ourselves with the original exception code.
		DWORD exitCode = 0xDEAD;
		if (exceptionPointers && exceptionPointers->ExceptionRecord)
			exitCode = exceptionPointers->ExceptionRecord->ExceptionCode;
		TerminateProcess(GetCurrentProcess(), exitCode);

		return 0; // not reached
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
