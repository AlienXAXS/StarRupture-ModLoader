#include "sdk_globals.h"
#include "../plugins/plugin_interface.h"

#if defined(MODLOADER_CLIENT_BUILD)

#include "CoreUObject_classes.hpp"
#include "../hooks/game/scan_patterns.h"
#include "../logging/logger.h"
#include "../memory_scanner/pattern_preflight.h"
#include "../memory_scanner/scan_validation.h"
#include "../memory_scanner/version_map.h"
#include "../hooks/game/version_map.generated.h"
#include "Engine_classes.hpp"

#include <algorithm>
#include <cstring>

namespace SdkGlobals
{
    namespace
    {
        struct Global
        {
            const char* name;       // as the SDK's Offsets:: spells it
            const char* pattern;    // the preflight registry row that finds it
            int32_t     sdkOffset;  // Offsets::<name>
            uintptr_t   address = 0;
        };

        // Set by ResolveAndApply on the init thread before any hook is
        // installed, and by VerifyGObjects on the same thread; read-only
        // afterwards.
        bool                    g_gObjectsOverridden = false;
        bool                    g_gObjectsVerified = false;  // VerifyGObjects kept the resolved one
        bool                    g_appendStringApplied = false;
        std::vector<Resolution> g_resolutions;
        uintptr_t               g_gNames = 0;
        uintptr_t               g_appendString = 0;
        uintptr_t               g_gWorld = 0;   // the engine's GWorld variable; 0 = not found

        // The registry row for a pattern, so the request is the one preflight
        // verified -- the same transform, the same kind check.
        const ScanPatterns::PreflightEntry* FindRegistryEntry(const char* pattern)
        {
            for (const ScanPatterns::PreflightEntry& entry : ScanPatterns::PreflightRegistry)
            {
                if (entry.pattern == pattern)
                    return &entry;
            }
            return nullptr;
        }

        // Resolves one global through its registry row. False (and logged) on
        // any miss; preflight has already logged the reason for the row.
        bool Resolve(Global& global)
        {
            const ScanPatterns::PreflightEntry* entry = FindRegistryEntry(global.pattern);
            if (!entry)
            {
                ModLoaderLogger::LogError(L"[SdkGlobals] %S: no preflight registry row for its pattern", global.name);
                return false;
            }

            const ScanValidation::Result result = PatternPreflight::Resolve(*entry);
            if (!result.Succeeded())
            {
                ModLoaderLogger::LogWarn(L"[SdkGlobals] %S: did not resolve (see \"%S\" in the preflight above)",
                    global.name, entry->name);
                return false;
            }

            global.address = result.address;
            return true;
        }

        // A global the SDK is handed: a difference from Offsets:: is the thing
        // worth noticing.
        void LogApplied(const Global& global, uintptr_t base)
        {
            const unsigned long long rva = global.address - base;
            ModLoaderLogger::LogInfo(L"[SdkGlobals] %-12S base+0x%llX  SDK Offsets::%S base+0x%X  %s",
                global.name, rva, global.name, global.sdkOffset,
                rva == static_cast<unsigned long long>(global.sdkOffset) ? L"(same)" : L"(DIFFERENT)");
        }

        // A global that is only logged. The SDK's Offsets:: value for these is
        // not the engine global at all (GNames is TextFormatter, GWorld a field
        // of a static singleton that tracks the world), so no comparison.
        void LogOnly(const Global& global, uintptr_t base)
        {
            ModLoaderLogger::LogInfo(L"[SdkGlobals] %-12S base+0x%llX  (log only; the SDK does not take it)",
                global.name, static_cast<unsigned long long>(global.address - base));
        }

        struct ObjectArrayProbe
        {
            int32_t num;
            int32_t sampled;    // non-null objects among the first kProbeObjects
            int32_t badIndex;   // first index whose object fails a check, -1 if none
        };

        constexpr int32_t kProbeObjects = 256;

        // Structural checks on an object array, under SEH so a wrong address
        // reads as "no" instead of a crash. Engine-registered objects fill the
        // first slots of GUObjectArray before engine init returns and are never
        // freed, and each one records its own slot in UObject::Index, so a
        // wrong-but-plausible address fails here almost at once.
        bool ProbeObjectArray(const SDK::TUObjectArray* array, ObjectArrayProbe* out)
        {
            out->num = 0;
            out->sampled = 0;
            out->badIndex = -1;

            __try
            {
                if (!array || !array->Objects)
                    return false;

                const int32_t num = array->NumElements;
                out->num = num;
                // Enough chunks for the objects, no more than the maximum: an
                // array pre-allocated by gc.PreAllocateUObjectArray has all of
                // them from the start.
                if (num <= 0 || num > array->MaxElements || array->NumChunks > array->MaxChunks
                    || array->NumChunks < (num + SDK::TUObjectArray::ElementsPerChunk - 1) / SDK::TUObjectArray::ElementsPerChunk)
                    return false;

                const int32_t count = (std::min)(num, kProbeObjects);
                for (int32_t i = 0; i < count; ++i)
                {
                    const SDK::UObject* object = array->GetByIndex(i);
                    if (!object)
                        continue;

                    if (object->Index != i || !object->Class || !object->Class->Class)
                    {
                        out->badIndex = i;
                        return false;
                    }
                    ++out->sampled;
                }
                return out->sampled >= count / 2;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool CheckObjectArray(const wchar_t* which, uintptr_t address)
        {
            ObjectArrayProbe probe{};
            const bool ok = ProbeObjectArray(reinterpret_cast<const SDK::TUObjectArray*>(address), &probe);
            if (ok)
            {
                ModLoaderLogger::LogInfo(L"[SdkGlobals] GObjects (%s) looks valid: %d objects, %d of the first %d checked",
                    which, probe.num, probe.sampled, (std::min)(probe.num, kProbeObjects));
            }
            else
            {
                ModLoaderLogger::LogError(L"[SdkGlobals] GObjects (%s) at base+0x%llX does not look like the object array (count %d, first bad slot %d)",
                    which, static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
                    probe.num, probe.badIndex);
            }
            return ok;
        }
    }

    void ResolveAndApply()
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

        Global gObjects     { "GObjects",     ScanPatterns::GObjects_PreAllocate,  SDK::Offsets::GObjects };
        Global gNames       { "GNames",       ScanPatterns::GNames_NamePoolData,   SDK::Offsets::GNames };
        Global appendString { "AppendString", ScanPatterns::FName_AppendString,    SDK::Offsets::AppendString };
        Global gWorld       { "GWorld",       ScanPatterns::GWorld_EngineLoopTick, SDK::Offsets::GWorld };

        // Each override stands on its own: a miss on one row says nothing
        // about another row's bytes. Any miss keeps the SDK on Offsets:: for
        // that global only, exactly as before.
        if (Resolve(gObjects))
        {
            LogApplied(gObjects, base);
            SDK::UObject::GObjects.InitManually(reinterpret_cast<void*>(gObjects.address));
            g_gObjectsOverridden = true;
        }
        if (Resolve(appendString))
        {
            LogApplied(appendString, base);
            SDK::FName::InitManually(reinterpret_cast<void*>(appendString.address));
            g_appendStringApplied = true;
            g_appendString = appendString.address;
        }

        // GNames and GWorld stop at the log line. The SDK never reads GNames
        // (FName goes through AppendString), and UWorld::GetWorld() reads
        // Offsets::GWorld through an `if constexpr`, so there is nothing to
        // hand either of them to.
        if (Resolve(gNames))
        {
            LogOnly(gNames, base);
            g_gNames = gNames.address;
        }
        if (Resolve(gWorld))
        {
            LogOnly(gWorld, base);
            g_gWorld = gWorld.address;
        }

        for (const Global* global : { &gObjects, &gNames, &appendString, &gWorld })
        {
            g_resolutions.push_back(Resolution{ global->name, global->address != 0,
                global->address ? global->address - base : 0, global->sdkOffset, false });
        }

        ModLoaderLogger::LogInfo(L"[SdkGlobals] SDK uses the %s GObjects and the %s FName::AppendString",
            g_gObjectsOverridden ? L"resolved" : L"built-in",
            appendString.address ? L"resolved" : L"built-in");
    }

    bool VerifyGObjects()
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t builtIn = base + SDK::Offsets::GObjects;

        if (!g_gObjectsOverridden)
            return CheckObjectArray(L"built-in", builtIn);
        // Kept once, kept for good: plugins may already hold it.
        if (g_gObjectsVerified)
            return true;

        const uintptr_t resolved = reinterpret_cast<uintptr_t>(SDK::UObject::GObjects.GetTypedPtr());
        if (CheckObjectArray(L"resolved", resolved))
        {
            g_gObjectsVerified = true;
            return true;
        }

        // A unique match that found the wrong .data address. Go back to the
        // SDK's own value, the policy for every other kind of miss.
        SDK::UObject::GObjects.InitManually(reinterpret_cast<void*>(builtIn));
        g_gObjectsOverridden = false;
        for (Resolution& entry : g_resolutions)
        {
            if (std::strcmp(entry.name, "GObjects") == 0)
                entry.rejected = true;
        }
        ModLoaderLogger::LogWarn(L"[SdkGlobals] Falling back to the built-in Offsets::GObjects");
        return CheckObjectArray(L"built-in", builtIn);
    }

    const std::vector<Resolution>& Resolutions()
    {
        return g_resolutions;
    }

    bool SdkOnResolvedGlobals()
    {
        return g_gObjectsOverridden && g_appendStringApplied;
    }

    const PluginSdkGlobals* ForPlugins()
    {
        // Settled on the first call, which comes from a PluginInit: after the
        // layout check has verified GObjects, so never changes afterwards.
        static const PluginSdkGlobals* const globals = []() -> const PluginSdkGlobals*
        {
            static PluginSdkGlobals value{};
            const uintptr_t gObjects = g_gObjectsVerified
                ? reinterpret_cast<uintptr_t>(SDK::UObject::GObjects.GetTypedPtr()) : 0;
            const std::string withheld = VersionMap::SdkGlobalsWithheld(VersionMap::RunningExe(),
                gObjects, g_appendString, g_gWorld);
            if (!withheld.empty())
            {
                ModLoaderLogger::LogWarn(L"[SdkGlobals] Not handed to plugins: %S (a GObjects the layout check did not verify counts as not found)",
                    withheld.c_str());
                return nullptr;
            }
            value = { static_cast<uint32_t>(sizeof(PluginSdkGlobals)), gObjects, g_gNames, g_appendString, g_gWorld };
            const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            const auto rva = [base](uintptr_t address) { return static_cast<unsigned long long>(address ? address - base : 0); };
            ModLoaderLogger::LogInfo(L"[SdkGlobals] Handed to plugins on CL-%u: GObjects base+0x%llX, GNames base+0x%llX, AppendString base+0x%llX, GWorld base+0x%llX (0 = not found)",
                VersionMap::RunningExe().cl, rva(gObjects), rva(g_gNames), rva(g_appendString), rva(g_gWorld));
            return &value;
        }();
        return globals;
    }

    // The engine's GWorld global: Offsets::GWorld on the SDK's build, else the
    // one ResolveAndApply found (null when none). GetWorld's fallback.
    static SDK::UWorld* GetGWorld()
    {
        if (VersionMap::RunningExe().cl == VersionMap::kBuiltCL)
            return SDK::UWorld::GetWorld();
        return g_gWorld ? *reinterpret_cast<SDK::UWorld**>(g_gWorld) : nullptr;
    }
}

#else

#include "Engine_classes.hpp"

namespace SdkGlobals
{
    void ResolveAndApply() {}
    bool VerifyGObjects() { return true; }
    bool SdkOnResolvedGlobals() { return false; }
    const PluginSdkGlobals* ForPlugins() { return nullptr; }
    static SDK::UWorld* GetGWorld() { return SDK::UWorld::GetWorld(); }
}

#endif

// ---- Members the loader reads, resolved per build (both builds) -----------

#include "Chimera_classes.hpp"
#include "Engine_classes.hpp"
#include "../hooks/game/version_map.generated.h"
#include "../logging/logger.h"
#include "../memory_scanner/version_map.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <type_traits>

namespace SdkGlobals
{
    namespace
    {
        // One member, under its key in the loader's version map
        // (VersionMap::kLoaderFieldKeys, in FieldId order).
        struct FieldSpec
        {
            const char* key;
            const char* className;
            const char* member;
            int32_t     compiled;   // offsetof in the SDK the loader is built against
            uint32_t    size;
        };

        // The mapped members (kLoaderFieldKeys) come first. The engine's
        // viewport members after them are reflected engine members GetWorld
        // reads; a miss on one is not a layout failure, GetWorld just keeps
        // to GWorld, so they are not in kLoaderFieldKeys (which every map
        // build must cover).
        enum FieldId { kCraftComponent, kGameState, kLineBatchers, kNetDriver, kMappedFieldCount,
                       kGameViewport = kMappedFieldCount, kViewportWorld, kFieldCount };

        static_assert(std::is_same_v<decltype(SDK::ACrCrafter::CraftComponent), SDK::UCrCraftingComponent*>);
        static_assert(std::is_same_v<decltype(SDK::UWorld::GameState), SDK::AGameStateBase*>);
        static_assert(std::is_same_v<std::remove_extent_t<decltype(SDK::UWorld::LineBatchers)>, SDK::ULineBatchComponent*>);
        static_assert(std::is_same_v<decltype(SDK::UWorld::NetDriver), SDK::UNetDriver*>);
        static_assert(std::is_same_v<decltype(SDK::UEngine::GameViewport), SDK::UGameViewportClient*>);
        static_assert(std::is_same_v<decltype(SDK::UGameViewportClient::World), SDK::UWorld*>);

        static_assert(std::size(VersionMap::kLoaderFieldKeys) == kMappedFieldCount, "one key per mapped loader field");

        const FieldSpec kFieldSpecs[kFieldCount] = {
            { VersionMap::kLoaderFieldKeys[kCraftComponent], "ACrCrafter", "CraftComponent",
              offsetof(SDK::ACrCrafter, CraftComponent), sizeof(SDK::ACrCrafter::CraftComponent) },
            { VersionMap::kLoaderFieldKeys[kGameState], "UWorld", "GameState",
              offsetof(SDK::UWorld, GameState), sizeof(SDK::UWorld::GameState) },
            { VersionMap::kLoaderFieldKeys[kLineBatchers], "UWorld", "LineBatchers",
              offsetof(SDK::UWorld, LineBatchers), sizeof(SDK::UWorld::LineBatchers) },
            { VersionMap::kLoaderFieldKeys[kNetDriver], "UWorld", "NetDriver",
              offsetof(SDK::UWorld, NetDriver), sizeof(SDK::UWorld::NetDriver) },
            { "UEngine::GameViewport", "UEngine", "GameViewport",
              offsetof(SDK::UEngine, GameViewport), sizeof(SDK::UEngine::GameViewport) },
            { "UGameViewportClient::World", "UGameViewportClient", "World",
              offsetof(SDK::UGameViewportClient, World), sizeof(SDK::UGameViewportClient::World) },
        };

        // Written once by ResolveFields on the init thread, read from any.
        // -1: not resolved (yet, or on this build).
        std::atomic<int32_t> g_fieldOffsets[kFieldCount] = { -1, -1, -1, -1, -1, -1 };
        std::atomic<bool>    g_unresolvedLogged[kFieldCount] = {};

        // The pointer stored at the resolved offset of `id` in `object`, as
        // element `index` of the member.
        template <class T>
        T* PointerAt(const void* object, FieldId id, int index = 0)
        {
            const int32_t offset = g_fieldOffsets[id].load(std::memory_order_acquire);
            // After a layout safe-off ResolveFields never runs, so every
            // feature of the loader's that reads this member stays off for
            // the session. Said once, the first time one asks.
            if (offset < 0 && object && !g_unresolvedLogged[id].exchange(true))
                ModLoaderLogger::LogWarn(L"[Map] ModLoader: %S has no offset yet, and none for this session after a layout safe-off; what the loader reads through it gives nothing until it has one",
                    kFieldSpecs[id].key);
            if (!object || offset < 0 || index < 0 || static_cast<uint32_t>(index + 1) * sizeof(T*) > kFieldSpecs[id].size)
                return nullptr;
            return reinterpret_cast<T* const*>(static_cast<const uint8_t*>(object) + offset)[index];
        }
    }

    std::vector<std::string> ResolveFields()
    {
        std::vector<std::string> unresolved;
#if defined(MODLOADER_CLIENT_BUILD)
        const VersionMap::Exe running = VersionMap::RunningExe();
#endif
        for (int id = 0; id < kFieldCount; ++id)
        {
            const FieldSpec& spec = kFieldSpecs[id];
#if defined(MODLOADER_CLIENT_BUILD)
            VersionMap::FieldValue value;
            std::string error;
            if (VersionMap::SelectField("ModLoader", VersionMap::LoaderFieldRequest(spec.key, spec.size, spec.compiled), running, value, error))
            {
                g_fieldOffsets[id].store(value.offset, std::memory_order_release);
                continue;
            }
            if (running.cl != VersionMap::kBuiltCL)
            {
                // SelectField has logged it; GetWorld keeps to GWorld.
                if (id < kMappedFieldCount)
                    unresolved.push_back(std::string(spec.key) + ": " + error);
                continue;
            }
            ModLoaderLogger::LogWarn(L"[Map] ModLoader: %S: the SDK's compiled offset 0x%X, on the build it was made for",
                spec.key, spec.compiled);
#endif
            g_fieldOffsets[id].store(spec.compiled, std::memory_order_release);
        }
        return unresolved;
    }

    bool IsPerBuildField(const char* className, const char* member)
    {
        return std::any_of(std::begin(kFieldSpecs), std::end(kFieldSpecs), [&](const FieldSpec& spec)
            { return strcmp(spec.className, className) == 0 && strcmp(spec.member, member) == 0; });
    }

    SDK::AGameStateBase* WorldGameState(const SDK::UWorld* world)
    {
        return PointerAt<SDK::AGameStateBase>(world, kGameState);
    }

    SDK::UNetDriver* WorldNetDriver(const SDK::UWorld* world)
    {
        return PointerAt<SDK::UNetDriver>(world, kNetDriver);
    }

    SDK::ULineBatchComponent* WorldLineBatcher(const SDK::UWorld* world, int index)
    {
        return PointerAt<SDK::ULineBatchComponent>(world, kLineBatchers, index);
    }

    SDK::UCrCraftingComponent* CrafterCraftComponent(const SDK::ACrCrafter* crafter)
    {
        return PointerAt<SDK::UCrCraftingComponent>(crafter, kCraftComponent);
    }

    SDK::UWorld* GetWorld()
    {
        // Both members resolved (or, on the SDK's build, the compiled ones)
        // and an engine with a viewport: that viewport's world, null included.
        // Anything short of that is the GWorld global, as before. The offsets
        // are tested first: they are set only once the layout check has
        // verified GObjects, which GetEngine walks.
        if (g_fieldOffsets[kGameViewport].load(std::memory_order_acquire) >= 0
            && g_fieldOffsets[kViewportWorld].load(std::memory_order_acquire) >= 0)
        {
            if (SDK::UEngine* engine = SDK::UEngine::GetEngine())
                if (SDK::UGameViewportClient* viewport = PointerAt<SDK::UGameViewportClient>(engine, kGameViewport))
                    return PointerAt<SDK::UWorld>(viewport, kViewportWorld);
        }
        return GetGWorld();
    }
}
