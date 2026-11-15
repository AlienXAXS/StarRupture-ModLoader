#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../plugins/plugin_interface.h"

struct ID3D12Fence;

// ---------------------------------------------------------------------------
// ImGui host DLL interface
//
// StarRupture-ImGui.dll is a separate DLL that contains all ImGui code and
// the D3D12 vtable hooks.  This header is shared by both the host DLL and
// the main Mod_Loader DLL that loads it.
//
// The main DLL passes an ImGuiRenderCallbacks struct to ImGuiHost_Initialize.
// The host DLL calls back into the main DLL through those function pointers
// for operations that live in the main DLL (UI rendering, input, logging).
// ---------------------------------------------------------------------------

struct ImGuiRenderCallbacks
{
    // Render all modloader UI surfaces for one frame.
    // Called inside HookedPresent after ImGui::NewFrame().
    void    (*RenderFrame)(IModLoaderImGui* api);

    // Return true if the modloader UI is visible and should capture input.
    bool    (*ShouldCaptureInput)();

    // Return true if a plugin has requested cooperative (passthrough) input.
    // Only consulted when ShouldCaptureInput() is false: ImGui still receives
    // every message and draws its cursor, but the game keeps receiving input
    // too except for the classes ImGui actually wants (mouse while hovering an
    // ImGui window, keyboard while a text field is focused).
    bool    (*ShouldPassthroughInput)();

    // Process a WM_KEY*/WM_MOUSE* message; fire registered keybind callbacks.
    // Returns true if the message should be swallowed (blocking mode).
    bool    (*DispatchKey)(UINT msg, WPARAM wParam, LPARAM lParam);

    // Return the font family key from GlobalSettings (e.g. "Default", "Inter").
    const char* (*GetFontFamily)();

    // Return the font scale factor from GlobalSettings.
    float       (*GetFontScale)();

    // Write a pre-formatted log message.
    // level: 0=trace, 1=debug, 2=info, 3=warn, 4=error
    void    (*Log)(int level, const char* msg);

    // Called when the D3D12 device is lost; show an error splash window.
    void    (*OnDeviceLost)(const wchar_t* msg);

    // Uninstall the UGameViewportClient::InputKey detour.
    // Called at the start of Shutdown() before releasing D3D12 resources.
    void    (*RemoveInputHook)();

    // Deliver a WM_MOUSEWHEEL / WM_MOUSEHWHEEL to plugin wheel handlers.
    // uiCapturing is true under exclusive capture. Returns true if a handler
    // consumed it and the game must not see it (never while uiCapturing).
    bool    (*DispatchMouseWheel)(UINT msg, WPARAM wParam, LPARAM lParam, bool uiCapturing);

    // True while the game shows its own cursor (the pause menu, or any game
    // screen with the mouse cursor up) and nothing has exclusive capture. ImGui
    // is fed the mouse then and keeps only the messages it wants; the keyboard
    // stays with the game unless an ImGui text field has focus.
    bool    (*IsGameMouseShared)();

    // Engine ticks so far (UGameEngine::Tick), for per-tick limits; 0 until the
    // first tick, and for good if the tick hook could not be installed. Any thread.
    unsigned long long (*GameTickCount)();

    // The offset of a native engine member from the version map's row for the
    // running game build, or -1 when there is none, with the reason in why.
    // key is "Class::Member", size its size in bytes. Logs a [Map] line when
    // there is no row, so call it once per key per session.
    int     (*NativeFieldOffset)(const char* key, unsigned size, char* why, size_t whySize);
};

// The two fences that order one overlay draw, recorded at Slate's present
// step, with UE's queue: UE's queue signals readyFence/readyValue once
// Slate's draw is done (the host's queue waits for it), and waits for
// doneFence/doneValue, which the host's queue signals after the overlay.
struct ImGuiUiOverlaySync
{
    ID3D12Fence* readyFence;
    UINT64       readyValue;
    ID3D12Fence* doneFence;
    UINT64       doneValue;
};

// ---------------------------------------------------------------------------
// Exported by StarRupture-ImGui.dll
// ---------------------------------------------------------------------------

#ifdef BUILDING_IMGUI_HOST_DLL
#  define IMGUI_HOST_API extern "C" __declspec(dllexport)
#else
#  define IMGUI_HOST_API extern "C" __declspec(dllimport)
#endif

IMGUI_HOST_API void                   ImGuiHost_Initialize(const ImGuiRenderCallbacks* cbs);
IMGUI_HOST_API void                   ImGuiHost_Shutdown();
IMGUI_HOST_API void                   ImGuiHost_SetRenderingReady();
IMGUI_HOST_API IModLoaderImGui*       ImGuiHost_GetImGuiAPI();
IMGUI_HOST_API IPluginImGuiTextures*  ImGuiHost_GetTextureAPI();
IMGUI_HOST_API void                   ImGuiHost_RequestFontRebuild();

// True when ImGui itself currently wants text input (i.e. an InputText-style
// widget is focused and accepting keystrokes/caret blink), regardless of
// which modloader/plugin window it lives in.  Used to suppress toggle-style
// keybinds while the player is typing into an ImGui field -- e.g. a plugin
// bound to "P" must not close its own panel when the player types "Port"
// into a text box inside that panel.
IMGUI_HOST_API bool                   ImGuiHost_IsTextInputActive();

// The game window the overlay draws on, or null before the first Present.
// Any thread.
IMGUI_HOST_API HWND                   ImGuiHost_GetGameWindow();

// Slate's present step (hooks/game/slate_present). Render thread: builds the
// ImGui frame for rhiBackBuffer (an FRHITexture*) and keeps a copy of it.
// Returns the slot to submit, or -1 when nothing was recorded (not
// initialised, not the game's back buffer, a Present-time draw in progress).
// Every slot returned must be submitted, and rhiBackBuffer must stay valid
// until it is.
IMGUI_HOST_API int                    ImGuiHost_RecordUiOverlay(void* rhiBackBuffer);

// RHI thread, in command order: draws a recorded slot into the buffer
// rhiBackBuffer names at this point and submits it on the host's queue
// behind the ready fence. True fills *sync; the caller must then signal the
// ready fence and wait on the done fence on UE's queue. False: nothing was
// queued and there is nothing to signal or wait on.
IMGUI_HOST_API bool                   ImGuiHost_SubmitUiOverlay(int slot, ImGuiUiOverlaySync* sync);

// Try mode for the image library: copies one level of an engine texture (a UTexture2D*) into a
// texture of that level's size, and never waits on the game thread. Game thread only.
//
// The level is the smallest whose larger side is at least minSize, or level 0 when none is (an
// image smaller than that is read at its own size). name is for the log. The texture is a slot of
// the texture API: the caller frees it with FreeTexture once the copy is no longer wanted.
//
// Results:
//   Started      the copy is on the GPU; *out is filled in and draws once it has finished.
//   NotYet       the source is not ready: not streamed in, not in a readable state, or its
//                memory is still being made resident. Ask again on a later tick.
//   Busy         over the per-tick or in-flight limit, or no texture slot is free. Ask again.
//   Failed       this texture cannot be copied (not a texture the copy can read, or the copy
//                failed). Asking again does not help.
//   Unavailable  no copy can run: no version map rows for this game build, the device is lost,
//                or copies are off for the session.
enum ImGuiHostCopyResult : int
{
    ImGuiHostCopy_Started     = 0,
    ImGuiHostCopy_NotYet      = 1,
    ImGuiHostCopy_Busy        = 2,
    ImGuiHostCopy_Failed      = 3,
    ImGuiHostCopy_Unavailable = 4,
};

struct ImGuiHostCopyOut
{
    PluginTextureHandle texture;       // the new texture, at the size below
    int                 width;
    int                 height;
    int                 sourceWidth;   // level 0 of the source
    int                 sourceHeight;
    unsigned            level;         // which level of the source was copied
    unsigned            sourceFormat;  // the source's DXGI_FORMAT
};

IMGUI_HOST_API int                    ImGuiHost_TryCopyTextureLevel(void* uTexture2D, const char* name, unsigned minSize, ImGuiHostCopyOut* out);

// The shrink for the image library: draws a texture made by ImGuiHost_TryCopyTextureLevel into a new
// 8-bit RGBA texture and builds its smaller levels. The new texture is at most maxSize on the larger
// side (a smaller image keeps its own size) with the full chain of levels down to 1x1. Game thread only.
//
// The source stays untouched and alive on the GPU until the shrink has run, so the caller frees its
// handle right after Started. The new texture is a slot of the texture API and draws once the shrink
// has finished.
//
// Results:
//   Started  the shrink is on the GPU; *out is filled in.
//   Busy     the shrink pass is still being set up, the source copy has not finished, or no copy
//            entry or texture slot is free. Ask again.
//   Failed   the shrink cannot be made (not a texture this DLL made, the GPU side is gone, or the
//            pass could not be built). Asking again does not help.
enum ImGuiHostShrinkResult : int
{
    ImGuiHostShrink_Started = 0,
    ImGuiHostShrink_Busy    = 1,
    ImGuiHostShrink_Failed  = 2,
};

struct ImGuiHostShrinkOut
{
    PluginTextureHandle texture;
    int                 width;
    int                 height;
    unsigned            levels;   // levels in the chain, level 0 included
    unsigned            readback; // ticket for ImGuiHost_TakeReadback, 0 when none was made
};

IMGUI_HOST_API int                    ImGuiHost_ShrinkTexture(PluginTextureHandle source, unsigned maxSize, ImGuiHostShrinkOut* out);

// Starts the one-time set-up of the shrink pass and the readback buffers (shader compile, pipeline
// state, buffers) on a thread of its own, so the first ImGuiHost_ShrinkTexture does not have to.
// maxSize is the one the shrinks will use. Does nothing when it is already started or done, or the
// GPU side is not up. Never waits; any thread. Until the set-up has ended a shrink answers Busy.
IMGUI_HOST_API void                   ImGuiHost_PrepareShrink(unsigned maxSize);

// The pixels of a shrink's stored copy, for saving to disk. The shrink's command list also copied
// every level into a readback buffer; this takes them once the GPU has finished. dest receives
// every level tightly packed (8-bit RGBA, level 0 first) and bytes must be exactly that size. With
// a null dest the readback is only freed. Never waits. Game thread.
//
// Returns 1 done (dest is filled, the ticket is spent), 0 still on the GPU (ask again), -1 gone
// (never given, already taken, wrong size, or the GPU side is gone; the ticket is spent).
IMGUI_HOST_API int                    ImGuiHost_TakeReadback(unsigned ticket, unsigned char* dest, unsigned long long bytes);

// The upload for the image library's saved files: makes a texture from the pixels of a stored copy
// (8-bit RGBA, every level tightly packed, as ImGuiHost_TakeReadback gives them) on a copy queue of
// its own, and waits for that copy. It shares no queue or fence with the game or the overlay and
// must not be called from the game thread or the render thread: it waits for the GPU. One call at a
// time; a second one waits for the first. The new texture is a slot of the texture API, drawable at
// once, and counts as a stored copy (the caller frees it like a shrink's).
//
// Results:
//   Started  *out is filled in.
//   Busy     no texture slot is free, or the GPU side is not up yet. Ask again later.
//   Failed   the upload cannot be made (wrong sizes, the GPU side is gone, or the copy did not finish).
enum ImGuiHostUploadResult : int
{
    ImGuiHostUpload_Started = 0,
    ImGuiHostUpload_Busy    = 1,
    ImGuiHostUpload_Failed  = 2,
};

struct ImGuiHostUploadOut
{
    PluginTextureHandle texture;
    int                 width;
    int                 height;
};

IMGUI_HOST_API int                    ImGuiHost_UploadTexture(unsigned width, unsigned height, unsigned levels, const unsigned char* pixels, unsigned long long bytes, ImGuiHostUploadOut* out);

// True once the overlay's GPU side is up (the first frame has set it up) and until it is shut
// down or the device is lost. The image library does nothing before it. Any thread.
IMGUI_HOST_API bool                   ImGuiHost_GpuReady();

// Where a copy made by ImGuiHost_TryCopyTextureLevel stands: 1 finished, 0 still on the GPU,
// -1 the texture is gone (freed, or the host shut down). Game thread.
IMGUI_HOST_API int                    ImGuiHost_CopyState(PluginTextureHandle texture);

// The adapter's local video memory budget in bytes, or 0 when it cannot be read. Game thread.
IMGUI_HOST_API unsigned long long     ImGuiHost_LocalVideoBudget();
// The adapter's local video memory in use and its budget, in bytes. False when the adapter cannot be asked.
IMGUI_HOST_API int                    ImGuiHost_LocalVideoMemory(unsigned long long* usage, unsigned long long* budget);
