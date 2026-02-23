#pragma once

#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Engine Allocator - resolves FMemory::Malloc / FMemory::Free from the game
// binary so that plugins can safely allocate and free engine-owned memory
// (FStrings, UObject fields, etc.) without corrupting FMallocBinned2 canaries.
//
// Resolved once at engine init time by the mod loader.  Plugins access it
// through IPluginHooks::EngineAlloc / EngineFree / IsEngineAllocatorAvailable.
// ---------------------------------------------------------------------------

namespace EngineAllocator
{
    // Attempt to resolve FMemory::Malloc and FMemory::Free from the game binary.
    // Should be called once after the engine has initialised (pattern scanner ready).
    // Returns true if both functions were found and smoke-tested successfully.
  bool Resolve();

    // Release cached function pointers (call on shutdown).
    void Shutdown();

    // Returns true if Resolve() succeeded and the allocator is usable.
    bool IsAvailable();

    // Allocate memory via the engine's FMemory::Malloc.
    // Returns nullptr if the allocator is not available or allocation fails.
    void* Alloc(size_t count, uint32_t alignment);

    // Free memory via the engine's FMemory::Free.
    // Does nothing if the allocator is not available or ptr is nullptr.
    void Free(void* ptr);
}
