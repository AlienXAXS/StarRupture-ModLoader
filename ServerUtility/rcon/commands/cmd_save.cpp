#include "cmd_save.h"
#include "command_handler.h"
#include "plugin_helpers.h"

#include "CoreUObject_classes.hpp"

#include <windows.h>

namespace Cmd_Save
{
	// -----------------------------------------------------------------------
	// UCrSaveSubsystem::SaveNextSaveGame(UCrSaveSubsystem* this)
	//
	// This is the game's internal function that determines whether the
	// server is dedicated or not and serialises the current world state
	// to the appropriate save file.  We call it directly to force an
	// immediate save on demand.
	// -----------------------------------------------------------------------
	static constexpr const char* SAVE_PATTERN =
		"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 4C 89 74 24 ?? "
		"55 48 8B EC 48 83 EC ?? 48 8B F9 E8 ?? ?? ?? ?? 45 33 F6 "
		"48 8B D8 48 85 C0 74 ?? E8 ?? ?? ?? ?? 48 8B 53 ?? 4C 8D 40 ?? "
		"48 63 40 ?? 3B 42 ?? 7F ?? 48 8B C8 48 8B 42 ?? ?? ?? ?? ?? "
		"74 ?? 49 8B DE 48 8D 55 ?? 48 8B CB E8 ?? ?? ?? ?? 48 63 5D";

	using SaveNextSaveGame_t = void(__fastcall*)(void* thisPtr);
	static SaveNextSaveGame_t g_saveFunc = nullptr;

	// -----------------------------------------------------------------------
	// SEH-protected save call
	//
	// Isolated into its own function because MSVC does not allow __try in
	// functions that contain C++ objects requiring unwinding (std::string).
	// Returns: 0 = success, non-zero = exception code.
	// -----------------------------------------------------------------------
	static DWORD TryCallSave(void* subsystem)
	{
		__try
		{
			g_saveFunc(subsystem);
			return 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return GetExceptionCode();
		}
	}

	// -----------------------------------------------------------------------
	// Command handler
	// -----------------------------------------------------------------------
	static std::string Handle(const std::string& /*args*/)
	{
		LOG_INFO("[RCON] Save command received via RCON.");

		if (!g_saveFunc)
		{
			LOG_ERROR("[RCON] SaveNextSaveGame function not resolved - cannot force save.");
			return "Error: save function not found (pattern not matched).\n";
		}

		// Look up the live UCrSaveSubsystem instance via the SDK's global
		// object array.  This avoids the need for any hook to capture it.
		auto* subsystem = SDK::UObject::FindObjectFast("CrSaveSubsystem");
		if (!subsystem)
		{
			LOG_ERROR("[RCON] UCrSaveSubsystem instance not found - world may not be loaded yet.");
			return "Error: save subsystem not available (world may not be loaded yet).\n";
		}

		LOG_INFO("[RCON] Forcing world save via UCrSaveSubsystem::SaveNextSaveGame (instance at %p)...", subsystem);

		DWORD exCode = TryCallSave(subsystem);
		if (exCode == 0)
		{
			LOG_INFO("[RCON] World save completed successfully.");
			return "World saved successfully.\n";
		}
		else
		{
			LOG_ERROR("[RCON] Exception during save (0x%08lX) - save may be incomplete.", exCode);
			return "Error: exception occurred during save.\n";
		}
	}

	// -----------------------------------------------------------------------
	// Registration
	// -----------------------------------------------------------------------
	void Register(CommandHandler& handler)
	{
		auto scanner = GetScanner();

		// Resolve UCrSaveSubsystem::SaveNextSaveGame
		if (scanner)
		{
			uintptr_t addr = scanner->FindPatternInMainModule(SAVE_PATTERN);
			if (addr)
			{
				g_saveFunc = reinterpret_cast<SaveNextSaveGame_t>(addr);
				LOG_INFO("[RCON] UCrSaveSubsystem::SaveNextSaveGame resolved at 0x%llX",
					static_cast<unsigned long long>(addr));
			}
			else
			{
				LOG_ERROR("[RCON] Failed to find UCrSaveSubsystem::SaveNextSaveGame pattern - "
					"save command will not work until pattern is updated.");
			}
		}

		handler.Register(
			{ "save", "savegame", "forcesave" },
			"Force an immediate save of the current world state",
			Handle);
	}
}
