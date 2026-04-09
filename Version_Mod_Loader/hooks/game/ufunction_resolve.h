#pragma once

#include "CoreUObject_classes.hpp"
#include "logging/logger.h"
#include "Basic.hpp"

#include "CoreUObject_classes.hpp"
#include "CoreUObject_parameters.hpp"

// ---------------------------------------------------------------------------
// UFunction-based hook address resolution
//
// Replaces pattern scanning for any function that is a UFUNCTION -- which
// covers almost all game-logic hooks (subsystem callbacks, game mode events,
// actor lifecycle, etc.).
//
// How it works:
//   Every UFUNCTION in a packaged UE5 game has a UFunction object in GObjects.
//   That object carries ExecFunction -- the actual native C++ function pointer.
//   We find the class by short name, ask it for the UFunction, and read the
//   pointer.  No byte patterns, no IDA, no manual updates after game patches.
//
// Constraints:
//   - Only call after EngineInit has fired (GObjects must be populated).
//   - Does NOT work for engine-internal functions that are not UFunctions
//     (FEngineLoop::Init, UGameEngine::Init, UGameEngine::Tick, etc.).
//
// className : short class name WITHOUT the U/A prefix
//             e.g. "CrGameModeBase"  not "ACrGameModeBase"
//             e.g. "Actor"           not "AActor"
// funcName  : short function name
//             e.g. "PostLogin", "BeginPlay", "OnWorldBeginPlay"
// ---------------------------------------------------------------------------

namespace Hooks
{
    inline uintptr_t ResolveUFunctionNativeAddr(const char* className, const char* funcName)
    {
        SDK::UClass* cls = SDK::UObject::FindClassFast(className);
        if (!cls)
        {
            ModLoaderLogger::LogError(
                L"[Hooks] ResolveUFunction: class '%S' not found in GObjects", className);
            return 0;
        }

        ModLoaderLogger::LogTrace(
            L"[Hooks] ResolveUFunction: class '%S' found at %p (SuperStruct=%p, Children=%p)",
            className, cls, cls->SuperStruct, cls->Children);

        SDK::UFunction* fn = cls->GetFunction(className, funcName);
        if (!fn)
        {
            // Walk the same SuperStruct chain GetFunction uses and dump every
            // function name found so the caller can see what is actually available.
            ModLoaderLogger::LogWarn(
                L"[Hooks] Resolve UFunction: '%S::%S' not found, attempting alternative hook.", className, funcName);

            for (const SDK::UStruct* Clss = cls; Clss; Clss = Clss->SuperStruct)
            {
                std::string clsName = Clss->GetName();
                for (SDK::UField* Field = Clss->Children; Field; Field = Field->Next)
                {
                    if (Field->HasTypeFlag(SDK::EClassCastFlags::Function))
                    {
                        std::string fieldName = Field->GetName();
                        ModLoaderLogger::LogTrace(
                            L"[Hooks]   %S::%S", clsName.c_str(), fieldName.c_str());
                    }
                }
            }

            return 0;
        }

        auto addr = reinterpret_cast<uintptr_t>(fn->ExecFunction);
        if (!addr)
        {
            ModLoaderLogger::LogError(
                L"[Hooks] ResolveUFunction: '%S::%S' has null ExecFunction", className, funcName);
            return 0;
        }

        HMODULE mainMod = GetModuleHandleW(nullptr);
        auto base = reinterpret_cast<uintptr_t>(mainMod);
        ModLoaderLogger::LogDebug(
            L"[Hooks] Resolved %S::%S via GObjects: 0x%llX (base+0x%llX)",
            className, funcName,
            static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(addr - base));
        return addr;
    }
}
