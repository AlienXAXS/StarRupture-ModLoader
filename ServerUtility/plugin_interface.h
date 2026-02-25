#pragma once

#include <windows.h>
#include <cstdint>

// Plugin interface version - increment when breaking changes are made
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
#define PLUGIN_INTERFACE_VERSION 8

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

// Hook interface provided by mod loader
// Allows plugins to install inline hooks without needing the full implementation
struct IPluginHooks
{
    // Install an inline hook at the target address
    // Returns a hook handle on success, nullptr on failure
    // The originalFunc pointer is set to a trampoline that can call the original function
    HookHandle (*InstallHook)(uintptr_t targetAddress, void* detourFunction, void** originalFunction);

    // Remove a hook and restore original bytes
    void (*RemoveHook)(HookHandle handle);

    // Check if a hook is currently installed
    bool (*IsHookInstalled)(HookHandle handle);

  // Memory patching utilities
    bool (*PatchMemory)(uintptr_t address, const uint8_t* data, size_t size);
    bool (*NopMemory)(uintptr_t address, size_t size);
    bool (*ReadableMemory)(uintptr_t address, size_t size);
    bool (*WriteableMemory)(uintptr_t address, size_t size);
    
    // Register for world begin play events (ChimeraMain world only)
    // Callback signature: void OnWorldBeginPlay(SDK::UWorld* world)
    void (*RegisterWorldBeginPlayCallback)(void (*callback)(SDK::UWorld*));
    
    // Unregister world begin play callback
    void (*UnregisterWorldBeginPlayCallback)(void (*callback)(SDK::UWorld*));

    // Register for engine initialization events
    // Callback signature: void OnEngineInit()
    void (*RegisterEngineInitCallback)(void (*callback)());

    // Unregister engine init callback
    void (*UnregisterEngineInitCallback)(void (*callback)());

    // Register for engine shutdown events - fires before UObject system tears down.
    // Use this to clean up any patches/allocations that reference engine-owned memory.
    // Callback signature: void OnEngineShutdown()
    void (*RegisterEngineShutdownCallback)(void (*callback)());

    // Unregister engine shutdown callback
    void (*UnregisterEngineShutdownCallback)(void (*callback)());

    // -----------------------------------------------------------------------
    // Engine memory allocator (v5)
    //
    // These wrap the game's FMemory::Malloc / FMemory::Free so plugins can
    // safely allocate and free memory that the engine (GC, FString destructors,
    // etc.) may later touch.  Resolved automatically at engine init time.
    //
    // Returns nullptr / does nothing if the allocator could not be resolved.
    // Always check IsEngineAllocatorAvailable() before use.
    // -----------------------------------------------------------------------

    void* (*EngineAlloc)(size_t count, uint32_t alignment);
    void (*EngineFree)(void* ptr);
    bool (*IsEngineAllocatorAvailable)();

    // -----------------------------------------------------------------------
    // Any-world begin play callbacks (v6)
    //
    // Unlike RegisterWorldBeginPlayCallback (ChimeraMain only), these fire for
    // every world that begins play — the world name is passed as a C string.
    // Callback signature: void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName)
    // -----------------------------------------------------------------------

    void (*RegisterAnyWorldBeginPlayCallback)(void (*callback)(SDK::UWorld*, const char*));
    void (*UnregisterAnyWorldBeginPlayCallback)(void (*callback)(SDK::UWorld*, const char*));

    // -----------------------------------------------------------------------
    // Save-loaded callbacks (v7)
    //
    // Fires when UCrMassSaveSubsystem::OnSaveLoaded completes — i.e. after
    // the save has finished loading and all actors should be spawned.
    // Callback signature: void OnSaveLoaded()
    // -----------------------------------------------------------------------

    void (*RegisterSaveLoadedCallback)(void (*callback)());
    void (*UnregisterSaveLoadedCallback)(void (*callback)());

    // -----------------------------------------------------------------------
    // Experience-load-complete callbacks (v8)
    //
    // Fires when UCrExperienceManagerComponent::OnExperienceLoadComplete
    // completes — this is significantly later than OnSaveLoaded and indicates
    // the map/gameplay experience is fully ready with all actors spawned.
    // Callback signature: void OnExperienceLoadComplete()
    // -----------------------------------------------------------------------

    void (*RegisterExperienceLoadCompleteCallback)(void (*callback)());
    void (*UnregisterExperienceLoadCompleteCallback)(void (*callback)());
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
