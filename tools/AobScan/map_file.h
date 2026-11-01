#pragma once
// sr-aobscan --map: reads a version map to resolve against, in place of the
// one compiled into the loader. See map_file.cpp.

#include "memory_scanner/version_map.h"

#include <string>

namespace AobScan
{
	// A generated header (hooks/game/version_map.generated.h) when the path ends
	// in ".h", otherwise a feed: the text of version-map-client.v1.txt, with or
	// without its trailing `sig` line (the signature is not checked here).
	// False with `error` set when the file cannot be read or parsed.
	bool ReadMapFile(const std::wstring& path, VersionMap::MapData& out, std::string& error);
}
