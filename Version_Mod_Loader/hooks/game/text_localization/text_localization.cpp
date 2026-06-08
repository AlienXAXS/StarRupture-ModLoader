#include "pch.h"
#include "text_localization.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "Basic.hpp"

namespace Hooks::TextLocalization
{
	// FText* __fastcall FText::AsLocalizable_Advanced(FText* result,
	//     const FTextKey* Namespace, const FTextKey* Key, const wchar_t* String)
	// FTextKey is not reflected in the SDK -- pass the raw pointers through.
	using FText_AsLocalizable_Advanced_t =
		SDK::FText*(__fastcall*)(SDK::FText* result, const void* Namespace, const void* Key, const wchar_t* String);

	static Hook g_hook;
	static FText_AsLocalizable_Advanced_t g_original = nullptr;

	static SDK::FText* __fastcall Detour(SDK::FText* result, const void* Namespace, const void* Key, const wchar_t* String)
	{
		if (!g_original)
			return result;

		try
		{
			return g_original(result, Namespace, Key, String);
		}
		catch (const std::exception& e)
		{
			ModLoaderLogger::LogError(L"[TextLocalization] Exception in AsLocalizable_Advanced: %S", e.what());
		}
		catch (...)
		{
			ModLoaderLogger::LogError(L"[TextLocalization] Unknown exception in AsLocalizable_Advanced");
		}

		return result;
	}

	bool Install()
	{
		ModLoaderLogger::LogInfo(L"[TextLocalization] Installing hook...");

		uintptr_t addr = Scanner::FindPatternInMainModule(
			"FText::AsLocalizable_Advanced", ScanPatterns::FText_AsLocalizable_Advanced);
		if (!addr)
		{
			ModLoaderLogger::LogError(L"[TextLocalization] Failed to find FText::AsLocalizable_Advanced pattern");
			return false;
		}

		bool hookOk = g_hook.Install(
			addr,
			reinterpret_cast<void*>(&Detour),
			reinterpret_cast<void**>(&g_original));

		if (hookOk)
			ModLoaderLogger::LogInfo(L"[TextLocalization] Hook installed successfully");
		else
			ModLoaderLogger::LogError(L"[TextLocalization] Hook installation failed");

		return hookOk;
	}

	void Remove()
	{
		ModLoaderLogger::LogInfo(L"[TextLocalization] Removing hook...");
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
