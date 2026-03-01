# Developer Guide � StarRupture ModLoader

> **End-user documentation is in [README.md](README.md) and [How To Use](How%20To%20Use.md).**

This guide covers building the mod loader from source, creating new plugins, and the internal architecture.

---

## Building from Source

### Prerequisites

- **Visual Studio 2022** with the C++ desktop workload (C++20 required)
- Windows SDK 10.0+

### Steps

1. Clone the repository.
2. Open `StarRupture-ModLoader.sln`.
3. Select **Client Debug/Release** or **Server Debug/Release** configuration (x64).
4. Build the solution.
5. Output DLLs are placed in `build/[Configuration]/`.

---

## Project Structure

```
StarRupture-ModLoader/
+-- Version_Mod_Loader/          # Core mod loader (ships as version.dll)
|   +-- logger.cpp               # Multi-level logging system
|   +-- scanner.cpp        # IDA-style pattern scanning engine
|   +-- hooks_interface.cpp      # MinHook-based function hooking
|   +-- config_manager.cpp       # INI config with schema validation
|   +-- plugin_manager.cpp    # Plugin discovery and lifecycle
|   +-- game/     # Game-specific hooks
|       +-- engine_init/  # Engine initialisation callback
|       +-- engine_shutdown/     # Engine shutdown callback
|       +-- world_begin_play/    # World loading callback
|
+-- StarRupture SDK/             # Dumper-7 generated Unreal Engine SDK
|   +-- SDK/  # Headers and implementations
|
+-- ExamplePlugin/   # Starter template for new plugins
+-- KeepTicking_Plugin/        # Anti-AFK / keep-alive for servers
+-- RailJunctionFixer/   # Runtime UStruct inheritance patcher
+-- ServerUtility/               # RCON, Steam Query, and CLI settings
```

### Version_Mod_Loader (Core)

The main DLL that loads as `version.dll` via DLL proxy injection. It provides:

- Plugin lifecycle management (load | init | tick | shutdown)
- Core services exposed to plugins via interfaces
- Game event detection and distribution
- Interface bridging between plugins and the Unreal Engine binary

### Plugin Projects

Each plugin is a separate DLL that:

- Exports three functions: `GetPluginInfo`, `PluginInit`, `PluginShutdown`
- Receives four interface pointers on init (logger, config, scanner, hooks)
- Outputs to the `alienx_mods/` directory for the loader to discover

---

## Creating a Plugin

Use `ExamplePlugin` as your starting point.

### Minimal Plugin

```cpp
#include "plugin.h"
#include "plugin_helpers.h"

static PluginInfo s_pluginInfo = {
    "MyPlugin",
    "1.0.0",
    "Your Name",
    "What this plugin does",
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
    LOG_INFO("Plugin initialising...");
    // Your init code here
    return true;
}

__declspec(dllexport) void PluginShutdown()
{
  LOG_INFO("Plugin shutting down...");
    // Cleanup here
}

} // extern "C"
```

### Steps to Create a New Plugin

1. **Copy** the `ExamplePlugin` folder and rename it.
2. **Rename** the `.vcxproj` file and generate a new `<ProjectGuid>`.
3. **Update** `s_pluginInfo` in `plugin.cpp` with your plugin's metadata.
4. **Update** the log tag in `plugin_helpers.h`.
5. **Add** the project to the solution (right-click solution ? Add ? Existing Project).

---

## Plugin API Reference

### IPluginLogger

```cpp
LOG_TRACE("Very detailed trace message");
LOG_DEBUG("Diagnostic information");
LOG_INFO("General information");
LOG_WARN("Warning message");
LOG_ERROR("Error message");
```

Logs are written to `alienx_mods/logs/modloader.log`.

### IPluginConfig

```cpp
// Schema-based (recommended) � auto-generates .ini with defaults
// See ExamplePlugin/plugin_config.h for full example

// Manual access
int value = GetConfig()->ReadInt("PluginName", "Section", "Key", defaultValue);
GetConfig()->WriteString("PluginName", "Section", "Key", "value");
```

Config files are stored in `alienx_mods/config/<PluginName>.ini`.

### IPluginScanner

```cpp
// Find a pattern in the main game module (IDA-style, ?? = wildcard)
uintptr_t addr = scanner->FindPatternInMainModule("48 89 5C 24 ?? 57 48 83 EC 20");

// Find all occurrences
std::vector<uintptr_t> results = scanner->FindAllPatternsInMainModule("E8 ?? ?? ?? ??");

// Find a unique match from multiple candidate patterns
const char* patterns[] = {
    "48 8B C4 48 89 58 ??",
    "48 89 5C 24 ?? 48 89 6C"
};
uintptr_t addr = scanner->FindUniquePattern(patterns, 2, &matchedIndex);
```

### IPluginHooks

```cpp
typedef void(__fastcall* OriginalFunc_t)(void* thisPtr, int param);
static OriginalFunc_t g_original = nullptr;

void __fastcall MyDetour(void* thisPtr, int param)
{
    LOG_DEBUG("Function called with param: %d", param);
    g_original(thisPtr, param);  // Call original
}

// Install
HookHandle handle = hooks->InstallHook(targetAddr, (void*)&MyDetour, (void**)&g_original);

// Remove
hooks->RemoveHook(handle);
```

### Game Event Callbacks

```cpp
// Called once when the engine is fully initialised
hooks->RegisterEngineInitCallback(OnEngineInit);

// Called when the engine is shutting down
hooks->RegisterEngineShutdownCallback(OnEngineShutdown);

// Called when any world begins play
hooks->RegisterAnyWorldBeginPlayCallback(OnAnyWorldBeginPlay);

// Called when the experience (game mode) finishes loading
hooks->RegisterExperienceLoadCompleteCallback(OnExperienceLoadComplete);

// Called every engine tick
hooks->RegisterEngineTickCallback(OnEngineTick);
```

---

## Advanced Examples

### Runtime Type System Patching (RailJunctionFixer)

```cpp
// Modify UStruct inheritance at runtime
auto* targetStruct = SDK::UObject::FindObjectFast<SDK::UScriptStruct>(
    "MyFragment", SDK::EClassCastFlags::ScriptStruct);

// Change parent class
targetStruct->SuperStruct = newParentStruct;
```

### Engine Allocator Integration (ServerUtility)

When writing to engine FString fields from a hook, you must allocate via `FMemory::Malloc` / `FMemory::Free` so the garbage collector sees valid `FMallocBinned2` canary values. The ServerUtility plugin demonstrates resolving these functions via pattern scanning at runtime.

---

## Troubleshooting (Development)

### Plugin Not Loading
- Verify the DLL is in `alienx_mods/` and exports all three functions.
- Check `PLUGIN_INTERFACE_VERSION` matches the loader.
- Check `alienx_mods/logs/modloader.log` for initialisation errors.

### Pattern Not Found
- Confirm the pattern against a disassembler (IDA, Ghidra, x64dbg).
- Game updates change binary layouts � patterns may need updating.
- Use `FindAllPatternsInMainModule` to check for multiple matches.

### Hook Crashes
- Ensure the calling convention matches (`__fastcall` for most UE5 member functions).
- Verify the function signature (parameter count and types).
- Check the original function pointer is valid before calling through it.
- Wrap hook bodies in try-catch for error isolation.

### Config Not Saving
- Ensure `alienx_mods/config/` exists (created automatically on first run).
- Check file permissions.

---

## Contributing

1. Fork the repository.
2. Create a feature branch.
3. Follow existing code style (tabs for indentation, existing naming conventions).
4. Include comprehensive logging.
5. Test with both Debug and Release builds.
6. Submit a pull request.

---

## License

This project is provided as-is for educational purposes.
