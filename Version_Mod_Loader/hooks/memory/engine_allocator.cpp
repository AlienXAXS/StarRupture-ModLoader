#include "engine_allocator.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

// ---------------------------------------------------------------------------
// Engine memory allocator function pointer types
// ---------------------------------------------------------------------------
using FMemoryMalloc_t = void* (__cdecl*)(size_t Count, uint32_t Alignment);
using FMemoryFree_t = void(__cdecl*)(void* Original);

static FMemoryMalloc_t g_engineMalloc = nullptr;
static FMemoryFree_t g_engineFree = nullptr;

// ---------------------------------------------------------------------------
// Patterns used to locate allocator functions
// ---------------------------------------------------------------------------
// Reference pattern located in a function that references the allocator.
// FMemory::Malloc can be found by resolving the E8 call located17 bytes
// after this reference pattern.
static const char* FMEMORY_REF_PATTERN = "40 53 48 83 EC ?? BA ?? ?? ?? ?? 48 8B D9 8D 4A";
static constexpr size_t FMEMORY_REF_TO_MALLOC_OFFSET = 17;

// Exact pattern that identifies FMemory::Free (function entry or nearby).
static const char* FMEMORY_FREE_PATTERN = "48 85 C9 74 ?? 53 48 83 EC ?? 48 8B D9 48 8B 0D";

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
// Helper: resolve an E8 rel32 CALL instruction at a given address.
// Caller must ensure the address is readable.
// ---------------------------------------------------------------------------
static uintptr_t ResolveE8Call(uintptr_t addr)
{
	if (!IsReadableMemory(addr, 5))
		return 0;

	const auto* bytes = reinterpret_cast<const uint8_t*>(addr);
	if (bytes[0] != 0xE8)
		return 0;

	int32_t rel32;
	memcpy(&rel32, bytes + 1, sizeof(int32_t));
	return addr + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel32));
}

// ---------------------------------------------------------------------------
// Helper: dump the first N bytes at an address as hex for diagnostics.
// ---------------------------------------------------------------------------
static void DumpBytes(const wchar_t* label, uintptr_t addr, size_t count)
{
	if (!IsReadableMemory(addr, count))
	{
		ModLoaderLogger::LogWarn(L"[EngineAllocator] %s at0x%llX not readable", label,
			static_cast<unsigned long long>(addr));
		return;
	}

	const auto* bytes = reinterpret_cast<const uint8_t*>(addr);
	char hexBuf[256] = {};
	size_t pos = 0;
	for (size_t i = 0; i < count && pos + 3 < sizeof(hexBuf); ++i)
		pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", bytes[i]);

	ModLoaderLogger::LogDebug(L"[EngineAllocator] %s at0x%llX: %S", label,
		static_cast<unsigned long long>(addr), hexBuf);
}

// ---------------------------------------------------------------------------
// Helper: extract the absolute address of the GMalloc global pointer from
// a function body by scanning for RIP-relative MOV instructions.
// ---------------------------------------------------------------------------
static uintptr_t ExtractGMallocAddress(uintptr_t funcAddr, size_t scanLen = 264)
{
	if (!IsReadableMemory(funcAddr, scanLen))
		return 0;

	const auto* bytes = reinterpret_cast<const uint8_t*>(funcAddr);

	for (size_t i = 0; i + 7 <= scanLen; ++i)
	{
		// REX.W prefix:48 or4C
		if (bytes[i] != 0x48 && bytes[i] != 0x4C)
			continue;
		// Opcode:8B = MOV r64, r/m64
		if (bytes[i + 1] != 0x8B)
			continue;
		// ModRM: mod=00 r/m=101 -> RIP-relative
		uint8_t modrm = bytes[i + 2];
		if ((modrm & 0xC7) != 0x05)
			continue;

		int32_t disp32;
		memcpy(&disp32, &bytes[i + 3], sizeof(int32_t));

		uintptr_t globalAddr = funcAddr + i + 7 + static_cast<uintptr_t>(static_cast<intptr_t>(disp32));

		ModLoaderLogger::LogDebug(L"[EngineAllocator] Found RIP-relative MOV at +0x%zX -> global at0x%llX",
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
	ModLoaderLogger::LogDebug(L"[EngineAllocator] Smoke testing Malloc=0x%llX Free=0x%llX ...",
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mallocFn)),
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(freeFn)));

	__try
	{
		void* ptr = mallocFn(64, 16);
		if (!ptr)
		{
			ModLoaderLogger::LogWarn(L"[EngineAllocator] Smoke test: Malloc returned null");
			return false;
		}

		memset(ptr, 0xAB, 64);
		freeFn(ptr);

		ModLoaderLogger::LogInfo(L"[EngineAllocator] Smoke test PASSED");
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		DWORD code = GetExceptionCode();
		ModLoaderLogger::LogError(L"[EngineAllocator] Smoke test FAILED - exception0x%08lX", code);
		return false;
	}
}

// ---------------------------------------------------------------------------
// Find FMemory::Malloc via a reference pattern and an E8 call at a fixed offset
// ---------------------------------------------------------------------------
static uintptr_t FindMallocViaRefPattern()
{
	uintptr_t refAddr = Scanner::FindPatternInMainModule("AllocRef", FMEMORY_REF_PATTERN);
	if (refAddr == 0)
	{
		ModLoaderLogger::LogWarn(L"[EngineAllocator] Reference pattern for allocator not found");
		return 0;
	}

	ModLoaderLogger::LogDebug(L"[EngineAllocator] Found allocator reference at0x%llX",
		static_cast<unsigned long long>(refAddr));

	uintptr_t callSite = refAddr + FMEMORY_REF_TO_MALLOC_OFFSET;
	if (!IsReadableMemory(callSite, 5))
	{
		ModLoaderLogger::LogWarn(L"[EngineAllocator] Call site at ref+%zu not readable", FMEMORY_REF_TO_MALLOC_OFFSET);
		return 0;
	}

	const auto* b = reinterpret_cast<const uint8_t*>(callSite);
	if (b[0] != 0xE8)
	{
		ModLoaderLogger::LogWarn(L"[EngineAllocator] Expected E8 call at ref+%zu but found0x%02X",
			FMEMORY_REF_TO_MALLOC_OFFSET, b[0]);
		return 0;
	}

	uintptr_t mallocAddr = ResolveE8Call(callSite);
	if (mallocAddr == 0 || !IsReadableMemory(mallocAddr, 16))
	{
		ModLoaderLogger::LogWarn(L"[EngineAllocator] Failed to resolve malloc target from call at0x%llX",
			static_cast<unsigned long long>(callSite));
		return 0;
	}

	ModLoaderLogger::LogInfo(L"[EngineAllocator] Resolved FMemory::Malloc =0x%llX (from ref+%zu)",
		static_cast<unsigned long long>(mallocAddr), FMEMORY_REF_TO_MALLOC_OFFSET);
	DumpBytes(L"FMemory::Malloc", mallocAddr, 64);
	return mallocAddr;
}

// ---------------------------------------------------------------------------
// Find FMemory::Free via an exact pattern match
// ---------------------------------------------------------------------------
static uintptr_t FindFreeViaExactPattern()
{
	uintptr_t addr = Scanner::FindPatternInMainModule("FMemory::Free", FMEMORY_FREE_PATTERN);
	if (addr == 0)
	{
		ModLoaderLogger::LogWarn(L"[EngineAllocator] Exact Free pattern not found");
		return 0;
	}

	ModLoaderLogger::LogInfo(L"[EngineAllocator] Found FMemory::Free at0x%llX (exact pattern)",
		static_cast<unsigned long long>(addr));
	DumpBytes(L"FMemory::Free", addr, 64);
	return addr;
}

// ---------------------------------------------------------------------------
// Fallback: try to find Free by scanning referenced calls for the same GMalloc
// ---------------------------------------------------------------------------
static uintptr_t FindFreeViaGMalloc(uintptr_t refFuncAddr, uintptr_t gmallocAddr)
{
	if (refFuncAddr == 0 || gmallocAddr == 0)
		return 0;

	ModLoaderLogger::LogInfo(L"[EngineAllocator] Scanning ref function at0x%llX for calls referencing GMalloc0x%llX...",
		static_cast<unsigned long long>(refFuncAddr), static_cast<unsigned long long>(gmallocAddr));

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
			ModLoaderLogger::LogInfo(L"[EngineAllocator] FMemory::Free =0x%llX (from ref+0x%zX, same GMalloc)",
				static_cast<unsigned long long>(target), offset);
			DumpBytes(L"FMemory::Free", target, 64);
			return target;
		}
	}

	ModLoaderLogger::LogWarn(L"[EngineAllocator] No call target references GMalloc0x%llX",
		static_cast<unsigned long long>(gmallocAddr));
	return 0;
}

// ===========================================================================
// Public API
// ===========================================================================

bool EngineAllocator::Resolve()
{
	ModLoaderLogger::LogInfo(L"[EngineAllocator] Resolving FMemory::Malloc and FMemory::Free...");

	// Step1: Find FMemory::Malloc via reference pattern + E8 call
	uintptr_t mallocAddr = FindMallocViaRefPattern();
	if (mallocAddr == 0)
	{
		ModLoaderLogger::LogError(L"[EngineAllocator] Could not find FMemory::Malloc (via reference pattern)");
		return false;
	}

	// Step2: Try to extract GMalloc address for cross-referencing
	uintptr_t gmallocAddr = ExtractGMallocAddress(mallocAddr, 64);
	if (gmallocAddr != 0)
	{
		ModLoaderLogger::LogInfo(L"[EngineAllocator] GMalloc global at0x%llX",
			static_cast<unsigned long long>(gmallocAddr));
	}

	// Step3: Find FMemory::Free via exact pattern first
	uintptr_t freeAddr = FindFreeViaExactPattern();

	// Step4: If exact pattern failed, try GMalloc cross-reference fallback
	if (freeAddr == 0 && gmallocAddr != 0)
	{
		// Use the same reference function used to find malloc as a hint
		uintptr_t refFuncAddr = Scanner::FindPatternInMainModule("AllocRef", FMEMORY_REF_PATTERN);
		if (refFuncAddr != 0)
			freeAddr = FindFreeViaGMalloc(refFuncAddr, gmallocAddr);
	}

	if (freeAddr == 0)
	{
		ModLoaderLogger::LogError(L"[EngineAllocator] Could not find FMemory::Free");
		return false;
	}

	// Final smoke test
	if (!SmokeTestAllocator(reinterpret_cast<FMemoryMalloc_t>(mallocAddr),
		reinterpret_cast<FMemoryFree_t>(freeAddr)))
	{
		ModLoaderLogger::LogError(L"[EngineAllocator] Final smoke test FAILED");
		return false;
	}

	// Commit
	g_engineMalloc = reinterpret_cast<FMemoryMalloc_t>(mallocAddr);
	g_engineFree = reinterpret_cast<FMemoryFree_t>(freeAddr);

	ModLoaderLogger::LogInfo(L"[EngineAllocator] SUCCESS - FMemory::Malloc=0x%llX, FMemory::Free=0x%llX",
		static_cast<unsigned long long>(mallocAddr), static_cast<unsigned long long>(freeAddr));

	return true;
}

void EngineAllocator::Shutdown()
{
	g_engineMalloc = nullptr;
	g_engineFree = nullptr;
	ModLoaderLogger::LogInfo(L"[EngineAllocator] Shut down");
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
