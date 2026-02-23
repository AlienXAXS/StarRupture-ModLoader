#include "set_password.h"
#include "plugin_helpers.h"

#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

// ---------------------------------------------------------------------------
// Minimal FString mirror matching UE5 TArray<TCHAR> layout.
// ---------------------------------------------------------------------------
struct PasswordEngineString
{
	wchar_t* Data;
	int32_t  Num; // includes null terminator
	int32_t  Max;
};

// ---------------------------------------------------------------------------
// Command-line parameter names
// ---------------------------------------------------------------------------
static constexpr const wchar_t* PARAM_PASSWORD = L"-Password=";
static constexpr const wchar_t* PARAM_PLAYER_PASSWORD = L"-PlayerPassword=";

// ---------------------------------------------------------------------------
// Helper: parse a single command-line parameter value.
// Quoted values (e.g. -Password="my pass") are supported.
// ---------------------------------------------------------------------------
static bool GetCommandLineParam(const wchar_t* paramName, std::wstring& out)
{
	const wchar_t* cmdLine = GetCommandLineW();
	if (!cmdLine)
		return false;

	const wchar_t* pos = wcsstr(cmdLine, paramName);
	if (!pos)
		return false;

	pos += wcslen(paramName);

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
// Original function signatures.
//
// Both SetPassword and SetPlayerPassword follow the same signature:
//   FString* __fastcall Func(void* this, FString* result, FString* InPassword)
//
// The caller provides a stack-allocated FString for 'result' and a pointer
// to the password FString.  The function returns 'result' (pass-through).
// ---------------------------------------------------------------------------
typedef PasswordEngineString* (__fastcall* SetPassword_t)(
	void* thisPtr,
	PasswordEngineString* result,
	PasswordEngineString* inPassword);

// ---------------------------------------------------------------------------
// Hook state – SetPassword (-Password=)
// ---------------------------------------------------------------------------
static SetPassword_t g_originalSetPassword = nullptr;
static HookHandle    g_hookHandleSetPassword = nullptr;

// ---------------------------------------------------------------------------
// Hook state – SetPlayerPassword (-PlayerPassword=)
// ---------------------------------------------------------------------------
static SetPassword_t g_originalSetPlayerPassword = nullptr;
static HookHandle    g_hookHandleSetPlayerPassword = nullptr;

// ---------------------------------------------------------------------------
// Detour: SetPassword
//
// Strategy: always call the original first so the engine initialises its
// internal state normally.  If the -Password= command-line parameter is
// present we then call the original a second time with our override value
// so the engine's own validation / hashing path is used.
// ---------------------------------------------------------------------------
static PasswordEngineString* __fastcall Hook_SetPassword(
	void* thisPtr,
	PasswordEngineString* result,
	PasswordEngineString* inPassword)
{
	LOG_DEBUG("[Hook_SetPassword] Called (thisPtr=0x%p)", thisPtr);

	// Always let the original run first
	PasswordEngineString* origResult = g_originalSetPassword(thisPtr, result, inPassword);

	std::wstring overrideValue;
	if (!GetCommandLineParam(PARAM_PASSWORD, overrideValue))
	{
		LOG_DEBUG("[Hook_SetPassword] No -Password= on command line, using default");
		return origResult;
	}

	LOG_INFO("[Hook_SetPassword] Overriding server password from command line");

	// Build a temporary FString on the stack that points to our override
	const int32_t len = static_cast<int32_t>(overrideValue.size()) + 1;
	PasswordEngineString overrideStr;
	overrideStr.Data = const_cast<wchar_t*>(overrideValue.c_str());
	overrideStr.Num = len;
	overrideStr.Max = len;

	// Call the original again with the override password so the engine
	// processes it through its own code path
	origResult = g_originalSetPassword(thisPtr, result, &overrideStr);

	LOG_INFO("[Hook_SetPassword] Server password set successfully");
	return origResult;
}

// ---------------------------------------------------------------------------
// Detour: SetPlayerPassword
// ---------------------------------------------------------------------------
static PasswordEngineString* __fastcall Hook_SetPlayerPassword(
	void* thisPtr,
	PasswordEngineString* result,
	PasswordEngineString* inPassword)
{
	LOG_DEBUG("[Hook_SetPlayerPassword] Called (thisPtr=0x%p)", thisPtr);

	// Always let the original run first
	PasswordEngineString* origResult = g_originalSetPlayerPassword(thisPtr, result, inPassword);

	std::wstring overrideValue;
	if (!GetCommandLineParam(PARAM_PLAYER_PASSWORD, overrideValue))
	{
		LOG_DEBUG("[Hook_SetPlayerPassword] No -PlayerPassword= on command line, using default");
		return origResult;
	}

	LOG_INFO("[Hook_SetPlayerPassword] Overriding player password from command line");

	const int32_t len = static_cast<int32_t>(overrideValue.size()) + 1;
	PasswordEngineString overrideStr;
	overrideStr.Data = const_cast<wchar_t*>(overrideValue.c_str());
	overrideStr.Num = len;
	overrideStr.Max = len;

	origResult = g_originalSetPlayerPassword(thisPtr, result, &overrideStr);

	LOG_INFO("[Hook_SetPlayerPassword] Player password set successfully");
	return origResult;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void SetPasswordHook::Install(uintptr_t setPasswordAddress, uintptr_t setPlayerPasswordAddress)
{
	auto* hooks = GetHooks();
	if (!hooks || !hooks->InstallHook)
	{
		LOG_ERROR("[SetPasswordHook::Install] Hook interface not available!");
		return;
	}

	// --- SetPassword ---
	if (setPasswordAddress != 0)
	{
		if (g_hookHandleSetPassword)
		{
			LOG_WARN("[SetPasswordHook::Install] SetPassword hook already installed - skipping");
		}
		else
		{
			LOG_INFO("[SetPasswordHook::Install] Installing SetPassword hook at 0x%llX...",
				static_cast<unsigned long long>(setPasswordAddress));

			g_hookHandleSetPassword = hooks->InstallHook(
				setPasswordAddress,
				reinterpret_cast<void*>(&Hook_SetPassword),
				reinterpret_cast<void**>(&g_originalSetPassword));

			if (g_hookHandleSetPassword)
				LOG_INFO("[SetPasswordHook::Install] SetPassword hook installed (handle=%p)", g_hookHandleSetPassword);
			else
				LOG_ERROR("[SetPasswordHook::Install] SetPassword InstallHook failed!");
		}
	}
	else
	{
		LOG_WARN("[SetPasswordHook::Install] SetPassword address is 0 - skipping");
	}

	// --- SetPlayerPassword ---
	if (setPlayerPasswordAddress != 0)
	{
		if (g_hookHandleSetPlayerPassword)
		{
			LOG_WARN("[SetPasswordHook::Install] SetPlayerPassword hook already installed - skipping");
		}
		else
		{
			LOG_INFO("[SetPasswordHook::Install] Installing SetPlayerPassword hook at 0x%llX...",
				static_cast<unsigned long long>(setPlayerPasswordAddress));

			g_hookHandleSetPlayerPassword = hooks->InstallHook(
				setPlayerPasswordAddress,
				reinterpret_cast<void*>(&Hook_SetPlayerPassword),
				reinterpret_cast<void**>(&g_originalSetPlayerPassword));

			if (g_hookHandleSetPlayerPassword)
				LOG_INFO("[SetPasswordHook::Install] SetPlayerPassword hook installed (handle=%p)", g_hookHandleSetPlayerPassword);
			else
				LOG_ERROR("[SetPasswordHook::Install] SetPlayerPassword InstallHook failed!");
		}
	}
	else
	{
		LOG_WARN("[SetPasswordHook::Install] SetPlayerPassword address is 0 - skipping");
	}
}

void SetPasswordHook::Remove()
{
	auto* hooks = GetHooks();

	if (g_hookHandleSetPassword)
	{
		LOG_INFO("[SetPasswordHook::Remove] Removing SetPassword hook (handle=%p)...", g_hookHandleSetPassword);
		if (hooks && hooks->RemoveHook)
			hooks->RemoveHook(g_hookHandleSetPassword);
		else
			LOG_WARN("[SetPasswordHook::Remove] Hook interface not available");

		g_hookHandleSetPassword = nullptr;
		g_originalSetPassword = nullptr;
	}

	if (g_hookHandleSetPlayerPassword)
	{
		LOG_INFO("[SetPasswordHook::Remove] Removing SetPlayerPassword hook (handle=%p)...", g_hookHandleSetPlayerPassword);
		if (hooks && hooks->RemoveHook)
			hooks->RemoveHook(g_hookHandleSetPlayerPassword);
		else
			LOG_WARN("[SetPasswordHook::Remove] Hook interface not available");

		g_hookHandleSetPlayerPassword = nullptr;
		g_originalSetPlayerPassword = nullptr;
	}

	LOG_INFO("[SetPasswordHook::Remove] Password hooks removed");
}
