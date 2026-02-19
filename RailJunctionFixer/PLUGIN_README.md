# RailJunctionFixer Plugin

This plugin uses the StarRupture Mod Loader plugin interface.

## Plugin Structure

- **plugin_interface.h** - Defines the plugin interface structures and functions
- **plugin.h** - Plugin export declarations
- **plugin.cpp** - Plugin implementation with PluginInit, PluginShutdown, and GetPluginInfo exports
- **plugin_helpers.h** - Helper functions and macros for easier plugin development

## Usage

### Logging

Use the convenience macros for logging:

```cpp
#include "plugin_helpers.h"

LOG_TRACE("Trace message: %d", value);
LOG_DEBUG("Debug message: %s", str);
LOG_INFO("Info message");
LOG_WARN("Warning message");
LOG_ERROR("Error message");
```

Or use the logger interface directly:

```cpp
if (auto logger = GetLogger())
{
    logger->Info("RailJunctionFixer", "Message");
}
```

### Configuration

Read and write configuration values:

```cpp
if (auto config = GetConfig())
{
    int myValue = config->ReadInt("RailJunctionFixer", "Settings", "MyValue", 100);
    config->WriteInt("RailJunctionFixer", "Settings", "MyValue", 200);
}
```

### Pattern Scanning

Find patterns in memory:

```cpp
if (auto scanner = GetScanner())
{
    uintptr_t address = scanner->FindPatternInMainModule("48 89 5C 24 ?? 57 48 83 EC 20");
    if (address)
    {
     LOG_INFO("Found pattern at: 0x%llX", address);
    }
}
```

### Hooking

Install inline hooks:

```cpp
typedef void (*OriginalFunctionType)(int param);
OriginalFunctionType g_originalFunction = nullptr;

void MyDetourFunction(int param)
{
    LOG_INFO("Hook called with param: %d", param);
    
    // Call original function
    if (g_originalFunction)
    {
        g_originalFunction(param);
    }
}

// In PluginInit:
if (auto hooks = GetHooks())
{
    uintptr_t targetAddress = /* find address */;
 hooks->InstallHook(targetAddress, (void*)MyDetourFunction, (void**)&g_originalFunction);
}
```

### Memory Patching

Patch memory directly:

```cpp
if (auto hooks = GetHooks())
{
    uint8_t patch[] = { 0x90, 0x90, 0x90 }; // NOP instructions
    hooks->PatchMemory(address, patch, sizeof(patch));
    
    // Or use NOP helper
    hooks->NopMemory(address, 3);
}
```

## Plugin Metadata

Update the plugin metadata in `plugin.cpp`:

```cpp
static PluginInfo s_pluginInfo = {
    "RailJunctionFixer",      // Plugin name
    "1.0.0",         // Version
    "Author",     // Author
    "Fixes rail junction issues in the game", // Description
    PLUGIN_INTERFACE_VERSION  // Interface version (don't change)
};
```

## Implementation

Add your plugin logic in the `PluginInit` function in `plugin.cpp`. Use the TODO comments as guides:

1. Find memory patterns using the scanner
2. Install hooks using the hooks interface
3. Patch memory if needed
4. Register for world events if needed

Remember to cleanup in `PluginShutdown`:

1. Remove installed hooks
2. Cleanup any allocated resources
3. Unregister callbacks
