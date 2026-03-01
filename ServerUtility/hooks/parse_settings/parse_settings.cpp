#include "parse_settings.h"
#include "plugin_helpers.h"

#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// UCrDedicatedServerSettingsComp memory layout
//
// ParseSettings writes five fields on the component object.  The offsets
// below are derived from the order the fields are accessed in the
// decompiled function:
//
//   SessionName    FString   - first GetStringField call
//   SaveGameName     FString   - second GetStringField call
//   SaveGameInterval int     - _wtoi() result
//   bStartNewGame    bool      - Stricmp("true") == 0
//bLoadSavedGame   bool      - Stricmp("true") == 0
// ---------------------------------------------------------------------------

// Minimal FString mirror so we can manipulate engine strings from outside.
// Layout matches UE4/UE5 TArray<TCHAR> used as FString storage.
struct EngineString
{
	wchar_t* Data;   // AllocatorInstance.Data
	int32_t  Num;    // ArrayNum  (includes null terminator)
	int32_t  Max;    // ArrayMax
};

// ---------------------------------------------------------------------------
// Engine memory allocator function pointers
//
// Resolved at hook install time via pattern scanning.  These point into the
// game binary's FMemory::Malloc / FMemory::Free which go through
// FMallocBinned2 and properly set the canary values.
//
// Using these ensures that when the GC later calls FMemory::Free on an
// FString::Data pointer we set, it sees a valid FMallocBinned2 block.
// ---------------------------------------------------------------------------
typedef void* (__cdecl* FMemoryMalloc_t)(size_t Count, uint32_t Alignment);
typedef void(__cdecl* FMemoryFree_t)(void* Original);

static FMemoryMalloc_t g_engineMalloc = nullptr;
static FMemoryFree_t   g_engineFree = nullptr;

// ---------------------------------------------------------------------------
// Field accessor namespace for UCrDedicatedServerSettingsComp
// Based on IDA decompilation, provides safe access to member fields
// ---------------------------------------------------------------------------
namespace FieldAccessor
{
	constexpr size_t OFFSET_SESSION_NAME = 0xB8;
	constexpr size_t OFFSET_SAVEGAME_NAME = 0xC8;
	constexpr size_t OFFSET_SAVE_INTERVAL = 0xD8;
	constexpr size_t OFFSET_START_NEW_GAME = 0xE0;
	constexpr size_t OFFSET_LOAD_SAVED_GAME = 0xE1;

	inline EngineString* GetSessionName(void* thisPtr)
	{
		return reinterpret_cast<EngineString*>(reinterpret_cast<uint8_t*>(thisPtr) + OFFSET_SESSION_NAME);
	}

	inline EngineString* GetSaveGameName(void* thisPtr)
	{
		return reinterpret_cast<EngineString*>(reinterpret_cast<uint8_t*>(thisPtr) + OFFSET_SAVEGAME_NAME);
	}

	inline int32_t* GetSaveGameInterval(void* thisPtr)
	{
		return reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(thisPtr) + OFFSET_SAVE_INTERVAL);
	}

	inline bool* GetStartNewGame(void* thisPtr)
	{
		return reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(thisPtr) + OFFSET_START_NEW_GAME);
	}

	inline bool* GetLoadSavedGame(void* thisPtr)
	{
		return reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(thisPtr) + OFFSET_LOAD_SAVED_GAME);
	}
}

// ---------------------------------------------------------------------------
// Command-line parameter names
// ---------------------------------------------------------------------------
static constexpr const wchar_t* PARAM_SESSION_NAME = L"-SessionName=";
static constexpr const wchar_t* PARAM_SAVE_INTERVAL = L"-SaveGameInterval=";

// Save game name is always AutoSave0
static constexpr const wchar_t* SAVE_GAME_NAME = L"AutoSave0";

// Default save interval in seconds (5 minutes)
static constexpr int DEFAULT_SAVE_INTERVAL = 300;

// ---------------------------------------------------------------------------
// Helper: assign an FString using the engine's own allocator.
//
// This mirrors exactly what the original ParseSettings does:
//1. Free the old Data pointer via FMemory::Free (if non-null)
//   2. Allocate a new buffer via FMemory::Malloc
//   3. Copy the string into the new buffer
//   4. Update Num and Max
//
// Because both Malloc and Free go through FMallocBinned2, the canary
// values are correct and the GC destructor will not crash.
// ---------------------------------------------------------------------------
static bool AssignEngineString(EngineString* str, const wchar_t* value)
{
	if (!str)
		return false;

	if (!g_engineMalloc || !g_engineFree)
	{
		LOG_ERROR("[AssignEngineString] Engine allocator not resolved!");
		return false;
	}

	// Free old allocation if present AND looks valid.
	// When the original ParseSettings fails (returns 0), the FString fields
	// may contain uninitialized garbage.  We only call FMemory::Free if
	// Data is non-null AND Num/Max look sane (positive, consistent).
	// Otherwise we just zero the struct and allocate fresh.
	if (str->Data)
	{
		bool looksValid = (str->Num > 0 && str->Max > 0 && str->Num <= str->Max && str->Max < 0x100000);

		if (looksValid)
		{
			LOG_DEBUG("[AssignEngineString] Freeing old Data at %p (Num=%d, Max=%d)",
				static_cast<void*>(str->Data), str->Num, str->Max);
			g_engineFree(str->Data);
		}
		else
		{
			LOG_WARN("[AssignEngineString] Skipping free of suspicious Data=%p (Num=%d, Max=%d) - likely uninitialized",
				static_cast<void*>(str->Data), str->Num, str->Max);
		}

		str->Data = nullptr;
		str->Num = 0;
		str->Max = 0;
	}

	if (!value || value[0] == L'\0')
	{
		// Leave as empty/null - already cleared above
		return true;
	}

	const int32_t len = static_cast<int32_t>(wcslen(value));
	const int32_t numElements = len + 1; // include null terminator
	const size_t byteSize = static_cast<size_t>(numElements) * sizeof(wchar_t);

	// Allocate via the engine's FMemory::Malloc with default alignment
	// UE5 FString uses alignment of __STDCPP_DEFAULT_NEW_ALIGNMENT__ which is
	// typically 16 on x64 MSVC.  FMallocBinned2 expects the same alignment
	// that was used at allocation time, so we use 16 to match.
	void* newData = g_engineMalloc(byteSize, 16);
	if (!newData)
	{
		LOG_ERROR("[AssignEngineString] FMemory::Malloc(%zu, 16) returned null!", byteSize);
		return false;
	}

	wmemcpy(static_cast<wchar_t*>(newData), value, static_cast<size_t>(numElements));

	str->Data = static_cast<wchar_t*>(newData);
	str->Num = numElements;
	str->Max = numElements;

	LOG_DEBUG("[AssignEngineString] Allocated new Data at %p (Num=%d, Max=%d)",
		newData, str->Num, str->Max);

	return true;
}

// ---------------------------------------------------------------------------
// Helper: parse a single command-line parameter value.
// Returns true and fills 'out' when the parameter is found.
// Quoted values (e.g. -SessionName="My Server") are supported.
// ---------------------------------------------------------------------------
static bool GetCommandLineParam(const wchar_t* paramName, std::wstring& out)
{
	const wchar_t* cmdLine = GetCommandLineW();
	if (!cmdLine)
		return false;

	const wchar_t* pos = wcsstr(cmdLine, paramName);
	if (!pos)
		return false;

	pos += wcslen(paramName); // advance past "ParamName=="

	bool quoted = (*pos == L'"');
	if (quoted)
		++pos;

	const wchar_t* end = pos;
	if (quoted)
	{
		while (*end && *end != L'"')
			++end;
	}
	else
	{
		while (*end && *end != L' ' && *end != L'\t')
			++end;
	}

	out.assign(pos, end);
	return !out.empty();
}

// ---------------------------------------------------------------------------
// Check whether the required parameters are present on the command line.
// ---------------------------------------------------------------------------
static bool RequiredParamsPresent()
{
	std::wstring tmp;
	return GetCommandLineParam(PARAM_SESSION_NAME, tmp);
}

// ---------------------------------------------------------------------------
// Determine whether a prior session save exists on disk.
// ---------------------------------------------------------------------------
static bool SaveGameExists(const std::wstring& sessionName)
{
	LOG_DEBUG("[SaveGameExists] Checking for existing save file for session: %ls", sessionName.c_str());

	wchar_t exePath[MAX_PATH] = {};
	if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0)
	{
		LOG_ERROR("[SaveGameExists] GetModuleFileNameW failed with error code: %lu", GetLastError());
		return false;
	}

	LOG_DEBUG("[SaveGameExists] Executable path: %ls", exePath);

	wchar_t* lastSlash = wcsrchr(exePath, L'\\');
	if (!lastSlash)
	{
		LOG_ERROR("[SaveGameExists] No backslash found in exe path: %ls", exePath);
		return false;
	}
	*lastSlash = L'\0';

	LOG_DEBUG("[SaveGameExists] Binary directory: %ls", exePath);

	std::wstring basePath = exePath;
	for (int i = 0; i < 2; ++i)
	{
		const size_t sep = basePath.rfind(L'\\');
		if (sep == std::wstring::npos)
		{
			LOG_ERROR("[SaveGameExists] Could not navigate back %d directories from: %ls", i + 1, basePath.c_str());
			return false;
		}
		basePath.erase(sep);
		LOG_DEBUG("[SaveGameExists] After navigating up %d level(s): %ls", i + 1, basePath.c_str());
	}

	LOG_INFO("[SaveGameExists] Root save path: %ls", basePath.c_str());

	std::wstring savePath = basePath;
	savePath += L"\\Saved\\SaveGames\\";
	savePath += sessionName;
	savePath += L"\\AutoSave0.sav";

	LOG_INFO("[SaveGameExists] Full save file path: %ls", savePath.c_str());

	const DWORD attr = GetFileAttributesW(savePath.c_str());

	if (attr == INVALID_FILE_ATTRIBUTES)
	{
		const DWORD error = GetLastError();
		LOG_DEBUG("[SaveGameExists] File does not exist or is inaccessible (error %lu)", error);
		return false;
	}

	if (attr & FILE_ATTRIBUTE_DIRECTORY)
	{
		LOG_WARN("[SaveGameExists] Path exists but is a directory, not a file");
		return false;
	}

	LOG_INFO("[SaveGameExists] Save file found! Will load existing session.");
	return true;
}

// ---------------------------------------------------------------------------
// Hook state
// ---------------------------------------------------------------------------
typedef __int64(__fastcall* ParseSettings_t)(void* thisPtr);
static ParseSettings_t  g_originalParseSettings = nullptr;
static HookHandle       g_hookHandle = nullptr;

// ---------------------------------------------------------------------------
// Detour
//
// Strategy: ALWAYS call the original function first.  This lets the engine
// do its normal DSSettings.txt load and allocate FString buffers via FMalloc.
// We then REPLACE the string contents using the engine's own allocator -
// freeing old data with FMemory::Free and allocating new data with
// FMemory::Malloc - so the GC's destructor always finds valid canary values.
// ---------------------------------------------------------------------------
static __int64 __fastcall Hook_ParseSettings(void* thisPtr)
{
	static std::atomic<int> callCount = 0;
	int currentCall = ++callCount;

	LOG_INFO("[Hook_ParseSettings] ===== CALL #%d START =====", currentCall);
	LOG_DEBUG("[Hook_ParseSettings] Called with thisPtr=0x%p", thisPtr);

	// Validate thisPtr
	if (!thisPtr)
	{
		LOG_ERROR("[Hook_ParseSettings] thisPtr is NULL - delegating to original");
		__int64 result = g_originalParseSettings(thisPtr);
		LOG_INFO("[Hook_ParseSettings] ===== CALL #%d END (NULL ptr, delegated) =====", currentCall);
		return result;
	}

	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(thisPtr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
	{
		LOG_ERROR("[Hook_ParseSettings] thisPtr (0x%p) points to invalid memory - delegating to original", thisPtr);
		__int64 result = g_originalParseSettings(thisPtr);
		LOG_INFO("[Hook_ParseSettings] ===== CALL #%d END (invalid memory, delegated) =====", currentCall);
		return result;
	}

	if (!RequiredParamsPresent())
	{
		LOG_DEBUG("[Hook_ParseSettings] Required command-line parameters not present - delegating to DSSettings.txt");
		__int64 result = g_originalParseSettings(thisPtr);
		LOG_INFO("[Hook_ParseSettings] ===== CALL #%d END (no params, delegated) =====", currentCall);
		return result;
	}

	if (!g_engineMalloc || !g_engineFree)
	{
		LOG_ERROR("[Hook_ParseSettings] Engine allocator not resolved - cannot safely set FStrings, delegating to original");
		__int64 result = g_originalParseSettings(thisPtr);
		LOG_INFO("[Hook_ParseSettings] ===== CALL #%d END (no allocator, delegated) =====", currentCall);
		return result;
	}

	LOG_INFO("[Hook_ParseSettings] Command-line parameters detected - will override after original runs");

	// -----------------------------------------------------------------------
	// Step 1: Let the ORIGINAL function run so the engine does its normal
	//     initialization (loads DSSettings.txt, allocates FStrings, etc.)
	// -----------------------------------------------------------------------
	LOG_DEBUG("[Hook_ParseSettings] Calling original ParseSettings to let engine initialize...");
	__int64 originalResult = g_originalParseSettings(thisPtr);
	LOG_DEBUG("[Hook_ParseSettings] Original ParseSettings returned %lld", originalResult);

	// If the original failed (returned 0), the FString fields may contain
	// uninitialized garbage.  Zero-init them so AssignEngineString doesn't
	// try to free garbage pointers.
	if (originalResult == 0)
	{
		LOG_WARN("[Hook_ParseSettings] Original ParseSettings FAILED (returned 0) - "
			"zero-initializing FString fields before override");

		EngineString* sessionStr = FieldAccessor::GetSessionName(thisPtr);
		EngineString* saveStr = FieldAccessor::GetSaveGameName(thisPtr);

		// Log current (potentially garbage) values for diagnostics
		LOG_DEBUG("[Hook_ParseSettings] Pre-init SessionName: Data=%p Num=%d Max=%d",
			static_cast<void*>(sessionStr->Data), sessionStr->Num, sessionStr->Max);
		LOG_DEBUG("[Hook_ParseSettings] Pre-init SaveGameName: Data=%p Num=%d Max=%d",
			static_cast<void*>(saveStr->Data), saveStr->Num, saveStr->Max);

		// Zero-init the FString structs so they look like empty strings
		memset(sessionStr, 0, sizeof(EngineString));
		memset(saveStr, 0, sizeof(EngineString));

		// Also zero the other fields
		*FieldAccessor::GetSaveGameInterval(thisPtr) = 0;
		*FieldAccessor::GetStartNewGame(thisPtr) = false;
		*FieldAccessor::GetLoadSavedGame(thisPtr) = false;
	}

	// -----------------------------------------------------------------------
	// Step 2: Parse our command-line overrides.
	// -----------------------------------------------------------------------
	std::wstring sessionName, saveGameInterval;

	GetCommandLineParam(PARAM_SESSION_NAME, sessionName);

	int saveInterval = DEFAULT_SAVE_INTERVAL;
	if (GetCommandLineParam(PARAM_SAVE_INTERVAL, saveGameInterval))
	{
		saveInterval = _wtoi(saveGameInterval.c_str());
		LOG_INFO("  SessionName      = %ls", sessionName.c_str());
		LOG_INFO("  SaveGameName     = %ls (fixed)", SAVE_GAME_NAME);
		LOG_INFO("  SaveGameInterval = %d seconds (from command line)", saveInterval);
	}
	else
	{
		LOG_INFO("  SessionName      = %ls", sessionName.c_str());
		LOG_INFO("  SaveGameName     = %ls (fixed)", SAVE_GAME_NAME);
		LOG_INFO("  SaveGameInterval = %d seconds (default)", saveInterval);
	}

	const bool hasSave = SaveGameExists(sessionName);
	const bool bStartNew = !hasSave;
	const bool bLoadSaved = hasSave;

	LOG_INFO("  AutoSave found   = %s  ->  %s",
		hasSave ? "yes" : "no",
		hasSave ? "loading existing session" : "starting new session");

	// -----------------------------------------------------------------------
	// Step 3: Replace FString contents using engine allocator.
	//   Free old Data via FMemory::Free, allocate new via FMemory::Malloc.
	//   This guarantees correct FMallocBinned2 canary values.
	// -----------------------------------------------------------------------
	LOG_DEBUG("[Hook_ParseSettings] Replacing fields via engine allocator (thisPtr at 0x%p)...", thisPtr);

	try
	{
		if (!AssignEngineString(FieldAccessor::GetSessionName(thisPtr), sessionName.c_str()))
		{
			LOG_ERROR("[Hook_ParseSettings] Failed to assign SessionName");
		}
		else
		{
			LOG_DEBUG("[Hook_ParseSettings] SessionName assigned successfully");
		}

		if (!AssignEngineString(FieldAccessor::GetSaveGameName(thisPtr), SAVE_GAME_NAME))
		{
			LOG_ERROR("[Hook_ParseSettings] Failed to assign SaveGameName");
		}
		else
		{
			LOG_DEBUG("[Hook_ParseSettings] SaveGameName assigned successfully");
		}

		*FieldAccessor::GetSaveGameInterval(thisPtr) = saveInterval;
		LOG_DEBUG("[Hook_ParseSettings] SaveGameInterval set successfully");

		*FieldAccessor::GetStartNewGame(thisPtr) = bStartNew;
		*FieldAccessor::GetLoadSavedGame(thisPtr) = bLoadSaved;
		LOG_DEBUG("[Hook_ParseSettings] Boolean flags set successfully");
	}
	catch (const std::exception& ex)
	{
		LOG_ERROR("[Hook_ParseSettings] C++ exception while setting fields: %s", ex.what());
		LOG_INFO("[Hook_ParseSettings] ===== CALL #%d END (exception, original result used) =====", currentCall);
		return originalResult;
	}
	catch (...)
	{
		LOG_ERROR("[Hook_ParseSettings] Unknown exception while setting fields");
		LOG_INFO("[Hook_ParseSettings] ===== CALL #%d END (exception, original result used) =====", currentCall);
		return originalResult;
	}

	// -----------------------------------------------------------------------
	// Step 4: Read back and verify the assigned values for diagnostics
	// -----------------------------------------------------------------------
	{
		EngineString* sessionStr = FieldAccessor::GetSessionName(thisPtr);
		EngineString* saveStr = FieldAccessor::GetSaveGameName(thisPtr);

		if (sessionStr->Data && sessionStr->Num > 0)
		{
			LOG_INFO("[Hook_ParseSettings] Readback SessionName: \"%ls\" (Num=%d, Max=%d)",
				sessionStr->Data, sessionStr->Num, sessionStr->Max);
		}
		else
		{
			LOG_ERROR("[Hook_ParseSettings] Readback SessionName: EMPTY/NULL!");
		}

		if (saveStr->Data && saveStr->Num > 0)
		{
			LOG_INFO("[Hook_ParseSettings] Readback SaveGameName: \"%ls\" (Num=%d, Max=%d)",
				saveStr->Data, saveStr->Num, saveStr->Max);
		}
		else
		{
			LOG_ERROR("[Hook_ParseSettings] Readback SaveGameName: EMPTY/NULL!");
		}
	}

	LOG_INFO("[Hook_ParseSettings] Settings applied (SaveGameInterval=%d, bStartNewGame=%s, bLoadSavedGame=%s)",
		*FieldAccessor::GetSaveGameInterval(thisPtr),
		*FieldAccessor::GetStartNewGame(thisPtr) ? "true" : "false",
		*FieldAccessor::GetLoadSavedGame(thisPtr) ? "true" : "false");

	LOG_INFO("[Hook_ParseSettings] ===== CALL #%d END (success) =====", currentCall);

	// Return 1 (success) regardless of whether the original succeeded,
	// because we've successfully populated all five fields.
	return 1;
}

// ---------------------------------------------------------------------------
// Engine allocator resolution
//
// Strategy: find FMemory::Malloc FIRST (easy – unique call-site pattern),
// then derive FMemory::Free by cross-referencing the same GMalloc global
// from within ParseSettings.
//
// From IDA, FMemory::Malloc body (at +0xF from entry):
//   48 8B 0D xx xx xx xx   mov rcx, cs:GMalloc
//
// We use this GMalloc address to identify FMemory::Free among the E8 CALLs
// inside ParseSettings (Free also loads GMalloc in its body).
//
// Final validation: Malloc/Free smoke test under SEH.
// ---------------------------------------------------------------------------

// Address of the ParseSettings function in the game binary.
// Set by Install() before ResolveEngineAllocator is called.
static uintptr_t g_parseSettingsAddress = 0;

// Known offset from ParseSettings to the `call FMemory::Free` instruction.
// From IDA:  ParseSettings+16F  call  ?Free@FMemory@@SAXPEAX@Z
// Used as a hint in the fallback path.
static constexpr size_t PARSESETTINGS_FREE_CALL_OFFSET = 0x16F;

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
// jz   ...
//
// The E8 is followed by: 48 8B D8 48 85 C0 0F 84
// This sequence (save result, null-check, branch) is extremely common after
// Malloc calls, but the full pattern with the trailing bytes is unique enough.
// ---------------------------------------------------------------------------
static const char* MALLOC_CALL_PATTERN =
"E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 0F 84 ?? ?? ?? ?? "
"33 D2 41 B8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? "
"0F 10 05 ?? ?? ?? ?? 33 C0 48 C7 43 ?? ?? ?? ?? ?? "
"80 63 ?? ?? 48 89 43";

// ---------------------------------------------------------------------------
// Helper: resolve an E8 rel32 CALL instruction at a given address.
// Returns the absolute target address, or 0 if the byte at addr is not 0xE8.
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
// Helper: dump the first N bytes at an address as hex for diagnostics.
// ---------------------------------------------------------------------------
static void DumpBytes(const char* label, uintptr_t addr, size_t count)
{
	const auto* bytes = reinterpret_cast<const uint8_t*>(addr);
	char hexBuf[256] = {};
	size_t pos = 0;
	for (size_t i = 0; i < count && pos + 3 < sizeof(hexBuf); ++i)
		pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", bytes[i]);
	LOG_DEBUG("[DumpBytes] %s at 0x%llX: %s", label, static_cast<unsigned long long>(addr), hexBuf);
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
	// Reject guard/noaccess pages
	if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))
		return false;
	// Make sure the full range fits within this committed region
	uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
	if (addr + size > regionEnd)
		return false;
	return true;
}

// ---------------------------------------------------------------------------
// Helper: extract the absolute address of the GMalloc global pointer from
// a function body.
//
// Looks for any RIP-relative MOV load into a 64-bit register:
//   (48|4C) 8B ModRM xx xx xx xx
// where ModRM has mod=00, r/m=101 (RIP-relative addressing).
//
// Returns the absolute address of the referenced global, or 0 on failure.
// ---------------------------------------------------------------------------
static uintptr_t ExtractGMallocAddress(uintptr_t funcAddr, size_t scanLen = 64)
{
	// Verify the memory is readable before scanning
	if (!IsReadableMemory(funcAddr, scanLen))
	{
		LOG_DEBUG("[ExtractGMallocAddress] Address 0x%llX (len %zu) is not readable",
			static_cast<unsigned long long>(funcAddr), scanLen);
		return 0;
	}

	const auto* bytes = reinterpret_cast<const uint8_t*>(funcAddr);

	for (size_t i = 0; i + 7 <= scanLen; ++i)
	{
		// REX.W prefix: 48 or 4C (REX.W + REX.R)
		if (bytes[i] != 0x48 && bytes[i] != 0x4C)
			continue;

		// Opcode: 8B = MOV r64, r/m64
		if (bytes[i + 1] != 0x8B)
			continue;

		// ModRM: mod=00 r/m=101 → RIP-relative
		uint8_t modrm = bytes[i + 2];
		if ((modrm & 0xC7) != 0x05)
			continue;

		int32_t disp32;
		memcpy(&disp32, &bytes[i + 3], sizeof(int32_t));

		uintptr_t globalAddr = funcAddr + i + 7 + static_cast<uintptr_t>(static_cast<intptr_t>(disp32));

		LOG_DEBUG("[ExtractGMallocAddress] Found RIP-relative MOV at +0x%zX (%02X %02X %02X) -> global at 0x%llX",
			i, bytes[i], bytes[i + 1], bytes[i + 2], static_cast<unsigned long long>(globalAddr));
		return globalAddr;
	}

	LOG_DEBUG("[ExtractGMallocAddress] No RIP-relative MOV found in first %zu bytes of 0x%llX",
		scanLen, static_cast<unsigned long long>(funcAddr));
	return 0;
}

// ---------------------------------------------------------------------------
// Smoke test: attempt a small Malloc -> write -> Free cycle under SEH.
// ---------------------------------------------------------------------------
static bool SmokeTestAllocator(FMemoryMalloc_t mallocFn, FMemoryFree_t freeFn)
{
	LOG_DEBUG("[SmokeTestAllocator] Testing Malloc=0x%llX  Free=0x%llX ...",
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mallocFn)),
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(freeFn)));

	__try
	{
		void* ptr = mallocFn(64, 16);
		if (!ptr)
		{
			LOG_WARN("[SmokeTestAllocator] Malloc returned null");
			return false;
		}

		LOG_DEBUG("[SmokeTestAllocator] Malloc returned %p", ptr);
		memset(ptr, 0xAB, 64);
		freeFn(ptr);

		LOG_INFO("[SmokeTestAllocator] PASSED - Malloc/Free cycle completed successfully");
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		DWORD code = GetExceptionCode();
		LOG_ERROR("[SmokeTestAllocator] FAILED - exception 0x%08lX during Malloc/Free cycle", code);
		return false;
	}
}

// ---------------------------------------------------------------------------
// Find FMemory::Malloc via the call-site pattern.
// The pattern starts on the E8 byte, so we just resolve the rel32.
// ---------------------------------------------------------------------------
static uintptr_t FindMallocViaPattern()
{
	auto* scanner = GetScanner();
	if (!scanner)
	{
		LOG_ERROR("[FindMalloc] Scanner not available");
		return 0;
	}

	uintptr_t callSite = scanner->FindPatternInMainModule(MALLOC_CALL_PATTERN);
	if (callSite == 0)
	{
		LOG_WARN("[FindMalloc] Malloc call-site pattern not found");
		return 0;
	}

	LOG_INFO("[FindMalloc] Call-site pattern matched at 0x%llX", static_cast<unsigned long long>(callSite));

	uintptr_t mallocAddr = ResolveE8Call(callSite);
	if (mallocAddr == 0)
	{
		LOG_WARN("[FindMalloc] Failed to decode E8 CALL at pattern match");
		return 0;
	}

	LOG_INFO("[FindMalloc] FMemory::Malloc = 0x%llX", static_cast<unsigned long long>(mallocAddr));
	DumpBytes("FMemory::Malloc", mallocAddr, 64);
	return mallocAddr;
}

// ---------------------------------------------------------------------------
// Find FMemory::Free by scanning ParseSettings for E8 CALLs whose target
// references the same GMalloc global as Malloc.
// ---------------------------------------------------------------------------
static uintptr_t FindFreeViaGMalloc(uintptr_t gmallocAddr)
{
	if (g_parseSettingsAddress == 0 || gmallocAddr == 0)
		return 0;

	LOG_INFO("[FindFree] Scanning ParseSettings at 0x%llX for calls referencing GMalloc 0x%llX...",
		static_cast<unsigned long long>(g_parseSettingsAddress),
		static_cast<unsigned long long>(gmallocAddr));

	int callsFound = 0;
	int callsReadable = 0;

	for (size_t offset = 0; offset < 0x400; ++offset)
	{
		uintptr_t instrAddr = g_parseSettingsAddress + offset;
		uintptr_t target = ResolveE8Call(instrAddr);
		if (target == 0)
			continue;

		callsFound++;

		// Validate the target address is readable before scanning its bytes
		if (!IsReadableMemory(target, 64))
		{
			LOG_DEBUG("[FindFree]   +0x%03zX -> 0x%llX (NOT READABLE, skipping)",
				offset, static_cast<unsigned long long>(target));
			continue;
		}

		callsReadable++;

		// Check if this call target references the same GMalloc
		uintptr_t candidateGMalloc = ExtractGMallocAddress(target, 64);
		if (candidateGMalloc == gmallocAddr)
		{
			LOG_INFO("[FindFree] FMemory::Free = 0x%llX (from ParseSettings+0x%zX, same GMalloc)",
				static_cast<unsigned long long>(target), offset);
			LOG_INFO("[FindFree]   Scanned %d E8 candidates (%d readable) before match",
				callsFound, callsReadable);
			DumpBytes("FMemory::Free", target, 64);
			return target;
		}
	}

	LOG_WARN("[FindFree] No call target in ParseSettings references GMalloc 0x%llX",
		static_cast<unsigned long long>(gmallocAddr));
	LOG_WARN("[FindFree]   Scanned %d E8 candidates (%d readable), none matched",
		callsFound, callsReadable);
	return 0;
}

// ---------------------------------------------------------------------------
// Find FMemory::Free via the known ParseSettings offset (fallback).
static uintptr_t FindFreeViaOffset(uintptr_t mallocAddr)
{
	if (g_parseSettingsAddress == 0)
		return 0;

	uintptr_t freeCallSite = g_parseSettingsAddress + PARSESETTINGS_FREE_CALL_OFFSET;

	// Check the call site itself is readable
	if (!IsReadableMemory(freeCallSite, 5))
	{
		LOG_WARN("[FindFree:Offset] ParseSettings+0x%zX is not readable", PARSESETTINGS_FREE_CALL_OFFSET);
		return 0;
	}

	const auto* callByte = reinterpret_cast<const uint8_t*>(freeCallSite);
	if (*callByte != 0xE8)
	{
		LOG_WARN("[FindFree:Offset] Byte at ParseSettings+0x%zX is 0x%02X, not 0xE8",
			PARSESETTINGS_FREE_CALL_OFFSET, *callByte);
		return 0;
	}

	uintptr_t freeAddr = ResolveE8Call(freeCallSite);
	if (freeAddr == 0)
		return 0;

	// Validate target is readable
	if (!IsReadableMemory(freeAddr, 64))
	{
		LOG_WARN("[FindFree:Offset] Resolved target 0x%llX is not readable",
			static_cast<unsigned long long>(freeAddr));
		return 0;
	}

	LOG_INFO("[FindFree:Offset] Candidate FMemory::Free = 0x%llX (from ParseSettings+0x%zX)",
		static_cast<unsigned long long>(freeAddr), PARSESETTINGS_FREE_CALL_OFFSET);
	DumpBytes("FMemory::Free candidate", freeAddr, 64);

	// Validate with smoke test
	if (!SmokeTestAllocator(reinterpret_cast<FMemoryMalloc_t>(mallocAddr),
		reinterpret_cast<FMemoryFree_t>(freeAddr)))
	{
		LOG_WARN("[FindFree:Offset] Smoke test FAILED for offset candidate");
		return 0;
	}

	return freeAddr;
}

// ---------------------------------------------------------------------------
// Resolve both FMemory::Malloc and FMemory::Free from the game binary.
//
// Strategy:
//   1. Find FMemory::Malloc via a unique call-site pattern.
//   2. Extract the GMalloc global address from Malloc's body.
//   3. Scan ParseSettings for E8 CALLs whose targets also reference GMalloc
//      → that gives us FMemory::Free.
//   4. Fallback: use a known offset from ParseSettings to locate Free.
//   5. Smoke-test the pair under SEH before committing.
// ---------------------------------------------------------------------------
static bool ResolveEngineAllocator()
{
	LOG_INFO("[ResolveEngineAllocator] Resolving FMemory::Malloc and FMemory::Free...");

	// Step 1: Find FMemory::Malloc
	uintptr_t mallocAddr = FindMallocViaPattern();
	if (mallocAddr == 0)
	{
		LOG_ERROR("[ResolveEngineAllocator] Could not find FMemory::Malloc");
		return false;
	}

	// Step 2: Extract GMalloc address from Malloc's body
	uintptr_t gmallocAddr = ExtractGMallocAddress(mallocAddr, 64);
	if (gmallocAddr == 0)
	{
		LOG_WARN("[ResolveEngineAllocator] Could not extract GMalloc address from Malloc body");
	}
	else
	{
		LOG_INFO("[ResolveEngineAllocator] GMalloc global at 0x%llX",
			static_cast<unsigned long long>(gmallocAddr));
	}

	// Step 3: Find FMemory::Free via GMalloc cross-reference in ParseSettings
	uintptr_t freeAddr = 0;
	if (gmallocAddr != 0)
	{
		freeAddr = FindFreeViaGMalloc(gmallocAddr);
	}

	// Step 4: Fallback - use known offset from ParseSettings
	if (freeAddr == 0)
	{
		LOG_WARN("[ResolveEngineAllocator] GMalloc cross-reference failed, trying offset fallback...");
		freeAddr = FindFreeViaOffset(mallocAddr);
	}

	if (freeAddr == 0)
	{
		LOG_ERROR("[ResolveEngineAllocator] Could not find FMemory::Free");
		return false;
	}

	// Step 5: Smoke test the Malloc/Free pair
	if (!SmokeTestAllocator(reinterpret_cast<FMemoryMalloc_t>(mallocAddr),
		reinterpret_cast<FMemoryFree_t>(freeAddr)))
	{
		LOG_ERROR("[ResolveEngineAllocator] Smoke test FAILED - not safe to use these allocators");
		return false;
	}

	// Commit
	g_engineMalloc = reinterpret_cast<FMemoryMalloc_t>(mallocAddr);
	g_engineFree = reinterpret_cast<FMemoryFree_t>(freeAddr);

	LOG_INFO("[ResolveEngineAllocator] SUCCESS - FMemory::Malloc=0x%llX, FMemory::Free=0x%llX",
		static_cast<unsigned long long>(mallocAddr),
		static_cast<unsigned long long>(freeAddr));

	return true;
}

// ---------------------------------------------------------------------------
// Public API: ParseSettingsHook namespace
// ---------------------------------------------------------------------------
void ParseSettingsHook::Install(uintptr_t targetAddress)
{
	LOG_INFO("[ParseSettingsHook::Install] Installing hook at 0x%llX...",
		static_cast<unsigned long long>(targetAddress));

	if (g_hookHandle)
	{
		LOG_WARN("[ParseSettingsHook::Install] Hook already installed - skipping");
		return;
	}

	// Store the address for use by engine allocator resolution
	g_parseSettingsAddress = targetAddress;

	// Resolve engine allocator (FMemory::Malloc / FMemory::Free)
	if (!ResolveEngineAllocator())
	{
		LOG_WARN("[ParseSettingsHook::Install] Engine allocator resolution failed - "
			"hook will still be installed but FString overrides will be disabled");
	}

	// Install the inline hook via the mod loader's hook interface
	auto* hooks = GetHooks();
	if (!hooks || !hooks->InstallHook)
	{
		LOG_ERROR("[ParseSettingsHook::Install] Hook interface not available!");
		return;
	}

	g_hookHandle = hooks->InstallHook(
		targetAddress,
		reinterpret_cast<void*>(&Hook_ParseSettings),
		reinterpret_cast<void**>(&g_originalParseSettings));

	if (!g_hookHandle)
	{
		LOG_ERROR("[ParseSettingsHook::Install] InstallHook failed!");
		return;
	}

	LOG_INFO("[ParseSettingsHook::Install] Hook installed successfully (handle=%p)", g_hookHandle);
}

void ParseSettingsHook::Remove()
{
	if (!g_hookHandle)
	{
		LOG_DEBUG("[ParseSettingsHook::Remove] No hook installed - nothing to remove");
		return;
	}

	LOG_INFO("[ParseSettingsHook::Remove] Removing hook (handle=%p)...", g_hookHandle);

	auto* hooks = GetHooks();
	if (hooks && hooks->RemoveHook)
	{
		hooks->RemoveHook(g_hookHandle);
	}
	else
	{
		LOG_WARN("[ParseSettingsHook::Remove] Hook interface not available - cannot remove hook cleanly");
	}

	g_hookHandle = nullptr;
	g_originalParseSettings = nullptr;
	g_engineMalloc = nullptr;
	g_engineFree = nullptr;
	g_parseSettingsAddress = 0;

	LOG_INFO("[ParseSettingsHook::Remove] Hook removed successfully");
}
