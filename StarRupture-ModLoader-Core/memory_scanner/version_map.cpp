#include "memory_scanner/version_map.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/scan_cache.h"
#include "memory_scanner/scanner.h"
#include "core/version_check.h"
#include "hooks/game/scan_patterns.h"
#include "hooks/game/version_map.generated.h"
#include "logging/logger.h"
#include "plugins/plugin_interface.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>

namespace VersionMap
{
	namespace
	{
		const char* NL = "\r\n";

		std::string Hex(unsigned long long v)
		{
			char buf[32]{};
			sprintf_s(buf, "0x%llX", v);
			return buf;
		}

		std::string Hex8(uint32_t v)
		{
			char buf[16]{};
			sprintf_s(buf, "0x%08X", v);
			return buf;
		}

		std::string ClText(uint32_t cl)
		{
			return cl ? "CL-" + std::to_string(cl) : std::string("an unknown CL");
		}

		// "loader map, CL-128786 row" or "own CL-127004 row", for the [Map] line.
		std::string RowLabel(const Candidate& c)
		{
			return (c.fromMap ? "loader map, " : "own ") + ClText(c.gameCL) + " row";
		}

		ScanValidation::Request WithLocator(const ScanValidation::Request& base, const Candidate& c)
		{
			ScanValidation::Request r = base;
			r.pattern         = c.pattern;
			r.resultOffset    = c.resultOffset;
			r.followRel32At   = c.followRel32At;
			r.followRipDispAt = c.followRipDispAt;
			return r;
		}

		// FNV-1a over everything that decides the agreement verdict: the
		// request's own locator, every candidate, and the exe it ran on.
		struct Hash
		{
			uint64_t h = 1469598103934665603ull;
			void Bytes(const void* p, size_t n)
			{
				const auto* b = static_cast<const uint8_t*>(p);
				for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
			}
			void Text(const std::string& s) { Bytes(s.data(), s.size()); Bytes("", 1); }
			template <typename T> void Value(T v) { Bytes(&v, sizeof(v)); }
		};

		std::string SelectionCacheKey(const ScanValidation::Request& request,
			const std::vector<Candidate>& candidates, const Exe& running)
		{
			Hash h;
			h.Text(request.pattern);
			h.Value(request.resultOffset);
			h.Value(request.followRel32At);
			h.Value(request.followRipDispAt);
			h.Value(static_cast<int>(request.kind));
			h.Value(running.imageStamp);
			h.Value(running.imageSize);
			for (const Candidate& c : candidates)
			{
				h.Value(c.gameCL); h.Value(c.imageStamp); h.Value(c.imageSize);
				h.Text(c.pattern);
				h.Value(c.resultOffset); h.Value(c.followRel32At); h.Value(c.followRipDispAt);
				h.Value(c.rva);
				h.Value(c.fromMap);
			}
			// Not an AOB, so it can never share a scan_cache.ini entry with one.
			return "vmap " + request.name + " " + Hex(h.h);
		}

		struct Tried
		{
			std::string            label;   // "own pattern", "loader map, CL-128786 row"
			ScanValidation::Request request;
			ScanValidation::Result  result;
		};

		void LogPick(const char* owner, const std::string& key, const std::string& how)
		{
			ModLoaderLogger::LogInfo(L"[Map] %S: %S: %S", owner ? owner : "?", key.c_str(), how.c_str());
		}

		// A map row decides which address a plugin gets, so its own pattern is
		// never needed to load. It still has to be unique: a loader without the
		// map (or a build with no row) scans it and refuses a plugin whose
		// pattern matches 0 or 2+ places. Resolve the plugin's own pattern once
		// per process, through scan_cache, and say so when it would be refused.
		// `known` is the result when the caller has already resolved it.
		//
		// A unique pattern is cached by Resolve itself. A pattern that matches 0
		// or 2+ places is not, and Resolve would then scan it again on every
		// launch (with the match enumeration a refusal gets), so its count is
		// kept here under its own key, 1 + the count like the "vmap " entries.
		// The cache section is the exe's, so the count is as good as the scan.
		void WarnIfOwnPatternNotUnique(const char* owner, const ScanValidation::Request& request,
			const ScanValidation::Result* known = nullptr)
		{
			if (request.pattern.empty() || (owner && strcmp(owner, "ModLoader") == 0))
				return;

			static std::mutex  lock;
			static std::set<std::string> checked;
			{
				std::lock_guard<std::mutex> guard(lock);
				if (!checked.insert(request.pattern).second)
					return;
			}

			const std::wstring gameVersion = request.module == nullptr || request.module == GetModuleHandleW(nullptr)
				? GetGameVersionString() : std::wstring();
			const std::string  cacheKey    = "own " + request.pattern;

			size_t count = 1;
			ScanCache::Entry cached;
			if (!gameVersion.empty() && ScanCache::TryGet(gameVersion, cacheKey, cached) && cached.matchCount >= 1)
				count = cached.matchCount - 1;
			else
			{
				const ScanValidation::Result own = known ? *known : ScanValidation::Resolve(request);
				if (own.outcome != ScanValidation::Outcome::NoMatch && own.outcome != ScanValidation::Outcome::MultipleMatches)
					return;
				count = own.matchCount;
				if (!gameVersion.empty())
				{
					ScanCache::Entry entry;
					entry.matchCount = static_cast<uint32_t>(count + 1);
					ScanCache::Store(gameVersion, cacheKey, entry);
				}
			}
			if (count == 1)
				return;

			if (count == 0)
				ModLoaderLogger::LogWarn(L"[Map] %S: %S: its own pattern matches 0 times on this exe (stale for this build?); "
					L"a row decides this key and is carrying it, but a loader without that row would not find the function. Update the pattern.",
					owner ? owner : "?", request.name.c_str());
			else
				ModLoaderLogger::LogWarn(L"[Map] %S: %S: its own pattern matches %zu times on this exe, and a pattern must match exactly once; "
					L"a row decides this key and is carrying it, but a loader without that row refuses this plugin. Make the pattern unique.",
					owner ? owner : "?", request.name.c_str(), count);
		}
	}

	uint32_t ParseCL(const std::wstring& productVersion)
	{
		size_t end = productVersion.size();
		size_t start = end;
		while (start > 0 && productVersion[start - 1] >= L'0' && productVersion[start - 1] <= L'9')
			--start;
		if (start == end || start < 3 || productVersion.compare(start - 3, 3, L"CL-") != 0 || end - start > 9
			|| productVersion[start] == L'0')
			return 0;
		uint32_t cl = 0;
		for (size_t i = start; i < end; ++i)
			cl = cl * 10 + static_cast<uint32_t>(productVersion[i] - L'0');
		return cl;
	}

	const MapData& BaselineMap()
	{
		static const MapData baseline = []
		{
			MapData m;
			for (const Build& b : kBuilds)
				m.builds.push_back({ b.gameCL, b.imageStamp, b.imageSize, b.source ? b.source : "" });
			for (const Index& i : kFunctions)
				for (uint16_t n = i.first; n < i.first + i.count; ++n)
				{
					const FunctionRow& f = kFunctionRows[n];
					m.functions.push_back({ i.key, f.gameCL, f.imageStamp, f.imageSize, f.kind, f.fp, f.absent,
						f.pattern ? f.pattern : "", f.resultOffset, f.followRel32At, f.followRipDispAt, f.rva,
						f.owners ? f.owners : "" });
				}
			for (const Index& i : kFields)
				for (uint16_t n = i.first; n < i.first + i.count; ++n)
				{
					const FieldRow& f = kFieldRows[n];
					m.fields.push_back({ i.key, f.gameCL, f.offset, f.size, f.boolMask, f.member ? f.member : "" });
				}
			return m;
		}();
		return baseline;
	}

	namespace
	{
		std::shared_ptr<const MapData> g_activeMap;   // set at most once, before any read
	}

	const MapData& ActiveMap()
	{
		return g_activeMap ? *g_activeMap : BaselineMap();
	}

	void SetActiveMap(std::shared_ptr<const MapData> map)
	{
		g_activeMap = std::move(map);
	}

	namespace
	{
		std::atomic<bool> g_pluginMapOff{ false };
	}

	void SetPluginMapOff(bool off)
	{
		g_pluginMapOff = off;
	}

	bool PluginMapOff()
	{
		return g_pluginMapOff;
	}

	namespace
	{
		// "0x" and 1 to 8 upper-case hex digits, as the feed writes every hex number.
		bool FeedHex(const std::string& s, uint32_t& v)
		{
			if (s.size() < 3 || s.size() > 10 || s[0] != '0' || s[1] != 'x')
				return false;
			uint64_t n = 0;
			for (size_t i = 2; i < s.size(); ++i)
			{
				const char c = s[i];
				const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
				if (d < 0)
					return false;
				n = n * 16 + static_cast<uint64_t>(d);
			}
			v = static_cast<uint32_t>(n);
			return true;
		}

		// An optional minus and 1 to 10 digits with no leading zero, in [lo, hi].
		bool FeedDec(const std::string& s, int64_t lo, int64_t hi, int64_t& v)
		{
			const size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
			if (s.size() == i || s.size() - i > 10 || (s[i] == '0' && s.size() - i > 1) || (i && s[i] == '0'))
				return false;
			int64_t n = 0;
			for (size_t k = i; k < s.size(); ++k)
			{
				if (s[k] < '0' || s[k] > '9')
					return false;
				n = n * 10 + (s[k] - '0');
			}
			v = i ? -n : n;
			return v >= lo && v <= hi;
		}
	}

	bool ParseFeed(const std::string& text, MapData& out, std::string& error)
	{
		out = MapData{};
		MapData m;
		const auto fail = [&](size_t line, const std::string& why)
		{
			error = "feed line " + std::to_string(line) + ": " + why;
			return false;
		};
		if (text.empty() || text.back() != '\n')
			return fail(1, "the text does not end in a line feed");
		if (text.find('\r') != std::string::npos)
			return fail(1, "a carriage return (the feed is LF only)");

		const int64_t firstKind = static_cast<int>(ScanValidation::Kind::Unspecified) + 1;
		const int64_t lastKind  = static_cast<int>(ScanValidation::Kind::Any);
		int    phase = 0;   // 0 build, 1 function and absent, 2 field
		size_t lineNo = 0;
		std::set<std::tuple<std::string, uint32_t, uint32_t>> seenFunction;
		std::set<std::pair<std::string, uint32_t>> seenField;
		for (size_t pos = 0; pos < text.size();)
		{
			const size_t end = text.find('\n', pos);
			const std::string line = text.substr(pos, end - pos);
			pos = end + 1;
			++lineNo;
			std::vector<std::string> f;
			for (size_t a = 0;;)
			{
				const size_t tab = line.find('\t', a);
				f.push_back(line.substr(a, tab == std::string::npos ? std::string::npos : tab - a));
				if (tab == std::string::npos)
					break;
				a = tab + 1;
			}
			if (lineNo == 1)
			{
				if (f.size() != 2 || f[0] != "format" || f[1] != std::to_string(kFeedFormat))
					return fail(1, "not a format " + std::to_string(kFeedFormat) + " feed");
				continue;
			}
			int64_t cl = 0, kind = 0, ro = 0, r32 = 0, rip = 0;
			uint32_t stamp = 0, size = 0, fp = 0, rva = 0, offset = 0, fsize = 0, mask = 0;
			const std::string& type = f[0];
			if (type == "build" && f.size() == 5)
			{
				if (phase > 0)
					return fail(lineNo, "a build line after a function, absent or field line");
				if (!FeedDec(f[1], 1, 0x7FFFFFFF, cl) || !FeedHex(f[2], stamp) || !FeedHex(f[3], size) || f[4].empty())
					return fail(lineNo, "a build line with a bad number or no source");
				for (const MapBuild& b : m.builds)
					if (b.gameCL == cl)
						return fail(lineNo, "two build lines for CL-" + std::to_string(cl));
				m.builds.push_back({ static_cast<uint32_t>(cl), stamp, size, f[4] });
			}
			else if ((type == "function" && f.size() == 12) || (type == "absent" && f.size() == 8))
			{
				if (phase > 1)
					return fail(lineNo, "a function or absent line after a field line");
				phase = 1;
				const bool absent = type == "absent";
				if (f[1].empty() || !FeedHex(f[2], fp) || !FeedDec(f[3], 1, 0x7FFFFFFF, cl) || !FeedHex(f[4], stamp)
					|| !FeedHex(f[5], size) || !FeedDec(f[6], firstKind, lastKind, kind))
					return fail(lineNo, "a " + type + " line with a bad key, fp, CL, stamp, size or kind");
				const bool global = kind == static_cast<int>(ScanValidation::Kind::FunctionStart)
					|| kind == static_cast<int>(ScanValidation::Kind::Data);
				if (global != (fp == 0))
					return fail(lineNo, "a global key (FunctionStart, Data) has fp 0 and a scoped key has an fp");
				const auto build = std::find_if(m.builds.begin(), m.builds.end(),
					[&](const MapBuild& b) { return b.gameCL == cl; });
				if (build == m.builds.end() || build->imageStamp != stamp || build->imageSize != size)
					return fail(lineNo, "CL-" + std::to_string(cl) + " has no build line with this stamp and size");
				if (!seenFunction.insert({ f[1], fp, static_cast<uint32_t>(cl) }).second)
					return fail(lineNo, "a second row for this key, fp and CL");
				MapFunctionRow row;
				row.key = f[1]; row.gameCL = static_cast<uint32_t>(cl); row.imageStamp = stamp; row.imageSize = size;
				row.kind = static_cast<int32_t>(kind); row.fp = fp; row.absent = absent;
				if (absent)
				{
					if (f[7].empty())
						return fail(lineNo, "an absent line with an empty owner list (use -)");
					row.owners = f[7] == "-" ? "" : f[7];
				}
				else
				{
					if (f[7].empty() || !FeedDec(f[8], -0x7FFFFFFF, 0x7FFFFFFF, ro) || !FeedDec(f[9], -1, 0x7FFFFFFF, r32)
						|| !FeedDec(f[10], -1, 0x7FFFFFFF, rip) || !FeedHex(f[11], rva))
						return fail(lineNo, "a function line with a bad pattern, offset or RVA");
					if (f[7] == "-" && !(stamp && size && rva))
						return fail(lineNo, "an address-only row (pattern -) without the exe's stamp and size and an RVA");
					row.pattern = f[7] == "-" ? "" : f[7];
					row.resultOffset = static_cast<int32_t>(ro); row.followRel32At = static_cast<int32_t>(r32);
					row.followRipDispAt = static_cast<int32_t>(rip); row.rva = rva;
				}
				m.functions.push_back(std::move(row));
			}
			else if (type == "field" && f.size() == 7)
			{
				phase = 2;
				if (f[1].empty() || !FeedDec(f[2], 1, 0x7FFFFFFF, cl) || !FeedHex(f[3], offset) || offset > 0x7FFFFFFF
					|| !FeedHex(f[4], fsize) || fsize == 0 || !FeedHex(f[5], mask) || mask > 0xFF || f[6].empty())
					return fail(lineNo, "a field line with a bad key, CL, offset, size, mask or member");
				if (std::none_of(m.builds.begin(), m.builds.end(), [&](const MapBuild& b) { return b.gameCL == cl; }))
					return fail(lineNo, "CL-" + std::to_string(cl) + " has no build line");
				if (!seenField.insert({ f[1], static_cast<uint32_t>(cl) }).second)
					return fail(lineNo, "a second row for this key and CL");
				m.fields.push_back({ f[1], static_cast<uint32_t>(cl), static_cast<int32_t>(offset), fsize,
					static_cast<uint8_t>(mask), f[6] == "-" ? "" : f[6] });
			}
			else
				return fail(lineNo, "not a feed line");
		}
		out = std::move(m);
		return true;
	}

	std::vector<uint32_t> SupportedBuilds()
	{
		std::vector<uint32_t> builds;
#if defined(MODLOADER_CLIENT_BUILD)
		for (const MapBuild& b : ActiveMap().builds)
			builds.push_back(b.gameCL);
#else
		builds.push_back(kBuiltCL);
#endif
		return builds;
	}

	bool IsSupported(const std::wstring& productVersion)
	{
		const uint32_t cl = ParseCL(productVersion);
		const std::vector<uint32_t> builds = SupportedBuilds();
		return cl != 0 && std::find(builds.begin(), builds.end(), cl) != builds.end();
	}

	namespace
	{
		// "CL-127004", or for gameCL 0 the builds before kLegacyPluginCL.
		std::string DescribeBuiltFor(uint32_t gameCL)
		{
			return gameCL != 0 ? "CL-" + std::to_string(gameCL)
				: "a build before CL-" + std::to_string(kLegacyPluginCL);
		}
	}

	std::string OffBuildRefusal(const Exe& running, int interfaceVersion, const PluginGameBuild& built, bool askedSdkGlobals)
	{
		constexpr int kFirstMapInterface = 72;   // ResolveField, candidates, GetSdkGlobals
		const bool migrated = (built.mapFlags & PLUGIN_MAP_MIGRATED) != 0;
		if (migrated && !built.declared)
			return "It declares that it takes its game data from the version map, but its PluginInfo names no game build"
				" (gameBuiltCL is 0), so the loader cannot tell which build its SDK came from.";
		if (built.gameCL != 0 && running.cl == built.gameCL)
			return {};
		std::string why;
		if (interfaceVersion < kFirstMapInterface)
			why = "it was built for plugin interface v" + std::to_string(interfaceVersion) + ", before the version map";
		else if (!migrated)
			why = std::string("it does not declare PLUGIN_MAP_MIGRATED") + (built.declared ? "" : " and names no game build");
		else if (!askedSdkGlobals)
			why = "it does not take the Game-SDK globals from the loader (GetSdkGlobals)";
		else
			return {};
		const std::string was = DescribeBuiltFor(built.gameCL);
		return "Needs an update to run on CL-" + std::to_string(running.cl) + ": " + why
			+ ", so it would read this game's data where " + was + " had it."
			+ (built.gameCL != 0 ? " It loads on " + was + "." : std::string());
	}

	namespace
	{
		std::mutex               g_layoutChangesLock;
		std::vector<std::string> g_layoutChanges;   // SetLayoutChanges
		std::vector<std::string> g_layoutChangedPaths;

		bool LayoutChanged(const std::string& className)
		{
			std::lock_guard<std::mutex> lock(g_layoutChangesLock);
			return std::find(g_layoutChanges.begin(), g_layoutChanges.end(), className) != g_layoutChanges.end();
		}

		// Whether the class or one of its supers (LiveField::paths) changed.
		bool LayoutChangedWithSupers(const std::vector<std::string>& paths)
		{
			std::lock_guard<std::mutex> lock(g_layoutChangesLock);
			return std::any_of(paths.begin(), paths.end(), [](const std::string& path)
			{
				return std::find(g_layoutChangedPaths.begin(), g_layoutChangedPaths.end(), path) != g_layoutChangedPaths.end();
			});
		}
	}

	void SetLayoutChanges(std::vector<std::string> classNames, std::vector<std::string> paths)
	{
		std::lock_guard<std::mutex> lock(g_layoutChangesLock);
		g_layoutChanges = std::move(classNames);
		g_layoutChangedPaths = std::move(paths);
	}

	std::string LayoutRefusal(int interfaceVersion, const PluginGameBuild& built)
	{
		constexpr int kFirstMapInterface = 72;
		const bool migrated = (built.mapFlags & PLUGIN_MAP_MIGRATED) != 0;
		// The SDK names (not the /Script/ forms); the first two by name, so
		// the reason fits the plugin list's line.
		std::vector<std::string> names;
		{
			std::lock_guard<std::mutex> lock(g_layoutChangesLock);
			for (const std::string& name : g_layoutChanges)
				if (name.find('/') == std::string::npos)
					names.push_back(name);
		}
		if (names.empty() || (interfaceVersion >= kFirstMapInterface && migrated && built.declared))
			return {};
		std::string changed = names[0];
		if (names.size() == 2)
			changed += " and " + names[1];
		else if (names.size() > 2)
			changed += ", " + names[1] + " and " + std::to_string(names.size() - 2) + " more";
		const std::string why = interfaceVersion < kFirstMapInterface
			? "it was built for plugin interface v" + std::to_string(interfaceVersion) + ", before the version map"
			: !migrated ? std::string("it does not declare PLUGIN_MAP_MIGRATED")
			: std::string("its PluginInfo names no game build");
		return "Needs an update: this game build changed the layout of " + changed + ", and " + why
			+ ", so it would read their old layouts.";
	}

	PluginGameBuild ReadPluginGameBuild(const PluginInfo& info)
	{
		PluginGameBuild build;
		if (info.interfaceVersion < kLegacyPluginInterface)
			build.gameCL = 0;
		if (info.interfaceVersion < 72)   // the struct ends before the tail
			return build;
		build.mapFlags = info.mapFlags;
		if (info.gameBuiltCL != 0)
		{
			build.gameCL   = info.gameBuiltCL;
			build.declared = true;
		}
		return build;
	}

	std::string DescribePluginGameBuild(const PluginGameBuild& build)
	{
		if (build.gameCL == 0)
			return DescribeBuiltFor(0);
		return "CL-" + std::to_string(build.gameCL) + (build.declared ? "" : " (none declared)");
	}

	std::string SdkGlobalsWithheld(const Exe& running, uintptr_t gobjects, uintptr_t appendString, uintptr_t gworld)
	{
		std::string missing;
		const auto need = [&](bool found, const char* name)
		{
			if (!found)
				missing += (missing.empty() ? "" : ", ") + std::string(name);
		};
		need(gobjects != 0, "GObjects");
		need(appendString != 0, "FName::AppendString");
		if (running.cl != kBuiltCL)
			need(gworld != 0, "GWorld");
		return missing.empty() ? std::string() : missing + " not found on CL-" + std::to_string(running.cl);
	}

	Exe ExeOf(HMODULE module)
	{
		Exe e;
		e.cl = ParseCL(GetGameVersionString());
		const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
		if (image.valid)
		{
			e.imageStamp = image.stamp;
			e.imageSize  = static_cast<uint32_t>(image.size);
		}
		return e;
	}

	Exe RunningExe()
	{
		static const Exe exe = ExeOf(GetModuleHandleW(nullptr));
		return exe;
	}

	ScanValidation::Result Select(const char* owner, const ScanValidation::Request& request,
		const std::vector<Candidate>& candidates, const Exe& running)
	{
		using ScanValidation::Outcome;
		using ScanValidation::Result;

		HMODULE module = request.module ? request.module : GetModuleHandleW(nullptr);
		const Scanner::ImageInfo image = Scanner::GetImageInfo(module);
		const std::string here = ClText(running.cl);

		// An address-only row is honoured on its own exe only, so it needs all of
		// the exe's stamp and size and the RVA. A plugin's own bad row is its
		// bug on every build; a map row only counts on the build it is for.
		for (const Candidate& c : candidates)
		{
			if (c.fromMap && c.gameCL != running.cl)
				continue;
			if (c.pattern.empty() && !(c.imageStamp && c.imageSize && c.rva))
			{
				Result bad;
				bad.outcome = Outcome::BadRequest;
				bad.detail  = "the " + RowLabel(c) + " has no pattern, so it needs the exe's stamp and size and an RVA";
				return bad;
			}
		}

		// --- 1. Exact: the loader map's row for this build, then the own row ------
		const Candidate* exactMap = nullptr;
		const Candidate* exactOwn = nullptr;
		if (running.cl)
		{
			for (const Candidate& c : candidates)
			{
				if (c.gameCL != running.cl)
					continue;
				const Candidate*& slot = c.fromMap ? exactMap : exactOwn;
				if (slot)
				{
					Result bad;
					bad.outcome = Outcome::BadRequest;
					bad.detail  = std::string("two ") + (c.fromMap ? "loader map" : "own") + " locators for " + here
					            + "; a map has one row per key and build";
					return bad;
				}
				slot = &c;
			}
		}

		const auto onThisExe = [&](const Candidate& c)
		{
			return c.imageStamp && c.imageSize && c.imageStamp == running.imageStamp && c.imageSize == running.imageSize;
		};

		// True when the row decides (out is the verdict), false when it is
		// skipped and the next rule runs.
		const auto tryExact = [&](const Candidate& exact, Result& out)
		{
			const std::string label = RowLabel(exact);
			if (exact.pattern.empty())
			{
				if (onThisExe(exact))
				{
					out = ScanValidation::CheckAddress(request, image.base + exact.rva);
					if (!out.Succeeded())
						out.detail = label + ", address " + Hex(exact.rva) + " on this exe: " + out.detail;
					return true;
				}
				ModLoaderLogger::LogWarn(L"[Map] %S: %S: the %S gives an address for another exe of that build; trying every other locator",
					owner ? owner : "?", request.name.c_str(), label.c_str());
				return false;
			}
			Result r = ScanValidation::Resolve(WithLocator(request, exact));
			if (r.Succeeded())
			{
				const uintptr_t rva = r.address - image.base;
				if (onThisExe(exact) && exact.rva && rva != exact.rva)
				{
					out            = Result{};
					out.outcome    = Outcome::WrongKind;
					out.rawMatch   = r.rawMatch;
					out.address    = r.address;
					out.matchCount = r.matchCount;
					out.detail     = "on the exe the " + label + " was verified on, it resolved to RVA " + Hex(rva)
					               + " instead of the recorded " + Hex(exact.rva)
					               + ": same exe, different answer, so no other locator is tried."
					               + NL + "            Pattern: " + exact.pattern;
					return true;
				}
				out = r;
				return true;
			}
			if (r.outcome != Outcome::NoMatch)
			{
				r.detail = label + ": " + r.detail;
				out = r;
				return true;
			}
			if (onThisExe(exact) && exact.rva)
			{
				r.detail = "on the exe the " + label + " was verified on (RVA " + Hex(exact.rva)
				         + "), its pattern is not found: same exe, different answer, so no other locator is tried."
				         + NL + "            Pattern: " + exact.pattern;
				out = r;
				return true;
			}
			ModLoaderLogger::LogWarn(L"[Map] %S: %S: the %S's pattern is not in this exe; trying every other locator",
				owner ? owner : "?", request.name.c_str(), label.c_str());
			return false;
		};

		std::vector<const Candidate*> skipped;
		for (const Candidate* exact : { exactMap, exactOwn })
		{
			if (!exact)
				continue;
			Result r;
			if (!tryExact(*exact, r))
			{
				skipped.push_back(exact);
				continue;
			}
			if (!r.Succeeded())
				return r;
			std::string how = RowLabel(*exact) + (exact->pattern.empty() ? " (address on this exe)" : "");

			// No scan: an own row for this very exe must say where the map's
			// row landed.
			if (exact == exactMap && exactOwn && onThisExe(*exactOwn) && exactOwn->rva)
			{
				const uintptr_t rva = r.address - image.base;
				if (rva != exactOwn->rva)
				{
					Result bad;
					bad.outcome    = Outcome::MultipleMatches;
					bad.rawMatch   = r.rawMatch;
					bad.address    = r.address;
					bad.matchCount = r.matchCount;
					bad.detail     = "AMBIGUOUS: the " + RowLabel(*exactMap) + " resolves to RVA " + Hex(rva)
					               + ", the " + RowLabel(*exactOwn) + " for this exe says " + Hex(exactOwn->rva);
					LogPick(owner, request.name, bad.detail + " (refused)");
					return bad;
				}
				how += ", the " + RowLabel(*exactOwn) + " agrees";
			}
			LogPick(owner, request.name, how);
			WarnIfOwnPatternNotUnique(owner, request);
			return r;
		}

		// --- 2. Agreement -----------------------------------------------------------
		std::vector<Tried> tried;
		if (!request.pattern.empty())   // a map-key request may carry rows only
			tried.push_back({ "own pattern", request, {} });
		for (const Candidate& c : candidates)
		{
			// An address-only row applies to its own exe, which is the exact row's.
			if (c.pattern.empty() || std::find(skipped.begin(), skipped.end(), &c) != skipped.end())
				continue;
			tried.push_back({ RowLabel(c), WithLocator(request, c), {} });
		}

		const std::wstring gameVersion = module == GetModuleHandleW(nullptr) ? GetGameVersionString() : std::wstring();
		const std::string  cacheKey    = SelectionCacheKey(request, candidates, running);
		std::string lead;
		for (const Candidate* s : skipped)
			lead += "the " + RowLabel(*s) + " missed; ";
		if (lead.empty())
			lead = "no row for " + here + "; ";

		// A verdict already reached on this exe: re-check only the locator that
		// won (its bytes, through its own cache entry) and skip the rest.
		ScanCache::Entry cached;
		if (!gameVersion.empty() && ScanCache::TryGet(gameVersion, cacheKey, cached)
			&& cached.matchCount >= 1 && cached.matchCount <= tried.size())
		{
			Tried& t = tried[cached.matchCount - 1];
			const Result r = ScanValidation::Resolve(t.request);
			if (r.Succeeded() && r.address - image.base == cached.offset)
			{
				LogPick(owner, request.name, lead + t.label + " (agreed on an earlier launch)");
				WarnIfOwnPatternNotUnique(owner, request);
				return r;
			}
		}

		size_t winner = 0;
		uintptr_t address = 0;
		bool disagree = false;
		std::string agreed;
		for (size_t i = 0; i < tried.size(); ++i)
		{
			tried[i].result = ScanValidation::Resolve(tried[i].request);
			if (!tried[i].result.Succeeded())
				continue;
			if (!address)
			{
				address = tried[i].result.address;
				winner  = i;
			}
			else if (tried[i].result.address != address)
				disagree = true;
			agreed += agreed.empty() ? tried[i].label : ", " + tried[i].label;
		}

		if (address && !disagree)
		{
			if (!gameVersion.empty())
			{
				ScanCache::Entry entry;
				entry.offset     = address - image.base;
				entry.matchCount = static_cast<uint32_t>(winner + 1);
				ScanCache::Store(gameVersion, cacheKey, entry);
			}
			LogPick(owner, request.name, lead + agreed + (agreed.find(',') == std::string::npos ? " resolves" : " agree"));
			if (!request.pattern.empty())   // tried[0] is the own pattern, already resolved
				WarnIfOwnPatternNotUnique(owner, request, &tried[0].result);
			return tried[winner].result;
		}

		Result fail;
		std::string list;
		for (const Tried& t : tried)
		{
			list += NL;
			list += "            " + t.label + ": ";
			if (t.result.Succeeded())
				list += "RVA " + Hex(t.result.address - image.base);
			else
			{
				const std::string& d = t.result.detail;
				list += d.substr(0, d.find('\r'));
			}
		}

		if (disagree)
		{
			fail.outcome = Outcome::MultipleMatches;
			fail.detail  = "AMBIGUOUS: " + lead + "the locators that resolve disagree:" + list;
		}
		else
		{
			fail.outcome = Outcome::NoMatch;
			fail.detail  = lead + "no locator resolves:" + list;
		}
		return fail;
	}

	uint32_t Fingerprint(int kind, const std::string& pattern, int32_t resultOffset,
		int32_t followRel32At, int32_t followRipDispAt, uint32_t siteArithmetic)
	{
		const std::vector<Scanner::PatternByte> bytes = Scanner::ParsePattern(pattern);
		if (bytes.empty())
			return 0;
		std::string text;
		for (const Scanner::PatternByte& b : bytes)
		{
			char hex[4]{};
			sprintf_s(hex, "%02X", b.value);
			if (!text.empty())
				text += ' ';
			text += b.wildcard ? "??" : hex;
		}

		uint32_t h = 0x811C9DC5u;
		const auto byte = [&h](uint8_t v) { h ^= v; h *= 0x01000193u; };
		const auto word = [&byte](uint32_t v) { for (int i = 0; i < 4; ++i) byte(static_cast<uint8_t>(v >> (8 * i))); };
		word(static_cast<uint32_t>(kind));
		word(static_cast<uint32_t>(text.size()));
		for (const char ch : text)
			byte(static_cast<uint8_t>(ch));
		word(static_cast<uint32_t>(resultOffset));
		word(static_cast<uint32_t>(followRel32At));
		word(static_cast<uint32_t>(followRipDispAt));
		word(siteArithmetic);
		return h ? h : 1;
	}

	namespace
	{
		bool IsGlobalKind(int kind)
		{
			return kind == static_cast<int>(ScanValidation::Kind::FunctionStart)
				|| kind == static_cast<int>(ScanValidation::Kind::Data);
		}

		bool IsSiteKind(int kind)
		{
			return kind > static_cast<int>(ScanValidation::Kind::Unspecified)
				&& kind <= static_cast<int>(ScanValidation::Kind::Any) && !IsGlobalKind(kind);
		}

		// Whether a global absent row's owner list ("A,B") names `owner`.
		bool ListsOwner(const char* owners, const char* owner)
		{
			const size_t length = owner ? strlen(owner) : 0;
			for (const char* p = owners; p && *p && length; )
			{
				const char*  comma = strchr(p, ',');
				const size_t n     = comma ? static_cast<size_t>(comma - p) : strlen(p);
				if (n == length && strncmp(p, owner, n) == 0)
					return true;
				p = comma ? comma + 1 : nullptr;
			}
			return false;
		}

		// What the loader's map holds for one request: the rows that apply to
		// it (its kind; on a site, its fingerprint), or why none do.
		struct Lookup
		{
			std::vector<Candidate> rows;      // fromMap
			std::string            refusal;   // an absent row for the running build binds this request
			std::string            setAside;  // the key is in the map, but no row of it applies
		};

		Lookup LookUp(const std::string& key, int kind, uint32_t fp, const char* owner, uint32_t runningCL)
		{
			Lookup out;
#if defined(MODLOADER_CLIENT_BUILD)
			const MapData& map = ActiveMap();
			if (key.empty() || std::none_of(map.functions.begin(), map.functions.end(),
					[&](const MapFunctionRow& f) { return f.key == key; }))
				return out;
			int      otherKind  = 0;
			bool     otherFp    = false;
			bool     absentHere = false;
			uint32_t absentCL   = 0;   // the newest build the key is absent on, for a request it binds
			for (const MapFunctionRow& f : map.functions)
			{
				if (f.key != key)
					continue;
				if (f.kind != kind)
				{
					otherKind = f.kind;
					continue;
				}
				if (f.fp != fp)
				{
					otherFp = true;
					continue;
				}
				if (f.absent)
				{
					if (f.gameCL != runningCL)
					{
						if (fp != 0 || ListsOwner(f.owners.c_str(), owner))
							absentCL = std::max(absentCL, f.gameCL);
						continue;
					}
					// A site's absent row binds its fingerprint, a global one the owners it lists.
					if (fp != 0 || ListsOwner(f.owners.c_str(), owner))
					{
						out.rows.clear();
						out.refusal = "loader map: absent on " + ClText(runningCL);
						return out;
					}
					absentHere = true;
					continue;
				}
				Candidate c;
				c.gameCL          = f.gameCL;
				c.imageStamp      = f.imageStamp;
				c.imageSize       = f.imageSize;
				c.pattern         = f.pattern;
				c.resultOffset    = f.resultOffset;
				c.followRel32At   = f.followRel32At;
				c.followRipDispAt = f.followRipDispAt;
				c.rva             = f.rva;
				c.fromMap         = true;
				out.rows.push_back(std::move(c));
			}
			// The map is consulted for the running build's row, and failing that
			// for the latest build's, which Select then rescans on this build.
			// Older rows are not used: the latest one is the one CI last
			// verified, and an older locator that still matches adds nothing a
			// newer one lacks. When the key is absent on a build newer than the
			// latest row, that row is not used either: CI could not place it
			// there, so it is not known to hold.
			uint32_t latestCL = 0;
			for (const Candidate& c : out.rows)
				if (c.gameCL != runningCL && !c.pattern.empty() && c.gameCL > latestCL)
					latestCL = c.gameCL;
			std::string newerAbsent;
			if (absentCL > latestCL)
			{
				if (latestCL)
					newerAbsent = "loader map: absent on " + ClText(absentCL) + ", newer than its latest row";
				latestCL = 0;
			}
			out.rows.erase(std::remove_if(out.rows.begin(), out.rows.end(),
				[&](const Candidate& c) { return c.gameCL != runningCL && c.gameCL != latestCL; }), out.rows.end());

			// Absent on this build for other owners: for this request the key is
			// not placed, so none of its rows is used.
			if (absentHere)
			{
				out.rows.clear();
				out.setAside = "loader map: absent on " + ClText(runningCL) + " for other requests";
			}
			else if (out.rows.empty() && !newerAbsent.empty())
				out.setAside = newerAbsent;
			else if (out.rows.empty() && otherKind)
				out.setAside = std::string("loader map row is for another kind (")
					+ ScanValidation::KindName(static_cast<ScanValidation::Kind>(otherKind)) + ")";
			else if (out.rows.empty() && otherFp)
				out.setAside = "loader map row is for another request (fp " + Hex8(fp) + ")";
#else
			(void)key; (void)kind; (void)fp; (void)owner; (void)runningCL;
#endif
			return out;
		}

		const char* const kRefusedWhy = ": this key could not be placed on this build for this request, so no other locator is tried";

		ScanValidation::Result Failure(ScanValidation::Outcome outcome, const std::string& detail)
		{
			ScanValidation::Result r;
			r.outcome = outcome;
			r.detail  = detail;
			return r;
		}

		// The map's rows for one registry row, as Select reads them.
		std::vector<Candidate> LoaderRows(const ScanPatterns::PreflightEntry& entry, const Exe& running, std::string& refusal)
		{
			Lookup found = LookUp(LoaderEntryKey(entry), static_cast<int>(entry.kind), LoaderEntryFingerprint(entry),
				"ModLoader", running.cl);
			refusal = found.refusal;
			if (!found.setAside.empty())
				ModLoaderLogger::LogInfo(L"[Map] ModLoader: %S: %S; own pattern", entry.name, found.setAside.c_str());

			std::vector<Candidate> rows;
			for (Candidate& c : found.rows)
			{
				// On the build the code was written for, the code's own pattern
				// stands in for the row's, so the row's RVA checks the pattern
				// the code ships: one edited in the code fails here instead of
				// passing on the row it replaced.
				if (c.gameCL == kBuiltCL)
				{
					c.pattern         = entry.pattern;
					c.resultOffset    = 0;
					c.followRel32At   = entry.followRel32At;
					c.followRipDispAt = entry.followRipDispAt;
					rows.push_back(std::move(c));
					continue;
				}
				if (entry.siteArithmetic && c.pattern != entry.pattern)
				{
					ModLoaderLogger::LogWarn(L"[Map] ModLoader: %S: the CL-%u row is not this site's own pattern, so it is not used",
						entry.name, c.gameCL);
					continue;
				}
				rows.push_back(std::move(c));
			}
			return rows;
		}

		// Written during preflight only, read after it (see RememberPick).
		std::unordered_map<std::string, uintptr_t> g_picks;
	}

	std::string LoaderEntryKey(const ScanPatterns::PreflightEntry& entry)
	{
		return IsGlobalKind(static_cast<int>(entry.kind)) ? std::string(entry.name) : "ModLoader/" + std::string(entry.name);
	}

	uint32_t LoaderEntryFingerprint(const ScanPatterns::PreflightEntry& entry)
	{
		if (IsGlobalKind(static_cast<int>(entry.kind)))
			return 0;
		// Preflight asks with no result offset.
		return Fingerprint(static_cast<int>(entry.kind), entry.pattern, 0, entry.followRel32At, entry.followRipDispAt,
			entry.siteArithmetic);
	}

	ScanValidation::Result ResolveMapKey(const char* owner, const ScanValidation::Request& request,
		const std::vector<Candidate>& own, const Exe& running, bool ownerNameShared)
	{
		using ScanValidation::Outcome;
		const char* who  = owner ? owner : "?";
		const int   kind = static_cast<int>(request.kind);

		if (strcmp(who, "ModLoader") == 0)
		{
			LogPick(who, request.name, "a plugin may not be named ModLoader (refused)");
			return Failure(Outcome::BadRequest, "a plugin may not be named ModLoader: the map's ModLoader/ keys are the loader's own");
		}

		std::string key = request.name;
		uint32_t    fp  = 0;
		if (IsSiteKind(kind))
		{
			if (ownerNameShared)
			{
				LogPick(who, request.name, "another loaded plugin is also named " + std::string(who)
					+ ", so neither reads site keys (refused)");
				return Failure(Outcome::BadRequest, request.name + ": another loaded plugin is also named "
					+ who + ", and the map's " + who + "/ keys cannot tell the two apart, so neither reads them");
			}
			if (strchr(who, '/'))
			{
				LogPick(who, request.name, "a plugin name with a '/' reads no site keys (refused)");
				return Failure(Outcome::BadRequest, request.name + ": the plugin name " + who
					+ " has a '/', so its site keys would run into another plugin's");
			}
			fp = Fingerprint(kind, request.pattern, request.resultOffset, request.followRel32At, -1, 0);
			if (!fp)
			{
				LogPick(who, request.name, "a site key needs its own pattern (refused)");
				return Failure(Outcome::BadRequest, request.name + ": a map key of kind "
					+ ScanValidation::KindName(request.kind) + " is this plugin's own site and needs its own pattern: "
					"the map keeps a site's rows under the fingerprint of the request they were verified with");
			}
			key = std::string(who) + "/" + request.name;
		}
		else if (!IsGlobalKind(kind) || key.find('/') != std::string::npos)
			key.clear();   // no global key has a '/', and an unknown kind reads none

		// Switched off in modloader.ini: no row of the map applies to a plugin, so it
		// takes the same road as a key the map has no row for.
		Lookup found;
		if (PluginMapOff())
			found.setAside = "the plugin map is switched off in modloader.ini";
		else
			found = LookUp(key, kind, fp, who, running.cl);
		if (!found.refusal.empty())
		{
			LogPick(who, request.name, found.refusal + " (refused)");
			return Failure(Outcome::NoMatch, request.name + ": " + found.refusal + kRefusedWhy);
		}
		if (!found.rows.empty())
		{
			std::vector<Candidate> all = found.rows;
			all.insert(all.end(), own.begin(), own.end());
			return Select(owner, request, all, running);
		}

		const std::string why = found.setAside.empty()
			? "not in the loader's map" + (fp ? " (" + key + ", fp " + Hex8(fp) + ")" : std::string())
			: found.setAside;
		if (request.pattern.empty() && own.empty())
		{
			LogPick(who, request.name, why + "; no pattern or rows of its own (refused)");
			return Failure(Outcome::NoMatch, request.name + ": " + why + ", and the request has no pattern or rows of its own");
		}
		LogPick(who, request.name, why + "; own " + (request.pattern.empty() ? "rows" : "pattern"));
		return own.empty() ? ScanValidation::Resolve(request) : Select(owner, request, own, running);
	}

	ScanValidation::Result ResolveLoaderEntry(const ScanPatterns::PreflightEntry& entry,
		const ScanValidation::Request& request, const Exe& running)
	{
		std::string refusal;
		const std::vector<Candidate> rows = LoaderRows(entry, running, refusal);
		if (!refusal.empty())
		{
			LogPick("ModLoader", request.name, refusal + " (refused)");
			return Failure(ScanValidation::Outcome::NoMatch, request.name + ": " + refusal + kRefusedWhy);
		}
		if (rows.empty())
			return ScanValidation::Resolve(request);
		return Select("ModLoader", request, rows, running);
	}

	void RememberPick(const ScanPatterns::PreflightEntry& entry, uintptr_t address)
	{
		if (entry.followRel32At < 0 && entry.followRipDispAt < 0 && !entry.siteArithmetic)
			g_picks[entry.pattern] = address;
	}

	uintptr_t PickFor(const std::string& pattern)
	{
		const auto it = g_picks.find(pattern);
		return it == g_picks.end() ? 0 : it->second;
	}
}

namespace VersionMap
{
	namespace
	{
		LiveFieldSource g_liveFields = nullptr;

		std::string FieldText(const FieldValue& v, bool bitfield)
		{
			std::string text = Hex(static_cast<unsigned long long>(v.offset)) + "/" + Hex(v.size);
			if (bitfield)
				text += " mask " + Hex(v.boolMask);
			return text;
		}

		bool SameField(const FieldValue& a, const FieldValue& b, bool bitfield)
		{
			return a.offset == b.offset && a.size == b.size && (!bitfield || a.boolMask == b.boolMask);
		}

		const FieldCandidate* RowFor(const std::vector<FieldCandidate>& rows, uint32_t cl)
		{
			for (const FieldCandidate& row : rows)
				if (row.gameCL == cl)
					return &row;
			return nullptr;
		}

		// The row for the newest build, whatever the running one is.
		const FieldCandidate* LatestRow(const std::vector<FieldCandidate>& rows)
		{
			const FieldCandidate* latest = nullptr;
			for (const FieldCandidate& row : rows)
				if (!latest || row.gameCL > latest->gameCL)
					latest = &row;
			return latest;
		}

		FieldValue ValueOf(const FieldCandidate& row)
		{
			return FieldValue{ row.offset, row.size, row.boolMask };
		}

		// "loader map, CL-128786 row" or "CL-127004 row" (the component's own).
		std::string RowLabel(const FieldCandidate& row)
		{
			return (row.fromMap ? "loader map, " : "") + ClText(row.gameCL) + " row";
		}

		// The rows a request is decided on: the loader map's under its key, then
		// its own for every changelist the map has no row for. False, with
		// error, when an own row disagrees with the map's for its changelist.
		bool RowsFor(const char* owner, const FieldRequest& request, std::vector<FieldCandidate>& rows, std::string& error)
		{
			rows = request.candidates;
#if defined(MODLOADER_CLIENT_BUILD)
			// Switched off in modloader.ini: a plugin reads its own rows only. The loader's
			// own fields (owner ModLoader) keep the map.
			if (PluginMapOff() && !(owner && strcmp(owner, "ModLoader") == 0))
			{
				LogPick(owner, request.key, "the plugin map is switched off in modloader.ini; own rows");
				return true;
			}
			const MapData& data = ActiveMap();
			if (std::none_of(data.fields.begin(), data.fields.end(),
					[&](const MapFieldRow& f) { return f.key == request.key; }))
				return true;

			const bool bitfield = (request.flags & PLUGIN_FIELD_BITFIELD) != 0;
			const auto text = [bitfield](const FieldCandidate& row) {
				return FieldText(ValueOf(row), bitfield) + (row.member.empty() ? "" : " as " + row.member);
			};
			// The map's key is "Class::Member": its rows are this request's
			// only when they describe the same member. A map row names the
			// member relative to the key, a request's rows relative to the
			// request's member, so the map's are restated the request's way.
			const size_t split = request.key.find("::");
			const std::string keyClass  = request.key.substr(0, split);
			const std::string keyMember = split == std::string::npos ? std::string() : request.key.substr(split + 2);
			std::vector<FieldCandidate> map;
			bool renamed = false;
			for (const MapFieldRow& f : data.fields)
			{
				if (f.key != request.key)
					continue;
				const std::string name = f.member.empty() ? keyMember : f.member;
				renamed = renamed || (!f.member.empty() && name == request.member);
				map.push_back(FieldCandidate{ f.gameCL, f.offset, f.size, f.boolMask,
					name == request.member ? std::string() : name, true });
			}
			if (keyClass != request.className || (keyMember != request.member && !renamed))
			{
				LogPick(owner, request.key, "loader map rows are for " + request.key + ", not "
					+ request.className + (request.member.empty() ? "" : "::" + request.member) + "; own rows");
				return true;
			}

			for (const FieldCandidate& own : request.candidates)
			{
				const auto same = std::find_if(map.begin(), map.end(),
					[&](const FieldCandidate& m) { return m.fromMap && m.gameCL == own.gameCL; });
				if (same == map.end())
				{
					map.push_back(own);
					continue;
				}
				if (!SameField(ValueOf(*same), ValueOf(own), bitfield) || same->member != own.member)
				{
					const std::string disagreement = "the loader map's " + ClText(own.gameCL) + " row says " + text(*same)
						+ ", the plugin's own says " + text(own);
					// A native member has no live source to tell them apart, so
					// neither is used. A reflected one is read from the game
					// anyway, and the rows only cross-check it.
					if ((request.flags & PLUGIN_FIELD_NATIVE) != 0)
					{
						error = disagreement;
						return false;
					}
					ModLoaderLogger::LogWarn(L"[Map] %S: %S: %S; the game's reflection decides",
						owner ? owner : "?", request.key.c_str(), disagreement.c_str());
				}
			}
			rows = std::move(map);
#else
			(void)owner; (void)error;
#endif
			return true;
		}
	}

	void SetLiveFieldSource(LiveFieldSource source)
	{
		g_liveFields = source;
	}

	namespace
	{
		// A class's supers are not named in the request: the game's reflection
		// says which they are.
		bool SupersChanged(const std::string& className)
		{
			return g_liveFields && LayoutChangedWithSupers(g_liveFields(className, std::string()).paths);
		}

		bool PickField(const char* owner, const FieldRequest& request, const Exe& running,
			FieldValue& out, std::string& error)
		{
			const bool native   = (request.flags & PLUGIN_FIELD_NATIVE) != 0;
			const bool check    = (request.flags & PLUGIN_FIELD_CHECK) != 0;
			const bool bitfield = (request.flags & PLUGIN_FIELD_BITFIELD) != 0;
			const bool sizeRow  = request.member.empty();
			const bool anySize  = (request.flags & PLUGIN_FIELD_ANY_SIZE) != 0;

			if (request.className.empty() || (sizeRow && (native || bitfield)))
			{
				error = "bad request: a field needs a class, and a size row is neither native nor a bitfield";
				return false;
			}
			if (anySize && (sizeRow || native || check || bitfield || request.size != 0))
			{
				error = "bad request: PLUGIN_FIELD_ANY_SIZE is for a reflected member, with size 0";
				return false;
			}
			for (size_t i = 0; i < request.candidates.size(); ++i)
				for (size_t j = i + 1; j < request.candidates.size(); ++j)
					if (request.candidates[i].gameCL == request.candidates[j].gameCL)
					{
						error = "bad request: two rows for " + ClText(request.candidates[i].gameCL);
						return false;
					}

			std::vector<FieldCandidate> rows;
			if (!RowsFor(owner, request, rows, error))
				return false;
			const FieldCandidate* row = running.cl ? RowFor(rows, running.cl) : nullptr;
			const bool onBuilt = running.cl != 0 && running.cl == request.builtCL;

			// The compiled value: offsetof and sizeof, or for a bitfield (which has
			// no offsetof) its builtCL row.
			FieldValue built{ request.builtOffset, request.size, 0 };
			bool haveBuilt = !bitfield && !sizeRow;
			if (bitfield)
			{
				if (const FieldCandidate* builtRow = RowFor(rows, request.builtCL))
				{
					built     = ValueOf(*builtRow);
					haveBuilt = true;
				}
			}

			FieldValue value;
			std::string how;
			if (native)
			{
				if (row)
				{
					value = ValueOf(*row);
					how   = RowLabel(*row);
				}
				else if (onBuilt && haveBuilt && (LayoutChanged(request.className) || SupersChanged(request.className)))
				{
					// The compiled offset is the SDK's, and the layout check
					// found this class, or one it derives from, laid out
					// otherwise on this build.
					error = "no row for " + ClText(running.cl) + ", and the layout check found " + request.className
						+ " or a class it derives from changed on this build, so its built offset is not used";
					return false;
				}
				else if (onBuilt && haveBuilt)
				{
					value = built;
					how   = "built offset";
				}
				else
				{
					error = "no row for " + ClText(running.cl) + ", and a native member has no other source";
					// A row for an older build says where the member sat then.
					// Nothing on this build can confirm it still does, so it
					// is not used: the feature using it turns off until CI has
					// verified a row for this build.
					if (const FieldCandidate* carried = LatestRow(rows))
						error += "; the " + RowLabel(*carried) + " cannot be checked on this build, so it is not used";
					return false;
				}
				if (value.size != request.size)
				{
					error = how + " gives size " + Hex(value.size) + ", the code was compiled for " + Hex(request.size);
					return false;
				}
			}
			else
			{
				if (!g_liveFields)
				{
					error = "no reflection data to read a reflected field from";
					return false;
				}
				// The row to check the game against: this build's, else the newest.
				const FieldCandidate* checked = row ? row : LatestRow(rows);
				std::string member = checked && !checked->member.empty() ? checked->member : request.member;
				LiveField live = g_liveFields(request.className, member);
				if (!live.error.empty() && member != request.member)
				{
					// The row's new name is not in this build: ask for it by its own.
					member = request.member;
					live   = g_liveFields(request.className, member);
				}
				if (!live.error.empty())
				{
					error = "reflection: " + live.error;
					return false;
				}
				if (bitfield && !live.isBool)
				{
					error = "asked for as a bitfield, but the game's member is not a bool";
					return false;
				}
				if (!bitfield && !sizeRow && live.isBool && live.boolMask != 0xFF)
				{
					error = "the game's member is a bitfield bool; ask for it with PLUGIN_FIELD_BITFIELD";
					return false;
				}
				value = FieldValue{ sizeRow ? 0 : live.offset, live.size, bitfield ? live.boolMask : uint8_t(0) };
				// PLUGIN_FIELD_ANY_SIZE takes whatever size the game reflects and
				// gets it back in the result; any other request must match.
				if (!sizeRow && !anySize && value.size != request.size)
				{
					error = "the game's member is " + Hex(value.size) + " bytes, the code was compiled for " + Hex(request.size);
					return false;
				}
				// The game is the source. A row is a cross-check, and where it
				// disagrees the game wins and the disagreement is logged.
				if (checked)
				{
					const bool agrees = SameField(ValueOf(*checked), value, bitfield) || (sizeRow && checked->size == value.size);
					if (!agrees)
						ModLoaderLogger::LogWarn(L"[Map] %S: %S: the %S says %S, the game says %S; the game wins",
							owner ? owner : "?", request.key.c_str(), RowLabel(*checked).c_str(),
							FieldText(ValueOf(*checked), bitfield).c_str(), FieldText(value, bitfield).c_str());
					how = row ? RowLabel(*row) + (agrees ? ", as reflected" : ", overruled by reflection")
					          : "reflected (no row for " + ClText(running.cl) + "; the " + RowLabel(*checked)
					            + (agrees ? " agrees)" : " does not)");
				}
				else
				{
					how = "reflected (no row for " + ClText(running.cl) + ")";
				}
			}

			if (value.offset < 0)
			{
				error = how + " gives a negative offset";
				return false;
			}

#if defined(MODLOADER_CLIENT_BUILD)
			// Reflection does not describe a native member, but it does describe
			// its class: the member has to lie inside the class as this build
			// lays it out, or the offset is not this member's.
			if (native)
			{
				if (!g_liveFields)
				{
					error = "no reflection data to read the class's size from, so a native member cannot be bounded";
					return false;
				}
				const LiveField cls = g_liveFields(request.className, std::string());
				if (!cls.error.empty())
				{
					error = "reflection, for the size of " + request.className + ": " + cls.error;
					return false;
				}
				if (static_cast<uint64_t>(value.offset) + value.size > cls.size)
				{
					error = how + " gives " + FieldText(value, bitfield) + ", past the end of " + request.className
						+ " (" + Hex(cls.size) + " bytes on this build)";
					return false;
				}
			}
#endif

			if (check)
			{
				const bool same = sizeRow ? value.size == request.size : haveBuilt && SameField(built, value, bitfield);
				if (!same)
				{
					error = !sizeRow && !haveBuilt
						? std::string("a checked bitfield needs its ") + ClText(request.builtCL) + " row"
						: "the code relies on " + (sizeRow ? Hex(request.size) : FieldText(built, bitfield))
							+ ", this build has " + (sizeRow ? Hex(value.size) : FieldText(value, bitfield));
					return false;
				}
				how += ", as compiled";
			}

			LogPick(owner, request.key, how + " (" + (sizeRow ? Hex(value.size) : FieldText(value, bitfield)) + ")");
			out = value;
			return true;
		}
	}

	bool SelectField(const char* owner, const FieldRequest& request, const Exe& running,
		FieldValue& out, std::string& error)
	{
		if (PickField(owner, request, running, out, error))
			return true;
		ModLoaderLogger::LogWarn(L"[Map] %S: %S: refused: %S", owner ? owner : "?", request.key.c_str(), error.c_str());
		return false;
	}

	FieldRequest LoaderFieldRequest(const char* key, uint32_t size, int32_t builtOffset)
	{
		FieldRequest request;
		request.key         = key;
		const size_t split  = request.key.find("::");
		request.className   = request.key.substr(0, split);
		request.member      = split == std::string::npos ? std::string() : request.key.substr(split + 2);
		request.size        = size;
		request.builtCL     = kBuiltCL;
		request.builtOffset = builtOffset;
		return request;
	}
}
