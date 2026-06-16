#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../plugins/plugin_interface.h"

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
