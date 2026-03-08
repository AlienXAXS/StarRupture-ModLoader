#pragma once

#include <windows.h>
#include <cstdint>

// Plugin interface version history - increment MAX when new hooks/features are added.
// Increment MIN only when an ABI-breaking change is unavoidable.
// Loader accepts any plugin whose interfaceVersion is in [MIN, MAX].
// Plugins compiled against older (but still supported) headers load without recompilation
// because all interface structs are append-only — new fields are always added at the end.
// v2: Added RegisterEngineShutdownCallback / UnregisterEngineShutdownCallback to IPluginHooks
// v3: Replaced std::vector return types in IPluginScanner with caller-buffer API to fix
//     cross-DLL heap corruption (EXCEPTION_ACCESS_VIOLATION on plugin load)
// v4: Added FindXrefsToAddress / FindXrefsToAddressInModule / FindXrefsToAddressInMainModule
//     to IPluginScanner for function-pointer cross-reference scanning
// v5: Added EngineAlloc / EngineFree / IsEngineAllocatorAvailable to IPluginHooks
//     for safe FString / engine-owned memory manipulation from plugins
// v6: Added RegisterAnyWorldBeginPlayCallback / UnregisterAnyWorldBeginPlayCallback to IPluginHooks
//     for receiving notifications when ANY world begins play (not just ChimeraMain)
// v7: Added RegisterSaveLoadedCallback / UnregisterSaveLoadedCallback to IPluginHooks
//     for receiving notifications when UCrMassSaveSubsystem::OnSaveLoaded fires (save fully loaded)
// v8: Added RegisterExperienceLoadCompleteCallback / UnregisterExperienceLoadCompleteCallback
//     to IPluginHooks for receiving notifications when UCrExperienceManagerComponent::OnExperienceLoadComplete fires
// v9: Added RegisterEngineTickCallback / UnregisterEngineTickCallback to IPluginHooks
//     for receiving per-frame game-thread tick notifications (UGameEngine::Tick)
// v10: Added RegisterActorBeginPlayCallback / UnregisterActorBeginPlayCallback
//      to IPluginHooks for receiving notifications when any AActor::BeginPlay fires
// v11: Added RegisterPlayerJoinedCallback / UnregisterPlayerJoinedCallback
//      to IPluginHooks for receiving notifications when ACrGameModeBase::PostLogin fires
//      (player controller fully connected and ready on server)
// v12: Added RegisterPlayerLeftCallback / UnregisterPlayerLeftCallback
//      to IPluginHooks for receiving notifications when ACrGameModeBase::Logout fires
//      (player controller about to be destroyed — still valid at callback time)
// v14: Replaced flat function-pointer fields (v1-v12) with typed sub-interface pointers.
//      IPluginHooks now contains only 7 sub-interface pointers — all functionality
//      accessed via hooks->Group->Method(...). MIN bumped to 14 (ABI break).
//      Added 10 named callback typedefs (PluginEngineInitCallback, etc.).
//      Sub-interfaces:
//        Spawner  — Before/After hooks for ActivateSpawner, DeactivateSpawner, DoSpawning
//                   (Before callbacks return bool; true cancels + suppresses After callbacks)
//        Hooks    — low-level hook install/remove/query  (hooks->Hooks->Install)
//        Memory   — patch/nop/read/alloc utilities       (hooks->Memory->Patch)
//        Engine   — init/shutdown/tick subscriptions     (hooks->Engine->RegisterOnInit)
//        World    — world-begin-play/save/experience     (hooks->World->RegisterOnWorldBeginPlay)
//        Players  — player joined/left subscriptions     (hooks->Players->RegisterOnPlayerJoined)
//        Actors   — actor begin-play subscriptions       (hooks->Actors->RegisterOnActorBeginPlay)
#define PLUGIN_INTERFACE_VERSION_MIN 14  // oldest plugin ABI still accepted by this loader
#define PLUGIN_INTERFACE_VERSION_MAX 14  // current interface version (this header)
#define PLUGIN_INTERFACE_VERSION PLUGIN_INTERFACE_VERSION_MAX  // alias used by plugins in PluginInfo

// Log levels
enum class PluginLogLevel
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4
};

// Config value types
enum class ConfigValueType
{
    String,
    Integer,
    Float,
    Boolean
};

// Config entry definition for auto-generation
struct ConfigEntry
{
    const char* section;     // INI section name (e.g., "General", "Advanced")
    const char* key;            // INI key name
    ConfigValueType type;     // Value type
    const char* defaultValue;   // Default value as string (converted based on type)
    const char* description;    // Optional description/comment
};

// Config schema - defines all config entries for a plugin
struct ConfigSchema
{
    const ConfigEntry* entries; // Array of config entries
    int entryCount;          // Number of entries in array
};

// Universal logger interface provided by mod loader
struct IPluginLogger
{
    // Log a message with the specified level
    void (*Log)(PluginLogLevel level, const char* pluginName, const char* message);

    // Convenience methods
    void (*Trace)(const char* pluginName, const char* format, ...);
    void (*Debug)(const char* pluginName, const char* format, ...);
    void (*Info)(const char* pluginName, const char* format, ...);
    void (*Warn)(const char* pluginName, const char* format, ...);
    void (*Error)(const char* pluginName, const char* format, ...);
};

// Config manager interface provided by mod loader
struct IPluginConfig
{
    // Read string value from plugin's config file
    bool (*ReadString)(const char* pluginName, const char* section, const char* key, char* outValue, int maxLen, const char* defaultValue);

    // Write string value to plugin's config file
    bool (*WriteString)(const char* pluginName, const char* section, const char* key, const char* value);

    // Read integer value from plugin's config file
    int (*ReadInt)(const char* pluginName, const char* section, const char* key, int defaultValue);

    // Write integer value to plugin's config file
    bool (*WriteInt)(const char* pluginName, const char* section, const char* key, int value);

    // Read float value from plugin's config file
    float (*ReadFloat)(const char* pluginName, const char* section, const char* key, float defaultValue);

    // Write float value to plugin's config file
    bool (*WriteFloat)(const char* pluginName, const char* section, const char* key, float value);

    // Read boolean value from plugin's config file
    bool (*ReadBool)(const char* pluginName, const char* section, const char* key, bool defaultValue);

    // Write boolean value to plugin's config file
    bool (*WriteBool)(const char* pluginName, const char* section, const char* key, bool value);

    // Initialize plugin config from schema
    // Creates config file with defaults if it doesn't exist
    // Returns true if config was loaded/created successfully
    bool (*InitializeFromSchema)(const char* pluginName, const ConfigSchema* schema);

    // Validate and repair config file based on schema
    // Adds missing entries with defaults, preserves existing values
    void (*ValidateConfig)(const char* pluginName, const ConfigSchema* schema);
};

// A single cross-reference result returned by the xref scanner.
// isRelative: true  = relative near CALL (E8) or JMP (E9) instruction
//             false = absolute 8-byte function pointer (vtable, data, etc.)
struct PluginXRef
{
    uintptr_t address;    // Address of the referencing instruction / pointer slot
    bool      isRelative; // true = relative CALL/JMP  |  false = absolute pointer
};

// Pattern scanner interface provided by mod loader
// Use IDA-style patterns: "48 89 5C 24 ?? 57 48 83 EC 20" where ?? is wildcard
struct IPluginScanner
{
    // Find the first occurrence of a pattern in the main executable module
    // Returns the absolute address of the match, or 0 if not found
    uintptr_t (*FindPatternInMainModule)(const char* pattern);

    // Find the first occurrence of a pattern in a specific module
    // Returns the absolute address of the match, or 0 if not found
    uintptr_t (*FindPatternInModule)(HMODULE module, const char* pattern);

    // Find all occurrences of a pattern in the main executable module.
    // Writes up to maxResults addresses into outAddresses and returns the total match count.
    // Pass outAddresses=nullptr / maxResults=0 to query the count without writing.
    // Safe across DLL boundaries - no heap ownership transfer.
    int (*FindAllPatternsInMainModule)(const char* pattern, uintptr_t* outAddresses, int maxResults);

    // Find all occurrences of a pattern in a specific module.
    // Writes up to maxResults addresses into outAddresses and returns the total match count.
    // Pass outAddresses=nullptr / maxResults=0 to query the count without writing.
    // Safe across DLL boundaries - no heap ownership transfer.
    int (*FindAllPatternsInModule)(HMODULE module, const char* pattern, uintptr_t* outAddresses, int maxResults);

    // Find a unique pattern from a list of candidates
    // Returns the address if exactly one pattern matches exactly once
    // Returns 0 if no patterns match uniquely
    // outPatternIndex (if provided) is set to the index of the matching pattern
    uintptr_t (*FindUniquePattern)(const char** patterns, int patternCount, int* outPatternIndex);

    // XRef scanning - find all locations that reference a given address.
    //
    // Two reference types are detected:
    //   - Absolute 8-byte pointer: any 8-byte-aligned slot whose value equals
    //     targetAddress exactly (vtables, function-pointer arrays, global data).
    //   - Relative near CALL (E8) / JMP (E9): instruction whose computed target
    //     equals targetAddress.
    //
    // All three variants use the caller-buffer pattern (safe across DLL boundaries):
    //   pass outXRefs=nullptr / maxResults=0 to query the count only.
    //
    // FindXrefsToAddress          - scan an arbitrary memory range [start, start+size)
    // FindXrefsToAddressInModule  - scan an entire PE module
    // FindXrefsToAddressInMainModule - convenience: scans the main .exe module
    int (*FindXrefsToAddress)(uintptr_t targetAddress, uintptr_t start, size_t size,
                              PluginXRef* outXRefs, int maxResults);
    int (*FindXrefsToAddressInModule)(uintptr_t targetAddress, HMODULE module,
                                      PluginXRef* outXRefs, int maxResults);
    int (*FindXrefsToAddressInMainModule)(uintptr_t targetAddress,
                                          PluginXRef* outXRefs, int maxResults);
};

// Opaque hook handle - plugins don't need to know the internals
typedef void* HookHandle;

// Forward declare SDK::UWorld for callback
namespace SDK { class UWorld; }

// ============================================================
// Event callback typedefs (v14)
// Named equivalents of the anonymous inline types used in the flat IPluginHooks
// fields. Used by the typed sub-interface structs (IPluginEngineEvents, etc.)
// and may also be used by plugin authors for cleaner callback declarations.
// ============================================================

typedef void (*PluginEngineInitCallback)();
typedef void (*PluginEngineShutdownCallback)();
typedef void (*PluginEngineTickCallback)(float deltaSeconds);
typedef void (*PluginWorldBeginPlayCallback)(SDK::UWorld* world);
typedef void (*PluginAnyWorldBeginPlayCallback)(SDK::UWorld* world, const char* worldName);
typedef void (*PluginSaveLoadedCallback)();
typedef void (*PluginExperienceLoadCompleteCallback)();
typedef void (*PluginActorBeginPlayCallback)(void* actor);
typedef void (*PluginPlayerJoinedCallback)(void* playerController);
typedef void (*PluginPlayerLeftCallback)(void* exitingController);

// ============================================================
// Spawner hook callback typedefs (v14)
// Used with IPluginSpawnerHooks accessible via hooks->Spawner
// ============================================================

// AAbstractMassEnemySpawner::ActivateSpawner — Before: return true to cancel
typedef bool (*PluginBeforeActivateSpawnerCallback)(void* spawner, bool bDisableAggroLock);
// AAbstractMassEnemySpawner::ActivateSpawner — After: fires only if not cancelled
typedef void (*PluginAfterActivateSpawnerCallback)(void* spawner, bool bDisableAggroLock);

// AAbstractMassEnemySpawner::DeactivateSpawner — Before: return true to cancel
typedef bool (*PluginBeforeDeactivateSpawnerCallback)(void* spawner, bool bPermanently);
// AAbstractMassEnemySpawner::DeactivateSpawner — After: fires only if not cancelled
typedef void (*PluginAfterDeactivateSpawnerCallback)(void* spawner, bool bPermanently);

// AMassSpawner::DoSpawning — Before: return true to cancel the batch spawn
typedef bool (*PluginBeforeDoSpawningCallback)(void* spawner);
// AMassSpawner::DoSpawning — After: fires only if not cancelled
typedef void (*PluginAfterDoSpawningCallback)(void* spawner);

// ============================================================
// IPluginHookUtils — low-level hook install/remove/query (v14)
// Access via: hooks->Hooks->Install(...)
// ============================================================
struct IPluginHookUtils
{
    HookHandle (*Install)(uintptr_t targetAddress, void* detourFunction, void** originalFunction);
    void       (*Remove)(HookHandle handle);
    bool       (*IsInstalled)(HookHandle handle);
};

// ============================================================
// IPluginMemoryUtils — memory patch/nop/read/alloc utilities (v14)
// Access via: hooks->Memory->Patch(...)
// ============================================================
struct IPluginMemoryUtils
{
    bool  (*Patch)(uintptr_t address, const uint8_t* data, size_t size);
    bool  (*Nop)(uintptr_t address, size_t size);
    bool  (*Read)(uintptr_t address, void* buffer, size_t size);
    void* (*Alloc)(size_t count, uint32_t alignment);
    void  (*Free)(void* ptr);
    bool  (*IsAllocatorAvailable)();
};

// ============================================================
// IPluginEngineEvents — engine lifecycle event subscriptions (v14)
// Access via: hooks->Engine->RegisterOnInit(...)
// ============================================================
struct IPluginEngineEvents
{
    void (*RegisterOnInit)(PluginEngineInitCallback);
    void (*UnregisterOnInit)(PluginEngineInitCallback);
    void (*RegisterOnShutdown)(PluginEngineShutdownCallback);
    void (*UnregisterOnShutdown)(PluginEngineShutdownCallback);
    void (*RegisterOnTick)(PluginEngineTickCallback);
    void (*UnregisterOnTick)(PluginEngineTickCallback);
};

// ============================================================
// IPluginWorldEvents — world / level event subscriptions (v14)
// Access via: hooks->World->RegisterOnWorldBeginPlay(...)
// ============================================================
struct IPluginWorldEvents
{
    void (*RegisterOnWorldBeginPlay)(PluginWorldBeginPlayCallback);
    void (*UnregisterOnWorldBeginPlay)(PluginWorldBeginPlayCallback);
    void (*RegisterOnAnyWorldBeginPlay)(PluginAnyWorldBeginPlayCallback);
    void (*UnregisterOnAnyWorldBeginPlay)(PluginAnyWorldBeginPlayCallback);
    void (*RegisterOnSaveLoaded)(PluginSaveLoadedCallback);
    void (*UnregisterOnSaveLoaded)(PluginSaveLoadedCallback);
    void (*RegisterOnExperienceLoadComplete)(PluginExperienceLoadCompleteCallback);
    void (*UnregisterOnExperienceLoadComplete)(PluginExperienceLoadCompleteCallback);
};

// ============================================================
// IPluginPlayerEvents — player join/leave event subscriptions (v14)
// Access via: hooks->Players->RegisterOnPlayerJoined(...)
// ============================================================
struct IPluginPlayerEvents
{
    void (*RegisterOnPlayerJoined)(PluginPlayerJoinedCallback);
    void (*UnregisterOnPlayerJoined)(PluginPlayerJoinedCallback);
    void (*RegisterOnPlayerLeft)(PluginPlayerLeftCallback);
    void (*UnregisterOnPlayerLeft)(PluginPlayerLeftCallback);
};

// ============================================================
// IPluginActorEvents — actor lifecycle event subscriptions (v14)
// Access via: hooks->Actors->RegisterOnActorBeginPlay(...)
// ============================================================
struct IPluginActorEvents
{
    void (*RegisterOnActorBeginPlay)(PluginActorBeginPlayCallback);
    void (*UnregisterOnActorBeginPlay)(PluginActorBeginPlayCallback);
};

// ============================================================
// IPluginSpawnerHooks — enemy spawner Before/After hook group (v14)
// Access via: hooks->Spawner->RegisterOnBeforeActivate(...)
// Before callbacks run in registration order; any returning true cancels the
// operation and suppresses the original call and all After callbacks.
// ============================================================
struct IPluginSpawnerHooks
{
    // -----------------------------------------------------------------------
    // AAbstractMassEnemySpawner::ActivateSpawner(bool bDisableAggroLock) (v14)
    // Before: fires before the spawner activates. Return true to cancel.
    // After:  fires after the spawner has activated (only if not cancelled).
    // Spawner pointer is AAbstractMassEnemySpawner* passed as void*.
    // -----------------------------------------------------------------------
    void (*RegisterOnBeforeActivate)(PluginBeforeActivateSpawnerCallback callback);
    void (*UnregisterOnBeforeActivate)(PluginBeforeActivateSpawnerCallback callback);
    void (*RegisterOnAfterActivate)(PluginAfterActivateSpawnerCallback callback);
    void (*UnregisterOnAfterActivate)(PluginAfterActivateSpawnerCallback callback);

    // -----------------------------------------------------------------------
    // AAbstractMassEnemySpawner::DeactivateSpawner(bool bPermanently) (v14)
    // Before: fires before the spawner deactivates. Return true to cancel.
    // After:  fires after the spawner has deactivated (only if not cancelled).
    // Spawner pointer is AAbstractMassEnemySpawner* passed as void*.
    // -----------------------------------------------------------------------
    void (*RegisterOnBeforeDeactivate)(PluginBeforeDeactivateSpawnerCallback callback);
    void (*UnregisterOnBeforeDeactivate)(PluginBeforeDeactivateSpawnerCallback callback);
    void (*RegisterOnAfterDeactivate)(PluginAfterDeactivateSpawnerCallback callback);
    void (*UnregisterOnAfterDeactivate)(PluginAfterDeactivateSpawnerCallback callback);

    // -----------------------------------------------------------------------
    // AMassSpawner::DoSpawning() (v14)
    // Before: fires before the Mass Entity batch is spawned. Return true to cancel.
    // After:  fires after the batch has been spawned (only if not cancelled).
    // Spawner pointer is AMassSpawner* passed as void*.
    // -----------------------------------------------------------------------
    void (*RegisterOnBeforeDoSpawning)(PluginBeforeDoSpawningCallback callback);
    void (*UnregisterOnBeforeDoSpawning)(PluginBeforeDoSpawningCallback callback);
    void (*RegisterOnAfterDoSpawning)(PluginAfterDoSpawningCallback callback);
    void (*UnregisterOnAfterDoSpawning)(PluginAfterDoSpawningCallback callback);
};

// ============================================================
// IPluginHooks — top-level hook interface provided by the mod loader (v14)
// Contains only typed sub-interface pointers. Access functionality via the
// named group, e.g.:
//   hooks->Engine->RegisterOnInit(&MyInit);
//   hooks->Players->RegisterOnPlayerJoined(&MyJoinCb);
//   hooks->Memory->Patch(addr, bytes, len);
//   hooks->Hooks->Install(addr, detour, &original);
//   hooks->Spawner->RegisterOnBeforeActivate(&MyBeforeCb);
// ============================================================
struct IPluginHooks
{
    IPluginSpawnerHooks* Spawner;   // v14 — enemy spawner Before/After hooks
    IPluginHookUtils*    Hooks;     // v14 — low-level hook install/remove/query
    IPluginMemoryUtils*  Memory;    // v14 — memory patch/nop/read/alloc
    IPluginEngineEvents* Engine;    // v14 — engine init/shutdown/tick subscriptions
    IPluginWorldEvents*  World;     // v14 — world begin-play / save / experience subscriptions
    IPluginPlayerEvents* Players;   // v14 — player joined/left subscriptions
    IPluginActorEvents*  Actors;    // v14 — actor begin-play subscriptions
};

// Plugin metadata structure
struct PluginInfo
{
    const char* name;
    const char* version;
    const char* author;
    const char* description;
    int interfaceVersion;
};

// Plugin interface - all plugins must implement these functions
// These should be exported with extern "C" __declspec(dllexport)
typedef PluginInfo* (*GetPluginInfoFunc)();
typedef bool (*PluginInitFunc)(IPluginLogger* logger, IPluginConfig* config, IPluginScanner* scanner, IPluginHooks* hooks);
typedef void (*PluginShutdownFunc)();

// Function names that plugins must export
#define PLUGIN_GET_INFO_FUNC_NAME "GetPluginInfo"
#define PLUGIN_INIT_FUNC_NAME "PluginInit"
#define PLUGIN_SHUTDOWN_FUNC_NAME "PluginShutdown"
