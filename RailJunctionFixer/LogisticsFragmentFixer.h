#pragma once

#include <cstdint>  // for uint8_t
#include <cstddef>  // for size_t

namespace RailJunctionFixer
{
    // UStruct offsets for hierarchy chain patching
    namespace UStructOff
    {
        constexpr size_t InheritanceChain = 0x30;  // uintptr_t* � array of ancestor identities
        constexpr size_t HierarchyDepth = 0x38;    // int32_t � depth in hierarchy
        constexpr size_t SuperStruct = 0x40;       // uintptr_t � pointer to parent UStruct
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

        // Restore original struct data and free allocated chain memory
        static void Shutdown();

    private:
        // Allocated chain memory
        static uintptr_t* s_newChain;

        // Address of the patched UScriptStruct (needed for restore)
        static uintptr_t s_socketsStruct;

        // Original values saved before patching (for restoration on shutdown)
        static uintptr_t* s_origChain;
        static int32_t    s_origDepth;
        static uintptr_t  s_origSuperStruct;

        // Patch the inheritance hierarchy chain for IsChildOf to work correctly
        static bool PatchHierarchyChain(uintptr_t socketsStruct, uintptr_t savableStruct);
    };
}