#include "preload/preload_manager.h"
#include "preload/preload_interface.h"
#include "preload/preload_patch.h"

#include "core/globals.h"
#include "core/startup_utils.h"
#include "memory_scanner/hook_scanner_interface.h"
#include "plugins/plugin_hook_report.h"
#include "logging/log.h"
#include "logging/logger.h"
#include "logging/logger_interface.h"
#ifdef MODLOADER_CLIENT_BUILD
#include "UI/splash_window.h"
#endif

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace PreloadManager
{
	// -----------------------------------------------------------------------
	// SEH helpers.
	//
	// Plain POD functions with no C++ objects (C2712), same shape as the ones
	// in plugin_manager.cpp. Everything a preload DLL exports is called through
	// one of these: a fault here happens before the game's entry point has run,
	// so there is no crash handler, no minidump and no usable stack. Catching it
	// and naming the DLL is the only diagnostic anyone will ever get.
	// -----------------------------------------------------------------------
	struct CrashCtx { DWORD code; uintptr_t addr; };
	static CrashCtx g_lastCrash;

	static LONG CrashFilter(EXCEPTION_POINTERS* ep)
	{
		g_lastCrash.code = ep->ExceptionRecord->ExceptionCode;
		g_lastCrash.addr = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	struct LoadResult { HMODULE module; bool crashed; };

	static LoadResult LoadLibrarySEH(const wchar_t* path)
	{
		LoadResult r{ nullptr, false };
		g_lastCrash = CrashCtx{};
		__try { r.module = LoadLibraryW(path); }
		__except (CrashFilter(GetExceptionInformation())) { r.crashed = true; }
		return r;
	}

	static PreloadInfo* CallGetInfoSEH(GetPreloadInfoFunc fn)
	{
		PreloadInfo* info = nullptr;
		g_lastCrash = CrashCtx{};
		__try { info = fn(); }
		__except (CrashFilter(GetExceptionInformation())) { info = nullptr; }
		return info;
	}

	static bool CallScanSEH(PreloadScanFunc fn, IPluginSelf* self, IPluginHookScanner* scanner)
	{
		g_lastCrash = CrashCtx{};
		__try { fn(self, scanner); return true; }
		__except (CrashFilter(GetExceptionInformation())) { return false; }
	}

	struct InitResult { bool retval; bool crashed; };

	static InitResult CallInitSEH(PreloadInitFunc fn, IPluginSelf* self, IPreloadPatch* patch)
	{
		InitResult r{ false, false };
		g_lastCrash = CrashCtx{};
		__try { r.retval = fn(self, patch); }
		__except (CrashFilter(GetExceptionInformation())) { r.crashed = true; }
		return r;
	}

	static void CallShutdownSEH(PreloadShutdownFunc fn)
	{
		g_lastCrash = CrashCtx{};
		__try { fn(); }
		__except (CrashFilter(GetExceptionInformation())) { }
	}

	namespace
	{
		// One preload plugin that got as far as passing its version and target
		// checks. The IPluginSelf is the identity the hook report keys sessions
		// on and the identity PreloadPatch attributes hooks to, so it has to
		// outlive the module -- hence stable unique_ptr storage and name/version
		// strings copied out of the DLL rather than pointed into it.
		struct Loaded
		{
			HMODULE             module   = nullptr;
			PreloadScanFunc     scan     = nullptr;
			PreloadInitFunc     init     = nullptr;
			PreloadShutdownFunc shutdown = nullptr;

			std::wstring path;
			std::string  fileName;
			std::string  name;
			std::string  version;
			std::string  author;
			int          priority = 0;

			IPluginSelf self{};
		};

		std::mutex                           g_mutex;
		std::vector<std::unique_ptr<Loaded>> g_running;   // survived the whole phase
		std::vector<Record>                  g_records;   // every DLL looked at
		bool                                 g_didRun = false;
		std::string                          g_skipReason;

		std::wstring PreloadDir()     { return GetModLoaderDirPath(L"Preload"); }
		std::wstring BreadcrumbPath() { return GetModLoaderDirPath(L"Preload\\.state"); }

		std::string Narrow(const wchar_t* wide)
		{
			if (!wide || !wide[0]) return {};
			char buffer[512]{};
			WideCharToMultiByte(CP_ACP, 0, wide, -1, buffer,
				static_cast<int>(sizeof(buffer)), "?", nullptr);
			return buffer;
		}

		bool EqualsNoCase(const std::string& a, const std::string& b)
		{
			return _stricmp(a.c_str(), b.c_str()) == 0;
		}

		// --- Switches -------------------------------------------------------

		bool CommandLineHas(const wchar_t* flag)
		{
			const wchar_t* cmdLine = GetCommandLineW();
			return cmdLine && wcsstr(cmdLine, flag) != nullptr;
		}

		// Read with the raw profile API on purpose: InitializeConfigManager()
		// runs in Stage 2, long after this phase, so there is no config
		// subsystem yet. One GetPrivateProfile* call needs nothing.
		bool EnabledInIni()
		{
			const std::wstring ini = GetModLoaderDirPath(L"modloader.ini");
			return GetPrivateProfileIntW(L"Preload", L"Enabled", 1, ini.c_str()) != 0;
		}

		std::vector<std::string> DisabledList()
		{
			std::vector<std::string> out;

			const std::wstring ini = GetModLoaderDirPath(L"modloader.ini");
			wchar_t value[1024]{};
			GetPrivateProfileStringW(L"Preload", L"Disabled", L"", value,
				static_cast<DWORD>(std::size(value)), ini.c_str());

			const std::string list = Narrow(value);
			size_t start = 0;
			while (start < list.size())
			{
				const size_t comma = list.find(',', start);
				std::string item = list.substr(start,
					comma == std::string::npos ? std::string::npos : comma - start);

				// Trimmed because a hand-edited ini will have spaces after the commas.
				while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) item.erase(item.begin());
				while (!item.empty() && (item.back()  == ' ' || item.back()  == '\t')) item.pop_back();

				if (!item.empty())
					out.push_back(item);

				if (comma == std::string::npos)
					break;
				start = comma + 1;
			}

			return out;
		}

		// --- Breadcrumb -----------------------------------------------------
		//
		// Holds the file name of whichever DLL is currently being touched, and
		// is deleted the moment that DLL is through. A file still there on the
		// next launch is the only evidence that survives a hang or a hard crash
		// at Stage 1, where there is no crash handler yet and the log stops
		// wherever the process died. That plugin is then skipped, and the log
		// says why and how to undo it.

		std::string ReadBreadcrumb()
		{
			HANDLE file = CreateFileW(BreadcrumbPath().c_str(), GENERIC_READ, FILE_SHARE_READ,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return {};

			char buffer[256]{};
			DWORD read = 0;
			ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer) - 1), &read, nullptr);
			CloseHandle(file);

			std::string name(buffer, read);
			while (!name.empty() && (name.back() == '\r' || name.back() == '\n' || name.back() == ' '))
				name.pop_back();
			return name;
		}

		void WriteBreadcrumb(const std::string& fileName)
		{
			HANDLE file = CreateFileW(BreadcrumbPath().c_str(), GENERIC_WRITE, 0, nullptr,
				CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return;

			DWORD written = 0;
			WriteFile(file, fileName.c_str(), static_cast<DWORD>(fileName.size()), &written, nullptr);

			// Flushed because the whole point of this file is to survive a
			// process that is about to die without unwinding anything.
			FlushFileBuffers(file);
			CloseHandle(file);
		}

		void ClearBreadcrumb()
		{
			DeleteFileW(BreadcrumbPath().c_str());
		}

		// --- Discovery ------------------------------------------------------

		struct Candidate
		{
			std::wstring path;
			std::string  fileName;
		};

		std::vector<Candidate> CollectCandidates()
		{
			std::vector<Candidate> out;

			const std::wstring dir = PreloadDir();

			// Created when missing so the folder is discoverable: someone
			// looking for where preload plugins go should find an empty folder
			// rather than have to read the docs to learn the name.
			CreateDirectoryW(dir.c_str(), nullptr);

			const std::wstring pattern = dir + L"\\*.dll";

			WIN32_FIND_DATAW fd{};
			HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
			if (find == INVALID_HANDLE_VALUE)
				return out;

			do
			{
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;

				Candidate c;
				c.path     = dir + L"\\" + fd.cFileName;
				c.fileName = Narrow(fd.cFileName);
				out.push_back(std::move(c));
			}
			while (FindNextFileW(find, &fd));

			FindClose(find);

			// Alphabetical, so the order a plugin sees does not depend on how
			// the filesystem happened to enumerate the folder. Priority is
			// applied after loading, because it cannot be read until then.
			std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b)
			{
				return _stricmp(a.fileName.c_str(), b.fileName.c_str()) < 0;
			});

			return out;
		}

		void LogCrash(const char* what, const char* who)
		{
			LogToFile::Error("[Preload] %s CRASHED in %s -- code=0x%08X at 0x%llX",
				who, what, g_lastCrash.code,
				static_cast<unsigned long long>(g_lastCrash.addr));
		}

		void Reject(const Candidate& candidate, const std::string& name, Status status, const std::string& detail)
		{
			Record record;
			record.fileName = candidate.fileName;
			record.name     = name.empty() ? candidate.fileName : name;
			record.status   = status;
			record.detail   = detail;
			g_records.push_back(std::move(record));
		}

		// --- Pass 1: load and vet -------------------------------------------
		//
		// Everything that can be decided without running any of the plugin's
		// own logic happens here, so that pass 2 has the priority values it
		// needs to order the DLLs that are actually going to do something.
		std::unique_ptr<Loaded> LoadAndVet(const Candidate& candidate,
			const std::vector<std::string>& disabled, const std::string& crashedLastLaunch)
		{
			for (const std::string& d : disabled)
			{
				if (EqualsNoCase(d, candidate.fileName))
				{
					LogToFile::Info("[Preload] %s: skipped (disabled in modloader.ini)", candidate.fileName.c_str());
					Reject(candidate, {}, Status::Disabled, "listed in [Preload] Disabled in modloader.ini");
					return nullptr;
				}
			}

			if (!crashedLastLaunch.empty() && EqualsNoCase(crashedLastLaunch, candidate.fileName))
			{
				LogToFile::Error("[Preload] %s: SKIPPED -- it did not finish loading on the previous launch.",
					candidate.fileName.c_str());
				LogToFile::Error("[Preload]   The game most likely hung or crashed inside it before the engine started.");
				LogToFile::Error("[Preload]   Delete ModLoader\\Preload\\.state to let it try again.");
				Reject(candidate, {}, Status::SkippedAfterCrash,
					"did not finish loading on the previous launch -- delete ModLoader\\Preload\\.state to retry");
				return nullptr;
			}

			WriteBreadcrumb(candidate.fileName);
			LogToFile::Info("[Preload] Loading %s", candidate.fileName.c_str());

			const LoadResult load = LoadLibrarySEH(candidate.path.c_str());
			if (load.crashed)
			{
				LogCrash("DllMain", candidate.fileName.c_str());
				Reject(candidate, {}, Status::Crashed, "faulted inside its own DllMain");
				ClearBreadcrumb();
				return nullptr;
			}

			if (!load.module)
			{
				const DWORD error = GetLastError();
				char detail[192]{};
				sprintf_s(detail, "LoadLibrary failed with error %lu%s", error,
					error == 126 ? " (a DLL it depends on is missing -- check the VC++ redistributable)" :
					error == 193 ? " (not a 64-bit DLL)" :
					error == 5   ? " (access denied -- the file may be blocked or quarantined)" : "");
				LogToFile::Error("[Preload] %s: %s", candidate.fileName.c_str(), detail);
				Reject(candidate, {}, Status::LoadFailed, detail);
				ClearBreadcrumb();
				return nullptr;
			}

			auto getInfo  = reinterpret_cast<GetPreloadInfoFunc>(GetProcAddress(load.module, PRELOAD_GET_INFO_FUNC_NAME));
			auto scan     = reinterpret_cast<PreloadScanFunc>(GetProcAddress(load.module, PRELOAD_SCAN_FUNC_NAME));
			auto init     = reinterpret_cast<PreloadInitFunc>(GetProcAddress(load.module, PRELOAD_INIT_FUNC_NAME));
			auto shutdown = reinterpret_cast<PreloadShutdownFunc>(GetProcAddress(load.module, PRELOAD_SHUTDOWN_FUNC_NAME));

			if (!getInfo || !init)
			{
				LogToFile::Error("[Preload] %s: missing required exports -- skipping", candidate.fileName.c_str());
				Reject(candidate, {}, Status::MissingExports,
					"does not export GetPreloadInfo and PreloadInit -- is this an ordinary plugin in the wrong folder?");
				FreeLibrary(load.module);
				ClearBreadcrumb();
				return nullptr;
			}

			PreloadInfo* info = CallGetInfoSEH(getInfo);
			if (!info)
			{
				if (g_lastCrash.code)
					LogCrash("GetPreloadInfo", candidate.fileName.c_str());
				Reject(candidate, {}, Status::Crashed, "GetPreloadInfo returned null or faulted");
				FreeLibrary(load.module);
				ClearBreadcrumb();
				return nullptr;
			}

			const std::string name = (info->name && info->name[0]) ? info->name : candidate.fileName;

			if (info->interfaceVersion < PRELOAD_INTERFACE_VERSION_MIN ||
				info->interfaceVersion > PRELOAD_INTERFACE_VERSION_MAX)
			{
				char detail[224]{};
				sprintf_s(detail, "built for preload interface v%d, this loader supports v%d-%d%s",
					info->interfaceVersion, PRELOAD_INTERFACE_VERSION_MIN, PRELOAD_INTERFACE_VERSION_MAX,
					info->interfaceVersion > PRELOAD_INTERFACE_VERSION_MAX
						? " -- update the mod loader" : " -- update the plugin");
				LogToFile::Error("[Preload] %s: %s", name.c_str(), detail);
				Reject(candidate, name, Status::BadInterface, detail);
				FreeLibrary(load.module);
				ClearBreadcrumb();
				return nullptr;
			}

#if defined(MODLOADER_CLIENT_BUILD)
			constexpr int kThisTarget    = PRELOAD_TARGET_CLIENT;
			const char*   kThisTargetName = "client";
#else
			constexpr int kThisTarget    = PRELOAD_TARGET_SERVER;
			const char*   kThisTargetName = "server";
#endif
			if (info->target != kThisTarget)
			{
				char detail[192]{};
				sprintf_s(detail, "built for the %s executable, this is the %s one",
					info->target == PRELOAD_TARGET_CLIENT ? "client" : "server", kThisTargetName);
				LogToFile::Info("[Preload] %s: %s -- skipping", name.c_str(), detail);
				Reject(candidate, name, Status::WrongTarget, detail);
				FreeLibrary(load.module);
				ClearBreadcrumb();
				return nullptr;
			}

			auto entry      = std::make_unique<Loaded>();
			entry->module   = load.module;
			entry->scan     = scan;
			entry->init     = init;
			entry->shutdown = shutdown;
			entry->path     = candidate.path;
			entry->fileName = candidate.fileName;
			entry->name     = name;
			entry->version  = (info->version && info->version[0]) ? info->version : "";
			entry->author   = (info->author  && info->author[0])  ? info->author  : "";
			entry->priority = info->priority;

			entry->self.name    = entry->name.c_str();
			entry->self.version = entry->version.c_str();
			entry->self.logger  = ModLoaderLogger::GetPluginLogger();
			entry->self.config  = nullptr;   // no config manager exists at Stage 1
			entry->self.hooks   = nullptr;   // no engine exists at Stage 1

			LogToFile::Info("[Preload] %s v%s by %s (priority %d)",
				entry->name.c_str(),
				entry->version.empty() ? "?" : entry->version.c_str(),
				entry->author.empty()  ? "?" : entry->author.c_str(),
				entry->priority);

			ClearBreadcrumb();
			return entry;
		}

		// --- Pass 2: scan, then install --------------------------------------
		//
		// Returns true when the plugin is running. On any failure the module is
		// freed, every hook it managed to install is taken back out, and the
		// caller carries on with the next one.
		bool Activate(Loaded& entry)
		{
			Record record;
			record.fileName = entry.fileName;
			record.name     = entry.name;
			record.version  = entry.version;
			record.author   = entry.author;
			record.priority = entry.priority;

			WriteBreadcrumb(entry.fileName);

			// Resolve. Same session, same scanner and same report as an ordinary
			// plugin's OnPluginLoadHooks, including the rule that one miss --
			// or one pattern that matches twice -- refuses the plugin. A preload
			// plugin with no patterns to resolve has no PreloadScan export, and
			// that is not an error.
			if (entry.scan)
			{
				PluginHookReport::BeginSession(&entry.self, entry.name.c_str(),
					entry.fileName.c_str(), /*preload*/ true);

				const bool survived = CallScanSEH(entry.scan, &entry.self,
					ModLoaderLogger::GetPluginHookScanner());

				if (!survived)
				{
					LogCrash("PreloadScan", entry.name.c_str());
					PluginHookReport::RecordSessionCrash(&entry.self,
						"crashed while resolving its patterns -- see modloader.log for the fault address");
				}

				bool refused = false;
				PluginHookReport::EndSession(&entry.self, &refused);

				if (refused)
				{
					record.status = survived ? Status::HookScanFailed : Status::Crashed;
					record.detail = survived
						? "one or more patterns did not resolve -- run `hookfailures` for the detail"
						: "faulted while resolving its patterns";
					g_records.push_back(std::move(record));
					FreeLibrary(entry.module);
					entry.module = nullptr;
					ClearBreadcrumb();
					return false;
				}
			}

			// Install.
			const InitResult result = CallInitSEH(entry.init, &entry.self, PreloadPatch::GetTable());

			if (result.crashed)
			{
				LogCrash("PreloadInit", entry.name.c_str());

				// It may have installed hooks before it faulted, and its module
				// is about to go away -- every one of those detours would then
				// be a jump into freed memory, firing on the game thread with
				// nothing left to attribute it to.
				const int removed = PreloadPatch::RemoveAllHooksFor(&entry.self);
				if (removed)
					LogToFile::Error("[Preload] %s: removed %d hook(s) it had installed before it crashed",
						entry.name.c_str(), removed);

				record.status = Status::Crashed;
				record.detail = "faulted inside PreloadInit";
				g_records.push_back(std::move(record));
				FreeLibrary(entry.module);
				entry.module = nullptr;
				ClearBreadcrumb();
				return false;
			}

			if (!result.retval)
			{
				PreloadPatch::RemoveAllHooksFor(&entry.self);

				LogToFile::Info("[Preload] %s: declined to run (PreloadInit returned false)", entry.name.c_str());
				record.status = Status::InitFailed;
				record.detail = "PreloadInit returned false";
				g_records.push_back(std::move(record));
				FreeLibrary(entry.module);
				entry.module = nullptr;
				ClearBreadcrumb();
				return false;
			}

			record.status         = Status::Running;
			record.hooksInstalled = PreloadPatch::CountHooksFor(&entry.self);

			LogToFile::Info("[Preload] %s is running (%d hook(s) installed)",
				entry.name.c_str(), record.hooksInstalled);

			g_records.push_back(std::move(record));
			ClearBreadcrumb();
			return true;
		}
	}

	const char* StatusName(Status status)
	{
		switch (status)
		{
		case Status::Running:           return "running";
		case Status::Disabled:          return "disabled";
		case Status::SkippedAfterCrash: return "skipped (crashed last launch)";
		case Status::BadInterface:      return "wrong interface version";
		case Status::WrongTarget:       return "wrong build target";
		case Status::MissingExports:    return "missing exports";
		case Status::HookScanFailed:    return "pattern scan failed";
		case Status::InitFailed:        return "init failed";
		case Status::Crashed:           return "crashed";
		case Status::LoadFailed:
		default:                        return "load failed";
		}
	}

	void RunPhase()
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		if (g_didRun || !g_skipReason.empty())
			return;

		// Rule 2 in preload_manager.h. On the injected path the main thread was
		// frozen mid-instruction and may hold the loader lock, so LoadLibrary
		// from here can deadlock the process before the game draws a frame --
		// and the pre-start window this phase exists for is gone anyway.
		if (!g_mainThreadParked)
		{
			g_skipReason = "the mod loader was injected into a running game, so there is no pre-start window";
			LogToFile::Info("[Preload] Phase skipped: %s", g_skipReason.c_str());
			return;
		}

		if (CommandLineHas(L"-NoPreload"))
		{
			g_skipReason = "-NoPreload was passed on the command line";
			LogToFile::Info("[Preload] Phase skipped: %s", g_skipReason.c_str());
			return;
		}

		if (!EnabledInIni())
		{
			g_skipReason = "[Preload] Enabled=0 in modloader.ini";
			LogToFile::Info("[Preload] Phase skipped: %s", g_skipReason.c_str());
			return;
		}

		const std::vector<Candidate> candidates = CollectCandidates();
		if (candidates.empty())
		{
			g_didRun = true;
			LogToFile::Info("[Preload] No preload plugins found in ModLoader\\Preload");
			return;
		}

		const std::string crashedLastLaunch = ReadBreadcrumb();
		const std::vector<std::string> disabled = DisabledList();

		LogToFile::Info("[Preload] ---- Preload phase: %zu DLL(s) ----", candidates.size());

		// Pass 1: load and vet, so priorities are known before anything runs.
		std::vector<std::unique_ptr<Loaded>> vetted;
		for (const Candidate& candidate : candidates)
		{
			if (auto entry = LoadAndVet(candidate, disabled, crashedLastLaunch))
				vetted.push_back(std::move(entry));
		}

		// Priority, then file name. This is the order two plugins patching the
		// same function will see each other in, so it has to be stable and it
		// has to be something an author can control.
		std::stable_sort(vetted.begin(), vetted.end(),
			[](const std::unique_ptr<Loaded>& a, const std::unique_ptr<Loaded>& b)
			{
				if (a->priority != b->priority)
					return a->priority < b->priority;
				return _stricmp(a->fileName.c_str(), b->fileName.c_str()) < 0;
			});

		// Pass 2: scan and install, in that order.
		int index = 0;
		for (auto& entry : vetted)
		{
			++index;
#ifdef MODLOADER_CLIENT_BUILD
			{
				wchar_t message[192]{};
				swprintf_s(message, L"Preload: %S (%d/%zu)", entry->name.c_str(), index, vetted.size());
				Splash::SetSubStatus(message);
				Splash::SetSubProgress(static_cast<float>(index) / static_cast<float>(vetted.size()));
			}
#endif
			if (Activate(*entry))
				g_running.push_back(std::move(entry));
		}

#ifdef MODLOADER_CLIENT_BUILD
		Splash::ClearSubBar();
#endif

		ClearBreadcrumb();

		int running = 0;
		int failed  = 0;
		for (const Record& r : g_records)
		{
			if (r.status == Status::Running)
				++running;
			else if (r.status != Status::Disabled && r.status != Status::WrongTarget)
				++failed;
		}

		g_didRun = true;
		LogToFile::Info("[Preload] ---- Preload phase complete: %d running, %d failed, %zu total ----",
			running, failed, g_records.size());

		if (failed > 0)
		{
			LogToFile::Warn("[Preload] %d preload plugin(s) did not run. The game is unaffected and will", failed);
			LogToFile::Warn("[Preload] start normally -- see the lines above, or run `preload` in the console.");
		}
	}

	void Shutdown()
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		if (g_running.empty())
			return;

		LogToFile::Info("[Preload] Shutting down %zu preload plugin(s)", g_running.size());

		// Hooks first, always. A PreloadShutdown that runs while its own detour
		// is still installed can be re-entered through it, and once the module
		// is gone an installed detour is a jump into nothing.
		for (const auto& entry : g_running)
		{
			const int removed = PreloadPatch::RemoveAllHooksFor(&entry->self);
			if (removed)
				LogToFile::Info("[Preload] %s: removed %d hook(s)", entry->name.c_str(), removed);
		}

		for (const auto& entry : g_running)
		{
			if (entry->shutdown)
				CallShutdownSEH(entry->shutdown);
		}

		// Deliberately no FreeLibrary. The loader is shutting down, the process
		// is on its way out, and unmapping a module that may still have a thread
		// or a callback in flight buys nothing at this point. The hooks are
		// gone, which is the part that would have crashed.
		g_running.clear();
	}

	std::vector<Record> GetRecords()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_records;
	}

	bool DidRun()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_didRun;
	}

	std::string GetSkipReason()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_skipReason;
	}
}
