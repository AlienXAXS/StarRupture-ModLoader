#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// FText::AsLocalizable_Advanced Hook
//
// Purpose: Detours the engine's localizable-FText constructor so the loader
//          (and plugins, via the trampoline address) can observe / produce
//          localized text.
//
// Hook point:
//   FText* __fastcall FText::AsLocalizable_Advanced(FText* result,
//       const FTextKey* Namespace, const FTextKey* Key, const wchar_t* String)
//
// Note: FTextKey is not reflected in the SDK, so the detour signature uses
// void* for the Namespace/Key parameters and simply passes them through.
// ---------------------------------------------------------------------------

namespace Hooks::TextLocalization
{
	// Install the hook (AOB scan + detour with error handling)
	bool Install();

	// Remove the hook
	void Remove();

	// Returns true if the hook is currently installed
	bool IsInstalled();

	// Returns the trampoline address of the original FText::AsLocalizable_Advanced, or 0 if not installed.
	// Cast to: FText*(__fastcall*)(FText* result, const FTextKey* Namespace, const FTextKey* Key, const wchar_t* String)
	uintptr_t GetOriginalPtr();
}
