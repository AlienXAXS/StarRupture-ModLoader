#include "pch.h"
#include "framegen_pause.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "logging/logger.h"
#include "Engine_classes.hpp"
#include "../engine_exec/engine_exec.h"
#include "../engine_shutdown/engine_shutdown.h"
#include "../world_begin_play/world_begin_play.h"
#include "../world_end_play/world_end_play.h"
#include "utils/game_thread_dispatch.h"
#include <string>

namespace Hooks::FrameGenPause
{
    static const wchar_t* const kEnableCVar = L"r.Streamline.DLSSG.Enable";

    // NVIDIA's Streamline plugin passes this as sl::DLSSGFlags::eRetainResourcesWhenOff
    // with the DLSS-G options it sets each frame. Off (the plugin default),
    // turning DLSS-G off releases its own buffers straight away -- while
    // frames already submitted can still read them. On, they stay allocated
    // until DLSS-G is turned back on or the swapchain goes away.
    static const wchar_t* const kRetainCVar = L"r.Streamline.DLSSG.RetainResourcesWhenOff";

    // A tick longer than this means the engine is loading or hitching, and a
    // renderer that is reallocating is the worst moment to switch frame
    // generation. The state only changes after this many seconds of ticks
    // that are all shorter.
    static constexpr float kMaxSteadyTickSeconds = 0.1f;
    static constexpr float kSteadySecondsRequired = 1.0f;

    // Ticks between turning retain on and turning DLSS-G off, so the render
    // thread has picked up the retain flag before the frame that disables.
    static constexpr int kRetainLeadTicks = 3;

    static bool  s_lastWantPaused = false;
    static bool  s_requestDone = false;   // nothing more to do for the current request
    static bool  s_retainSet = false;     // we changed kRetainCVar and owe a restore
    static int   s_retainTicks = 0;
    static bool  s_paused = false;        // we turned DLSS-G off and owe a restore
    static int   s_savedEnable = -1;
    static int   s_savedRetain = -1;

    // Set by the world begin/end hooks, both on the game thread.
    static bool  s_worldUp = false;
    static float s_steadySeconds = 0.0f;

    // A bare cvar name run through Exec logs 'r.Streamline.DLSSG.Enable = "N"'
    // (IConsoleManager::ProcessUserConsoleInput writes that through the same
    // FOutputDevice EngineExec captures). A bool cvar prints "true"/"false".
    static bool ReadCurrentValue(const wchar_t* cvar, int& outValue)
    {
        std::wstring result;
        if (Hooks::EngineExec::Execute(cvar, result) != Hooks::EngineExec::Result::Handled)
            return false;

        const size_t eq = result.find(L'=');
        if (eq == std::wstring::npos)
            return false;

        for (size_t i = eq + 1; i < result.size(); ++i)
        {
            const wchar_t c = result[i];
            if (c >= L'0' && c <= L'9')
            {
                outValue = c - L'0';
                return true;
            }
            if (result.compare(i, 4, L"true") == 0)  { outValue = 1; return true; }
            if (result.compare(i, 5, L"false") == 0) { outValue = 0; return true; }
        }
        return false;
    }

    static void WriteValue(const wchar_t* cvar, int value)
    {
        wchar_t cmd[96];
        swprintf_s(cmd, L"%s %d", cvar, value);
        std::wstring result;
        Hooks::EngineExec::Execute(cmd, result);
    }

    // True when a world has finished BeginPlay, no travel has started since,
    // and the engine has been ticking smoothly for a while. The crash this
    // guards against came from a switch made while the game was starting a
    // save load: the menu world was still up but ticking about once a second.
    static bool IsSettled()
    {
        if (!s_worldUp || s_steadySeconds < kSteadySecondsRequired)
            return false;

        SDK::UWorld* world = SDK::UWorld::GetWorld();
        return world && world->GameState && world->GameState->bReplicatedHasBegunPlay;
    }

    static void RestoreRetain()
    {
        if (s_retainSet && s_savedRetain >= 0 && s_savedRetain != 1)
            WriteValue(kRetainCVar, s_savedRetain);
        s_retainSet = false;
        s_savedRetain = -1;
    }

    static void RestoreAll()
    {
        if (s_paused && s_savedEnable >= 0)
        {
            WriteValue(kEnableCVar, s_savedEnable);
            ModLoaderLogger::LogInfo(L"[FrameGenPause] Restoring %s to %d", kEnableCVar, s_savedEnable);
        }
        s_paused = false;
        s_savedEnable = -1;

        // After DLSS-G is back on: retain only matters when it is switched off.
        RestoreRetain();
    }

    static void OnWorldBeginPlay(SDK::UWorld* /*world*/, const char* /*worldName*/)
    {
        s_worldUp = true;
        s_steadySeconds = 0.0f;
    }

    static void OnWorldEndPlay(SDK::UWorld* /*world*/, const char* /*worldName*/)
    {
        s_worldUp = false;
        s_steadySeconds = 0.0f;
    }

    static void OnEngineShutdown()
    {
        if (!s_paused && !s_retainSet)
            return;
        if (!GameThreadDispatch::IsGameThread())
        {
            ModLoaderLogger::LogWarn(L"[FrameGenPause] Shutdown off the game thread -- leaving %s as it is", kEnableCVar);
            return;
        }
        // No frames render past this point, so this only puts the values back.
        RestoreAll();
    }

    void Initialize()
    {
        Hooks::WorldBeginPlay::RegisterAnyWorldCallback(&OnWorldBeginPlay);
        Hooks::WorldEndPlay::RegisterBeforeCallback(&OnWorldEndPlay);
        Hooks::EngineShutdown::RegisterPluginCallback(&OnEngineShutdown);
    }

    void Tick(bool wantPaused, float deltaSeconds)
    {
        if (deltaSeconds > kMaxSteadyTickSeconds)
            s_steadySeconds = 0.0f;
        else
            s_steadySeconds += deltaSeconds;

        if (wantPaused != s_lastWantPaused)
        {
            s_lastWantPaused = wantPaused;
            s_requestDone = false;
        }

        if (!wantPaused)
        {
            // Closed before we got as far as disabling: DLSS-G is still on, so
            // putting retain back is safe at any time.
            if (!s_paused)
            {
                RestoreRetain();
                return;
            }
            // Turning DLSS-G back on allocates its buffers again. A travel that
            // starts while it is paused leaves it paused through the load, and
            // it comes back only once the next world is up and ticking steadily:
            // it costs nothing behind a loading screen, and switching mid-load
            // is exactly the case that faulted the GPU.
            if (IsSettled())
            {
                ModLoaderLogger::LogInfo(L"[FrameGenPause] Last overlay window closed");
                RestoreAll();
            }
            return;
        }

        if (s_paused || s_requestDone || !IsSettled())
            return;

        if (!s_retainSet)
        {
            int enable = 0;
            if (!ReadCurrentValue(kEnableCVar, enable))
            {
                ModLoaderLogger::LogWarn(L"[FrameGenPause] Could not read %s -- leaving DLSS-G alone", kEnableCVar);
                s_requestDone = true;
                return;
            }
            if (enable == 0)
            {
                s_requestDone = true; // already off, nothing to pause
                return;
            }

            int retain = 0;
            if (!ReadCurrentValue(kRetainCVar, retain))
            {
                // Without it, turning DLSS-G off frees buffers in-flight frames
                // may still read. Flicker is the lesser problem.
                ModLoaderLogger::LogWarn(L"[FrameGenPause] Could not read %s -- leaving DLSS-G alone", kRetainCVar);
                s_requestDone = true;
                return;
            }
            s_savedRetain = retain;
            s_retainSet = true;
            s_retainTicks = 0;
            if (retain != 1)
                WriteValue(kRetainCVar, 1);
            return;
        }

        if (++s_retainTicks < kRetainLeadTicks)
            return;

        int enable = 0;
        if (!ReadCurrentValue(kEnableCVar, enable) || enable == 0)
        {
            RestoreRetain();
            s_requestDone = true;
            return;
        }
        s_savedEnable = enable;
        s_paused = true;
        WriteValue(kEnableCVar, 0);
        ModLoaderLogger::LogInfo(
            L"[FrameGenPause] Overlay window opened -- disabling DLSS-G "
            L"(%s was %d, %s was %d)", kEnableCVar, enable, kRetainCVar, s_savedRetain);
    }
}

#endif // MODLOADER_CLIENT_BUILD
