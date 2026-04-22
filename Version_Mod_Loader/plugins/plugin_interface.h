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
#define PLUGIN_INTERFACE_VERSION_MIN 23
#define PLUGIN_INTERFACE_VERSION_MAX 23
#define PLUGIN_INTERFACE_VERSION PLUGIN_INTERFACE_VERSION_MAX

enum class PluginLogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };
enum class ConfigValueType { String, Integer, Float, Boolean };

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
namespace SDK { class UWorld; }

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

typedef void (*PluginKeybindCallback)(EModKey key, EModKeyEvent event);

struct IPluginInputEvents
{
	void (*RegisterKeybind)(EModKey key, EModKeyEvent event, PluginKeybindCallback callback);
	void (*UnregisterKeybind)(EModKey key, EModKeyEvent event, PluginKeybindCallback callback);
	void (*RegisterKeybindByName)(const char* keyName, EModKeyEvent event, PluginKeybindCallback callback);
	void (*UnregisterKeybindByName)(const char* keyName, EModKeyEvent event, PluginKeybindCallback callback);
};

// ---------------------------------------------------------------------------
// UI (v15–v16, client only)
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
};

typedef void (*PluginImGuiRenderCallback)(IModLoaderImGui* imgui);

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
	const char* name;
	PluginImGuiRenderCallback renderFn;
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
};

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
