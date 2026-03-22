#include "auto_profession.h"
#include "plugin_helpers.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// ACrGameModeBase::GetProfessionForNewPlayer
//
// Pattern (from IDA):
//   48 8B C4 48 89 50 ?? 55 53 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 48 89 70 ?? 48 8B F2 48 89 78 ?? 48 8B F9
//
// We patch the prologue to:
//   mov eax, 1   ; B8 01 00 00 00   (return EProfessionType::Soldier)
//   ret          ; C3
//
// Total: 6 bytes.  The original prologue is at least 6 bytes long (starts
// with `48 8B C4 48 89 50 xx` - 7 bytes), so this is safe.
// ---------------------------------------------------------------------------

static constexpr auto GET_PROFESSION_PATTERN =
	"48 8B C4 48 89 50 ?? 55 53 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 48 89 70 ?? 48 8B F2 48 89 78 ?? 48 8B F9";

// The 6-byte patch: mov eax, 1 ; ret
static constexpr uint8_t PATCH_BYTES[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
static constexpr size_t PATCH_SIZE = sizeof(PATCH_BYTES);

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static uintptr_t g_patchAddress = 0; // Start of GetProfessionForNewPlayer
static uint8_t g_originalBytes[PATCH_SIZE]{}; // Saved original prologue bytes
static bool g_patched = false;

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------
void AutoProfessionHook::Install()
{
	auto* scanner = GetScanner();
	auto* hooks = GetHooks();
	if (!scanner || !hooks)
	{
		LOG_ERROR("[AutoProfession] Scanner or hooks interface not available");
		return;
	}

	LOG_INFO("[AutoProfession] Scanning for ACrGameModeBase::GetProfessionForNewPlayer...");

	uintptr_t addr = scanner->FindPatternInMainModule(GET_PROFESSION_PATTERN);
	if (addr == 0)
	{
		LOG_ERROR("[AutoProfession] Pattern scan failed - could not locate GetProfessionForNewPlayer");
		return;
	}

	HMODULE mainModule = GetModuleHandleW(nullptr);
	auto base = reinterpret_cast<uintptr_t>(mainModule);

	LOG_INFO("[AutoProfession] Found GetProfessionForNewPlayer at 0x%llX (base+0x%llX)",
	         static_cast<unsigned long long>(addr),
	         static_cast<unsigned long long>(addr - base));

	// Save original bytes so we can restore them on Remove()
	if (!hooks->Memory->Read(addr, g_originalBytes, PATCH_SIZE))
	{
		LOG_ERROR("[AutoProfession] Failed to read original bytes at 0x%llX",
		          static_cast<unsigned long long>(addr));
		return;
	}

	LOG_DEBUG("[AutoProfession] Original prologue: %02X %02X %02X %02X %02X %02X",
	          g_originalBytes[0], g_originalBytes[1], g_originalBytes[2],
	          g_originalBytes[3], g_originalBytes[4], g_originalBytes[5]);

	// Apply the patch: mov eax, 1 ; ret
	if (!hooks->Memory->Patch(addr, PATCH_BYTES, PATCH_SIZE))
	{
		LOG_ERROR("[AutoProfession] PatchMemory failed at 0x%llX",
		          static_cast<unsigned long long>(addr));
		return;
	}

	g_patchAddress = addr;
	g_patched = true;

	LOG_INFO("[AutoProfession] SUCCESS - GetProfessionForNewPlayer patched to always return Soldier (1)");
}

void AutoProfessionHook::Remove()
{
	if (!g_patched || g_patchAddress == 0)
	{
		LOG_DEBUG("[AutoProfession] No patch to restore");
		return;
	}

	auto* hooks = GetHooks();
	if (hooks && hooks->Memory)
	{
		if (hooks->Memory->Patch(g_patchAddress, g_originalBytes, PATCH_SIZE))
		{
			LOG_INFO("[AutoProfession] Restored original GetProfessionForNewPlayer prologue at 0x%llX",
			         static_cast<unsigned long long>(g_patchAddress));
		}
		else
		{
			LOG_WARN("[AutoProfession] Failed to restore original bytes at 0x%llX",
			         static_cast<unsigned long long>(g_patchAddress));
		}
	}

	g_patchAddress = 0;
	g_patched = false;
	std::memset(g_originalBytes, 0, PATCH_SIZE);
}
