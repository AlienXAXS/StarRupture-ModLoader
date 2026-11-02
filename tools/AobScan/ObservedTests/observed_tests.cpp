// ObservedTests: the locator sr-aobscan reports for a registry row
// (observed.cpp) against the loader's own resolve (ResolveLoaderEntry), over a
// map made in code. The pattern scan is a test double: a pattern is found at
// the RVA the table gives it, or not at all.
#include "test_doubles.h"

#include "../observed.h"
#include "../plugin_scan.h"
#include "hooks/game/version_map.generated.h"
#include "memory_scanner/image_info.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

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

	// A site (kind InFunction) is kept under its fingerprint.
	const ScanPatterns::PreflightEntry site = Entry("Test::Site", "B0 01", PatternKind::InFunction);
	UseMap({ Row(site, 128786, "B0 02") });
	TestDoubles::g_rvas = { { "B0 01", 0x7000 }, { "B0 02", 0x7000 } };
	seen = Observe(site, kNew, r);
	Check(r.Succeeded() && seen.found && seen.fromMap && seen.pattern == "B0 02", "a site: the map's row for the fingerprint");

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
