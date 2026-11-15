#include "pch.h"
#include "imgui_backend.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui_host_interface.h"
#include "../logging/log.h"

#include <atomic>

namespace
{
    HMODULE g_hostDll = nullptr;

    void                   (*g_initFn)(const ImGuiRenderCallbacks*)  = nullptr;
    void                   (*g_shutdownFn)()                          = nullptr;
    void                   (*g_setReadyFn)()                          = nullptr;
    IModLoaderImGui*       (*g_getAPIFn)()                            = nullptr;
    IPluginImGuiTextures*  (*g_getTexFn)()                            = nullptr;
    void                   (*g_fontRebuildFn)()                       = nullptr;
    bool                   (*g_isTextInputActiveFn)()                 = nullptr;
    // Atomic, unlike the rest: GetGameWindow is called from any thread.
    std::atomic<HWND (*)()> g_getGameWindowFn{ nullptr };
    // Atomic too: the render and RHI threads call these.
    std::atomic<int  (*)(void*)>                    g_recordUiOverlayFn{ nullptr };
    std::atomic<bool (*)(int, ImGuiUiOverlaySync*)> g_submitUiOverlayFn{ nullptr };
    // And this one: the image library calls it from the game thread, shutdown clears it.
    std::atomic<int  (*)(void*, const char*, unsigned, ImGuiHostCopyOut*)> g_tryCopyTextureLevelFn{ nullptr };
    std::atomic<int  (*)(PluginTextureHandle, unsigned, ImGuiHostShrinkOut*)> g_shrinkTextureFn{ nullptr };
    std::atomic<void (*)(unsigned)>                 g_prepareShrinkFn{ nullptr };
    std::atomic<int  (*)(unsigned, unsigned char*, unsigned long long)> g_takeReadbackFn{ nullptr };
    // Called from the image cache's worker thread.
    std::atomic<int  (*)(unsigned, unsigned, unsigned, const unsigned char*, unsigned long long, ImGuiHostUploadOut*)> g_uploadTextureFn{ nullptr };
    std::atomic<bool (*)()>                         g_gpuReadyFn{ nullptr };
    std::atomic<int  (*)(PluginTextureHandle)>      g_copyStateFn{ nullptr };
    std::atomic<unsigned long long (*)()>           g_localVideoBudgetFn{ nullptr };
    std::atomic<int (*)(unsigned long long*, unsigned long long*)> g_localVideoMemoryFn{ nullptr };
}

namespace UI::ImGuiBackend
{
    void Initialize(const ImGuiRenderCallbacks& cbs)
    {
        wchar_t dllPath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
        wchar_t* slash = wcsrchr(dllPath, L'\\');
        if (slash)
            wcscpy_s(slash + 1, static_cast<rsize_t>(MAX_PATH - (slash + 1 - dllPath)),
                L"ModLoader\\StarRupture-ImGui.dll");

        g_hostDll = LoadLibraryW(dllPath);
        if (!g_hostDll)
        {
            LogToFile::Error("[ImGuiBackend] Failed to load ModLoader\\StarRupture-ImGui.dll (error %lu)",
                GetLastError());
            return;
        }

        g_initFn        = reinterpret_cast<decltype(g_initFn)>      (GetProcAddress(g_hostDll, "ImGuiHost_Initialize"));
        g_shutdownFn    = reinterpret_cast<decltype(g_shutdownFn)>   (GetProcAddress(g_hostDll, "ImGuiHost_Shutdown"));
        g_setReadyFn    = reinterpret_cast<decltype(g_setReadyFn)>   (GetProcAddress(g_hostDll, "ImGuiHost_SetRenderingReady"));
        g_getAPIFn      = reinterpret_cast<decltype(g_getAPIFn)>     (GetProcAddress(g_hostDll, "ImGuiHost_GetImGuiAPI"));
        g_getTexFn      = reinterpret_cast<decltype(g_getTexFn)>     (GetProcAddress(g_hostDll, "ImGuiHost_GetTextureAPI"));
        g_fontRebuildFn = reinterpret_cast<decltype(g_fontRebuildFn)>(GetProcAddress(g_hostDll, "ImGuiHost_RequestFontRebuild"));
        g_isTextInputActiveFn = reinterpret_cast<decltype(g_isTextInputActiveFn)>(GetProcAddress(g_hostDll, "ImGuiHost_IsTextInputActive"));
        g_getGameWindowFn.store(reinterpret_cast<HWND (*)()>(GetProcAddress(g_hostDll, "ImGuiHost_GetGameWindow")));
        g_recordUiOverlayFn.store(reinterpret_cast<int (*)(void*)>(GetProcAddress(g_hostDll, "ImGuiHost_RecordUiOverlay")));
        g_submitUiOverlayFn.store(reinterpret_cast<bool (*)(int, ImGuiUiOverlaySync*)>(GetProcAddress(g_hostDll, "ImGuiHost_SubmitUiOverlay")));
        g_tryCopyTextureLevelFn.store(reinterpret_cast<int (*)(void*, const char*, unsigned, ImGuiHostCopyOut*)>(GetProcAddress(g_hostDll, "ImGuiHost_TryCopyTextureLevel")));
        g_shrinkTextureFn.store(reinterpret_cast<int (*)(PluginTextureHandle, unsigned, ImGuiHostShrinkOut*)>(GetProcAddress(g_hostDll, "ImGuiHost_ShrinkTexture")));
        g_prepareShrinkFn.store(reinterpret_cast<void (*)(unsigned)>(GetProcAddress(g_hostDll, "ImGuiHost_PrepareShrink")));
        g_takeReadbackFn.store(reinterpret_cast<int (*)(unsigned, unsigned char*, unsigned long long)>(GetProcAddress(g_hostDll, "ImGuiHost_TakeReadback")));
        g_uploadTextureFn.store(reinterpret_cast<int (*)(unsigned, unsigned, unsigned, const unsigned char*, unsigned long long, ImGuiHostUploadOut*)>(GetProcAddress(g_hostDll, "ImGuiHost_UploadTexture")));
        g_gpuReadyFn.store(reinterpret_cast<bool (*)()>(GetProcAddress(g_hostDll, "ImGuiHost_GpuReady")));
        g_copyStateFn.store(reinterpret_cast<int (*)(PluginTextureHandle)>(GetProcAddress(g_hostDll, "ImGuiHost_CopyState")));
        g_localVideoBudgetFn.store(reinterpret_cast<unsigned long long (*)()>(GetProcAddress(g_hostDll, "ImGuiHost_LocalVideoBudget")));
        g_localVideoMemoryFn.store(reinterpret_cast<int (*)(unsigned long long*, unsigned long long*)>(GetProcAddress(g_hostDll, "ImGuiHost_LocalVideoMemory")));

        if (!g_initFn)
        {
            LogToFile::Error("[ImGuiBackend] StarRupture-ImGui.dll missing expected exports");
            FreeLibrary(g_hostDll);
            g_hostDll = nullptr;
            return;
        }

        g_initFn(&cbs);
    }

    void Shutdown()
    {
        g_getGameWindowFn.store(nullptr);
        g_recordUiOverlayFn.store(nullptr);
        g_tryCopyTextureLevelFn.store(nullptr);
        g_shrinkTextureFn.store(nullptr);
        g_prepareShrinkFn.store(nullptr);
        g_takeReadbackFn.store(nullptr);
        g_uploadTextureFn.store(nullptr);
        g_gpuReadyFn.store(nullptr);
        g_copyStateFn.store(nullptr);
        g_localVideoBudgetFn.store(nullptr);
        g_localVideoMemoryFn.store(nullptr);
        if (g_shutdownFn) g_shutdownFn();
        g_submitUiOverlayFn.store(nullptr);
        if (g_hostDll)
        {
            FreeLibrary(g_hostDll);
            g_hostDll = nullptr;
        }
        g_initFn = nullptr;
        g_shutdownFn = nullptr;
        g_setReadyFn = nullptr;
        g_fontRebuildFn = nullptr;
        g_getAPIFn = nullptr;
        g_getTexFn = nullptr;
        g_isTextInputActiveFn = nullptr;
    }

    void SetRenderingReady()
    {
        if (g_setReadyFn) g_setReadyFn();
    }

    IModLoaderImGui* GetImGuiAPI()
    {
        return g_getAPIFn ? g_getAPIFn() : nullptr;
    }

    IPluginImGuiTextures* GetTextureAPI()
    {
        return g_getTexFn ? g_getTexFn() : nullptr;
    }

    void RequestFontRebuild()
    {
        if (g_fontRebuildFn) g_fontRebuildFn();
    }

    bool IsTextInputActive()
    {
        return g_isTextInputActiveFn ? g_isTextInputActiveFn() : false;
    }

    HWND GetGameWindow()
    {
        const auto fn = g_getGameWindowFn.load();
        return fn ? fn() : nullptr;
    }

    int RecordUiOverlay(void* rhiBackBuffer)
    {
        const auto fn = g_recordUiOverlayFn.load();
        return fn ? fn(rhiBackBuffer) : -1;
    }

    bool SubmitUiOverlay(int slot, ImGuiUiOverlaySync* sync)
    {
        const auto fn = g_submitUiOverlayFn.load();
        return fn ? fn(slot, sync) : false;
    }

    int TryCopyTextureLevel(void* uTexture2D, const char* name, unsigned minSize, ImGuiHostCopyOut* out)
    {
        const auto fn = g_tryCopyTextureLevelFn.load();
        return fn ? fn(uTexture2D, name, minSize, out) : ImGuiHostCopy_Unavailable;
    }

    int ShrinkTexture(PluginTextureHandle source, unsigned maxSize, ImGuiHostShrinkOut* out)
    {
        const auto fn = g_shrinkTextureFn.load();
        return fn ? fn(source, maxSize, out) : ImGuiHostShrink_Failed;
    }

    void PrepareShrink(unsigned maxSize)
    {
        const auto fn = g_prepareShrinkFn.load();
        if (fn)
            fn(maxSize);
    }

    int UploadTexture(unsigned width, unsigned height, unsigned levels, const unsigned char* pixels,
                      unsigned long long bytes, ImGuiHostUploadOut* out)
    {
        const auto fn = g_uploadTextureFn.load();
        return fn ? fn(width, height, levels, pixels, bytes, out) : ImGuiHostUpload_Failed;
    }

    int TakeReadback(unsigned ticket, unsigned char* dest, unsigned long long bytes)
    {
        const auto fn = g_takeReadbackFn.load();
        return fn ? fn(ticket, dest, bytes) : -1;
    }

    bool GpuReady()
    {
        const auto fn = g_gpuReadyFn.load();
        return fn && fn();
    }

    int CopyState(PluginTextureHandle texture)
    {
        const auto fn = g_copyStateFn.load();
        return fn ? fn(texture) : -1;
    }

    unsigned long long LocalVideoBudget()
    {
        const auto fn = g_localVideoBudgetFn.load();
        return fn ? fn() : 0;
    }

    bool LocalVideoMemory(unsigned long long* usage, unsigned long long* budget)
    {
        const auto fn = g_localVideoMemoryFn.load();
        return fn && fn(usage, budget) != 0;
    }
}

#endif // MODLOADER_CLIENT_BUILD
