#include "pch.h"
#include "mass_do_spawning.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include <vector>
#include <algorithm>

namespace Hooks::MassDoSpawning
{
    // AMassSpawner::DoSpawning(AMassSpawner* this)
    typedef void(__fastcall* DoSpawning_t)(void* thisPtr);

    static Hook                        g_hook;
    static DoSpawning_t                g_original = nullptr;
    static std::vector<BeforeCallback> g_beforeCallbacks;
    static std::vector<AfterCallback>  g_afterCallbacks;

    static void __fastcall Detour(void* thisPtr)
    {
        ModLoaderLogger::LogDebug(L"[MassDoSpawning] DoSpawning called — spawner=%p", thisPtr);

        // Before pass — any callback returning true cancels the operation
        bool cancelled = false;
        if (!g_beforeCallbacks.empty())
        {
            ModLoaderLogger::LogDebug(L"[MassDoSpawning] Running %zu Before callback(s)", g_beforeCallbacks.size());
            for (size_t i = 0; i < g_beforeCallbacks.size(); ++i)
            {
                if (!g_beforeCallbacks[i])
                    continue;
                try
                {
                    if (g_beforeCallbacks[i](thisPtr))
                    {
                        ModLoaderLogger::LogInfo(L"[MassDoSpawning] Before callback #%zu cancelled spawn batch", i + 1);
                        cancelled = true;
                    }
                }
                catch (const std::exception& e)
                {
                    ModLoaderLogger::LogError(L"[MassDoSpawning] Exception in Before callback #%zu: %S", i + 1, e.what());
                }
                catch (...)
                {
                    ModLoaderLogger::LogError(L"[MassDoSpawning] Unknown exception in Before callback #%zu", i + 1);
                }
            }
        }

        if (cancelled)
        {
            ModLoaderLogger::LogDebug(L"[MassDoSpawning] Spawn batch cancelled — skipping original and After callbacks");
            return;
        }

        if (g_original)
            g_original(thisPtr);
        else
            ModLoaderLogger::LogError(L"[MassDoSpawning] Original function pointer is null!");

        // After pass — only fires if not cancelled
        if (!g_afterCallbacks.empty())
        {
            ModLoaderLogger::LogDebug(L"[MassDoSpawning] Running %zu After callback(s)", g_afterCallbacks.size());
            for (size_t i = 0; i < g_afterCallbacks.size(); ++i)
            {
                if (!g_afterCallbacks[i])
                    continue;
                try
                {
                    g_afterCallbacks[i](thisPtr);
                }
                catch (const std::exception& e)
                {
                    ModLoaderLogger::LogError(L"[MassDoSpawning] Exception in After callback #%zu: %S", i + 1, e.what());
                }
                catch (...)
                {
                    ModLoaderLogger::LogError(L"[MassDoSpawning] Unknown exception in After callback #%zu", i + 1);
                }
            }
        }
    }

    bool Install()
    {
        ModLoaderLogger::LogInfo(L"[MassDoSpawning] Installing hook...");

        const char* pattern = ScanPatterns::AMassSpawner_DoSpawning;
        if (!pattern || pattern[0] == '\0')
        {
            ModLoaderLogger::LogWarn(L"[MassDoSpawning] Pattern not set (TODO: fill via IDA/x64dbg) — hook not installed");
            return false;
        }

        ModLoaderLogger::LogDebug(L"[MassDoSpawning]   Pattern: %S", pattern);
        uintptr_t addr = Scanner::FindPatternInMainModule("AMassSpawner::DoSpawning", pattern);
        if (!addr)
        {
            ModLoaderLogger::LogError(L"[MassDoSpawning] Pattern scan failed — hook not installed");
            return false;
        }

        HMODULE mainModule = GetModuleHandleW(nullptr);
        auto base = reinterpret_cast<uintptr_t>(mainModule);
        ModLoaderLogger::LogInfo(L"[MassDoSpawning] Found at 0x%llX (base+0x%llX)",
            static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(addr - base));

        bool ok = g_hook.Install(
            addr,
            reinterpret_cast<void*>(&Detour),
            reinterpret_cast<void**>(&g_original));

        if (ok)
            ModLoaderLogger::LogInfo(L"[MassDoSpawning] Hook installed successfully");
        else
            ModLoaderLogger::LogError(L"[MassDoSpawning] Hook installation failed");

        return ok;
    }

    void Remove()
    {
        ModLoaderLogger::LogInfo(L"[MassDoSpawning] Removing hook...");
        g_hook.Remove();
        g_beforeCallbacks.clear();
        g_afterCallbacks.clear();
    }

    bool IsInstalled()
    {
        return g_hook.installed;
    }

    void RegisterBeforeCallback(BeforeCallback callback)
    {
        if (!callback)
        {
            ModLoaderLogger::LogWarn(L"[MassDoSpawning] RegisterBeforeCallback: null callback provided");
            return;
        }
        if (!g_hook.installed)
        {
            ModLoaderLogger::LogInfo(L"[MassDoSpawning] First callback registered — installing hook now...");
            if (!Install())
            {
                ModLoaderLogger::LogError(L"[MassDoSpawning] Failed to install hook — callback not registered");
                return;
            }
        }
        g_beforeCallbacks.push_back(callback);
        ModLoaderLogger::LogDebug(L"[MassDoSpawning] Before callback registered (%zu total)", g_beforeCallbacks.size());
    }

    void UnregisterBeforeCallback(BeforeCallback callback)
    {
        auto it = std::find(g_beforeCallbacks.begin(), g_beforeCallbacks.end(), callback);
        if (it != g_beforeCallbacks.end())
        {
            g_beforeCallbacks.erase(it);
            ModLoaderLogger::LogDebug(L"[MassDoSpawning] Before callback unregistered (%zu remaining)", g_beforeCallbacks.size());
        }
    }

    void RegisterAfterCallback(AfterCallback callback)
    {
        if (!callback)
        {
            ModLoaderLogger::LogWarn(L"[MassDoSpawning] RegisterAfterCallback: null callback provided");
            return;
        }
        if (!g_hook.installed)
        {
            ModLoaderLogger::LogInfo(L"[MassDoSpawning] First callback registered — installing hook now...");
            if (!Install())
            {
                ModLoaderLogger::LogError(L"[MassDoSpawning] Failed to install hook — callback not registered");
                return;
            }
        }
        g_afterCallbacks.push_back(callback);
        ModLoaderLogger::LogDebug(L"[MassDoSpawning] After callback registered (%zu total)", g_afterCallbacks.size());
    }

    void UnregisterAfterCallback(AfterCallback callback)
    {
        auto it = std::find(g_afterCallbacks.begin(), g_afterCallbacks.end(), callback);
        if (it != g_afterCallbacks.end())
        {
            g_afterCallbacks.erase(it);
            ModLoaderLogger::LogDebug(L"[MassDoSpawning] After callback unregistered (%zu remaining)", g_afterCallbacks.size());
        }
    }
}
