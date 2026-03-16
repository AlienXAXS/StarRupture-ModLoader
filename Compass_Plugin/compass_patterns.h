#pragma once

// ---------------------------------------------------------------------------
// Compass_Plugin — byte pattern registry
// Use IDA-style format: "48 89 5C 24 ?? 57 48 83 EC 20"  (?? = wildcard byte)
// ---------------------------------------------------------------------------

namespace CompassPatterns
{
	// AHUD::PostRender — called every frame; hud->Canvas is valid inside.
	// ACrHUD does not override this, so AHUD::PostRender is the hook target.
	// Signature: void AHUD::PostRender()
	static constexpr const char* AHUD_PostRender = "40 55 53 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0";

	// StaticLoadObject — CoreUObject free function that synchronously loads (or
	// returns the already-loaded instance of) any UObject asset by full path.
	// Signature: UObject* StaticLoadObject(UClass*, UObject* Outer, const wchar_t* Name,
	//                                      const wchar_t* Filename, uint32 LoadFlags,
	//                                      UPackageMap*, bool bAllowReconciliation,
	//                                      const FLinkerInstancingContext*)
	// TODO: fill in the actual byte pattern once found in IDA / the game binary.
	static constexpr const char* StaticLoadObject = "40 55 53 56 41 54 41 55 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 0F B6 85";
}
