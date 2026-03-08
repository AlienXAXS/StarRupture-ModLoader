#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// MemUtils — safe memory inspection and relative branch decoding
// ---------------------------------------------------------------------------

namespace MemUtils
{
    // Returns true if the memory range [addr, addr+size) is committed and readable.
    inline bool IsReadableMemory(uintptr_t addr, size_t size)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0)
            return false;
        if (mbi.State != MEM_COMMIT)
            return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
            return false;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        return (addr + size <= regionEnd);
    }

    // Resolve an E9 relative JMP at `instrAddr`.
    // Returns the absolute target address, or 0 on failure.
    inline uintptr_t ResolveRelJmp(uintptr_t instrAddr)
    {
        if (!IsReadableMemory(instrAddr, 5))
            return 0;
        if (*reinterpret_cast<const uint8_t*>(instrAddr) != 0xE9)
            return 0;

        int32_t rel32;
        std::memcpy(&rel32, reinterpret_cast<const void*>(instrAddr + 1), sizeof(int32_t));
        return instrAddr + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel32));
    }

    // Resolve an E8 relative CALL at `instrAddr`.
    // Returns the absolute target address, or 0 on failure.
    inline uintptr_t ResolveRelCall(uintptr_t instrAddr)
    {
        if (!IsReadableMemory(instrAddr, 5))
            return 0;
        if (*reinterpret_cast<const uint8_t*>(instrAddr) != 0xE8)
            return 0;

        int32_t rel32;
        std::memcpy(&rel32, reinterpret_cast<const void*>(instrAddr + 1), sizeof(int32_t));
        return instrAddr + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel32));
    }
}
