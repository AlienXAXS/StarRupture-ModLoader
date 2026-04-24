#pragma once
#include <windows.h>
#include <string>

// Validates that the running executable matches the expected build type.
// Must be called before any modloader subsystem is initialised.
class GameTypeChecker
{
public:
    enum class Result { Correct, SilentBail, ErrorAndExit };

    static Result Check()
    {
        char path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, path, MAX_PATH);

        std::string exeName(path);
        const auto pos = exeName.find_last_of("\\/");
        if (pos != std::string::npos)
            exeName = exeName.substr(pos + 1);

        constexpr const char* CLIENT_EXE = "StarRuptureGameSteam-Win64-Shipping.exe";
        constexpr const char* SERVER_EXE = "StarRuptureServerEOS-Win64-Shipping.exe";

#if defined(MODLOADER_SERVER_BUILD)
        if (exeName == CLIENT_EXE)
        {
            MessageBoxA(
                nullptr,
                "You have installed the StarRupture Server ModLoader into a game client installation.\n\n"
                "The server modloader cannot run inside the game client.\n"
                "Please install the client version of the ModLoader or remove dwmapi.dll from your game client directory.\n\n"
                "The game will start without the ModLoader.",
                "StarRupture ModLoader - Wrong Build",
                MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
            return Result::SilentBail;
        }
#elif defined(MODLOADER_CLIENT_BUILD)
        if (exeName == SERVER_EXE)
            return Result::SilentBail;
#endif
        return Result::Correct;
    }
};
