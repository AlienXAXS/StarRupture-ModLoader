#pragma once
// sr-aobscan --layout: reads class layouts from the game exe's own PDB for a
// list of keys. See layout.cpp.

namespace AobScan
{
	// keysPath: one key per line, "Class::Member" or "Class" (the class's size).
	// Returns kExitOk when every key resolved, kExitFailed when one did not,
	// kExitBadInput when the key list is empty or unreadable, or the exe has no matching PDB.
	int ScanLayout(const wchar_t* exePath, const wchar_t* keysPath, bool json);

	// The field source for the loader's ResolveField in --plugin mode
	// (VersionMap::SetLiveFieldSource): the exe's own PDB stands in for the
	// game's reflection data, which an exe that is not running does not have.
	// The PDB is loaded on the first lookup.
	void UseLayoutForFields(const wchar_t* exePath);
}
