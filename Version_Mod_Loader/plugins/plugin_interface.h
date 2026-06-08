#pragma once

#include <windows.h>
#include <cstdint>

// v19: Introduced IPluginSelf.  MIN bumped to 19.
// v20: Added OnBeforeWorldEndPlay / OnAfterWorldEndPlay.  MIN remains 19.
// v21: Added IPluginNativePointers.  MIN remains 19.
// v22: Added IPluginHttpServer (hooks->HttpServer).
//   Static file route registration (AddRoute/RemoveRoute), raw-request
//      filter hook (RegisterOnRawRequest/UnregisterOnRawRequest), and
//  raw-response route registration (AddRawRoute/RemoveRawRoute).
//      URL scheme: /<pluginName>/<routeName>/...  (case-insensitive).
//      Static files are served from <exe_dir>\Plugins\<pluginName>\<folderName>\.
//      Raw-response routes let plugins handle arbitrary URL prefixes and write
//      any response body + content-type directly (e.g. JSON API endpoints).
//   Server builds only; nullptr on client/generic builds.
//      MIN remains 19.
// v23 - SR Hotfix update 22/04/2026 118961
// v24: Added scaling/metrics functions to IModLoaderImGui (GetFontSize, GetTextLineHeight,
//      GetTextLineHeightWithSpacing, GetFrameHeight, GetFrameHeightWithSpacing, CalcTextSize,
//      SetWindowFontScale, GetContentRegionAvail, GetDisplaySize).
//      Added PluginWindowHints struct and windowHints field to PluginWidgetDesc.
//      MIN remains 23.
// v31: Added extra_window_flags to PluginWindowHints.
//      Allows plugins to pass additional ImGuiWindowFlags (e.g. NoTitleBar, NoResize).
//      0 = no extra flags (default behaviour unchanged). MIN remains 26.
// v32: Added IPluginClientSessionInfo (client only, null on server/generic).
//      Exposes GetSessionOnlineMode, IsMultiplayer, IsServer query functions.
// v33: Added pluginTarget field to PluginInfo. Every plugin must now declare
//      PLUGIN_TARGET_CLIENT or PLUGIN_TARGET_SERVER. The loader rejects plugins
//      that don't match the current build target.
// v34: Game had an update, needed interface bump
// v35: Added layout/sidebar functions to IModLoaderImGui:
//      BeginChild, EndChild, PushStyleColor, PopStyleColor,
//      PushStyleVarFloat, PushStyleVarVec2, PopStyleVar,
//      PushItemWidth, PopItemWidth, SetCursorPosX, GetCursorPosX,
//      BeginTable, TableNextColumn, EndTable, IsItemClicked,
//      GetWindowWidth, GetWindowHeight, Dummy.
//      Also wired SetWindowFontScale which was declared but not populated.
//      MIN remains 34.
//      -- Mass expansion of IModLoaderImGui — added ~100 additional ImGui
//      functions covering: window queries, scroll, groups, extended cursor
//      control, all drag/slider variants, listbox, tab bar, menus, popups,
//      tooltips, full table API, item state predicates, disabled regions,
//      clip rect, mouse queries, color utilities, and misc helpers.
//      MIN remains 34.
//      --- Added MakeTextKey to IPluginTextUtils (trampoline for FTextKey::FTextKey),
//      letting plugins build the Namespace/Key FTextKey arguments required by
//      AsLocalizable_Advanced.  MIN remains 34.
// v36: Replaced IPluginClientSessionInfo with IPluginNetModeInfo (server + client,
//      null on generic). The old offset-based UCrSessionSubsystem reads were
//      replaced with an AOB-resolved trampoline to AActor::InternalGetNetMode,
//      exposed via the new EPluginNetMode enum (mirrors engine ENetMode) so
//      plugins can compare against typed names instead of raw integers.
//      MIN should be bumped, but no one is using it yet.
//      --- Added object/package lookup address resolvers to IPluginEngineEvents:
//      GetStaticFindObjectByPathAddress, GetStaticFindObjectByNameAddress,
//      GetStaticFindObjectSafeByPathAddress, GetStaticFindObjectSafeByNameAddress,
//      GetStaticFindObjectFastAddress, GetFindPackageAddress,
//      GetPackageFullyLoadAddress, GetLoadPackageAddress,
//      GetAssetDataFastGetAssetAddress -- AOB-resolved during early modloader
//      startup (Hooks::ObjectLookup), exposed as raw trampoline addresses.
//      MIN remains 34.
// v37: Added IPluginImGuiTextures (client only, null on server/generic).
//      Lets plugins load images from file or memory (WIC: PNG/JPG/BMP/GIF/TIFF),
//      from raw RGBA pixels, or directly from a live SDK::UTexture2D*, and render
//      them via Image/ImageButton without any direct D3D12 access.
//      Up to 64 textures may be live at once.
//      hooks->ImGuiTextures->{LoadFromFile, LoadFromMemory, LoadFromRGBA,
//      LoadFromUTexture2D, FreeTexture, GetSize, Image, ImageButton}.
//      LoadFromUTexture2D wraps the engine-owned GPU texture in-place (no copy).
//      The UTexture2D must remain alive while the handle is in use.
//      MIN remains 34.
// v38: IPluginImGuiTextures: raised texture slot cap 64 -> 2048.
//      Added GetFreeSlotCount(), GetCapacity().
//      Load functions now throw std::out_of_range when all slots are in use
//      (previously returned nullptr silently).
//      MIN remains 34.

#define PLUGIN_INTERFACE_VERSION_MIN 34
#define PLUGIN_INTERFACE_VERSION_MAX 38
#define PLUGIN_INTERFACE_VERSION 38

enum class PluginLogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };
enum class ConfigValueType { String, Integer, Float, Boolean, Keybind };

// Network mode — mirrors the engine's ENetMode enum, letting plugins compare
// against nicely-typed names instead of raw integers when querying GetNetMode().
//   Standalone      = solo/offline, no networking
//   DedicatedServer = headless server, no local players
//   ListenServer    = server with a local player
//   Client          = connected to a remote server
enum class EPluginNetMode : uint8_t
{
    Standalone      = 0,
    DedicatedServer = 1,
    ListenServer    = 2,
    Client          = 3,
    Unknown         = 255
};

struct ConfigEntry
{
	const char* section;
	const char* key;
	ConfigValueType type;
	const char* defaultValue;
	const char* description;
	float rangeMin;
	float rangeMax;
};

struct ConfigSchema { const ConfigEntry* entries; int entryCount; };

struct IPluginSelf; // forward — defined after all sub-interfaces

struct IPluginLogger
{
	void (*Log)(PluginLogLevel level, const IPluginSelf* self, const char* message);
	void (*Trace)(const IPluginSelf* self, const char* format, ...);
	void (*Debug)(const IPluginSelf* self, const char* format, ...);
	void (*Info) (const IPluginSelf* self, const char* format, ...);
	void (*Warn) (const IPluginSelf* self, const char* format, ...);
	void (*Error)(const IPluginSelf* self, const char* format, ...);
};

struct IPluginConfig
{
	bool  (*ReadString)(const IPluginSelf* self, const char* section, const char* key, char* outValue, int maxLen, const char* defaultValue);
	bool  (*WriteString)(const IPluginSelf* self, const char* section, const char* key, const char* value);
	int   (*ReadInt)(const IPluginSelf* self, const char* section, const char* key, int defaultValue);
	bool  (*WriteInt)(const IPluginSelf* self, const char* section, const char* key, int value);
	float (*ReadFloat)(const IPluginSelf* self, const char* section, const char* key, float defaultValue);
	bool  (*WriteFloat)(const IPluginSelf* self, const char* section, const char* key, float value);
	bool  (*ReadBool)(const IPluginSelf* self, const char* section, const char* key, bool defaultValue);
	bool  (*WriteBool)(const IPluginSelf* self, const char* section, const char* key, bool value);
	bool  (*InitializeFromSchema)(const IPluginSelf* self, const ConfigSchema* schema);
	void  (*ValidateConfig)(const IPluginSelf* self, const ConfigSchema* schema);
};

struct PluginXRef { uintptr_t address; bool isRelative; };

struct IPluginScanner
{
	uintptr_t (*FindPatternInMainModule)(const char* pattern);
	uintptr_t (*FindPatternInModule)(HMODULE module, const char* pattern);
	int (*FindAllPatternsInMainModule)(const char* pattern, uintptr_t* outAddresses, int maxResults);
	int (*FindAllPatternsInModule)(HMODULE module, const char* pattern, uintptr_t* outAddresses, int maxResults);
	uintptr_t (*FindUniquePattern)(const char** patterns, int patternCount, int* outPatternIndex);
	int (*FindXrefsToAddress)(uintptr_t targetAddress, uintptr_t start, size_t size, PluginXRef* outXRefs, int maxResults);
	int (*FindXrefsToAddressInModule)(uintptr_t targetAddress, HMODULE module, PluginXRef* outXRefs, int maxResults);
	int (*FindXrefsToAddressInMainModule)(uintptr_t targetAddress, PluginXRef* outXRefs, int maxResults);
};

typedef void* HookHandle;
namespace SDK { class UWorld; class UTexture2D; }

// ---------------------------------------------------------------------------
// Callback typedefs (v14+)
// ---------------------------------------------------------------------------
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
typedef void (*PluginHUDPostRenderCallback)(void* hud);
typedef void (*PluginNetworkMessageCallback)(const char* pluginName, const char* typeTag, const uint8_t* data, size_t size);
typedef void (*PluginNetworkServerMessageCallback)(void* senderPlayerController, const char* pluginName, const char* typeTag, const uint8_t* data, size_t size);
typedef void (*PluginGameThreadCallback)(void* context);

typedef bool (*PluginBeforeActivateSpawnerCallback)(void* spawner, bool bDisableAggroLock);
typedef void (*PluginAfterActivateSpawnerCallback)(void* spawner, bool bDisableAggroLock);
typedef bool (*PluginBeforeDeactivateSpawnerCallback)(void* spawner, bool bPermanently);
typedef void (*PluginAfterDeactivateSpawnerCallback)(void* spawner, bool bPermanently);
typedef bool (*PluginBeforeDoSpawningCallback)(void* spawner);
typedef void (*PluginAfterDoSpawningCallback)(void* spawner);

// ---------------------------------------------------------------------------
// Sub-interface structs (v14)
// ---------------------------------------------------------------------------
struct IPluginHookUtils
{
	HookHandle (*Install)(uintptr_t targetAddress, void* detourFunction, void** originalFunction);
	void       (*Remove)(HookHandle handle);
	bool       (*IsInstalled)(HookHandle handle);
};

struct IPluginMemoryUtils
{
	bool  (*Patch)(uintptr_t address, const uint8_t* data, size_t size);
	bool  (*Nop)(uintptr_t address, size_t size);
	bool  (*Read)(uintptr_t address, void* buffer, size_t size);
	void* (*Alloc)(size_t count, uint32_t alignment);
	void  (*Free)(void* ptr);
	bool  (*IsAllocatorAvailable)();
};

struct IPluginEngineEvents
{
	void      (*RegisterOnInit)(PluginEngineInitCallback);
	void      (*UnregisterOnInit)(PluginEngineInitCallback);
	void      (*RegisterOnShutdown)(PluginEngineShutdownCallback);
	void      (*UnregisterOnShutdown)(PluginEngineShutdownCallback);
	void      (*RegisterOnTick)(PluginEngineTickCallback);
	void    (*UnregisterOnTick)(PluginEngineTickCallback);
	uintptr_t (*GetStaticLoadObjectAddress)();           // v16
	void      (*PostToGameThread)(PluginGameThreadCallback fn, void* context); // v18

	// v36 -- resolved addresses of CoreUObject object/package lookup and loading
	// functions, scanned during early modloader startup. Each returns 0 if the
	// AOB scan failed to resolve the function on the running build.
	//
	//   GetStaticFindObjectByPathAddress     -> UObject* __fastcall StaticFindObject(UClass*, FTopLevelAssetPath*, bool)
	//   GetStaticFindObjectByNameAddress     -> UObject* __fastcall StaticFindObject(UClass*, UObject*, const wchar_t*, bool)
	//   GetStaticFindObjectSafeByPathAddress -> UObject* __fastcall StaticFindObjectSafe(UClass*, FTopLevelAssetPath*, bool)
	//   GetStaticFindObjectSafeByNameAddress -> UObject* __fastcall StaticFindObjectSafe(UClass*, UObject*, const wchar_t*, bool)
	//   GetStaticFindObjectFastAddress       -> UObject* __fastcall StaticFindObjectFast(UClass*, UObject*, FName, bool, EObjectFlags, EInternalObjectFlags)
	//   GetFindPackageAddress                -> UPackage* __fastcall FindPackage(UObject*, const wchar_t*)
	//   GetPackageFullyLoadAddress           -> void __fastcall UPackage::FullyLoad(UPackage*)
	//   GetLoadPackageAddress                -> UPackage* __fastcall LoadPackage(UPackage*, FScriptContainerElement*, unsigned int, FArchive*, const FLinkerInstancingContext*)
	//   GetAssetDataFastGetAssetAddress      -> UObject* __fastcall FAssetData::FastGetAsset(FAssetData*, bool, TMap<FName,FName,FDefaultSetAllocator,TDefaultMapHashableKeyFuncs<FName,FName,0>>*)
	uintptr_t (*GetStaticFindObjectByPathAddress)();
	uintptr_t (*GetStaticFindObjectByNameAddress)();
	uintptr_t (*GetStaticFindObjectSafeByPathAddress)();
	uintptr_t (*GetStaticFindObjectSafeByNameAddress)();
	uintptr_t (*GetStaticFindObjectFastAddress)();
	uintptr_t (*GetFindPackageAddress)();
	uintptr_t (*GetPackageFullyLoadAddress)();
	uintptr_t (*GetLoadPackageAddress)();
	uintptr_t (*GetAssetDataFastGetAssetAddress)();
};

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
	void (*RegisterOnBeforeWorldEndPlay)(PluginWorldEndPlayCallback);   // v20
	void (*UnregisterOnBeforeWorldEndPlay)(PluginWorldEndPlayCallback); // v20
	void (*RegisterOnAfterWorldEndPlay)(PluginWorldEndPlayCallback);    // v20
	void (*UnregisterOnAfterWorldEndPlay)(PluginWorldEndPlayCallback);  // v20
};

struct IPluginPlayerEvents
{
	void (*RegisterOnPlayerJoined)(PluginPlayerJoinedCallback);
	void (*UnregisterOnPlayerJoined)(PluginPlayerJoinedCallback);
	void (*RegisterOnPlayerLeft)(PluginPlayerLeftCallback);
	void (*UnregisterOnPlayerLeft)(PluginPlayerLeftCallback);
};

struct IPluginActorEvents
{
	void (*RegisterOnActorBeginPlay)(PluginActorBeginPlayCallback);
	void (*UnregisterOnActorBeginPlay)(PluginActorBeginPlayCallback);
};

struct IPluginSpawnerHooks
{
	void (*RegisterOnBeforeActivate)(PluginBeforeActivateSpawnerCallback callback);
	void (*UnregisterOnBeforeActivate)(PluginBeforeActivateSpawnerCallback callback);
	void (*RegisterOnAfterActivate)(PluginAfterActivateSpawnerCallback callback);
	void (*UnregisterOnAfterActivate)(PluginAfterActivateSpawnerCallback callback);
	void (*RegisterOnBeforeDeactivate)(PluginBeforeDeactivateSpawnerCallback callback);
	void (*UnregisterOnBeforeDeactivate)(PluginBeforeDeactivateSpawnerCallback callback);
	void (*RegisterOnAfterDeactivate)(PluginAfterDeactivateSpawnerCallback callback);
	void (*UnregisterOnAfterDeactivate)(PluginAfterDeactivateSpawnerCallback callback);
	void (*RegisterOnBeforeDoSpawning)(PluginBeforeDoSpawningCallback callback);
	void (*UnregisterOnBeforeDoSpawning)(PluginBeforeDoSpawningCallback callback);
	void (*RegisterOnAfterDoSpawning)(PluginAfterDoSpawningCallback callback);
	void (*UnregisterOnAfterDoSpawning)(PluginAfterDoSpawningCallback callback);
};

// ---------------------------------------------------------------------------
// Input (v15, client only)
// ---------------------------------------------------------------------------
enum class EModKey : uint32_t
{
	F1 = 0, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
	A, B, C, D, E, F, G, H, I, J, K, L, M,
	N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
	Zero, One, Two, Three, Four, Five, Six, Seven, Eight, Nine,
	Escape, Tab, CapsLock, SpaceBar, Enter, BackSpace, Delete, Insert,
	LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt,
	Up, Down, Left, Right, Home, End, PageUp, PageDown,
	Tilde, Hyphen, Equals, LeftBracket, RightBracket, Backslash,
	Semicolon, Apostrophe, Comma, Period, Slash,
	NumPadZero, NumPadOne, NumPadTwo, NumPadThree, NumPadFour,
	NumPadFive, NumPadSix, NumPadSeven, NumPadEight, NumPadNine,
	Add, Subtract, Multiply, Divide, Decimal,
	LeftMouseButton, RightMouseButton, MiddleMouseButton,
	ThumbMouseButton, ThumbMouseButton2,
	Unknown
};

enum class EModKeyEvent : uint32_t { Pressed = 0, Released = 1 };

// Modifier bitmask used with combo keybinds (v28).
// Each flag covers both Left and Right variants of that modifier key.
enum EModKeyModifiers : uint32_t
{
	EModKeyMod_None  = 0,
	EModKeyMod_Ctrl  = 1 << 0,
	EModKeyMod_Shift = 1 << 1,
	EModKeyMod_Alt   = 1 << 2,
};

typedef void (*PluginKeybindCallback)(EModKey key, EModKeyEvent event);

// Combo callback — receives the base key, the modifier bitmask held at the
// moment of the transition, and the event type (v28).
typedef void (*PluginKeybindComboCallback)(EModKey key, EModKeyModifiers mods, EModKeyEvent event);

struct IPluginInputEvents
{
	// v15 — register by enum; fires on key transition regardless of modifier state.
	void (*RegisterKeybind)(EModKey key, EModKeyEvent event, PluginKeybindCallback callback);
	void (*UnregisterKeybind)(EModKey key, EModKeyEvent event, PluginKeybindCallback callback);

	// v15/v30 — register by name string.  Accepts plain key names ("F5") and
	// combo strings ("Ctrl+C", "Shift+F5", "Ctrl+Shift+Delete").  Modifier tokens
	// are case-insensitive.  The callback receives the base key and event; the
	// modifiers are implicit in the name you registered.
	// The modloader tracks these registrations and automatically re-registers
	// the keybind when the user rebinds it in the plugin config UI.
	void (*RegisterKeybindByName)(const char* combo, EModKeyEvent event, PluginKeybindCallback callback);
	void (*UnregisterKeybindByName)(const char* combo, EModKeyEvent event, PluginKeybindCallback callback);

	// v28 — advanced: register by enum + explicit modifier mask.  Fires only when
	// the key transitions with exactly those modifiers held.  The callback receives
	// the modifier mask at fire time.  Use this only when you need the mods passed
	// back; most plugins should use RegisterKeybindByName instead.
	void (*RegisterKeybindCombo)(EModKey key, EModKeyModifiers mods, EModKeyEvent event, PluginKeybindComboCallback callback);
	void (*UnregisterKeybindCombo)(EModKey key, EModKeyModifiers mods, EModKeyEvent event, PluginKeybindComboCallback callback);
};

// ---------------------------------------------------------------------------
// UI (v15–v16, client only; extended v24)
// ---------------------------------------------------------------------------
struct IModLoaderImGui
{
	void (*Text)(const char* text);
	void (*TextColored)(float r, float g, float b, float a, const char* text);
	void (*TextDisabled)(const char* text);
	void (*TextWrapped)(const char* text);
	void (*LabelText)(const char* label, const char* text);
	void (*SeparatorText)(const char* label);
	bool (*InputText)(const char* label, char* buf, size_t buf_size);
	bool (*InputInt)(const char* label, int* v, int step, int step_fast);
	bool (*InputFloat)(const char* label, float* v, float step, float step_fast, const char* format);
	bool (*Checkbox)(const char* label, bool* v);
	bool (*SliderFloat)(const char* label, float* v, float v_min, float v_max, const char* format);
	bool (*SliderInt)(const char* label, int* v, int v_min, int v_max, const char* format);
	bool (*Button)(const char* label);
	bool (*SmallButton)(const char* label);
	void (*SameLine)(float offset_from_start_x, float spacing);
	void (*NewLine)();
	void (*Separator)();
	void (*Spacing)();
	void (*Indent)(float indent_w);
	void (*Unindent)(float indent_w);
	void (*PushIDStr)(const char* str_id);
	void (*PushIDInt)(int int_id);
	void (*PopID)();
	bool (*BeginCombo)(const char* label, const char* preview_value);
	bool (*Selectable)(const char* label, bool selected);
	void (*EndCombo)();
	bool (*CollapsingHeader)(const char* label);
	bool (*TreeNodeStr)(const char* label);
	void (*TreePop)();
	bool (*ColorEdit3)(const char* label, float col[3]);
	bool (*ColorEdit4)(const char* label, float col[4]);
	void (*SetTooltip)(const char* text);
	bool (*IsItemHovered)();
	void (*SetNextItemWidth)(float item_width);

	// Font / text metrics
	float (*GetFontSize)();
	float (*GetTextLineHeight)();
	float (*GetTextLineHeightWithSpacing)();
	float (*GetFrameHeight)();
	float (*GetFrameHeightWithSpacing)();
	void  (*CalcTextSize)(const char* text, float* out_x, float* out_y, bool hide_text_after_double_hash, float wrap_width);

	// Per-window font scale (call inside your render callback)
	void  (*SetWindowFontScale)(float scale);

	// Content / display size queries
	void  (*GetContentRegionAvail)(float* out_x, float* out_y);
	void  (*GetDisplaySize)(float* out_x, float* out_y);

	// v35: Child windows
	bool (*BeginChild)(const char* id, float size_x, float size_y, bool border);
	void (*EndChild)();

	// v35: Style color / var stack
	void (*PushStyleColor)(int idx, float r, float g, float b, float a);
	void (*PopStyleColor)(int count);
	void (*PushStyleVarFloat)(int idx, float val);
	void (*PushStyleVarVec2)(int idx, float x, float y);
	void (*PopStyleVar)(int count);

	// v35: Item width / cursor control
	void  (*PushItemWidth)(float item_width);
	void  (*PopItemWidth)();
	void  (*SetCursorPosX)(float x);
	float (*GetCursorPosX)();

	// v35: Table layout
	bool (*BeginTable)(const char* id, int columns, int flags);
	void (*TableNextColumn)();
	void (*EndTable)();

	// v35: Misc queries
	bool  (*IsItemClicked)(int mouse_button);
	float (*GetWindowWidth)();
	float (*GetWindowHeight)();
	void  (*Dummy)(float size_x, float size_y);

	// -------------------------------------------------------------------------
	// v36: Window queries
	// -------------------------------------------------------------------------
	bool  (*IsWindowAppearing)();
	bool  (*IsWindowCollapsed)();
	bool  (*IsWindowFocused)(int flags);
	bool  (*IsWindowHovered)(int flags);
	void  (*GetWindowPos)(float* out_x, float* out_y);
	void  (*GetWindowSize)(float* out_x, float* out_y);
	void  (*SetNextWindowBgAlpha)(float alpha);

	// v36: Scroll
	float (*GetScrollX)();
	float (*GetScrollY)();
	void  (*SetScrollX)(float scroll_x);
	void  (*SetScrollY)(float scroll_y);
	float (*GetScrollMaxX)();
	float (*GetScrollMaxY)();
	void  (*SetScrollHereX)(float center_x_ratio);
	void  (*SetScrollHereY)(float center_y_ratio);

	// v36: Grouping / alignment
	void  (*BeginGroup)();
	void  (*EndGroup)();
	void  (*AlignTextToFramePadding)();

	// v36: Extended cursor control
	float (*GetCursorPosY)();
	void  (*SetCursorPosY)(float y);
	void  (*SetCursorPos)(float x, float y);
	void  (*GetCursorScreenPos)(float* out_x, float* out_y);
	void  (*SetCursorScreenPos)(float x, float y);
	void  (*GetCursorStartPos)(float* out_x, float* out_y);
	float (*CalcItemWidth)();
	void  (*PushTextWrapPos)(float wrap_local_pos_x);
	void  (*PopTextWrapPos)();

	// v36: Style extras (PushStyleVarX/Y from ImGui 1.87+)
	void  (*PushStyleVarX)(int idx, float val_x);
	void  (*PushStyleVarY)(int idx, float val_y);
	void  (*PushItemFlag)(int option, bool enabled);
	void  (*PopItemFlag)();

	// v36: Text helpers
	void  (*BulletText)(const char* text);
	void  (*Bullet)();

	// v36: Buttons / widgets
	bool  (*ButtonSized)(const char* label, float w, float h);
	bool  (*InvisibleButton)(const char* str_id, float w, float h);
	bool  (*ArrowButton)(const char* str_id, int dir);
	bool  (*RadioButton)(const char* label, bool active);
	bool  (*RadioButtonInt)(const char* label, int* v, int v_button);
	void  (*ProgressBar)(float fraction, float w, float h, const char* overlay);
	bool  (*CheckboxFlagsInt)(const char* label, int* flags, int flags_value);
	bool  (*SelectableFull)(const char* label, bool selected, int flags, float w, float h);
	bool  (*TextLink)(const char* label);

	// v36: Input extras
	bool  (*InputTextMultiline)(const char* label, char* buf, size_t buf_size, float w, float h);
	bool  (*InputTextWithHint)(const char* label, const char* hint, char* buf, size_t buf_size);
	bool  (*InputFloat2)(const char* label, float v[2]);
	bool  (*InputFloat3)(const char* label, float v[3]);
	bool  (*InputFloat4)(const char* label, float v[4]);
	bool  (*InputInt2)(const char* label, int v[2]);
	bool  (*InputInt3)(const char* label, int v[3]);
	bool  (*InputInt4)(const char* label, int v[4]);

	// v36: Drag widgets
	bool  (*DragFloat)(const char* label, float* v, float v_speed, float v_min, float v_max, const char* format);
	bool  (*DragFloat2)(const char* label, float v[2], float v_speed, float v_min, float v_max, const char* format);
	bool  (*DragFloat3)(const char* label, float v[3], float v_speed, float v_min, float v_max, const char* format);
	bool  (*DragFloat4)(const char* label, float v[4], float v_speed, float v_min, float v_max, const char* format);
	bool  (*DragInt)(const char* label, int* v, float v_speed, int v_min, int v_max, const char* format);
	bool  (*DragInt2)(const char* label, int v[2], float v_speed, int v_min, int v_max, const char* format);
	bool  (*DragInt3)(const char* label, int v[3], float v_speed, int v_min, int v_max, const char* format);
	bool  (*DragInt4)(const char* label, int v[4], float v_speed, int v_min, int v_max, const char* format);
	bool  (*DragFloatRange2)(const char* label, float* v_min, float* v_max, float speed, float mn, float mx, const char* format);
	bool  (*DragIntRange2)(const char* label, int* v_min, int* v_max, float speed, int mn, int mx, const char* format);

	// v36: Vertical sliders + angle slider
	bool  (*VSliderFloat)(const char* label, float w, float h, float* v, float v_min, float v_max, const char* format);
	bool  (*VSliderInt)(const char* label, float w, float h, int* v, int v_min, int v_max, const char* format);
	bool  (*SliderAngle)(const char* label, float* v_rad, float deg_min, float deg_max);

	// v36: Color pickers / button
	bool  (*ColorPicker3)(const char* label, float col[3]);
	bool  (*ColorPicker4)(const char* label, float col[4]);
	bool  (*ColorButton)(const char* desc_id, float r, float g, float b, float a, float w, float h);

	// v36: Plot
	void  (*PlotLines)(const char* label, const float* values, int values_count, int values_offset, const char* overlay, float scale_min, float scale_max, float graph_w, float graph_h);
	void  (*PlotHistogram)(const char* label, const float* values, int values_count, int values_offset, const char* overlay, float scale_min, float scale_max, float graph_w, float graph_h);

	// v36: Tree extras
	bool  (*TreeNodeExStr)(const char* label, int flags);
	void  (*TreePushStr)(const char* str_id);
	float (*GetTreeNodeToLabelSpacing)();
	void  (*SetNextItemOpen)(bool is_open, int cond);

	// v36: Listbox
	bool  (*BeginListBox)(const char* label, float w, float h);
	void  (*EndListBox)();

	// v36: Tab bar
	bool  (*BeginTabBar)(const char* str_id, int flags);
	void  (*EndTabBar)();
	bool  (*BeginTabItem)(const char* label, bool* p_open, int flags);
	void  (*EndTabItem)();
	bool  (*TabItemButton)(const char* label, int flags);

	// v36: Menu bar / menus / menu items
	bool  (*BeginMenuBar)();
	void  (*EndMenuBar)();
	bool  (*BeginMenu)(const char* label, bool enabled);
	void  (*EndMenu)();
	bool  (*MenuItem)(const char* label, const char* shortcut, bool selected, bool enabled);

	// v36: Popups
	bool  (*BeginPopup)(const char* str_id, int flags);
	bool  (*BeginPopupModal)(const char* name, bool* p_open, int flags);
	void  (*EndPopup)();
	void  (*OpenPopup)(const char* str_id, int popup_flags);
	void  (*CloseCurrentPopup)();
	bool  (*BeginPopupContextItem)(const char* str_id, int popup_flags);
	bool  (*BeginPopupContextWindow)(const char* str_id, int popup_flags);
	bool  (*IsPopupOpen)(const char* str_id, int flags);

	// v36: Tooltips
	bool  (*BeginTooltip)();
	void  (*EndTooltip)();
	bool  (*BeginItemTooltip)();
	void  (*SetItemTooltip)(const char* text);

	// v36: Table extras
	void  (*TableNextRow)(int row_flags, float min_row_height);
	bool  (*TableSetColumnIndex)(int column_n);
	void  (*TableSetupColumn)(const char* label, int flags, float init_width);
	void  (*TableSetupScrollFreeze)(int cols, int rows);
	void  (*TableHeadersRow)();
	void  (*TableHeader)(const char* label);
	int   (*TableGetColumnCount)();
	int   (*TableGetColumnIndex)();
	int   (*TableGetRowIndex)();
	const char* (*TableGetColumnName)(int column_n);
	void  (*TableSetBgColor)(int target, unsigned int color, int column_n);

	// v36: Item state predicates
	bool  (*IsItemActive)();
	bool  (*IsItemFocused)();
	bool  (*IsItemVisible)();
	bool  (*IsItemEdited)();
	bool  (*IsItemActivated)();
	bool  (*IsItemDeactivated)();
	bool  (*IsItemDeactivatedAfterEdit)();
	bool  (*IsItemToggledOpen)();
	bool  (*IsAnyItemHovered)();
	bool  (*IsAnyItemActive)();
	bool  (*IsAnyItemFocused)();
	void  (*GetItemRectMin)(float* out_x, float* out_y);
	void  (*GetItemRectMax)(float* out_x, float* out_y);
	void  (*GetItemRectSize)(float* out_x, float* out_y);
	void  (*SetItemDefaultFocus)();
	void  (*SetKeyboardFocusHere)(int offset);
	void  (*SetNextItemAllowOverlap)();

	// v36: Disabled regions
	void  (*BeginDisabled)(bool disabled);
	void  (*EndDisabled)();

	// v36: Clip rect
	void  (*PushClipRect)(float min_x, float min_y, float max_x, float max_y, bool intersect_current);
	void  (*PopClipRect)();

	// v36: Mouse queries
	bool  (*IsMouseDown)(int button);
	bool  (*IsMouseClicked)(int button, bool repeat);
	bool  (*IsMouseReleased)(int button);
	bool  (*IsMouseDoubleClicked)(int button);
	void  (*GetMousePos)(float* out_x, float* out_y);
	bool  (*IsMouseDragging)(int button, float lock_threshold);
	void  (*GetMouseDragDelta)(int button, float lock_threshold, float* out_x, float* out_y);
	void  (*ResetMouseDragDelta)(int button);
	void  (*SetMouseCursor)(int cursor_type);
	bool  (*IsMouseHoveringRect)(float min_x, float min_y, float max_x, float max_y, bool clip);

	// v36: Color utilities
	unsigned int (*GetColorU32FromCol)(int idx, float alpha_mul);
	unsigned int (*GetColorU32FromVec4)(float r, float g, float b, float a);
	void         (*GetStyleColorVec4)(int idx, float* out_r, float* out_g, float* out_b, float* out_a);
	void         (*ColorConvertRGBtoHSV)(float r, float g, float b, float* out_h, float* out_s, float* out_v);
	void         (*ColorConvertHSVtoRGB)(float h, float s, float v, float* out_r, float* out_g, float* out_b);

	// v36: Misc
	double        (*GetTime)();
	int           (*GetFrameCount)();
	bool          (*IsRectVisible)(float w, float h);
	const char*   (*GetClipboardText)();
	void          (*SetClipboardText)(const char* text);
	const char*   (*GetStyleColorName)(int idx);
};

typedef void (*PluginImGuiRenderCallback)(IModLoaderImGui* imgui);

// ---------------------------------------------------------------------------
// Texture API (v37, client only)
// ---------------------------------------------------------------------------

// Opaque handle returned by the texture load functions. NULL = invalid/failed.
typedef void* PluginTextureHandle;

// Interface for loading and rendering images without direct D3D12 access.
// Retrieved via hooks->ImGuiTextures (client only; nullptr on server/generic).
//
// Up to GetCapacity() textures may be live at once (256 as of v38).
// Load before or during rendering; free when no longer needed.
// Image/ImageButton must be called from inside a plugin render callback
// while an ImGui frame is in progress.
//
// All Load* functions throw std::out_of_range if no slots are available.
// They return NULL (without throwing) only when D3D12 is not yet ready or
// when the specific resource cannot be loaded (bad file, null texture, etc.).
// Call GetFreeSlotCount() before loading if you need to handle a full registry
// without catching exceptions.
struct IPluginImGuiTextures
{
    // Load from a UTF-8 file path. Supports PNG, JPG, BMP, GIF, TIFF (via WIC).
    // Returns NULL if D3D12 is not ready yet or if decoding fails.
    // Throws std::out_of_range if all slots are in use.
    PluginTextureHandle (*LoadFromFile)(const char* utf8_path);

    // Load from encoded image bytes in memory (same formats as LoadFromFile).
    // data must remain valid only for the duration of this call.
    // Throws std::out_of_range if all slots are in use.
    PluginTextureHandle (*LoadFromMemory)(const void* data, size_t size);

    // Load from raw 32-bit RGBA pixels (4 bytes/pixel, top-left row-major).
    // data must remain valid only for the duration of this call.
    // Throws std::out_of_range if all slots are in use.
    PluginTextureHandle (*LoadFromRGBA)(const unsigned char* rgba, int width, int height);

    // Wrap a live UTexture2D in an ImGui handle without copying pixels.
    // Creates an SRV directly over the engine-owned GPU resource.
    // The UTexture2D must remain valid and GPU-resident for as long as the
    // handle is in use. FreeTexture releases the SRV slot but does NOT
    // release the underlying D3D12 resource (the engine owns it).
    // Returns NULL if D3D12 is not ready or the texture has no GPU resource yet.
    // Throws std::out_of_range if all slots are in use.
    PluginTextureHandle (*LoadFromUTexture2D)(SDK::UTexture2D* texture);

    // Release the texture and free its slot. Safe to call with NULL.
    // Do not call while the texture may still be rendered on the GPU.
    void (*FreeTexture)(PluginTextureHandle handle);

    // Query the texture's natural dimensions. Either out pointer may be NULL.
    void (*GetSize)(PluginTextureHandle handle, int* out_width, int* out_height);

    // Render the texture at the current ImGui cursor position.
    // Pass width=0, height=0 to use the texture's natural size.
    void (*Image)(PluginTextureHandle handle, float width, float height);

    // Clickable image button. Returns true if clicked.
    // Pass width=0, height=0 to use the texture's natural size.
    bool (*ImageButton)(const char* str_id, PluginTextureHandle handle, float width, float height);

    // Returns the number of texture slots not currently in use. (v38)
    int (*GetFreeSlotCount)();

    // Returns the total texture slot capacity. (v38)
    int (*GetCapacity)();
};

// Flags for PluginWindowHints::extra_window_flags (v31).
// Values mirror ImGuiWindowFlags so plugins do not need imgui.h.
#define PluginWindowFlags_NoTitleBar        (1 << 0)
#define PluginWindowFlags_NoResize          (1 << 1)
#define PluginWindowFlags_NoMove            (1 << 2)
#define PluginWindowFlags_NoScrollbar       (1 << 3)
#define PluginWindowFlags_NoBackground      (1 << 7)
#define PluginWindowFlags_NoSavedSettings   (1 << 8)
#define PluginWindowFlags_NoMouseInputs     (1 << 9)

// Optional size/position hints for RegisterWidget windows.
// Set width/height to 0 for no size hint. Set pos_x/pos_y to -1 to skip positioning.
// size_cond / pos_cond: 0 = Always, 1 = FirstUseEver.
struct PluginWindowHints
{
	float width;
	float height;
	float pos_x;
	float pos_y;
	float pivot_x;
	float pivot_y;
	int   size_cond;
	int   pos_cond;
	int   extra_window_flags;  // v31: OR'd into ImGuiWindowFlags; 0 = default
};

struct PluginPanelDesc
{
	const char* buttonLabel;
	const char* windowTitle;
	PluginImGuiRenderCallback renderFn;
};

typedef void* PanelHandle;
typedef void* WidgetHandle;

struct PluginWidgetDesc
{
	const char*               name;
	PluginImGuiRenderCallback renderFn;
	const PluginWindowHints*  windowHints;  // optional, may be nullptr
};

typedef void (*PluginConfigChangedCallback)(const char* section, const char* key, const char* newValue);

struct IPluginUIEvents
{
	PanelHandle  (*RegisterPanel)(const PluginPanelDesc* desc);
	void         (*UnregisterPanel)(PanelHandle handle);
	void         (*RegisterOnConfigChanged)(PluginConfigChangedCallback callback);
	void         (*UnregisterOnConfigChanged)(PluginConfigChangedCallback callback);
	void         (*SetPanelOpen)(PanelHandle handle);
	void       (*SetPanelClose)(PanelHandle handle);
	WidgetHandle (*RegisterWidget)(const PluginWidgetDesc* desc);      // v16
	void      (*UnregisterWidget)(WidgetHandle handle);   // v16
	void         (*SetWidgetVisible)(WidgetHandle handle, bool visible); // v16
};

struct IPluginHUDEvents
{
	void      (*RegisterOnPostRender)(PluginHUDPostRenderCallback callback);
	void  (*UnregisterOnPostRender)(PluginHUDPostRenderCallback callback);
	uintptr_t (*GetGatherPlayersDataAddress)();
};

// ---------------------------------------------------------------------------
// Network channel (v17–v18)
// ---------------------------------------------------------------------------
struct IPluginNetworkChannel
{
	bool (*IsServer)();
	void (*SendPacketToClient)(void* playerController, const IPluginSelf* self, const char* typeTag, const uint8_t* data, size_t size);
	void (*SendPacketToAllClients)(const IPluginSelf* self, const char* typeTag, const uint8_t* data, size_t size);
	void (*RegisterMessageHandler)(const IPluginSelf* self, const char* typeTag, PluginNetworkMessageCallback callback);
	void (*UnregisterMessageHandler)(const IPluginSelf* self, const char* typeTag, PluginNetworkMessageCallback callback);
	void (*SendPacketToServer)(const IPluginSelf* self, const char* typeTag, const uint8_t* data, size_t size);  // v18
	void (*RegisterServerMessageHandler)(const IPluginSelf* self, const char* typeTag, PluginNetworkServerMessageCallback callback); // v18
	void (*UnregisterServerMessageHandler)(const IPluginSelf* self, const char* typeTag, PluginNetworkServerMessageCallback callback); // v18
	void (*ExcludeFromBroadcast)(void* playerController);    // v18
	void (*UnexcludeFromBroadcast)(void* playerController);  // v18
};

// ---------------------------------------------------------------------------
// Native pointers (v21)
// ---------------------------------------------------------------------------
struct IPluginNativePointers
{
	uintptr_t (*EngineLoopInit)();
	uintptr_t (*GameEngineInit)();
	uintptr_t (*EngineLoopExit)();
	uintptr_t (*EnginePreExit)();
	uintptr_t (*EngineTick)();
	uintptr_t (*WorldBeginPlay)();
	uintptr_t (*WorldEndPlay)();
	uintptr_t (*SaveLoaded)();
	uintptr_t (*ExperienceLoadComplete)();
	uintptr_t (*ActorBeginPlay)();
	uintptr_t (*PlayerJoined)();
	uintptr_t (*PlayerLeft)();
	uintptr_t (*SpawnerActivate)();
	uintptr_t (*SpawnerDeactivate)();
	uintptr_t (*SpawnerDoSpawning)();
	uintptr_t (*HUDPostRender)();   // client only (nullptr on server/generic)
	uintptr_t (*ClientMessageExec)();  // client only (nullptr on server/generic)
};

// ---------------------------------------------------------------------------
// Text utilities
// ---------------------------------------------------------------------------
struct IPluginTextUtils
{
	// Trampoline address of the original FText::AsLocalizable_Advanced, or 0 if the hook
	// failed to install. Cast to:
	//   FText*(__fastcall*)(FText* result, const FTextKey* Namespace, const FTextKey* Key, const wchar_t* String)
	uintptr_t (*AsLocalizable_Advanced)();

	// Resolved native address of SDK::UKismetTextLibrary::Conv_TextToString, or 0 if not found.
	// Cast to: FString*(__fastcall*)(FString* result, const FText* InText)
	uintptr_t (*Conv_TextToString)();

	// Trampoline address of the original FTextKey::FTextKey(const wchar_t*), or 0 if the hook
	// failed to install. Use this to build the Namespace/Key arguments required by
	// AsLocalizable_Advanced -- FTextKey is just an interned-string-table index and cannot
	// be constructed any other way.
	// Cast to: void(__fastcall*)(FTextKey* this, const wchar_t* InStr)
	uintptr_t (*MakeTextKey)();
};

// ---------------------------------------------------------------------------
// HTTP server (v22, server only)
// ---------------------------------------------------------------------------

// HTTP verb reported to OnRawRequest filter callbacks.
enum class HttpMethod : uint8_t
{
	Get = 0, Post = 1, Put = 2, Delete = 3,
	Patch = 4, Options = 5, Head = 6, Other = 7,
};

// Return value for OnRawRequest filter callbacks.
enum class HttpRequestAction : uint8_t
{
	Approve = 0, // Continue processing (route handlers, then original engine handler).
	Deny    = 1, // Send 403 Forbidden; stop all further processing.
};

// Read-only snapshot of the incoming request. Pointers valid only for the callback duration.
// body is NOT null-terminated — use bodyLen.
struct PluginHttpRequest
{
	const char* url;     // UTF-8, null-terminated, e.g. "/remote/object/call"
	const char* body;    // Raw body bytes; nullptr when bodyLen == 0
	size_t      bodyLen;
	HttpMethod  method;
};

// Signature for raw-request filter callbacks.
typedef HttpRequestAction (*PluginHttpRequestFilterCallback)(const PluginHttpRequest* req);

// ---------------------------------------------------------------------------
// Raw-response routes (v22)
//
// A plugin-owned HTTP handler. The modloader calls the callback whenever an
// incoming URL starts with /<pluginName>/<urlPrefix>/ (case-insensitive).
// The plugin populates PluginHttpResponse to control what is sent back.
// If the callback leaves body/bodyLen at zero, a 200 with an empty body is sent.
// To send a non-200 status, set statusCode accordingly.
// The callback is always called from the HTTP connection thread — do NOT block
// indefinitely or access UObjects without proper synchronisation.
// ---------------------------------------------------------------------------

// Response descriptor filled by the plugin's route callback.
// All fields must remain valid until the callback returns.
// The modloader copies body bytes and the content-type string before returning.
struct PluginHttpResponse
{
	int      statusCode;   // HTTP status code, e.g. 200, 404, 500. Default: 200.
	const char* contentType;  // MIME type string, e.g. "application/json". Default: "text/plain".
	const char* body; // Response body bytes. May be nullptr when bodyLen == 0.
	size_t      bodyLen;      // Byte count of body. 0 produces an empty body.
};

// Callback signature for raw-response routes.
// req  : read-only incoming request snapshot — same semantics as filter callbacks.
// resp : output descriptor — plugin fills this before returning.
//        All pointer fields in resp must stay valid until the function returns;
// the modloader copies them immediately after.
typedef void (*PluginHttpRouteCallback)(const PluginHttpRequest* req, PluginHttpResponse* resp);

// ============================================================
// IPluginHttpServer — HTTP intercept interface (v22, server only)
//
// hooks->HttpServer is nullptr on client and generic builds.
//
// URL scheme:  /<pluginName>/<routeName>/...  (always case-insensitive)
//   Static files:   /<self->name>/<folderName>/path/to/file.html
//          → served from <exe_dir>\Plugins\<self->name>\<folderName>\
//   Raw routes:     /<self->name>/<urlPrefix>/any/sub/path
//          → callback receives the full original URL
//
// Processing order for every incoming request:
//   1. Raw-request filters  — RegisterOnRawRequest / UnregisterOnRawRequest
//        First Deny sends 403 and stops all further processing.
//   2. Raw-response routes  — AddRawRoute / RemoveRawRoute
//   Plugin-owned handler; plugin writes status, content-type, and body.
//   3. Static-file routes   — AddRoute / RemoveRoute
//        Files served from disk with a 200 and an inferred MIME type.
//   4. Pass-through     — original engine handler (produces 404 for unknown paths).
// ============================================================
struct IPluginHttpServer
{
	// -----------------------------------------------------------------------
	// Static-file routes
	// -----------------------------------------------------------------------

	// Register a static-file route.
	// URL matched (case-insensitive): /<self->name>/<folderName>/...
	// Files served from:     <exe_dir>\Plugins\<self->name>\<folderName>\
	// Returns false if already registered or folderName is empty/null.
	bool (*AddRoute)(const IPluginSelf* self, const char* folderName);

	// Unregister a static-file route. No-op if not registered.
	void (*RemoveRoute)(const IPluginSelf* self, const char* folderName);

	// -----------------------------------------------------------------------
	// Raw-request filters
	// -----------------------------------------------------------------------

	// Register a raw-request filter. Fires for every HTTP request before routes.
	// Return Deny to send 403; return Approve to continue. First Deny wins.
	void (*RegisterOnRawRequest)(PluginHttpRequestFilterCallback callback);

	// Unregister a raw-request filter. No-op if not registered.
	void (*UnregisterOnRawRequest)(PluginHttpRequestFilterCallback callback);

	// -----------------------------------------------------------------------
	// Raw-response routes (v22)
	// -----------------------------------------------------------------------

	// Register a plugin-owned HTTP handler.
	// urlPrefix  : route name, e.g. "api".
	// Matched URL (case-insensitive): /<self->name>/<urlPrefix>/...
	// callback   : called when a matching request arrives; plugin fills PluginHttpResponse.
	// Returns false if already registered or urlPrefix is empty/null.
	bool (*AddRawRoute)(const IPluginSelf* self, const char* urlPrefix, PluginHttpRouteCallback callback);

	// Unregister a previously added raw-response route. No-op if not registered.
	// Always call during PluginShutdown to avoid dangling callback pointers.
	void (*RemoveRawRoute)(const IPluginSelf* self, const char* urlPrefix);
};

// ---------------------------------------------------------------------------
// Net mode info (v36) — server + client, null on generic
// ---------------------------------------------------------------------------
struct IPluginNetModeInfo
{
    // Returns the current network mode via AActor::InternalGetNetMode, resolved
    // by AOB scan and invoked directly as a trampoline (no hook/detour).
    // Returns Unknown if the function could not be resolved or no actor is
    // available yet to query (e.g. called before world begin play).
    EPluginNetMode (*GetNetMode)();

    // Convenience: true if GetNetMode() is ListenServer or Client, i.e. an
    // active multiplayer session as opposed to a Standalone game.
    bool (*IsMultiplayer)();

    // Convenience: true if GetNetMode() is DedicatedServer or ListenServer.
    bool (*IsServer)();
};

// ---------------------------------------------------------------------------
// Top-level hooks interface (v14+)
// ---------------------------------------------------------------------------
struct IPluginHooks
{
	IPluginSpawnerHooks*   Spawner;        // v14
	IPluginHookUtils*  Hooks;    // v14
	IPluginMemoryUtils*    Memory;         // v14
	IPluginEngineEvents*   Engine;         // v14
	IPluginWorldEvents*    World;          // v14
	IPluginPlayerEvents* Players;        // v14
	IPluginActorEvents*    Actors;       // v14
	IPluginInputEvents*    Input;          // v15 — client only, null on server
	IPluginUIEvents*  UI;          // v15 — client only, null on server
	IPluginHUDEvents*      HUD;// v16 — client only, null on server
	IPluginNetworkChannel* Network;   // v17 — server+client; null on generic
	IPluginNativePointers* NativePointers; // v21
	IPluginHttpServer*     HttpServer;     // v22 — server only, null on client/generic
	IPluginNetModeInfo*    NetMode;          // v36 — server + client; null on generic
	IPluginTextUtils*      Text;             // FText localization helpers (AsLocalizable_Advanced, Conv_TextToString)
	IPluginImGuiTextures*  ImGuiTextures;    // v37 — client only; null on server/generic
};

// ---------------------------------------------------------------------------
// Plugin build target
// ---------------------------------------------------------------------------
enum PluginTarget : int
{
    PLUGIN_TARGET_CLIENT = 0,
    PLUGIN_TARGET_SERVER = 1,
};

#define PLUGIN_TARGET_CLIENT_ONLY  PLUGIN_TARGET_CLIENT
#define PLUGIN_TARGET_SERVER_ONLY  PLUGIN_TARGET_SERVER

// ---------------------------------------------------------------------------
// Plugin metadata and identity
// ---------------------------------------------------------------------------
struct PluginInfo
{
	const char* name;
	const char* version;
	const char* author;
	const char* description;
	int interfaceVersion;
	int pluginTarget;  // PluginTarget value -- required
};

struct IPluginSelf
{
	const char*     name;
	const char*     version;
	IPluginLogger*  logger;
	IPluginConfig*  config;
	IPluginScanner* scanner;
	IPluginHooks*   hooks;
};

typedef PluginInfo* (*GetPluginInfoFunc)();
typedef bool        (*PluginInitFunc)(IPluginSelf* self);
typedef void        (*PluginShutdownFunc)();

#define PLUGIN_GET_INFO_FUNC_NAME  "GetPluginInfo"
#define PLUGIN_INIT_FUNC_NAME      "PluginInit"
#define PLUGIN_SHUTDOWN_FUNC_NAME  "PluginShutdown"
