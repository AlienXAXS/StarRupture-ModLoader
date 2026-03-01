#include "cmd_save.h"
#include "command_handler.h"
#include "plugin_helpers.h"
#include "../state/game_thread_dispatch.h"

#include <windows.h>
#include <chrono>

// SDK access: UCrSaveSubsystem and UObject::FindObjectFast are engine types.
// Only available when the full SDK is compiled in.
#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)
#  include "CoreUObject_classes.hpp"
#  define CMD_SAVE_HAS_SDK 1
#else
#  define CMD_SAVE_HAS_SDK 0
#endif

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
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B F9 E8 ?? ?? ?? ?? 33 ED 48 8B D8 48 85 C0 74 ?? E8 ?? ?? ?? ?? 48 8B 53 ?? 4C 8D 40 ?? 48 63 40 ?? 3B 42 ?? 7F ?? 48 8B C8 48 8B 42 ?? ?? ?? ?? ?? 74 ?? 48 8B DD 48 8D 54 24 ?? 48 8B CB E8 ?? ?? ?? ?? 48 63 5C 24";

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
	// Command handler — posts save work to the game thread and waits
	// -----------------------------------------------------------------------
	static std::string Handle(const std::string& /*args*/)
	{
		LOG_INFO("[RCON] Save command received via RCON.");

		if (!g_saveFunc)
		{
			LOG_ERROR("[RCON] SaveNextSaveGame function not resolved - cannot force save.");
			return "Error: save function not found (pattern not matched).\n";
		}

		// The save subsystem requires IsInGameThread().  RCON command handlers
		// run on a network thread, so we must bounce the actual work to the
		// game thread via GameThreadDispatch::Post and wait for the result.
		auto fut = GameThreadDispatch::Post([]() -> std::string {
#if CMD_SAVE_HAS_SDK
			// Find the UCrSaveSubsystem *instance* (not the UClass or CDO).
			SDK::UObject* subsystem = nullptr;
			{
				auto& GObjects = SDK::UObject::GObjects;
				const int32_t count = GObjects->Num();
				for (int32_t i = 0; i < count; ++i)
				{
					SDK::UObject* obj = GObjects->GetByIndex(i);
					if (!obj || !obj->Class)
						continue;

					if (obj->IsDefaultObject())
						continue;

					if (obj->Class->GetName() == "CrSaveSubsystem")
					{
						subsystem = obj;
						break;
					}
				}
			}
#else
			void* subsystem = nullptr;
#endif

			if (!subsystem)
			{
				LOG_ERROR("[RCON] UCrSaveSubsystem instance not found - world may not be loaded yet.");
				return "Error: save subsystem not available (world may not be loaded yet).\n";
			}

#if CMD_SAVE_HAS_SDK
			LOG_INFO("[RCON] Forcing world save via UCrSaveSubsystem::SaveNextSaveGame "
				     "(instance at %p, name: %s)...", subsystem, subsystem->GetName().c_str());
#endif

			DWORD exCode = TryCallSave(subsystem);
			if (exCode == 0)
			{
				LOG_INFO("[RCON] World save completed successfully.");
				return "World saved successfully.\n";
			}

			LOG_ERROR("[RCON] Exception during save (0x%08lX) - save may be incomplete.", exCode);
			return "Error: exception occurred during save.\n";
		});

		// Block the RCON network thread waiting for the game thread to
		// execute the save.  30 seconds should be more than enough.
		using namespace std::chrono_literals;
		if (fut.wait_for(30s) == std::future_status::ready)
			return fut.get();

		LOG_WARN("[RCON] Save command timed out waiting for game thread (30s).");
		return "Error: save command timed out waiting for game thread.\n";
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
