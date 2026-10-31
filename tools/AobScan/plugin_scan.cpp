// sr-aobscan --plugin <dll>: a plugin's patterns, checked the way the loader
// checks them.
//
// The patterns are not read from a list. The plugin DLL is loaded and its own
// OnPluginLoadHooks export is called, exactly as PluginManager calls it before
// PluginInit, with the loader's own IPluginHookScanner table
// (memory_scanner/hook_scanner_interface.cpp, compiled in as it is). So every
// pattern the plugin resolves on this build of its source is checked, through
// the same ScanValidation calls, with the same kinds, follows and offsets, and
// whatever the plugin works out for itself between scans (raw finds, its own
// unwind or RIP decoding, ReportFailure) runs as it does in game.
//
// The one difference is where "the game" is. Inside this process the main
// module is the game exe as MapImage laid it out: GetModuleHandle(nullptr) is
// redirected to the mapped image in this tool's own imports and in the
// plugin's, so loader code and plugin code that ask for the main module both
// get the game. Each section then gets the page protection the Windows loader
// would give it, because plugins check that (an executable branch target, a
// readable global). Game code is never started: its imports, TLS and
// initialisers are not set up, so game code a plugin calls during the event
// faults. Such a call is stopped (see "Calls into the game" below) and the
// hook carries on; any other fault is reported as CRASH.
//
// self->hooks is null, as it is for the duration of the event in game. The
// logger prints to stderr; the config hands back every default.
//
// The hook then runs a second time, with the scan cache the first run filled,
// as on the next launch of the same exe: there the loader re-checks only what
// it picked before ("agreed on an earlier launch"). Every row the plugin asks
// for again must come out as it did the first time. The report is the first
// run's; the second only adds its line to the note, or turns a pass into FAIL.
// It is skipped after a crash or a stopped game call.
//
// A request can carry locators for other game builds (interface 72). Their
// patterns are listed in the row after the request's own, and the loader's
// "[Map]" line, which says which locator it took, goes to stderr.
//
// Verdict, as the loader decides it (plugins/plugin_hook_report.h): any
// recorded failure refuses the plugin, required or optional, and so does a
// crash inside the event or an interface version outside the loader's range.
// Raw scans (FindAllPatterns*) record nothing with the loader; they are listed
// for information and the plugin decides what a miss means.
//
//   PASS       no recorded failure
//   GAME-CALL  PARTIAL coverage, never a full pass: no recorded failure, at
//              least one pattern resolved, and the hook's calls into game code
//              were stopped. A plugin that bails out on the null a stopped call
//              returns skips every later scan; those are not exercised
//   FAIL       a recorded failure (MISSING, AMBIGUOUS, ...), game calls or not,
//              or a row the second run answered differently
//   CRASH      a fault that is not a game call, or a game call with no
//              pattern resolved
//   REFUSED    interface version outside [MIN, MAX]; the hook is not run.
//              Or, on a build other than the loader's kBuiltCL, a plugin the
//              loader does not admit there (VersionMap::OffBuildRefusal): from
//              before interface 72, or one that asked the map for nothing. The
//              hook still runs, so its rows say what would need repair. The
//              loader's third condition, GetSdkGlobals in PluginInit, is
//              checked in game only.
//   NONE       no OnPluginLoadHooks export
#include "plugin_scan.h"
#include "suggest.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "memory_scanner/hook_scanner_interface.h"
#include "memory_scanner/scanner.h"
#include "memory_scanner/version_map.h"
#include "plugins/plugin_hook_report.h"

namespace
{
#if defined(MODLOADER_SERVER_BUILD)
	constexpr int         kTarget     = PLUGIN_TARGET_SERVER;
	constexpr const char* kTargetName = "server";
#else
	constexpr int         kTarget     = PLUGIN_TARGET_CLIENT;
	constexpr const char* kTargetName = "client";
#endif

	// A failed resolve is classified by counting the raw pattern matches, which
	// is the uniqueness verdict ScanValidation applies; this many is plenty to
	// tell "none", "one" and "several" apart in a report.
	constexpr size_t kCountCap = 64;

	HMODULE g_image   = nullptr;
	bool    g_verbose = false;

	// ---- The main module is the mapped game image ---------------------------

	using GetModuleHandleWFn = HMODULE(WINAPI*)(LPCWSTR);
	using GetModuleHandleAFn = HMODULE(WINAPI*)(LPCSTR);
	GetModuleHandleWFn g_realGetModuleHandleW = nullptr;
	GetModuleHandleAFn g_realGetModuleHandleA = nullptr;

	HMODULE WINAPI ImageGetModuleHandleW(LPCWSTR name)
	{
		return name ? g_realGetModuleHandleW(name) : g_image;
	}

	HMODULE WINAPI ImageGetModuleHandleA(LPCSTR name)
	{
		return name ? g_realGetModuleHandleA(name) : g_image;
	}

	// Points module's imports of GetModuleHandleW/A at the two above.
	bool RedirectMainModule(HMODULE module)
	{
		auto* base = reinterpret_cast<uint8_t*>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress)
			return true;

		for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc)
		{
			if (!desc->OriginalFirstThunk)
				continue;
			const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
			auto*       slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++slots)
			{
				if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
					continue;
				const char* fn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
				ULONGLONG replacement = 0;
				if (strcmp(fn, "GetModuleHandleW") == 0)      replacement = reinterpret_cast<ULONGLONG>(&ImageGetModuleHandleW);
				else if (strcmp(fn, "GetModuleHandleA") == 0) replacement = reinterpret_cast<ULONGLONG>(&ImageGetModuleHandleA);
				else continue;

				DWORD old = 0;
				if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old))
					return false;
				slots->u1.Function = replacement;
				VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
			}
		}
		return true;
	}

	// Gives each section of the mapped image the protection its characteristics
	// ask for, and the headers read-only, as the Windows loader does.
	bool ProtectSections(HMODULE module)
	{
		auto* base = reinterpret_cast<uint8_t*>(module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const size_t align = nt->OptionalHeader.SectionAlignment ? nt->OptionalHeader.SectionAlignment : 0x1000;
		const auto   round = [align](size_t n) { return (n + align - 1) & ~(align - 1); };

		DWORD old = 0;
		if (!VirtualProtect(base, round(nt->OptionalHeader.SizeOfHeaders), PAGE_READONLY, &old))
			return false;

		const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			const size_t size = round(section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData);
			if (!size)
				continue;
			const DWORD c = section->Characteristics;
			const bool  x = (c & IMAGE_SCN_MEM_EXECUTE) != 0;
			const bool  r = (c & IMAGE_SCN_MEM_READ) != 0;
			const bool  w = (c & IMAGE_SCN_MEM_WRITE) != 0;
			const DWORD protect = x ? (w ? PAGE_EXECUTE_READWRITE : r ? PAGE_EXECUTE_READ : PAGE_EXECUTE)
			                        : (w ? PAGE_READWRITE : r ? PAGE_READONLY : PAGE_NOACCESS);
			if (!VirtualProtect(base + section->VirtualAddress, size, protect, &old))
				return false;
		}
		return true;
	}

	// ---- What the plugin is handed as self ----------------------------------

	void Print(const IPluginSelf* self, const char* level, const char* format, va_list args)
	{
		char text[2048];
		vsnprintf(text, sizeof(text), format, args);
		fprintf(stderr, "  [%s] %s: %s\n", (self && self->name) ? self->name : "plugin", level, text);
	}

	void LogLine(PluginLogLevel, const IPluginSelf* self, const char* message)
	{
		fprintf(stderr, "  [%s] %s\n", (self && self->name) ? self->name : "plugin", message ? message : "");
	}

#define AOBSCAN_LOG_FN(fnName, level, always)                                  \
	void fnName(const IPluginSelf* self, const char* format, ...)              \
	{                                                                          \
		if (!(always) && !g_verbose) return;                                   \
		va_list args;                                                          \
		va_start(args, format);                                                \
		Print(self, level, format, args);                                      \
		va_end(args);                                                          \
	}
	AOBSCAN_LOG_FN(LogTrace, "TRACE", false)
	AOBSCAN_LOG_FN(LogDebug, "DEBUG", false)
	AOBSCAN_LOG_FN(LogInfo,  "INFO",  true)
	AOBSCAN_LOG_FN(LogWarn,  "WARN",  true)
	AOBSCAN_LOG_FN(LogError, "ERROR", true)
#undef AOBSCAN_LOG_FN

	bool ReadString(const IPluginSelf*, const char*, const char*, char* out, int maxLen, const char* def)
	{
		if (out && maxLen > 0)
			strncpy_s(out, static_cast<size_t>(maxLen), def ? def : "", _TRUNCATE);
		return true;
	}
	bool  WriteString(const IPluginSelf*, const char*, const char*, const char*) { return true; }
	int   ReadInt(const IPluginSelf*, const char*, const char*, int def) { return def; }
	bool  WriteInt(const IPluginSelf*, const char*, const char*, int) { return true; }
	float ReadFloat(const IPluginSelf*, const char*, const char*, float def) { return def; }
	bool  WriteFloat(const IPluginSelf*, const char*, const char*, float) { return true; }
	bool  ReadBool(const IPluginSelf*, const char*, const char*, bool def) { return def; }
	bool  WriteBool(const IPluginSelf*, const char*, const char*, bool) { return true; }
	bool  InitializeFromSchema(const IPluginSelf*, const ConfigSchema*) { return true; }
	void  ValidateConfig(const IPluginSelf*, const ConfigSchema*) {}

	// ---- The scan session (stands in for PluginHookReport) -------------------

	struct Failure
	{
		std::string hookName;
		std::string detail;
		bool        required = true;
	};

	struct Session
	{
		const IPluginSelf*   self     = nullptr;
		int                  resolved = 0;
		std::vector<Failure> failures;
		bool                 usedMap  = false;   // a request with candidates, or ResolveField
	} g_session;

	// ---- What the plugin asked for, one row per call --------------------------

	struct Candidate
	{
		std::string pattern;
		size_t      matches = 0;
	};

	struct Row
	{
		std::string            call;            // the IPluginHookScanner entry used
		std::string            name;            // hookName, empty for a raw scan
		std::vector<Candidate> patterns;        // one, or the candidate list
		std::string            kind;
		bool                   required = true;
		bool                   recorded = true; // false: a raw scan the loader does not judge
		int32_t                resultOffset  = 0;
		int32_t                followRel32At = -1;
		bool                   otherModule   = false;
		std::string            result;
		bool                   hasRva = false;
		uint64_t               rva    = 0;
		std::string            detail;
		AobScan::Suggest::Result suggest;  // MISSING / AMBIGUOUS, with --suggest
	};

	std::vector<Row> g_rows;
	AobScan::Suggest::Session* g_suggest = nullptr;

	const IPluginHookScanner& Loader()
	{
		return *ModLoaderLogger::GetPluginHookScanner();
	}

	size_t CountMatches(HMODULE module, const char* pattern)
	{
		if (!pattern || !pattern[0])
			return 0;
		return Scanner::CountMatchesInModule(module ? module : g_image, pattern, kCountCap, nullptr);
	}

	void SetRva(Row& row, uintptr_t address)
	{
		const auto base = reinterpret_cast<uintptr_t>(g_image);
		if (!row.otherModule && address >= base)
		{
			row.hasRva = true;
			row.rva    = address - base;
		}
	}

	// Runs one recorded resolve through the loader and fills in the row from
	// its verdict. A failure is classified by the raw match count of each
	// pattern: none is MISSING, several is AMBIGUOUS, exactly one means the
	// match was found but refused (WRONGKIND, or BADREQUEST for a request the
	// loader turned down before scanning).
	template <typename Fn>
	uintptr_t Record(Row row, HMODULE module, bool badRequest, Fn&& resolve)
	{
		const size_t failuresBefore = g_session.failures.size();
		const uintptr_t address = resolve();

		if (address)
		{
			row.result = "OK";
			for (Candidate& c : row.patterns)
				c.matches = CountMatches(module, c.pattern.c_str());
			SetRva(row, address);
		}
		else
		{
			size_t total = 0, most = 0;
			for (Candidate& c : row.patterns)
			{
				c.matches = CountMatches(module, c.pattern.c_str());
				total += c.matches;
				if (c.matches > most) most = c.matches;
			}
			if (badRequest || row.patterns.empty())  row.result = "BADREQUEST";
			else if (total == 0)                     row.result = "MISSING";
			else if (most > 1)                       row.result = "AMBIGUOUS";
			else                                     row.result = "WRONGKIND";

			for (size_t i = failuresBefore; i < g_session.failures.size(); ++i)
			{
				if (!row.detail.empty()) row.detail += "\r\n";
				row.detail += g_session.failures[i].detail;
			}

			// Locators for several builds that each match once but land on
			// different addresses: the loader's selection says so itself.
			if (row.detail.rfind("AMBIGUOUS", 0) == 0)
				row.result = "AMBIGUOUS";
		}
		g_rows.push_back(std::move(row));
		return address;
	}

	const char* KindShortName(int kind)
	{
		switch (kind)
		{
		case PLUGIN_SCAN_FUNCTION_START: return "function";
		case PLUGIN_SCAN_IN_FUNCTION:    return "infunction";
		case PLUGIN_SCAN_CODE:           return "code";
		case PLUGIN_SCAN_DATA:           return "data";
		case PLUGIN_SCAN_VTABLE:         return "vtable";
		case PLUGIN_SCAN_ANY:            return "any";
		default:                         return "unspecified";
		}
	}

	Row LegacyRow(const char* call, const char* hookName, const char* pattern, HMODULE module, bool required)
	{
		Row row;
		row.call        = call;
		row.name        = hookName ? hookName : "";
		row.kind        = "any";
		row.required    = required;
		row.otherModule = module != nullptr && module != g_image;
		row.patterns.push_back({ pattern ? pattern : "" });
		return row;
	}

	// ---- The table handed to the plugin ---------------------------------------
	// Each entry records the call and passes it to the loader's own entry.

	uintptr_t ResolveRequired(const IPluginSelf* self, const char* hookName, const char* pattern)
	{
		return Record(LegacyRow("ResolveRequired", hookName, pattern, nullptr, true), nullptr,
			!hookName || !hookName[0] || !pattern || !pattern[0],
			[&] { return Loader().ResolveRequired(self, hookName, pattern); });
	}

	uintptr_t ResolveOptional(const IPluginSelf* self, const char* hookName, const char* pattern)
	{
		return Record(LegacyRow("ResolveOptional", hookName, pattern, nullptr, false), nullptr,
			!hookName || !hookName[0] || !pattern || !pattern[0],
			[&] { return Loader().ResolveOptional(self, hookName, pattern); });
	}

	uintptr_t ResolveRequiredInModule(const IPluginSelf* self, const char* hookName, HMODULE module, const char* pattern)
	{
		return Record(LegacyRow("ResolveRequiredInModule", hookName, pattern, module, true), module,
			!hookName || !hookName[0] || !pattern || !pattern[0],
			[&] { return Loader().ResolveRequiredInModule(self, hookName, module, pattern); });
	}

	uintptr_t ResolveOptionalInModule(const IPluginSelf* self, const char* hookName, HMODULE module, const char* pattern)
	{
		return Record(LegacyRow("ResolveOptionalInModule", hookName, pattern, module, false), module,
			!hookName || !hookName[0] || !pattern || !pattern[0],
			[&] { return Loader().ResolveOptionalInModule(self, hookName, module, pattern); });
	}

	Row UniqueRow(const char* call, const char* hookName, const char** patterns, int count, bool required)
	{
		Row row;
		row.call     = call;
		row.name     = hookName ? hookName : "";
		row.kind     = "any";
		row.required = required;
		for (int i = 0; patterns && i < count; ++i)
			if (patterns[i] && patterns[i][0])
				row.patterns.push_back({ patterns[i] });
		return row;
	}

	uintptr_t ResolveRequiredUnique(const IPluginSelf* self, const char* hookName, const char** patterns, int count, int* outIndex)
	{
		return Record(UniqueRow("ResolveRequiredUnique", hookName, patterns, count, true), nullptr,
			!hookName || !hookName[0] || !patterns || count <= 0,
			[&] { return Loader().ResolveRequiredUnique(self, hookName, patterns, count, outIndex); });
	}

	uintptr_t ResolveOptionalUnique(const IPluginSelf* self, const char* hookName, const char** patterns, int count, int* outIndex)
	{
		return Record(UniqueRow("ResolveOptionalUnique", hookName, patterns, count, false), nullptr,
			!hookName || !hookName[0] || !patterns || count <= 0,
			[&] { return Loader().ResolveOptionalUnique(self, hookName, patterns, count, outIndex); });
	}

	// Raw scans record nothing with the loader. Listed once per pattern.
	void RecordRaw(const char* call, const char* pattern, HMODULE module, int count, const uintptr_t* out, int maxResults)
	{
		const std::string text = pattern ? pattern : "";
		for (const Row& row : g_rows)
			if (!row.recorded && !row.patterns.empty() && row.patterns[0].pattern == text)
				return;

		Row row;
		row.call        = call;
		row.kind        = "raw";
		row.required    = false;
		row.recorded    = false;
		row.otherModule = module != nullptr && module != g_image;
		row.patterns.push_back({ text, static_cast<size_t>(count > 0 ? count : 0) });
		row.result      = count == 0 ? "MISSING" : count == 1 ? "OK" : "AMBIGUOUS";
		if (count >= 1 && out && maxResults > 0)
			SetRva(row, out[0]);
		g_rows.push_back(std::move(row));
	}

	int FindAllPatternsInMainModule(const IPluginSelf* self, const char* pattern, uintptr_t* out, int maxResults)
	{
		const int count = Loader().FindAllPatternsInMainModule(self, pattern, out, maxResults);
		RecordRaw("FindAllPatternsInMainModule", pattern, nullptr, count, out, maxResults);
		return count;
	}

	int FindAllPatternsInModule(const IPluginSelf* self, HMODULE module, const char* pattern, uintptr_t* out, int maxResults)
	{
		const int count = Loader().FindAllPatternsInModule(self, module, pattern, out, maxResults);
		RecordRaw("FindAllPatternsInModule", pattern, module, count, out, maxResults);
		return count;
	}

	int FindXrefsToAddress(const IPluginSelf* self, uintptr_t target, uintptr_t start, size_t size, PluginXRef* out, int maxResults)
	{
		return Loader().FindXrefsToAddress(self, target, start, size, out, maxResults);
	}

	int FindXrefsToAddressInModule(const IPluginSelf* self, uintptr_t target, HMODULE module, PluginXRef* out, int maxResults)
	{
		return Loader().FindXrefsToAddressInModule(self, target, module, out, maxResults);
	}

	int FindXrefsToAddressInMainModule(const IPluginSelf* self, uintptr_t target, PluginXRef* out, int maxResults)
	{
		return Loader().FindXrefsToAddressInMainModule(self, target, out, maxResults);
	}

	void RecordReported(const char* call, const char* hookName, const char* detail, bool required)
	{
		Row row;
		row.call     = call;
		row.name     = hookName ? hookName : "(unnamed)";
		row.kind     = "reported";
		row.required = required;
		row.result   = "REPORTED";
		row.detail   = detail ? detail : "";
		g_rows.push_back(std::move(row));
	}

	void ReportFailure(const IPluginSelf* self, const char* hookName, const char* detail)
	{
		RecordReported("ReportFailure", hookName, detail, true);
		Loader().ReportFailure(self, hookName, detail);
	}

	void ReportWarning(const IPluginSelf* self, const char* hookName, const char* detail)
	{
		RecordReported("ReportWarning", hookName, detail, false);
		Loader().ReportWarning(self, hookName, detail);
	}

	bool HasFailures(const IPluginSelf* self)
	{
		return Loader().HasFailures(self);
	}

	uintptr_t Resolve(const IPluginSelf* self, const PluginScanRequest* request)
	{
		// Read the request the way the loader does: no further than the
		// plugin's own structSize.
		PluginScanRequest local{};
		local.followRel32At = -1;
		if (request && request->structSize >= offsetof(PluginScanRequest, kind) + sizeof(int))
			memcpy(&local, request, request->structSize < sizeof(local) ? request->structSize : sizeof(local));

		Row row;
		row.call          = "Resolve";
		row.name          = local.hookName ? local.hookName : "";
		row.kind          = KindShortName(local.kind);
		row.required      = (local.flags & PLUGIN_SCAN_FLAG_OPTIONAL) == 0;
		row.resultOffset  = local.resultOffset;
		row.followRel32At = (local.flags & PLUGIN_SCAN_FLAG_FOLLOW_REL32) ? local.followRel32At : -1;
		row.otherModule   = local.module != nullptr && local.module != g_image;
		row.patterns.push_back({ local.pattern ? local.pattern : "" });
#if PLUGIN_INTERFACE_VERSION >= 72
		// Locators for other game builds, after the request's own pattern.
		for (uint32_t i = 0; local.candidates && i < local.candidateCount && i < 64; ++i)
			if (local.candidates[i].pattern && local.candidates[i].pattern[0])
				row.patterns.push_back({ local.candidates[i].pattern });
#endif

		const bool badRequest = !request || !local.hookName || !local.hookName[0] || !local.pattern
			|| !local.pattern[0] || local.kind < PLUGIN_SCAN_FUNCTION_START || local.kind > PLUGIN_SCAN_ANY;

		return Record(std::move(row), local.module, badRequest,
			[&] { return Loader().Resolve(self, request); });
	}

#if PLUGIN_INTERFACE_VERSION >= 72
	bool ResolveField(const IPluginSelf* self, const PluginFieldRequest* request, PluginFieldResult* result)
	{
		return Loader().ResolveField(self, request, result);
	}
#endif

	IPluginHookScanner g_scanner = {
		ResolveRequired,
		ResolveOptional,
		ResolveRequiredInModule,
		ResolveOptionalInModule,
		ResolveRequiredUnique,
		ResolveOptionalUnique,
		FindAllPatternsInMainModule,
		FindAllPatternsInModule,
		FindXrefsToAddress,
		FindXrefsToAddressInModule,
		FindXrefsToAddressInMainModule,
		ReportFailure,
		ReportWarning,
		HasFailures,
		Resolve,
#if PLUGIN_INTERFACE_VERSION >= 72
		ResolveField,
#endif
	};

	// ---- Calls into the game --------------------------------------------------
	// Game code cannot run here, so a plugin that calls a game function from its
	// load hook (MiniMap asks for a StaticStruct once its patterns resolve) gets
	// a fault. Whether the fault is such a call is decided from the stack alone:
	// the faulting code is game code, or the unbound value of one of the game's
	// import slots (game code calling or jumping to an import), and every frame
	// between it and the plugin is game code. The call is then stopped: the
	// plugin resumes just after it as if it had returned 0, so the rest of the
	// hook still runs and every pattern it goes on to resolve is still seen.
	// What the plugin would only have done had the call succeeded is not seen,
	// which is why a game call never upgrades a verdict, only keeps a pass
	// from becoming a false CRASH. Any other fault is a CRASH.

	constexpr int kMaxGameCalls = 64;

	struct GameCall
	{
		DWORD       code       = 0;
		uintptr_t   callSite   = 0;        // the plugin's return address
		uintptr_t   function   = 0;        // the outermost game function entered, or 0
		const char* importDll  = nullptr;  // set when the fault was an unbound import
		const char* importName = nullptr;
	};

	HMODULE  g_plugin     = nullptr;
	size_t   g_pluginSize = 0;
	size_t   g_imageSize  = 0;
	int      g_gameCalls  = 0;
	GameCall g_firstGameCall;
	size_t   g_rowsAtFirstCall = 0;  // scans done when the first game call was stopped

	size_t ImageSize(HMODULE module)
	{
		const auto* base = reinterpret_cast<const uint8_t*>(module);
		const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew)->OptionalHeader.SizeOfImage;
	}

	bool InModule(uintptr_t address, HMODULE module, size_t size)
	{
		const auto base = reinterpret_cast<uintptr_t>(module);
		return module && address >= base && address - base < size;
	}

	// Code, not data: a jump into the game's data is the plugin's own fault.
	bool IsGameCode(uintptr_t address)
	{
		MEMORY_BASIC_INFORMATION mbi{};
		constexpr DWORD kExecutable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		return InModule(address, g_image, g_imageSize)
			&& VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == sizeof(mbi)
			&& (mbi.Protect & kExecutable) != 0;
	}

	// True when address is what one of the game's import slots holds. The
	// slots are never bound here, so they still hold the RVA of the import's
	// name, and a call through one lands on that value.
	bool FindUnboundImport(uintptr_t address, const char*& dll, const char*& name)
	{
		const auto* base = reinterpret_cast<const uint8_t*>(g_image);
		const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt   = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress || address == 0 || address >= g_imageSize)
			return false;

		for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc)
		{
			const auto* slot  = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
			const auto* names = desc->OriginalFirstThunk
				? reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk) : slot;
			for (; slot->u1.Function; ++slot, ++names)
			{
				if (slot->u1.Function != address)
					continue;
				dll  = reinterpret_cast<const char*>(base + desc->Name);
				name = IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) ? nullptr
					: reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
				return true;
			}
		}
		return false;
	}

	// Unwinds c from the fault to the plugin frame that called into the game.
	// Returns false, leaving the fault a CRASH, unless every frame on the way
	// is game code (or, innermost, an unbound game import).
	bool UnwindGameCall(CONTEXT& c, GameCall& call)
	{
		ULONG_PTR stackLow = 0, stackHigh = 0;
		GetCurrentThreadStackLimits(&stackLow, &stackHigh);

		bool inGame = false;
		for (int depth = 0; depth < 256; ++depth)
		{
			const uintptr_t ip = c.Rip;
			if (InModule(ip, g_plugin, g_pluginSize))
			{
				call.callSite = ip;
				return inGame;
			}

			DWORD64           base     = 0;
			PRUNTIME_FUNCTION function = nullptr;
			if (IsGameCode(ip))
			{
				function      = RtlLookupFunctionEntry(ip, &base, nullptr);
				call.function = function ? base + function->BeginAddress : ip;
			}
			else if (depth != 0 || !FindUnboundImport(ip, call.importDll, call.importName))
				return false;
			inGame = true;

			if (function)
			{
				void*   handlerData = nullptr;
				DWORD64 frame       = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ip, function, &c, &handlerData, &frame, nullptr);
			}
			else
			{
				// A leaf, or the import's landing address: the return address is
				// on top of the stack.
				if (c.Rsp < stackLow || c.Rsp + sizeof(DWORD64) > stackHigh)
					return false;
				c.Rip  = *reinterpret_cast<const DWORD64*>(c.Rsp);
				c.Rsp += sizeof(DWORD64);
			}
		}
		return false;
	}

	// The load hook's exception filter. Stops a game call and resumes the
	// plugin after it; anything else unwinds to the handler as a CRASH.
	int OnLoadHooksException(EXCEPTION_POINTERS* e, DWORD& code)
	{
		code = e->ExceptionRecord->ExceptionCode;
		if (code == EXCEPTION_STACK_OVERFLOW || (e->ExceptionRecord->ExceptionFlags & EXCEPTION_NONCONTINUABLE)
			|| g_gameCalls >= kMaxGameCalls)
			return EXCEPTION_EXECUTE_HANDLER;

		CONTEXT  c = *e->ContextRecord;
		GameCall call;
		call.code = code;
		if (!UnwindGameCall(c, call))
			return EXCEPTION_EXECUTE_HANDLER;

		if (g_gameCalls++ == 0)
		{
			g_firstGameCall   = call;
			g_rowsAtFirstCall = g_rows.size();
		}
		c.Rax  = 0;
		c.Xmm0 = {};
		*e->ContextRecord = c;
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	// ---- Calling into the plugin ----------------------------------------------

	HMODULE LoadSEH(const wchar_t* path, DWORD& code)
	{
		__try
		{
			return LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		}
		__except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	PluginInfo* GetInfoSEH(GetPluginInfoFunc fn, DWORD& code)
	{
		__try
		{
			return fn();
		}
		__except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	bool LoadHooksSEH(PluginLoadHooksFunc fn, IPluginSelf* self, IPluginHookScanner* scanner, DWORD& code)
	{
		__try
		{
			fn(self, scanner);
			return true;
		}
		__except (OnLoadHooksException(GetExceptionInformation(), code))
		{
			return false;
		}
	}

	std::string Narrow(const wchar_t* text)
	{
		char buf[MAX_PATH * 2]{};
		WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, static_cast<int>(sizeof(buf)), nullptr, nullptr);
		return buf;
	}

	std::string Hex(uint64_t value)
	{
		char buf[24];
		sprintf_s(buf, "0x%llX", static_cast<unsigned long long>(value));
		return buf;
	}

	struct Summary
	{
		size_t total = 0, ok = 0, missing = 0, ambiguous = 0, other = 0, reported = 0, failing = 0, raw = 0;
	};

	Summary Summarise()
	{
		Summary s;
		for (const Row& row : g_rows)
		{
			if (!row.recorded) { ++s.raw; continue; }
			++s.total;
			if (row.result == "OK")             ++s.ok;
			else if (row.result == "MISSING")   ++s.missing;
			else if (row.result == "AMBIGUOUS") ++s.ambiguous;
			else if (row.result == "REPORTED")  ++s.reported;
			else                                ++s.other;
		}
		s.failing = g_session.failures.size();
		return s;
	}

	// The verdict, and what the report says about it beyond the counts.
	struct Outcome
	{
		const char* verdict = "PASS";
		std::string crash;     // CRASH: what faulted
		std::string note;      // GAME-CALL and REFUSED: the one-line reason
		std::string gameCall;  // the first game call, when there was one
		std::string second;    // what the second run, with the cache warm, found
		bool        secondDiffers = false;
	};

	std::string DescribeGameCall(const std::string& file, const GameCall& call)
	{
		std::string text = file + "+" + Hex(call.callSite - reinterpret_cast<uintptr_t>(g_plugin)) + " called ";
		if (call.function)
		{
			const uint64_t rva = call.function - reinterpret_cast<uintptr_t>(g_image);
			std::string name;
			for (const Row& row : g_rows)
				if (row.recorded && row.hasRva && row.rva == rva && !row.otherModule)
				{
					name = row.name;
					break;
				}
			text += (name.empty() ? "game code" : name) + " (RVA " + Hex(rva) + ")";
			if (call.importDll)
				text += ", which called its import ";
		}
		else
			text += "the game's import ";
		if (call.importDll)
			text += std::string(call.importDll) + "!" + (call.importName ? call.importName : "(ordinal)") + ", not bound offline";
		return text + "; exception " + Hex(call.code);
	}

	bool SameRequest(const Row& a, const Row& b)
	{
		if (a.call != b.call || a.name != b.name || a.patterns.size() != b.patterns.size())
			return false;
		for (size_t i = 0; i < a.patterns.size(); ++i)
			if (a.patterns[i].pattern != b.patterns[i].pattern)
				return false;
		return true;
	}

	std::string Answer(const Row& row)
	{
		return row.result + (row.hasRva ? " at " + Hex(row.rva) : "");
	}

	// The hook again, with the scan cache the first run stored (see the top of
	// this file). Leaves the first run's rows, session and game calls as they
	// were.
	void SecondPass(PluginLoadHooksFunc loadHooks, IPluginSelf* self, Outcome& o)
	{
		std::vector<Row> first = std::move(g_rows);
		const Session  session         = g_session;
		const int      gameCalls       = g_gameCalls;
		const GameCall firstCall       = g_firstGameCall;
		const size_t   rowsAtFirstCall = g_rowsAtFirstCall;
		g_rows.clear();

		AobScan::WarmScanCache();
		fputs("Second run, with the scan cache the first one filled:\n", stderr);
		DWORD code = 0;
		const bool ran    = LoadHooksSEH(loadHooks, self, &g_scanner, code);
		const bool called = g_gameCalls != gameCalls;

		std::vector<Row> second = std::move(g_rows);
		g_rows            = std::move(first);
		g_session         = session;
		g_gameCalls       = gameCalls;
		g_firstGameCall   = firstCall;
		g_rowsAtFirstCall = rowsAtFirstCall;

		o.secondDiffers = true;
		if (!ran)
		{
			o.second = "the second run (scan cache warm) crashed, exception " + Hex(code);
			return;
		}
		if (called)
		{
			o.second = "the second run (scan cache warm) called into the game; the first did not";
			return;
		}

		std::vector<bool> used(g_rows.size());
		size_t same = 0;
		std::string diff;
		for (const Row& again : second)
		{
			const Row* before = nullptr;
			for (size_t i = 0; i < g_rows.size() && !before; ++i)
				if (!used[i] && SameRequest(g_rows[i], again))
				{
					used[i] = true;
					before  = &g_rows[i];
				}
			if (before && before->result == again.result && before->hasRva == again.hasRva && before->rva == again.rva)
			{
				++same;
				continue;
			}
			if (diff.empty())
				diff = (again.name.empty() ? "(raw) " + (again.patterns.empty() ? std::string() : again.patterns[0].pattern) : again.name)
					+ ": " + (before ? Answer(*before) : std::string("not asked in the first run")) + ", then " + Answer(again);
		}
		o.secondDiffers = !diff.empty();
		o.second = "second run, scan cache warm: " + std::to_string(same) + "/" + std::to_string(second.size())
			+ " rows the same" + (diff.empty() ? "" : "; " + diff);
	}

	void PrintTable(FILE* out, const std::string& file, const PluginInfo* info, const Outcome& o, const Summary& s)
	{
		fprintf(out, "sr-aobscan --plugin (%s build): %s", kTargetName, file.c_str());
		if (info)
			fprintf(out, " = %s %s (interface %d)", info->name ? info->name : "?", info->version ? info->version : "?",
				info->interfaceVersion);
		fprintf(out, "\n%-10s %-60s %-10s %-4s %7s  %s\n", "RESULT", "NAME", "KIND", "REQ", "MATCHES", "RVA");
		for (const Row& row : g_rows)
		{
			const std::string rva = row.hasRva ? Hex(row.rva) : "-";
			size_t matches = 0;
			for (const Candidate& c : row.patterns) matches += c.matches;
			const std::string name = row.name.empty() ? "(raw) " + (row.patterns.empty() ? "" : row.patterns[0].pattern) : row.name;
			fprintf(out, "%-10s %-60.60s %-10s %-4s %7zu  %s\n", row.result.c_str(), name.c_str(), row.kind.c_str(),
				!row.recorded ? "-" : row.required ? "yes" : "no", matches, rva.c_str());
			if (row.recorded && row.result != "OK" && !row.detail.empty())
				AobScan::PrintDetail(out, row.detail);
			AobScan::Suggest::PrintText(out, row.suggest);
		}
		if (g_gameCalls)
			fprintf(out, "Game calls stopped: %d. The first: %s\n", g_gameCalls, o.gameCall.c_str());
		if (!o.crash.empty())
			fprintf(out, "OnPluginLoadHooks crashed: %s\n", o.crash.c_str());
		fprintf(out, "%s: %zu recorded patterns: %zu OK, %zu MISSING, %zu AMBIGUOUS, %zu other, %zu reported by the plugin; "
			"%zu failures (any refuses the plugin); %zu raw scans\n",
			o.verdict, s.total, s.ok, s.missing, s.ambiguous, s.other, s.reported, s.failing, s.raw);
		if (!o.note.empty())
			fprintf(out, "%s: %s\n", o.verdict, o.note.c_str());
	}

	void PrintJson(const std::string& file, const PluginInfo* info, bool hasLoadHooks, const Outcome& o, const Summary& s)
	{
		using AobScan::JsonEscape;
		printf("{\n  \"target\": \"%s\",\n", kTargetName);
		if (g_suggest)
			printf("  \"suggest\": %s,\n", g_suggest->EnvJson().c_str());
		printf("  \"plugin\": { \"file\": \"%s\"", JsonEscape(file).c_str());
		if (info)
			printf(", \"name\": \"%s\", \"version\": \"%s\", \"interfaceVersion\": %d",
				JsonEscape(info->name ? info->name : "").c_str(), JsonEscape(info->version ? info->version : "").c_str(),
				info->interfaceVersion);
		printf(", \"hasLoadHooks\": %s },\n", hasLoadHooks ? "true" : "false");
		printf("  \"verdict\": \"%s\",\n", o.verdict);
		if (!o.note.empty())
			printf("  \"note\": \"%s\",\n", JsonEscape(o.note).c_str());
		if (!o.crash.empty())
			printf("  \"crash\": \"%s\",\n", JsonEscape(o.crash).c_str());
		if (g_gameCalls)
			printf("  \"gameCalls\": { \"stopped\": %d, \"first\": \"%s\" },\n", g_gameCalls, JsonEscape(o.gameCall).c_str());
		printf("  \"summary\": { \"total\": %zu, \"ok\": %zu, \"missing\": %zu, \"ambiguous\": %zu, \"other\": %zu, "
		       "\"reported\": %zu, \"failing\": %zu, \"raw\": %zu },\n  \"patterns\": [\n",
			s.total, s.ok, s.missing, s.ambiguous, s.other, s.reported, s.failing, s.raw);
		for (size_t i = 0; i < g_rows.size(); ++i)
		{
			const Row& row = g_rows[i];
			printf("    { \"call\": \"%s\", \"name\": \"%s\", \"kind\": \"%s\", \"required\": %s, \"recorded\": %s, "
			       "\"result\": \"%s\"",
				row.call.c_str(), JsonEscape(row.name).c_str(), row.kind.c_str(), row.required ? "true" : "false",
				row.recorded ? "true" : "false", row.result.c_str());
			if (row.patterns.size() == 1)
				printf(", \"pattern\": \"%s\", \"matches\": %zu", JsonEscape(row.patterns[0].pattern).c_str(), row.patterns[0].matches);
			else if (!row.patterns.empty())
			{
				printf(", \"candidates\": [");
				for (size_t c = 0; c < row.patterns.size(); ++c)
					printf("%s{ \"pattern\": \"%s\", \"matches\": %zu }", c ? ", " : "",
						JsonEscape(row.patterns[c].pattern).c_str(), row.patterns[c].matches);
				printf("]");
			}
			if (row.followRel32At >= 0)
				printf(", \"followRel32At\": %d", row.followRel32At);
			if (row.resultOffset != 0)
				printf(", \"resultOffset\": %d", row.resultOffset);
			if (row.otherModule)
				printf(", \"otherModule\": true");
			if (row.hasRva)
				printf(", \"rva\": \"%s\"", Hex(row.rva).c_str());
			if (!row.detail.empty())
				printf(", \"detail\": \"%s\"", JsonEscape(AobScan::JsonDetail(row.detail)).c_str());
			if (row.suggest.attempted)
				printf(", \"suggest\": %s", AobScan::Suggest::Json(row.suggest).c_str());
			printf(" }%s\n", i + 1 < g_rows.size() ? "," : "");
		}
		printf("  ]\n}\n");
	}
}

// The four PluginHookReport calls hook_scanner_interface.cpp makes, kept for
// the one session this tool opens.
namespace PluginHookReport
{
	bool HasSession(const IPluginSelf* self)
	{
		return self && self == g_session.self;
	}

	void RecordResolved(const IPluginSelf* self)
	{
		if (HasSession(self))
			++g_session.resolved;
	}

	void RecordFailure(const IPluginSelf* self, const char* hookName, const char* detail, bool required)
	{
		if (HasSession(self))
			g_session.failures.push_back({ hookName ? hookName : "(unnamed)", detail ? detail : "", required });
	}

	bool SessionHasFailures(const IPluginSelf* self)
	{
		return HasSession(self) && !g_session.failures.empty();
	}

	void RecordMapUse(const IPluginSelf* self)
	{
		if (HasSession(self))
			g_session.usedMap = true;
	}

	bool SessionUsedMap(const IPluginSelf* self)
	{
		return HasSession(self) && g_session.usedMap;
	}
}

namespace AobScan
{
	int ScanPlugin(HMODULE image, const wchar_t* dllPath, bool json, bool verbose, Suggest::Session* suggest)
	{
		g_image     = image;
		g_suggest   = suggest;
		g_imageSize = ImageSize(image);
		g_verbose   = verbose;

		wchar_t fullPath[MAX_PATH]{};
		if (!GetFullPathNameW(dllPath, MAX_PATH, fullPath, nullptr))
		{
			fprintf(stderr, "sr-aobscan: %ls: bad path\n", dllPath);
			return kExitBadInput;
		}
		const wchar_t* slash = wcsrchr(fullPath, L'\\');
		const std::string file = Narrow(slash ? slash + 1 : fullPath);

		const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
		g_realGetModuleHandleW = reinterpret_cast<GetModuleHandleWFn>(GetProcAddress(kernel32, "GetModuleHandleW"));
		g_realGetModuleHandleA = reinterpret_cast<GetModuleHandleAFn>(GetProcAddress(kernel32, "GetModuleHandleA"));
		const HMODULE self_ = GetModuleHandleW(nullptr);
		if (!g_realGetModuleHandleW || !g_realGetModuleHandleA || !RedirectMainModule(self_))
		{
			fprintf(stderr, "sr-aobscan: cannot redirect the main module to the game image\n");
			return kExitBadInput;
		}
		if (!ProtectSections(image))
		{
			fprintf(stderr, "sr-aobscan: cannot set the game image's section protections, error %lu\n", GetLastError());
			return kExitBadInput;
		}

		DWORD code = 0;
		const HMODULE plugin = LoadSEH(fullPath, code);
		if (!plugin)
		{
			fprintf(stderr, "sr-aobscan: %s: %s\n", file.c_str(),
				code ? ("DllMain crashed, exception " + Hex(code)).c_str()
				     : ("LoadLibrary failed, error " + std::to_string(GetLastError())).c_str());
			return kExitBadInput;
		}
		if (!RedirectMainModule(plugin))
		{
			fprintf(stderr, "sr-aobscan: %s: cannot redirect its main module to the game image\n", file.c_str());
			return kExitBadInput;
		}
		g_plugin     = plugin;
		g_pluginSize = ImageSize(plugin);

		const auto getInfo   = reinterpret_cast<GetPluginInfoFunc>(GetProcAddress(plugin, PLUGIN_GET_INFO_FUNC_NAME));
		const auto loadHooks = reinterpret_cast<PluginLoadHooksFunc>(GetProcAddress(plugin, PLUGIN_LOAD_HOOKS_FUNC_NAME));
		if (!getInfo)
		{
			fprintf(stderr, "sr-aobscan: %s has no %s export, so it is not a plugin\n", file.c_str(), PLUGIN_GET_INFO_FUNC_NAME);
			return kExitBadInput;
		}
		const PluginInfo* info = GetInfoSEH(getInfo, code);
		if (!info)
		{
			fprintf(stderr, "sr-aobscan: %s: GetPluginInfo %s\n", file.c_str(),
				code ? ("crashed, exception " + Hex(code)).c_str() : "returned null");
			return kExitBadInput;
		}
		if (info->pluginTarget != kTarget)
		{
			fprintf(stderr, "sr-aobscan: %s is not a %s plugin\n", file.c_str(), kTargetName);
			return kExitBadInput;
		}

		Outcome o;
		const bool refused = info->interfaceVersion < PLUGIN_INTERFACE_VERSION_MIN
			|| info->interfaceVersion > PLUGIN_INTERFACE_VERSION_MAX;
		if (refused)
			o.note = "interface " + std::to_string(info->interfaceVersion) + " is outside the loader's ["
				+ std::to_string(PLUGIN_INTERFACE_VERSION_MIN) + ", " + std::to_string(PLUGIN_INTERFACE_VERSION_MAX)
				+ "], so the loader does not load it";
		else if (loadHooks)
		{
			static IPluginLogger logger{};
			logger.Log   = LogLine;
			logger.Trace = LogTrace;
			logger.Debug = LogDebug;
			logger.Info  = LogInfo;
			logger.Warn  = LogWarn;
			logger.Error = LogError;

			static IPluginConfig config{};
			config.ReadString           = ReadString;
			config.WriteString          = WriteString;
			config.ReadInt              = ReadInt;
			config.WriteInt             = WriteInt;
			config.ReadFloat            = ReadFloat;
			config.WriteFloat           = WriteFloat;
			config.ReadBool             = ReadBool;
			config.WriteBool            = WriteBool;
			config.InitializeFromSchema = InitializeFromSchema;
			config.ValidateConfig       = ValidateConfig;

			static IPluginSelf self{};
			self.name    = info->name;
			self.version = info->version;
			self.logger  = &logger;
			self.config  = &config;
			self.hooks   = nullptr;

			g_session.self = &self;
			code = 0;
			if (!LoadHooksSEH(loadHooks, &self, &g_scanner, code))
				o.crash = "exception " + Hex(code)
					+ (g_gameCalls ? " after " + std::to_string(g_gameCalls) + " game call(s) were stopped" : "");
			else if (!g_gameCalls)
				SecondPass(loadHooks, &self, o);
			g_session.self = nullptr;
		}

		// Proposals for what failed, once the plugin is done asking.
		if (suggest)
			for (Row& row : g_rows)
			{
				if (!row.recorded || row.otherModule || (row.result != "MISSING" && row.result != "AMBIGUOUS"))
					continue;
				Suggest::Request failed;
				failed.name          = row.name;
				failed.kind          = ScanValidation::ParseKind(row.kind.c_str());
				failed.resultOffset  = row.resultOffset;
				failed.followRel32At = row.followRel32At;
				for (const Candidate& c : row.patterns)
					failed.patterns.push_back(c.pattern);
				row.suggest = suggest->Propose(failed);
			}

		const Summary s = Summarise();
		if (g_gameCalls)
			o.gameCall = DescribeGameCall(file, g_firstGameCall);
		// A stopped game call only keeps a pass from turning into a false
		// CRASH, and only once something was checked: with no pattern resolved
		// there is no evidence the hook ever reached its patterns.
		const bool patternsOk = s.total > 0 && s.ok == s.total && !s.failing;
		const std::string offBuild = refused ? std::string()
			: VersionMap::OffBuildRefusal(VersionMap::RunningExe(), info->interfaceVersion, g_session.usedMap, true);
		if (refused)               o.verdict = "REFUSED";
		else if (!offBuild.empty())
		{
			o.verdict = "REFUSED";
			o.note    = offBuild;
		}
		else if (!loadHooks)       o.verdict = "NONE";
		else if (!o.crash.empty()) o.verdict = "CRASH";
		else if (s.failing)        o.verdict = "FAIL";
		else if (o.secondDiffers)  o.verdict = "FAIL";
		else if (!g_gameCalls)     o.verdict = "PASS";
		else if (patternsOk)
		{
			o.verdict = "GAME-CALL";
			size_t before = 0, beforeOk = 0;
			for (size_t i = 0; i < g_rowsAtFirstCall && i < g_rows.size(); ++i)
			{
				if (!g_rows[i].recorded) continue;
				++before;
				if (g_rows[i].result == "OK") ++beforeOk;
			}
			o.note    = std::to_string(beforeOk) + "/" + std::to_string(before)
				+ " resolved before first game call; later scans not exercised";
		}
		else
		{
			o.verdict = "CRASH";
			o.crash   = "called into the game before any pattern resolved";
		}

		if (!o.second.empty())
			o.note = o.note.empty() ? o.second : o.note + "; " + o.second;

		PrintTable(json ? stderr : stdout, file, info, o, s);
		if (json)
			PrintJson(file, info, loadHooks != nullptr, o, s);

		const std::string verdict = o.verdict;
		return (verdict == "PASS" || verdict == "GAME-CALL" || verdict == "NONE") ? kExitOk : kExitFailed;
	}
}
