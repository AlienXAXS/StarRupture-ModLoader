#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>

// ---------------------------------------------------------------------------
// Common hooking infrastructure
// Shared by all hook modules
// ---------------------------------------------------------------------------

namespace Hooks
{
    // ---------------------------------------------------------------------------
    // Lightweight x64 instruction length decoder
    // Returns the length of the instruction at the given address
    // ---------------------------------------------------------------------------
    size_t GetInstructionLength(const uint8_t* code);

    // ---------------------------------------------------------------------------
    // Calculate minimum bytes to steal for a hook
    // Returns total bytes needed to cover at least minBytes worth of complete instructions
    // ---------------------------------------------------------------------------
    size_t CalculateStolenBytes(const uint8_t* code, size_t minBytes);

    struct Hook
    {
        uintptr_t target         = 0;   // Address we hooked
    uintptr_t detour         = 0;   // Our replacement function
        uint8_t*  trampoline     = nullptr; // Allocated trampoline (calls original)
        uint8_t   originalBytes[64]{};      // Saved original bytes (increased for safety)
        size_t    patchSize = 0; // How many bytes we overwrote (dynamically calculated)
  bool      installed      = false;

        // Install a hook at `target`. Writes a 14-byte absolute JMP (x64).
        // `originalFunc` receives a pointer to a trampoline that calls the
        // original code � cast it to the right function pointer type.
        bool Install(uintptr_t target, void* detour, void** originalFunc);

        // Remove the hook, restoring original bytes.
        void Remove();
    };

  // Simple memory patching utilities
    bool Patch(uintptr_t address, const uint8_t* data, size_t size);
    bool Nop(uintptr_t address, size_t size);

    // Read memory safely
    bool ReadMemory(uintptr_t address, void* buffer, size_t size);

    // ---------------------------------------------------------------------------
    // VTable Hook
    //
    // Hooks a virtual function by patching the vtable entry directly.
    // This avoids the need for unique byte patterns (which virtual functions
    // often don't have since they're short stubs).
    //
    // Usage:
    //   1. Get a UObject instance (e.g., via StaticClass()->GetDefaultObject())
    //   2. Read the vtable pointer from offset 0x00 of the object
    //   3. Specify the vtable slot index for the virtual function
    //   4. The hook replaces the function pointer in the vtable
    //
    // To find vtable slot indices:
    //   - In IDA, find the vtable for the class
    //   - Count the function pointer entries (each is 8 bytes on x64)
    //   - The slot index is the position (0-based) in the vtable
    // ---------------------------------------------------------------------------
    struct VTableHook
    {
        uintptr_t  vtableAddr      = 0;     // Address of the vtable
        size_t     slotIndex       = 0;     // Which vtable slot we hooked
        uintptr_t  originalFunc    = 0;     // Original function pointer (saved)
        bool       installed       = false;

        // Install by patching a vtable slot.
        // `objectInstance` - pointer to any instance of the class (for reading vtable ptr)
        // `vtableSlotIndex` - 0-based index into the vtable
        // `detour` - your replacement function
        // `outOriginal` - receives the original function pointer
        bool Install(void* objectInstance, size_t vtableSlotIndex, void* detour, void** outOriginal);

        // Install by directly specifying the vtable address (if you already know it)
        bool InstallByVTableAddr(uintptr_t vtableAddress, size_t vtableSlotIndex, void* detour, void** outOriginal);

        // Remove the hook, restoring the original vtable entry
        void Remove();
    };
}
