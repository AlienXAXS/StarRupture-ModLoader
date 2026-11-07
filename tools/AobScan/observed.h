#pragma once
// sr-aobscan: the locator that matched at a registry row's address.
//
// The loader decides a registry row, and a plugin's map-key request, with the
// map's rows (VersionMap::ResolveLoaderEntry, ResolveMapKey), so the address a
// scan reports can come from a pattern that is not the one the code carries.
// The JSON carries the locator that matched, not the code's, so that a
// version-map row made from it matches its own address on its own exe.

#include <cstdint>

#include "hooks/game/scan_patterns.h"
#include "memory_scanner/scan_validation.h"
#include "memory_scanner/version_map.h"

#include <string>

namespace AobScan
{
	struct ObservedLocator
	{
		bool        found = false;   // false: no pattern is known to land on the address (an address-only map row)
		bool        fromMap = false; // a row of the map, not the registry's request
		std::string pattern;
		int32_t     resultOffset    = 0;
		int32_t     followRel32At   = -1;
		int32_t     followRipDispAt = -1;
	};

	// `address` is the final address ResolveLoaderEntry gave for `request`. The
	// locators are tried in the loader's order: the map's row for this exe's
	// build, then the request, then the map's row for the latest other build
	// (a build the map has no row for is decided by that one); the first whose
	// pattern resolves to `address` is the one observed. On the build the code
	// was written for the registry's pattern stands in for the row's, as in the
	// loader. ObservedTests pins which rows are used, and the address each
	// order gives, against ResolveLoaderEntry's; the order the loader tries the
	// agreeing locators in is mirrored here and cannot be seen from outside.
	ObservedLocator ObserveLoaderEntry(const ScanPatterns::PreflightEntry& entry,
		const ScanValidation::Request& request, const VersionMap::Exe& exe, uintptr_t address);

	// The same for a plugin's map-key request that resolved at `address`: the
	// map's rows for `key` (the plugin's own site, or a global key) and `fp`,
	// as VersionMap::ResolveMapKey reads them for `owner`, and the request's
	// own pattern. found is false when none of them lands on the address, so a
	// row made from it would carry no pattern the loader accepts there.
	ObservedLocator ObservePluginRequest(const char* owner, const std::string& key, uint32_t fp,
		const ScanValidation::Request& request, const VersionMap::Exe& exe, uintptr_t address);
}
