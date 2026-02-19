#pragma once

#include <cstdint>  // for uint8_t
#include <cstddef>  // for size_t

namespace RailJunctionFixer
{
    // UStruct offsets for hierarchy chain patching
    namespace UStructOff
    {
        constexpr size_t InheritanceChain = 0x30;  // uintptr_t* — array of ancestor identities
        constexpr size_t HierarchyDepth = 0x38;    // int32_t — depth in hierarchy
        constexpr size_t SuperStruct = 0x40;       // uintptr_t — pointer to parent UStruct
    }

    // Template helper to read a value at an offset from a pointer
    template<typename T>
    inline T ReadAt(uintptr_t base, size_t offset)
    {
        return *reinterpret_cast<T*>(base + offset);
    }

    // Template helper to write a value at an offset from a pointer
    template<typename T>
    inline void WriteAt(uintptr_t base, size_t offset, T value)
    {
        *reinterpret_cast<T*>(base + offset) = value;
    }

    // Class to handle the parent class override for FCrLogisticsSocketsFragment
    class LogisticsFragmentFixer
    {
    public:
        // Initialize the fixer - patches the parent class
        static bool Initialize();

        // Cleanup (if needed)
        static void Shutdown();

    private:
        // Memory address where we need to make the patch
        static void* s_targetAddress;

        // Original bytes for restoration (if needed)
        static uint8_t s_originalBytes[32];
        static size_t s_patchSize;

        // Allocated chain memory (leaked intentionally - permanent for game session)
        static uintptr_t* s_newChain;

        // Patch the inheritance hierarchy chain for IsChildOf to work correctly
        static bool PatchHierarchyChain(uintptr_t socketsStruct, uintptr_t savableStruct);
    };
}