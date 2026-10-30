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
// initialisers are not set up, so a plugin that calls into the game during the
// event lands in the SEH guard below and is reported as CRASH.
//
// self->hooks is null, as it is for the duration of the event in game. The
// logger prints to stderr; the config hands back every default.
//
// Verdict, as the loader decides it (plugins/plugin_hook_report.h): any
// recorded failure refuses the plugin, required or optional, and so does a
// crash inside the event. Raw scans (FindAllPatterns*) record nothing with the
// loader; they are listed for information and the plugin decides what a miss
// means.
#include "plugin_scan.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "memory_scanner/hook_scanner_interface.h"
#include "memory_scanner/scanner.h"
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
	};

	std::vector<Row> g_rows;

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

		const bool badRequest = !request || !local.hookName || !local.hookName[0] || !local.pattern
			|| !local.pattern[0] || local.kind < PLUGIN_SCAN_FUNCTION_START || local.kind > PLUGIN_SCAN_ANY;

		return Record(std::move(row), local.module, badRequest,
			[&] { return Loader().Resolve(self, request); });
	}

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
		Resolve
	};

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
		__except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
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

	void PrintDetail(FILE* out, const std::string& detail)
	{
		size_t start = 0;
		while (start <= detail.size())
		{
			const size_t end = detail.find("\r\n", start);
			const std::string line = detail.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!line.empty())
				fprintf(out, "    %s\n", line.c_str());
			if (end == std::string::npos)
				break;
			start = end + 2;
		}
	}

	void PrintTable(FILE* out, const std::string& file, const PluginInfo* info, const char* verdict, const Summary& s,
		const std::string& crash)
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
				PrintDetail(out, row.detail);
		}
		if (!crash.empty())
			fprintf(out, "OnPluginLoadHooks crashed: %s\n", crash.c_str());
		fprintf(out, "%s: %zu recorded patterns: %zu OK, %zu MISSING, %zu AMBIGUOUS, %zu other, %zu reported by the plugin; "
			"%zu failures (any refuses the plugin); %zu raw scans\n",
			verdict, s.total, s.ok, s.missing, s.ambiguous, s.other, s.reported, s.failing, s.raw);
	}

	void PrintJson(const std::string& file, const PluginInfo* info, bool hasLoadHooks, const char* verdict,
		const Summary& s, const std::string& crash)
	{
		using AobScan::JsonEscape;
		printf("{\n  \"target\": \"%s\",\n", kTargetName);
		printf("  \"plugin\": { \"file\": \"%s\"", JsonEscape(file).c_str());
		if (info)
			printf(", \"name\": \"%s\", \"version\": \"%s\", \"interfaceVersion\": %d",
				JsonEscape(info->name ? info->name : "").c_str(), JsonEscape(info->version ? info->version : "").c_str(),
				info->interfaceVersion);
		printf(", \"hasLoadHooks\": %s },\n", hasLoadHooks ? "true" : "false");
		printf("  \"verdict\": \"%s\",\n", verdict);
		if (!crash.empty())
			printf("  \"crash\": \"%s\",\n", JsonEscape(crash).c_str());
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
				printf(", \"detail\": \"%s\"", JsonEscape(row.detail).c_str());
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
}

namespace AobScan
{
	int ScanPlugin(HMODULE image, const wchar_t* dllPath, bool json, bool verbose)
	{
		g_image   = image;
		g_verbose = verbose;

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

		std::string crash;
		if (loadHooks)
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
				crash = "exception " + Hex(code);
			g_session.self = nullptr;
		}

		const Summary s = Summarise();
		const char* verdict = !loadHooks ? "NONE" : !crash.empty() ? "CRASH" : s.failing ? "FAIL" : "PASS";

		PrintTable(json ? stderr : stdout, file, info, verdict, s, crash);
		if (json)
			PrintJson(file, info, loadHooks != nullptr, verdict, s, crash);

		return (!crash.empty() || s.failing) ? kExitFailed : kExitOk;
	}
}
