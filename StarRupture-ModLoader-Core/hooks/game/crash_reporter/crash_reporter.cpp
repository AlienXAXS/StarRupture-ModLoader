#include "pch.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "crash_reporter.h"
#include "crash_dialog.h"
#include "logging/logger.h"
#include <string>
#include <vector>
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "../../symbol_resolver.h"
#include "core/version_check.h"
#include "utils/pak_list.h"
#include <DbgHelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "DbgHelp.lib")

#ifndef MODLOADER_BUILD_TAG
#define MODLOADER_BUILD_TAG "dev"
#endif

#if defined(MODLOADER_CLIENT_BUILD)
#define MODLOADER_BUILD_KIND "client"
#elif defined(MODLOADER_SERVER_BUILD)
#define MODLOADER_BUILD_KIND "server"
#else
#define MODLOADER_BUILD_KIND "generic"
#endif

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

	// One entry per distinct module seen in the stack walk, in first-seen
	// order. Filled during the walk, emitted after it by EmitModuleTable.
	//
	// This exists because an unsymbolized stack and a correctly symbolized one
	// are indistinguishable in the report. With no PDB for a module, DbgHelp
	// silently falls back to its export table, and every frame in a DLL that
	// exports three symbols then resolves to the same nearby export with a
	// huge displacement -- "StarRupture-ModLoader-Core.dll!Core_Detach +
	// 0x26FFA" for what is really Hooks::SaveLoaded::Install. That is not a
	// near miss, it is an unrelated function, and the old output gave the
	// reader no way to tell.
	struct StackModule
	{
		DWORD64      base         = 0;
		DWORD64      size         = 0;
		std::wstring name;                 // file name only
		bool         hasPdb       = false; // SymType == SymPdb: names are real
		bool         pdbUnmatched = false; // a PDB was found but is not this build's
		bool         isOurs       = false; // ships with the mod loader (Core, ImGui, plugins)
	};

	// Directory the mod loader's own modules live in (the game's
	// Binaries\Win64\ModLoader\ folder, which also contains Plugins\).
	// Used only to decide whether a module missing its PDB is worth
	// complaining about: for one of ours that is a deployment mistake the
	// reader can fix, for ntdll.dll it is simply how Windows ships.
	static const std::wstring& OwnModuleDir()
	{
		static const std::wstring dir = []() -> std::wstring
		{
			HMODULE self = nullptr;
			if (!GetModuleHandleExW(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&OwnModuleDir), &self) || !self)
				return {};

			wchar_t path[MAX_PATH]{};
			if (!GetModuleFileNameW(self, path, MAX_PATH))
				return {};

			wchar_t* slash = wcsrchr(path, L'\\');
			if (!slash)
				return {};

			*(slash + 1) = L'\0'; // keep the trailing backslash so the prefix test can't
			                      // match a sibling folder with a longer name
			return std::wstring(path);
		}();
		return dir;
	}

	// Index into `modules` of the entry covering `addr`, appending a new one on
	// first sight. Returns kNoModule when the address is in no module DbgHelp
	// knows about. Caller must hold SymbolResolver::GetMutex().
	//
	// An index rather than a pointer on purpose: the vector grows as the walk
	// finds new modules, and a pointer handed out before a push_back is a
	// dangling read waiting for the one crash report nobody can reproduce.
	static constexpr size_t kNoModule = static_cast<size_t>(-1);

	static size_t FindOrAddModule(HANDLE process, DWORD64 addr, std::vector<StackModule>& modules)
	{
		for (size_t i = 0; i < modules.size(); ++i)
		{
			const StackModule& m = modules[i];
			if (m.base && addr >= m.base && addr < m.base + m.size)
				return i;
		}

		IMAGEHLP_MODULEW64 info{};
		info.SizeOfStruct = sizeof(info);
		if (!SymGetModuleInfoW64(process, addr, &info))
			return kNoModule;

		StackModule m;
		m.base         = info.BaseOfImage;
		m.size         = info.ImageSize;
		m.hasPdb       = (info.SymType == SymPdb);
		m.pdbUnmatched = (info.PdbUnmatched != FALSE);

		wchar_t path[MAX_PATH]{};
		if (GetModuleFileNameW(reinterpret_cast<HMODULE>(info.BaseOfImage), path, MAX_PATH))
		{
			const wchar_t* slash = wcsrchr(path, L'\\');
			m.name = slash ? slash + 1 : path;

			const std::wstring& ourDir = OwnModuleDir();
			m.isOurs = !ourDir.empty() && _wcsnicmp(path, ourDir.c_str(), ourDir.size()) == 0;
		}
		else
		{
			m.name = info.ModuleName[0] ? info.ModuleName : L"<unknown>";
		}

		modules.push_back(std::move(m));
		return modules.size() - 1;
	}

	// Emits the per-module symbol status for everything the walk touched, so a
	// pasted report says on its face whether its function names can be trusted
	// -- and carries the load addresses needed to symbolize it offline later
	// against the matching PDB, which is the only option once the crash is in
	// a bug report rather than on the machine that produced it.
	static void EmitModuleTable(std::wstring& details, const std::vector<StackModule>& modules)
	{
		if (modules.empty())
			return;

		details += L"\r\n";
		EmitCrashLine(details, L"Modules in stack:");

		for (const StackModule& m : modules)
		{
			const wchar_t* symState = m.hasPdb       ? L"PDB"
			                        : m.pdbUnmatched ? L"PDB MISMATCH"
			                                         : L"exports only";

			EmitCrashLine(details, L"  %-36s base=0x%016llX  size=0x%08llX  symbols=%s",
				m.name.c_str(), m.base, m.size, symState);
		}

		for (const StackModule& m : modules)
		{
			if (!m.isOurs || m.hasPdb)
				continue;

			if (m.pdbUnmatched)
				EmitCrashLine(details,
					L"  ! %s: a PDB was found but does not match this build. Frame names in this "
					L"module come from its export table and are almost certainly the wrong function.",
					m.name.c_str());
			else
				EmitCrashLine(details,
					L"  ! %s: no matching PDB. Frame names in this module come from its export "
					L"table and are almost certainly the wrong function -- copy the .pdb next to "
					L"the .dll and reproduce.",
					m.name.c_str());
		}
	}

	// Walks the crashing thread's call stack from the captured CONTEXT and logs one line per
	// frame: module name, symbol name (demangled Class::Func with source file and line when a
	// PDB is available, the nearest export marked [export] when it is not), and both a
	// symbol-relative and module-relative offset.
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

		// Before the lock: RefreshModules takes the same (non-recursive) mutex.
		// Picks up every plugin DLL loaded after DbgHelp's one-shot module
		// snapshot -- without it a frame inside a plugin has no module and no
		// symbol at all, which is the single most useful frame in a crash
		// caused by a plugin.
		//
		// It walks the loader's module list, so it touches the loader lock in a
		// process that has just faulted. That exposure is not new: the
		// EnsureInitialized above is a SymInitialize with fInvadeProcess=TRUE,
		// which enumerates the same list, and StackWalk64 below maps PDBs off
		// disk. We are also on the engine's crash-reporting thread rather than
		// the faulting one, which is what makes any of this survivable.
		Hooks::SymbolResolver::RefreshModules();

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

		std::vector<StackModule> modules;

		constexpr int kMaxFrames = 64;
		for (int i = 0; i < kMaxFrames; ++i)
		{
			if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(),
					&frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
				break;

			DWORD64 addr = frame.AddrPC.Offset;
			if (!addr)
				break;

			uint8_t symBuffer[sizeof(SYMBOL_INFOW) + MAX_SYM_NAME * sizeof(wchar_t)]{};
			auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(symBuffer);
			symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
			symbol->MaxNameLen = MAX_SYM_NAME;

			DWORD64 symDisplacement = 0;
			const bool haveSymbol = SymFromAddrW(process, addr, &symDisplacement, symbol) != FALSE;

			// Queried only after SymFromAddrW: SYMOPT_DEFERRED_LOADS leaves
			// SymType as SymDeferred until something actually asks for a
			// symbol, so asking first would report every module as unsymbolized.
			const size_t modIdx = FindOrAddModule(process, addr, modules);
			const wchar_t* moduleName = (modIdx != kNoModule) ? modules[modIdx].name.c_str() : L"<unknown>";
			const DWORD64 modDisplacement = (modIdx != kNoModule) ? (addr - modules[modIdx].base) : 0;

			if (!haveSymbol)
			{
				EmitCrashLine(details, L"  #%02d 0x%016llX  %s + 0x%llX",
					i, addr, moduleName, modDisplacement);
				continue;
			}

			// Source file and line, when the PDB carries them. Basename only --
			// the full build-machine path is noise in a pasted report.
			std::wstring where;
			DWORD lineDisplacement = 0;
			IMAGEHLP_LINEW64 line{};
			line.SizeOfStruct = sizeof(line);
			if (SymGetLineFromAddrW64(process, addr, &lineDisplacement, &line) && line.FileName)
			{
				const wchar_t* slash = wcsrchr(line.FileName, L'\\');
				where = L"  [";
				where += (slash ? slash + 1 : line.FileName);
				where += L":";
				where += std::to_wstring(line.LineNumber);
				where += L"]";
			}

			// The name came from the export table, not a PDB -- say so rather
			// than letting a plausible-looking wrong name stand unqualified.
			const bool exportOnly = (modIdx != kNoModule) && !modules[modIdx].hasPdb;

			EmitCrashLine(details, L"  #%02d 0x%016llX  %s!%s + 0x%llX  (module+0x%llX)%s%s",
				i, addr, moduleName, symbol->Name, symDisplacement, modDisplacement,
				where.c_str(), exportOnly ? L"  [export]" : L"");
		}

		EmitModuleTable(details, modules);
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

		// Leads with which loader build produced this stack and which game
		// build it was running against -- the two facts needed to pick the
		// right PDB when the report has to be symbolized offline, and the two
		// that decide whether a pattern was ever going to match in the first
		// place. Same header as the plugin hook-failure report, for the same
		// reason.
		{
			std::wstring gameVersion = GetGameVersionString();
			if (gameVersion.empty())
				gameVersion = L"unknown";

			EmitCrashLine(details, L"Loader:       %S (%S build)",
				MODLOADER_BUILD_TAG, MODLOADER_BUILD_KIND);
			EmitCrashLine(details, L"Game version: %s", gameVersion.c_str());
			details += L"\r\n";
		}

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
