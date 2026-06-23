#include "pch.h"
#include "text_key.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "Basic.hpp"

namespace Hooks::TextKey
{
	// void __fastcall FTextKey::FTextKey(FTextKey* this, const wchar_t* InStr)
	// FTextKey is not reflected in the SDK -- pass the raw pointer through.
	using FTextKey_Ctor_t = void(__fastcall*)(void* This, const wchar_t* InStr);

	static Hook g_hook;
	static FTextKey_Ctor_t g_original = nullptr;

	static void __fastcall Detour(void* This, const wchar_t* InStr)
	{
		if (!g_original)
			return;

		try
		{
			g_original(This, InStr);
			return;
		}
		catch (const std::exception& e)
		{
			ModLoaderLogger::LogError(L"[TextKey] Exception in FTextKey::FTextKey: %S", e.what());
		}
		catch (...)
		{
			ModLoaderLogger::LogError(L"[TextKey] Unknown exception in FTextKey::FTextKey");
		}
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[TextKey] Installing hook...");

		uintptr_t addr = Scanner::FindPatternInMainModule(
			"FTextKey::FTextKey", ScanPatterns::FTextKey_FTextKey);
		if (!addr)
		{
			ModLoaderLogger::LogError(L"[TextKey] Failed to find FTextKey::FTextKey pattern");
			return false;
		}

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[TextKey] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[TextKey] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[TextKey] Removing hook...");
		g_hook.Remove();
	}

	bool IsInstalled()
	{
		return g_hook.installed;
	}

	uintptr_t GetOriginalPtr()
	{
		return reinterpret_cast<uintptr_t>(g_original);
	}
}
