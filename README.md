# StarRupture ModLoader

> [!TIP]
> 🎮 **Just want to install and play?** → [**How To Use**](How%20To%20Use.md)

A plugin-based mod loader for StarRupture, providing a framework for creating game modifications without directly modifying game files.

## Overview

This mod loader uses DLL proxying (`version.dll`) to inject into the game process and provides a comprehensive plugin API for creating mods. The loader handles memory scanning, function hooking, configuration management, and game event callbacks.

## Current Plugins
- **ServerUtility** - Allows servers to boot without DSSettings.txt using the `-SessionName="MySaveGame"` parameter.  Will soon support password generation too.
- **KeepTicking** - Allows the server to keep running even when no one is online (Note: this will spawn aliens etc)
- **RailJunctionFixer** - Fixes the 3x and 5x rails so that the logistic drones no longer teleport to a single rail. (Note: this only fixes newly placed junctions)

## Features

- **DLL Proxy Injection** - Automatic loading via `version.dll` proxy
- **Plugin System** - Modular architecture for easy mod development
- **Memory Scanner** - Pattern scanning for finding game functions
- **Function Hooking** - Safe hooking system for intercepting game functions
- **Configuration System** - INI-based config with schema validation
- **Logging System** - Multi-level logging (Trace/Debug/Info/Warn/Error)
- **Game Event Callbacks** - Engine initialization and world begin play events
- **Unreal SDK Integration** - Full access to Unreal Engine classes via Dumper-7 SDK

## Architecture

### Core Components

```
StarRupture-ModLoader/
+-- Version_Mod_Loader/      # Core mod loader (version.dll proxy)
|   +-- logger.cpp           # Multi-level logging system
|   +-- scanner.cpp          # Pattern scanning engine
|   +-- hooks_interface.cpp  # Function hooking system
|   +-- config_manager.cpp   # INI config management
|   +-- plugin_manager.cpp   # Plugin loading/unloading
|   +-- game/                # Game-specific hooks
|       +-- engine_init/     # Engine initialization callback
|       +-- engine_shutdown/ # Engine shutdown callback
|       +-- world_begin_play/ # World loading callback
|
+-- StarRupture SDK/         # Dumper-7 generated Unreal Engine SDK
|   +-- SDK/                 # SDK headers and implementations
|
+-- ExamplePlugin/           # Template plugin (starter example)
+-- AntiLogSpam/             # Suppresses null-pointer log spam
+-- KeepTicking_Plugin/      # Anti-AFK plugin for dedicated servers
+-- RailJunctionFixer/       # Runtime type system patcher
```

## Installation

### For Users

1. Download the latest release
2. Copy `version.dll` to your StarRupture game directory (where `StarRupture.exe` is located)
3. Create an `alienx_mods` folder in the game directory
4. Place plugin DLL files in the `alienx_mods` folder
5. Launch the game normally

The mod loader will automatically:
- Load all plugins from `alienx_mods`
- Create config files in `alienx_mods/configs/`
- Generate logs in `alienx_mods/logs/`

### For Developers

1. Clone this repository
2. Open `StarRupture-ModLoader.sln` in Visual Studio 2022
3. Build the solution (Debug or Release)
4. Output DLLs are placed in `build/[Configuration]/`

## Creating Plugins

### Quick Start

Use `ExamplePlugin` as a template:

```cpp
// plugin.cpp
#include "plugin.h"
#include "plugin_helpers.h"

static PluginInfo s_pluginInfo = {
    "MyPlugin",           // Name
    "1.0.0",   // Version
    "YourName", // Author
    "Plugin description", // Description
    PLUGIN_INTERFACE_VERSION
};

extern "C" {
    __declspec(dllexport) PluginInfo* GetPluginInfo()
    {
        return &s_pluginInfo;
    }

    __declspec(dllexport) bool PluginInit(
        IPluginLogger* logger, 
        IPluginConfig* config, 
        IPluginScanner* scanner, 
        IPluginHooks* hooks)
    {
        LOG_INFO("Plugin initializing...");
        
        // Your initialization code here
        
        return true;
    }

    __declspec(dllexport) void PluginShutdown()
    {
     LOG_INFO("Plugin shutting down...");
        // Cleanup code here
    }
}
```

### Plugin API

Plugins receive four interfaces:

#### IPluginLogger
```cpp
LOG_TRACE("Detailed trace message");
LOG_DEBUG("Debug information");
LOG_INFO("General information");
LOG_WARN("Warning message");
LOG_ERROR("Error message");
```

#### IPluginScanner
```cpp
// Find a pattern in the main game module
uintptr_t addr = scanner->FindPatternInMainModule("48 89 5C 24 ?? 48 89 74 24");

// Find all occurrences
std::vector<uintptr_t> results = scanner->FindAllPatternsInMainModule("E8 ?? ?? ?? ??");
```

#### IPluginHooks
```cpp
// Install a function hook
bool success = hooks->InstallHook(
    targetAddress,
    (void*)&MyDetourFunction,
    (void**)&originalFunction
);

// Remove a hook
hooks->RemoveHook(targetAddress);
```

#### IPluginConfig
```cpp
// Get config values
int value = config->GetInt("MyPlugin", "SettingName", 42);
std::string str = config->GetString("MyPlugin", "TextSetting", "default");
bool enabled = config->GetBool("MyPlugin", "Enabled", true);

// Set config values
config->SetInt("MyPlugin", "SettingName", 100);
```

### Game Event Callbacks

Register for engine events:

```cpp
// Called when the game world loads
void OnWorldBeginPlay(SDK::UWorld* world)
{
    LOG_INFO("World loaded: %s", world->GetName().c_str());
}

// Register in PluginInit:
hooks->RegisterEngineInitCallback(MyInitCallback);
hooks->RegisterWorldBeginPlayCallback(OnWorldBeginPlay);
```

## Example Plugins

### KeepTicking
**Purpose**: Prevents dedicated servers from automatically shutting down when no players are connected.

**How it works**:
- Spawns an invisible, invulnerable fake player when server would otherwise be empty
- Configurable via `KeepTicking.INI`
- Uses SDK to spawn actors and manage player controllers

**Use case**: Running persistent dedicated servers that should stay online 24/7

### RailJunctionFixer
**Purpose**: Fixes save/load issues with rail logistics by patching the Unreal Engine type system at runtime.

**How it works**:
- Modifies `FCrLogisticsSocketsFragment` to inherit from `FCrMassSavableFragment`
- Rebuilds the UStruct inheritance chain for proper `IsChildOf` checks
- Ensures logistics data is included in the mass entity save system

**Technical details**: Direct memory patching of UE5 reflection metadata

## Building from Source

### Requirements

- Visual Studio 2022
- Windows SDK 10.0
- C++20 compiler support

### Build Steps

1. Open `StarRupture-ModLoader.sln`
2. Select configuration (Debug or Release)
3. Build Solution (F7)

### Output

- **Version_Mod_Loader**: `build/[Configuration]/version.dll` (core loader)
- **Plugins**: `build/[Configuration]/alienx_mods/*.dll`

## SDK Integration

This project uses a Dumper-7 generated SDK for StarRupture. The SDK provides:

- Full Unreal Engine class definitions
- Blueprint function wrappers
- Struct and enum definitions
- Direct access to game objects via `UObject::GObjects`

### Using the SDK in Plugins

Each plugin includes SDK wrapper files:
- `Basic.cpp` - Core SDK functionality
- `CoreUObject_functions.cpp` - UObject/UClass functions
- `Engine_functions.cpp` - Engine-specific functions (UWorld, AActor, etc.)

Example SDK usage:
```cpp
#include "SDK/Engine_classes.hpp"

// Get the game world
SDK::UWorld* world = SDK::UWorld::GetWorld();

// Find a class by name
SDK::UClass* actorClass = SDK::UObject::FindClassFast<SDK::UClass>(
    "PlayerController", SDK::EClassCastFlags::Class);

// Spawn an actor
SDK::AActor* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
    world, actorClass, transform, spawnMethod, owner);
```

## Configuration

Plugins can use INI configuration files stored in `alienx_mods/configs/[PluginName].ini`.

Example configuration:
```ini
[Section]
Key=Value
```

## Logging

Logs are written to `alienx_mods/logs/modloader_[timestamp].log` with the following levels:

- **TRACE**: Very detailed debugging information
- **DEBUG**: Diagnostic information for troubleshooting
- **INFO**: General informational messages
- **WARN**: Warning messages for potential issues
- **ERROR**: Error messages for failures

## Advanced Features

### Memory Scanning

The scanner supports IDA-style patterns:
```cpp
// ?? = wildcard byte
"48 89 5C 24 ?? 57 48 83 EC 20"

// Find unique pattern from multiple candidates
const char* patterns[] = {
    "48 8B C4 48 89 58 ??",
    "48 89 5C 24 ?? 48 89 6C"
};
uintptr_t addr = scanner->FindUniquePattern(patterns, 2, &matchedIndex);
```

### Function Hooking

Safe MinHook-based hooking with trampoline generation:
```cpp
typedef void(__fastcall* OriginalFunc_t)(void* thisPtr, int param);
static OriginalFunc_t g_original = nullptr;

void __fastcall MyDetour(void* thisPtr, int param)
{
    // Pre-hook logic
    LOG_DEBUG("Function called with param: %d", param);
    
    // Call original
    g_original(thisPtr, param);
    
    // Post-hook logic
}

// Install hook
hooks->InstallHook(targetAddr, (void*)&MyDetour, (void**)&g_original);
```

### Runtime Type System Patching

Advanced example from `RailJunctionFixer`:
```cpp
// Modify UStruct inheritance at runtime
auto* targetStruct = SDK::UObject::FindObjectFast<SDK::UScriptStruct>(
    "MyFragment", SDK::EClassCastFlags::ScriptStruct);

// Change parent class
targetStruct->SuperStruct = newParentStruct;

// Rebuild inheritance chain for IsChildOf checks
// (See RailJunctionFixer for full implementation)
```

## Project Structure

### Version_Mod_Loader (Core)
The main DLL that loads as `version.dll`. Provides:
- Plugin lifecycle management
- Core services (logging, config, scanning, hooks)
- Game event detection and distribution
- Interface bridging between plugins and game

### Plugin Projects
Each plugin is a separate DLL project that:
- Implements the plugin interface (`GetPluginInfo`, `PluginInit`, `PluginShutdown`)
- Links against SDK wrapper files
- Outputs to `alienx_mods/` directory

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Test thoroughly
5. Submit a pull request

### Plugin Development Guidelines

- Use the `ExamplePlugin` template as a starting point
- Follow existing code style (tabs, naming conventions)
- Include comprehensive logging
- Handle errors gracefully
- Document configuration options
- Test with both Debug and Release builds

## Troubleshooting

### Plugin Not Loading
- Check that DLL is in `alienx_mods/` folder
- Verify plugin exports (`GetPluginInfo`, `PluginInit`, `PluginShutdown`)
- Check logs for initialization errors
- Ensure `PLUGIN_INTERFACE_VERSION` matches loader version

### Pattern Not Found
- Verify pattern is correct using a disassembler
- Check if game version matches
- Use `FindAllPatternsInMainModule` to see all matches
- Consider using multiple pattern candidates with `FindUniquePattern`

### Hook Crashes
- Ensure calling convention matches (`__fastcall`, `__stdcall`, etc.)
- Verify function signature is correct
- Check that original function pointer is valid before calling
- Use try-catch blocks for error isolation

### Config Not Saving
- Ensure config directory exists (`alienx_mods/configs/`)
- Check file permissions
- Verify ini syntax is valid
- Use schema validation in config initialization

## Credits

- **Dumper-7** - Unreal Engine SDK generation
- **MinHook** - Function hooking library
- **nlohmann/json** - JSON parsing library
- **Wilhelm-af** - Fantastic work on Rail Fix Logic

## Disclaimer

This is a modding tool for educational purposes. Use at your own risk. The authors are not responsible for any damage caused by using this software.

---

**Game**: StarRupture  
**Engine**: Unreal Engine 5  
**Mod Loader Version**: 1.0.0  
**Interface Version**: 3
