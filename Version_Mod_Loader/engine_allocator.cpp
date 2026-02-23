#include "engine_allocator.h"
#include "logger.h"
#include "scanner.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Engine memory allocator function pointer types
// ---------------------------------------------------------------------------
typedef void* (__cdecl* FMemoryMalloc_t)(size_t Count, uint32_t Alignment);
typedef void  (__cdecl* FMemoryFree_t)(void* Original);

static FMemoryMalloc_t g_engineMalloc = nullptr;
static FMemoryFree_t   g_engineFree   = nullptr;

// ---------------------------------------------------------------------------
// Pattern that lands directly on the E8 CALL to FMemory::Malloc.
//
// From IDA - UCrAbilitySystemGlobals::AllocAbilityActorInfo:
//   sub rsp, 28h
//   mov edx, 10h
//   lea ecx, [rdx+70h]
//   call FMemory::Malloc          <-- pattern starts here (E8 xx xx xx xx)
//   mov rbx, rax
//   test rax, rax
//   jz   ...
// ---------------------------------------------------------------------------
static const char* MALLOC_CALL_PATTERN =
    "E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 84 ?? ?? ?? ?? "
    "33 D2 41 B8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? "
    "0F 10 05 ?? ?? ?? ?? 33 C0 48 C7 43 ?? ?? ?? ?? ?? "
    "80 63 ?? ?? 48 89 43";

// ---------------------------------------------------------------------------
// Pattern for a known function that calls FMemory::Free.
// ParseSettings is used as a reference function to locate Free via GMalloc
// cross-referencing or a known offset.
// ---------------------------------------------------------------------------
static const char* PARSE_SETTINGS_PATTERN = "48 8B C4 55 41 54 48 8D 6C 24";

// Known offset from ParseSettings to the `call FMemory::Free` instruction.
static constexpr size_t PARSESETTINGS_FREE_CALL_OFFSET = 0x16F;

// ---------------------------------------------------------------------------
// Helper: resolve an E8 rel32 CALL instruction at a given address.
// ---------------------------------------------------------------------------
static uintptr_t ResolveE8Call(uintptr_t addr)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(addr);
    if (bytes[0] != 0xE8)
    return 0;

    int32_t rel32;
    memcpy(&rel32, bytes + 1, sizeof(int32_t));
  return addr + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel32));
}

// ---------------------------------------------------------------------------
// Helper: check if an address range is readable (committed memory).
// ---------------------------------------------------------------------------
static bool IsReadableMemory(uintptr_t addr, size_t size)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0)
      return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
        return false;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (addr + size > regionEnd)
        return false;
    return true;
}

// ---------------------------------------------------------------------------
// Helper: dump the first N bytes at an address as hex for diagnostics.
// ---------------------------------------------------------------------------
static void DumpBytes(const wchar_t* label, uintptr_t addr, size_t count)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(addr);
    char hexBuf[256] = {};
    size_t pos = 0;
    for (size_t i = 0; i < count && pos + 3 < sizeof(hexBuf); ++i)
        pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", bytes[i]);
    ModLoader::LogDebug(L"[EngineAllocator] %s at 0x%llX: %S", label,
        static_cast<unsigned long long>(addr), hexBuf);
}

// ---------------------------------------------------------------------------
// Helper: extract the absolute address of the GMalloc global pointer from
// a function body by scanning for RIP-relative MOV instructions.
// ---------------------------------------------------------------------------
static uintptr_t ExtractGMallocAddress(uintptr_t funcAddr, size_t scanLen = 64)
{
    if (!IsReadableMemory(funcAddr, scanLen))
        return 0;

    const auto* bytes = reinterpret_cast<const uint8_t*>(funcAddr);

    for (size_t i = 0; i + 7 <= scanLen; ++i)
    {
        // REX.W prefix: 48 or 4C
        if (bytes[i] != 0x48 && bytes[i] != 0x4C)
    continue;
   // Opcode: 8B = MOV r64, r/m64
        if (bytes[i + 1] != 0x8B)
            continue;
     // ModRM: mod=00 r/m=101 ? RIP-relative
        uint8_t modrm = bytes[i + 2];
     if ((modrm & 0xC7) != 0x05)
 continue;

    int32_t disp32;
        memcpy(&disp32, &bytes[i + 3], sizeof(int32_t));

        uintptr_t globalAddr = funcAddr + i + 7 + static_cast<uintptr_t>(static_cast<intptr_t>(disp32));

        ModLoader::LogDebug(L"[EngineAllocator] Found RIP-relative MOV at +0x%zX -> global at 0x%llX",
          i, static_cast<unsigned long long>(globalAddr));
        return globalAddr;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Smoke test: Malloc -> write -> Free cycle under SEH.
// ---------------------------------------------------------------------------
static bool SmokeTestAllocator(FMemoryMalloc_t mallocFn, FMemoryFree_t freeFn)
{
  ModLoader::LogDebug(L"[EngineAllocator] Smoke testing Malloc=0x%llX  Free=0x%llX ...",
   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mallocFn)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(freeFn)));

    __try
    {
void* ptr = mallocFn(64, 16);
        if (!ptr)
        {
       ModLoader::LogWarn(L"[EngineAllocator] Smoke test: Malloc returned null");
      return false;
        }

        memset(ptr, 0xAB, 64);
      freeFn(ptr);

        ModLoader::LogInfo(L"[EngineAllocator] Smoke test PASSED");
 return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      DWORD code = GetExceptionCode();
        ModLoader::LogError(L"[EngineAllocator] Smoke test FAILED - exception 0x%08lX", code);
     return false;
    }
}

// ---------------------------------------------------------------------------
// Find FMemory::Malloc via the call-site pattern.
// ---------------------------------------------------------------------------
static uintptr_t FindMallocViaPattern()
{
    uintptr_t callSite = Scanner::FindPatternInMainModule(MALLOC_CALL_PATTERN);
    if (callSite == 0)
    {
  ModLoader::LogWarn(L"[EngineAllocator] Malloc call-site pattern not found");
        return 0;
  }

    ModLoader::LogInfo(L"[EngineAllocator] Malloc call-site pattern matched at 0x%llX",
        static_cast<unsigned long long>(callSite));

    uintptr_t mallocAddr = ResolveE8Call(callSite);
    if (mallocAddr == 0)
    {
  ModLoader::LogWarn(L"[EngineAllocator] Failed to decode E8 CALL at pattern match");
        return 0;
 }

    ModLoader::LogInfo(L"[EngineAllocator] FMemory::Malloc = 0x%llX",
    static_cast<unsigned long long>(mallocAddr));
DumpBytes(L"FMemory::Malloc", mallocAddr, 64);
    return mallocAddr;
}

// ---------------------------------------------------------------------------
// Find FMemory::Free by scanning a reference function for E8 CALLs whose
// target references the same GMalloc global as Malloc.
// ---------------------------------------------------------------------------
static uintptr_t FindFreeViaGMalloc(uintptr_t refFuncAddr, uintptr_t gmallocAddr)
{
    if (refFuncAddr == 0 || gmallocAddr == 0)
        return 0;

    ModLoader::LogInfo(L"[EngineAllocator] Scanning ref function at 0x%llX for calls referencing GMalloc 0x%llX...",
        static_cast<unsigned long long>(refFuncAddr),
        static_cast<unsigned long long>(gmallocAddr));

    for (size_t offset = 0; offset < 0x400; ++offset)
    {
  uintptr_t instrAddr = refFuncAddr + offset;
        uintptr_t target = ResolveE8Call(instrAddr);
        if (target == 0)
            continue;

        if (!IsReadableMemory(target, 64))
       continue;

        uintptr_t candidateGMalloc = ExtractGMallocAddress(target, 64);
        if (candidateGMalloc == gmallocAddr)
        {
   ModLoader::LogInfo(L"[EngineAllocator] FMemory::Free = 0x%llX (from ref+0x%zX, same GMalloc)",
static_cast<unsigned long long>(target), offset);
     DumpBytes(L"FMemory::Free", target, 64);
        return target;
        }
    }

    ModLoader::LogWarn(L"[EngineAllocator] No call target references GMalloc 0x%llX",
        static_cast<unsigned long long>(gmallocAddr));
    return 0;
}

// ---------------------------------------------------------------------------
// Find FMemory::Free via a known offset from the reference function (fallback).
// ---------------------------------------------------------------------------
static uintptr_t FindFreeViaOffset(uintptr_t refFuncAddr, uintptr_t mallocAddr)
{
    if (refFuncAddr == 0)
        return 0;

    uintptr_t freeCallSite = refFuncAddr + PARSESETTINGS_FREE_CALL_OFFSET;

    if (!IsReadableMemory(freeCallSite, 5))
        return 0;

    const auto* callByte = reinterpret_cast<const uint8_t*>(freeCallSite);
  if (*callByte != 0xE8)
    {
   ModLoader::LogWarn(L"[EngineAllocator] Byte at offset 0x%zX is 0x%02X, not 0xE8",
    PARSESETTINGS_FREE_CALL_OFFSET, *callByte);
        return 0;
    }

    uintptr_t freeAddr = ResolveE8Call(freeCallSite);
    if (freeAddr == 0 || !IsReadableMemory(freeAddr, 64))
        return 0;

    ModLoader::LogInfo(L"[EngineAllocator] Candidate FMemory::Free = 0x%llX (via offset fallback)",
  static_cast<unsigned long long>(freeAddr));
 DumpBytes(L"FMemory::Free candidate", freeAddr, 64);

    // Validate with smoke test
    if (!SmokeTestAllocator(reinterpret_cast<FMemoryMalloc_t>(mallocAddr),
  reinterpret_cast<FMemoryFree_t>(freeAddr)))
    {
        ModLoader::LogWarn(L"[EngineAllocator] Offset fallback smoke test FAILED");
        return 0;
    }

    return freeAddr;
}

// ===========================================================================
// Public API
// ===========================================================================

bool EngineAllocator::Resolve()
{
    ModLoader::LogInfo(L"[EngineAllocator] Resolving FMemory::Malloc and FMemory::Free...");

    // Step 1: Find FMemory::Malloc
    uintptr_t mallocAddr = FindMallocViaPattern();
    if (mallocAddr == 0)
    {
        ModLoader::LogError(L"[EngineAllocator] Could not find FMemory::Malloc");
        return false;
    }

    // Step 2: Extract GMalloc address from Malloc's body
    uintptr_t gmallocAddr = ExtractGMallocAddress(mallocAddr, 64);
    if (gmallocAddr != 0)
    {
  ModLoader::LogInfo(L"[EngineAllocator] GMalloc global at 0x%llX",
        static_cast<unsigned long long>(gmallocAddr));
    }

    // Step 3: Find a reference function (ParseSettings) for cross-referencing Free
    uintptr_t refFuncAddr = Scanner::FindPatternInMainModule(PARSE_SETTINGS_PATTERN);
    if (refFuncAddr != 0)
    {
        ModLoader::LogInfo(L"[EngineAllocator] Reference function found at 0x%llX",
            static_cast<unsigned long long>(refFuncAddr));
    }

    // Step 4: Find FMemory::Free via GMalloc cross-reference
    uintptr_t freeAddr = 0;
    if (gmallocAddr != 0 && refFuncAddr != 0)
    {
    freeAddr = FindFreeViaGMalloc(refFuncAddr, gmallocAddr);
    }

    // Step 5: Fallback - use known offset
    if (freeAddr == 0 && refFuncAddr != 0)
    {
        ModLoader::LogWarn(L"[EngineAllocator] GMalloc cross-reference failed, trying offset fallback...");
        freeAddr = FindFreeViaOffset(refFuncAddr, mallocAddr);
    }

    if (freeAddr == 0)
  {
        ModLoader::LogError(L"[EngineAllocator] Could not find FMemory::Free");
        return false;
    }

    // Step 6: Final smoke test
    if (!SmokeTestAllocator(reinterpret_cast<FMemoryMalloc_t>(mallocAddr),
  reinterpret_cast<FMemoryFree_t>(freeAddr)))
    {
        ModLoader::LogError(L"[EngineAllocator] Final smoke test FAILED");
        return false;
    }

    // Commit
    g_engineMalloc = reinterpret_cast<FMemoryMalloc_t>(mallocAddr);
    g_engineFree   = reinterpret_cast<FMemoryFree_t>(freeAddr);

    ModLoader::LogInfo(L"[EngineAllocator] SUCCESS - FMemory::Malloc=0x%llX, FMemory::Free=0x%llX",
  static_cast<unsigned long long>(mallocAddr),
        static_cast<unsigned long long>(freeAddr));

    return true;
}

void EngineAllocator::Shutdown()
{
    g_engineMalloc = nullptr;
    g_engineFree   = nullptr;
    ModLoader::LogInfo(L"[EngineAllocator] Shut down");
}

bool EngineAllocator::IsAvailable()
{
 return g_engineMalloc != nullptr && g_engineFree != nullptr;
}

void* EngineAllocator::Alloc(size_t count, uint32_t alignment)
{
    if (!g_engineMalloc)
    return nullptr;
    return g_engineMalloc(count, alignment);
}

void EngineAllocator::Free(void* ptr)
{
    if (!g_engineFree || !ptr)
        return;
    g_engineFree(ptr);
}
