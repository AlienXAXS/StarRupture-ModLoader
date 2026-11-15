#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui_host_interface.h"

// ---------------------------------------------------------------------------
// ImGuiBackend
//
// Thin loader that loads StarRupture-ImGui.dll and delegates all calls into it.
//
// Flow:
//   Initialize(cbs) — loads StarRupture-ImGui.dll, passes callbacks, installs hooks.
//   (first Present call with queue captured) — inits D3D12 resources + ImGui backends.
//   (each Present) — runs the ImGui frame via cbs.RenderFrame callback.
//   Shutdown()      — unloads the host DLL, releases D3D12 resources.
//
// The caller (client_ui.cpp) builds the ImGuiRenderCallbacks struct to wire
// the host DLL back into the main DLL's UI/input subsystems.
// ---------------------------------------------------------------------------

namespace UI::ImGuiBackend
{
    // Load StarRupture-ImGui.dll, pass callbacks, and install D3D12 Present hooks.
    // Call from InitClientUI() after loading settings and registering keybinds.
    void Initialize(const ImGuiRenderCallbacks& cbs);

    // Called once the game's rendering pipeline is fully stable (i.e. from a
    // WorldBeginPlay callback).  Until this is called the Present hook passes
    // through without initialising ImGui, avoiding conflicts with Streamline
    // and UE5 viewport setup that happen in the first few seconds.
    void SetRenderingReady();

    // Shutdown: uninstall hooks, release all resources.
    void Shutdown();

    // Returns the IModLoaderImGui function table (valid after Initialize).
    IModLoaderImGui* GetImGuiAPI();

    // Returns the IPluginImGuiTextures function table (valid after Initialize).
    IPluginImGuiTextures* GetTextureAPI();

    // Request a font atlas rebuild on the next rendered frame.
    // Safe to call from any thread.  No-op before D3D12 resources are initialised.
    void RequestFontRebuild();

    // True when ImGui currently wants text input (an InputText-style widget
    // is focused). Returns false if the host DLL isn't loaded/initialised.
    bool IsTextInputActive();

    // The game window the overlay draws on: null before the first Present, or
    // when the host DLL is not loaded. Safe to call from any thread.
    HWND GetGameWindow();

    // Slate's present step (hooks/game/slate_present); see
    // ImGuiHost_RecordUiOverlay / ImGuiHost_SubmitUiOverlay.
    int  RecordUiOverlay(void* rhiBackBuffer);
    bool SubmitUiOverlay(int slot, ImGuiUiOverlaySync* sync);

    // The image library's read; see ImGuiHost_TryCopyTextureLevel for the arguments and the
    // ImGuiHostCopyResult it returns. ImGuiHostCopy_Unavailable when the host DLL is not loaded.
    int  TryCopyTextureLevel(void* uTexture2D, const char* name, unsigned minSize, ImGuiHostCopyOut* out);

    // The image library's shrink; see ImGuiHost_ShrinkTexture for the arguments and the
    // ImGuiHostShrinkResult it returns. ImGuiHostShrink_Failed when the host DLL is not loaded.
    int  ShrinkTexture(PluginTextureHandle source, unsigned maxSize, ImGuiHostShrinkOut* out);

    // Starts the shrink pass set-up on a thread of its own; see ImGuiHost_PrepareShrink.
    void PrepareShrink(unsigned maxSize);

    // The upload of a saved file; see ImGuiHost_UploadTexture. Not for the game or render thread.
    // ImGuiHostUpload_Failed when the host DLL is not loaded.
    int  UploadTexture(unsigned width, unsigned height, unsigned levels, const unsigned char* pixels,
                       unsigned long long bytes, ImGuiHostUploadOut* out);

    // The pixels of a shrink's stored copy; see ImGuiHost_TakeReadback. -1 when the host DLL is not loaded.
    int  TakeReadback(unsigned ticket, unsigned char* dest, unsigned long long bytes);

    // True while the overlay's GPU side is up; false before the first frame, after a lost device
    // and after shutdown. Any thread.
    bool GpuReady();

    // Where a copy TryCopyTextureLevel made stands: 1 finished, 0 on the GPU, -1 gone. Game thread.
    int  CopyState(PluginTextureHandle texture);

    // The adapter's local video memory budget in bytes, 0 when unknown. Game thread.
    unsigned long long LocalVideoBudget();

    // The adapter's local video memory in use and its budget, in bytes. False when unknown. Game thread.
    bool LocalVideoMemory(unsigned long long* usage, unsigned long long* budget);
}

#endif // MODLOADER_CLIENT_BUILD
