#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>

// Plugin interface version - increment when breaking changes are made
#define PLUGIN_INTERFACE_VERSION 1

// Log levels
enum class PluginLogLevel
{
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4
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

    // Find all occurrences of a pattern in the main executable module
    // Returns a vector of absolute addresses where the pattern was found
    // Note: Caller is responsible for managing the returned vector
    std::vector<uintptr_t> (*FindAllPatternsInMainModule)(const char* pattern);

    // Find all occurrences of a pattern in a specific module
    // Returns a vector of absolute addresses where the pattern was found
    // Note: Caller is responsible for managing the returned vector
    std::vector<uintptr_t> (*FindAllPatternsInModule)(HMODULE module, const char* pattern);

    // Find a unique pattern from a list of candidates
    // Returns the address if exactly one pattern matches exactly once
    // Returns 0 if no patterns match uniquely
    // outPatternIndex (if provided) is set to the index of the matching pattern
    uintptr_t (*FindUniquePattern)(const char** patterns, int patternCount, int* outPatternIndex);
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
    bool (*ReadMemory)(uintptr_t address, void* buffer, size_t size);
    
    // Register for world begin play events (ChimeraMain world only)
    // Callback signature: void OnWorldBeginPlay(SDK::UWorld* world)
    void (*RegisterWorldBeginPlayCallback)(void (*callback)(SDK::UWorld*));
    
    // Unregister world begin play callback
    void (*UnregisterWorldBeginPlayCallback)(void (*callback)(SDK::UWorld*));
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
