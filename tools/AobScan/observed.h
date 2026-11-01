#pragma once
// sr-aobscan: the locator that matched at a registry row's address.
//
// The loader decides a registry row with the map's row for the exe's build
// first (VersionMap::ResolveLoaderEntry), so the address a scan reports can
// come from a pattern that is not the one the registry carries. The JSON
// carries the locator that matched, not the registry's, so that a version-map
// row made from it matches its own address on its own exe.

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
		bool        fromMap = false; // the map's row for the exe, not the registry's request
		std::string pattern;
		int32_t     resultOffset    = 0;
		int32_t     followRel32At   = -1;
		int32_t     followRipDispAt = -1;
	};

	// `address` is the final address ResolveLoaderEntry gave for `request`. The
	// map's row for this exe's build is tried first, as the loader tries it, then
	// the request; the first whose pattern resolves to `address` is the one
	// observed. On the build the code was written for the registry's pattern
	// stands in for the row's, as in the loader.
	ObservedLocator ObserveLoaderEntry(const ScanPatterns::PreflightEntry& entry,
		const ScanValidation::Request& request, const VersionMap::Exe& exe, uintptr_t address);
}
