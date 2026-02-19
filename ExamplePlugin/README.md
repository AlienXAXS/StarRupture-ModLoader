# ExamplePlugin

A minimal example plugin demonstrating the basic structure required by the StarRupture Mod Loader.

## Purpose

This plugin demonstrates:
- ? Minimum required plugin structure
- ? How to use the logging system
- ? How to use the config system (optional)
- ? Does NOT modify gameplay
- ? Does NOT install hooks
- ? Does NOT scan patterns

## Structure

### Required Files

1. **plugin.h** - Plugin export declarations
2. **plugin.cpp** - Plugin implementation with required exports
3. **plugin_interface.h** - Copy of mod loader's plugin interface
4. **plugin_helpers.h** - Helper functions and logging macros
5. **pch.h/pch.cpp** - Precompiled header (standard C++ project setup)
6. **dllmain.cpp** - DLL entry point (standard Windows DLL)
7. **framework.h** - Windows headers (standard Windows DLL)

### Optional Files (Config Example)

8. **plugin_config.h** - Config schema and type-safe accessors
9. **plugin_config.cpp** - Config implementation

### Required Exports

Every plugin **must** export these three functions:

```cpp
extern "C" {
    __declspec(dllexport) PluginInfo* GetPluginInfo();
    __declspec(dllexport) bool PluginInit(IPluginLogger* logger, IPluginConfig* config, 
        IPluginScanner* scanner, IPluginHooks* hooks);
    __declspec(dllexport) void PluginShutdown();
}
```

## Config System Example

This plugin demonstrates the config schema system. On first load, it creates:

**ExamplePlugin.ini**
```ini
[General]
Enabled=true
ExampleString=Hello World

[Settings]
ExampleNumber=42
ExampleFloat=3.14
```

### Config Schema Definition

```cpp
static const ConfigEntry CONFIG_ENTRIES[] = {
    {
        "General",    // Section
        "Enabled",        // Key
        ConfigValueType::Boolean,  // Type
    "true",   // Default value
        "Enable or disable the example plugin"  // Description
    },
    // ... more entries ...
};
```

### Using Config Values

```cpp
// Initialize config (in PluginInit)
ExamplePluginConfig::Config::Initialize(config);

// Read values with type safety
if (ExamplePluginConfig::Config::IsEnabled())
{
    const char* str = ExamplePluginConfig::Config::GetExampleString();
    int num = ExamplePluginConfig::Config::GetExampleNumber();
    float val = ExamplePluginConfig::Config::GetExampleFloat();
}
```

## Plugin Lifecycle

1. **Mod Loader Scans** - Finds `ExamplePlugin.dll` in `plugins/` folder
2. **GetPluginInfo()** - Mod loader calls this to read metadata
3. **PluginInit()** - Mod loader calls this with interface pointers
   - Store the interface pointers in static variables
   - Initialize config system (optional)
   - Initialize your plugin
   - Return `true` for success, `false` for failure
4. **Plugin Runs** - Your plugin is now active
5. **PluginShutdown()** - Called when game closes or plugin is unloaded
   - Clean up resources
   - Remove hooks
   - Clear interface pointers

## Available Interfaces

### IPluginLogger
```cpp
LOG_INFO("Your message here");
LOG_DEBUG("Debug info: %d", value);
LOG_WARN("Warning!");
LOG_ERROR("Error occurred");
```

### IPluginConfig
```cpp
// Automatic schema-based config (recommended)
ExamplePluginConfig::Config::Initialize(config);
bool enabled = ExamplePluginConfig::Config::IsEnabled();

// Or manual config access
int value = GetConfig()->ReadInt("ExamplePlugin", "Section", "Key", defaultValue);
GetConfig()->WriteString("ExamplePlugin", "Section", "Key", "value");
```

### IPluginScanner
```cpp
uintptr_t address = GetScanner()->FindPatternInMainModule("48 89 5C 24 ?? 57");
```

### IPluginHooks
```cpp
HookHandle hook = GetHooks()->InstallHook(address, MyDetour, (void**)&originalFunc);
GetHooks()->RemoveHook(hook);
```

## Building

1. Open `StarRupture-ModLoader.sln`
2. **Right-click solution ? Reload Project** (to pick up config files)
3. Build solution
4. DLL outputs to `bin\x64\[Debug|Release]\plugins\ExamplePlugin.dll`

## Installation

Copy `ExamplePlugin.dll` to:
```
<game_directory>\alienx_mods\plugins\
```

The mod loader will automatically load it on next game start.

## What This Plugin Does

On load:
- ? Loads successfully
- ? Creates config file with defaults (if missing)
- ? Reads and validates config values
- ? Logs initialization messages
- ? Demonstrates enable/disable via config
- ? Stores interface pointers
- ? Shuts down cleanly
- ? Does not modify gameplay
- ? Does not install hooks

This is intentional - it's a template with config examples, not a functional mod.

## Example Output

When loaded, you'll see in `alienx_mods\logs\modloader.log`:

```
[INFO] [ConfigManager] Creating new config for 'ExamplePlugin' with 4 entries
[INFO] [ConfigManager] Config created: ...\alienx_mods\config\ExamplePlugin.ini
[INFO] [ExamplePlugin] Plugin initializing...
[INFO] [ExamplePlugin] Config values:
[INFO] [ExamplePlugin]   ExampleString: Hello World
[INFO] [ExamplePlugin]   ExampleNumber: 42
[INFO] [ExamplePlugin]   ExampleFloat: 3.14
[INFO] [ExamplePlugin] Plugin initialized successfully
```

If you edit the config and set `Enabled=false`:

```
[WARN] [ExamplePlugin] Plugin is disabled in config file
```

## Customization

To create your own plugin:

1. **Copy the folder**
   ```
   cp -r ExamplePlugin MyPlugin
   ```

2. **Rename project** (close solution first)
   - Rename `ExamplePlugin.vcxproj` ? `MyPlugin.vcxproj`
   - Edit .vcxproj and change `<ProjectGuid>` (generate new GUID)

3. **Update metadata in plugin.cpp**
   ```cpp
   static PluginInfo s_pluginInfo = {
       "MyPlugin",      // ? Change
   "1.0.0",
       "Your Name",     // ? Change
       "Description",   // ? Change
       PLUGIN_INTERFACE_VERSION
   };
   ```

4. **Update logging macros in plugin_helpers.h**
   ```cpp
   #define LOG_INFO(format, ...) \
       logger->Info("MyPlugin", format, ##__VA_ARGS__)
   ```

5. **Update config (if using)**
   - Edit `plugin_config.h`
   - Change `"ExamplePlugin"` ? `"MyPlugin"` in all places
   - Update config entries in `CONFIG_ENTRIES`
   - Update accessor methods in `Config` class

6. **Add to solution**
   - Right-click solution ? Add ? Existing Project
   - Select `MyPlugin\MyPlugin.vcxproj`

## Config System Notes

The config system is **completely optional**. You can:

1. **Use it** (as shown) - Automatic config generation
2. **Skip it** - Remove `plugin_config.*` files and related code
3. **Manual config** - Use `IPluginConfig` directly without schema

### Config File Location

```
<game_dir>\alienx_mods\config\ExamplePlugin.ini
```

Users can edit this file while the game is running, and changes will be read on next config access (though the `Enabled` flag is only checked during `PluginInit`).

## Next Steps

For actual plugin development with game modification, see:
- **KeepTicking_Plugin** - Full-featured example with hooks and SDK
- **RailJunctionFixer** - Pattern scanning example
- **docs/PLUGIN_CONFIG_GUIDE.md** - Complete config system guide
- **docs/CONFIG_SYSTEM_IMPLEMENTATION.md** - Config system internals

## License

This example plugin is provided as-is for educational purposes.
