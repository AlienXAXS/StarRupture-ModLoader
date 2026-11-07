// ObservedTests: the locator sr-aobscan reports for a registry row
// (observed.cpp) against the loader's own resolve (ResolveLoaderEntry), over a
// map made in code. The pattern scan is a test double: a pattern is found at
// the RVA the table gives it, or not at all.
#include "test_doubles.h"

#include "../carry.h"
#include "../observed.h"
#include "../plugin_scan.h"
#include "hooks/game/version_map.generated.h"
#include "memory_scanner/image_info.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using ScanValidation::Kind;
using ScanValidation::Request;
using ScanValidation::Result;
using VersionMap::MapFunctionRow;

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

	// The build the map has a row for, and the build the code was written for.
	const VersionMap::Exe kNew{ 128786, 0x22222222, 0x02000000 };
	const VersionMap::Exe kBuilt{ VersionMap::kBuiltCL, 0x11111111, 0x01000000 };

	constexpr const char* kRegistry = "48 8D 6C 24 ?? 48 81 EC 78 01 00 00";          // the code's own pattern
	constexpr const char* kMapRow   = "48 8D AC 24 78 FF FF FF 48 81 EC 88 01 00 00"; // the map's row for kNew

	ScanPatterns::PreflightEntry Entry(const char* name, const char* pattern, ScanPatterns::PatternKind kind)
	{
		ScanPatterns::PreflightEntry e{};
		e.name    = name;
		e.pattern = pattern;
		e.kind    = kind;
		return e;
	}

	Request Ask(const ScanPatterns::PreflightEntry& e)
	{
		Request r;
		r.name    = e.name;
		r.pattern = e.pattern;
		r.kind    = static_cast<Kind>(static_cast<int>(e.kind));
		return r;
	}

	MapFunctionRow Row(const ScanPatterns::PreflightEntry& e, uint32_t cl, const char* pattern)
	{
		MapFunctionRow row;
		row.key     = VersionMap::LoaderEntryKey(e);
		row.kind    = static_cast<int>(e.kind);
		row.fp      = VersionMap::LoaderEntryFingerprint(e);
		row.gameCL  = cl;
		row.pattern = pattern;
		row.rva     = 0;
		return row;
	}

	void UseMap(std::vector<MapFunctionRow> rows)
	{
		auto map = std::make_shared<VersionMap::MapData>();
		map->functions = std::move(rows);
		VersionMap::SetActiveMap(std::move(map));
	}

	uintptr_t Base() { return Scanner::GetImageInfo(GetModuleHandleW(nullptr)).base; }

	// What the harness does for a row: resolve through the loader, then ask
	// which locator matched there.
	AobScan::ObservedLocator Observe(const ScanPatterns::PreflightEntry& e, const VersionMap::Exe& exe, Result& resolved)
	{
		const Request request = Ask(e);
		resolved = VersionMap::ResolveLoaderEntry(e, request, exe);
		if (!resolved.Succeeded())
			return {};
		return AobScan::ObserveLoaderEntry(e, request, exe, resolved.address);
	}

	AobScan::ObservedLocator At(const ScanPatterns::PreflightEntry& e, const VersionMap::Exe& exe, uintptr_t rva)
	{
		return AobScan::ObserveLoaderEntry(e, Ask(e), exe, Base() + rva);
	}

	// The last "[Map]" line the loader logged: which locators it took, in the order it tried them.
	std::wstring LastPick()
	{
		for (size_t i = TestDoubles::g_log.size(); i-- > 0; )
			if (TestDoubles::g_log[i].rfind(L"[Map]", 0) == 0)
				return TestDoubles::g_log[i];
		return {};
	}

	// A plugin's map-key request: the key is its hook name for a global kind,
	// and the plugin's own site, "<owner>/<name>", under the request's
	// fingerprint for any other.
	constexpr const char* kPlugin = "Codex";

	Request AskPlugin(const char* name, const char* pattern, Kind kind)
	{
		Request r;
		r.name    = name;
		r.pattern = pattern;
		r.kind    = kind;
		return r;
	}

	MapFunctionRow PluginRow(const Request& r, uint32_t cl, const char* pattern)
	{
		const bool site = r.kind != Kind::FunctionStart && r.kind != Kind::Data;
		MapFunctionRow row;
		row.key     = site ? std::string(kPlugin) + "/" + r.name : r.name;
		row.kind    = static_cast<int>(r.kind);
		row.fp      = site ? VersionMap::Fingerprint(static_cast<int>(r.kind), r.pattern, 0, -1, -1, 0) : 0;
		row.gameCL  = cl;
		row.pattern = pattern;
		return row;
	}

	// What the harness does for a plugin's request: resolve through the loader,
	// then ask which locator matched there.
	AobScan::ObservedLocator ObservePlugin(const Request& r, const VersionMap::Exe& exe, Result& resolved)
	{
		resolved = VersionMap::ResolveMapKey(kPlugin, r, {}, exe, false);
		if (!resolved.Succeeded())
			return {};
		const MapFunctionRow row = PluginRow(r, 0, "");
		return AobScan::ObservePluginRequest(kPlugin, row.key, row.fp, r, exe, resolved.address);
	}

	// The carry report (sr-aobscan --carry): the row the map would need for a
	// build it has none for, against the loader's own resolve of the same key.
	const AobScan::CarryRow* FindCarry(const AobScan::Carry& carry, const std::string& key)
	{
		for (const AobScan::CarryRow& row : carry.rows)
			if (row.key == key)
				return &row;
		return nullptr;
	}

	bool Has(const std::string& text, const char* part)
	{
		return text.find(part) != std::string::npos;
	}
}

int main()
{
	using ScanPatterns::PatternKind;
	const ScanPatterns::PreflightEntry widget = Entry("UCrUW_MainMenuWidget::NativeConstruct", kRegistry, PatternKind::FunctionStart);
	Result r;

	// The map's pattern lands, and the registry's lands on the same address.
	TestDoubles::g_rvas = { { kRegistry, 0x98B1640 }, { kMapRow, 0x98B1640 } };
	UseMap({ Row(widget, 128786, kMapRow) });
	AobScan::ObservedLocator seen = Observe(widget, kNew, r);
	Check(r.Succeeded() && r.address == Base() + 0x98B1640, "the loader resolves the row");
	Check(seen.found && seen.fromMap && seen.pattern == kMapRow,
		"a map row that decided the address: its pattern, not the registry's");

	// The registry's pattern lands nowhere (the 127004 shape on 128786).
	TestDoubles::g_rvas = { { kMapRow, 0x98B1640 } };
	seen = Observe(widget, kNew, r);
	Check(r.Succeeded() && seen.found && seen.fromMap && seen.pattern == kMapRow,
		"the registry's pattern matches nowhere: the map row's");

	// The map's row has a result offset and a follow: all of it is the row's.
	{
		MapFunctionRow row = Row(widget, 128786, kMapRow);
		row.resultOffset  = 0x10;
		row.followRel32At = 3;
		UseMap({ row });
		TestDoubles::g_rvas = { { kMapRow, 0x98B1640 } };
		const AobScan::ObservedLocator s = At(widget, kNew, 0x98B1650);
		Check(s.found && s.fromMap && s.resultOffset == 0x10 && s.followRel32At == 3,
			"the row's offset and follow come with its pattern");
	}

	// No row for this build: the registry's request.
	UseMap({ Row(widget, 127004, kMapRow) });
	TestDoubles::g_rvas = { { kRegistry, 0x1000 }, { kMapRow, 0x2000 } };
	seen = Observe(widget, kNew, r);
	Check(r.Succeeded() && seen.found && !seen.fromMap && seen.pattern == kRegistry, "no row for the build: the registry's pattern");

	// The build the code was written for: the code's pattern stands in for the row's.
	UseMap({ Row(widget, VersionMap::kBuiltCL, kMapRow) });
	TestDoubles::g_rvas = { { kRegistry, 0x1000 }, { kMapRow, 0x1000 } };
	seen = Observe(widget, kBuilt, r);
	Check(r.Succeeded() && seen.found && !seen.fromMap && seen.pattern == kRegistry, "the built CL: the registry's pattern");

	// An absent row, or a row of another kind, is not this row.
	{
		MapFunctionRow absent = Row(widget, 128786, "");
		absent.absent = true;
		UseMap({ absent });
		TestDoubles::g_rvas = { { kRegistry, 0x1000 } };
		const AobScan::ObservedLocator s = At(widget, kNew, 0x1000);
		Check(s.found && !s.fromMap && s.pattern == kRegistry, "an absent row is skipped");

		MapFunctionRow other = Row(widget, 128786, kMapRow);
		other.kind = static_cast<int>(PatternKind::Data);
		UseMap({ other });
		TestDoubles::g_rvas = { { kRegistry, 0x1000 }, { kMapRow, 0x1000 } };
		const AobScan::ObservedLocator t = At(widget, kNew, 0x1000);
		Check(t.found && !t.fromMap && t.pattern == kRegistry, "a row of another kind is skipped");
	}

	// A map row that lands elsewhere is not the observation.
	UseMap({ Row(widget, 128786, kMapRow) });
	TestDoubles::g_rvas = { { kRegistry, 0x1000 }, { kMapRow, 0x2000 } };
	{
		const AobScan::ObservedLocator s = At(widget, kNew, 0x1000);
		Check(s.found && !s.fromMap && s.pattern == kRegistry, "a map row at another address is not the observation");
	}

	// Nothing known to land on the address (an address-only map row): no pattern.
	UseMap({ Row(widget, 128786, "") });
	TestDoubles::g_rvas = {};
	{
		const AobScan::ObservedLocator s = At(widget, kNew, 0x1000);
		Check(!s.found && s.pattern.empty(), "no pattern lands on the address: none is reported");
	}

	// A build the map has no row for is decided by the latest other build's row,
	// after the registry's request.
	{
		const VersionMap::Exe next{ 128786 + 1000, 0x33333333, 0x03000000 };
		constexpr const char* kOlder = "48 89 5C 24 08 57 48 83 EC 20";

		UseMap({ Row(widget, VersionMap::kBuiltCL, kRegistry), Row(widget, 128786, kMapRow) });
		TestDoubles::g_rvas = { { kMapRow, 0x98B4480 } };
		seen = Observe(widget, next, r);
		Check(r.Succeeded() && r.address == Base() + 0x98B4480, "the loader resolves a new build through the latest row");
		Check(seen.found && seen.fromMap && seen.pattern == kMapRow,
			"a new build: the latest other build's row that decided the address");

		TestDoubles::g_rvas = { { kRegistry, 0x98B4480 }, { kMapRow, 0x98B4480 } };
		seen = Observe(widget, next, r);
		Check(r.Succeeded() && seen.found && !seen.fromMap && seen.pattern == kRegistry,
			"a new build where both agree: the registry's pattern, tried first");

		// Only the latest other build's row is used, not an older one.
		UseMap({ Row(widget, 128000, kOlder), Row(widget, 128786, kMapRow) });
		TestDoubles::g_rvas = { { kOlder, 0x5000 } };
		seen = Observe(widget, next, r);
		Check(!r.Succeeded(), "the loader does not use an older build's row");
		Check(!At(widget, next, 0x5000).found, "an older build's row is not observed");

		// Absent on a build newer than the latest row: that row is not used.
		MapFunctionRow gone = Row(widget, 128900, "");
		gone.absent = true;
		gone.owners = "ModLoader";
		UseMap({ Row(widget, 128786, kMapRow), gone });
		TestDoubles::g_rvas = { { kMapRow, 0x6000 } };
		seen = Observe(widget, next, r);
		Check(!r.Succeeded(), "the loader does not use a row older than an absent one");
		Check(!At(widget, next, 0x6000).found, "a row older than an absent one is not observed");
	}

	// A site (kind InFunction) is kept under its fingerprint.
	const ScanPatterns::PreflightEntry site = Entry("Test::Site", "B0 01", PatternKind::InFunction);
	UseMap({ Row(site, 128786, "B0 02") });
	TestDoubles::g_rvas = { { "B0 01", 0x7000 }, { "B0 02", 0x7000 } };
	seen = Observe(site, kNew, r);
	Check(r.Succeeded() && seen.found && seen.fromMap && seen.pattern == "B0 02", "a site: the map's row for the fingerprint");

	// A site with arithmetic uses no row with another pattern.
	{
		ScanPatterns::PreflightEntry patched = site;
		patched.siteArithmetic = 1;
		const VersionMap::Exe next{ 128786 + 1000, 0x33333333, 0x03000000 };
		UseMap({ Row(patched, 128786, "B0 02") });
		TestDoubles::g_rvas = { { "B0 02", 0x7000 } };
		seen = Observe(patched, next, r);
		Check(!r.Succeeded(), "the loader does not use another pattern for a site with arithmetic");
		Check(!At(patched, next, 0x7000).found, "another pattern for a site with arithmetic is not observed");
	}

	// The absent rows. A global key's absent row binds the owners it lists, and a
	// build newer than the latest row that the key is absent on withdraws that
	// row. An absent row that lists other owners binds nothing here.
	{
		const VersionMap::Exe next{ 128786 + 1000, 0x33333333, 0x03000000 };

		// Absent on the running build for other owners: none of the key's rows is
		// used, and the request resolves on its own pattern.
		MapFunctionRow elsewhere = Row(widget, 128786, "");
		elsewhere.absent = true;
		elsewhere.owners = "BetterCheats";
		UseMap({ Row(widget, 128000, kMapRow), elsewhere });
		TestDoubles::g_rvas = { { kRegistry, 0x1000 }, { kMapRow, 0x2000 } };
		seen = Observe(widget, kNew, r);
		Check(r.Succeeded() && r.address == Base() + 0x1000,
			"absent on the running build for other owners: the request resolves on its own pattern");
		Check(seen.found && !seen.fromMap && seen.pattern == kRegistry,
			"absent on the running build for other owners: no row of the key is observed");

		// Absent on the running build and listing the loader: refused.
		MapFunctionRow refused = elsewhere;
		refused.owners = "BetterCheats,ModLoader";
		UseMap({ Row(widget, 128000, kMapRow), refused });
		seen = Observe(widget, kNew, r);
		Check(!r.Succeeded() && !seen.found, "absent on the running build for the loader: refused, nothing observed");

		// Absent on a newer build for other owners: the latest row is still used.
		MapFunctionRow later = Row(widget, 128900, "");
		later.absent = true;
		later.owners = "BetterCheats";
		UseMap({ Row(widget, 128786, kMapRow), later });
		TestDoubles::g_rvas = { { kMapRow, 0x6000 } };
		seen = Observe(widget, next, r);
		Check(r.Succeeded() && r.address == Base() + 0x6000,
			"absent on a newer build for other owners: the loader still uses the latest row");
		Check(seen.found && seen.fromMap && seen.pattern == kMapRow,
			"absent on a newer build for other owners: the latest row is observed");

		// The same list naming the loader withdraws it.
		later.owners = "BetterCheats,ModLoader";
		UseMap({ Row(widget, 128786, kMapRow), later });
		seen = Observe(widget, next, r);
		Check(!r.Succeeded() && !seen.found, "absent on a newer build for the loader: the latest row is withdrawn");

		// A site's absent row binds its fingerprint, whatever owners it lists.
		const ScanPatterns::PreflightEntry spot = Entry("Test::Spot", "B0 01", PatternKind::InFunction);
		MapFunctionRow spotLater = Row(spot, 128900, "");
		spotLater.absent = true;
		spotLater.owners = "BetterCheats";
		UseMap({ Row(spot, 128786, "B0 02"), spotLater });
		TestDoubles::g_rvas = { { "B0 02", 0x7000 } };
		seen = Observe(spot, next, r);
		Check(!r.Succeeded() && !seen.found, "a site's absent row on a newer build withdraws the latest row, whatever owners it lists");
	}

	// The loader's agreement order, which no result shows (the locators that
	// resolve all give one address): the request's own pattern first, then the
	// map's rows, as its "[Map]" line lists them. The scanner tries them in the
	// same order, so the pattern it reports when both agree is the request's.
	{
		const VersionMap::Exe next{ 128786 + 1000, 0x33333333, 0x03000000 };
		UseMap({ Row(widget, 128786, kMapRow) });
		TestDoubles::g_rvas = { { kRegistry, 0x98B4480 }, { kMapRow, 0x98B4480 } };
		TestDoubles::g_log.clear();
		seen = Observe(widget, next, r);
		const std::wstring pick = LastPick();
		const size_t own   = pick.find(L"own pattern");
		const size_t rowAt = pick.find(L"loader map");
		Check(r.Succeeded() && own != std::wstring::npos && rowAt != std::wstring::npos && own < rowAt,
			"the loader tries the request's own pattern before the map's rows");
		Check(seen.found && !seen.fromMap && seen.pattern == kRegistry,
			"the scanner reports the first the loader tried when both agree");
	}

	// A plugin's map-key request (sr-aobscan --plugin). Its own pattern can be
	// missing on a build the map's latest row still resolves, and a row made
	// from that pattern would be refused on the exe it was verified on.
	{
		const VersionMap::Exe next{ 128786 + 1000, 0x33333333, 0x03000000 };
		constexpr const char* kOwn    = "48 89 5C 24 08 57 48 83 EC 30";
		constexpr const char* kLatest = "48 89 5C 24 10 57 48 83 EC 30";

		// Global key: the own pattern matches nowhere, the latest row lands.
		const Request hook = AskPlugin("UFoo::Bar", kOwn, Kind::FunctionStart);
		UseMap({ PluginRow(hook, 128786, kLatest) });
		TestDoubles::g_rvas = { { kLatest, 0x4000 } };
		TestDoubles::g_counts = {};
		Check(!ScanValidation::Resolve(hook).Succeeded(), "the plugin's own pattern matches nowhere on this build");
		AobScan::ObservedLocator got = ObservePlugin(hook, next, r);
		Check(r.Succeeded() && r.address == Base() + 0x4000, "the loader resolves a plugin key through the latest row");
		Check(got.found && got.fromMap && got.pattern == kLatest,
			"a plugin key resolved by a row: the row's pattern, not its own missing one");

		// The own pattern lands on the address too: it is the first tried.
		TestDoubles::g_rvas = { { kOwn, 0x4000 }, { kLatest, 0x4000 } };
		got = ObservePlugin(hook, next, r);
		Check(r.Succeeded() && got.found && !got.fromMap && got.pattern == kOwn,
			"a plugin key whose own pattern lands: its own pattern");

		// Neither pattern lands on the address given: nothing to report.
		TestDoubles::g_rvas = { { kOwn, 0x1000 }, { kLatest, 0x2000 } };
		got = AobScan::ObservePluginRequest(kPlugin, "UFoo::Bar", 0, hook, next, Base() + 0x3000);
		Check(!got.found && got.pattern.empty(), "no pattern lands on the plugin key's address: none is reported");

		// A row with a result offset and a follow carries them with its pattern.
		MapFunctionRow shifted = PluginRow(hook, 128786, kLatest);
		shifted.resultOffset  = 0x10;
		shifted.followRel32At = 3;
		UseMap({ shifted });
		TestDoubles::g_rvas = { { kLatest, 0x4000 } };
		got = AobScan::ObservePluginRequest(kPlugin, "UFoo::Bar", 0, hook, next, Base() + 0x4010);
		Check(got.found && got.fromMap && got.resultOffset == 0x10 && got.followRel32At == 3,
			"a plugin key's row brings its offset and follow with its pattern");

		// A global key's absent row on a newer build binds the owners it lists.
		MapFunctionRow gone = PluginRow(hook, 128900, "");
		gone.absent = true;
		gone.owners = "BetterCheats";
		UseMap({ PluginRow(hook, 128786, kLatest), gone });
		TestDoubles::g_rvas = { { kLatest, 0x4000 } };
		got = ObservePlugin(hook, next, r);
		Check(r.Succeeded() && got.found && got.fromMap && got.pattern == kLatest,
			"a plugin key absent on a newer build for other plugins: the latest row stands");
		gone.owners = "ModLoader";   // the loader's own rows are not a plugin's
		UseMap({ PluginRow(hook, 128786, kLatest), gone });
		got = ObservePlugin(hook, next, r);
		Check(r.Succeeded() && got.found && got.fromMap && got.pattern == kLatest,
			"a plugin key absent on a newer build for the loader only: the latest row stands");
		gone.owners = std::string("BetterCheats,") + kPlugin;
		UseMap({ PluginRow(hook, 128786, kLatest), gone });
		got = ObservePlugin(hook, next, r);
		Check(!r.Succeeded() && !got.found, "a plugin key absent on a newer build for the plugin: the latest row is withdrawn");

		// The plugin's own site: kept under the fingerprint of the request.
		const Request spot = AskPlugin("Spot", kOwn, Kind::InFunction);
		UseMap({ PluginRow(spot, 128786, kLatest) });
		TestDoubles::g_rvas = { { kLatest, 0x5000 } };
		got = ObservePlugin(spot, next, r);
		Check(r.Succeeded() && got.found && got.fromMap && got.pattern == kLatest,
			"a plugin site resolved by a row under its fingerprint: the row's pattern");

		// A row under another fingerprint is another request's: not observed.
		const Request other = AskPlugin("Spot", "48 89 5C 24 18", Kind::InFunction);
		UseMap({ PluginRow(other, 128786, kLatest) });
		got = AobScan::ObservePluginRequest(kPlugin, "Codex/Spot", PluginRow(spot, 0, "").fp, spot, next, Base() + 0x5000);
		Check(!got.found, "a plugin site's row under another fingerprint is not observed");
	}

	// sr-aobscan --carry: a key with no row for the exe's build, resolved as the
	// loader does at run time, which reads only the latest earlier build's row.
	{
		const VersionMap::Exe next{ 129402, 0x44444444, 0x04000000 };
		const HMODULE module = GetModuleHandleW(nullptr);
		constexpr const char* kOld    = "48 89 5C 24 08 57 48 83 EC 20";
		constexpr const char* kNewest = "48 89 5C 24 10 57 48 83 EC 20";
		const Request hook = AskPlugin("UFoo::Bar", "", Kind::FunctionStart);
		const Request twice = AskPlugin("UFoo::Twice", "", Kind::FunctionStart);
		const Request nowhere = AskPlugin("UFoo::Other", "", Kind::FunctionStart);
		const Request gone = AskPlugin("UFoo::Gone", "", Kind::FunctionStart);

		// The newest earlier row lands: its pattern, its address, the loader's answer.
		UseMap({ PluginRow(hook, 127004, kOld), PluginRow(hook, 128786, kNewest) });
		TestDoubles::g_rvas = { { kOld, 0x1000 }, { kNewest, 0x4000 } };
		TestDoubles::g_counts = {};
		AobScan::Carry carry = AobScan::ScanCarry(module, next);
		const AobScan::CarryRow* got = FindCarry(carry, "UFoo::Bar");
		Check(carry.cl == 129402 && carry.keys == 1 && carry.withRow == 0 && carry.rows.size() == 1, "carry: one key, none rowed");
		Check(got && got->result == AobScan::CarryResult::Match && got->fromCL == 128786 && got->matches == 1
			&& got->hasRva && got->rva == 0x4000 && got->pattern == kNewest && got->reason.empty(),
			"carry: the newest earlier row's pattern, unique");
		Result loader = VersionMap::ResolveMapKey("Codex", hook, {}, next, false);
		Check(loader.Succeeded() && got && loader.address == Base() + got->rva, "carry: the same address as the loader's run-time resolve");

		// The newest row's pattern misses and an older one would land: the loader
		// reads no older row, so neither does the report.
		TestDoubles::g_rvas = { { kOld, 0x1000 } };
		carry = AobScan::ScanCarry(module, next);
		got = FindCarry(carry, "UFoo::Bar");
		Check(got && got->result == AobScan::CarryResult::None && got->fromCL == 128786 && !got->hasRva && got->matches == 0
			&& !got->reason.empty(), "carry: the newest row misses: none, an older row is not tried");
		Check(!VersionMap::ResolveMapKey("Codex", hook, {}, next, false).Succeeded(), "carry: the loader does not use the older row either");

		// The newest earlier row has no pattern (an address for its own exe), so
		// the latest row that has one is the older: the loader's choice.
		MapFunctionRow addressOnly = PluginRow(hook, 128786, "");
		addressOnly.imageStamp = 0xCF8579A6;
		addressOnly.imageSize  = 0x0F47D000;
		addressOnly.rva        = 0x7000;
		UseMap({ PluginRow(hook, 127004, kOld), addressOnly });
		TestDoubles::g_rvas = { { kOld, 0x1000 } };
		carry = AobScan::ScanCarry(module, next);
		got = FindCarry(carry, "UFoo::Bar");
		loader = VersionMap::ResolveMapKey("Codex", hook, {}, next, false);
		Check(got && got->result == AobScan::CarryResult::Match && got->fromCL == 127004 && got->rva == 0x1000,
			"carry: a newest row with no pattern: the older row that has one");
		Check(loader.Succeeded() && got && loader.address == Base() + got->rva, "carry: the loader picks that older row too");

		// Matches nowhere, found in many places, and a newer absent row that binds an owner.
		MapFunctionRow absentNewer = PluginRow(gone, 128900, "");
		absentNewer.absent = true;
		absentNewer.owners = "Codex";
		UseMap({ PluginRow(twice, 128786, kNewest), PluginRow(nowhere, 128786, kOld),
			PluginRow(gone, 128786, kNewest), absentNewer });
		TestDoubles::g_rvas = { { kNewest, 0x4000 } };
		TestDoubles::g_counts = { { kNewest, 3 } };
		carry = AobScan::ScanCarry(module, next);
		const AobScan::CarryRow* many = FindCarry(carry, "UFoo::Twice");
		const AobScan::CarryRow* none = FindCarry(carry, "UFoo::Other");
		got = FindCarry(carry, "UFoo::Gone");
		Check(many && many->result == AobScan::CarryResult::Ambiguous && many->matches == 3 && !many->hasRva && many->fromCL == 128786
			&& !many->reason.empty(), "carry: a pattern found in many places: ambiguous, with the count");
		Check(none && none->result == AobScan::CarryResult::None && none->matches == 0 && none->fromCL == 128786 && !none->reason.empty(),
			"carry: a pattern found nowhere: none");
		Check(got && got->owners == "Codex", "carry: a newer absent row's owners are reported, for the loader withdraws the row for them");

		// A key with no pattern row to carry.
		const Request only = AskPlugin("UFoo::Only", "", Kind::FunctionStart);
		MapFunctionRow onlyAbsent = PluginRow(only, 128786, "");
		onlyAbsent.absent = true;
		UseMap({ onlyAbsent });
		carry = AobScan::ScanCarry(module, next);
		got = FindCarry(carry, "UFoo::Only");
		Check(got && got->result == AobScan::CarryResult::None && got->fromCL == 0 && Has(got->reason, "no locator to carry"),
			"carry: a key with no pattern row to carry: none, no row tried");

		// A key that already has a row for the build is skipped, an absent row
		// for it too, and both are counted.
		MapFunctionRow rowed = PluginRow(hook, 129402, kNewest);
		MapFunctionRow absentHere = PluginRow(gone, 129402, "");
		absentHere.absent = true;
		UseMap({ PluginRow(hook, 128786, kNewest), rowed, PluginRow(gone, 128786, kNewest), absentHere,
			PluginRow(twice, 128786, kNewest) });
		TestDoubles::g_counts = {};
		TestDoubles::g_rvas = { { kNewest, 0x4000 } };
		carry = AobScan::ScanCarry(module, next);
		Check(carry.keys == 3 && carry.withRow == 2 && carry.absentRow == 1 && carry.rows.size() == 1
			&& carry.rows[0].key == "UFoo::Twice", "carry: a key with a row for the build is skipped, and counted");

		// The plugin's own site: kept under its fingerprint.
		const Request spot = AskPlugin("Spot", "B0 01", Kind::InFunction);
		UseMap({ PluginRow(spot, 128786, kNewest) });
		TestDoubles::g_rvas = { { kNewest, 0x5000 } };
		carry = AobScan::ScanCarry(module, next);
		got = FindCarry(carry, "Codex/Spot");
		Check(got && got->result == AobScan::CarryResult::Match && got->fp == PluginRow(spot, 0, "").fp && got->fp != 0
			&& got->kind == static_cast<int>(Kind::InFunction) && got->rva == 0x5000, "carry: a site keeps its kind and fingerprint");

		// A row's offset and follow come with its pattern.
		MapFunctionRow shifted = PluginRow(hook, 128786, kNewest);
		shifted.resultOffset  = 0x10;
		shifted.followRel32At = 3;
		UseMap({ shifted });
		TestDoubles::g_rvas = { { kNewest, 0x4000 } };
		carry = AobScan::ScanCarry(module, next);
		got = FindCarry(carry, "UFoo::Bar");
		Check(got && got->result == AobScan::CarryResult::Match && got->rva == 0x4010 && got->resultOffset == 0x10 && got->followRel32At == 3,
			"carry: the row's offset and follow apply and are reported");

		// The JSON the pipeline reads.
		UseMap({ PluginRow(hook, 128786, kNewest), PluginRow(nowhere, 128786, kOld), PluginRow(twice, 128786, kNewest) });
		TestDoubles::g_rvas = { { kNewest, 0x4000 } };
		TestDoubles::g_counts = {};
		carry = AobScan::ScanCarry(module, next);
		const std::string json = AobScan::CarryJson(carry);
		Check(Has(json, "\"kind\": \"carry\"") && Has(json, "\"cl\": 129402") && Has(json, "\"rows\": ["), "carry JSON: kind, cl and rows");
		Check(Has(json, "\"summary\": { \"keys\": 3, \"withRow\": 0, \"absentRow\": 0, \"carried\": 3, \"match\": 2, \"none\": 1, \"ambiguous\": 0 }"),
			"carry JSON: the summary");
		Check(Has(json, "{ \"key\": \"UFoo::Bar\", \"fromCL\": 128786, \"matches\": 1, \"rva\": \"0x00004000\", \"result\": \"match\", \"kind\": 1, \"fp\": \"0x00000000\", "
			"\"pattern\": \"48 89 5C 24 10 57 48 83 EC 20\", \"resultOffset\": 0, \"followRel32At\": -1, \"followRipDispAt\": -1 }"),
			"carry JSON: a matched key, with no reason");
		Check(Has(json, "{ \"key\": \"UFoo::Other\", \"fromCL\": 128786, \"matches\": 0, \"rva\": null, \"result\": \"none\", \"reason\": \""),
			"carry JSON: a key that does not match: rva null, a reason");
	}

	VersionMap::SetActiveMap(nullptr);

	// The plugin's own pattern (sr-aobscan --plugin, plugin_scan.cpp): a request
	// that resolved passes only when its own pattern matches the exe once,
	// whatever the map held for it.
	Check(AobScan::OwnPatternLabel(1) == nullptr, "an own pattern that matches once is not graded");
	Check(AobScan::OwnPatternLabel(0) && !strcmp(AobScan::OwnPatternLabel(0), "OWN-MISSING"), "an own pattern that matches nowhere is OWN-MISSING");
	Check(AobScan::OwnPatternLabel(2) && !strcmp(AobScan::OwnPatternLabel(2), "OWN-AMBIGUOUS"), "an own pattern that matches twice is OWN-AMBIGUOUS");
	Check(AobScan::OwnPatternLabel(64) && !strcmp(AobScan::OwnPatternLabel(64), "OWN-AMBIGUOUS"), "an own pattern that fills the count cap is OWN-AMBIGUOUS");

	// Which rows are graded on it.
	Check(AobScan::OwnPatternGraded("Resolve", true, "OK", 1, false), "a resolved Resolve request is graded");
	Check(AobScan::OwnPatternGraded("Resolve", true, "OK", 3, false), "a Resolve request with locators for other builds is graded on its own pattern");
	Check(AobScan::OwnPatternGraded("ResolveRequired", true, "OK", 1, false), "a resolved single-pattern legacy call is graded");
	Check(!AobScan::OwnPatternGraded("ResolveRequiredUnique", true, "OK", 2, false), "a call that lists alternatives is not graded");
	Check(AobScan::OwnPatternGraded("ResolveOptionalUnique", true, "OK", 1, false), "a Unique call with one pattern is graded");
	Check(!AobScan::OwnPatternGraded("Resolve", true, "OK", 1, true), "a map key with no pattern is not graded");
	Check(!AobScan::OwnPatternGraded("Resolve", true, "OK", 0, true), "a request with no patterns is not graded");
	Check(!AobScan::OwnPatternGraded("Resolve", true, "AMBIGUOUS", 1, false), "a request that failed is left to its own result");
	Check(!AobScan::OwnPatternGraded("FindAllPatternsInMainModule", false, "OK", 1, false), "a raw scan is not graded");

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
