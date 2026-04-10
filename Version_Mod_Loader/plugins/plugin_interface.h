#pragma once

#include <windows.h>
#include <cstdint>

// v19: Introduced IPluginSelf — a single struct bundling name, version, logger, config, scanner,
//      and hooks. PluginInit now receives IPluginSelf* instead of four separate pointers.
//      Removed const char* pluginName from IPluginLogger, IPluginConfig, and IPluginNetworkChannel;
//      replaced with const IPluginSelf* so the loader can identify the calling plugin without
//      plugins having to duplicate their own name string in every call.
//      MIN bumped to 19 (ABI break — all plugins must be recompiled against this header).
// v20: Added OnBeforeWorldEndPlay / OnAfterWorldEndPlay to IPluginWorldEvents.
//      MIN remains 19 — loader accepts v19 plugins (they won't use the new callbacks).
#define PLUGIN_INTERFACE_VERSION_MIN 19  // oldest plugin ABI still accepted by this loader
#define PLUGIN_INTERFACE_VERSION_MAX 20  // current interface version (this header)
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
    const char* section;      // INI section name (e.g., "General", "Advanced")
    const char* key;          // INI key name
    ConfigValueType type;     // Value type
    const char* defaultValue; // Default value as string (converted based on type)
    const char* description;  // Optional description/comment
    float rangeMin;           // Slider min -- ignored when rangeMax <= rangeMin
    float rangeMax;           // Slider max -- when rangeMax > rangeMin a slider is shown
};

// Config schema - defines all config entries for a plugin
struct ConfigSchema
{
    const ConfigEntry* entries; // Array of config entries
    int entryCount;          // Number of entries in array
};

// Forward declaration — IPluginSelf is defined after IPluginHooks (all sub-interfaces must be complete first).
struct IPluginSelf;

// Universal logger interface provided by mod loader
struct IPluginLogger
{
    // Log a message with the specified level
    void (*Log)(PluginLogLevel level, const IPluginSelf* self, const char* message);

    // Convenience methods
    void (*Trace)(const IPluginSelf* self, const char* format, ...);
    void (*Debug)(const IPluginSelf* self, const char* format, ...);
    void (*Info) (const IPluginSelf* self, const char* format, ...);
    void (*Warn) (const IPluginSelf* self, const char* format, ...);
    void (*Error)(const IPluginSelf* self, const char* format, ...);
};

// Config manager interface provided by mod loader
struct IPluginConfig
{
    // Read string value from plugin's config file
    bool (*ReadString)(const IPluginSelf* self, const char* section, const char* key, char* outValue, int maxLen, const char* defaultValue);

    // Write string value to plugin's config file
    bool (*WriteString)(const IPluginSelf* self, const char* section, const char* key, const char* value);

    // Read integer value from plugin's config file
    int (*ReadInt)(const IPluginSelf* self, const char* section, const char* key, int defaultValue);

    // Write integer value to plugin's config file
    bool (*WriteInt)(const IPluginSelf* self, const char* section, const char* key, int value);

    // Read float value from plugin's config file
    float (*ReadFloat)(const IPluginSelf* self, const char* section, const char* key, float defaultValue);

    // Write float value to plugin's config file
    bool (*WriteFloat)(const IPluginSelf* self, const char* section, const char* key, float value);

    // Read boolean value from plugin's config file
    bool (*ReadBool)(const IPluginSelf* self, const char* section, const char* key, bool defaultValue);

    // Write boolean value to plugin's config file
    bool (*WriteBool)(const IPluginSelf* self, const char* section, const char* key, bool value);

    // Initialize plugin config from schema
    // Creates config file with defaults if it doesn't exist
    // Returns true if config was loaded/created successfully
    bool (*InitializeFromSchema)(const IPluginSelf* self, const ConfigSchema* schema);

    // Validate and repair config file based on schema
    // Adds missing entries with defaults, preserves existing values
    void (*ValidateConfig)(const IPluginSelf* self, const ConfigSchema* schema);
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
typedef void (*PluginWorldEndPlayCallback)(SDK::UWorld* world, const char* worldName);
// v16 (client only) — fired after AHUD::PostRender; hud is AHUD* cast to void*
typedef void (*PluginHUDPostRenderCallback)(void* hud);
// v17 -- fired on the client when a server packet arrives for this plugin+typeTag.
// pluginName : the name the server-side plugin passed to SendPacketToClient/SendPacketToAllClients
// typeTag    : identifies the packet type (use typeid(T).name() via plugin_network_helpers.h)
// data       : decoded payload bytes -- exactly 'size' bytes; pointer is valid only during this call
// size       : byte count of the payload; always > 0
typedef void (*PluginNetworkMessageCallback)(const char* pluginName, const char* typeTag,
                                             const uint8_t* data, size_t size);

// v18 -- fired on the server when a client packet arrives for this plugin+typeTag.
// senderPlayerController : the APlayerController* of the sending client cast to void*
// pluginName / typeTag / data / size : same semantics as PluginNetworkMessageCallback
typedef void (*PluginNetworkServerMessageCallback)(void* senderPlayerController,
                                                   const char* pluginName, const char* typeTag,
                                                   const uint8_t* data, size_t size);

// v18 -- callback type for PostToGameThread.
// context : the void* passed to PostToGameThread; cast to your own struct inside the callback.
typedef void (*PluginGameThreadCallback)(void* context);

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
    // v16 -- resolved address of CoreUObject::StaticLoadObject, or 0 if not found.
    // Available on all build types.
    uintptr_t (*GetStaticLoadObjectAddress)();
    // v18 -- post a fire-and-forget callback onto the game thread.
    // fn is called from the game thread during the next engine tick.
    // context is passed unchanged to fn; pass nullptr if you have no state.
    // Thread-safe; may be called from any thread (RCON handlers, network callbacks, etc.).
    void (*PostToGameThread)(PluginGameThreadCallback fn, void* context);
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
    // Fired before UWorld::EndPlay runs (world still valid). world/worldName valid for the call duration.
    void (*RegisterOnBeforeWorldEndPlay)(PluginWorldEndPlayCallback);
    void (*UnregisterOnBeforeWorldEndPlay)(PluginWorldEndPlayCallback);
    // Fired after UWorld::EndPlay runs. world pointer may be partially torn down; use worldName only.
    void (*RegisterOnAfterWorldEndPlay)(PluginWorldEndPlayCallback);
    void (*UnregisterOnAfterWorldEndPlay)(PluginWorldEndPlayCallback);
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
// EModKey — enumeration of keys that can be bound by plugins (v15)
// Maps directly to Unreal Engine EKeys names and Win32 VK codes.
// Use with IPluginInputEvents::RegisterKeybind or RegisterKeybindByName.
// ============================================================
enum class EModKey : uint32_t
{
    // Function keys
    F1 = 0, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,

    // Letters
    A, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,

    // Digit row
    Zero, One, Two, Three, Four, Five, Six, Seven, Eight, Nine,

    // Control keys
    Escape, Tab, CapsLock, SpaceBar, Enter, BackSpace, Delete, Insert,

    // Modifier keys
    LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt,

    // Navigation keys
    Up, Down, Left, Right, Home, End, PageUp, PageDown,

    // Punctuation / OEM keys
    Tilde, Hyphen, Equals, LeftBracket, RightBracket, Backslash,
    Semicolon, Apostrophe, Comma, Period, Slash,

    // Numpad digits
    NumPadZero, NumPadOne, NumPadTwo, NumPadThree, NumPadFour,
    NumPadFive, NumPadSix, NumPadSeven, NumPadEight, NumPadNine,

    // Numpad operators
    Add, Subtract, Multiply, Divide, Decimal,

    // Mouse buttons
    LeftMouseButton, RightMouseButton, MiddleMouseButton,
    ThumbMouseButton, ThumbMouseButton2,

    Unknown  // sentinel / error value
};

// ============================================================
// EModKeyEvent — key state transitions reported to keybind callbacks (v15)
// ============================================================
enum class EModKeyEvent : uint32_t
{
    Pressed  = 0,   // fired once when the key goes from up to down
    Released = 1    // fired once when the key goes from down to up
};

// ============================================================
// PluginKeybindCallback — signature for keybind event callbacks (v15)
// key   : the key that changed state
// event : Pressed or Released
// ============================================================
typedef void (*PluginKeybindCallback)(EModKey key, EModKeyEvent event);

// ============================================================
// IPluginInputEvents — keybind event subscriptions (v15, client only)
// Access via: hooks->Input->RegisterKeybind(...)
// hooks->Input is nullptr on server and generic builds.
// Both the enum API and the string API resolve to the same underlying handler.
// UE key name strings are case-insensitive ("F1", "LeftShift", "Tilde", etc.)
// ============================================================
struct IPluginInputEvents
{
    // Register a callback for a specific key+event (by enum)
    void (*RegisterKeybind)(EModKey key, EModKeyEvent event, PluginKeybindCallback callback);
    // Unregister a previously registered callback (by enum)
    void (*UnregisterKeybind)(EModKey key, EModKeyEvent event, PluginKeybindCallback callback);

    // Register a callback for a specific key+event (by UE key name string)
    // keyName is matched case-insensitively, e.g. "F1", "LeftShift", "Tilde"
    void (*RegisterKeybindByName)(const char* keyName, EModKeyEvent event, PluginKeybindCallback callback);
    // Unregister a previously registered callback (by UE key name string)
    void (*UnregisterKeybindByName)(const char* keyName, EModKeyEvent event, PluginKeybindCallback callback);
};

// ============================================================
// IModLoaderImGui — ImGui function table exposed to plugins (v15)
// The modloader owns Dear ImGui; plugins must NOT call ImGui:: directly.
// Call through this table inside a PluginImGuiRenderCallback.
// All text parameters are plain C strings — format with snprintf before calling.
// ============================================================
struct IModLoaderImGui
{
    // --- Text ---
    void (*Text)(const char* text);
    void (*TextColored)(float r, float g, float b, float a, const char* text);
    void (*TextDisabled)(const char* text);
    void (*TextWrapped)(const char* text);
    void (*LabelText)(const char* label, const char* text);
    void (*SeparatorText)(const char* label);

    // --- Inputs (return true when value changed) ---
    bool (*InputText)(const char* label, char* buf, size_t buf_size);
    bool (*InputInt)(const char* label, int* v, int step, int step_fast);
    bool (*InputFloat)(const char* label, float* v, float step, float step_fast, const char* format);
    bool (*Checkbox)(const char* label, bool* v);
    bool (*SliderFloat)(const char* label, float* v, float v_min, float v_max, const char* format);
    bool (*SliderInt)(const char* label, int* v, int v_min, int v_max, const char* format);

    // --- Buttons ---
    bool (*Button)(const char* label);
    bool (*SmallButton)(const char* label);

    // --- Layout ---
    void (*SameLine)(float offset_from_start_x, float spacing);
    void (*NewLine)();
    void (*Separator)();
    void (*Spacing)();
    void (*Indent)(float indent_w);
    void (*Unindent)(float indent_w);

    // --- ID stack ---
    void (*PushIDStr)(const char* str_id);
    void (*PushIDInt)(int int_id);
    void (*PopID)();

    // --- Combo / Selectable ---
    bool (*BeginCombo)(const char* label, const char* preview_value);
    bool (*Selectable)(const char* label, bool selected);
    void (*EndCombo)();

    // --- Tree / Collapsible ---
    bool (*CollapsingHeader)(const char* label);
    bool (*TreeNodeStr)(const char* label);
    void (*TreePop)();

    // --- Color ---
    bool (*ColorEdit3)(const char* label, float col[3]);
    bool (*ColorEdit4)(const char* label, float col[4]);

    // --- Misc ---
    void (*SetTooltip)(const char* text);
    bool (*IsItemHovered)();
    void (*SetNextItemWidth)(float item_width);
};

// Render callback for a plugin-registered custom panel.
// Called by the modloader each frame while the panel window is open.
// imgui: the function table above — use it for all ImGui calls.
typedef void (*PluginImGuiRenderCallback)(IModLoaderImGui* imgui);

// Descriptor passed to IPluginUIEvents::RegisterPanel.
struct PluginPanelDesc
{
    const char* buttonLabel;        // button label shown in ModLoader Config tab
    const char* windowTitle;        // ImGui window title (must be globally unique)
    PluginImGuiRenderCallback renderFn;
};

// Opaque handle returned by IPluginUIEvents::SetPanelOpen.
// Pass it to SetPanelClose to close the same panel.
// Null means the panel was not found or the UI backend is disabled.
typedef void* PanelHandle;

// Opaque handle returned by IPluginUIEvents::RegisterWidget.
// Pass it to UnregisterWidget during PluginShutdown.
// Null means registration failed or the UI backend is disabled.
typedef void* WidgetHandle;

// Descriptor passed to IPluginUIEvents::RegisterWidget.
struct PluginWidgetDesc
{
    const char* name;                   // ImGui window title (must be globally unique)
    PluginImGuiRenderCallback renderFn;
};

// Config-change notification — fired after the modloader UI writes a new value.
// section / key: the INI section and key that changed.
// newValue: the new value as a string (same representation as the INI file).
typedef void (*PluginConfigChangedCallback)(const char* section, const char* key, const char* newValue);

// ============================================================
// IPluginUIEvents — UI registration (v16, client only)
// Access via: hooks->UI->RegisterPanel(...)
//             hooks->UI->RegisterWidget(...)
// hooks->UI is nullptr on server and generic builds.
// ============================================================
struct IPluginUIEvents
{
    // Register a custom panel button + window.  desc must remain valid until Unregister.
    // Returns a PanelHandle that uniquely identifies this panel.
    // Store this handle — it is required for UnregisterPanel, SetPanelOpen, and SetPanelClose.
    // Returns null if registration failed (duplicate title, null fields, UI disabled).
    PanelHandle (*RegisterPanel)(const PluginPanelDesc* desc);
    // Unregister a panel using the handle returned by RegisterPanel.  Call during PluginShutdown.
    // Silently ignored if handle is null.
    void (*UnregisterPanel)(PanelHandle handle);
    // Receive a notification whenever the modloader UI writes a config value.
    void (*RegisterOnConfigChanged)(PluginConfigChangedCallback callback);
    void (*UnregisterOnConfigChanged)(PluginConfigChangedCallback callback);
    // Open a registered panel window using the handle returned by RegisterPanel.
    // Silently ignored if handle is null.
    void (*SetPanelOpen)(PanelHandle handle);
    // Close a registered panel window using the handle returned by RegisterPanel.
    // Silently ignored if handle is null.
    void (*SetPanelClose)(PanelHandle handle);
    // Register an always-on widget window rendered every frame.
    // desc and all strings it points to must remain valid until UnregisterWidget is called.
    // Returns a WidgetHandle that uniquely identifies this widget.
    // Returns null if registration failed (duplicate name, null fields, UI disabled).
    WidgetHandle (*RegisterWidget)(const PluginWidgetDesc* desc);
    // Unregister a widget using the handle returned by RegisterWidget.  Call during PluginShutdown.
    // Silently ignored if handle is null.
    void (*UnregisterWidget)(WidgetHandle handle);
    // Show or hide a widget window.  Widgets are visible by default after registration.
    // Use this to let players toggle a widget (e.g. via a keybind or config option).
    // Silently ignored if handle is null.
    void (*SetWidgetVisible)(WidgetHandle handle, bool visible);
};

// ============================================================
// IPluginHUDEvents — HUD hook subscriptions (v16, client only)
// Access via: hooks->HUD->RegisterOnPostRender(...)
// hooks->HUD is nullptr on server and generic builds — always null-check before use.
// ============================================================
struct IPluginHUDEvents
{
    // Register a per-frame callback fired after AHUD::PostRender.
    // The engine draws its own HUD before plugins are notified.
    // hud is the raw AHUD* — cast to SDK::AHUD* inside your callback.
    void      (*RegisterOnPostRender)(PluginHUDPostRenderCallback callback);
    void      (*UnregisterOnPostRender)(PluginHUDPostRenderCallback callback);
    // Resolved address of UCrMapManuSubsystem::GatherPlayersData, or 0 if not found.
    // Cast to a function pointer and call with the subsystem instance to force an
    // immediate refresh of PlayersMarkerDataContainer.
    uintptr_t (*GetGatherPlayersDataAddress)();
};

// ============================================================
// IPluginNetworkChannel — server-to-client plugin packet messaging (v17)
// Access via: hooks->Network->SendPacketToClient(...)
// hooks->Network is non-null on server AND client builds; nullptr on generic (Debug/Release) builds.
// Always null-check hooks->Network before use.
//
// Packet payloads must be plain-old-data (POD) structs with no pointers, no vtables,
// and no std containers.  The same plugin DLL running on both ends guarantees identical
// struct layout so plain memcpy serialization works without a schema.
//
// Prefer the typed wrappers in plugin_network_helpers.h (SendPacketToPlayer<T>,
// SendPacketToAllClients<T>, OnReceive<T>) over calling these functions directly.
// ============================================================
struct IPluginNetworkChannel
{
    // True when called from a server build; false on a client build.
    bool (*IsServer)();

    // Server-side: send a raw packet to a single player.
    // playerController : the APlayerController* for the target player (cast to void*)
    // self             : the calling plugin's IPluginSelf (name is used for packet routing)
    // typeTag          : arbitrary string identifying the packet type
    // data             : payload bytes; copied before the call returns
    // size             : byte count of the payload (max ~1 KB recommended)
    // No-op on client builds.
    void (*SendPacketToClient)(void* playerController, const IPluginSelf* self,
                               const char* typeTag, const uint8_t* data, size_t size);

    // Server-side: send a raw packet to all currently connected players.
    // self / typeTag / data / size: same semantics as SendPacketToClient.
    // No-op on client builds.
    void (*SendPacketToAllClients)(const IPluginSelf* self, const char* typeTag,
                                   const uint8_t* data, size_t size);

    // Client-side: register a handler for packets arriving with the given self->name+typeTag pair.
    // Multiple handlers for the same pair are supported.
    // No-op on server builds.
    void (*RegisterMessageHandler)(const IPluginSelf* self, const char* typeTag,
                                   PluginNetworkMessageCallback callback);

    // Client-side: unregister a previously registered handler.
    // Silently ignored if the handler was not registered.
    // No-op on server builds.
    void (*UnregisterMessageHandler)(const IPluginSelf* self, const char* typeTag,
                                     PluginNetworkMessageCallback callback);

    // v18 -- Client-side: send a raw packet to the server.
    // self / typeTag / data / size: same semantics as SendPacketToClient.
    // Transport: ACrPlayerControllerBase::ServerChatCommit (NetServer RPC).
    // No-op on server builds.
    void (*SendPacketToServer)(const IPluginSelf* self, const char* typeTag,
                               const uint8_t* data, size_t size);

    // v18 -- Server-side: register a handler for packets arriving from any client.
    // callback receives the sender's APlayerController* (cast to void*) along with
    // the standard pluginName/typeTag/data/size fields.
    // No-op on client builds.
    void (*RegisterServerMessageHandler)(const IPluginSelf* self, const char* typeTag,
                                         PluginNetworkServerMessageCallback callback);

    // v18 -- Server-side: unregister a previously registered server-message handler.
    // Silently ignored if the handler was not registered.
    // No-op on client builds.
    void (*UnregisterServerMessageHandler)(const IPluginSelf* self, const char* typeTag,
                                           PluginNetworkServerMessageCallback callback);

    // v18 -- Server-only: exclude a PlayerController from SendPacketToAllClients sends.
    // Intended for plugin-spawned controllers (e.g. fake/AI players) that have no real
    // network connection.  Sending to them is a silent no-op anyway, but registering an
    // exclusion avoids the wasted ProcessEvent call and log noise.
    // No-op on client builds.  Passing null is a no-op.
    void (*ExcludeFromBroadcast)(void* playerController);

    // v18 -- Server-only: undo a previous ExcludeFromBroadcast registration.
    // No-op on client builds or if the controller was not registered.
    void (*UnexcludeFromBroadcast)(void* playerController);
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
//   hooks->Input->RegisterKeybind(EModKey::F1, EModKeyEvent::Pressed, &MyKeyCb);
//   hooks->HUD->RegisterOnPostRender(&MyPostRenderCb);  // client only, null-check first
//   hooks->Network->SendPacketToAllClients(name, tag, data, size);  // server+client, null-check first
//   hooks->Engine->PostToGameThread(&MyFn, ctx);                    // any thread -> game thread (v18)
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
    IPluginInputEvents*  Input;     // v15 — keybind subscriptions (client only, null on server)
    IPluginUIEvents*     UI;        // v15 — custom panel registration + config-change callbacks (client only, null on server)
    IPluginHUDEvents*        HUD;     // v16 — AHUD::PostRender callbacks + HUD function addresses (client only, null on server)
    IPluginNetworkChannel*   Network; // v17 — plugin-to-plugin net channel (server+client builds; null on generic)
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

// ============================================================
// IPluginSelf — plugin identity + all service interfaces (v19)
// Passed to PluginInit. The pointer is stable for the plugin's
// entire lifetime — plugins may store it and use it for all
// subsequent calls to logger, config, scanner, and hooks.
// ============================================================
struct IPluginSelf
{
    const char*      name;     // from PluginInfo::name
    const char*      version;  // from PluginInfo::version
    IPluginLogger*   logger;
    IPluginConfig*   config;
    IPluginScanner*  scanner;
    IPluginHooks*    hooks;
};

// Plugin interface - all plugins must implement these functions
// These should be exported with extern "C" __declspec(dllexport)
typedef PluginInfo* (*GetPluginInfoFunc)();
typedef bool (*PluginInitFunc)(IPluginSelf* self);
typedef void (*PluginShutdownFunc)();

// Function names that plugins must export
#define PLUGIN_GET_INFO_FUNC_NAME "GetPluginInfo"
#define PLUGIN_INIT_FUNC_NAME "PluginInit"
#define PLUGIN_SHUTDOWN_FUNC_NAME "PluginShutdown"
