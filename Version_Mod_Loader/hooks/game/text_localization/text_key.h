#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// FTextKey::FTextKey(const wchar_t*) Hook
//
// Purpose: Detours the engine's FTextKey string-interning constructor so the
//          loader (and plugins, via the trampoline address) can build the
//          FTextKey objects required by FText::AsLocalizable_Advanced.
//          FTextKey is just an index into the engine's interned-string table --
//          it cannot be constructed without calling this constructor.
//
// Hook point:
//   void __fastcall FTextKey::FTextKey(FTextKey* this, const wchar_t* InStr)
//
// Note: FTextKey is not reflected in the SDK, so the detour signature uses
// void* for the "this" parameter and simply passes it through.
// ---------------------------------------------------------------------------

namespace Hooks::TextKey
{
	// Install the hook (AOB scan + detour with error handling)
	bool Install();

	// Remove the hook
	void Remove();

	// Returns true if the hook is currently installed
	bool IsInstalled();

	// Returns the trampoline address of the original FTextKey::FTextKey, or 0 if not installed.
	// Cast to: void(__fastcall*)(FTextKey* this, const wchar_t* InStr)
	uintptr_t GetOriginalPtr();
}
