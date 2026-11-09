// VersionMapTests: the loader's map keys (memory_scanner/version_map.cpp)
// against a fixture map (fixture/hooks/game/version_map.generated.h), and the
// loader's real map against its registry (hooks/game/scan_patterns.h).
#include "test_doubles.h"

#include "hooks/game/scan_patterns.h"
#include "memory_scanner/hook_scanner_interface.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/version_map.h"
#include "plugins/plugin_hook_report.h"
#include "plugins/plugin_interface.h"
#include "plugins/reload_policy.h"
#include "memory_scanner/scan_cache.h"
#include "plugins/crash_restart.h"
#include "plugins/remote_reload.h"
#include "UI/update_notice_window.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>

// The real map, beside the fixture one version_map.cpp reads (its includes are already in).

namespace Real
{
#include "../../StarRupture-ModLoader-Core/hooks/game/version_map.generated.h"
}

using ScanValidation::Kind;
using ScanValidation::Outcome;
using ScanValidation::Request;
using ScanValidation::Result;
using VersionMap::Candidate;

namespace
{
	int g_checks = 0;
	int g_failed = 0;

	void Check(bool ok, const char* what)
	{
		++g_checks;
		if (!ok)
		{
			++g_failed;
			printf("FAIL: %s\n", what);
		}
	}

	uintptr_t Base() { return Scanner::GetImageInfo(GetModuleHandleW(nullptr)).base; }

	const VersionMap::Exe kRunning{ 128786, 0x22222222, 0x02000000 };

	bool Logged(const wchar_t* text)
	{
		for (const std::wstring& line : TestDoubles::g_log)
			if (line.find(text) != std::wstring::npos)
				return true;
		return false;
	}

	Request Ask(const char* name, Kind kind, const char* pattern = "")
	{
		Request r;
		r.name    = name;
		r.kind    = kind;
		r.pattern = pattern;
		return r;
	}

	Result Key(const char* owner, const Request& request, const std::vector<Candidate>& own = {}, bool nameShared = false)
	{
		TestDoubles::g_log.clear();
		return VersionMap::ResolveMapKey(owner, request, own, kRunning, nameShared);
	}

	Candidate AddressRow(uint32_t rva)
	{
		Candidate c;
		c.gameCL     = kRunning.cl;
		c.imageStamp = kRunning.imageStamp;
		c.imageSize  = kRunning.imageSize;
		c.rva        = rva;
		return c;
	}

	bool At(const Result& r, uintptr_t rva) { return r.Succeeded() && r.address == Base() + rva; }

	// Reflection as the field tests see it: ATest is 0x50 bytes, AOther 0x10,
	// and AChild derives from ATest.
	VersionMap::LiveField FakeLive(const std::string& className, const std::string& member)
	{
		VersionMap::LiveField f;
		if (className == "AChild" && member.empty())
			f.size = 0x58, f.paths = { "Mod.Child", "Mod.Test" };
		else if (className == "AOther" && member.empty())
			f.size = 0x10, f.paths = { "Mod.Other" };
		else if (className == "AUnready")
			f.error = "the game's reflection data is not ready";
		else if (className != "ATest")
		{
			f.error = "no class " + className;
			f.unreflected = true;
		}
		else if (member.empty())
			f.size = 0x50;
		else if (member == "Reflected")
			f.offset = 0x30, f.size = 8;
		else if (member == "Renamed")
			f.offset = 0x40, f.size = 4;
		else if (member == "NewName")
			f.offset = 0x44, f.size = 4;
		else
			f.error = "no member " + member;
		return f;
	}

	bool Field(const char* key, const char* className, const char* member, uint32_t flags, uint32_t size,
		const std::vector<VersionMap::FieldCandidate>& own, VersionMap::FieldValue& out, std::string& error,
		const VersionMap::Exe& running = kRunning)
	{
		TestDoubles::g_log.clear();
		error.clear();
		VersionMap::FieldRequest request;
		request.key         = key;
		request.className   = className;
		request.member      = member;
		request.flags       = flags;
		request.size        = size;
		request.builtCL     = 127004;
		request.builtOffset = 0;
		request.candidates  = own;
		return VersionMap::SelectField("Test", request, running, out, error);
	}

	VersionMap::FieldCandidate FieldRow(uint32_t cl, int32_t offset, uint32_t size)
	{
		VersionMap::FieldCandidate c;
		c.gameCL = cl;
		c.offset = offset;
		c.size   = size;
		return c;
	}
}

int main()
{
	TestDoubles::g_rvas = {
		{ "AA 01", 0x1000 }, { "AB 01", 0x2100 }, { "AC 05", 0x3005 }, { "AD 01 ?? 02", 0x4000 },
		{ "AD 01 ?? 03", 0x4200 }, { "AE 09", 0x5000 }, { "AF 01", 0x6100 }, { "AF 09", 0x6000 }, { "B0 01", 0x7000 },
		{ "B1 00", 0x8000 }, { "C0 01", 0x9000 }, { "C0 02", 0x9100 }, { "C0 03", 0x9200 },
	};

	// Fingerprint test vectors: a plugin request of kind 2 with resultOffset -4
	// and followRel32At 3, without and with FOLLOW_REL32.
	Check(VersionMap::Fingerprint(2, "48 8b 05 ? ??  ?? ??", -4, -1, -1, 0) == 0xB3405899, "fp vector, no rel32 follow");
	Check(VersionMap::Fingerprint(2, "48 8b 05 ? ??  ?? ??", -4, 3, -1, 0) == 0x2D1AC6AE, "fp vector, rel32 follow");
	Check(VersionMap::Fingerprint(2, "48 8B 05 ?? ?? ?? ??", -4, -1, -1, 0) == 0xB3405899, "fp reads the pattern as ParsePattern does");
	Check(VersionMap::Fingerprint(2, "", 0, -1, -1, 0) == 0, "fp of no pattern is 0");
	Check(VersionMap::Fingerprint(2, "48 zz", 0, -1, -1, 0) == 0, "fp of a pattern that does not parse is 0");
	Check(VersionMap::Fingerprint(2, "B0 01", 0, -1, -1, 1) == 0x0BD817FA, "fp carries the arithmetic revision");
	Check(VersionMap::Fingerprint(2, "B0 01", 0, -1, -1, 0) != 0x0BD817FA, "fp changes with the revision");

	// The scan cache keeps one INI section per exe: the ProductVersion plus the PE
	// timestamp and image size, so a rebuilt exe with the same ProductVersion misses.
	{
		const std::wstring v = L"1.0.0-CL-127004";
		const std::wstring a = ScanCache::SectionName(v, 0x65A1B2C3, 0x3F40000);
		Check(a == L"1.0.0-CL-127004|65A1B2C3|3F40000", "scan cache section: version, PE timestamp, image size");
		Check(a != v, "scan cache section: a bare ProductVersion section from an older file does not match");
		Check(a != ScanCache::SectionName(v, 0x65A1B2C4, 0x3F40000), "scan cache section: another timestamp is another section");
		Check(a != ScanCache::SectionName(v, 0x65A1B2C3, 0x3F40001), "scan cache section: another image size is another section");
		Check(a == ScanCache::SectionName(v, 0x65A1B2C3, 0x3F40000), "scan cache section: the same exe is the same section");
	}

	// A global key only the map holds; its row for the running build wins.
	Result r = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart));
	Check(At(r, 0x1000) && Logged(L"loader map, CL-128786 row"), "global key: the map's row for this build");

	// The map's row for this build comes before the plugin's own row for it.
	Candidate ownRow = AddressRow(0);
	ownRow.imageStamp = 0x33333333;   // another exe of this build
	ownRow.pattern    = "AB 01";
	r = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart, "AB 01"), { ownRow });
	Check(At(r, 0x1000) && Logged(L"loader map, CL-128786 row") && !Logged(L"own CL-128786 row"),
		"the map's row first: the own row for this build is not tried");

	// The plugin's own pattern is checked even when the map's row decides. It must
	// match exactly once: 0 or 2+ matches is a warning that names the plugin, the
	// key and the count, and the row is still used.
	{
		TestDoubles::g_counts["D1 01"] = 3;
		TestDoubles::g_rvas["D3 01"] = 0xA000;
		r = Key("OwnCheck", Ask("Test::MapOnly", Kind::FunctionStart, "D1 01"));
		Check(At(r, 0x1000) && Logged(L"[Map] OwnCheck: Test::MapOnly: its own pattern matches 3 times"),
			"own pattern: 3 matches warns with the plugin, the key and the count, and the row is still used");
		r = Key("OwnCheck", Ask("Test::MapOnly", Kind::FunctionStart, "D2 01"));
		Check(At(r, 0x1000) && Logged(L"its own pattern matches 0 times"), "own pattern: 0 matches warns, and the row is still used");
		r = Key("OwnCheck", Ask("Test::MapOnly", Kind::FunctionStart, "D3 01"));
		Check(At(r, 0x1000) && !Logged(L"its own pattern matches"), "own pattern: a unique pattern does not warn");
		r = Key("OwnCheck", Ask("Test::MapOnly", Kind::FunctionStart));
		Check(At(r, 0x1000) && !Logged(L"its own pattern matches"), "own pattern: a request without one has nothing to check");

		// Once per pattern: a second request for the same pattern neither scans nor warns again.
		const int calls = TestDoubles::g_resolveCalls["D1 01"];
		r = Key("OwnCheck", Ask("Test::MapOnly", Kind::FunctionStart, "D1 01"));
		Check(At(r, 0x1000) && !Logged(L"its own pattern matches") && TestDoubles::g_resolveCalls["D1 01"] == calls,
			"own pattern: scanned and reported once per pattern");

		// The loader's own registry rows are not plugin patterns.
		TestDoubles::g_counts["D5 01"] = 2;
		Candidate loaderRow;
		loaderRow.gameCL  = kRunning.cl;
		loaderRow.fromMap = true;
		loaderRow.pattern = "AA 01";
		TestDoubles::g_log.clear();
		r = VersionMap::Select("ModLoader", Ask("Test::MapOnly", Kind::FunctionStart, "D5 01"), { loaderRow }, kRunning);
		Check(!Logged(L"its own pattern matches") && TestDoubles::g_resolveCalls["D5 01"] == 0,
			"own pattern: the loader's own rows are not checked");
	}

	// The plugin's own address-only row for this exe: agrees, or AMBIGUOUS.
	r = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart), { AddressRow(0x1000) });
	Check(At(r, 0x1000) && Logged(L"the own CL-128786 row agrees"), "own row for this exe agrees with the map");
	r = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart), { AddressRow(0x1010) });
	Check(r.outcome == Outcome::MultipleMatches && r.detail.find("AMBIGUOUS") == 0 && Logged(L"AMBIGUOUS"),
		"own row for this exe disagrees: AMBIGUOUS");

	// A global absent row binds the owners it lists, and nobody else.
	r = Key("Listed", Ask("Test::Absent", Kind::FunctionStart, "AB 01"));
	Check(r.outcome == Outcome::NoMatch && Logged(L"loader map: absent on CL-128786 (refused)"), "absent binds a listed owner, no fallback");
	r = Key("Other", Ask("Test::Absent", Kind::FunctionStart, "AB 01"));
	Check(At(r, 0x2100) && Logged(L"for other requests; own pattern"), "absent for another owner: own pattern");

	// Another kind under the key: the map is set aside.
	r = Key("VmapProbe", Ask("Test::Data", Kind::FunctionStart, "AC 05"));
	Check(At(r, 0x3005) && Logged(L"loader map row is for another kind (data); own pattern"), "kind mismatch skips the map");
	r = Key("VmapProbe", Ask("Test::Data", Kind::FunctionStart));
	Check(r.outcome == Outcome::NoMatch && r.detail.find("another kind") != std::string::npos
		&& Logged(L"another kind (data); no pattern or rows of its own (refused)"),
		"kind mismatch on a key-only request: refused");

	// Sites: the owner's key, matched by fingerprint.
	r = Key("VmapProbe", Ask("Site", Kind::InFunction, "AD 01 ?? 02"));
	Check(At(r, 0x4000) && Logged(L"loader map, CL-128786 row"), "site: the row with the request's fp");
	r = Key("VmapProbe", Ask("Site", Kind::InFunction, "AD 01 ?? 03"));
	Check(At(r, 0x4200) && Logged(L"loader map row is for another request (fp "), "site: another fp is set aside");
	r = Key("VmapProbe", Ask("Site", Kind::InFunction));
	Check(r.outcome == Outcome::BadRequest && Logged(L"(refused)"), "site key with no pattern is refused");
	r = Key("OtherPlugin", Ask("Site", Kind::InFunction, "AD 01 ?? 03"));
	Check(At(r, 0x4200) && Logged(L"not in the loader's map (OtherPlugin/Site, fp "), "another owner's site is not this plugin's key");
	r = Key("VmapProbe", Ask("AbsentSite", Kind::InFunction, "AF 01"));
	Check(r.outcome == Outcome::NoMatch && Logged(L"(refused)"), "site absent with this fp: refused, though the own pattern matches");
	r = Key("VmapProbe", Ask("AbsentSite", Kind::InFunction, "AF 09"));
	Check(At(r, 0x6000), "site absent under another fp: this fp's row");
	r = Key("VmapProbe", Ask("OtherFp", Kind::InFunction, "AE 09"));
	Check(At(r, 0x5000) && Logged(L"no row for CL-128786; own pattern, loader map, CL-127004 row agree"), "no row for this build: agreement");
	// No row for this build, a row for another build with another fp whose
	// pattern resolves here, and an own pattern that does not match: a failure,
	// never the other request's address.
	r = Key("VmapProbe", Ask("OtherFp", Kind::InFunction, "AE 01"));
	Check(!r.Succeeded() && r.address != Base() + 0x5000 && Logged(L"loader map row is for another request (fp "),
		"site: another fp's row is not used, the own pattern decides");

	// Keys the map does not hold, and names it refuses.
	r = Key("VmapProbe", Ask("Test::Nope", Kind::FunctionStart, "B1 00"));
	Check(At(r, 0x8000) && Logged(L"not in the loader's map; own pattern"), "unknown global key: own pattern");
	r = Key("VmapProbe", Ask("Test::Nope", Kind::FunctionStart));
	Check(r.outcome == Outcome::NoMatch && r.detail.find("no pattern or rows of its own") != std::string::npos
		&& Logged(L"not in the loader's map; no pattern or rows of its own (refused)"), "unknown key, nothing of its own");
	r = Key("VmapProbe", Ask("ModLoader/Test::Site", Kind::FunctionStart, "B1 00"));
	Check(At(r, 0x8000) && Logged(L"not in the loader's map; own pattern"), "a global key with a '/' reads no key");
	r = Key("ModLoader", Ask("Test::MapOnly", Kind::FunctionStart));
	Check(r.outcome == Outcome::BadRequest && Logged(L"(refused)"), "a plugin named ModLoader is refused");
	r = Key("Vmap/Probe", Ask("Site", Kind::InFunction, "AD 01 ?? 02"));
	Check(r.outcome == Outcome::BadRequest && Logged(L"a plugin name with a '/' reads no site keys (refused)"),
		"a plugin name with a '/' reads no site keys");
	r = Key("Vmap/Probe", Ask("Test::MapOnly", Kind::FunctionStart));
	Check(At(r, 0x1000), "a plugin name with a '/' still reads global keys");

	// Two loaded plugins with one name: neither reads site keys, with one line
	// saying why; global keys are unaffected.
	r = Key("VmapProbe", Ask("Site", Kind::InFunction, "AD 01 ?? 02"), {}, true);
	Check(r.outcome == Outcome::BadRequest && TestDoubles::g_log.size() == 1
		&& Logged(L"[Map] VmapProbe: Site: another loaded plugin is also named VmapProbe, so neither reads site keys (refused)"),
		"shared name: a site key is refused");
	r = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart), {}, true);
	Check(At(r, 0x1000) && Logged(L"loader map, CL-128786 row"), "shared name: a global key is unaffected");

	// The loader's own site: its key, fingerprint and arithmetic filter.
	ScanPatterns::PreflightEntry site{};
	site.name           = "Test::Site";
	site.pattern        = "B0 01";
	site.kind           = ScanPatterns::PatternKind::InFunction;
	site.siteArithmetic = 1;
	Check(VersionMap::LoaderEntryKey(site) == "ModLoader/Test::Site", "loader site key");
	Check(VersionMap::LoaderEntryFingerprint(site) == 0x0BD817FA, "loader site fp");
	TestDoubles::g_log.clear();
	r = VersionMap::ResolveLoaderEntry(site, Ask("Test::Site", Kind::InFunction, "B0 01"), kRunning);
	Check(At(r, 0x7000) && Logged(L"is not this site's own pattern") && Logged(L"own pattern, loader map, CL-127004 row agree"),
		"loader site: a new pattern is not used, the built row stands in");

	// The real map against the registry: every key is a registry row's, with its kind and fingerprint.
	int unmatched = 0, wrong = 0;
	for (const Real::VersionMap::Index& index : Real::VersionMap::kFunctions)
	{
		const ScanPatterns::PreflightEntry* entry = nullptr;
		for (const ScanPatterns::PreflightEntry& e : ScanPatterns::PreflightRegistry)
			if (VersionMap::LoaderEntryKey(e) == index.key)
				entry = &e;
		if (!entry)
		{
			// A plugin's own function key: a global key (a global kind, no fingerprint), which any plugin may
			// ask for by name; or a scoped key, "<Plugin>/<name>", whose rows each carry the fingerprint of
			// the one request they were verified with.
			const bool scoped = strchr(index.key, '/') != nullptr;
			for (uint16_t i = index.first; i < index.first + index.count; ++i)
			{
				const Real::VersionMap::FunctionRow& row = Real::VersionMap::kFunctionRows[i];
				const bool globalKind = row.kind == static_cast<int>(Kind::FunctionStart) || row.kind == static_cast<int>(Kind::Data);
				if (scoped ? (globalKind || row.fp == 0) : (!globalKind || row.fp != 0))
				{
					++unmatched;
					printf("  not a registry key, and not a plugin's global or scoped key: %s\n", index.key);
					break;
				}
			}
			continue;
		}
		for (uint16_t i = index.first; i < index.first + index.count; ++i)
		{
			const Real::VersionMap::FunctionRow& row = Real::VersionMap::kFunctionRows[i];
			if (row.kind != static_cast<int>(entry->kind) || row.fp != VersionMap::LoaderEntryFingerprint(*entry))
			{
				++wrong;
				printf("  wrong kind or fp: %s\n", index.key);
			}
		}
	}
	// The order for a function: the row for this build, the latest build's row rescanned here, the
	// request's own pattern, then nothing. Older rows are not read.
	{
		using VersionMap::MapFunctionRow;
		const auto row = [](const char* key, uint32_t cl, const char* pattern, bool absent = false, const char* owners = "")
		{
			MapFunctionRow f;
			f.key = key; f.gameCL = cl; f.imageStamp = 0x10000000 + cl; f.imageSize = 0x1000000; f.kind = 1;
			f.absent = absent; f.pattern = pattern ? pattern : ""; f.owners = owners;
			return f;
		};
		VersionMap::MapData chain;
		chain.builds = { { 120000, 0x10000000 + 120000, 0x1000000, "x" }, { 127004, 0x10000000 + 127004, 0x1000000, "x" },
			{ 128786, 0x22222222, 0x02000000, "x" } };
		chain.functions = {
			row("Test::Chain", 120000, "C0 01"),     // old: resolves to 0x9000, must not be read
			row("Test::Chain", 127004, "C0 02"),     // the latest: resolves to 0x9100
			row("Test::Missed", 120000, "C0 01"),
			row("Test::Missed", 127004, "ZZ 99"),    // the latest: not found on this build
			row("Test::NewerAbsent", 127004, "C0 02"),
			row("Test::NewerAbsent", 130000, nullptr, true, "VmapProbe"),   // CI could not place it on a newer build
			row("Test::Exact", 127004, "C0 01"),
			row("Test::Exact", 128786, "C0 02"),
		};
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(chain));

		// 1. The row for this build, with nothing else read.
		Result q = Key("VmapProbe", Ask("Test::Exact", Kind::FunctionStart));
		Check(At(q, 0x9100) && Logged(L"loader map, CL-128786 row"), "order: the row for this build");

		// 2. No row for this build: the latest build's row, rescanned here. The older row is not read, or the two would disagree.
		q = Key("VmapProbe", Ask("Test::Chain", Kind::FunctionStart));
		Check(At(q, 0x9100) && Logged(L"no row for CL-128786; loader map, CL-127004 row agree") && !Logged(L"CL-120000"),
			"order: the latest build's row, verified on this build; older rows are not read");

		// 3. The latest row does not resolve here: the request's own pattern.
		q = Key("VmapProbe", Ask("Test::Missed", Kind::FunctionStart, "C0 03"));
		Check(At(q, 0x9200) && Logged(L"no row for CL-128786; own pattern resolves"),
			"order: the latest row is not found here, so the request's own pattern");

		// 3b. No row for this build and an own pattern that matches twice: the latest
		// row decides, and the own pattern is reported.
		TestDoubles::g_counts["D4 01"] = 2;
		q = Key("OwnCheck", Ask("Test::Chain", Kind::FunctionStart, "D4 01"));
		Check(At(q, 0x9100) && Logged(L"[Map] OwnCheck: Test::Chain: its own pattern matches 2 times"),
			"own pattern: no row for this build, the latest row decides and the ambiguous own pattern is reported");
		Check(TestDoubles::g_resolveCalls["D4 01"] == 1, "own pattern: the agreement path reuses its own resolve for the check");

		// 4. Nothing resolves: a miss, which the caller turns into a feature off or a refusal.
		q = Key("VmapProbe", Ask("Test::Missed", Kind::FunctionStart));
		Check(q.outcome == Outcome::NoMatch && q.address == 0 && q.detail.find("no locator resolves") != std::string::npos,
			"order: nothing resolves is a miss, never an address");
		q = Key("VmapProbe", Ask("Test::Missed", Kind::FunctionStart, "ZZ 98"));
		Check(q.outcome == Outcome::NoMatch && q.address == 0, "order: no locator resolves with an own pattern that misses too");

		// A newer build the key is absent on: the older latest row is not known to hold.
		q = Key("VmapProbe", Ask("Test::NewerAbsent", Kind::FunctionStart));
		Check(!q.Succeeded() && q.address == 0, "order: absent on a newer build, the latest row is not used");
		q = Key("Other", Ask("Test::NewerAbsent", Kind::FunctionStart));
		Check(At(q, 0x9100), "order: absent on a newer build for another owner leaves the latest row");

		// Locators that resolve to different addresses are no address at all.
		q = Key("VmapProbe", Ask("Test::Chain", Kind::FunctionStart, "C0 03"));
		Check(q.outcome == Outcome::MultipleMatches && q.address == 0 && q.detail.find("AMBIGUOUS") == 0,
			"order: the latest row and the own pattern disagree: a miss");
		VersionMap::SetActiveMap(nullptr);
	}

	// Features: a miss on a declared feature's key turns off that feature; a miss anywhere else refuses the plugin.
	{
		VersionMap::SetLiveFieldSource(FakeLive);
		IPluginHookScanner* scan = ModLoaderLogger::GetPluginHookScanner();
		const auto resolve = [&](const IPluginSelf* self, const char* name, const char* pattern)
		{
			PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
			request.hookName = name;
			request.pattern  = pattern;
			request.kind     = PLUGIN_SCAN_ANY;
			return scan->Resolve(self, &request);
		};
		const auto field = [&](const IPluginSelf* self, const char* key, const char* member)
		{
			PluginFieldRequest request = PLUGIN_FIELD_REQUEST_INIT;
			request.key       = key;
			request.className = "ATest";
			request.member    = member;
			request.size      = 8;
			request.builtCL   = 127004;
			PluginFieldResult result{};
			return scan->ResolveField(self, &request, &result);
		};
		const auto declare = [&](const IPluginSelf* self, const char* id, const char* title,
			std::vector<const char*> keys, std::vector<const char*> configKeys, bool* enabled)
		{
			PluginFeatureDesc desc = PLUGIN_FEATURE_DESC_INIT;
			desc.id             = id;
			desc.title          = title;
			desc.keys           = keys.data();
			desc.keyCount       = static_cast<uint32_t>(keys.size());
			desc.configKeys     = configKeys.data();
			desc.configKeyCount = static_cast<uint32_t>(configKeys.size());
			desc.enabled        = enabled;
			return scan->DeclareFeature(self, &desc);
		};
		const auto find = [](const char* plugin) -> const PluginHookReport::PluginReport*
		{
			static std::vector<PluginHookReport::PluginReport> snapshot;
			snapshot = PluginHookReport::Snapshot();
			for (const PluginHookReport::PluginReport& r : snapshot)
				if (r.plugin == plugin)
					return &r;
			return nullptr;
		};

		IPluginSelf self{ "FeatPlug", "1", nullptr, nullptr, nullptr };
		bool laser = false, hud = false, refusedFlag = true;

		// Outside a session nothing is declared.
		Check(!declare(&self, "x", "X", { "K" }, {}, nullptr), "feature: no session, no declaration");

		const unsigned serial = PluginHookReport::GetCommitSerial();
		PluginHookReport::BeginSession(&self, "FeatPlug", "FeatPlug.dll");
		Check(declare(&self, "laser", "Mining laser", { "Test::Gone", "ATest::Missing" }, { "Laser.Scale", "Beam.*" }, &laser)
			&& laser, "feature: declared, flag set");
		Check(declare(&self, "hud", "Heads-up display", { "Test::Gone" }, { "Hud.Show" }, &hud) && hud,
			"feature: a key two features list");
		Check(!declare(&self, "laser", "Again", { "K" }, {}, nullptr), "feature: an id declared twice is refused");
		Check(!declare(&self, "empty", "No keys", {}, {}, nullptr) && !declare(&self, "", "No id", { "K" }, {}, nullptr)
			&& !declare(&self, "notitle", "", { "K" }, {}, nullptr) && !declare(&self, "emptykey", "Empty key", { "" }, {}, nullptr),
			"feature: a malformed description declares nothing");
		{
			PluginFeatureDesc small = PLUGIN_FEATURE_DESC_INIT;
			small.structSize = 8;
			Check(!scan->DeclareFeature(&self, &small) && !scan->DeclareFeature(&self, nullptr), "feature: a short or null description");
		}

		Check(resolve(&self, "Test::Kept", "AE 09") != 0, "feature: a key that resolves is untouched");
		Check(resolve(&self, "Test::Gone", "ZZ 99") == 0, "feature: the key misses");
		Check(!field(&self, "ATest::Missing", "Missing"), "feature: a field of the feature misses");
		Check(!PluginHookReport::SessionHasFailures(&self), "feature: a feature's miss is not a failure");
		Check(!laser && !hud, "feature: both flags are off");

		PluginHookReport::EndSession(&self, &refusedFlag);
		Check(!refusedFlag, "feature: the plugin is not refused");
		const PluginHookReport::PluginReport* report = find("FeatPlug");
		Check(report && !report->refused && report->failures.empty() && report->featuresOff.size() == 2
			&& report->featuresOff[0].id == "laser" && report->featuresOff[0].misses.size() == 2
			&& report->featuresOff[0].misses[0].key == "Test::Gone" && report->featuresOff[1].id == "hud"
			&& report->featuresOff[1].misses.size() == 1, "feature: the report lists both, with their misses");
		Check(PluginHookReport::GetCommitSerial() == serial + 1 && PluginHookReport::GetRefusedPluginCount() == 0
			&& PluginHookReport::GetFeaturesOffCount("featplug") == 2
			&& PluginHookReport::GetFeaturesOffCount("Other") == 0, "feature: counted, and the popup is told");
		Check(PluginHookReport::ConfigOffReason("FeatPlug", "Laser", "Scale") == "Mining laser is off: this game build changed something it needs"
			&& !PluginHookReport::ConfigOffReason("featplug", "LASER", "scale").empty()
			&& !PluginHookReport::ConfigOffReason("FeatPlug", "beam", "Anything").empty()
			&& !PluginHookReport::ConfigOffReason("FeatPlug", "Hud", "Show").empty()
			&& PluginHookReport::ConfigOffReason("FeatPlug", "Laser", "Other").empty()
			&& PluginHookReport::ConfigOffReason("FeatPlug", "Hud", "Other").empty()
			&& PluginHookReport::ConfigOffReason("Other", "Laser", "Scale").empty(), "feature: the config entries it drives");
		const std::string text = PluginHookReport::BuildReportText();
		Check(text.find("FeatPlug (FeatPlug.dll) -- loaded, 2 feature(s) off") != std::string::npos
			&& text.find("[feature off] Mining laser (laser)") != std::string::npos
			&& text.find("NOT LOADED") == std::string::npos
			&& text.find("turns that feature off") != std::string::npos
			&& text.find("Any unresolved hook stops the plugin loading") == std::string::npos,
			"feature: the report text, and its closing paragraph");
		Check(Logged(L"[Map] FeatPlug: feature \"Mining laser\" is off: Test::Gone, ATest::Missing"), "feature: one log line per feature");

		// A miss no feature lists refuses the plugin; a feature that was fine stays on.
		IPluginSelf core{ "CorePlug", "1", nullptr, nullptr, nullptr };
		bool fine = false;
		refusedFlag = false;
		PluginHookReport::BeginSession(&core, "CorePlug", "CorePlug.dll");
		declare(&core, "fine", "Fine", { "Test::Kept" }, {}, &fine);
		resolve(&core, "Test::Kept", "AE 09");
		Check(resolve(&core, "Test::CoreGone", "ZZ 99") == 0 && PluginHookReport::SessionHasFailures(&core), "core: the miss is a failure");
		PluginHookReport::EndSession(&core, &refusedFlag);
		report = find("CorePlug");
		Check(refusedFlag && fine && report && report->refused && report->failures.size() == 1 && report->featuresOff.empty(),
			"core: the plugin is refused, the other feature is untouched");

		// A core miss beside a feature miss: refused, and both are in the report.
		IPluginSelf both{ "BothPlug", "1", nullptr, nullptr, nullptr };
		bool gone = true;
		refusedFlag = false;
		PluginHookReport::BeginSession(&both, "BothPlug", "BothPlug.dll");
		declare(&both, "g", "Gone", { "Test::Gone" }, {}, &gone);
		resolve(&both, "Test::Gone", "ZZ 99");
		resolve(&both, "Test::CoreGone", "ZZ 99");
		PluginHookReport::EndSession(&both, &refusedFlag);
		report = find("BothPlug");
		Check(refusedFlag && !gone && report && report->refused && report->failures.size() == 1 && report->featuresOff.size() == 1,
			"core: a core miss beside a feature miss still refuses");

		// A key missed before its feature was declared is core.
		IPluginSelf late{ "LatePlug", "1", nullptr, nullptr, nullptr };
		refusedFlag = false;
		PluginHookReport::BeginSession(&late, "LatePlug", "LatePlug.dll");
		resolve(&late, "Test::Gone", "ZZ 99");
		declare(&late, "g", "Gone", { "Test::Gone" }, {}, nullptr);
		PluginHookReport::EndSession(&late, &refusedFlag);
		Check(refusedFlag, "core: a miss before the declaration is core");

		// ReportFailure from the plugin itself is a miss like any other.
		IPluginSelf reported{ "ReportPlug", "1", nullptr, nullptr, nullptr };
		bool viaReport = true;
		refusedFlag = true;
		PluginHookReport::BeginSession(&reported, "ReportPlug", "ReportPlug.dll");
		declare(&reported, "r", "Reported", { "Own::Check" }, {}, &viaReport);
		scan->ReportFailure(&reported, "Own::Check", "vtable slot held something unexpected");
		PluginHookReport::EndSession(&reported, &refusedFlag);
		Check(!refusedFlag && !viaReport, "feature: ReportFailure on a feature's key turns it off");
		report = find("ReportPlug");
		Check(report && report->featuresOff.size() == 1
			&& report->featuresOff[0].reason == "the plugin reported a problem with something it needs",
			"feature: a plugin's own report does not blame the game update");

		// A bad request on a feature's key says so, and a game miss says the build changed.
		IPluginSelf asked{ "AskPlug", "1", nullptr, nullptr, nullptr };
		bool badKey = true, goneKey = true;
		PluginHookReport::BeginSession(&asked, "AskPlug", "AskPlug.dll");
		declare(&asked, "bad", "Bad request", { "Bad::Key" }, {}, &badKey);
		declare(&asked, "gone", "Gone key", { "Test::Gone" }, {}, &goneKey);
		{
			PluginFieldRequest request = PLUGIN_FIELD_REQUEST_INIT;
			request.key = "Bad::Key";   // no className
			PluginFieldResult result{};
			scan->ResolveField(&asked, &request, &result);
		}
		resolve(&asked, "Test::Gone", "ZZ 99");
		PluginHookReport::EndSession(&asked, &refusedFlag);
		report = find("AskPlug");
		Check(!badKey && !goneKey && report && report->featuresOff.size() == 2
			&& report->featuresOff[0].reason.find("asked for something it needs") != std::string::npos
			&& report->featuresOff[1].reason == "this game build changed something it needs",
			"feature: a bad request is the plugin's fault, a missed locator the game's");

		// A feature key no request named is logged at the end of the session, even when everything resolved.
		IPluginSelf typo{ "TypoPlug", "1", nullptr, nullptr, nullptr };
		PluginHookReport::BeginSession(&typo, "TypoPlug", "TypoPlug.dll");
		declare(&typo, "t", "Typed", { "Test::Kept", "Test::Kpet" }, {}, nullptr);
		resolve(&typo, "Test::Kept", "AE 09");
		TestDoubles::g_log.clear();
		PluginHookReport::EndSession(&typo, &refusedFlag);
		Check(!refusedFlag && Logged(L"[Map] TypoPlug: feature \"Typed\" lists \"Test::Kpet\", which no request asked for")
			&& !Logged(L"lists \"Test::Kept\""), "feature: a key that no request named is logged");

		// A preload record and a normal one with one name are counted apart.
		IPluginSelf pre{ "PreDup", "1", nullptr, nullptr, nullptr };
		IPluginSelf normal{ "PreDup", "1", nullptr, nullptr, nullptr };
		PluginHookReport::BeginSession(&pre, "PreDup", "PreDup.dll", true);
		declare(&pre, "p", "Early", { "Test::Gone" }, {}, nullptr);
		resolve(&pre, "Test::Gone", "ZZ 99");
		PluginHookReport::EndSession(&pre, &refusedFlag);
		PluginHookReport::BeginSession(&normal, "PreDup", "PreDup.dll", false);
		declare(&normal, "n", "Late", { "Test::Gone" }, {}, nullptr);
		resolve(&normal, "Test::Gone", "ZZ 99");
		PluginHookReport::EndSession(&normal, &refusedFlag);
		Check(PluginHookReport::GetFeaturesOffCount("PreDup") == 1 && PluginHookReport::GetFeaturesOffCount("PreDup", true) == 1,
			"feature: the count follows the preload flag");

		// A reload that now resolves cleanly drops the old entry.
		bool again = false;
		PluginHookReport::BeginSession(&self, "FeatPlug", "FeatPlug.dll");
		declare(&self, "laser", "Mining laser", { "Test::Kept" }, { "Laser.Scale" }, &again);
		resolve(&self, "Test::Kept", "AE 09");
		PluginHookReport::EndSession(&self, &refusedFlag);
		Check(find("FeatPlug") == nullptr && again && PluginHookReport::ConfigOffReason("FeatPlug", "Laser", "Scale").empty(),
			"feature: a clean reload switches the entries back on");
		VersionMap::SetLiveFieldSource(nullptr);
	}

	// Fields: the map's rows first, the class and member match, the class bound.
	{
		VersionMap::SetLiveFieldSource(FakeLive);
		VersionMap::FieldValue v;
		std::string e;
		Check(Field("ATest::Native", "ATest", "Native", PLUGIN_FIELD_NATIVE, 4, {}, v, e) && v.offset == 0x28
			&& Logged(L"ATest::Native: loader map, CL-128786 row (0x28/0x4)"), "field: a native member only the map holds");
		Check(Field("ATest::Native", "ATest", "Native", PLUGIN_FIELD_NATIVE, 4, { FieldRow(128786, 0x28, 4) }, v, e)
			&& v.offset == 0x28, "field: an own row that agrees with the map's");

		// On the build it was compiled for, a native member with no row takes its
		// compiled offset, unless the layout check found its class changed.
		const VersionMap::Exe built{ 127004, 0, 0 };
		Check(Field("ATest::Plain", "ATest", "Plain", PLUGIN_FIELD_NATIVE, 4, {}, v, e, built) && v.offset == 0,
			"field: a native member's built offset on its own build");
		VersionMap::SetLayoutChanges({ "ATest", "/Script/Mod.ATest" });
		Check(!Field("ATest::Plain", "ATest", "Plain", PLUGIN_FIELD_NATIVE, 4, {}, v, e, built)
			&& e.find("the layout check found ATest or a class it derives from changed") != std::string::npos,
			"field: a changed class's built offset is refused (only the feature asking for it goes off)");
		Check(Field("AOther::Plain", "AOther", "Plain", PLUGIN_FIELD_NATIVE, 4, {}, v, e, built) && v.offset == 0,
			"field: another class's request is unaffected");
		Check(Field("AChild::Plain", "AChild", "Plain", PLUGIN_FIELD_NATIVE, 4, {}, v, e, built) && v.offset == 0,
			"field: a derived class is not refused while only its name is listed, no path");
		VersionMap::SetLayoutChanges({ "ATest", "/Script/Mod.ATest" }, { "Mod.Test" });
		Check(!Field("AChild::Plain", "AChild", "Plain", PLUGIN_FIELD_NATIVE, 4, {}, v, e, built)
			&& e.find("AChild or a class it derives from changed") != std::string::npos,
			"field: a base class's change refuses a derived class's built offset");
		Check(Field("AOther::Plain", "AOther", "Plain", PLUGIN_FIELD_NATIVE, 4, {}, v, e, built) && v.offset == 0,
			"field: a class with another base is still unaffected");
		VersionMap::SetLayoutChanges({ "ATest", "/Script/Mod.ATest" });
		Check(Field("ATest::Reflected", "ATest", "Reflected", 0, 8, {}, v, e, built) && v.offset == 0x30,
			"field: a reflected member of a changed class is read from the game");
		VersionMap::SetLayoutChanges({});
		Check(!Field("ATest::Native", "ATest", "Native", PLUGIN_FIELD_NATIVE, 4, { FieldRow(127004, 0x24, 4) }, v, e)
			&& e == "the loader map's CL-127004 row says 0x20/0x4, the plugin's own says 0x24/0x4"
			&& Logged(L"ATest::Native: refused: the loader map's CL-127004 row says"), "field: an own row that disagrees, for any build, is refused");
		Check(Field("ATest::Old", "ATest", "Old", PLUGIN_FIELD_NATIVE, 4, { FieldRow(128786, 0x18, 4) }, v, e) && v.offset == 0x18
			&& Logged(L"ATest::Old: CL-128786 row (0x18/0x4)"), "field: an own row for a build the map has no row for");
		Check(Field("ATest::Native", "AOther", "Native", PLUGIN_FIELD_NATIVE, 4, { FieldRow(128786, 0x8, 4) }, v, e) && v.offset == 0x8
			&& Logged(L"loader map rows are for ATest::Native, not AOther::Native; own rows"), "field: a key for another class is set aside");
		Check(!Field("ATest::Native", "ATest", "Other", PLUGIN_FIELD_NATIVE, 4, {}, v, e)
			&& Logged(L"not ATest::Other; own rows") && e.find("no row for CL-128786") == 0, "field: a key for another member is set aside");
		Check(Field("ATest::Renamed", "ATest", "NewName", 0, 4, {}, v, e) && v.offset == 0x44
			&& Logged(L"loader map, CL-128786 row, as reflected (0x44/0x4)"), "field: a key whose row records the rename");
		Check(Field("ATest::Renamed", "ATest", "NewName", 0, 4, {}, v, e, VersionMap::Exe{ 127004, 0, 0 }) && v.offset == 0x40,
			"field: a renamed key's row from before the rename reads the key's member");
		// PLUGIN_FIELD_ANY_SIZE takes the size the game reflects and returns it; any other size must match, and 0 is a size.
		Check(Field("ATest::Renamed", "ATest", "Renamed", PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e) && v.offset == 0x44 && v.size == 4,
			"field: the any-size flag takes a 4-byte reflected member and returns its size");
		Check(Field("ATest::Reflected", "ATest", "Reflected", PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e) && v.offset == 0x30 && v.size == 8,
			"field: the any-size flag takes an 8-byte reflected member and returns its size");
		Check(!Field("ATest::Reflected", "ATest", "Reflected", 0, 0, {}, v, e)
			&& e == "the game's member is 0x8 bytes, the code was compiled for 0x0", "field: size 0 without the flag is refused");
		Check(!Field("ATest::Reflected", "ATest", "Reflected", 0, 4, {}, v, e)
			&& e == "the game's member is 0x8 bytes, the code was compiled for 0x4", "field: a non-zero size that does not match is still refused");
		Check(!Field("ATest::Reflected", "ATest", "Reflected", PLUGIN_FIELD_ANY_SIZE, 4, {}, v, e)
			&& e.find("bad request: PLUGIN_FIELD_ANY_SIZE") == 0, "field: the any-size flag with a non-zero size is refused");
		Check(!Field("ATest::Native", "ATest", "Native", PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e)
			&& e.find("bad request: PLUGIN_FIELD_ANY_SIZE") == 0, "field: a native member refuses the any-size flag");
		Check(!Field("ATest::Missing", "ATest", "Missing", PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e) && e == "reflection: no member Missing",
			"field: the any-size flag does not accept a member the game does not have");
		Check(Field("ATest::Renamed", "ATest", "NewName", 0, 4, { FieldRow(128786, 0x44, 4) }, v, e) && v.offset == 0x44,
			"field: an own row under the new name agrees with the map's rename row");
		Check(Field("ATest::Reflected", "ATest", "Reflected", 0, 8, {}, v, e) && v.offset == 0x30
			&& Logged(L"the loader map, CL-128786 row says 0x38/0x8, the game says 0x30/0x8; the game wins")
			&& Logged(L"overruled by reflection"), "field: a map row reflection disagrees with: the game wins, logged");
		// A reflected field's own row against the map's: the game decides too, a native member's neither.
		Check(Field("ATest::Reflected", "ATest", "Reflected", 0, 8, { FieldRow(128786, 0x20, 8) }, v, e) && v.offset == 0x30
			&& Logged(L"the plugin's own says 0x20/0x8; the game's reflection decides"), "field: own row against the map's row, reflected");
		// The latest build's row stands in when the running one has none: it is checked against the game, and the game wins.
		const VersionMap::Exe later{ 130000, 0x33333333, 0x03000000 };
		Check(Field("ATest::Reflected", "ATest", "Reflected", 0, 8, {}, v, e, later) && v.offset == 0x30
			&& Logged(L"the loader map, CL-128786 row says 0x38/0x8, the game says 0x30/0x8; the game wins")
			&& Logged(L"reflected (no row for CL-130000; the loader map, CL-128786 row does not)"),
			"field: no row for this build: the latest row is checked and the game wins");
		Check(Field("ATest::Renamed", "ATest", "Renamed", 0, 4, {}, v, e, later) && v.offset == 0x44
			&& Logged(L"reflected (no row for CL-130000; the loader map, CL-128786 row agrees)"),
			"field: no row for this build: the latest row's new name is asked for first");
		// The latest row's new name is not in this build: the request's own name is asked for.
		Check(Field("ATest::Own", "ATest", "Reflected", 0, 8, { FieldRow(129500, 0x30, 8) }, v, e, later) && v.offset == 0x30
			&& Logged(L"CL-129500 row agrees)"), "field: no row for this build: the plugin's own latest row agrees with the game");
		{
			VersionMap::FieldCandidate gone = FieldRow(129500, 0x30, 8);
			gone.member = "Gone";
			Check(Field("ATest::Own", "ATest", "Reflected", 0, 8, { gone }, v, e, later) && v.offset == 0x30
				&& Logged(L"agrees)"), "field: a row naming a member the game lacks falls back to the request's name");
		}
		// A native member carried forward from an older build cannot be checked, so it is not used.
		Check(!Field("ATest::Native", "ATest", "Native", PLUGIN_FIELD_NATIVE, 4, {}, v, e, later)
			&& e.find("no row for CL-130000") == 0 && e.find("cannot be checked on this build, so it is not used") != std::string::npos,
			"field: a native member carried forward from the latest row degrades");
		Check(!Field("ATest::Past", "ATest", "Past", PLUGIN_FIELD_NATIVE, 8, { FieldRow(128786, 0x4C, 8) }, v, e)
			&& e == "CL-128786 row gives 0x4C/0x8, past the end of ATest (0x50 bytes on this build)", "field: native past the class's end");
		Check(Field("ATest::Past", "ATest", "Past", PLUGIN_FIELD_NATIVE, 8, { FieldRow(128786, 0x48, 8) }, v, e) && v.offset == 0x48,
			"field: native ending at the class's end");
		// A class reflection does not know (a plain C++ type) is bounded by nothing: its map row stands, and it needs one.
		Check(Field("FPlain::Block", "FPlain", "Block", PLUGIN_FIELD_NATIVE, 0xB0, {}, v, e) && v.offset == 0x120 && v.size == 0xB0
			&& Logged(L"FPlain::Block: loader map, CL-128786 row, unreflected type, no class bound (0x120/0xB0)"),
			"field: an unreflected type's member resolves from the map row, with no class bound");
		Check(Field("FPlain::Block", "FPlain", "Block", PLUGIN_FIELD_NATIVE, 0xB0, {}, v, e, VersionMap::Exe{ 127004, 0, 0 })
			&& v.offset == 0x120, "field: an unreflected type's member resolves on the other build's row");
		Check(!Field("FPlain::Block", "FPlain", "Block", PLUGIN_FIELD_NATIVE, 0x20, {}, v, e)
			&& e == "loader map, CL-128786 row gives size 0xB0, the code was compiled for 0x20", "field: an unreflected type's member still needs the size it was compiled for");
		Check(!Field("FPlain::Nothing", "FPlain", "Nothing", PLUGIN_FIELD_NATIVE, 4, {}, v, e, VersionMap::Exe{ 127004, 0, 0 })
			&& e == "FPlain is not reflected, so its member needs a row for CL-127004 and has no other source",
			"field: an unreflected type's member with no row is refused, even on the build it was compiled for");
		Check(!Field("FPlain::Block", "FPlain", "Block", PLUGIN_FIELD_NATIVE, 0xB0, {}, v, e, later)
			&& e.find("no row for CL-130000") == 0, "field: an unreflected type's member on a build with no row is refused");
		Check(!Field("AUnready::X", "AUnready", "X", PLUGIN_FIELD_NATIVE, 4, { FieldRow(128786, 0, 4) }, v, e)
			&& e == "reflection, for the size of AUnready: the game's reflection data is not ready",
			"field: reflection that is not ready is not an unreflected type");
		VersionMap::SetLiveFieldSource(nullptr);
		Check(!Field("ATest::Native", "ATest", "Native", PLUGIN_FIELD_NATIVE, 4, {}, v, e) && e.find("cannot be bounded") != std::string::npos,
			"field: no reflection reader, no native member");
	}

	// Struct sizes (member empty) are never map data: they come live from the game's reflection, and a plugin that
	// needs the compiled size to hold asks for PLUGIN_FIELD_CHECK. See "Not map data" in VersionMap.md.
	{
		VersionMap::SetLiveFieldSource(FakeLive);
		VersionMap::FieldValue v;
		std::string e;

		// Size 0 and no CHECK: the live size comes back, whatever the code was compiled for.
		Check(Field("AOther", "AOther", "", 0, 0, {}, v, e) && v.size == 0x10 && v.offset == 0 && v.boolMask == 0
			&& Logged(L"AOther: reflected (no row for CL-128786) (0x10)"), "size: size 0 takes the game's live size");
		Check(Field("AOther", "AOther", "", 0, 0x10, {}, v, e) && v.size == 0x10, "size: the compiled size matches the game's: the live size");
		Check(Field("AOther", "AOther", "", 0, 0x20, {}, v, e) && v.size == 0x10 && !Logged(L"as compiled"),
			"size: without CHECK a different compiled size is not compared: the live size is returned");

		// CHECK compares the compiled size with the game's and refuses a difference.
		Check(Field("AOther", "AOther", "", PLUGIN_FIELD_CHECK, 0x10, {}, v, e) && v.size == 0x10
			&& Logged(L"AOther: reflected (no row for CL-128786), as compiled (0x10)"), "size: CHECK passes when the game's size is the compiled one");
		Check(!Field("AOther", "AOther", "", PLUGIN_FIELD_CHECK, 0x18, {}, v, e)
			&& e == "the code relies on 0x18, this build has 0x10"
			&& Logged(L"AOther: refused: the code relies on 0x18, this build has 0x10"), "size: CHECK refuses a game size that is not the compiled one");

		// Only a reflected struct has a size: a native or bitfield flag on a size request is a bad request, an unknown class an error.
		Check(!Field("AOther", "AOther", "", PLUGIN_FIELD_NATIVE, 0x10, {}, v, e)
			&& e == "bad request: a field needs a class, and a size row is neither native nor a bitfield", "size: NATIVE on a size request is a bad request");
		Check(!Field("AOther", "AOther", "", PLUGIN_FIELD_BITFIELD, 0, {}, v, e) && e.find("bad request:") == 0,
			"size: BITFIELD on a size request is a bad request");
		Check(!Field("AOther", "AOther", "", PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e) && e.find("bad request: PLUGIN_FIELD_ANY_SIZE") == 0,
			"size: ANY_SIZE is for a member, not a size request");
		Check(!Field("ANone", "ANone", "", 0, 0, {}, v, e) && e == "reflection: no class ANone", "size: a class the game does not have is an error");
		Check(!Field("AOther", "AOther", "", PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_CHECK, 0x10, { FieldRow(128786, 0, 0x10) }, v, e)
			&& e.find("bad request:") == 0, "size: an own row does not turn a size request native");

		// An own row is only a cross-check too: the game's size is the answer.
		Check(Field("AOther", "AOther", "", 0, 0, { FieldRow(128786, 0, 0x30) }, v, e) && v.size == 0x10
			&& Logged(L"AOther: the CL-128786 row says 0x0/0x30, the game says 0x0/0x10; the game wins"), "size: an own row the game disagrees with: the game's size wins");

		// A class-keyed map row (no "::" in the key) is a cross-check as well: where it disagrees the game wins and the
		// difference is logged; where it agrees the pick says so.
		VersionMap::MapData classRows = VersionMap::BaselineMap();
		classRows.fields.push_back({ "AChild", 128786, 0, 0x50, 0, "" });
		classRows.fields.push_back({ "AOther", 128786, 0, 0x10, 0, "" });
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(classRows));
		Check(Field("AChild", "AChild", "", 0, 0, {}, v, e) && v.size == 0x58
			&& Logged(L"AChild: the loader map, CL-128786 row says 0x0/0x50, the game says 0x0/0x58; the game wins")
			&& Logged(L"overruled by reflection"), "size: a class-keyed map row the game disagrees with: the game wins, logged");
		Check(Field("AOther", "AOther", "", 0, 0, {}, v, e) && v.size == 0x10 && !Logged(L"the game wins")
			&& Logged(L"loader map, CL-128786 row, as reflected (0x10)"), "size: a class-keyed map row the game agrees with");
		Check(!Field("AChild", "AChild", "", PLUGIN_FIELD_CHECK, 0x50, {}, v, e) && e == "the code relies on 0x50, this build has 0x58",
			"size: CHECK compares with the game, not with the class-keyed row");
		// ... and it is never served without the game: no reflection reader, no size.
		VersionMap::SetLiveFieldSource(nullptr);
		Check(!Field("AOther", "AOther", "", 0, 0, {}, v, e) && e == "no reflection data to read a reflected field from",
			"size: a class-keyed row does not stand in for reflection");
		VersionMap::SetActiveMap(nullptr);
		VersionMap::SetLiveFieldSource(FakeLive);

		// The same request through the plugin ABI: member null, the live size in the result.
		IPluginHookScanner* scan = ModLoaderLogger::GetPluginHookScanner();
		const auto ask = [&](const IPluginSelf* self, const char* className, uint32_t flags, uint32_t size, PluginFieldResult& result)
		{
			PluginFieldRequest request = PLUGIN_FIELD_REQUEST_INIT;
			request.key       = className;
			request.className = className;
			request.member    = nullptr;
			request.flags     = flags;
			request.size      = size;
			request.builtCL   = 127004;
			result = PluginFieldResult{};
			return scan->ResolveField(self, &request, &result);
		};
		IPluginSelf sizes{ "SizePlug", "1", nullptr, nullptr, nullptr };
		bool refusedFlag = false;
		PluginHookReport::BeginSession(&sizes, "SizePlug", "SizePlug.dll");
		PluginFieldResult r{};
		Check(ask(&sizes, "AOther", PLUGIN_FIELD_FLAG_NONE, 0, r) && r.size == 0x10 && r.offset == 0 && r.boolMask == 0,
			"size: ResolveField with member null and size 0 returns the live size");
		Check(ask(&sizes, "AChild", PLUGIN_FIELD_CHECK, 0x58, r) && r.size == 0x58, "size: ResolveField with CHECK and the game's size");
		Check(!PluginHookReport::SessionHasFailures(&sizes), "size: those requests are not failures");
		Check(!ask(&sizes, "AChild", PLUGIN_FIELD_CHECK, 0x50, r) && r.size == 0 && PluginHookReport::SessionHasFailures(&sizes),
			"size: ResolveField with CHECK and a size the game does not have fails and writes nothing");
		PluginHookReport::EndSession(&sizes, &refusedFlag);
		VersionMap::SetLiveFieldSource(nullptr);
	}

	// Admission by the plugin's built CL (R3). A PluginInfo as a plugin fills it.
	{
		const auto info = [](int version, uint32_t builtCL, uint32_t flags)
		{
			PluginInfo i = { "T", "1", "t", "t", version, PLUGIN_TARGET_CLIENT };
			i.gameBuiltCL = builtCL;
			i.mapFlags    = flags;
			return i;
		};
		const auto refusal = [&](uint32_t runningCL, int version, uint32_t builtCL, uint32_t flags, bool globals)
		{
			const PluginInfo i = info(version, builtCL, flags);
			return VersionMap::OffBuildRefusal(VersionMap::Exe{ runningCL, 0, 0 }, version, VersionMap::ReadPluginGameBuild(i), globals);
		};
		constexpr uint32_t kM = PLUGIN_MAP_MIGRATED;

		// v70 and v71 have no tail (a garbled one must not be read) and are judged as built on kLegacyPluginCL.
		const VersionMap::PluginGameBuild legacy = VersionMap::ReadPluginGameBuild(info(71, 128786, kM));
		Check(legacy.gameCL == VersionMap::kLegacyPluginCL && !legacy.declared && legacy.mapFlags == 0, "tail: v71 gets kLegacyPluginCL and no flags");
		Check(refusal(127004, 70, 0, 0, false).empty() && refusal(127004, 71, 0, 0, false).empty(), "admission: v70 and v71 load on 127004");
		Check(refusal(128786, 70, 0, 0, true).find("before the version map") != std::string::npos
			&& refusal(128786, 71, 0, 0, true).find("It loads on CL-127004") != std::string::npos, "admission: v70 and v71 need an update on 128786");

		// An older interface is never refused for its age, only judged by its build. Before v70
		// (StarRuptureTimer 1.3.0 is v46, ConfirmHotkey v67) that is a build before 127004, which no
		// supported build is, so it is never called; the reason names the build, not an interface range.
		const VersionMap::PluginGameBuild old46 = VersionMap::ReadPluginGameBuild(info(46, 128786, kM));
		Check(old46.gameCL == 0 && !old46.declared && old46.mapFlags == 0, "tail: v46 gets build 0 and no flags");
		for (const uint32_t running : { 127004u, 128786u, 0u })
			for (const int version : { 46, 62, 67, 69 })
			{
				const std::string r = refusal(running, version, 0, 0, true);
				Check(r.find("Needs an update") == 0 && r.find("a build before CL-127004 had it") != std::string::npos
					&& r.find("It loads on") == std::string::npos, "admission: before v70 is judged as built before 127004, on every build");
			}
		Check(VersionMap::DescribePluginGameBuild(old46) == "a build before CL-127004", "describe: build 0");

		// v72 built on 127004: loads there; off it, only when migrated and it took the globals.
		Check(refusal(127004, 72, 127004, 0, false).empty(), "admission: v72 on its built CL, no flag");
		Check(refusal(128786, 72, 127004, kM, true).empty(), "admission: v72 MIGRATED admitted off its build");
		Check(refusal(128786, 72, 127004, kM, false).find("GetSdkGlobals") != std::string::npos, "admission: MIGRATED without the globals is refused");
		const std::string noFlag = refusal(128786, 72, 127004, 0, true);
		Check(noFlag.find("Needs an update") == 0 && noFlag.find("PLUGIN_MAP_MIGRATED") != std::string::npos, "admission: v72 without the flag names the flag");

		// Judged by its own CL, not the loader's: built on 128786 without the flag it is refused on 127004.
		const std::string future = refusal(127004, 72, 128786, 0, true);
		Check(future.find("PLUGIN_MAP_MIGRATED") != std::string::npos && future.find("It loads on CL-128786") != std::string::npos,
			"admission: built CL 128786 without the flag is refused on 127004");
		Check(refusal(128786, 72, 128786, 0, false).empty(), "admission: built CL 128786 loads on 128786");
		Check(refusal(127004, 72, 128786, kM, true).empty(), "admission: built CL 128786 MIGRATED loads on 127004");

		// Classes the layout check found changed: only a plugin that takes every layout from the map runs.
		const auto layoutRefusal = [&](int version, uint32_t builtCL, uint32_t flags)
		{
			return VersionMap::LayoutRefusal(version, VersionMap::ReadPluginGameBuild(info(version, builtCL, flags)));
		};
		Check(layoutRefusal(71, 0, 0).empty() && layoutRefusal(72, 127004, 0).empty(), "layout: nothing changed, nothing refused");
		VersionMap::SetLayoutChanges({ "ACrX", "/Script/Chimera.ACrX", "FCrY", "/Script/Chimera.FCrY", "UCrZ", "/Script/Chimera.UCrZ" });
		const std::string notMigrated = layoutRefusal(72, 127004, 0);
		Check(notMigrated.find("Needs an update") == 0 && notMigrated.find("ACrX, FCrY and 1 more") != std::string::npos
			&& notMigrated.find("PLUGIN_MAP_MIGRATED") != std::string::npos && notMigrated.find("/Script/") == std::string::npos,
			"layout: a plugin without PLUGIN_MAP_MIGRATED is refused, the classes named");
		Check(layoutRefusal(71, 0, 0).find("before the version map") != std::string::npos, "layout: a v71 plugin is refused");
		Check(layoutRefusal(72, 0, kM).find("names no game build") != std::string::npos, "layout: MIGRATED with no build is refused");
		Check(layoutRefusal(72, 127004, kM).empty(), "layout: a MIGRATED plugin with a build is admitted");
		VersionMap::SetLayoutChanges({});
		Check(layoutRefusal(72, 127004, 0).empty(), "layout: cleared, admitted again");

		// A zero tail: judged as built on 127004, so refused on 128786; with the flag, refused everywhere.
		Check(refusal(127004, 72, 0, 0, false).empty(), "admission: zero tail loads on 127004");
		const std::string zero = refusal(128786, 72, 0, 0, true);
		Check(zero.find("Needs an update") == 0 && zero.find("names no game build") != std::string::npos && zero.find("It loads on CL-127004") != std::string::npos,
			"admission: zero tail is refused on 128786");
		Check(!refusal(127004, 72, 0, kM, true).empty() && !refusal(128786, 72, 0, kM, true).empty(), "admission: zero tail with MIGRATED is refused");
	}

	// The load and unload settings (v73): PluginInfo::mapFlags bits, the busy reason, the world flag.
	{
		using ReloadPolicy::Settings;
		constexpr PluginReload A = PluginReload::ANYTIME, M = PluginReload::MENU, R = PluginReload::RESTART;
		const std::string menu = ReloadPolicy::kMenuReason, restart = ReloadPolicy::kRestartReason;
		const auto flags = [](PluginReload load, PluginReload unload) { return PluginLoadSetting(load) | PluginUnloadSetting(unload); };
		const auto settings = [](PluginReload load, PluginReload unload) { Settings s; s.load = load; s.unload = unload; return s; };

		// What a plugin fills in reads back, and the layout stayed put.
		static_assert(offsetof(PluginInfo, mapFlags) == 44 && sizeof(PluginInfo) == 48, "no layout change");
		static_assert(PluginLoadSetting(PluginReload::ANYTIME) == 0 && PluginUnloadSetting(PluginReload::ANYTIME) == 0, "ANYTIME is 0");
		static_assert((PluginLoadSetting(PluginReload::RESTART) | PluginUnloadSetting(PluginReload::RESTART)) != PLUGIN_MAP_MIGRATED, "clear of the map flag");
		for (const PluginReload load : { A, M, R })
			for (const PluginReload unload : { A, M, R })
			{
				const Settings s = ReloadPolicy::Read(73, PLUGIN_MAP_MIGRATED | flags(load, unload));
				Check(s.load == load && s.unload == unload, "settings: load and unload read back, beside PLUGIN_MAP_MIGRATED");
			}

		// An old plugin reads as ANYTIME for both: no bits, or no tail, or an older interface whatever mapFlags holds.
		Check(ReloadPolicy::Read(73, 0).load == A && ReloadPolicy::Read(73, 0).unload == A, "settings: v73 with the field unset is ANYTIME");
		Check(ReloadPolicy::Read(72, PLUGIN_MAP_MIGRATED).load == A && ReloadPolicy::Read(72, PLUGIN_MAP_MIGRATED).unload == A,
			"settings: a v72 plugin with only the map flag is ANYTIME");
		Check(ReloadPolicy::Read(72, flags(R, R)).load == A && ReloadPolicy::Read(70, 0xFFFFFFFFu).unload == A, "settings: before v73 the bits are not read");
		PluginInfo unset = { "T", "1", "t", "t", 73, PLUGIN_TARGET_CLIENT };
		Check(ReloadPolicy::Read(unset.interfaceVersion, unset.mapFlags).load == A, "settings: a six-member PluginInfo is ANYTIME");
		Check(ReloadPolicy::Read(73, 3u << PLUGIN_RELOAD_LOAD_SHIFT).load == R, "settings: a value no setting uses reads as RESTART");

		// ANYTIME is unchanged: allowed in every state.
		for (const bool world : { false, true })
		{
			Check(ReloadPolicy::LoadRefusal(A, true, world).empty() && ReloadPolicy::LoadRefusal(A, false, world).empty(), "ANYTIME: load allowed");
			Check(ReloadPolicy::UnloadRefusal(A, "", world).empty(), "ANYTIME: unload allowed");
			Check(ReloadPolicy::ReloadRefusal(settings(A, A), "", true, world).empty() && ReloadPolicy::ReloadRefusal(settings(A, A), "", false, world).empty(),
				"ANYTIME: reload and load allowed");
		}

		// MENU: refused in a world, with its reason; allowed at the menu.
		Check(ReloadPolicy::LoadRefusal(M, false, true) == menu, "MENU: load refused in a world, with its reason");
		Check(ReloadPolicy::LoadRefusal(M, false, false).empty() && ReloadPolicy::LoadRefusal(M, true, false).empty(), "MENU: load allowed at the menu");
		Check(ReloadPolicy::UnloadRefusal(M, "", true) == menu, "MENU: unload refused in a world, with its reason");
		Check(ReloadPolicy::UnloadRefusal(M, "", false).empty(), "MENU: unload allowed at the menu");
		Check(ReloadPolicy::ReloadRefusal(settings(M, M), "", true, true) == menu && ReloadPolicy::ReloadRefusal(settings(M, M), "", false, true) == menu,
			"MENU: reload and load refused in a world");
		Check(ReloadPolicy::ReloadRefusal(settings(M, M), "", true, false).empty(), "MENU: reload allowed at the menu");

		// RESTART: load only at startup; never unloaded at run time.
		Check(ReloadPolicy::LoadRefusal(R, true, false).empty() && ReloadPolicy::LoadRefusal(R, true, true).empty(), "RESTART: load allowed at startup");
		Check(ReloadPolicy::LoadRefusal(R, false, false) == restart && ReloadPolicy::LoadRefusal(R, false, true) == restart,
			"RESTART: load refused at run time, in a world or not, with its reason");
		for (const bool world : { false, true })
		{
			const std::string why = ReloadPolicy::UnloadRefusal(R, "", world);
			Check(why.find("Unload not supported") == 0 && why.find("requires a game restart") != std::string::npos, "RESTART: unload refused, with its reason");
			Check(!ReloadPolicy::ReloadRefusal(settings(R, R), "", true, world).empty(), "RESTART: reload refused");
		}

		// Each combination of the two settings: a reload needs both, checked in full before it starts.
		for (const PluginReload load : { A, M, R })
			for (const PluginReload unload : { A, M, R })
				for (const bool world : { false, true })
				{
					const Settings s = settings(load, unload);
					const bool unloadOk = ReloadPolicy::UnloadRefusal(unload, "", world).empty();
					const bool loadOk   = ReloadPolicy::LoadRefusal(load, false, world).empty();
					Check(ReloadPolicy::ReloadRefusal(s, "", true, world).empty() == (unloadOk && loadOk), "combination: a reload needs the unload and the load");
					Check(ReloadPolicy::ReloadRefusal(s, "", false, world).empty() == loadOk, "combination: loading an unloaded plugin needs only the load");
					Check(unloadOk == (unload == A || (unload == M && !world)), "combination: the unload setting decides UNLOAD");
				}
		Check(ReloadPolicy::ReloadRefusal(settings(R, A), "", true, false) == restart, "combination: unload ANYTIME with load RESTART still refuses the reload");

		// The busy reason: set, refused with it, cleared, and independent per plugin.
		const IPluginSelf a = {}, b = {};
		Check(ReloadPolicy::BusyReason(&a).empty(), "busy: none to start with");
		ReloadPolicy::SetBusyReason(&a, "GPU work in flight");
		Check(ReloadPolicy::BusyReason(&a) == "GPU work in flight" && ReloadPolicy::BusyReason(&b).empty(), "busy: set for that plugin only");
		Check(ReloadPolicy::UnloadRefusal(A, ReloadPolicy::BusyReason(&a), false) == "Unload blocked: GPU work in flight", "busy: unload refused with the reason");
		Check(ReloadPolicy::ReloadRefusal(settings(A, A), ReloadPolicy::BusyReason(&a), true, false) == "Unload blocked: GPU work in flight", "busy: reload refused with the reason");
		Check(ReloadPolicy::ReloadRefusal(settings(A, A), ReloadPolicy::BusyReason(&a), false, false).empty(), "busy: loading an unloaded plugin is not blocked by it");
		Check(ReloadPolicy::UnloadRefusal(M, ReloadPolicy::BusyReason(&a), true) == menu, "busy: a MENU refusal is said first");
		ReloadPolicy::SetBusyReason(&a, nullptr);
		Check(ReloadPolicy::BusyReason(&a).empty() && ReloadPolicy::UnloadRefusal(A, ReloadPolicy::BusyReason(&a), false).empty(), "busy: cleared by null");
		ReloadPolicy::SetBusyReason(&a, "x");
		ReloadPolicy::SetBusyReason(&a, "");
		Check(ReloadPolicy::BusyReason(&a).empty(), "busy: cleared by an empty string");
		ReloadPolicy::SetBusyReason(&a, "line\none\ttab");
		Check(ReloadPolicy::BusyReason(&a) == "line one tab", "busy: control characters become spaces");
		ReloadPolicy::SetBusyReason(&a, std::string(200, 'x').c_str());
		Check(ReloadPolicy::BusyReason(&a).size() == ReloadPolicy::kBusyReasonMax, "busy: cut at the limit");
		ReloadPolicy::SetBusyReason(&a, nullptr);
		ReloadPolicy::SetBusyReason(nullptr, "ignored");

		// Any thread may set it while another reads.
		{
			std::atomic<bool> stop{ false };
			std::thread setter([&] { for (int i = 0; i < 20000; ++i) ReloadPolicy::SetBusyReason(&b, i % 2 ? "busy" : nullptr); stop = true; });
			bool sane = true;
			while (!stop) { const std::string r = ReloadPolicy::BusyReason(&b); sane = sane && (r.empty() || r == "busy"); }
			setter.join();
			Check(sane, "busy: set from another thread while it is read");
			ReloadPolicy::SetBusyReason(&b, nullptr);
		}

		// The world flag follows the begin and end events.
		Check(!ReloadPolicy::GameplayWorldRunning(), "world: none before any world began");
		ReloadPolicy::NoteWorldBegan("Map_MainMenu");
		Check(!ReloadPolicy::GameplayWorldRunning(), "world: the main menu is not a gameplay world");
		ReloadPolicy::NoteWorldBegan("ChimeraMain");
		Check(ReloadPolicy::GameplayWorldRunning(), "world: ChimeraMain began");
		ReloadPolicy::NoteWorldEnded("SomeOtherWorld");
		Check(ReloadPolicy::GameplayWorldRunning(), "world: another world ending does not end it");
		ReloadPolicy::NoteWorldEnded("ChimeraMain");
		Check(!ReloadPolicy::GameplayWorldRunning(), "world: ChimeraMain ended");
		ReloadPolicy::NoteWorldBegan("ChimeraMain");
		ReloadPolicy::NoteWorldBegan("Map_MainMenu");
		Check(!ReloadPolicy::GameplayWorldRunning(), "world: the menu world beginning means no session, even if the end was missed");
		ReloadPolicy::NoteWorldBegan(nullptr);
		ReloadPolicy::NoteWorldEnded(nullptr);
		Check(!ReloadPolicy::GameplayWorldRunning(), "world: a null name changes nothing");
	}

	// The feed reader and the active map.
	{
		const std::string head = "format\t1\nbuild\t128786\t0x22222222\t0x2000000\tpublic_test_branch 2\n";
		const std::string good = head
			+ "function\tTest::MapOnly\t0x0\t128786\t0x22222222\t0x2000000\t1\tB0 01\t0\t-1\t-1\t0x7000\n"
			+ "absent\tTest::Gone\t0x0\t128786\t0x22222222\t0x2000000\t1\t-\n"
			+ "field\tATest::Reflected\t128786\t0x30\t0x8\t0x0\t-\n";
		VersionMap::MapData m;
		std::string error;
		Check(VersionMap::ParseFeed(good, m, error) && m.builds.size() == 1 && m.functions.size() == 2 && m.fields.size() == 1
			&& m.functions[0].pattern == "B0 01" && m.functions[1].absent && m.fields[0].offset == 0x30, "feed: a good feed reads");
		const auto bad = [&](const std::string& text, const char* what)
		{
			VersionMap::MapData out;
			std::string e;
			Check(!VersionMap::ParseFeed(text, out, e) && !e.empty() && out.builds.empty() && out.functions.empty(), what);
		};
		bad("", "feed: empty text");
		bad(good.substr(0, good.size() - 1), "feed: no final line feed");
		bad("format\t2\n", "feed: another format");
		bad("format\t1\r\n", "feed: a carriage return");
		bad(head + "function\tTest::MapOnly\t0x0\t128786\t0x22222222\t0x2000001\t1\tB0 01\t0\t-1\t-1\t0x7000\n", "feed: a row whose size is not its build's");
		bad(head + "function\tTest::MapOnly\t0x0\t127004\t0x22222222\t0x2000000\t1\tB0 01\t0\t-1\t-1\t0x7000\n", "feed: a row for a CL with no build line");
		bad(head + "function\tTest::MapOnly\t0x0\t128786\t0x22222222\t0x2000000\t2\tB0 01\t0\t-1\t-1\t0x7000\n", "feed: a scoped kind with fp 0");
		bad(head + "function\tTest::MapOnly\t0xabcd\t128786\t0x22222222\t0x2000000\t1\tB0 01\t0\t-1\t-1\t0x7000\n", "feed: lower-case hex");
		bad(head + "function\tTest::MapOnly\t0x0\t0128786\t0x22222222\t0x2000000\t1\tB0 01\t0\t-1\t-1\t0x7000\n", "feed: a leading zero");
		bad(head + "function\tTest::MapOnly\t0x0\t128786\t0x22222222\t0x2000000\t99\tB0 01\t0\t-1\t-1\t0x7000\n", "feed: a kind out of range");
		{
			const std::string row = good.substr(head.size(), good.find("absent") - head.size());
			bad(head + row + row, "feed: a duplicate function row");
		}
		bad(head + "field\tATest::Reflected\t128786\t0x30\t0x8\t0x0\t-\nfunction\tTest::MapOnly\t0x0\t128786\t0x22222222\t0x2000000\t1\tB0 01\t0\t-1\t-1\t0x7000\n",
			"feed: a function line after a field line");
		bad(head + "build\t128786\t0x22222222\t0x2000000\tx\n", "feed: two build lines for one CL");
		bad(head + "\n", "feed: a line of no type");
		bad(head + "function\tTest::MapOnly\t0x0\t128786\t0x22222222\t0x2000000\t1\tB0 01\t0\t-1\t-1\n", "feed: a function line with too few columns");
		bad(head + "absent\tTest::Gone\t0x0\t128786\t0x22222222\t0x2000000\t1\n", "feed: an absent line with too few columns");
		bad(head + "field\tATest::Reflected\t128786\t0x30\t0x8\t0x0\n", "feed: a field line with too few columns");
		bad("format\t1\nbuild\t128786\t0x22222222\t0x2000000\n", "feed: a build line with too few columns");
		bad(head + "field\tATest::Reflected\t128786\t0xzz\t0x8\t0x0\t-\n", "feed: a field line with bad hex");
		bad(head + "field\tATest::Reflected\t128786\t0x30\t0x8\t0x100\t-\n", "feed: a field line with a mask out of range");
		bad(head + "field\textra\n", "feed: a known kind is not skipped for its columns");

		// A kind this loader does not know is skipped, with one summary line; extra columns on a known kind are ignored.
		{
			const std::string fn = "function\tTest::MapOnly\t0x0\t128786\t0x22222222\t0x2000000\t1\tB0 01\t0\t-1\t-1\t0x7000";
			const std::string fld = "field\tATest::Reflected\t128786\t0x30\t0x8\t0x0\t-";
			const std::string wide = "format\t1\nparam\tATest\t1\n" + std::string("build\t128786\t0x22222222\t0x2000000\tpublic_test_branch 2\textra\n")
				+ "flag\t1\n" + fn + "\tx\ty\n" + "param\tATest\t2\n" + "absent\tTest::Gone\t0x0\t128786\t0x22222222\t0x2000000\t1\t-\tx\n"
				+ fld + "\textra\nflag\t2\n";
			VersionMap::MapData w;
			std::string e;
			TestDoubles::g_log.clear();
			size_t summaries = 0;
			Check(VersionMap::ParseFeed(wide, w, e) && w.builds.size() == 1 && w.functions.size() == 2 && w.fields.size() == 1
				&& w.functions[0].pattern == "B0 01" && w.functions[0].rva == 0x7000 && w.functions[1].absent && w.fields[0].offset == 0x30,
				"feed: unknown kinds are skipped and the rest applies, extra columns are ignored");
			for (const std::wstring& line : TestDoubles::g_log)
				summaries += line.find(L"skipped") != std::wstring::npos;
			Check(summaries == 1 && Logged(L"[Map] Feed: skipped 4 rows of unknown kinds: param, flag"), "feed: one summary line counts the skipped rows and lists each kind once");
			TestDoubles::g_log.clear();
			Check(VersionMap::ParseFeed(good, w, e) && !Logged(L"skipped"), "feed: a feed with no unknown kind logs no summary");
			TestDoubles::g_log.clear();
			Check(!VersionMap::ParseFeed("format\t1\nparam\t1\n" + head.substr(9) + "field\tx\n", w, e) && !Logged(L"skipped"),
				"feed: a rejected feed logs no summary");
		}

		// A map read from the feed replaces the baseline for every lookup, and SetActiveMap(nullptr) restores it.
		Check(!VersionMap::ActiveMap().functions.empty() && VersionMap::ActiveMap().builds.size() == 2, "active map: the baseline to start");
		Result q = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart));
		Check(At(q, 0x1000), "active map: the baseline's row");
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(m));
		q = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart));
		Check(At(q, 0x7000) && Logged(L"loader map, CL-128786 row"), "active map: a fed row is read");
		Check(VersionMap::SupportedBuilds() == std::vector<uint32_t>{ 128786 }, "active map: builds come from the fed map");
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>());
		q = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart));
		Check(!q.Succeeded() && !Logged(L"loader map, CL-128786 row"), "active map: an empty map holds no row");
		VersionMap::SetActiveMap(nullptr);
		Check(At(Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart)), 0x1000), "active map: the baseline again");

		// An address-only row without an RVA is refused by the reader, and one
		// that reaches the map anyway refuses its key on its own build only.
		const std::string head2 = "format\t1\nbuild\t127004\t0x11111111\t0x1000000\tx\n"
			"build\t128786\t0x22222222\t0x2000000\tpublic_test_branch 2\n";
		bad(head2 + "function\tTest::MapOnly\t0x0\t127004\t0x11111111\t0x1000000\t1\t-\t0\t-1\t-1\t0x0\n"
			"function\tTest::MapOnly\t0x0\t128786\t0x22222222\t0x2000000\t1\tB0 01\t0\t-1\t-1\t0x7000\n",
			"feed: an address-only row with no RVA");
		bad(head + "field\tATest::Reflected\t127004\t0x30\t0x8\t0x0\t-\n", "feed: a field for a CL with no build line");
		VersionMap::MapData other = m;
		other.builds.push_back({ 127004, 0x11111111, 0x1000000, "x" });
		VersionMap::MapFunctionRow noRva = other.functions[0];
		noRva.gameCL = 127004; noRva.imageStamp = 0x11111111; noRva.imageSize = 0x1000000;
		noRva.pattern.clear(); noRva.rva = 0;
		other.functions.push_back(noRva);
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(other));
		Check(At(Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart, "B0 01")), 0x7000),
			"active map: an address-only row with no RVA for another build does not refuse the key here");
		q = VersionMap::ResolveMapKey("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart, "B0 01"), {},
			VersionMap::Exe{ 127004, 0x11111111, 0x1000000 }, false);
		Check(q.outcome == Outcome::BadRequest, "active map: on its own build that row refuses the key");
		VersionMap::SetActiveMap(nullptr);
	}

	Check(Real::VersionMap::kFormatVersion == 2, "the real map is format 2");

	// The real map's field rows: a member's row for each build, and the older build's row untouched.
	{
		const auto fieldRow = [](const char* key, uint32_t cl) -> const Real::VersionMap::FieldRow*
		{
			for (const Real::VersionMap::Index& index : Real::VersionMap::kFields)
				if (strcmp(index.key, key) == 0)
					for (uint16_t i = index.first; i < index.first + index.count; ++i)
						if (Real::VersionMap::kFieldRows[i].gameCL == cl)
							return &Real::VersionMap::kFieldRows[i];
			return nullptr;
		};
		const Real::VersionMap::FieldRow* oldRow = fieldRow("UCrGameInstance::ServerSessionName", 127004);
		const Real::VersionMap::FieldRow* newRow = fieldRow("UCrGameInstance::ServerSessionName", 128786);
		Check(oldRow && oldRow->offset == 0x290 && oldRow->size == 0x10, "real field rows: ServerSessionName keeps its CL-127004 row");
		Check(newRow && newRow->offset == 0x2A0 && newRow->size == 0x10, "real field rows: ServerSessionName has its CL-128786 row");
		oldRow = fieldRow("UCrEnviroWaveTimerSubsystem::TimerActor", 127004);
		newRow = fieldRow("UCrEnviroWaveTimerSubsystem::TimerActor", 128786);
		Check(oldRow && oldRow->offset == 0x90 && newRow && newRow->offset == 0x98, "real field rows: TimerActor 0x90 on CL-127004, 0x98 on CL-128786");
	}
	// The real map's function rows for Build Windows Anywhere and Build Benches Anywhere: one function-start row per
	// build, the CL-127004 rows untouched by the CL-128786 ones. Each row is the plugin's own pattern that was proven on
	// its build, with the RVA measured from that build's PDB, so a pattern that drifts on the exe it was verified on is refused.
	// The MiniMap's keys have a row for each build where the same pattern holds, the older build's row untouched, and
	// two near-identical natives told apart by their own rows.
	{
		const auto functionRow = [](const char* key, uint32_t cl) -> const Real::VersionMap::FunctionRow*
		{
			for (const Real::VersionMap::Index& index : Real::VersionMap::kFunctions)
				if (strcmp(index.key, key) == 0)
					for (uint16_t i = index.first; i < index.first + index.count; ++i)
						if (Real::VersionMap::kFunctionRows[i].gameCL == cl)
							return &Real::VersionMap::kFunctionRows[i];
			return nullptr;
		};
		const auto isStartRow = [](const Real::VersionMap::FunctionRow* row, uint32_t rva)
		{
			return row && !row->absent && row->kind == static_cast<int>(Kind::FunctionStart) && row->fp == 0 && row->rva == rva
			    && row->pattern && *row->pattern && row->resultOffset == 0;
		};
		struct AnywhereKey { const char* key; uint32_t rva127004, rva128786; };
		const AnywhereKey anywhereKeys[] = {
			{ "ACrAPHelperActorCustom::CheckMainMeshCollision", 0x074DEE60, 0x0755C5F0 },
			{ "ACrAPHelper::GetBoxCollisionResult",             0x074EB530, 0x07521070 },
			{ "UCrSocketSnapPlacementMethod::GetSnappedSocket", 0x074C0920, 0x074DAE00 },
			{ "UCrBuildingStabilitySubsystem::IsSocketFree",    0x074651D0, 0x074E6100 },
		};
		for (const AnywhereKey& k : anywhereKeys)
			Check(isStartRow(functionRow(k.key, 127004), k.rva127004) && isStartRow(functionRow(k.key, 128786), k.rva128786),
				(std::string("real function rows: ") + k.key + " has a measured row on each build").c_str());

		const Real::VersionMap::FunctionRow* oldRow = functionRow("SplinePayload::GetSplineActorData", 127004);
		const Real::VersionMap::FunctionRow* newRow = functionRow("SplinePayload::GetSplineActorData", 128786);
		Check(oldRow && oldRow->rva == 0x0668DEA0 && newRow && newRow->rva == 0x066953E0, "real function rows: GetSplineActorData on both builds");
		oldRow = functionRow("FRenderCommandFence::BeginFence", 127004);
		newRow = functionRow("FRenderCommandFence::BeginFence", 128786);
		Check(oldRow && newRow && newRow->rva == 0x03B01050 && strcmp(oldRow->pattern, newRow->pattern) != 0, "real function rows: BeginFence has a pattern of its own for CL-128786");
		oldRow = functionRow("MiniMap/FRenderCommandFence::FRenderCommandFence", 127004);
		newRow = functionRow("MiniMap/FRenderCommandFence::FRenderCommandFence", 128786);
		Check(oldRow && newRow && oldRow->fp == newRow->fp && oldRow->fp != 0, "real function rows: a site key's rows for both builds carry the one fingerprint");
		oldRow = functionRow("UCrBuildingStabilitySubsystem::OnCreateBuildingEntity", 128786);
		newRow = functionRow("UCrBuildingStabilitySubsystem::OnDestroyBuildingEntity", 128786);
		Check(oldRow && newRow && oldRow->rva == 0x074E7BF0 && newRow->rva == 0x074E7C30 && strcmp(oldRow->pattern, newRow->pattern) != 0,
			"real function rows: building add and remove are two functions with two patterns on CL-128786");
		oldRow = functionRow("UCrBuildingStabilitySubsystem::OnCreateBuildingEntity", 127004);
		newRow = functionRow("UCrBuildingStabilitySubsystem::OnDestroyBuildingEntity", 127004);
		Check(oldRow && newRow && oldRow->rva == 0x07469170 && newRow->rva == 0x074691B0 && strcmp(oldRow->pattern, newRow->pattern) != 0,
			"real function rows: building add and remove are two functions with two patterns on CL-127004 too");
		const char* const textureKeys[] = { "UTexture2D::CreateTransient", "FBulkData::Lock", "FBulkData::Unlock", "UTexture::GetResource", "MiniMap/UTexture::UpdateResource" };
		bool textureBoth = true;
		for (const char* key : textureKeys)
		{
			oldRow = functionRow(key, 127004);
			newRow = functionRow(key, 128786);
			textureBoth = textureBoth && oldRow && newRow && oldRow->fp == newRow->fp && strcmp(oldRow->pattern, newRow->pattern) == 0;
		}
		Check(textureBoth, "real function rows: the texture natives and the UpdateResource site have rows for both builds with one pattern");
	}
	Check(unmatched == 0, "every real map key is a registry row's or a plugin's global or scoped key");
	// The key index against the rows, for the function table and the field table: the ranges tile the
	// rows in order with none empty, each key appears once, and a key holds one row per game build. A
	// generated map grows by whole keys and rows, so no count is fixed here; a shifted first or count,
	// or a repeated key or build, still fails.
	const auto checkIndex = [](const auto& indexes, const auto& rows, const char* table)
	{
		int badRange = 0, repeatedKey = 0, repeatedBuild = 0;
		size_t nextRow = 0;
		for (size_t k = 0; k < indexes.size(); ++k)
		{
			const Real::VersionMap::Index& index = indexes[k];
			if (index.count == 0 || index.first != nextRow)
			{
				++badRange;
				printf("  index range does not follow the previous one: %s\n", index.key);
			}
			nextRow = static_cast<size_t>(index.first) + index.count;
			for (size_t j = 0; j < k; ++j)
				if (strcmp(indexes[j].key, index.key) == 0)
				{
					++repeatedKey;
					printf("  key indexed twice: %s\n", index.key);
				}
			for (size_t a = index.first; a < nextRow && nextRow <= rows.size(); ++a)
				for (size_t b = index.first; b < a; ++b)
					if (rows[a].gameCL == rows[b].gameCL)
					{
						++repeatedBuild;
						printf("  two rows for one build: %s\n", index.key);
					}
		}
		Check(badRange == 0 && nextRow == rows.size(), (std::string("the real map's key index tiles its ") + table + " rows exactly").c_str());
		Check(repeatedKey == 0, (std::string("every real map ") + table + " key is indexed once").c_str());
		Check(repeatedBuild == 0, (std::string("every real map ") + table + " key has at most one row per game build").c_str());
	};
	checkIndex(Real::VersionMap::kFunctions, Real::VersionMap::kFunctionRows, "function");
	checkIndex(Real::VersionMap::kFields, Real::VersionMap::kFieldRows, "field");
	Check(wrong == 0, "every real map row has its registry row's kind and fp");

	// Restarting a plugin the crash guard switched off: the plan for each setting, the limiter, the timing.
	{
		using CrashRestart::Plan;
		constexpr PluginReload A = PluginReload::ANYTIME, M = PluginReload::MENU, R = PluginReload::RESTART;
		const auto settings = [](PluginReload load, PluginReload unload) { ReloadPolicy::Settings s; s.load = load; s.unload = unload; return s; };

		// Each combination agrees with what RELOAD itself would allow (a crashed plugin has no busy reason).
		for (const PluginReload load : { A, M, R })
			for (const PluginReload unload : { A, M, R })
				for (const bool world : { false, true })
				{
					const Plan plan = CrashRestart::Decide(settings(load, unload), world, 0, true);
					const bool allowed = ReloadPolicy::ReloadRefusal(settings(load, unload), "", true, world).empty();
					Check((plan == Plan::RestartNow) == allowed, "restart: restarts exactly when RELOAD is allowed");
					if (load == R || unload == R)
						Check(plan == Plan::NeedsGameRestart, "restart: RESTART in either setting waits for a game restart");
					else if (world && (load == M || unload == M))
						Check(plan == Plan::WaitForMenu, "restart: MENU in a world waits for the main menu");
				}
		Check(CrashRestart::Decide(settings(M, M), false, 0, true) == Plan::RestartNow, "restart: MENU at the main menu restarts");
		Check(CrashRestart::Decide(settings(A, A), false, 0, false) == Plan::Disabled, "restart: the setting off stops a restart");
		Check(CrashRestart::Decide(settings(R, A), false, 0, false) == Plan::NeedsGameRestart && CrashRestart::Decide(settings(A, R), false, 9, true) == Plan::NeedsGameRestart,
			"restart: RESTART says restart the game, whatever the setting and the count");

		// The limiter: three restarts per plugin per session, then it gives up; counted by name.
		CrashRestart::Scheduler s;
		for (int i = 0; i < CrashRestart::kMaxRestarts; ++i)
		{
			Check(CrashRestart::Decide(settings(A, A), true, s.Restarts("BetterDrone"), true) == Plan::RestartNow, "limiter: restarts below the limit");
			s.CountRestart("BetterDrone");
		}
		Check(s.Restarts("BetterDrone") == 3, "limiter: three counted");
		Check(CrashRestart::Decide(settings(A, A), true, s.Restarts("BetterDrone"), true) == Plan::GaveUp, "limiter: the fourth crash gives up");
		Check(CrashRestart::Decide(settings(M, M), true, s.Restarts("BetterDrone"), true) == Plan::GaveUp, "limiter: gives up for MENU too");
		Check(s.Restarts("Codex") == 0, "limiter: per plugin");
		Check(CrashRestart::Announcement(Plan::GaveUp, "BetterDrone", "tick callback").find("stays off until the game restarts") != std::string::npos,
			"limiter: the give-up text");
		Check(CrashRestart::Announcement(Plan::NeedsGameRestart, "BetterDrone", "tick callback").find("Restart the game to turn it back on") != std::string::npos,
			"RESTART: the toast asks for a game restart");
		Check(CrashRestart::Restarted("BetterDrone", "tick callback") == "BetterDrone crashed (tick callback) and was restarted", "the restarted text");

		// Timing: a new crash is announced at once, reloads after the delay, and one noted mid-pass waits for the next pass.
		CrashRestart::Scheduler q;
		Check(!q.HasWork(), "scheduler: idle");
		q.NoteCrash(0x1000, "BetterDrone", "tick callback", 100);
		q.NoteCrash(0x1000, "BetterDrone", "tick callback", 100);
		Check(q.HasWork(), "scheduler: a crash is work");
		std::vector<CrashRestart::Entry> ready = q.TakeReady(100, false);
		Check(ready.size() == 1 && ready[0].stage == CrashRestart::Stage::New, "scheduler: one entry per module, taken at once to announce");
		q.Requeue(ready[0], CrashRestart::Stage::Scheduled, 100 + CrashRestart::kDelayMs);
		q.NoteCrash(0x2000, "Codex", "panel render", 150);          // a crash while the pass runs
		Check(q.TakeReady(101, false).size() == 1, "scheduler: the next pass takes only the new crash");
		Check(q.TakeReady(100 + CrashRestart::kDelayMs - 1, false).empty(), "scheduler: no reload before the delay");
		ready = q.TakeReady(100 + CrashRestart::kDelayMs, false);
		Check(ready.size() == 1 && ready[0].stage == CrashRestart::Stage::Scheduled && ready[0].name == "BetterDrone", "scheduler: due after the delay");
		Check(q.TakeReady(100 + CrashRestart::kDelayMs, false).empty() && !q.HasWork(), "scheduler: taken once, nothing left");

		// MENU: waits while a world runs, released at the main menu.
		q.Requeue(ready[0], CrashRestart::Stage::WaitingForMenu, 0);
		Check(q.TakeReady(1u << 30, true).empty() && q.HasWork(), "MENU: waits while a world runs");
		Check(q.TakeReady(1u << 30, false).size() == 1, "MENU: released at the main menu");

		// Exit: nothing is kept or accepted once stopped.
		q.NoteCrash(0x3000, "WAILA", "tick callback", 0);
		q.Stop();
		q.NoteCrash(0x4000, "Compass", "tick callback", 0);
		Check(!q.HasWork() && q.TakeReady(1u << 30, false).empty() && q.Stopped(), "exit: stopped drops everything and accepts nothing");
	}

	// The PLUGINS UPDATED window: plugins with word-for-word the same notes share one entry.
	{
		struct U { std::string name; std::vector<std::string> notes; };
		const std::vector<std::string> shared = { "Keeps working after a game update" };
		const std::vector<U> updates = {
			{ "BetterDrone", { "Speedometer", "Feet in mph" } },
			{ "ProductionViewer", shared },
			{ "Compass", shared },
			{ "NoNotesA", {} },
			{ "NoNotesB", {} },
			{ "RuptureTimer", shared },
			{ "Codex", { "Speedometer" } },
			{ "Waila", { "Speedometer", "Feet in mph" } },
		};
		const auto groups = UI::UpdateNoticeWindow::GroupByNotes(updates, [](const U& u) -> const std::vector<std::string>& { return u.notes; });
		const auto names = [](const std::vector<const U*>& g) { std::string s; for (const U* u : g) s += (s.empty() ? "" : ",") + u->name; return s; };
		Check(groups.size() == 5, "grouping: five entries");
		Check(groups.size() == 5 && names(groups[0]) == "BetterDrone,Waila", "grouping: identical multi-line notes group, at the first one's place");
		Check(groups.size() == 5 && names(groups[1]) == "ProductionViewer,Compass,RuptureTimer", "grouping: the shared line names all three, in order");
		Check(groups.size() == 5 && names(groups[2]) == "NoNotesA" && names(groups[3]) == "NoNotesB", "grouping: plugins with no notes stay apart");
		Check(groups.size() == 5 && names(groups[4]) == "Codex", "grouping: notes that are only a prefix of another's are their own");
		Check(UI::UpdateNoticeWindow::GroupByNotes(std::vector<U>{}, [](const U& u) -> const std::vector<std::string>& { return u.notes; }).empty(),
			"grouping: nothing in, nothing out");
	}

	// modloader.ini [Debug] DisablePluginVersionMap: plugins read no map rows, the loader keeps them.
	{
		Check(!VersionMap::PluginMapOff(), "plugin map switch: off by default");

		ScanPatterns::PreflightEntry loaderSite{};
		loaderSite.name           = "Test::Site";
		loaderSite.pattern        = "B0 01";
		loaderSite.kind           = ScanPatterns::PatternKind::InFunction;
		loaderSite.siteArithmetic = 1;
		const auto loaderOwned = [&] {
			TestDoubles::g_log.clear();
			return VersionMap::ResolveLoaderEntry(loaderSite, Ask("Test::Site", Kind::InFunction, "B0 01"), kRunning);
		};
		const auto fieldAs = [](const char* owner, VersionMap::FieldValue& out, std::string& error,
			const std::vector<VersionMap::FieldCandidate>& own = {}) {
			VersionMap::FieldRequest request;
			request.key         = "ATest::Native";
			request.className   = "ATest";
			request.member      = "Native";
			request.flags       = PLUGIN_FIELD_NATIVE;
			request.size        = 4;
			request.builtCL     = 127004;
			request.builtOffset = 0;
			request.candidates  = own;
			TestDoubles::g_log.clear();
			error.clear();
			return VersionMap::SelectField(owner, request, kRunning, out, error);
		};
		VersionMap::SetLiveFieldSource(FakeLive);
		VersionMap::FieldValue v;
		std::string e;

		// Switch off: nothing changes.
		Result q = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart));
		Check(At(q, 0x1000) && Logged(L"loader map, CL-128786 row"), "plugin map switch off: a plugin reads the map's row");
		Check(fieldAs("Test", v, e) && v.offset == 0x28, "plugin map switch off: a plugin reads the map's field row");
		Check(At(loaderOwned(), 0x7000), "plugin map switch off: the loader's site resolves");

		VersionMap::SetPluginMapOff(true);
		Check(VersionMap::PluginMapOff(), "plugin map switch: on after SetPluginMapOff(true)");

		// Switch on: a key the map holds misses; the plugin's own pattern, row or nothing decides.
		q = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart));
		Check(q.outcome == Outcome::NoMatch && Logged(L"the plugin map is switched off in modloader.ini; no pattern or rows of its own (refused)")
			&& !Logged(L"loader map"), "plugin map switch on: a map key with nothing of its own is NoMatch");
		q = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart, "B1 00"));
		Check(At(q, 0x8000) && Logged(L"switched off in modloader.ini; own pattern") && !Logged(L"loader map"),
			"plugin map switch on: the plugin's own pattern decides, not the map's row");
		q = Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart), { AddressRow(0x7000) });
		Check(At(q, 0x7000) && Logged(L"switched off in modloader.ini; own rows") && !Logged(L"loader map"),
			"plugin map switch on: the plugin's own row decides, not the map's row");
		q = Key("VmapProbe", Ask("Test::Nope", Kind::FunctionStart, "B1 00"));
		Check(At(q, 0x8000), "plugin map switch on: a key the map never held still takes its own pattern");
		q = Key("ModLoader", Ask("Test::MapOnly", Kind::FunctionStart));
		Check(q.outcome == Outcome::BadRequest, "plugin map switch on: a plugin named ModLoader is still refused");

		// Fields: a plugin's request reads its own rows only.
		Check(!fieldAs("Test", v, e) && !Logged(L"loader map") && Logged(L"switched off in modloader.ini; own rows"),
			"plugin map switch on: a native field with no row of its own is refused");
		Check(fieldAs("Test", v, e, { FieldRow(128786, 0x30, 4) }) && v.offset == 0x30,
			"plugin map switch on: a native field takes the plugin's own row");

		// The loader's own lookups keep the map.
		Check(fieldAs("ModLoader", v, e) && v.offset == 0x28 && Logged(L"loader map, CL-128786 row"),
			"plugin map switch on: the loader's field reads the map");
		Check(At(loaderOwned(), 0x7000) && Logged(L"loader map, CL-127004 row"), "plugin map switch on: the loader's site reads the map");
		Check(VersionMap::SupportedBuilds() == std::vector<uint32_t>{ 127004, 128786 }, "plugin map switch on: the loader still knows its builds");

		VersionMap::SetPluginMapOff(false);
		Check(At(Key("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart)), 0x1000), "plugin map switch off again: the map's row is back");
		VersionMap::SetLiveFieldSource(nullptr);
	}

	// Developer remote reload: off unless the ini key is on, the request file's names, the poll rate, the log lines.
	{
		const auto writeIni = [](const std::wstring& path, const char* text)
		{
			FILE* f = nullptr;
			_wfopen_s(&f, path.c_str(), L"wb");
			if (f) { fputs(text, f); fclose(f); }
		};
		wchar_t tmp[MAX_PATH]{};
		GetTempPathW(MAX_PATH, tmp);
		const std::wstring ini = std::wstring(tmp) + L"remote_reload_test.ini";

		DeleteFileW(ini.c_str());
		Check(!RemoteReload::IsEnabled(ini), "remote reload: off when there is no modloader.ini");
		writeIni(ini, "[Developer]\r\nSomethingElse=1\r\n");
		Check(!RemoteReload::IsEnabled(ini), "remote reload: off when the key is missing");
		writeIni(ini, "[Developer]\r\nRemoteReload=0\r\n");
		Check(!RemoteReload::IsEnabled(ini), "remote reload: off when the key is 0");
		writeIni(ini, "[Debug]\r\nRemoteReload=1\r\n");
		Check(!RemoteReload::IsEnabled(ini), "remote reload: the key in another section does not turn it on");
		writeIni(ini, "[Developer]\r\nRemoteReload=1\r\n");
		Check(RemoteReload::IsEnabled(ini), "remote reload: on when [Developer] RemoteReload=1");
		DeleteFileW(ini.c_str());

		RemoteReload::Request r = RemoteReload::Parse("BetterCheats\n");
		Check(r.names == std::vector<std::string>{ "BetterCheats" } && r.refused == 0, "remote reload: one name");
		r = RemoteReload::Parse("\xEF\xBB\xBF  WAILA \t\r\n\r\n# a comment\r\n   \r\nCodex.dll\r\nBetterDrone");
		Check(r.names == std::vector<std::string>{ "WAILA", "Codex.dll", "BetterDrone" },
			"remote reload: BOM, CR, blank and comment lines and the spaces around a name are dropped; a file name is kept as typed");
		r = RemoteReload::Parse("Codex\ncodex\nCODEX\nWAILA\n");
		Check(r.names == std::vector<std::string>{ "Codex", "WAILA" }, "remote reload: a name given twice in any case is asked once");
		r = RemoteReload::Parse("");
		Check(r.names.empty() && r.refused == 0, "remote reload: an empty file asks for nothing");
		r = RemoteReload::Parse("\n# nothing here\n \n");
		Check(r.names.empty(), "remote reload: a file of comments asks for nothing");
		std::string many;
		for (size_t i = 0; i < RemoteReload::kMaxNames + 3; ++i)
			many += "Plugin" + std::to_string(i) + "\n";
		r = RemoteReload::Parse(many);
		Check(r.names.size() == RemoteReload::kMaxNames && r.refused == 3 && r.names.front() == "Plugin0",
			"remote reload: past the limit the rest are counted and refused");
		Check(RemoteReload::SameName("BetterCheats", "bettercheats") && !RemoteReload::SameName("Codex", "Codex2"),
			"remote reload: names match without regard to case");

		RemoteReload::Throttle t;
		Check(t.Due(5000), "remote reload: the first look is due at once");
		Check(!t.Due(5000) && !t.Due(5000 + RemoteReload::kPollMs - 1), "remote reload: no second look within the poll time");
		Check(t.Due(5000 + RemoteReload::kPollMs) && !t.Due(5000 + RemoteReload::kPollMs + 1), "remote reload: due again after the poll time");
		RemoteReload::Throttle fresh;
		Check(fresh.Due(0), "remote reload: due at tick count 0");

		Check(RemoteReload::Reloaded("BetterCheats", "1.4.2") == "[RemoteReload] BetterCheats reloaded OK v1.4.2", "remote reload: the success line");
		Check(RemoteReload::Failed("Codex", "refused") == "[RemoteReload] Codex reload failed: refused", "remote reload: the failure line");
		Check(RemoteReload::NoSuchPlugin("Nope").find("[RemoteReload] Nope reload failed: no plugin") == 0, "remote reload: an unknown name is a failure line");
	}

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
