// VersionMapTests: the loader's map keys (memory_scanner/version_map.cpp)
// against a fixture map (fixture/hooks/game/version_map.generated.h), and the
// loader's real map against its registry (hooks/game/scan_patterns.h).
#include "test_doubles.h"
#include "plugin_class_matrix.h"

#include "hooks/game/scan_patterns.h"
#include "memory_scanner/hook_scanner_interface.h"
#include "memory_scanner/image_info.h"
#include "memory_scanner/version_map.h"
#include "core/layout_check.h"
#include "core/unknown_build.h"
#include "network_channel/admin_prompt.h"
#include "network_channel/host_rules_wire.h"
#include "plugins/host_rules.h"
#include "plugins/plugin_hook_report.h"
#include "plugins/plugin_interface.h"
#include "plugins/reload_policy.h"
#include "memory_scanner/scan_cache.h"
#include "plugins/crash_restart.h"
#include "plugins/remote_reload.h"
#include "UI/update_notice_window.h"
#include "utils/game_thread_dispatch.h"

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

	bool LoggedDebug(const wchar_t* text)
	{
		for (const std::wstring& line : TestDoubles::g_debugLog)
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

	// Whether the Blueprint class WBP_Slot_C is loaded, for FakeLive.
	bool g_bpLoaded = false;

	// Reflection as the field tests see it: ATest is 0x50 bytes, AOther 0x10,
	// AChild derives from ATest, and UTexture is 0x140 bytes, as on the game.
	VersionMap::LiveField FakeLive(const std::string& className, const std::string& member)
	{
		VersionMap::LiveField f;
		if (className == "AChild" && member.empty())
			f.size = 0x58, f.paths = { "Mod.Child", "Mod.Test" };
		else if (className == "AOther" && member.empty())
			f.size = 0x10, f.paths = { "Mod.Other" };
		else if (className == "AUnready")
			f.error = "the game's reflection data is not ready";
		else if (className == "Widget_Resize")
		{
			// A function's parameter struct: an in-parameter, a parameter a
			// game update put in front of the return, then the return, with
			// the ParmsSize the game reports.
			if (member.empty())
				f.size = 0x20;
			else if (member == "InSize")
				f.offset = 0, f.size = 4;
			else if (member == "ReturnValue")
				f.offset = 0x18, f.size = 8;
			else
				f.error = "no parameter " + member + " in Mod.Widget.Resize";
		}
		else if (className == "WBP_Slot_C_SetSize")
		{
			// A Blueprint function's parameter struct: pending until the class is loaded.
			if (!g_bpLoaded)
			{
				f.pending = true;
				f.error   = "no loaded Blueprint class has the function WBP_Slot_C_SetSize yet (the class is loaded with the screen that uses it)";
			}
			else if (member.empty())
				f.size = 8;
			else if (member == "InSize")
				f.offset = 0, f.size = 8;
			else
				f.error = "no parameter " + member + " in Game.WBP_Slot_C.SetSize";
		}
		else if (className == "UTexture" && member.empty())
			f.size = 0x140, f.paths = { "Engine.Texture", "Engine.StreamableRenderAsset", "Engine.Object" };
		else if (className != "ATest")
		{
			// As the game answers a type it does not find (LayoutCheck::FindType). The game names types without
			// the prefix letter, so the class Texture (UTexture) carries the name FTexture asks for too.
			const bool classSeen = className.size() > 1 && className.substr(1) == "Texture";
			f.unreflected = LayoutCheck::IsPlainType(className[0], false, classSeen);
			f.error = "no native " + std::string(className[0] == 'F' ? "struct " : "class ") + className
				+ (f.unreflected ? std::string() : " (the name exists under another module or kind)");
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
		const VersionMap::Exe& running = kRunning, bool* absent = nullptr, uint32_t builtCL = 127004)
	{
		TestDoubles::g_log.clear();
		TestDoubles::g_debugLog.clear();
		error.clear();
		VersionMap::FieldRequest request;
		request.key         = key;
		request.className   = className;
		request.member      = member;
		request.flags       = flags;
		request.size        = size;
		request.builtCL     = builtCL;
		request.builtOffset = 0;
		request.candidates  = own;
		return VersionMap::SelectField("Test", request, running, out, error, nullptr, absent);
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

// The texture chain members and heap members the icon copy reads (version_map.generated.h), same on both live builds.
struct ChainRow { const char* key; uint32_t offset; uint32_t size; };
static const ChainRow kChain[] = {
	{ "UTexture::PrivateResource", 0x120, 0x8 },
	{ "FTexture::TextureRHI", 0x10, 0x8 },
	{ "FD3D12Texture::ResourceLocation", 0xA8, 0x78 },
	{ "FD3D12ResourceLocation::UnderlyingResource", 0x10, 0x8 },
	{ "FD3D12Resource::DefaultResourceState", 0x9C, 0x4 },
	{ "FD3D12Resource::Resource", 0x20, 0x8 },
	{ "FD3D12Resource::Heap", 0x30, 0x8 },
	{ "FD3D12Heap::Heap", 0x20, 0x8 },
};

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

	// A build the map has no rows for (the unknown-build fallback): the newest row's pattern is rescanned on
	// this exe and checked, and the RVA recorded for another build is never used.
	{
		const VersionMap::Exe unknown{ 130045, 0x33333333, 0x03000000 };
		const auto askUnknown = [&](const char* owner, const Request& request)
		{
			TestDoubles::g_log.clear();
			return VersionMap::ResolveMapKey(owner, request, {}, unknown, false);
		};

		r = askUnknown("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart));
		Check(At(r, 0x1000) && Logged(L"no row for CL-130045") && Logged(L"CL-128786 row") && !Logged(L"CL-127004"),
			"unknown build: the newest build's row, rescanned on this exe; older rows are not read");

		const uintptr_t newestRva = TestDoubles::g_rvas["AA 01"];
		TestDoubles::g_rvas.erase("AA 01");
		r = askUnknown("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart));
		Check(!r.Succeeded() && r.address == 0, "unknown build: the newest row's pattern is not in this exe, so no address (the recorded RVA is not used)");
		TestDoubles::g_rvas["AA 01"] = newestRva;

		TestDoubles::g_counts["AA 01"] = 2;
		r = askUnknown("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart));
		Check(!r.Succeeded() && r.address == 0, "unknown build: the newest row's pattern matches twice here, so no address");
		TestDoubles::g_counts.erase("AA 01");

		r = askUnknown("VmapProbe", Ask("Test::MapOnly", Kind::FunctionStart, "AB 01"));
		Check(r.outcome == Outcome::MultipleMatches && r.address == 0 && r.detail.find("AMBIGUOUS") == 0,
			"unknown build: an own pattern that lands elsewhere than the newest row's is AMBIGUOUS");

		TestDoubles::g_log.clear();
		r = VersionMap::ResolveLoaderEntry(site, Ask("Test::Site", Kind::InFunction, "B0 01"), unknown);
		Check(At(r, 0x7000) && Logged(L"the CL-128786 row is not this site's own pattern") && !Logged(L"CL-127004"),
			"unknown build: a loader site looks at the newest build's row only, and scans its own pattern when that row's differs");
	}

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

	// A key the map lists as absent on the running build (kAbsentFields): the game build does not have the member. The
	// lookup tells it from a missing one, and the report shows it as information, not as a feature that failed.
	{
		VersionMap::SetLiveFieldSource(FakeLive);
		VersionMap::FieldValue v;
		std::string e;
		bool absent = false;

		Check(!Field("ATest::Gone", "ATest", "Gone", 0, 8, {}, v, e, kRunning, &absent) && absent
			&& e == "not in this game version (CL-128786)"
			&& Logged(L"[Map] Test: ATest::Gone: not in this game version (CL-128786)") && !Logged(L"refused"),
			"absent field: refused as not in this game version, at info");
		Check(!Field("ATest::Gone", "ATest", "Gone", 0, 8, { FieldRow(128786, 0x30, 8) }, v, e, kRunning, &absent) && absent,
			"absent field: the plugin's own row does not bring it back");
		Check(!Field("ATest::Gone", "ATest", "Gone", 0, 8, {}, v, e, kRunning) && e == "not in this game version (CL-128786)",
			"absent field: a caller that does not ask for the flag still gets the refusal");

		// Another build does not list it: an ordinary miss.
		Check(!Field("ATest::Gone", "ATest", "Gone", 0, 8, {}, v, e, VersionMap::Exe{ 127004, 0x11111111, 0x01000000 }, &absent) && !absent
			&& e == "reflection: no member Gone" && Logged(L"ATest::Gone: refused: reflection: no member Gone"),
			"absent field: another build, where it is not listed, keeps the miss wording");
		// A key the map does not list at all is a miss.
		Check(!Field("ATest::Missing", "ATest", "Missing", 0, 8, {}, v, e, kRunning, &absent) && !absent
			&& e == "reflection: no member Missing" && Logged(L"ATest::Missing: refused: reflection: no member Missing"),
			"absent field: a key the map does not list keeps the miss wording");
		// A key that is there resolves.
		Check(Field("ATest::Reflected", "ATest", "Reflected", 0, 8, {}, v, e, kRunning, &absent) && !absent && v.offset == 0x30,
			"absent field: a key that is there resolves");

		// The plugin map switched off serves no rows to plugins, and no absent list either; the loader's own lookups keep it.
		VersionMap::SetPluginMapOff(true);
		Check(!Field("ATest::Gone", "ATest", "Gone", 0, 8, {}, v, e, kRunning, &absent) && !absent && e == "reflection: no member Gone",
			"absent field: not applied while the plugin map is off");
		VersionMap::SetPluginMapOff(false);

		// The list is the active map's: a map without it asks the game.
		VersionMap::MapData noList = VersionMap::BaselineMap();
		noList.absentFields.clear();
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(noList));
		Check(!Field("ATest::Gone", "ATest", "Gone", 0, 8, {}, v, e, kRunning, &absent) && !absent && e == "reflection: no member Gone",
			"absent field: read from the active map");
		VersionMap::SetActiveMap(nullptr);

		// The report. One plugin with an absent feature, a missing one and a fine one; the plugin loads.
		IPluginHookScanner* scan = ModLoaderLogger::GetPluginHookScanner();
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
		const auto reportOf = [](const char* plugin) -> bool
		{
			for (const PluginHookReport::PluginReport& r : PluginHookReport::Snapshot())
				if (r.plugin == plugin)
					return true;
			return false;
		};

		IPluginSelf self{ "AbsentPlug", "1", nullptr, nullptr, nullptr };
		bool hotbar = true, hud = true, kept = false, refused = true;
		const unsigned serial = PluginHookReport::GetCommitSerial();
		TestDoubles::g_log.clear();
		PluginHookReport::BeginSession(&self, "AbsentPlug", "AbsentPlug.dll");
		declare(&self, "hotbar", "Hotbar", { "ATest::Gone", "ATest::AlsoGone" }, { "Hotbar.*" }, &hotbar);
		declare(&self, "hud", "Heads-up display", { "ATest::Missing" }, { "Hud.Show" }, &hud);
		declare(&self, "kept", "Kept", { "ATest::Kept" }, {}, &kept);
		PluginHookReport::RecordAbsent(&self, "ATest::Gone", "not in this game version (CL-128786)");
		PluginHookReport::RecordAbsent(&self, "ATest::Gone", "not in this game version (CL-128786)");   // asked twice: listed once
		PluginHookReport::RecordAbsent(&self, "ATest::AlsoGone", "not in this game version (CL-128786)");
		PluginHookReport::RecordFailure(&self, "ATest::Missing", "reflection: no member Missing", true);
		PluginHookReport::RecordFieldResolved(&self, "ATest::Kept");
		Check(!PluginHookReport::SessionHasFailures(&self) && !hotbar && !hud && kept, "absent report: the flags of the features that are off are false");
		PluginHookReport::EndSession(&self, &refused);
		Check(!refused, "absent report: the plugin loads");
		Check(reportOf("AbsentPlug") && PluginHookReport::GetCommitSerial() == serial + 1 && PluginHookReport::GetFeaturesOffCount("AbsentPlug") == 1,
			"absent report: only the missing key is a failure entry, and counted as a feature off");
		Check(PluginHookReport::ConfigOffReason("AbsentPlug", "Hotbar", "Scale") == "Hotbar is not in this game version"
			&& PluginHookReport::ConfigOffReason("absentplug", "Hotbar", "Anything") == "Hotbar is not in this game version"
			&& PluginHookReport::ConfigOffReason("AbsentPlug", "Hud", "Show") == "Heads-up display is off: this game build changed something it needs"
			&& PluginHookReport::ConfigOffReason("Other", "Hotbar", "Scale").empty(),
			"absent report: the config entries say not in this game version, apart from a feature that failed");
		const std::string text = PluginHookReport::BuildReportText();
		Check(text.find("Not in this game version (not failures; the feature does not apply to this build):\r\n"
				"AbsentPlug: Hotbar (ATest::Gone)\r\nAbsentPlug: Hotbar (ATest::AlsoGone)\r\n") != std::string::npos
			&& text.find("[feature off] Hotbar") == std::string::npos
			&& text.find("[feature off] Heads-up display (hud): this game build changed something it needs") != std::string::npos
			&& text.find("AbsentPlug (AbsentPlug.dll) -- loaded, 1 feature(s) off") != std::string::npos,
			"absent report: the text lists it apart from the failures, with no feature-off line of its own");
		Check(Logged(L"[Map] AbsentPlug: feature \"Hotbar\" is not in this game version: ATest::Gone, ATest::AlsoGone")
			&& Logged(L"[Map] AbsentPlug: feature \"Heads-up display\" is off: ATest::Missing (this game build changed something it needs)"),
			"absent report: one info line per feature, the failed one as before");

		// A plugin whose only miss is absent leaves no report and opens no popup; its text has no failure wording of its own.
		IPluginSelf only{ "OnlyAbsent", "1", nullptr, nullptr, nullptr };
		bool soloHotbar = true;
		const unsigned before = PluginHookReport::GetCommitSerial();
		PluginHookReport::BeginSession(&only, "OnlyAbsent", "OnlyAbsent.dll");
		declare(&only, "hotbar", "Hotbar", { "ATest::Gone" }, { "Hotbar.*" }, &soloHotbar);
		PluginHookReport::RecordAbsent(&only, "ATest::Gone", "not in this game version (CL-128786)");
		PluginHookReport::EndSession(&only, &refused);
		const std::string onlyText = PluginHookReport::BuildReportText();
		Check(!refused && !soloHotbar && !reportOf("OnlyAbsent") && PluginHookReport::GetCommitSerial() == before
			&& PluginHookReport::GetFeaturesOffCount("OnlyAbsent") == 0
			&& onlyText.find("OnlyAbsent: Hotbar (ATest::Gone)") != std::string::npos
			&& onlyText.find("OnlyAbsent (") == std::string::npos,
			"absent report: an absent-only plugin has no failure report and opens no popup");

		// One feature with an absent key and a real miss: the feature is off and warns, naming only the real miss.
		IPluginSelf mixed{ "MixedAbsent", "1", nullptr, nullptr, nullptr };
		bool mixedHotbar = true;
		TestDoubles::g_log.clear();
		PluginHookReport::BeginSession(&mixed, "MixedAbsent", "MixedAbsent.dll");
		declare(&mixed, "hotbar", "Hotbar", { "ATest::Gone", "ATest::Missing" }, { "Hotbar.*" }, &mixedHotbar);
		PluginHookReport::RecordAbsent(&mixed, "ATest::Gone", "not in this game version (CL-128786)");
		PluginHookReport::RecordFailure(&mixed, "ATest::Missing", "reflection: no member Missing", true);
		PluginHookReport::EndSession(&mixed, &refused);
		Check(!refused && !mixedHotbar
			&& Logged(L"[Map] MixedAbsent: feature \"Hotbar\" is off: ATest::Missing (this game build changed something it needs)")
			&& !Logged(L"is off: ATest::Gone"),
			"absent report: a feature with a real miss still warns, naming only the real miss");

		// A reload drops what the plugin had absent; the feature that no longer is gets no line.
		PluginHookReport::BeginSession(&only, "OnlyAbsent", "OnlyAbsent.dll");
		PluginHookReport::EndSession(&only, &refused);
		Check(PluginHookReport::ConfigOffReason("OnlyAbsent", "Hotbar", "Scale").empty()
			&& PluginHookReport::BuildReportText().find("OnlyAbsent: Hotbar") == std::string::npos,
			"absent report: a reload replaces the plugin's absent keys");

		// A key no feature lists cannot go off in its place: the plugin is refused, as for any miss.
		IPluginSelf core{ "CoreAbsent", "1", nullptr, nullptr, nullptr };
		PluginHookReport::BeginSession(&core, "CoreAbsent", "CoreAbsent.dll");
		PluginHookReport::RecordAbsent(&core, "ATest::Gone", "not in this game version (CL-128786)");
		Check(PluginHookReport::SessionHasFailures(&core), "absent report: a key no feature lists is a failure");
		PluginHookReport::EndSession(&core, &refused);
		Check(refused && reportOf("CoreAbsent")
			&& PluginHookReport::BuildReportText().find("not in this game version (CL-128786) (no feature lists this key, so the plugin cannot do without it)") != std::string::npos,
			"absent report: a plugin that cannot do without the key is refused, with the reason");
	}

	// Blueprint functions: a PLUGIN_FIELD_LATE request is pending while the class is not loaded, and resolves when
	// the plugin repeats it after the class is. Pending is not a failure; any other request is refused as before.
	{
		VersionMap::SetLiveFieldSource(FakeLive);
		IPluginHookScanner* scan = ModLoaderLogger::GetPluginHookScanner();
		const char* sizeKey   = "WBP_Slot_C_SetSize";
		const char* memberKey = "WBP_Slot_C_SetSize::InSize";
		const auto ask = [&](const IPluginSelf* self, const char* key, const char* member, uint32_t flags, PluginFieldResult& result)
		{
			PluginFieldRequest request = PLUGIN_FIELD_REQUEST_INIT;
			request.key       = key;
			request.className = "WBP_Slot_C_SetSize";
			request.member    = member;
			request.flags     = flags;
			request.size      = member ? 0 : 8;
			request.builtCL   = 127004;
			return scan->ResolveField(self, &request, &result);
		};
		const uint32_t late = PLUGIN_FIELD_LATE, lateAny = PLUGIN_FIELD_LATE | PLUGIN_FIELD_ANY_SIZE;
		const auto declare = [&](const IPluginSelf* self, const char* id, std::vector<const char*> keys, bool* enabled)
		{
			static const char* cfg[] = { "Slot.*" };
			PluginFeatureDesc desc = PLUGIN_FEATURE_DESC_INIT;
			desc.id             = id;
			desc.title          = "Slot sizing";
			desc.keys           = keys.data();
			desc.keyCount       = static_cast<uint32_t>(keys.size());
			desc.configKeys     = cfg;
			desc.configKeyCount = 1;
			desc.enabled        = enabled;
			return scan->DeclareFeature(self, &desc);
		};
		const auto pendingOf = [](const char* plugin)
		{
			size_t n = 0;
			for (const PluginHookReport::PendingKey& p : PluginHookReport::Pending())
				if (p.plugin == plugin)
					++n;
			return n;
		};
		const auto reportOf = [](const char* plugin) -> bool
		{
			for (const PluginHookReport::PluginReport& r : PluginHookReport::Snapshot())
				if (r.plugin == plugin)
					return true;
			return false;
		};
		const auto sizeFinding = [](const char* plugin, const char* key)
		{
			for (const VersionMap::SizeFinding& f : VersionMap::SizeFindings())
				if (f.plugin == plugin && f.key == key)
					return f;
			VersionMap::SizeFinding none;
			none.key = "(no finding)";
			return none;
		};

		g_bpLoaded = false;
		PluginFieldResult result{};
		bool slot = true, refused = true;
		IPluginSelf bp{ "BpPlug", "1", nullptr, nullptr, nullptr };

		// At load: pending, the feature is off, the plugin loads, nothing is a failure.
		const unsigned serial = PluginHookReport::GetCommitSerial();
		TestDoubles::g_log.clear();
		PluginHookReport::BeginSession(&bp, "BpPlug", "BpPlug.dll");
		Check(declare(&bp, "slot", { sizeKey, memberKey }, &slot) && slot, "blueprint: feature declared");
		Check(!ask(&bp, sizeKey, nullptr, late, result) && !ask(&bp, memberKey, "InSize", lateAny, result),
			"blueprint: both requests are false while the class is not loaded");
		Check(!PluginHookReport::SessionHasFailures(&bp) && !slot, "blueprint: pending is no failure, and the feature's flag is off");
		PluginHookReport::EndSession(&bp, &refused);
		Check(!refused && !reportOf("BpPlug") && PluginHookReport::GetCommitSerial() == serial
			&& PluginHookReport::GetFeaturesOffCount("BpPlug") == 0, "blueprint: the plugin loads, and no failure report is added");
		Check(pendingOf("BpPlug") == 2, "blueprint: both keys are kept as pending");
		Check(Logged(L"[Map] BpPlug: WBP_Slot_C_SetSize: pending:") && Logged(L"feature \"Slot sizing\" is waiting for a game class")
			&& !Logged(L"refused"), "blueprint: the log says pending, once, and never refused");
		Check(PluginHookReport::ConfigOffReason("BpPlug", "Slot", "Scale") == "Slot sizing is waiting for the game to load something it needs",
			"blueprint: the config entries show the waiting reason");
		Check(PluginHookReport::BuildReportText().find("[pending] WBP_Slot_C_SetSize") != std::string::npos,
			"blueprint: the report text lists it apart from the failures");
		Check(sizeFinding("BpPlug", sizeKey).result == VersionMap::SizeResult::Pending
			&& std::string(VersionMap::SizeResultText(VersionMap::SizeResult::Pending)) == "pending",
			"blueprint: the compatibility report's size finding is pending");

		// The plugin repeats the request on the game thread: still pending, and no line again.
		TestDoubles::g_log.clear();
		Check(!ask(&bp, sizeKey, nullptr, late, result) && !ask(&bp, memberKey, "InSize", lateAny, result) && pendingOf("BpPlug") == 2
			&& TestDoubles::g_log.empty(), "blueprint: a repeated pending request changes nothing and logs nothing");

		// Without the flag the scanner is closed outside the session, and in it the miss is an ordinary one.
		Check(!ask(&bp, sizeKey, nullptr, 0, result), "blueprint: no flag, no session: rejected");
		IPluginSelf plain{ "PlainPlug", "1", nullptr, nullptr, nullptr };
		bool plainSlot = true;
		TestDoubles::g_log.clear();
		PluginHookReport::BeginSession(&plain, "PlainPlug", "PlainPlug.dll");
		declare(&plain, "slot", { sizeKey }, &plainSlot);
		Check(!ask(&plain, sizeKey, nullptr, 0, result), "blueprint: a request without the flag is refused while the class is not loaded");
		PluginHookReport::EndSession(&plain, &refused);
		Check(!refused && !plainSlot && reportOf("PlainPlug") && pendingOf("PlainPlug") == 0 && Logged(L"PlainPlug: WBP_Slot_C_SetSize: refused:"),
			"blueprint: it is a feature-off failure, not pending");

		// A key no feature lists cannot wait.
		IPluginSelf core{ "CoreBp", "1", nullptr, nullptr, nullptr };
		PluginHookReport::BeginSession(&core, "CoreBp", "CoreBp.dll");
		Check(!ask(&core, sizeKey, nullptr, late, result) && PluginHookReport::SessionHasFailures(&core),
			"blueprint: a pending key no feature lists refuses the plugin");
		PluginHookReport::EndSession(&core, &refused);
		Check(refused && pendingOf("CoreBp") == 0, "blueprint: and keeps nothing pending");

		// LATE is for a reflected request.
		VersionMap::FieldValue v;
		std::string e;
		Check(!Field(memberKey, "WBP_Slot_C_SetSize", "InSize", PLUGIN_FIELD_LATE | PLUGIN_FIELD_CHECK, 8, {}, v, e)
			&& e.find("bad request: PLUGIN_FIELD_LATE") == 0, "blueprint: LATE with CHECK is a bad request");

		// The class is loaded and the plugin repeats: resolved, once; the pending entries go.
		g_bpLoaded = true;
		TestDoubles::g_log.clear();
		Check(ask(&bp, sizeKey, nullptr, late, result) && result.size == 8, "blueprint: the block size resolves once the class is loaded");
		Check(ask(&bp, memberKey, "InSize", lateAny, result) && result.offset == 0 && result.size == 8, "blueprint: and the parameter");
		Check(pendingOf("BpPlug") == 0 && PluginHookReport::ConfigOffReason("BpPlug", "Slot", "Scale").empty(),
			"blueprint: nothing pending, the config entries are free");
		Check(sizeFinding("BpPlug", sizeKey).result == VersionMap::SizeResult::ReflectedOnly && sizeFinding("BpPlug", sizeKey).reflectedSize == 8,
			"blueprint: the size finding is replaced by the reflected one");
		TestDoubles::g_log.clear();
		Check(ask(&bp, sizeKey, nullptr, late, result) && !Logged(L"WBP_Slot_C_SetSize"), "blueprint: resolving again logs nothing");

		// A loaded class with a wrong parameter: refused for good, once, and not pending.
		IPluginSelf wrong{ "BpWrong", "1", nullptr, nullptr, nullptr };
		bool wrongSlot = true;
		TestDoubles::g_log.clear();
		PluginHookReport::BeginSession(&wrong, "BpWrong", "BpWrong.dll");
		declare(&wrong, "slot", { "WBP_Slot_C_SetSize::Nope" }, &wrongSlot);
		Check(!ask(&wrong, "WBP_Slot_C_SetSize::Nope", "Nope", lateAny, result) && !wrongSlot, "blueprint: an unknown parameter is refused");
		PluginHookReport::EndSession(&wrong, &refused);
		Check(!refused && reportOf("BpWrong") && pendingOf("BpWrong") == 0 && Logged(L"BpWrong: WBP_Slot_C_SetSize::Nope: refused: reflection: no parameter Nope"),
			"blueprint: a feature-off failure naming the parameter, not pending");
		TestDoubles::g_log.clear();
		Check(!ask(&wrong, "WBP_Slot_C_SetSize::Nope", "Nope", lateAny, result) && !Logged(L"refused"),
			"blueprint: repeating a refused request does not log it again");

		// Pending first, then the class loads and the parameter is wrong: the entry becomes a refusal.
		g_bpLoaded = false;
		IPluginSelf later{ "BpLater", "1", nullptr, nullptr, nullptr };
		bool laterSlot = true;
		PluginHookReport::BeginSession(&later, "BpLater", "BpLater.dll");
		declare(&later, "slot", { "WBP_Slot_C_SetSize::Nope" }, &laterSlot);
		ask(&later, "WBP_Slot_C_SetSize::Nope", "Nope", lateAny, result);
		PluginHookReport::EndSession(&later, &refused);
		Check(pendingOf("BpLater") == 1, "blueprint: pending before the class loads");
		g_bpLoaded = true;
		TestDoubles::g_log.clear();
		Check(!ask(&later, "WBP_Slot_C_SetSize::Nope", "Nope", lateAny, result), "blueprint: it fails once the class is loaded");
		const std::vector<PluginHookReport::PendingKey> now = PluginHookReport::Pending();
		bool marked = false;
		for (const PluginHookReport::PendingKey& p : now)
			if (p.plugin == "BpLater" && p.refused && p.detail.find("no parameter Nope") != std::string::npos)
				marked = true;
		Check(marked && PluginHookReport::ConfigOffReason("BpLater", "Slot", "X").find("is off: ") != std::string::npos
			&& Logged(L"BpLater: WBP_Slot_C_SetSize::Nope: refused:"), "blueprint: the pending entry becomes a refusal with its reason");
		TestDoubles::g_log.clear();
		ask(&later, "WBP_Slot_C_SetSize::Nope", "Nope", lateAny, result);
		Check(!Logged(L"refused"), "blueprint: and that is logged once");

		// A reload of the plugin drops what it was waiting for.
		PluginHookReport::BeginSession(&later, "BpLater", "BpLater.dll");
		Check(pendingOf("BpLater") == 0, "blueprint: a reload starts without the old pending keys");
		PluginHookReport::EndSession(&later, &refused);

		g_bpLoaded = false;
		VersionMap::SetLiveFieldSource(nullptr);
	}

	// Host rules as text (host_rules_wire.h): the format a server operator writes, how it parses, and which set a connection gets.
	{
		using namespace HostRulesWire;
		using HostRules::Contains;
		const auto parse = [](const std::string& text, RulesSet& out, std::string& err) { return ParseRulesText(text, out, err); };
		const auto status = [&](const std::string& text, size_t* line = nullptr)
		{
			RulesSet s;
			std::string err;
			const ParseResult r = parse(text, s, err);
			if (line)
				*line = r.line;
			return r.status;
		};

		// Empty text, blank lines and comments: an empty set that allows everything.
		{
			RulesSet s;
			s.message = "kept";
			std::string err = "stale";
			const ParseResult r = parse("", s, err);
			Check(r.status == ParseStatus::Ok && s.defaultAllow && s.message.empty() && s.deny.empty() && s.allow.empty() && err.empty()
				&& s.onDenied == kOnDeniedSwitchOff, "host rules text: empty text is an empty set that allows everything");
			s.message = "kept";
			Check(parse("\n  \n# a comment\n   ; another\r\n\t\r\n", s, err).status == ParseStatus::Ok && s.message.empty() && s.deny.empty(),
				"host rules text: blank lines and comment lines are skipped");
		}

		// A full file, with CRLF, a BOM, case and spaces around keys and values.
		{
			RulesSet s;
			std::string err;
			const std::string text = "\xEF\xBB\xBF# rules\r\nDefault = Allow\r\nMESSAGE=  Be kind # not a comment = ok  \r\n"
				"Alpha.fly=deny\r\n  alpha.Dig = DENY \r\nBeta.*=deny\r\nBeta.ok=allow\r\nGamma.a.b=deny";
			const ParseResult r = parse(text, s, err);
			Check(r.status == ParseStatus::Ok && err.empty() && s.defaultAllow && s.message == "Be kind # not a comment = ok",
				"host rules text: CRLF, a BOM, case and spaces are read; a message keeps '#' and '='");
			Check(s.deny.size() == 4 && Contains(s.deny, "ALPHA", "fly") && Contains(s.deny, "Alpha", "Dig") && !Contains(s.deny, "Alpha", "dig")
				&& Contains(s.deny, "Beta", "*") && Contains(s.deny, "Gamma", "a.b") && s.allow.size() == 1 && Contains(s.allow, "Beta", "ok"),
				"host rules text: rows split at the first dot, ids stay exact, the plugin ignores case, the last line needs no newline");
			RulesSet d;
			Check(parse("default=deny\n", d, err).status == ParseStatus::Ok && !d.defaultAllow, "host rules text: default=deny");
			Check(parse("default=ALLOW", d, err).status == ParseStatus::Ok && d.defaultAllow, "host rules text: default=allow in capitals");
			Check(parse("message=\n", d, err).status == ParseStatus::Ok && d.message.empty(), "host rules text: an empty message is fine");
			Check(parse("Alpha.*=allow", d, err).status == ParseStatus::Ok && d.allow.size() == 1 && Contains(d.allow, "alpha", "*") && d.deny.empty(),
				"host rules text: a whole-mod allow row");
			// It encodes and decodes to the same rules.
			RulesSet back;
			Check(DecodeRules(EncodeRules(s).data(), EncodeRules(s).size(), &back) == Status::Ok && back.message == s.message
				&& back.deny.size() == 4 && back.allow.size() == 1 && back.defaultAllow, "host rules text: a parsed set survives the wire");
		}

		// Every error status, with its line number and an error text that starts "line N:". The output stays as it was.
		{
			RulesSet keep;
			keep.defaultAllow = false;
			keep.message = "before";
			keep.deny = { { "Old", "x" } };
			const auto fails = [&](const std::string& text, ParseStatus want, size_t wantLine, const char* what)
			{
				RulesSet s = keep;
				std::string err;
				const ParseResult r = parse(text, s, err);
				const std::string prefix = wantLine ? "line " + std::to_string(wantLine) + ": " : std::string();
				Check(r.status == want && r.line == wantLine && !err.empty() && err.compare(0, prefix.size(), prefix) == 0
					&& !s.defaultAllow && s.message == "before" && s.deny.size() == 1 && s.allow.empty(), what);
			};
			fails("# ok\nno equals here\n", ParseStatus::BadLine, 2, "host rules text: a line with no '=' is BadLine");
			fails("\n\n.x=deny\n", ParseStatus::BadKey, 3, "host rules text: '.x' (no plugin) is BadKey");
			fails("P.=deny", ParseStatus::BadKey, 1, "host rules text: 'P.' (no id) is BadKey");
			fails("$P.x=deny", ParseStatus::BadKey, 1, "host rules text: a plugin name starting with '$' is BadKey");
			fails("nodot=deny", ParseStatus::BadKey, 1, "host rules text: a key with no dot is BadKey");
			fails("=deny", ParseStatus::BadKey, 1, "host rules text: an empty key is BadKey");
			fails("P." + std::string(kMaxIdLen + 1, 'i') + "=deny", ParseStatus::BadKey, 1, "host rules text: an id over the limit is BadKey");
			fails(std::string(kMaxPluginLen + 1, 'p') + ".x=deny", ParseStatus::BadKey, 1, "host rules text: a plugin over the limit is BadKey");
			fails("P.x=maybe", ParseStatus::BadValue, 1, "host rules text: a row value other than allow or deny is BadValue");
			fails("default=maybe", ParseStatus::BadValue, 1, "host rules text: a default other than allow or deny is BadValue");
			fails("P.x=", ParseStatus::BadValue, 1, "host rules text: an empty row value is BadValue");
			fails("default=allow\ndefault=deny", ParseStatus::Duplicate, 2, "host rules text: default twice is Duplicate");
			fails("message=a\n\nmessage=b", ParseStatus::Duplicate, 3, "host rules text: message twice is Duplicate");
			fails("message=" + std::string(kMaxMessageLen + 1, 'm'), ParseStatus::MessageTooLong, 1, "host rules text: a message of 201 bytes is MessageTooLong");
			fails("P.x=de\x01ny", ParseStatus::ControlChar, 1, "host rules text: a control character in a row is ControlChar");
			fails("\n\nmessage=a\tb", ParseStatus::ControlChar, 3, "host rules text: a tab inside a message is ControlChar");
			fails("message=a\rb", ParseStatus::ControlChar, 1, "host rules text: a bare CR inside a line is ControlChar");
			fails("Al\tpha.fly=deny", ParseStatus::ControlChar, 1, "host rules text: a tab inside a key is ControlChar");
			fails("Alpha.fly=de\tny", ParseStatus::ControlChar, 1, "host rules text: a tab inside a value is ControlChar");
			fails("Alpha.fly\x01", ParseStatus::ControlChar, 1, "host rules text: a control character in a line with no '=' is ControlChar");
			{
				RulesSet r;
				std::string e;
				Check(parse("Alpha.fly\t=\tdeny\n", r, e).status == ParseStatus::Ok && r.deny.size() == 1 && r.deny[0].plugin == "Alpha" && r.deny[0].id == "fly",
					"host rules text: tabs around the key and the value are blanks");
				RulesSet d;
				Check(parse("default\t=\tdeny\nmessage\t=\t  hello  \n", d, e).status == ParseStatus::Ok && !d.defaultAllow && d.message == "hello",
					"host rules text: tabs around default and message are blanks");
			}
			fails(std::string(kMaxPayloadBytes + 1, '#'), ParseStatus::TooLarge, 0, "host rules text: text over the payload limit is TooLarge");
			Check(status(std::string(kMaxPayloadBytes, '#')) == ParseStatus::Ok, "host rules text: text of exactly the payload limit is read");
		}

		// The message limit is 200 bytes, counted after the trim.
		{
			RulesSet s;
			std::string err;
			Check(parse("message=" + std::string(kMaxMessageLen, 'm') + "   \n", s, err).status == ParseStatus::Ok && s.message.size() == kMaxMessageLen,
				"host rules text: a message of 200 bytes is read");
			Check(status("message=" + std::string(kMaxMessageLen + 1, 'm')) == ParseStatus::MessageTooLong, "host rules text: 201 bytes is too long");
		}

		// The row limit is what one $ML:rules payload holds: kMaxRecords with the header record.
		{
			const auto rows = [](size_t n, const char* verb)
			{
				std::string t;
				for (size_t i = 0; i < n; ++i)
					t += "P.f" + std::to_string(i) + "=" + verb + "\n";
				return t;
			};
			RulesSet s;
			std::string err;
			size_t line = 0;
			Check(parse(rows(kMaxRecords - 1, "deny"), s, err).status == ParseStatus::Ok && s.deny.size() == kMaxRecords - 1,
				"host rules text: 2047 rows are read");
			Check(status(rows(kMaxRecords, "deny"), &line) == ParseStatus::TooManyRows && line == kMaxRecords,
				"host rules text: 2048 rows are TooManyRows, at the line that passes the limit");
			Check(status(rows(kMaxRecords, "allow")) == ParseStatus::TooManyRows, "host rules text: the limit counts allow rows too");
			Check(status(rows(kMaxRecords - 1, "deny") + "P.f0=deny\nP.f1=allow\n") == ParseStatus::Ok,
				"host rules text: a repeated key adds no row, so it does not pass the limit");
			EncodeStats stats;
			EncodeRules(s, &stats);
			Check(stats.dropped == 0 && stats.truncated == 0, "host rules text: the largest parsed set encodes without a cut");
		}

		// The same key twice: deny wins, in either order, and a repeat adds no row.
		{
			RulesSet s;
			std::string err;
			Check(parse("P.x=allow\nP.x=deny\n", s, err).status == ParseStatus::Ok && s.deny.size() == 1 && s.allow.empty(), "host rules text: allow then deny is deny");
			Check(parse("p.x=deny\nP.x=allow\n", s, err).status == ParseStatus::Ok && s.deny.size() == 1 && s.allow.empty(), "host rules text: deny then allow is deny");
			Check(parse("P.x=deny\nP.x=deny\nP.y=allow\nP.y=allow", s, err).status == ParseStatus::Ok && s.deny.size() == 1 && s.allow.size() == 1,
				"host rules text: a repeated row adds nothing");
			Check(parse("P.x=allow\nP.X=deny\n", s, err).status == ParseStatus::Ok && s.deny.size() == 1 && s.allow.size() == 1,
				"host rules text: ids that differ in case are two features");
		}

		// MergeConsole: the console's deny list is added, its note wins when it says something, the default stays the global's.
		{
			RulesSet g;
			g.defaultAllow = false;
			g.message = "global note";
			g.deny = { { "Alpha", "x" } };
			g.allow = { { "Beta", "*" } };
			RulesSet m = MergeConsole(g, { { "alpha", "x" }, { "Gamma", "z" } }, "");
			Check(!m.defaultAllow && m.message == "global note" && m.deny.size() == 2 && Contains(m.deny, "Alpha", "x") && Contains(m.deny, "Gamma", "z")
				&& m.allow.size() == 1 && Contains(m.allow, "Beta", "*"), "host rules merge: the console list joins the deny rows once, the global note stays when the console has none");
			m = MergeConsole(g, {}, "console note");
			Check(m.message == "console note" && m.deny.size() == 1, "host rules merge: a console note replaces the global one");
			m = MergeConsole(g, {}, "  \t ");
			Check(m.message == "global note", "host rules merge: a blank console note does not replace the global one");
			m = MergeConsole(RulesSet(), {}, "only console");
			Check(m.defaultAllow && m.message == "only console" && m.deny.empty(), "host rules merge: a console note alone is kept");
			Check(g.deny.size() == 1 && g.message == "global note", "host rules merge: the global set is not changed");
			m = MergeConsole(RulesSet(), { { "A", "x" }, { "A", "x" }, { "", "y" } }, "");
			Check(m.deny.size() == 1, "host rules merge: the console list is normalized");
		}

		// EffectiveRules: a connection's own set is used as it is; with none the global set and the console's list apply.
		{
			RulesSet g;
			g.message = "global note";
			g.deny = { { "Alpha", "x" } };
			RulesSet own;
			own.defaultAllow = false;
			own.message = "own note";
			own.deny = { { "Beta", "y" } };
			const HostRules::DenySet console = { { "Gamma", "z" } };
			RulesSet e = EffectiveRules(g, &own, console, "console note");
			Check(!e.defaultAllow && e.message == "own note" && e.deny.size() == 1 && Contains(e.deny, "Beta", "y"),
				"host rules effective: a connection's own set replaces the global one, its note and the console list");
			RulesSet none;
			e = EffectiveRules(g, &none, console, "console note");
			Check(e.defaultAllow && e.message.empty() && e.deny.empty(), "host rules effective: an own set that allows everything is not topped up");
			e = EffectiveRules(g, nullptr, console, "console note");
			Check(e.message == "console note" && e.deny.size() == 2 && Contains(e.deny, "Alpha", "x") && Contains(e.deny, "Gamma", "z"),
				"host rules effective: with no set of its own a connection gets the global set merged with the console");
		}

		// NothingToSend: allows everything, no rows and no note.
		{
			RulesSet s;
			Check(NothingToSend(s), "host rules send: an empty set has nothing to send");
			s.message = "  \t";
			Check(NothingToSend(s), "host rules send: a note of blanks says nothing");
			s.message = "note";
			Check(!NothingToSend(s), "host rules send: a note alone is sent");
			s.message.clear();
			s.defaultAllow = false;
			Check(!NothingToSend(s), "host rules send: default deny is sent");
			s.defaultAllow = true;
			s.deny = { { "A", "x" } };
			Check(!NothingToSend(s), "host rules send: a deny row is sent");
			s.deny.clear();
			s.allow = { { "A", "x" } };
			Check(NothingToSend(s), "host rules send: allow rows alone add nothing to an allow-all set");
		}
	}

	// Host rules: the server's list of features to keep off. The list is plain data (host_rules.h); applying it writes the
	// plugins' own `enabled` flags and puts a "turned off by this server" line in the report, and lifting it hands a flag
	// back only where the game build has the feature on.
	{
		VersionMap::SetLiveFieldSource(FakeLive);
		IPluginHookScanner* scan = ModLoaderLogger::GetPluginHookScanner();
		using HostRules::RuleKey;
		using Info = PluginHookReport::FeatureInfo;

		// The arithmetic: empty and repeated entries go, the plugin name ignores case, the id does not.
		{
			const HostRules::DenySet raw = { { "Alpha", "x" }, { "alpha", "x" }, { "Alpha", "" }, { "", "x" }, { "Alpha", "X" }, { "Beta", "y" } };
			const HostRules::DenySet norm = HostRules::Normalize(raw);
			Check(norm.size() == 3 && norm[0].plugin == "Alpha" && norm[0].id == "x" && norm[1].id == "X" && norm[2].plugin == "Beta",
				"host rules: normalize drops empty entries and repeats, and keeps ids that differ in case");
			Check(HostRules::Contains(norm, "ALPHA", "x") && !HostRules::Contains(norm, "Alpha", "Y") && !HostRules::Contains(norm, "Gamma", "x"),
				"host rules: the plugin name compares without case, the id exactly");
			const HostRules::DenyDiff diff = HostRules::DiffDeny({ { "Alpha", "x" }, { "Beta", "y" } }, { { "ALPHA", "x" }, { "Gamma", "z" } });
			Check(diff.added.size() == 1 && diff.added[0].plugin == "Gamma" && diff.removed.size() == 1 && diff.removed[0].plugin == "Beta"
				&& HostRules::DiffDeny(norm, norm).added.empty() && HostRules::DiffDeny(norm, norm).removed.empty(),
				"host rules: the diff names what was added and what was removed, and nothing for an equal list");
			Check(HostRules::EffectiveOn(true, false) && !HostRules::EffectiveOn(true, true) && !HostRules::EffectiveOn(false, false)
				&& !HostRules::EffectiveOn(false, true), "host rules: a deny turns a feature off and nothing turns one on");

			// The two sources: this machine's own list and the one a joined server sent.
			HostRules::DenySources src;
			src.local = { { "Alpha", "x" } };
			Check(HostRules::Contains(src.Effective(), "Alpha", "x") && src.Effective().size() == 1,
				"host rules: a local entry alone is denied");
			src.local.clear();
			src.remote = { { "Beta", "y" } };
			Check(HostRules::Contains(src.Effective(), "Beta", "y") && src.Effective().size() == 1,
				"host rules: a server entry alone is denied");
			src.local = { { "Beta", "y" }, { "Alpha", "x" } };
			Check(src.Effective().size() == 2 && HostRules::Contains(src.Effective(), "beta", "y"),
				"host rules: an entry on both lists counts once");
			src.remote.clear();
			Check(src.Effective().size() == 2 && HostRules::Contains(src.Effective(), "Alpha", "x"),
				"host rules: clearing the server's list leaves the local one");
			src.remote = { { "Gamma", "z" } };
			src.local.clear();
			Check(src.Effective().size() == 1 && HostRules::Contains(src.Effective(), "Gamma", "z"),
				"host rules: clearing the local list leaves the server's");
		}

		const auto resolve = [&](const IPluginSelf* self, const char* name, const char* pattern)
		{
			PluginScanRequest request = PLUGIN_SCAN_REQUEST_INIT;
			request.hookName = name;
			request.pattern  = pattern;
			request.kind     = PLUGIN_SCAN_ANY;
			return scan->Resolve(self, &request);
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
		const auto feature = [](const char* plugin, const char* id) -> Info
		{
			for (const Info& f : PluginHookReport::Features())
				if (f.plugin == plugin && f.id == id)
					return f;
			Info none;
			none.title = "(not in the catalog)";
			return none;
		};
		const auto stateOf = [&](const char* plugin, const char* id) { return feature(plugin, id).state; };
		const auto buildOffOf = [&](const char* plugin, const char* id) { return feature(plugin, id).buildOff; };
		const auto inCatalog = [&](const char* plugin, const char* id) { return feature(plugin, id).title != "(not in the catalog)"; };
		// How many lines of this plugin's report are the server's.
		const auto hostLines = [](const char* plugin)
		{
			int n = 0;
			for (const PluginHookReport::PluginReport& r : PluginHookReport::Snapshot())
				if (r.plugin == plugin)
					for (const PluginHookReport::FeatureOff& f : r.featuresOff)
						if (f.host)
							++n;
			return n;
		};
		const auto reportOf = [](const char* plugin) -> bool
		{
			for (const PluginHookReport::PluginReport& r : PluginHookReport::Snapshot())
				if (r.plugin == plugin)
					return true;
			return false;
		};
		const Info::State On = Info::State::On, OffBuild = Info::State::OffBuild, OffServer = Info::State::OffServer;

		// A plugin with two features that are on, one the build lost (a key misses), one absent on this build.
		IPluginSelf self{ "HostPlug", "1", nullptr, nullptr, nullptr };
		bool laser = false, hud = false, gone = true, absent = true, refused = true;
		PluginHookReport::ClearHostRules();
		PluginHookReport::BeginSession(&self, "HostPlug", "HostPlug.dll");
		declare(&self, "laser", "Mining laser", { "Test::Kept" }, { "Laser.*" }, &laser);
		declare(&self, "hud", "Heads-up display", { "Test::Kept" }, { "Hud.Show" }, &hud);
		declare(&self, "gone", "Gone feature", { "Test::Gone" }, { "Gone.*" }, &gone);
		declare(&self, "absent", "Absent feature", { "ATest::Gone" }, { "Absent.*" }, &absent);
		resolve(&self, "Test::Kept", "AE 09");
		resolve(&self, "Test::Gone", "ZZ 99");
		PluginHookReport::RecordAbsent(&self, "ATest::Gone", "not in this game version (CL-128786)");
		PluginHookReport::EndSession(&self, &refused);
		Check(!refused && laser && hud && !gone && !absent
			&& stateOf("HostPlug", "laser") == On && stateOf("HostPlug", "hud") == On
			&& stateOf("HostPlug", "gone") == OffBuild && stateOf("HostPlug", "absent") == OffBuild
			&& hostLines("HostPlug") == 0, "host rules: with no list the catalog shows what the build has on and off");
		Check(!buildOffOf("HostPlug", "laser") && !buildOffOf("HostPlug", "hud") && buildOffOf("HostPlug", "gone") && buildOffOf("HostPlug", "absent"),
			"host rules: buildOff is true for the features the build has off, whatever state says");

		// A deny flips the flag, and says so in the report, without opening the failure window.
		unsigned serial = PluginHookReport::GetCommitSerial();
		TestDoubles::g_log.clear();
		PluginHookReport::ApplyHostRules({ { "hostplug", "laser" } });
		Check(!laser && hud && stateOf("HostPlug", "laser") == OffServer && stateOf("HostPlug", "hud") == On
			&& feature("HostPlug", "laser").reason == "turned off by this server", "host rules: a deny writes the plugin's flag false");
		Check(!buildOffOf("HostPlug", "laser") && !buildOffOf("HostPlug", "hud"),
			"host rules: a server's deny leaves buildOff false for a feature the build has on");
		Check(hostLines("HostPlug") == 1 && PluginHookReport::GetCommitSerial() == serial
			&& PluginHookReport::ConfigOffReason("HostPlug", "Laser", "Scale") == "Mining laser is off: turned off by this server"
			&& PluginHookReport::ConfigOffReason("HostPlug", "Hud", "Show").empty()
			&& PluginHookReport::BuildReportText().find("[feature off] Mining laser (laser): turned off by this server") != std::string::npos,
			"host rules: the report and the greyed config entries say the server did it, and no popup is opened");
		Check(Logged(L"[Map] HostPlug: feature \"Mining laser\" turned off by this server"), "host rules: one log line when it goes off");
		const auto list = PluginHookReport::HostDeny();
		Check(list.size() == 1 && list[0].first == "hostplug" && list[0].second == "laser", "host rules: the list is kept as given");

		// The same list again changes nothing and logs nothing.
		TestDoubles::g_log.clear();
		PluginHookReport::ApplyHostRules({ { "HOSTPLUG", "laser" }, { "HostPlug", "laser" } });
		Check(hostLines("HostPlug") == 1 && !Logged(L"turned off by this server") && !laser, "host rules: the same list again is quiet");

		// A feature the build already has off is not touched by the list, and a lift does not turn it on.
		PluginHookReport::ApplyHostRules({ { "HostPlug", "laser" }, { "HostPlug", "gone" }, { "HostPlug", "absent" }, { "HostPlug", "nosuch" } });
		Check(!gone && !absent && stateOf("HostPlug", "gone") == OffBuild && stateOf("HostPlug", "absent") == OffBuild
			&& hostLines("HostPlug") == 1, "host rules: a deny on a feature the build has off adds no line and keeps the build's reason");
		TestDoubles::g_log.clear();
		PluginHookReport::ClearHostRules();
		Check(laser && hud && !gone && !absent && hostLines("HostPlug") == 0 && PluginHookReport::HostDeny().empty()
			&& stateOf("HostPlug", "laser") == On && stateOf("HostPlug", "gone") == OffBuild && stateOf("HostPlug", "absent") == OffBuild
			&& PluginHookReport::ConfigOffReason("HostPlug", "Laser", "Scale").empty() && PluginHookReport::GetCommitSerial() == serial,
			"host rules: a lift hands the flag back only where the build has the feature on");
		Check(Logged(L"[Map] HostPlug: feature \"Mining laser\" turned back on") && !Logged(L"\"Gone feature\" turned back on")
			&& !Logged(L"\"Absent feature\" turned back on"), "host rules: only the feature that came back is logged as back on");

		// Changing the list: one comes back, another goes off.
		PluginHookReport::ApplyHostRules({ { "HostPlug", "laser" } });
		PluginHookReport::ApplyHostRules({ { "HostPlug", "hud" } });
		Check(laser && !hud && stateOf("HostPlug", "laser") == On && stateOf("HostPlug", "hud") == OffServer && hostLines("HostPlug") == 1,
			"host rules: a new list lifts what it dropped and denies what it added");

		// The list outlives the plugin: it applies again when the plugin loads, and a reload keeps it.
		PluginHookReport::ForgetFeatures(&self);
		Check(!inCatalog("HostPlug", "hud") && hostLines("HostPlug") == 0 && PluginHookReport::HostDeny().size() == 1,
			"host rules: unloading drops the catalog entries and the server's lines, and keeps the list");
		laser = hud = true;
		PluginHookReport::BeginSession(&self, "HostPlug", "HostPlug.dll");
		declare(&self, "laser", "Mining laser", { "Test::Kept" }, { "Laser.*" }, &laser);
		declare(&self, "hud", "Heads-up display", { "Test::Kept" }, { "Hud.Show" }, &hud);
		resolve(&self, "Test::Kept", "AE 09");
		serial = PluginHookReport::GetCommitSerial();
		PluginHookReport::EndSession(&self, &refused);
		Check(!refused && laser && !hud && stateOf("HostPlug", "hud") == OffServer && stateOf("HostPlug", "laser") == On
			&& hostLines("HostPlug") == 1 && reportOf("HostPlug") && PluginHookReport::GetCommitSerial() == serial,
			"host rules: a plugin that loads after the list is set starts with the feature off, with no popup");

		// A reload that has no failure of its own leaves a report made only for the server's line; unloading removes it.
		PluginHookReport::ForgetFeatures(&self);
		Check(!reportOf("HostPlug") && !inCatalog("HostPlug", "laser"), "host rules: a report made only for the server's line goes with the plugin");

		// A plugin with a failure of its own keeps that report when it unloads; only the server's line goes.
		IPluginSelf mix{ "MixHost", "1", nullptr, nullptr, nullptr };
		bool mixA = true, mixB = true;
		PluginHookReport::ApplyHostRules({ { "MixHost", "a" } });
		PluginHookReport::BeginSession(&mix, "MixHost", "MixHost.dll");
		declare(&mix, "a", "Feature A", { "Test::Kept" }, {}, &mixA);
		declare(&mix, "b", "Feature B", { "Test::Gone" }, {}, &mixB);
		resolve(&mix, "Test::Kept", "AE 09");
		resolve(&mix, "Test::Gone", "ZZ 99");
		PluginHookReport::EndSession(&mix, &refused);
		Check(!refused && !mixA && !mixB && hostLines("MixHost") == 1 && PluginHookReport::GetFeaturesOffCount("MixHost") == 2
			&& stateOf("MixHost", "a") == OffServer && stateOf("MixHost", "b") == OffBuild, "host rules: the server's line sits beside the plugin's own failure");
		PluginHookReport::ForgetFeatures(&mix);
		Check(reportOf("MixHost") && hostLines("MixHost") == 0 && PluginHookReport::GetFeaturesOffCount("MixHost") == 1,
			"host rules: unloading leaves the plugin's own failure report in place");
		PluginHookReport::BeginSession(&mix, "MixHost", "MixHost.dll");   // a reload with nothing wrong clears the rest
		PluginHookReport::EndSession(&mix, &refused);
		PluginHookReport::ForgetFeatures(&mix);

		// A plugin that was refused never reaches the catalog, so the list cannot touch its flags.
		IPluginSelf core{ "RefusedHost", "1", nullptr, nullptr, nullptr };
		bool fine = true;
		PluginHookReport::ApplyHostRules({ { "RefusedHost", "fine" } });
		PluginHookReport::BeginSession(&core, "RefusedHost", "RefusedHost.dll");
		declare(&core, "fine", "Fine", { "Test::Kept" }, {}, &fine);
		resolve(&core, "Test::Kept", "AE 09");
		resolve(&core, "Test::CoreGone", "ZZ 99");
		PluginHookReport::EndSession(&core, &refused);
		Check(refused && fine && !inCatalog("RefusedHost", "fine") && hostLines("RefusedHost") == 0,
			"host rules: a refused plugin has no catalog entry and keeps its flag");
		PluginHookReport::ForgetFeatures(&core);

		// A feature that waits for a game class: the list does not touch it while it waits, and applies one tick after the
		// plugin's repeated request resolves it (the plugin writes its own flag as that call returns).
		{
			const char* sizeKey = "WBP_Slot_C_SetSize";
			const auto ask = [&](const IPluginSelf* who, PluginFieldResult& result)
			{
				PluginFieldRequest request = PLUGIN_FIELD_REQUEST_INIT;
				request.key       = sizeKey;
				request.className = "WBP_Slot_C_SetSize";
				request.flags     = PLUGIN_FIELD_LATE;
				request.size      = 8;
				request.builtCL   = 127004;
				return scan->ResolveField(who, &request, &result);
			};
			IPluginSelf late{ "LateHost", "1", nullptr, nullptr, nullptr };
			bool slot = true;
			PluginFieldResult result{};
			g_bpLoaded = false;
			PluginHookReport::ClearHostRules();
			PluginHookReport::ApplyHostRules({ { "LateHost", "slot" } });
			PluginHookReport::BeginSession(&late, "LateHost", "LateHost.dll");
			declare(&late, "slot", "Slot sizing", { sizeKey }, { "Slot.*" }, &slot);
			Check(!ask(&late, result) && !slot, "host rules: the feature waits for its class");
			PluginHookReport::EndSession(&late, &refused);
			Check(!refused && !slot && stateOf("LateHost", "slot") == OffBuild && hostLines("LateHost") == 0,
				"host rules: a feature that waits for a class is off for the build, not yet for the server");

			g_bpLoaded = true;
			GameThreadDispatch::Drain();
			Check(ask(&late, result), "host rules: the class is loaded and the request resolves");
			slot = true;   // what the plugin does when the call returns
			Check(slot, "host rules: until the next tick the plugin's flag is its own");
			GameThreadDispatch::Drain();
			Check(!slot && stateOf("LateHost", "slot") == OffServer && hostLines("LateHost") == 1,
				"host rules: one tick later the server's list switches it off again");

			PluginHookReport::ClearHostRules();
			Check(slot && stateOf("LateHost", "slot") == On && hostLines("LateHost") == 0, "host rules: and a lift brings it back");

			// With no list, a resolve queues nothing for the server's rules.
			IPluginSelf plain{ "LateNoList", "1", nullptr, nullptr, nullptr };
			bool plainSlot = true;
			g_bpLoaded = false;
			PluginHookReport::BeginSession(&plain, "LateNoList", "LateNoList.dll");
			declare(&plain, "slot", "Slot sizing", { sizeKey }, { "Slot.*" }, &plainSlot);
			ask(&plain, result);
			PluginHookReport::EndSession(&plain, &refused);
			g_bpLoaded = true;
			ask(&plain, result);
			plainSlot = true;
			GameThreadDispatch::Drain();
			Check(plainSlot && hostLines("LateNoList") == 0, "host rules: a resolve with no list leaves the flag alone");
			PluginHookReport::ForgetFeatures(&plain);
			PluginHookReport::ForgetFeatures(&late);
			g_bpLoaded = false;
		}

		// End of the block: nothing of ours is left for the tests after it (the catalog and the list are global).
		PluginHookReport::ClearHostRules();
		PluginHookReport::ForgetFeatures(&self);
		PluginHookReport::ForgetFeatures(&mix);
		PluginHookReport::ForgetFeatures(&core);
		Check(PluginHookReport::HostDeny().empty() && !inCatalog("HostPlug", "laser") && !inCatalog("MixHost", "a") && !reportOf("HostPlug"),
			"host rules: the block leaves no list and no catalog entry behind");
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
		// The list is the loader's SDK (127004) against the game, so a plugin whose SDK is the running
		// build (not 127004) keeps its built offset; one built for another build gets no offset as before.
		const VersionMap::Exe other{ 130045, 0, 0 };
		VersionMap::SetLayoutChanges({ "ATest", "/Script/Mod.ATest" });
		Check(Field("ATest::Plain", "ATest", "Plain", PLUGIN_FIELD_NATIVE, 4, {}, v, e, other, nullptr, 130045) && v.offset == 0,
			"field: a changed class's built offset is kept for a plugin whose SDK is the running build");
		Check(!Field("ATest::Plain", "ATest", "Plain", PLUGIN_FIELD_NATIVE, 4, {}, v, e, other, nullptr, 127004)
			&& e.find("a native member has no other source") != std::string::npos,
			"field: a plugin built for another build than the running one still gets no built offset");
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
		// A parameter of a UFunction (class <Class>_<Function>) is a reflected member: its live offset and size, any compiled size.
		Check(Field("Widget_Resize::ReturnValue", "Widget_Resize", "ReturnValue", PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e)
			&& v.offset == 0x18 && v.size == 8, "field: a function parameter resolves to its live offset and size");
		Check(Field("Widget_Resize", "Widget_Resize", "", 0, 0x10, {}, v, e) && v.size == 0x20 && v.offset == 0,
			"field: a function's parameter block size is its live ParmsSize, not the compiled one");
		Check(!Field("Widget_Resize::Nope", "Widget_Resize", "Nope", PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e)
			&& e == "reflection: no parameter Nope in Mod.Widget.Resize", "field: an unknown parameter is refused with the reflection's reason");
		Check(Field("FPlain::Block", "FPlain", "Block", PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e)
			&& v.offset == 0x120 && v.size == 0xB0, "field: a native member with the any-size flag is served from the build's own row, with the row's size");
		Check(!Field("FPlain::Block", "FPlain", "Block", PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e, VersionMap::Exe{ 130000, 0, 0 })
			&& e.find("no row for CL-130000") == 0, "field: a native any-size request is refused on a build with only another build's row");
		Check(!Field("FPlain::Block", "FPlain", "Block", PLUGIN_FIELD_NATIVE, 0x20, {}, v, e)
			&& e.find("gives size 0xB0, the code was compiled for 0x20") != std::string::npos, "field: a native member of an exact size is still refused on a mismatch");
		Check(!Field("ATest::Plain", "ATest", "Plain", PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e, built)
			&& e.find("no row for CL-127004") == 0, "field: a native any-size request has no built offset to fall back on");
		Check(!Field("ATest::Missing", "ATest", "Missing", PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e) && e == "reflection: no member Missing",
			"field: the any-size flag does not accept a member the game does not have");
		Check(Field("ATest::Renamed", "ATest", "NewName", 0, 4, { FieldRow(128786, 0x44, 4) }, v, e) && v.offset == 0x44,
			"field: an own row under the new name agrees with the map's rename row");
		Check(Field("ATest::Reflected", "ATest", "Reflected", 0, 8, {}, v, e) && v.offset == 0x30
			&& Logged(L"the loader map, CL-128786 row says 0x38/0x8, the game says 0x30/0x8; the game wins")
			&& Logged(L"overruled by reflection") && !LoggedDebug(L"another build"),
			"field: this build's own map row reflection disagrees with: the game wins, a warning");
		// A reflected field's own row against the map's: the game decides too, a native member's neither.
		Check(Field("ATest::Reflected", "ATest", "Reflected", 0, 8, { FieldRow(128786, 0x20, 8) }, v, e) && v.offset == 0x30
			&& Logged(L"the plugin's own says 0x20/0x8; the game's reflection decides"), "field: own row against the map's row, reflected");
		// The latest build's row stands in when the running one has none: it is checked against the game, and the game wins.
		const VersionMap::Exe later{ 130000, 0x33333333, 0x03000000 };
		// Another build's row is expected to differ, so it is a debug line and never a warning.
		Check(Field("ATest::Reflected", "ATest", "Reflected", 0, 8, {}, v, e, later) && v.offset == 0x30
			&& Logged(L"reflected (no row for CL-130000; the loader map, CL-128786 row does not)")
			&& LoggedDebug(L"the loader map, CL-128786 row differs (another build); it says 0x38/0x8, the game says 0x30/0x8; the game wins")
			&& !Logged(L"the game wins"),
			"field: no row for this build: the latest row is checked, the game wins, and it is logged at debug, not as a warning");
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

	// Struct sizes (member empty): a reflected struct's size comes live from the game's reflection, with its class-keyed
	// row only a cross-check; a struct the game does not reflect is served from the running build's own row, else
	// refused. A plugin that needs the compiled size to hold asks for PLUGIN_FIELD_CHECK. See "Struct sizes" in VersionMap.md.
	{
		VersionMap::SetLiveFieldSource(FakeLive);
		VersionMap::FieldValue v;
		std::string e;
		// How the plugin's size request for key came out, for the compatibility report.
		const auto finding = [](const char* key)
		{
			for (const VersionMap::SizeFinding& f : VersionMap::SizeFindings())
				if (f.plugin == "Test" && f.key == key)
					return f;
			VersionMap::SizeFinding none;
			none.key = "(no finding)";
			return none;
		};

		// Size 0 and no CHECK: the live size comes back, whatever the code was compiled for.
		Check(Field("AOther", "AOther", "", 0, 0, {}, v, e) && v.size == 0x10 && v.offset == 0 && v.boolMask == 0
			&& Logged(L"AOther: reflected (no row for CL-128786) (0x10)"), "size: size 0 takes the game's live size");
		{
			const VersionMap::SizeFinding f = finding("AOther");
			Check(f.result == VersionMap::SizeResult::ReflectedOnly && f.rowCL == 0 && f.rowSize == 0 && f.reflected && f.reflectedSize == 0x10,
				"size report: a reflected size with no row at all is reflected-only");
		}
		Check(Field("AOther", "AOther", "", 0, 0x10, {}, v, e) && v.size == 0x10, "size: the compiled size matches the game's: the live size");
		Check(Field("AOther", "AOther", "", 0, 0x20, {}, v, e) && v.size == 0x10 && !Logged(L"as compiled"),
			"size: without CHECK a different compiled size is not compared: the live size is returned");

		// CHECK compares the compiled size with the game's and refuses a difference.
		Check(Field("AOther", "AOther", "", PLUGIN_FIELD_CHECK, 0x10, {}, v, e) && v.size == 0x10
			&& Logged(L"AOther: reflected (no row for CL-128786), as compiled (0x10)"), "size: CHECK passes when the game's size is the compiled one");
		Check(!Field("AOther", "AOther", "", PLUGIN_FIELD_CHECK, 0x18, {}, v, e)
			&& e == "the code relies on 0x18, this build has 0x10"
			&& Logged(L"AOther: refused: the code relies on 0x18, this build has 0x10"), "size: CHECK refuses a game size that is not the compiled one");

		// NATIVE on a size request changes nothing: a reflected struct is still answered by the game. A bitfield flag is a
		// bad request, an unknown class an error.
		Check(Field("AOther", "AOther", "", PLUGIN_FIELD_NATIVE, 0x10, {}, v, e) && v.size == 0x10
			&& Logged(L"AOther: reflected (no row for CL-128786) (0x10)"), "size: NATIVE on a reflected size request is still answered by reflection");
		Check(!Field("AOther", "AOther", "", PLUGIN_FIELD_BITFIELD, 0, {}, v, e)
			&& e == "bad request: a field needs a class, and a size row is not a bitfield", "size: BITFIELD on a size request is a bad request");
		Check(!Field("AOther", "AOther", "", PLUGIN_FIELD_ANY_SIZE, 0, {}, v, e) && e.find("bad request: PLUGIN_FIELD_ANY_SIZE") == 0,
			"size: ANY_SIZE is for a member, not a size request");
		Check(!Field("ANone", "ANone", "", 0, 0, {}, v, e) && e == "ANone is not reflected, so its size needs a row for CL-128786 and has no other source",
			"size: a class the game does not have, and no row for it, is an error");
		Check(!Field("AUnready", "AUnready", "", 0, 0, {}, v, e) && e == "reflection: the game's reflection data is not ready",
			"size: reflection that is not ready is not an unreflected struct");
		Check(Field("AOther", "AOther", "", PLUGIN_FIELD_NATIVE | PLUGIN_FIELD_CHECK, 0x10, { FieldRow(128786, 0, 0x10) }, v, e)
			&& v.size == 0x10 && Logged(L"AOther: CL-128786 row, as reflected, as compiled (0x10)"),
			"size: an own row on a reflected size is a cross-check, whatever the flags");

		// An own row is only a cross-check too: the game's size is the answer.
		Check(Field("AOther", "AOther", "", 0, 0, { FieldRow(128786, 0, 0x30) }, v, e) && v.size == 0x10
			&& Logged(L"AOther: the CL-128786 row says 0x0/0x30, the game says 0x0/0x10; the game wins"), "size: an own row the game disagrees with: the game's size wins");

		// A class-keyed map row (no "::" in the key) is a cross-check as well: where it disagrees the game wins and the
		// difference is logged; where it agrees the pick says so.
		VersionMap::MapData classRows = VersionMap::BaselineMap();
		classRows.fields.push_back({ "AChild", 128786, 0, 0x50, 0, "" });
		classRows.fields.push_back({ "AOther", 128786, 0, 0x10, 0, "" });
		// Structs the game does not reflect: one with a row for the running build, one with only another build's, one with none.
		classRows.fields.push_back({ "FPlain", 128786, 0, 0x28, 0, "" });
		classRows.fields.push_back({ "FOld", 127004, 0, 0x30, 0, "" });
		classRows.fields.push_back({ "FZero", 128786, 0, 0, 0, "" });
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(classRows));
		Check(Field("AChild", "AChild", "", 0, 0, {}, v, e) && v.size == 0x58
			&& Logged(L"AChild: the loader map, CL-128786 row says 0x0/0x50, the game says 0x0/0x58; the game wins")
			&& Logged(L"overruled by reflection") && !LoggedDebug(L"another build"), "size: a class-keyed map row the game disagrees with: the game wins, logged");
		{
			const VersionMap::SizeFinding f = finding("AChild");
			Check(f.result == VersionMap::SizeResult::Mismatch && f.rowCL == 128786 && f.rowSize == 0x50 && f.reflected && f.reflectedSize == 0x58,
				"size report: an own-build row that differs from the game is a mismatch");
		}
		Check(Field("AOther", "AOther", "", 0, 0, {}, v, e) && v.size == 0x10 && !Logged(L"the game wins")
			&& Logged(L"loader map, CL-128786 row, as reflected (0x10)"), "size: a class-keyed map row the game agrees with");
		{
			const VersionMap::SizeFinding f = finding("AOther");
			Check(f.result == VersionMap::SizeResult::Match && f.rowCL == 128786 && f.rowSize == 0x10 && f.reflected && f.reflectedSize == 0x10,
				"size report: an own-build row that matches the game is a match");
		}
		// Another build's row that differs is expected to (a game update): the game wins, and it is only a debug line.
		const VersionMap::Exe laterBuild{ 130000, 0x33333333, 0x03000000 };
		Check(Field("AChild", "AChild", "", 0, 0, {}, v, e, laterBuild) && v.size == 0x58
			&& LoggedDebug(L"AChild: the loader map, CL-128786 row differs (another build); it says 0x0/0x50, the game says 0x0/0x58; the game wins")
			&& !Logged(L"the game wins"), "size: another build's row the game disagrees with: the game wins, at debug");
		{
			const VersionMap::SizeFinding f = finding("AChild");
			Check(f.result == VersionMap::SizeResult::Mismatch && f.rowCL == 128786 && f.rowSize == 0x50 && f.reflectedSize == 0x58,
				"size report: another build's row that differs is a mismatch too");
		}

		// A struct the game does not reflect has no live size: the running build's own row stands for it.
		Check(Field("FPlain", "FPlain", "", 0, 0, {}, v, e) && v.size == 0x28 && v.offset == 0 && v.boolMask == 0
			&& Logged(L"FPlain: loader map, CL-128786 row, unreflected type (0x28)"), "size: an unreflected struct is served from the running build's row");
		{
			const VersionMap::SizeFinding f = finding("FPlain");
			Check(f.result == VersionMap::SizeResult::RowOnly && f.rowCL == 128786 && f.rowSize == 0x28 && !f.reflected && f.reflectedSize == 0,
				"size report: an unreflected size served from its row is row-only");
		}
		Check(Field("FPlain", "FPlain", "", PLUGIN_FIELD_NATIVE, 0x28, {}, v, e) && v.size == 0x28,
			"size: NATIVE on an unreflected size request is served the same way");
		Check(Field("FPlain", "FPlain", "", PLUGIN_FIELD_CHECK, 0x28, {}, v, e) && v.size == 0x28 && Logged(L"unreflected type, as compiled (0x28)"),
			"size: CHECK passes when the row has the size the code was compiled for");
		Check(!Field("FPlain", "FPlain", "", PLUGIN_FIELD_CHECK, 0x30, {}, v, e) && e == "the code relies on 0x30, this build has 0x28",
			"size: CHECK refuses an unreflected struct whose row has another size");
		Check(Field("FOwn", "FOwn", "", 0, 0, { FieldRow(128786, 0, 0x18) }, v, e) && v.size == 0x18,
			"size: the plugin's own row for the running build serves an unreflected struct too");
		Check(!Field("FZero", "FZero", "", 0, 0, {}, v, e) && e == "loader map, CL-128786 row gives size 0", "size: a row of size 0 is not a size");
		// Another build's row is never used: nothing on this build can confirm the size is still that.
		Check(!Field("FOld", "FOld", "", 0, 0, {}, v, e)
			&& e == "FOld is not reflected, so its size needs a row for CL-128786 and has no other source; the loader map, CL-127004 row cannot be checked on this build, so it is not used"
			&& Logged(L"FOld: refused: FOld is not reflected"), "size: an unreflected struct with only another build's row is refused");
		{
			const VersionMap::SizeFinding f = finding("FOld");
			Check(f.result == VersionMap::SizeResult::NoRow && f.rowCL == 127004 && f.rowSize == 0x30 && !f.reflected,
				"size report: an unreflected size with only another build's row is no-row, naming that row");
		}
		Check(Field("FOld", "FOld", "", 0, 0, {}, v, e, VersionMap::Exe{ 127004, 0, 0 }) && v.size == 0x30,
			"size: ... but on the build that row belongs to it is the running build's own row");
		Check(!Field("FNone", "FNone", "", 0, 0, {}, v, e)
			&& e == "FNone is not reflected, so its size needs a row for CL-128786 and has no other source",
			"size: an unreflected struct with no row is refused");
		{
			const VersionMap::SizeFinding f = finding("FNone");
			Check(f.result == VersionMap::SizeResult::NoRow && f.rowCL == 0 && f.rowSize == 0 && !f.reflected,
				"size report: an unreflected size with no row at all is no-row");
		}
		Check(!Field("FNone", "FNone", "", PLUGIN_FIELD_CHECK, 0x10, {}, v, e, laterBuild) && e.find("needs a row for CL-130000") != std::string::npos,
			"size: an unreflected struct on a build with no row is refused, CHECK or not");
		Check(std::string(VersionMap::SizeResultText(VersionMap::SizeResult::Match)) == "match"
			&& std::string(VersionMap::SizeResultText(VersionMap::SizeResult::Mismatch)) == "mismatch"
			&& std::string(VersionMap::SizeResultText(VersionMap::SizeResult::RowOnly)) == "row-only"
			&& std::string(VersionMap::SizeResultText(VersionMap::SizeResult::NoRow)) == "no-row"
			&& std::string(VersionMap::SizeResultText(VersionMap::SizeResult::ReflectedOnly)) == "reflected-only", "size report: the result texts");
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

	// The client's map serves every exe (its lookups pick the row by CL), so the Game-SDK globals are
	// resolved and handed out on any of them.
	Check(VersionMap::ServesExe(VersionMap::Exe{ 127004, 0xE8972298, 0x0F35C000 }) && VersionMap::ServesExe(VersionMap::Exe{ 128786, 1, 2 })
		&& VersionMap::ServesExe(VersionMap::Exe{}), "ServesExe: true on every exe in the client build");

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

		// Classes the layout check found changed: only a plugin that takes every layout from the map runs,
		// or one that declares the build that is running (its layouts are that build's).
		const auto layoutRefusalOn = [&](uint32_t runningCL, int version, uint32_t builtCL, uint32_t flags)
		{
			return VersionMap::LayoutRefusal(VersionMap::Exe{ runningCL, 0, 0 }, version, VersionMap::ReadPluginGameBuild(info(version, builtCL, flags)));
		};
		const auto layoutRefusal = [&](int version, uint32_t builtCL, uint32_t flags) { return layoutRefusalOn(128786, version, builtCL, flags); };
		Check(layoutRefusal(71, 0, 0).empty() && layoutRefusal(72, 127004, 0).empty(), "layout: nothing changed, nothing refused");
		VersionMap::SetLayoutChanges({ "ACrX", "/Script/Chimera.ACrX", "FCrY", "/Script/Chimera.FCrY", "UCrZ", "/Script/Chimera.UCrZ" });
		const std::string notMigrated = layoutRefusal(72, 127004, 0);
		Check(notMigrated.find("Needs an update") == 0 && notMigrated.find("ACrX, FCrY and 1 more") != std::string::npos
			&& notMigrated.find("PLUGIN_MAP_MIGRATED") != std::string::npos && notMigrated.find("/Script/") == std::string::npos,
			"layout: a plugin without PLUGIN_MAP_MIGRATED is refused, the classes named");
		Check(layoutRefusal(71, 0, 0).find("before the version map") != std::string::npos, "layout: a v71 plugin is refused");
		Check(layoutRefusal(72, 0, kM).find("names no game build") != std::string::npos, "layout: MIGRATED with no build is refused");
		Check(layoutRefusal(72, 127004, kM).empty(), "layout: a MIGRATED plugin with a build is admitted");
		// A plugin that declares the running build, without the flag: its compiled layouts are that build's.
		Check(layoutRefusalOn(130045, 72, 130045, 0).empty() && layoutRefusalOn(130045, 78, 130045, 0).empty(),
			"layout: a plugin declaring the running build is admitted without the flag");
		Check(layoutRefusalOn(130045, 72, 130045, kM).empty(), "layout: declaring the running build and MIGRATED is admitted");
		Check(layoutRefusalOn(130045, 72, 128786, 0).find("PLUGIN_MAP_MIGRATED") != std::string::npos,
			"layout: a plugin declaring another build than the running one is still refused");
		Check(layoutRefusalOn(130045, 72, 128786, kM).empty(), "layout: another declared build with MIGRATED is admitted, as before");
		// Undeclared counts as built on 127004, but declares nothing, so running 127004 does not admit it.
		Check(layoutRefusalOn(127004, 72, 0, 0).find("PLUGIN_MAP_MIGRATED") != std::string::npos
			&& layoutRefusalOn(127004, 71, 0, 0).find("before the version map") != std::string::npos,
			"layout: an undeclared plugin is still refused, even where its judged build is the running one");
		// On the loader's own build (127004) the list and the plugin's SDK are the same headers: no exemption.
		Check(layoutRefusalOn(127004, 72, 127004, 0).find("PLUGIN_MAP_MIGRATED") != std::string::npos,
			"layout: declaring the loader's own build, where a class changed, is still refused");
		Check(layoutRefusalOn(130045, 72, 0, kM).find("names no game build") != std::string::npos, "layout: MIGRATED with no build is still refused");
		VersionMap::SetLayoutChanges({});
		Check(layoutRefusal(72, 127004, 0).empty(), "layout: cleared, admitted again");
		Check(layoutRefusalOn(130045, 72, 130045, 0).empty() && layoutRefusalOn(130045, 72, 128786, 0).empty(), "layout: nothing changed, nothing refused, declared or not");

		// A zero tail: judged as built on 127004, so refused on 128786; with the flag, refused everywhere.
		Check(refusal(127004, 72, 0, 0, false).empty(), "admission: zero tail loads on 127004");
		const std::string zero = refusal(128786, 72, 0, 0, true);
		Check(zero.find("Needs an update") == 0 && zero.find("names no game build") != std::string::npos && zero.find("It loads on CL-127004") != std::string::npos,
			"admission: zero tail is refused on 128786");
		Check(!refusal(127004, 72, 0, kM, true).empty() && !refusal(128786, 72, 0, kM, true).empty(), "admission: zero tail with MIGRATED is refused");
	}

	// Every released plugin class on a running 127004 and 130045 (also compiled
	// into PluginClassTests, where the loader is made for 130045).
	PluginClassMatrix::Run([](bool ok, const char* what) { Check(ok, what); });

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
		bad(head + "format\t1\n", "feed: a second format line is not skipped");
		bad(head + "seq\t1\n", "feed: a seq line in the body is not skipped");
		bad(head + "sig\tk1\tAAAA\n", "feed: a sig line in the body is not skipped");

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
		// The controller's cinematic bits share one byte at 0x4D0 on every build: bit 1 bCinematicMode, bit 2
		// bHidePawnInCinematicMode and bit 4 bPlayerIsWaiting, one row each with the bit's mask.
		{
			struct BitKey { const char* key; uint8_t mask; };
			const BitKey kBits[] = {
				{ "APlayerController::bCinematicMode", 0x02 }, { "APlayerController::bHidePawnInCinematicMode", 0x04 },
				{ "APlayerController::bPlayerIsWaiting", 0x10 },
			};
			bool bitRows = true;
			for (const BitKey& b : kBits)
				for (const uint32_t cl : { 127004u, 128786u, 129402u, 130045u })
				{
					const Real::VersionMap::FieldRow* row = fieldRow(b.key, cl);
					bitRows = bitRows && row && row->offset == 0x4D0 && row->size == 0x1 && row->boolMask == b.mask;
				}
			Check(bitRows, "real field rows: the controller's three cinematic bits are 0x4D0 byte rows with their masks on all four builds");
		}
		// The hotbar subsystem exists only on CL-129402: a row there, none on the older builds.
		const Real::VersionMap::FieldRow* index = fieldRow("UCrHotbarLocalPlayerSubsystem::CurrentHotbarIndex", 129402);
		const Real::VersionMap::FieldRow* pages = fieldRow("UCrHotbarLocalPlayerSubsystem::Hotbars", 129402);
		Check(index && index->offset == 0x48 && index->size == 0x1 && index->boolMask == 0, "real field rows: CurrentHotbarIndex is a byte at 0x48 on CL-129402");
		Check(pages && pages->offset == 0x38 && pages->size == 0x10 && pages->boolMask == 0, "real field rows: Hotbars is a 16-byte array at 0x38 on CL-129402");
		Check(!fieldRow("UCrHotbarLocalPlayerSubsystem::CurrentHotbarIndex", 127004) && !fieldRow("UCrHotbarLocalPlayerSubsystem::Hotbars", 127004),
			"real field rows: the hotbar subsystem has no CL-127004 row");
		// ...and the map says so: both members are absent on CL-127004, so nothing warns there, and not on CL-129402.
		const auto absentOn = [](const char* key, uint32_t cl)
		{
			for (const Real::VersionMap::AbsentField& a : Real::VersionMap::kAbsentFields)
				if (strcmp(a.key, key) == 0 && a.gameCL == cl)
					return true;
			return false;
		};
		Check(absentOn("UCrHotbarLocalPlayerSubsystem::CurrentHotbarIndex", 127004) && absentOn("UCrHotbarLocalPlayerSubsystem::Hotbars", 127004)
			&& !absentOn("UCrHotbarLocalPlayerSubsystem::CurrentHotbarIndex", 129402) && !absentOn("UCrHotbarLocalPlayerSubsystem::Hotbars", 129402),
			"real field rows: the hotbar members are absent on CL-127004 only");
		Check(absentOn("FHotbarInternalSlotData::SlotType", 127004) && absentOn("FHotbarArray::SlotsArray", 127004)
			&& !absentOn("FHotbarInternalSlotData::SlotType", 129402) && !absentOn("FHotbarArray::SlotsArray", 129402)
			&& !fieldRow("FHotbarInternalSlotData::SlotType", 129402) && !fieldRow("FHotbarArray::SlotsArray", 129402),
			"real field rows: the slot members are absent on CL-127004, and reflected (no row) on CL-129402");
		// The members of the engine texture chain the icon copy reads, one row per live build, measured the same
		// on CL-127004 and CL-129402 and absent from CL-128786.
				for (const ChainRow& m : kChain)
		{
			for (const uint32_t cl : { 127004u, 129402u })
			{
				const Real::VersionMap::FieldRow* row = fieldRow(m.key, cl);
				Check(row && row->offset == m.offset && row->size == m.size && row->boolMask == 0,
					(std::string("real field rows: ") + m.key + " on CL-" + std::to_string(cl)).c_str());
			}
			Check(!fieldRow(m.key, 128786), (std::string("real field rows: ") + m.key + " has no CL-128786 row").c_str());
		}
	}
	// The loader's lookup of those rows (NativeFieldOffset): a native member, with no compiled offset, so only a row
	// for the running build serves it. Served on CL-127004 and CL-129402;
	// refused on any other build, which turns the library off there and sends old icon copies to their compiled values.
	{
		VersionMap::SetLiveFieldSource(FakeLive);
		const auto overlayLookup = [](const char* key, uint32_t size, uint32_t cl, VersionMap::FieldValue& v, std::string& e)
		{
			VersionMap::FieldRequest request = VersionMap::LoaderFieldRequest(key, size, -1);
			request.flags   = PLUGIN_FIELD_NATIVE;
			request.builtCL = 0;
			for (const Real::VersionMap::Index& index : Real::VersionMap::kFields)
				if (strcmp(index.key, key) == 0)
					for (uint16_t i = index.first; i < index.first + index.count; ++i)
						request.candidates.push_back(FieldRow(Real::VersionMap::kFieldRows[i].gameCL,
							Real::VersionMap::kFieldRows[i].offset, Real::VersionMap::kFieldRows[i].size));
			e.clear();
			TestDoubles::g_log.clear();
			return VersionMap::SelectField("ModLoader", request, VersionMap::Exe{ cl, 0, 0 }, v, e);
		};
		VersionMap::FieldValue v;
		std::string e;
		for (const ChainRow& m : kChain)
		{
			// UTexture is a reflected class, so its member is bounded by it; the others are plain C++ types,
			// FTexture among them although the game's class UTexture carries the name Texture.
			const bool reflected = strncmp(m.key, "UTexture::", 10) == 0;
			for (const uint32_t cl : { 127004u, 129402u })
			{
				const bool served = overlayLookup(m.key, m.size, cl, v, e) && v.offset == m.offset && v.size == m.size;
				Check(served, (std::string("chain rows: ") + m.key + " served on CL-" + std::to_string(cl)
					+ (served ? std::string() : ", refused: " + e)).c_str());
				Check(Logged(L"unreflected type, no class bound") != reflected,
					(std::string("chain rows: ") + m.key + (reflected ? " is bounded by its class" : " is served with no class bound")
					 + " on CL-" + std::to_string(cl)).c_str());
			}
			Check(!overlayLookup(m.key, m.size, 128786, v, e) && e.find("no row for CL-128786, and a native member has no other source") == 0,
				(std::string("chain rows: ") + m.key + " refused on CL-128786, no compiled fallback").c_str());
		}
		VersionMap::SetLiveFieldSource(nullptr);
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
		oldRow = functionRow("ACrPlayerControllerBase::NativeOnInputCloseInGameMenu", 127004);
		newRow = functionRow("ACrPlayerControllerBase::NativeOnInputCloseInGameMenu", 129402);
		const Real::VersionMap::FunctionRow* midRow = functionRow("ACrPlayerControllerBase::NativeOnInputCloseInGameMenu", 128786);
		Check(oldRow && newRow && midRow && oldRow->rva == 0x07606ED0 && midRow->rva == 0x076520F0 && newRow->rva == 0x07652E60
			&& strcmp(oldRow->pattern, newRow->pattern) == 0 && strcmp(midRow->pattern, newRow->pattern) == 0,
			"real function rows: CloseInGameMenu has one pattern and a row on CL-127004, CL-128786 and CL-129402");
		oldRow = functionRow("UCrBuildingComponent::CanRestoreInitialState", 127004);
		newRow = functionRow("UCrBuildingComponent::CanRestoreInitialState", 129402);
		midRow = functionRow("UCrBuildingComponent::CanRestoreInitialState", 128786);
		Check(isStartRow(oldRow, 0x07447230) && isStartRow(midRow, 0x0747B5E0) && isStartRow(newRow, 0x0747BF40)
			&& strcmp(oldRow->pattern, newRow->pattern) == 0 && strcmp(midRow->pattern, newRow->pattern) == 0,
			"real function rows: CanRestoreInitialState has one pattern and a row on CL-127004, CL-128786 and CL-129402");
		oldRow = functionRow("ACrCharacterPlayerBase::DeactivateBuildingDrone", 127004);
		newRow = functionRow("ACrCharacterPlayerBase::DeactivateBuildingDrone", 129402);
		midRow = functionRow("ACrCharacterPlayerBase::DeactivateBuildingDrone", 128786);
		Check(isStartRow(oldRow, 0x07568CF0) && isStartRow(midRow, 0x075B2F30) && isStartRow(newRow, 0x075B3CA0)
			&& strcmp(oldRow->pattern, newRow->pattern) == 0 && strcmp(midRow->pattern, newRow->pattern) == 0,
			"real function rows: DeactivateBuildingDrone has one pattern and a row on CL-127004, CL-128786 and CL-129402");
		oldRow = functionRow("UCrBuildingStabilitySubsystem::OnCreateBuildingEntity", 128786);
		newRow = functionRow("UCrBuildingStabilitySubsystem::OnDestroyBuildingEntity", 128786);
		Check(oldRow && newRow && oldRow->rva == 0x074E7BF0 && newRow->rva == 0x074E7C30 && strcmp(oldRow->pattern, newRow->pattern) != 0,
			"real function rows: building add and remove are two functions with two patterns on CL-128786");
		oldRow = functionRow("UCrBuildingStabilitySubsystem::OnCreateBuildingEntity", 127004);
		newRow = functionRow("UCrBuildingStabilitySubsystem::OnDestroyBuildingEntity", 127004);
		Check(oldRow && newRow && oldRow->rva == 0x07469170 && newRow->rva == 0x074691B0 && strcmp(oldRow->pattern, newRow->pattern) != 0,
			"real function rows: building add and remove are two functions with two patterns on CL-127004 too");
		oldRow = functionRow("UCrBuildingStabilitySubsystem::OnCreateBuildingEntity", 130045);
		newRow = functionRow("UCrBuildingStabilitySubsystem::OnDestroyBuildingEntity", 130045);
		Check(isStartRow(oldRow, 0x074E86D0) && isStartRow(newRow, 0x074E8710) && strcmp(oldRow->pattern, newRow->pattern) != 0,
			"real function rows: building add and remove have a measured row on CL-130045 in the compiled table");
		oldRow = functionRow("FAuAPSplineActorPayloadData::StaticStruct", 130045);
		Check(oldRow && !oldRow->absent && oldRow->rva == 0x0668E770 && !oldRow->pattern,
			"real function rows: the spline payload StaticStruct has an address-only row on CL-130045");
		oldRow = functionRow("UNetConnection::RemoteAddressToString", 129402);
		newRow = functionRow("UNetConnection::RemoteAddressToString", 130045);
		Check(isStartRow(oldRow, 0x0448F900) && isStartRow(newRow, 0x0448F280)
			&& strcmp(oldRow->pattern, ScanPatterns::UNetConnection_RemoteAddressToString) == 0 && strcmp(newRow->pattern, ScanPatterns::UNetConnection_RemoteAddressToString) == 0,
			"real function rows: RemoteAddressToString has the one pattern and a row on CL-129402 and CL-130045");
		// UObject::ReleaseRef is the engine's reference release that pairs with UObject::AddRef: a global key, so any
		// plugin may ask for it by name. Its entry is the target of the guard wrapper MiniMap's site row points at,
		// not that row. Both keys have one function-start row on each build, and ReleaseRef's pattern is its own.
		{
			struct RefKey { uint32_t cl, addRef, releaseRef; };
			const RefKey refKeys[] = {
				{ 127004, 0x016205F0, 0x0168F6F0 }, { 128786, 0x01626B90, 0x01695C90 },
				{ 129402, 0x016267B0, 0x016958B0 }, { 130045, 0x016262B0, 0x016953B0 },
			};
			bool refRows = true;
			for (const RefKey& k : refKeys)
			{
				oldRow = functionRow("UObject::AddRef", k.cl);
				newRow = functionRow("UObject::ReleaseRef", k.cl);
				refRows = refRows && isStartRow(oldRow, k.addRef) && isStartRow(newRow, k.releaseRef) && strcmp(oldRow->pattern, newRow->pattern) != 0;
			}
			Check(refRows, "real function rows: UObject::AddRef and UObject::ReleaseRef each have a function-start row on all four builds");
		}
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

	// Best Builder's heat-gate site: the rows for the builds with an exe on disk carry the fingerprint of the
	// plugin's own PluginMap::Site request (kind IN_FUNCTION, resultOffset 43, no follow), and the key resolves to
	// each row on its build.
	{
		const char* const key = "Best Builder/ACrPlayerControllerBase::FindDeconstructibleTarget.HeatGate";
		const char* const pattern =
			"0F 2F 30 0F 82 ?? ?? ?? ?? 48 8B 9D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B C0 48 8B D3 48 8B CE "
			"E8 ?? ?? ?? ?? 48 85 C0 74 05 0F 2F 30 72 ?? 48 8B 07 48 8B CF FF 90 00 08 00 00";
		const uint32_t expectedFp = VersionMap::Fingerprint(static_cast<int>(Kind::InFunction), pattern, 43, -1, -1, 0);
		struct HeatGate { uint32_t cl, rva; };
		const HeatGate expected[] = { { 127004, 0x075F3100 }, { 128786, 0x076461A0 }, { 129402, 0x07646F10 }, { 130045, 0x07648FC0 } };
		VersionMap::MapData data;
		int found = 0;
		for (const Real::VersionMap::Index& index : Real::VersionMap::kFunctions)
			if (strcmp(index.key, key) == 0)
				for (uint16_t i = index.first; i < index.first + index.count; ++i)
				{
					const Real::VersionMap::FunctionRow& row = Real::VersionMap::kFunctionRows[i];
					++found;
					VersionMap::MapFunctionRow f;
					f.key = key; f.gameCL = row.gameCL; f.imageStamp = row.imageStamp; f.imageSize = row.imageSize;
					f.kind = row.kind; f.fp = row.fp; f.absent = row.absent; f.pattern = row.pattern;
					f.resultOffset = row.resultOffset; f.followRel32At = row.followRel32At; f.followRipDispAt = row.followRipDispAt;
					f.rva = row.rva;
					data.functions.push_back(f);
					data.builds.push_back({ row.gameCL, row.imageStamp, row.imageSize, "x" });
					Check(row.kind == static_cast<int>(Kind::InFunction) && row.fp == expectedFp && !row.absent
						&& strcmp(row.pattern, pattern) == 0 && row.resultOffset == 43 && row.followRel32At == -1 && row.followRipDispAt == -1,
						"heat gate row: kind, fingerprint, pattern and offset are the plugin's request");
				}
		Check(found == 4, "heat gate: one row each for CL-127004, CL-128786, CL-129402 and CL-130045");
		VersionMap::SetActiveMap(std::make_shared<const VersionMap::MapData>(data));
		for (const HeatGate& e : expected)
		{
			const VersionMap::MapFunctionRow* mapRow = nullptr;
			for (const VersionMap::MapFunctionRow& f : data.functions)
				if (f.gameCL == e.cl)
					mapRow = &f;
			Check(mapRow && mapRow->rva == e.rva, "heat gate: the row's rva is the pattern match plus 43");
			if (!mapRow)
				continue;
			TestDoubles::g_rvas[pattern] = e.rva - 43;
			Request request = Ask("ACrPlayerControllerBase::FindDeconstructibleTarget.HeatGate", Kind::InFunction, pattern);
			request.resultOffset = 43;
			TestDoubles::g_log.clear();
			const Result r = VersionMap::ResolveMapKey("Best Builder", request, {}, VersionMap::Exe{ e.cl, mapRow->imageStamp, mapRow->imageSize }, false);
			Check(At(r, e.rva) && Logged(L"loader map, CL-"), "heat gate: the key resolves to the row on its build");
			TestDoubles::g_rvas.erase(pattern);
		}
		VersionMap::SetActiveMap(nullptr);
	}

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

	// The same window after a fresh install: only the loader, no previous version.
	{
		using UI::UpdateNoticeWindow::IsFreshLoaderInstall;
		using UI::UpdateNoticeWindow::kLoaderRowName;
		Check(IsFreshLoaderInstall(1, kLoaderRowName, ""), "fresh install: the loader alone with no previous version");
		Check(!IsFreshLoaderInstall(1, kLoaderRowName, "v1.23.1"), "fresh install: a known previous version is an update");
		Check(!IsFreshLoaderInstall(2, kLoaderRowName, ""), "fresh install: a second row keeps the plugins title");
		Check(!IsFreshLoaderInstall(1, "NewPlugin", ""), "fresh install: a plugin's first install keeps the plugins title");
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

	// Unknown builds: the decision, the session id, the crash guard in modloader.ini and the text the player
	// reads. Last in main(): Arm sets process-wide state, and every check above runs with Active() false.
	{
		using UnknownBuild::Decision;

		const auto decide = [](uint32_t cl, bool enabled, bool retry, const wchar_t* last, const wchar_t* now)
		{
			UnknownBuild::Inputs in;
			in.runningCL = cl; in.enabled = enabled; in.retry = retry; in.lastSession = last; in.thisSession = now;
			return UnknownBuild::Decide(in);
		};
		Check(decide(0, false, false, L"s", L"s") == Decision::NoCL, "unknown build: no CL in the version is NoCL, even when switched off");
		Check(decide(130045, false, false, L"", L"s") == Decision::SwitchedOff, "unknown build: Enabled=0 is SwitchedOff");
		Check(decide(130045, false, true, L"s", L"s") == Decision::SwitchedOff, "unknown build: Enabled=0 wins over a guarded session");
		Check(decide(130045, true, false, L"s", L"s") == Decision::Guarded, "unknown build: the same session left open is Guarded");
		Check(decide(130045, true, true, L"s", L"s") == Decision::Fallback, "unknown build: Retry=1 turns Guarded into Fallback");
		Check(decide(130045, true, false, L"", L"s") == Decision::Fallback, "unknown build: no marker is Fallback");
		Check(decide(130045, true, false, L"t", L"s") == Decision::Fallback, "unknown build: another session's marker is Fallback");

		const std::wstring id = UnknownBuild::SessionId(130045, 0x1A2B3C4D, 0x0F4A3000, "1.25.1");
		Check(id == L"130045:1A2B3C4D:F4A3000:1.25.1", "unknown build: the session id");
		Check(id != UnknownBuild::SessionId(130045, 0x1A2B3C4E, 0x0F4A3000, "1.25.1"), "unknown build: a new image stamp is a new session id");
		Check(id != UnknownBuild::SessionId(130045, 0x1A2B3C4D, 0x0F4A3001, "1.25.1"), "unknown build: a new image size is a new session id");
		Check(id != UnknownBuild::SessionId(130045, 0x1A2B3C4D, 0x0F4A3000, "1.25.2"), "unknown build: a new loader build is a new session id");
		Check(id != UnknownBuild::SessionId(130046, 0x1A2B3C4D, 0x0F4A3000, "1.25.1"), "unknown build: a new CL is a new session id");

		Check(UnknownBuild::NewestKnown({ 127004, 129402, 128786 }) == 129402, "unknown build: the newest known build is the highest CL");
		Check(UnknownBuild::NewestKnown({}) == 0, "unknown build: no known build gives 0");

		// The ini round trip, on a file of its own.
		wchar_t tmp[MAX_PATH]{};
		GetTempPathW(MAX_PATH, tmp);
		const std::wstring ini = std::wstring(tmp) + L"unknown_build_test_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
		const auto exists = [&] { return GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES; };
		const auto writeIni = [&](const char* text)
		{
			FILE* f = nullptr;
			_wfopen_s(&f, ini.c_str(), L"wb");
			if (f) { fputs(text, f); fclose(f); }
		};
		const auto session = [&]
		{
			wchar_t buf[128]{};
			GetPrivateProfileStringW(L"UnknownBuild", L"Session", L"<none>", buf, 128, ini.c_str());
			return std::wstring(buf);
		};
		const auto retryKey = [&]
		{
			return GetPrivateProfileIntW(L"UnknownBuild", L"Retry", -1, ini.c_str());
		};
		const auto decideFromIni = [&](uint32_t cl)
		{
			return UnknownBuild::Decide(UnknownBuild::ReadInputs(ini, cl, id));
		};

		DeleteFileW(ini.c_str());
		Check(!UnknownBuild::Active() && !UnknownBuild::EndSession(), "unknown build: nothing to end before a session starts");
		Check(!exists(), "unknown build: ending with no session writes no file");

		UnknownBuild::Inputs fresh = UnknownBuild::ReadInputs(ini, 130045, id);
		Check(fresh.enabled && !fresh.retry && fresh.lastSession.empty() && fresh.runningCL == 130045 && fresh.thisSession == id,
			"unknown build: a missing modloader.ini reads as enabled, no retry, no marker");
		Check(UnknownBuild::Decide(fresh) == Decision::Fallback, "unknown build: a missing modloader.ini is Fallback");

		const std::wstring unwritable = std::wstring(tmp) + L"unknown_build_no_such_dir_" + std::to_wstring(GetCurrentProcessId()) + L"\\modloader.ini";
		Check(!UnknownBuild::Arm(unwritable, id, 130045, 129402) && !UnknownBuild::Active() && !UnknownBuild::EndSession(),
			"unknown build: an ini that cannot be written arms nothing");

		writeIni("[UnknownBuild]\r\nRetry=1\r\nEnabled=1\r\n");
		Check(retryKey() == 1 && decideFromIni(130045) == Decision::Fallback, "unknown build: Retry=1 on a clean ini is Fallback");
		Check(UnknownBuild::Arm(ini, id, 130045, 129402), "unknown build: arming writes the marker");
		Check(session() == id, "unknown build: the marker is the session id");
		Check(retryKey() == -1, "unknown build: arming removes Retry");
		Check(GetPrivateProfileIntW(L"UnknownBuild", L"Enabled", -1, ini.c_str()) == 1, "unknown build: arming leaves the other keys");
		Check(UnknownBuild::Active() && UnknownBuild::RunningCL() == 130045 && UnknownBuild::NewestCL() == 129402,
			"unknown build: arming sets the state the UI reads");

		Check(decideFromIni(130045) == Decision::Guarded, "unknown build: after an unclean end the next start is Guarded");
		Check(UnknownBuild::Decide(UnknownBuild::ReadInputs(ini, 130045, UnknownBuild::SessionId(130045, 0x1A2B3C4D, 0x0F4A3000, "1.25.2")))
			== Decision::Fallback, "unknown build: another loader build tries again");
		WritePrivateProfileStringW(L"UnknownBuild", L"Retry", L"1", ini.c_str());
		Check(decideFromIni(130045) == Decision::Fallback, "unknown build: Retry=1 tries again after an unclean end");

		Check(UnknownBuild::EndSession(), "unknown build: a clean end clears the marker");
		Check(session() == L"<none>", "unknown build: the marker is gone after a clean end");
		Check(!UnknownBuild::EndSession(), "unknown build: a second end does nothing");
		Check(UnknownBuild::Active(), "unknown build: the session stays active in the process after it ends");
		Check(decideFromIni(130045) == Decision::Fallback, "unknown build: the next start after a clean end is Fallback");

		WritePrivateProfileStringW(L"UnknownBuild", L"Enabled", L"0", ini.c_str());
		Check(decideFromIni(130045) == Decision::SwitchedOff, "unknown build: Enabled=0 in the ini is SwitchedOff");
		DeleteFileW(ini.c_str());

		// The words the player reads: "map" is always the version object map, spelled out on first use.
		const auto narrow = [](const std::wstring& w) { std::string out; for (wchar_t c : w) out += static_cast<char>(c); return out; };
		const auto termsOk = [](const std::string& text)
		{
			bool first = true;
			for (size_t at = text.find("map"); at != std::string::npos; at = text.find("map", at + 1))
			{
				const bool wordStart = at == 0 || !isalpha(static_cast<unsigned char>(text[at - 1]));
				const bool wordEnd   = at + 3 >= text.size() || !isalpha(static_cast<unsigned char>(text[at + 3]));
				if (!wordStart || !wordEnd)
					continue;
				const std::string before = text.substr(0, at);
				const auto endsWith = [&](const char* tail)
				{
					const size_t n = strlen(tail);
					return before.size() >= n && before.compare(before.size() - n, n, tail) == 0;
				};
				if (first ? !endsWith("version object ") : !(endsWith("version ") || endsWith("version object ")))
					return false;
				first = false;
			}
			return !first;
		};
		const std::string notice    = UnknownBuild::NoticeText(129402);
		const std::string line      = UnknownBuild::ReportLine(130045, 129402);
		const std::string preflight = narrow(UnknownBuild::PreflightNote(130045, 129402));
		Check(termsOk(notice) && termsOk(line) && termsOk(preflight), "unknown build: the text says version object map first, then version map");
		Check(!termsOk("The map is not known.") && !termsOk("a version map first, then the version object map"),
			"unknown build: the term check catches a bare map and a short form before the long one");
		Check(notice.find("CL-129402") != std::string::npos, "unknown build: the notice names the newest known CL");
		Check(line.find("CL-130045") != std::string::npos && line.find("CL-129402") != std::string::npos
			&& line.find('\n') == std::string::npos, "unknown build: the report line is one line with both CLs");
		Check(preflight.find("CL-130045") != std::string::npos && preflight.find("CL-129402") != std::string::npos
			&& preflight.size() >= 4 && preflight.compare(preflight.size() - 4, 4, "\r\n\r\n") == 0,
			"unknown build: the preflight note names both CLs and ends in a blank line");
		const std::wstring guard = UnknownBuild::GuardDetails(L"1.0.0-CL-130045", "1.25.0-dev.2");
		Check(guard.find(L"[UnknownBuild]") != std::wstring::npos && guard.find(L"Retry=1") != std::wstring::npos
			&& guard.find(L"1.0.0-CL-130045") != std::wstring::npos && guard.find(L"1.25.0-dev.2") != std::wstring::npos
			&& guard.find(L"\r\n") != std::wstring::npos, "unknown build: the guard details say how to retry");
	}

	// Version minimums (host_rules_wire.h): the host's table as text, who is below it, and what the player is told.
	{
		using namespace HostRulesWire;
		const auto parseMin = [](const std::string& text, Minimums& out, std::string& err) { return ParseMinimumsText(text, out, err); };
		const auto statusOf = [&](const std::string& text, std::string* err = nullptr)
		{
			Minimums m;
			std::string e;
			const ParseResult r = parseMin(text, m, e);
			if (err)
				*err = e;
			return r.status;
		};
		const auto table = [&](const std::string& text)
		{
			Minimums m;
			std::string e;
			parseMin(text, m, e);
			return m;
		};
		const auto row = [](const char* plugin, const char* version, bool wireRefused = false)
		{
			ManifestRow r;
			r.plugin = plugin;
			r.version = version;
			r.wireRefused = wireRefused;
			return r;
		};
		const auto find = [&](const std::string& text, bool reported, const std::string& loaderTag, const std::vector<ManifestRow>& rows)
		{
			return FindBelowMinimum(table(text), reported, loaderTag, rows);
		};
		const auto reasonFor = [&](const std::string& text, bool reported, const std::string& loaderTag, const std::vector<ManifestRow>& rows)
		{
			return BuildBelowMinimumReason(find(text, reported, loaderTag, rows));
		};

		// Parsing: what is read.
		{
			Minimums m;
			std::string err = "stale";
			ParseResult r = parseMin("", m, err);
			Check(r.status == ParseStatus::Ok && m.Empty() && err.empty(), "minimums text: empty text is an empty table");
			r = parseMin("\xEF\xBB\xBF# note\r\n  ; another\r\n\r\n\t\n", m, err);
			Check(r.status == ParseStatus::Ok && m.Empty(), "minimums text: BOM, comments, blank lines and CRLF are skipped");
			r = parseMin("loader=1.27.0\r\nRuptureTimer = v0.11.0 \r\nproductionviewer=wire\n", m, err);
			Check(r.status == ParseStatus::Ok && m.loader == "1.27.0" && m.mods.size() == 2, "minimums text: loader and two mods");
			Check(m.mods[0].plugin == "RuptureTimer" && m.mods[0].version == "v0.11.0" && !m.mods[0].wire,
				"minimums text: a mod row keeps its name and its version as written, trimmed");
			Check(m.mods[1].plugin == "productionviewer" && m.mods[1].version.empty() && m.mods[1].wire,
				"minimums text: wire is a row with no number");
			Check(parseMin("LOADER=1.2\nWire=WIRE\n", m, err).status == ParseStatus::Ok && m.loader == "1.2" && m.mods.size() == 1 && m.mods[0].wire,
				"minimums text: the loader key and the wire value ignore case");
			Check(parseMin("loader=\n", m, err).status == ParseStatus::Ok && m.loader.empty() && m.mods.empty(),
				"minimums text: an empty loader value is off");
			Check(parseMin("loader=1.27.0-dev.3\nA=0.1\n", m, err).status == ParseStatus::Ok && m.loader == "1.27.0-dev.3" && m.mods[0].version == "0.1",
				"minimums text: a suffix and a two-part version are readable");
			Check(parseMin("Some Mod=1.0\n", m, err).status == ParseStatus::Ok && m.mods[0].plugin == "Some Mod",
				"minimums text: a mod name may hold a space");
		}

		// Parsing: what is refused, with the line and the text.
		{
			std::string err;
			Minimums keep;
			keep.loader = "9.9.9";
			Minimums m = keep;
			ParseResult r = parseMin("loader=1.2.3\nRuptureTimer=soon\n", m, err);
			Check(r.status == ParseStatus::BadValue && r.line == 2 && err == "line 2: RuptureTimer needs a version such as 0.11.0, or wire", "minimums text: a version that cannot be read is refused with its line");
			Check(m.loader == "9.9.9" && m.mods.empty(), "minimums text: out is untouched on an error");
			r = parseMin("# c\nloader=dev\n", m, err);
			Check(r.status == ParseStatus::BadValue && r.line == 2 && err == "line 2: loader needs a version such as 1.27.0, or nothing to turn it off", "minimums text: loader=dev is refused");
			Check(statusOf("loader=wire\n") == ParseStatus::BadValue, "minimums text: wire is not a loader value");
			Check(statusOf("loader=1\n") == ParseStatus::BadValue, "minimums text: a version with one part is refused");
			Check(statusOf("loader=1.2abc\n") == ParseStatus::BadValue, "minimums text: trailing letters are refused");
			Check(statusOf("A=\n") == ParseStatus::BadValue, "minimums text: a mod row with no value is refused");
			Check(statusOf("A=1.2.3.4.5\n") == ParseStatus::BadValue, "minimums text: five parts are refused");
			Check(statusOf("A=" + std::string(65, '1') + ".0\n") == ParseStatus::BadValue, "minimums text: a version over the tag limit is refused");
			Check(statusOf("loader=1.0\nloader=1.1\n", &err) == ParseStatus::Duplicate && err == "line 2: loader is given twice", "minimums text: loader twice");
			Check(statusOf("loader=\nloader=\n") == ParseStatus::Duplicate, "minimums text: empty loader twice is still a repeat");
			Check(statusOf("A=1.0\na=2.0\n", &err) == ParseStatus::Duplicate && err == "line 2: a is given twice", "minimums text: a mod twice, ignoring case");
			Check(statusOf("A=1.0\nB=wire\nb=wire\n") == ParseStatus::Duplicate, "minimums text: wire twice");
			Check(statusOf("loader 1.0\n", &err) == ParseStatus::BadLine && err == "line 1: expected key=value", "minimums text: a line with no '='");
			Check(statusOf("=1.0\n") == ParseStatus::BadKey, "minimums text: no key");
			Check(statusOf("$Hidden=1.0\n") == ParseStatus::BadKey, "minimums text: a mod name starting with '$' is refused");
			Check(statusOf(std::string(129, 'A') + "=1.0\n") == ParseStatus::BadKey, "minimums text: a mod name over the limit is refused");
			Check(statusOf(std::string(128, 'A') + "=1.0\n") == ParseStatus::Ok, "minimums text: a mod name at the limit is read");
			Check(statusOf("A=1.0\x01\n", &err) == ParseStatus::ControlChar && err == "line 1: control character", "minimums text: a control character");
			Check(statusOf("A\tB=1.0\n") == ParseStatus::ControlChar, "minimums text: a tab inside a key is refused like in the rules text");
			Check(statusOf("A=1.\t0\n") == ParseStatus::ControlChar, "minimums text: a tab inside a value is refused");
			{
				Minimums tm;
				std::string te;
				Check(parseMin("A\t=\t1.0\n", tm, te).status == ParseStatus::Ok && tm.mods.size() == 1 && tm.mods[0].plugin == "A" && tm.mods[0].version == "1.0",
					"minimums text: tabs around a mod key and value are blanks");
				Minimums lm;
				Check(parseMin("loader\t=\t1.27.0\t\n", lm, te).status == ParseStatus::Ok && lm.loader == "1.27.0",
					"minimums text: tabs around loader= are blanks");
			}
			Check(statusOf(std::string(kMaxPayloadBytes + 1, ' '), &err) == ParseStatus::TooLarge && err.find("line") == std::string::npos, "minimums text: text over the payload limit");
			Check(statusOf(std::string(kMaxPayloadBytes, '\n')) == ParseStatus::Ok, "minimums text: text at the limit is read");

			std::string many;
			for (size_t i = 0; i < kMaxModMinimums; ++i)
				many += "Mod" + std::to_string(i) + "=1.0\n";
			Minimums big;
			Check(parseMin(many, big, err).status == ParseStatus::Ok && big.mods.size() == kMaxModMinimums, "minimums text: the most rows are read");
			Check(statusOf(many + "Extra=1.0\n", &err) == ParseStatus::TooManyRows && err.find("line " + std::to_string(kMaxModMinimums + 1) + ":") == 0, "minimums text: one row too many");
		}

		// FindBelowMinimum: the version rules.
		{
			const std::vector<ManifestRow> none;
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "0.11.0") }).empty(), "below minimum: equal is fine");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "0.12.0") }).empty(), "below minimum: higher is fine");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "0.10.9") }).size() == 1, "below minimum: lower is below");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "0.9.2") }).size() == 1, "below minimum: numbers, not text (9 under 11)");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "0.11.0-dev.6") }).empty(), "below minimum: a suffix is ignored (0.11.0-dev.6 is 0.11.0)");
			Check(find("RuptureTimer=0.11.0-dev.1\n", true, "1.27.0", { row("RuptureTimer", "0.11.0") }).empty(), "below minimum: a suffix on the minimum is ignored too");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "0.10.0-dev.6") }).size() == 1, "below minimum: a lower release with a suffix is below");
			Check(find("RuptureTimer=v0.11.0\n", true, "1.27.0", { row("RuptureTimer", "0.11.0") }).empty(), "below minimum: a v prefix on the minimum");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "v0.10.0") }).size() == 1, "below minimum: a v prefix on the client's version");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("rupturetimer", "0.10.0") }).size() == 1, "below minimum: the mod name ignores case");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "dev") }).empty(), "below minimum: a mod version of dev is never below");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "") }).empty(), "below minimum: an empty mod version is never below");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "1") }).empty(), "below minimum: a one-part mod version is unreadable, never below");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", { row("Other", "0.1.0") }).empty(), "below minimum: a mod with no row is never below");
			Check(find("RuptureTimer=0.11.0\nOther=9.0\n", true, "1.27.0", { row("Other2", "0.1.0") }).empty(), "below minimum: a mod the player lacks is never below");
			Check(find("RuptureTimer=0.11.0\n", true, "1.27.0", none).empty(), "below minimum: an empty manifest");
			Check(find("", true, "0.1.0", { row("RuptureTimer", "0.0.1") }).empty(), "below minimum: an empty table is never below");
			Check(find("", false, "", { row("RuptureTimer", "0.0.1") }).empty(), "below minimum: an empty table is never below, loader silent");

			// The loader.
			Check(find("loader=1.27.0\n", true, "1.27.0", none).empty(), "below minimum: loader equal");
			Check(find("loader=1.27.0\n", true, "v1.28.0", none).empty(), "below minimum: loader higher");
			Check(find("loader=1.27.0\n", true, "1.27.0-dev.3", none).empty(), "below minimum: a labeled dev loader counts as its release (1.27.0-dev.3)");
			Check(find("loader=1.27.0\n", true, "1.26.1-dev.7", none).size() == 1, "below minimum: a labeled dev loader of an older release is below");
			Check(find("loader=1.27.0\n", true, "dev", none).empty(), "below minimum: a loader tagged dev is never below");
			Check(find("loader=1.27.0\n", true, "", none).empty(), "below minimum: a reported empty loader tag is never below");
			Check(find("loader=1.27.0\n", true, "garbage", none).empty(), "below minimum: an unreadable loader tag is never below");
			Check(find("loader=\n", false, "", none).empty(), "below minimum: no loader minimum, a silent loader is fine");
			Check(find("RuptureTimer=1.0\n", false, "", none).empty(), "below minimum: mod rows only, a silent loader is fine");
			{
				const auto v = find("loader=1.27.0\n", false, "", none);
				Check(v.size() == 1 && v[0].isLoader && v[0].have.empty() && v[0].need == "1.27.0" && v[0].name == "ModLoader",
					"below minimum: a loader that reported no version is below any minimum, with nothing to show");
				const auto w = find("loader=1.27.0\n", true, "1.25.0", none);
				Check(w.size() == 1 && w[0].isLoader && w[0].have == "1.25.0" && w[0].need == "1.27.0",
					"below minimum: a lower loader carries both numbers");
			}

			// wire.
			Check(find("ProductionViewer=wire\n", true, "1.27.0", { row("ProductionViewer", "0.18.4", true) }).size() == 1, "below minimum: wire and refused is below");
			Check(find("ProductionViewer=wire\n", true, "1.27.0", { row("ProductionViewer", "0.18.4", false) }).empty(), "below minimum: wire and accepted is fine");
			Check(find("ProductionViewer=0.1.0\n", true, "1.27.0", { row("ProductionViewer", "0.18.4", true) }).empty(), "below minimum: a refused wire does not matter without a wire row");
			Check(find("ProductionViewer=wire\n", true, "1.27.0", { row("ProductionViewer", "dev", true) }).size() == 1, "below minimum: wire refused with an unreadable version is still below");
			{
				const auto v = find("ProductionViewer=wire\n", true, "1.27.0", { row("ProductionViewer", "0.18.4", true) });
				Check(v.size() == 1 && !v[0].isLoader && v[0].need.empty() && v[0].have == "0.18.4" && v[0].name == "ProductionViewer", "below minimum: a wire violation has no number to ask for");
			}

			// Order: loader first, then the manifest's order, not the table's.
			const auto v = find("loader=1.27.0\nZeta=2.0\nAlpha=2.0\nMid=2.0\n", true, "1.0.0",
				{ row("Mid", "1.0"), row("Alpha", "1.0"), row("Fine", "9.9"), row("Zeta", "1.0") });
			Check(v.size() == 4 && v[0].isLoader && v[1].name == "Mid" && v[2].name == "Alpha" && v[3].name == "Zeta",
				"below minimum: loader first, then mods in manifest order");
			Check(v.size() == 4 && v[1].name == "Mid" && v[1].need == "2.0" && v[1].have == "1.0", "below minimum: a mod violation carries the host's number and the player's");
			Check(find("loader=9.0\nA=9.0\n", true, "1.0.0", { row("A", "1.0"), row("A", "1.0") }).size() == 3, "below minimum: each manifest row is judged");
		}

		// The player's text: every wording.
		{
			const std::vector<ManifestRow> none;
			const auto loaderOld = reasonFor("loader=1.27.0\n", true, "1.25.0", none);
			Check(loaderOld == "This server needs ModLoader 1.27.0 or newer (you have 1.25.0). Update it, or remove the ModLoader to join without mods.",
				"below minimum text: the loader line");
			const auto loaderSilent = reasonFor("loader=1.27.0\n", false, "", none);
			Check(loaderSilent == "This server needs ModLoader 1.27.0 or newer. Update it, or remove the ModLoader to join without mods.",
				"below minimum text: a loader that reported no version has no (you have ...)");
			const auto one = reasonFor("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "0.9.2") });
			Check(one == "This server needs RuptureTimer 0.11.0 or newer (you have 0.9.2). Update RuptureTimer, or remove it, and join again.",
				"below minimum text: one mod");
			const auto two = reasonFor("RuptureTimer=0.11.0\nProductionViewer=0.19.0\n", true, "1.27.0",
				{ row("RuptureTimer", "0.9.2"), row("ProductionViewer", "0.18.4") });
			Check(two == "This server needs RuptureTimer 0.11.0 or newer (you have 0.9.2) and ProductionViewer 0.19.0 or newer (you have 0.18.4). Update them and join again.",
				"below minimum text: two mods");
			const auto three = reasonFor("A=2.0\nB=2.0\nC=2.0\n", true, "1.27.0", { row("A", "1.0"), row("B", "1.0"), row("C", "1.0") });
			Check(three == "This server needs A 2.0 or newer (you have 1.0) and B 2.0 or newer (you have 1.0) and 1 more. Update them and join again.",
				"below minimum text: three mods name the first two and count the rest");
			const auto five = reasonFor("A=2.0\nB=2.0\nC=2.0\nD=2.0\nE=2.0\n", true, "1.27.0",
				{ row("A", "1.0"), row("B", "1.0"), row("C", "1.0"), row("D", "1.0"), row("E", "1.0") });
			Check(five.find(" and 3 more. Update them and join again.") != std::string::npos && five.find("C 2.0") == std::string::npos,
				"below minimum text: five mods say 3 more");
			const auto wire = reasonFor("ProductionViewer=wire\n", true, "1.27.0", { row("ProductionViewer", "0.18.4", true) });
			Check(wire == "This server needs ProductionViewer to match the server's version (you have 0.18.4). Update ProductionViewer, or remove it, and join again.",
				"below minimum text: a wire row");
			const auto wireNoVersion = reasonFor("ProductionViewer=wire\n", true, "1.27.0", { row("ProductionViewer", "", true) });
			Check(wireNoVersion == "This server needs ProductionViewer to match the server's version. Update ProductionViewer, or remove it, and join again.",
				"below minimum text: a wire row with no version shown");
			const auto mixed = reasonFor("loader=1.27.0\nRuptureTimer=0.11.0\n", true, "1.25.0", { row("RuptureTimer", "0.9.2") });
			Check(mixed == "This server needs ModLoader 1.27.0 or newer (you have 1.25.0) and RuptureTimer 0.11.0 or newer (you have 0.9.2). Update them and join again.",
				"below minimum text: the loader and a mod together");
			const auto wireAndNumber = reasonFor("A=wire\nB=2.0\n", true, "1.27.0", { row("A", "1.0", true), row("B", "1.0") });
			Check(wireAndNumber == "This server needs A to match the server's version (you have 1.0) and B 2.0 or newer (you have 1.0). Update them and join again.",
				"below minimum text: a wire row beside a number row");
			Check(reasonFor("RuptureTimer=0.11.0\n", true, "1.27.0", { row("RuptureTimer", "0.12.0") }).empty(), "below minimum text: nothing below, no text");
			Check(BuildBelowMinimumReason({}).empty(), "below minimum text: no violations, no text");
			Check(reasonFor("RuptureTimer=v0.11.0\n", true, "1.27.0", { row("RuptureTimer", "0.9.2") }).find("RuptureTimer v0.11.0 or newer") != std::string::npos,
				"below minimum text: the host's number is shown as written");

			Check(loaderOld.size() <= 150 && one.size() <= 150 && wire.size() <= 150, "below minimum text: loader, one mod and wire are within 150 characters");
			Check(two.size() <= 160, "below minimum text: two mods stay near 150 characters");

			// Plain: ASCII only, one line, no mention of the version object map.
			bool plain = true;
			for (const std::string* t : { &loaderOld, &loaderSilent, &one, &two, &three, &five, &wire, &wireNoVersion, &mixed, &wireAndNumber })
			{
				for (char c : *t)
					if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
						plain = false;
				if (t->find("map") != std::string::npos || t->find("Map") != std::string::npos)
					plain = false;
			}
			Check(plain, "below minimum text: plain ASCII on one line, no word map");

			// The cap: long mod names are cut at the end; the text never passes the cap.
			const std::string longA(128, 'A');
			const std::string longB(128, 'B');
			const std::string table2 = longA + "=2.0\n" + longB + "=2.0\n";
			const std::vector<ManifestRow> longRows = { row(longA.c_str(), "1.0"), row(longB.c_str(), "1.0") };
			const auto cut = reasonFor(table2, true, "1.27.0", longRows);
			Check(cut.size() <= kMaxBelowMinimumReason && cut.size() > 100, "below minimum text: two long names fit under the cap");
			Check(cut.find("A 2.0 or newer (you have 1.0) and B") != std::string::npos && cut.find(" 2.0 or newer (you have 1.0). Update them and join again.") != std::string::npos,
				"below minimum text: names are cut at the end, the numbers and the ask stay");
			const auto cutOne = reasonFor(longA + "=2.0\n", true, "1.27.0", { row(longA.c_str(), "1.0") });
			Check(cutOne.size() <= kMaxBelowMinimumReason && cutOne.find("Update A") != std::string::npos && cutOne.find("or remove it, and join again.") != std::string::npos,
				"below minimum text: one long name is cut too and the ask stays");
			Check(reasonFor("A=2.0\n", true, "1.27.0", { row("A", "1.0") }).size() < 150, "below minimum text: a short name is not cut");
			const auto manyLong = reasonFor(longA + "=2.0\n" + longB + "=2.0\n" + "loader=1.27.0\n", true, "1.0.0", longRows);
			Check(manyLong.size() <= kMaxBelowMinimumReason, "below minimum text: the loader and two long names stay under the cap");

			// Even maximal numbers cannot pass the hard cap.
			const std::string bigVer = "1." + std::string(60, '9');
			const auto huge = reasonFor(longA + "=" + bigVer + "\n" + longB + "=" + bigVer + "\n", true, "1.27.0",
				{ row(longA.c_str(), "1.0"), row(longB.c_str(), "1.0") });
			Check(huge.size() <= kMaxBelowMinimumReason, "below minimum text: the hard cap holds against maximal numbers");

			// Long version tags are cut, so the closing instruction is never the part that goes.
			const std::string longTag(60, '7');
			const auto longTags = BuildBelowMinimumReason({
				Violation{ false, longA, longTag, longTag },
				Violation{ false, longB, longTag, longTag } });
			const std::string closing = "Update them and join again.";
			Check(longTags.size() <= kMaxBelowMinimumReason && longTags.size() >= closing.size() &&
				longTags.compare(longTags.size() - closing.size(), closing.size(), closing) == 0,
				"below minimum text: two long-tag violations end with the instruction, within the cap");
			Check(longTags.find(std::string(25, '7')) == std::string::npos && longTags.find(std::string(24, '7')) != std::string::npos,
				"below minimum text: a version tag is shown at most 24 characters");

			// A name with control characters or non-ASCII bytes is made plain.
			const auto odd = BuildBelowMinimumReason({ Violation{ false, std::string("Bad\x01Na\xC3\xA9me"), "2.0", "1.0" } });
			bool oddPlain = !odd.empty();
			for (char c : odd)
				if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
					oddPlain = false;
			Check(oddPlain && odd.find("Bad?Na??me") != std::string::npos, "below minimum text: control and non-ASCII bytes in a name become '?'");
		}
	}

	// Client features text (host_rules_wire.h): what GetClientFeatures hands a server plugin.
	{
		using namespace HostRulesWire;
		const std::string none = BuildClientFeaturesText("v1.26.1", 79, {});
		Check(none == "loader=v1.26.1 interface=79", "client features text: no features is the loader line alone");

		FeatRecord a; a.plugin = "Codex"; a.id = "pins"; a.on = true;
		FeatRecord b; b.plugin = "MiniMap"; b.id = "zoom"; b.on = false;
		Check(BuildClientFeaturesText("v1.26.1", 79, { a }) == "loader=v1.26.1 interface=79\nCodex.pins=on",
			"client features text: one feature is one line after the loader line");
		const std::string two = BuildClientFeaturesText("v1.26.1", 79, { a, b });
		Check(two == "loader=v1.26.1 interface=79\nCodex.pins=on\nMiniMap.zoom=off", "client features text: lines keep the reported order and on/off");

		Check(CopyTextCut(two, nullptr, 0) == static_cast<int>(two.size()), "client features copy: a null buffer measures the full length");
		char big[128];
		std::memset(big, 'x', sizeof(big));
		Check(CopyTextCut(two, big, sizeof(big)) == static_cast<int>(two.size()) && two == big, "client features copy: a large buffer gets the whole text and a NUL");
		char small[10];
		std::memset(small, 'x', sizeof(small));
		Check(CopyTextCut(two, small, sizeof(small)) == static_cast<int>(two.size()) && std::strcmp(small, "loader=v1") == 0,
			"client features copy: a small buffer is cut and NUL-terminated, the length stays the full one");
		char one[1] = { 'x' };
		Check(CopyTextCut(two, one, 1) == static_cast<int>(two.size()) && one[0] == '\0', "client features copy: a one-byte buffer holds the NUL only");
	}

	// Admin prompt codec (host_rules_wire.h): $ML:adminq (server to client) and $ML:adminr (client to server).
	// These checks are in the Client-only test file; the Server exe builds server_map_tests.cpp and keeps its 179.
	{
		using namespace HostRulesWire;
		const std::string US(1, kFieldSep);
		const std::string RS(1, kRecordSep);
		auto dq = [](const std::string& p, AdminQuestion* q) { return DecodeAdminQuestion(p.data(), p.size(), q); };
		auto da = [](const std::string& p, AdminAnswer* a) { return DecodeAdminAnswer(p.data(), p.size(), a); };

		Check(std::string(kTagAdminQ) == "adminq" && std::string(kTagAdminR) == "adminr", "admin tags: adminq and adminr");

		// $ML:adminq: exact bytes and a round trip for each kind.
		Check(EncodeAdminQuestion(AdminQKind::Ask, "") == "ask" + US + RS, "adminq encode: ask with no text is 'ask' US RS");
		Check(EncodeAdminQuestion(AdminQKind::Info, "Wrong code, 4 tries left") == "info" + US + "Wrong code, 4 tries left" + RS,
			"adminq encode: info carries its text");
		Check(EncodeAdminQuestion(AdminQKind::Close, "Welcome, admin") == "close" + US + "Welcome, admin" + RS, "adminq encode: close carries its text");
		{
			const AdminQKind kinds[] = { AdminQKind::Ask, AdminQKind::Info, AdminQKind::Close };
			for (AdminQKind k : kinds)
			{
				AdminQuestion q;
				q.kind = (k == AdminQKind::Ask) ? AdminQKind::Close : AdminQKind::Ask; // not the kind under test
				q.text = "stale";
				const std::string txt = (k == AdminQKind::Ask) ? "" : "Line for the player";
				const std::string wire = EncodeAdminQuestion(k, txt);
				Check(dq(wire, &q) == Status::Ok && q.kind == k && q.text == txt, "adminq round trip: kind and text survive");
			}
		}
		{
			// A multi-byte text round trips as it is.
			AdminQuestion q;
			const std::string utf = "Caf\xC3\xA9 code?";
			Check(dq(EncodeAdminQuestion(AdminQKind::Ask, utf), &q) == Status::Ok && q.text == utf, "adminq round trip: UTF-8 text is kept");
		}

		// adminq text cap at the boundary: 200 travels whole, 201 is cut by the encoder and refused by the decoder.
		{
			const std::string t200(200, 'a');
			const std::string t201(201, 'a');
			AdminQuestion q;
			Check(dq(EncodeAdminQuestion(AdminQKind::Info, t200), &q) == Status::Ok && q.text == t200, "adminq cap: 200 bytes round trip whole");
			Check(dq(EncodeAdminQuestion(AdminQKind::Info, t201), &q) == Status::Ok && q.text == t200, "adminq cap: the encoder cuts 201 bytes to 200");
			Check(dq("info" + US + t200 + RS, &q) == Status::Ok, "adminq decode: 200 bytes of text are accepted");
			Check(dq("info" + US + t201 + RS, &q) == Status::FieldTooLong, "adminq decode: 201 bytes of text are too long");
			Check(dq("close" + US + t200 + RS, &q) == Status::Ok && q.kind == AdminQKind::Close, "adminq decode: the longest kind with 200 bytes is accepted");
			Check(dq("close" + US + t201 + RS, &q) == Status::TooLarge, "adminq decode: the longest kind with 201 bytes is over the payload maximum");
			// A cut never splits a UTF-8 character: 199 ASCII bytes then a 2-byte character is cut back to 199.
			const std::string split = std::string(199, 'b') + "\xC3\xA9";
			Check(dq(EncodeAdminQuestion(AdminQKind::Ask, split), &q) == Status::Ok && q.text == std::string(199, 'b'),
				"adminq cap: a cut does not split a UTF-8 character");
		}

		// adminq decode: every non-Ok status, and *out is left as it was.
		{
			AdminQuestion keep;
			keep.kind = AdminQKind::Info;
			keep.text = "keep";
			Check(DecodeAdminQuestion(nullptr, 5, &keep) == Status::Empty, "adminq decode: a null pointer is empty");
			Check(DecodeAdminQuestion("x", 0, &keep) == Status::Empty, "adminq decode: zero bytes are empty");
			Check(dq(std::string(300, 'a'), &keep) == Status::TooLarge, "adminq decode: a payload over the kind-plus-text maximum is too large");
			Check(dq("ask" + US, &keep) == Status::Truncated, "adminq decode: no record separator is truncated");
			Check(dq("ask" + US + "hello", &keep) == Status::Truncated, "adminq decode: cut text is truncated");
			Check(dq("ask", &keep) == Status::Truncated, "adminq decode: a bare kind with no separators is truncated");
			Check(dq("ask" + RS, &keep) == Status::BadRecord, "adminq decode: one field is a bad record");
			Check(dq("ask" + US + "a" + US + "b" + RS, &keep) == Status::BadRecord, "adminq decode: three fields are a bad record");
			Check(dq("ask" + US + RS + "ask" + US + RS, &keep) == Status::BadRecord, "adminq decode: two records are a bad record");
			Check(dq("nope" + US + "x" + RS, &keep) == Status::BadRecord, "adminq decode: an unknown kind is a bad record");
			Check(dq("ASK" + US + RS, &keep) == Status::BadRecord, "adminq decode: kinds are lower case only");
			Check(dq("code" + US + "x" + RS, &keep) == Status::BadRecord, "adminq decode: an adminr kind is not an adminq kind");
			Check(dq(US + "x" + RS, &keep) == Status::BadRecord, "adminq decode: an empty kind is a bad record");
			Check(dq("info" + US + "a\x01" "b" + RS, &keep) == Status::ControlChar, "adminq decode: a control character in the text is refused");
			Check(dq("info" + US + "a\x7F" "b" + RS, &keep) == Status::ControlChar, "adminq decode: DEL in the text is refused");
			Check(dq("info" + US + "a\nb" + RS, &keep) == Status::ControlChar, "adminq decode: a newline in the text is refused");
			Check(dq("in\x02" "fo" + US + "x" + RS, &keep) == Status::ControlChar, "adminq decode: a control character in the kind is refused");
			Check(keep.kind == AdminQKind::Info && keep.text == "keep", "adminq decode: *out is untouched after every error");

			// A control character in the encoder's input is made a space, not sent.
			AdminQuestion q;
			Check(dq(EncodeAdminQuestion(AdminQKind::Info, "a\x01\x1F" "b\r\nc"), &q) == Status::Ok && q.text == "a  b  c",
				"adminq encode: control characters (separators too) become spaces");
			Check(dq(EncodeAdminQuestion(AdminQKind::Info, "  padded  "), &q) == Status::Ok && q.text == "padded", "adminq encode: the text is trimmed");
			// Only the given length is read: the bytes after it are never touched.
			const std::string two = "ask" + US + RS + "junk";
			Check(DecodeAdminQuestion(two.data(), 3 + 1 + 1, &q) == Status::Ok && q.kind == AdminQKind::Ask,
				"adminq decode: only the given length is read");
		}

		// $ML:adminr: exact bytes and a round trip for each kind.
		Check(EncodeAdminAnswer(AdminRKind::Code, "S3cret") == "code" + US + "S3cret" + RS, "adminr encode: code carries the code");
		Check(EncodeAdminAnswer(AdminRKind::NotMe, "ignored") == "notme" + US + RS, "adminr encode: notme carries no code, whatever was passed");
		Check(EncodeAdminAnswer(AdminRKind::Code, "").empty(), "adminr encode: an empty code gives no payload");
		Check(EncodeAdminAnswer(AdminRKind::Code, " \x01 \t ").empty(), "adminr encode: a code of only controls and spaces gives no payload");
		{
			AdminAnswer a;
			a.kind = AdminRKind::NotMe;
			Check(da(EncodeAdminAnswer(AdminRKind::Code, "hunter2 with spaces"), &a) == Status::Ok && a.kind == AdminRKind::Code &&
				a.code == "hunter2 with spaces", "adminr round trip: a code keeps its inner spaces");
			a.kind = AdminRKind::Code;
			a.code = "stale";
			Check(da(EncodeAdminAnswer(AdminRKind::NotMe, ""), &a) == Status::Ok && a.kind == AdminRKind::NotMe && a.code.empty(),
				"adminr round trip: notme comes back with no code");
			Check(da(EncodeAdminAnswer(AdminRKind::Code, "a\x01\x1F" "b"), &a) == Status::Ok && a.code == "a  b",
				"adminr encode: control characters (separators too) in a code become spaces");
		}

		// adminr code cap at the boundary: 64 travels whole, 65 is cut by the encoder and refused by the decoder.
		{
			const std::string c64(64, 'k');
			const std::string c65(65, 'k');
			AdminAnswer a;
			Check(da(EncodeAdminAnswer(AdminRKind::Code, c64), &a) == Status::Ok && a.code == c64, "adminr cap: 64 bytes round trip whole");
			Check(da(EncodeAdminAnswer(AdminRKind::Code, c65), &a) == Status::Ok && a.code == c64, "adminr cap: the encoder cuts 65 bytes to 64");
			Check(da("code" + US + c64 + RS, &a) == Status::Ok, "adminr decode: 64 bytes of code are accepted");
			Check(da("code" + US + c65 + RS, &a) == Status::FieldTooLong, "adminr decode: 65 bytes of code are too long");
			Check(da("code" + US + std::string(200, 'k') + RS, &a) == Status::TooLarge, "adminr decode: a payload over the maximum is too large");
		}

		// adminr decode: every non-Ok status, and *out is left as it was.
		{
			AdminAnswer keep;
			keep.kind = AdminRKind::Code;
			keep.code = "keep";
			Check(DecodeAdminAnswer(nullptr, 5, &keep) == Status::Empty, "adminr decode: a null pointer is empty");
			Check(DecodeAdminAnswer("x", 0, &keep) == Status::Empty, "adminr decode: zero bytes are empty");
			Check(da("code" + US + "abc", &keep) == Status::Truncated, "adminr decode: no record separator is truncated");
			Check(da("notme" + US, &keep) == Status::Truncated, "adminr decode: notme with no record separator is truncated");
			Check(da("code" + RS, &keep) == Status::BadRecord, "adminr decode: one field is a bad record");
			Check(da("code" + US + "a" + US + "b" + RS, &keep) == Status::BadRecord, "adminr decode: three fields are a bad record");
			Check(da("code" + US + "a" + RS + "code" + US + "b" + RS, &keep) == Status::BadRecord, "adminr decode: two records are a bad record");
			Check(da("code" + US + RS, &keep) == Status::BadRecord, "adminr decode: a code answer with no code is a bad record");
			Check(da("notme" + US + "x" + RS, &keep) == Status::BadRecord, "adminr decode: a notme answer with a code is a bad record");
			Check(da("ask" + US + RS, &keep) == Status::BadRecord, "adminr decode: an adminq kind is not an adminr kind");
			Check(da("other" + US + "x" + RS, &keep) == Status::BadRecord, "adminr decode: an unknown kind is a bad record");
			Check(da("code" + US + "a\x01" "b" + RS, &keep) == Status::ControlChar, "adminr decode: a control character in the code is refused");
			Check(da("code" + US + "a\x7F" "b" + RS, &keep) == Status::ControlChar, "adminr decode: DEL in the code is refused");
			Check(da("notme" + US + "\t" + RS, &keep) == Status::ControlChar, "adminr decode: a control character in a notme text is refused");
			Check(keep.kind == AdminRKind::Code && keep.code == "keep", "adminr decode: *out is untouched after every error");
			const std::string more = "notme" + US + RS + "junk";
			AdminAnswer a;
			Check(DecodeAdminAnswer(more.data(), 5 + 1 + 1, &a) == Status::Ok && a.kind == AdminRKind::NotMe,
				"adminr decode: only the given length is read");
		}

		// The answer decision: who may answer, and the 500 ms gap.
		{
			Check(kAdminAnswerMinGapMs == 500, "admin answer: the gap is 500 ms");
			Check(DecideAdminAnswer(false, 10000, false, 0) == AdminAnswerDecision::NotAsked, "admin answer: never asked is not asked");
			Check(DecideAdminAnswer(false, 10000, true, 9999) == AdminAnswerDecision::NotAsked, "admin answer: not asked wins over the rate limit");
			Check(DecideAdminAnswer(true, 10000, false, 0) == AdminAnswerDecision::Accept, "admin answer: asked, no earlier answer, is accepted");
			Check(DecideAdminAnswer(true, 0, false, 0) == AdminAnswerDecision::Accept, "admin answer: a first answer at time 0 is accepted");
			Check(DecideAdminAnswer(true, 10000, true, 10000) == AdminAnswerDecision::RateLimited, "admin answer: the same millisecond is limited");
			Check(DecideAdminAnswer(true, 10499, true, 10000) == AdminAnswerDecision::RateLimited, "admin answer: 499 ms after the last is limited");
			Check(DecideAdminAnswer(true, 10500, true, 10000) == AdminAnswerDecision::Accept, "admin answer: 500 ms after the last is accepted");
			Check(DecideAdminAnswer(true, 10501, true, 10000) == AdminAnswerDecision::Accept, "admin answer: 501 ms after the last is accepted");
			Check(DecideAdminAnswer(true, 9000, true, 10000) == AdminAnswerDecision::RateLimited, "admin answer: a clock before the last answer is limited");
			Check(DecideAdminAnswer(true, 10000, true, 0) == AdminAnswerDecision::Accept, "admin answer: a last answer at time 0 is a real time");
			Check(DecideAdminAnswer(true, 100, true, 0) == AdminAnswerDecision::RateLimited, "admin answer: a last answer at time 0 still limits the next 500 ms");
			Check(DecideAdminAnswer(true, 10100, true, 10000, 100) == AdminAnswerDecision::Accept &&
				DecideAdminAnswer(true, 10099, true, 10000, 100) == AdminAnswerDecision::RateLimited, "admin answer: the gap is a parameter");
		}
	}

	// Admin prompt client state (admin_prompt.h): the pure steps behind the player's window.
	{
		using AdminPrompt::State;
		using AdminPrompt::SubmitResult;
		using HostRulesWire::AdminQKind;
		std::string out;

		State s;
		Check(!s.open && !s.final && s.status.empty(), "admin prompt: starts closed and empty");
		Check(AdminPrompt::CheckSubmit(s, "ABC", 1000, &out) == SubmitResult::NotOpen, "admin prompt: a code is refused while closed");
		Check(!AdminPrompt::NotMeSendsAnswer(s, 1000), "admin prompt: not me sends nothing while closed");

		// A line for a prompt that is not showing is dropped.
		AdminPrompt::ApplyQuestion(s, AdminQKind::Info, "late", 0);
		AdminPrompt::ApplyQuestion(s, AdminQKind::Close, "late", 0);
		Check(!s.open && !s.final && s.status.empty(), "admin prompt: info and close with no prompt open change nothing");

		// ask opens; info keeps it open and sets the line; close marks it final.
		AdminPrompt::ApplyQuestion(s, AdminQKind::Ask, "", 0);
		Check(s.open && !s.final && s.status.empty(), "admin prompt: ask opens it");
		AdminPrompt::ApplyQuestion(s, AdminQKind::Info, "Wrong code", 0);
		Check(s.open && !s.final && s.status == "Wrong code", "admin prompt: info sets the line and keeps it open");
		AdminPrompt::ApplyQuestion(s, AdminQKind::Info, "Wrong code, 2 tries left", 0);
		Check(s.open && s.status == "Wrong code, 2 tries left", "admin prompt: a second info replaces the line");

		// Submit: trimming, caps, control characters, the 500 ms gap.
		Check(AdminPrompt::CheckSubmit(s, "  AB12  ", 1000, &out) == SubmitResult::Ok && out == "AB12", "admin prompt: a code is trimmed and accepted");
		Check(AdminPrompt::CheckSubmit(s, nullptr, 1000, &out) == SubmitResult::Empty, "admin prompt: a null code is empty");
		Check(AdminPrompt::CheckSubmit(s, "", 1000, &out) == SubmitResult::Empty, "admin prompt: no code is empty");
		Check(AdminPrompt::CheckSubmit(s, "    ", 1000, &out) == SubmitResult::Empty, "admin prompt: only spaces is empty");
		Check(AdminPrompt::CheckSubmit(s, std::string(64, 'K').c_str(), 1000, &out) == SubmitResult::Ok && out.size() == 64, "admin prompt: 64 bytes are accepted");
		Check(AdminPrompt::CheckSubmit(s, std::string(65, 'K').c_str(), 1000, &out) == SubmitResult::TooLong, "admin prompt: 65 bytes are too long");
		Check(AdminPrompt::CheckSubmit(s, "AB\x01" "CD", 1000, &out) == SubmitResult::ControlChar, "admin prompt: a control character is refused");
		Check(AdminPrompt::CheckSubmit(s, "AB\tCD", 1000, &out) == SubmitResult::ControlChar, "admin prompt: a tab inside the code is refused");
		Check(AdminPrompt::CheckSubmit(s, "AB\x7F" "CD", 1000, &out) == SubmitResult::ControlChar, "admin prompt: DEL is refused");
		Check(AdminPrompt::CheckSubmit(s, "AB CD", 1000, &out) == SubmitResult::Ok && out == "AB CD", "admin prompt: a space inside the code is kept");

		AdminPrompt::NoteAnswerSent(s, 5000);
		Check(AdminPrompt::CheckSubmit(s, "ABC", 5499, &out) == SubmitResult::RateLimited, "admin prompt: a second code within 500 ms is ignored");
		Check(AdminPrompt::CheckSubmit(s, "ABC", 5500, &out) == SubmitResult::Ok, "admin prompt: a code 500 ms after the last is accepted");
		Check(!AdminPrompt::NotMeSendsAnswer(s, 5499) && AdminPrompt::NotMeSendsAnswer(s, 5500), "admin prompt: not me follows the same 500 ms gap");

		// close: final line stays until dismissed; no input, no answer, no later lines.
		AdminPrompt::ApplyQuestion(s, AdminQKind::Close, "You are the admin", 0);
		Check(s.open && s.final && s.status == "You are the admin", "admin prompt: close sets the last line and ends the input");
		Check(AdminPrompt::CheckSubmit(s, "ABC", 9000, &out) == SubmitResult::NotOpen, "admin prompt: no code after the last line");
		Check(!AdminPrompt::NotMeSendsAnswer(s, 9000), "admin prompt: dismissing the last line sends nothing");
		AdminPrompt::ApplyQuestion(s, AdminQKind::Info, "ignored", 0);
		AdminPrompt::ApplyQuestion(s, AdminQKind::Close, "ignored", 0);
		Check(s.final && s.status == "You are the admin", "admin prompt: lines after the last one are dropped");

		// Dismissed, then asked again: ignored for 60 s, then a clean prompt with no answer memory.
		AdminPrompt::CloseLocally(s, 100000);
		Check(!s.open && !s.final && s.status.empty(), "admin prompt: dismissing closes and clears the line");
		AdminPrompt::ApplyQuestion(s, AdminQKind::Ask, "", 100000);
		Check(!s.open, "admin prompt: an ask right after the dismissal is ignored");
		AdminPrompt::ApplyQuestion(s, AdminQKind::Ask, "", 159999);
		Check(!s.open, "admin prompt: an ask 59.999 s after the dismissal is ignored");
		AdminPrompt::ApplyQuestion(s, AdminQKind::Info, "late", 120000);
		AdminPrompt::ApplyQuestion(s, AdminQKind::Close, "late", 120000);
		Check(!s.open && s.status.empty(), "admin prompt: info and close after a dismissal change nothing");
		AdminPrompt::ApplyQuestion(s, AdminQKind::Ask, "", 99000);
		Check(!s.open, "admin prompt: an ask with the clock gone back is ignored");
		AdminPrompt::ApplyQuestion(s, AdminQKind::Ask, "", 160000);
		Check(s.open && !s.final && !s.hasLastAnswer, "admin prompt: an ask 60 s after the dismissal starts clean");

		// The server's own close still applies while the earlier dismissal's latch runs.
		AdminPrompt::ApplyQuestion(s, AdminQKind::Close, "You are the admin", 160001);
		Check(s.open && s.final, "admin prompt: a close from the server applies inside the latch window");
		AdminPrompt::Reset(s);
		AdminPrompt::ApplyQuestion(s, AdminQKind::Ask, "", 5);
		Check(s.open, "admin prompt: no dismissal, no latch");
		Check(!AdminPrompt::AskLatched(false, 5, 5) && AdminPrompt::AskLatched(true, 5, 5), "admin prompt: the latch needs a dismissal");

		// Not me while asking: answer goes out, the window closes.
		Check(AdminPrompt::NotMeSendsAnswer(s, 100), "admin prompt: not me while asking sends an answer");
		AdminPrompt::CloseLocally(s, 100);
		Check(!s.open, "admin prompt: not me closes it locally");

		// Leaving the server clears everything, the latch included.
		AdminPrompt::Reset(s);
		AdminPrompt::ApplyQuestion(s, AdminQKind::Ask, "", 101);
		Check(s.open, "admin prompt: leaving the server lifts the latch");
		AdminPrompt::ApplyQuestion(s, AdminQKind::Info, "line", 0);
		AdminPrompt::NoteAnswerSent(s, 123);
		AdminPrompt::Reset(s);
		Check(!s.open && !s.final && s.status.empty() && !s.hasLastAnswer && s.lastAnswerMs == 0 && !s.hasDismiss, "admin prompt: leaving the server clears the state");
		Check(std::string(AdminPrompt::kPromptText).find("Not me") != std::string::npos, "admin prompt: the fixed text names Not me");
	}

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
