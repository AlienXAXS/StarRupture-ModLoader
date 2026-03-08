#include "pch.h"
#include "mass_spawner_activate.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "utils/mem_utils.h"
#include <vector>
#include <algorithm>

namespace Hooks::MassSpawnerActivate
{
    // AAbstractMassEnemySpawner::ActivateSpawner(AAbstractMassEnemySpawner* this, bool bDisableAggroLock)
    typedef void(__fastcall* ActivateSpawner_t)(void* thisPtr, bool bDisableAggroLock);

    static Hook                        g_hook;
    static ActivateSpawner_t           g_original = nullptr;
    static std::vector<BeforeCallback> g_beforeCallbacks;
    static std::vector<AfterCallback>  g_afterCallbacks;

    static void __fastcall Detour(void* thisPtr, bool bDisableAggroLock)
    {
        ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] ActivateSpawner called — spawner=%p, aggroLock=%d",
            thisPtr, (int)bDisableAggroLock);

        // Before pass — any callback returning true cancels the operation
        bool cancelled = false;
        if (!g_beforeCallbacks.empty())
        {
            ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] Running %zu Before callback(s)", g_beforeCallbacks.size());
            for (size_t i = 0; i < g_beforeCallbacks.size(); ++i)
            {
                if (!g_beforeCallbacks[i])
                    continue;
                try
                {
                    if (g_beforeCallbacks[i](thisPtr, bDisableAggroLock))
                    {
                        ModLoaderLogger::LogInfo(L"[MassSpawnerActivate] Before callback #%zu cancelled activation", i + 1);
                        cancelled = true;
                    }
                }
                catch (const std::exception& e)
                {
                    ModLoaderLogger::LogError(L"[MassSpawnerActivate] Exception in Before callback #%zu: %S", i + 1, e.what());
                }
                catch (...)
                {
                    ModLoaderLogger::LogError(L"[MassSpawnerActivate] Unknown exception in Before callback #%zu", i + 1);
                }
            }
        }

        if (cancelled)
        {
            ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] Activation cancelled — skipping original and After callbacks");
            return;
        }

        if (g_original)
            g_original(thisPtr, bDisableAggroLock);
        else
            ModLoaderLogger::LogError(L"[MassSpawnerActivate] Original function pointer is null!");

        // After pass — only fires if not cancelled
        if (!g_afterCallbacks.empty())
        {
            ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] Running %zu After callback(s)", g_afterCallbacks.size());
            for (size_t i = 0; i < g_afterCallbacks.size(); ++i)
            {
                if (!g_afterCallbacks[i])
                    continue;
                try
                {
                    g_afterCallbacks[i](thisPtr, bDisableAggroLock);
                }
                catch (const std::exception& e)
                {
                    ModLoaderLogger::LogError(L"[MassSpawnerActivate] Exception in After callback #%zu: %S", i + 1, e.what());
                }
                catch (...)
                {
                    ModLoaderLogger::LogError(L"[MassSpawnerActivate] Unknown exception in After callback #%zu", i + 1);
                }
            }
        }
    }

    bool Install()
    {
        ModLoaderLogger::LogInfo(L"[MassSpawnerActivate] Installing hook...");

        const char* pattern = ScanPatterns::AAbstractMassEnemySpawner_ActivateSpawner;
        if (!pattern || pattern[0] == '\0')
        {
            ModLoaderLogger::LogWarn(L"[MassSpawnerActivate] Pattern not set — hook not installed");
            return false;
        }

        // The pattern matches AMegaMachineMassEnemySpawner::EnableSpawning,
        // which ends with a JMP into the real AAbstractMassEnemySpawner::ActivateSpawner.
        // We scan for the wrapper, then decode the E9 JMP at the known offset to get
        // the real target address.
        ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] Scanning for EnableSpawning wrapper...");
        uintptr_t wrapperAddr = Scanner::FindPatternInMainModule(
            "AMegaMachineMassEnemySpawner::EnableSpawning", pattern);
        if (!wrapperAddr)
        {
            ModLoaderLogger::LogError(L"[MassSpawnerActivate] Pattern scan failed — hook not installed");
            return false;
        }

        HMODULE mainModule = GetModuleHandleW(nullptr);
        auto base = reinterpret_cast<uintptr_t>(mainModule);
        ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] EnableSpawning at 0x%llX (base+0x%llX)",
            static_cast<unsigned long long>(wrapperAddr),
            static_cast<unsigned long long>(wrapperAddr - base));

        // Decode the E9 JMP at wrapperAddr + offset to find ActivateSpawner
        uintptr_t jmpAddr = wrapperAddr + ScanPatterns::AAbstractMassEnemySpawner_ActivateSpawner_Offset;
        ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] Resolving JMP at 0x%llX (offset +0x%X)",
            static_cast<unsigned long long>(jmpAddr),
            ScanPatterns::AAbstractMassEnemySpawner_ActivateSpawner_Offset);

        uintptr_t addr = MemUtils::ResolveRelJmp(jmpAddr);
        if (!addr)
        {
            ModLoaderLogger::LogError(
                L"[MassSpawnerActivate] Expected E9 JMP at offset +0x%X — byte was 0x%02X, not 0xE9",
                ScanPatterns::AAbstractMassEnemySpawner_ActivateSpawner_Offset,
                static_cast<unsigned>(*reinterpret_cast<const uint8_t*>(jmpAddr)));
            return false;
        }

        ModLoaderLogger::LogInfo(L"[MassSpawnerActivate] ActivateSpawner resolved to 0x%llX (base+0x%llX)",
            static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(addr - base));

        bool ok = g_hook.Install(
            addr,
            reinterpret_cast<void*>(&Detour),
            reinterpret_cast<void**>(&g_original));

        if (ok)
            ModLoaderLogger::LogInfo(L"[MassSpawnerActivate] Hook installed successfully");
        else
            ModLoaderLogger::LogError(L"[MassSpawnerActivate] Hook installation failed");

        return ok;
    }

    void Remove()
    {
        ModLoaderLogger::LogInfo(L"[MassSpawnerActivate] Removing hook...");
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
            ModLoaderLogger::LogWarn(L"[MassSpawnerActivate] RegisterBeforeCallback: null callback provided");
            return;
        }
        if (!g_hook.installed)
        {
            ModLoaderLogger::LogInfo(L"[MassSpawnerActivate] First callback registered — installing hook now...");
            if (!Install())
            {
                ModLoaderLogger::LogError(L"[MassSpawnerActivate] Failed to install hook — callback not registered");
                return;
            }
        }
        g_beforeCallbacks.push_back(callback);
        ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] Before callback registered (%zu total)", g_beforeCallbacks.size());
    }

    void UnregisterBeforeCallback(BeforeCallback callback)
    {
        auto it = std::find(g_beforeCallbacks.begin(), g_beforeCallbacks.end(), callback);
        if (it != g_beforeCallbacks.end())
        {
            g_beforeCallbacks.erase(it);
            ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] Before callback unregistered (%zu remaining)", g_beforeCallbacks.size());
        }
    }

    void RegisterAfterCallback(AfterCallback callback)
    {
        if (!callback)
        {
            ModLoaderLogger::LogWarn(L"[MassSpawnerActivate] RegisterAfterCallback: null callback provided");
            return;
        }
        if (!g_hook.installed)
        {
            ModLoaderLogger::LogInfo(L"[MassSpawnerActivate] First callback registered — installing hook now...");
            if (!Install())
            {
                ModLoaderLogger::LogError(L"[MassSpawnerActivate] Failed to install hook — callback not registered");
                return;
            }
        }
        g_afterCallbacks.push_back(callback);
        ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] After callback registered (%zu total)", g_afterCallbacks.size());
    }

    void UnregisterAfterCallback(AfterCallback callback)
    {
        auto it = std::find(g_afterCallbacks.begin(), g_afterCallbacks.end(), callback);
        if (it != g_afterCallbacks.end())
        {
            g_afterCallbacks.erase(it);
            ModLoaderLogger::LogDebug(L"[MassSpawnerActivate] After callback unregistered (%zu remaining)", g_afterCallbacks.size());
        }
    }
}
