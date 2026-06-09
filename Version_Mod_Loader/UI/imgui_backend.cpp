#include "pch.h"
#include "imgui_backend.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_dx12.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "modloader_window.h"
#include "overlay.h"
#include "global_settings.h"
#include "plugin_panel_registry.h"
#include "plugin_widget_registry.h"
#include "hooks/hooks_common.h"
#include "hooks/input/keybind_registry.h"
#include "hooks/input/input_hook.h"
#include "logging/log.h"
#include "splash_window.h"

#include <d3d12.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstring>
#include <intrin.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")

#include <wincodec.h>
#include <mutex>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags);
static void    STDMETHODCALLTYPE HookedECL(ID3D12CommandQueue* pQueue, UINT NumCmdLists, ID3D12CommandList* const* ppCmdLists);
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(void* pFactory, IUnknown* pDevice, HWND hWnd,
	const void* pDesc, const void* pFullscreenDesc, void* pRestrictToOutput, void* ppSwapChain);

// ---------------------------------------------------------------------------
// D3D12 per-frame resources
// ---------------------------------------------------------------------------
static const int MAX_FRAMES = 8;

struct FrameContext
{
	ID3D12CommandAllocator* commandAllocator = nullptr;
	UINT64                  fenceValue = 0;
	// Back buffers are NOT cached -- we GetBuffer/Release each Present so that
	// IDXGISwapChain::ResizeBuffers is never blocked by our reference.
};

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------
namespace
{
	// Vtable slot we patched -- restored in Shutdown().
	// We patch dxgi.dll's IDXGISwapChain vtable[8] pointer rather than
	// inline-patching the function code.  Steam overlay and RTSS both
	// inline-hook the same Present function; patching the vtable pointer
	// leaves their code hooks intact and avoids the conflict.
	void** g_vtableSlot = nullptr;

	// The swap chain we initialised on -- used to skip foreign swap chains.
	IDXGISwapChain* g_swapChain = nullptr;

	// Set true by SetRenderingReady() once WorldBeginPlay fires.
	// Gates D3D12 resource init to avoid conflicts with Streamline/UE5 startup.
	std::atomic<bool> g_renderingReady = true;

	std::atomic<bool> g_initialized = false;
	std::atomic<bool> g_shutdown = false;

	ID3D12Device* g_device = nullptr;
	ID3D12CommandQueue* g_cmdQueue = nullptr;
	ID3D12GraphicsCommandList* g_cmdList = nullptr;
	ID3D12DescriptorHeap* g_srvHeap = nullptr;
	ID3D12DescriptorHeap* g_rtvHeap = nullptr;
	ID3D12Fence* g_fence = nullptr;
	HANDLE                     g_fenceEvent = nullptr;
	UINT64                     g_fenceValue = 0;
	FrameContext               g_frames[MAX_FRAMES];
	UINT                       g_frameCount = 0;
	UINT                       g_rtvDescSize = 0;
	DXGI_FORMAT                g_rtvFormat = DXGI_FORMAT_UNKNOWN;

	HWND    g_hwnd = nullptr;
	WNDPROC g_origWndProc = nullptr;

	IModLoaderImGui g_imguiAPI = {};

	// -------------------------------------------------------------------------
	// Plugin texture registry (v37)
	// -------------------------------------------------------------------------
	static const int MAX_PLUGIN_TEXTURES = 2048;

	struct PluginTextureRecord
	{
		ID3D12Resource*              resource  = nullptr;
		D3D12_GPU_DESCRIPTOR_HANDLE  gpuHandle = {};
		int                          width     = 0;
		int                          height    = 0;
		bool                         inUse     = false;
		bool                         owned     = true;  // always true; Release() on free
		char                         name[64]  = {};    // plugin-supplied label for logging
	};

	PluginTextureRecord g_pluginTextures[MAX_PLUGIN_TEXTURES] = {};
	std::mutex          g_textureMutex;
	UINT                g_srvDescSize = 0;
	IPluginImGuiTextures g_textureAPI = {};

	// WIC factory — created once on first texture load, released in Shutdown.
	IWICImagingFactory* g_wicFactory = nullptr;

	// -------------------------------------------------------------------------
	// Font rebuild state
	// -------------------------------------------------------------------------
	std::atomic<bool> g_pendingFontRebuild{ false };

	struct FontEntry
	{
		const char*    iniKey;       // value stored in modloader.ini [UI] FontFamily
		const char*    displayName;  // shown in the UI combo box
		const wchar_t* primaryPath;  // nullptr = use ImGui built-in font
		const wchar_t* mergePath;    // nullptr = no merged font
	};

	static const FontEntry k_fonts[] =
	{
		{ "Default",  "Default (built-in)",  nullptr,                              nullptr                              },
		{ "Arial",    "Arial",               L"C:\\Windows\\Fonts\\arial.ttf",     nullptr                              },
		{ "YaHei",    "Microsoft YaHei",     L"C:\\Windows\\Fonts\\msyh.ttc",      nullptr                              },
		{ "Meiryo",   "Meiryo",              L"C:\\Windows\\Fonts\\meiryo.ttc",    nullptr                              },
		{ "Malgun",   "Malgun Gothic",       L"C:\\Windows\\Fonts\\malgun.ttf",    nullptr                              },
		{ "ArialCJK", "Arial + CJK (merged)",L"C:\\Windows\\Fonts\\arial.ttf",     L"C:\\Windows\\Fonts\\msyh.ttc"     },
	};
	static const int k_fontCount = static_cast<int>(sizeof(k_fonts) / sizeof(k_fonts[0]));

	using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	PresentFn g_originalPresent = nullptr;

	// g_capturedQueue: updated on every ID3D12CommandQueue::ExecuteCommandLists call
	// to the most recently seen DIRECT queue.  By the time HookedPresent fires,
	// this holds the queue that last submitted work touching the back buffer
	// (Streamline's composition queue when Streamline is active, UE5's render
	// queue otherwise).  We submit our ImGui commands on the same queue to avoid
	// cross-queue resource-state conflicts that cause DXGI_ERROR_DRIVER_INTERNAL_ERROR.
	// Weak reference -- UE5/Streamline own the queue lifetime.
	ID3D12CommandQueue* g_capturedQueue = nullptr;

	// ECL vtable patch -- mirrors the Present vtable patch approach.
	void** g_eclVtableSlot = nullptr;
	using ExecCmdListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
	ExecCmdListsFn g_originalECL = nullptr;

	// g_creationQueue: captured from IDXGIFactory2::CreateSwapChainForHwnd.
	// This is the queue DXGI implicitly synchronises with on Present -- always
	// correct for non-Streamline systems.  Weak reference; UE5 owns the lifetime.
	ID3D12CommandQueue* g_creationQueue = nullptr;

	// True if sl.interposer.dll was loaded at Initialize() time.
	// When Streamline is active we prefer g_capturedQueue (ECL heuristic) over
	// g_creationQueue because Streamline's composition queue is the one that
	// actually touches the back buffer last.
	bool g_streamlineActive = false;

	// IDXGIFactory2::CreateSwapChainForHwnd vtable patch (slot 15).
	// Typed as void* for parameters we don't inspect -- avoids dxgi1_2.h type
	// availability issues at namespace scope; ABI is identical (all pointer-sized).
	void** g_cscfhVtableSlot = nullptr;
	using CreateSCFHFn = HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, HWND,
		const void*, const void*, void*, void*);
	CreateSCFHFn g_originalCSCFH = nullptr;
}

// ---------------------------------------------------------------------------
// Returns true whenever any modloader UI surface is visible and should own
// the mouse and swallow game input.  Covers both the main modloader window
// and any open plugin panel windows.
// ---------------------------------------------------------------------------
static bool ShouldCaptureInput()
{
	return UI::ModLoaderWindow::IsOpen() || UI::PluginPanelRegistry::AnyPanelOpen();
}

// ---------------------------------------------------------------------------
// Keybind dispatch from WM messages
//
// Called for every key/mouse message before the game sees it.  Fires plugin
// keybind callbacks and returns true if the message should be swallowed
// (blocking mode -- return 0 from WndProc so UE5 never processes it).
//
// Works in ALL game states: main menu, loading screens, and in-game.
// The UGameViewportClient::InputKey hook is responsible for the in-game
// blocking path only; dispatch always originates here.
// ---------------------------------------------------------------------------
static bool DispatchKeybindMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
	using namespace Hooks::Input;

	EModKey      mk    = EModKey::Unknown;
	EModKeyEvent event = EModKeyEvent::Pressed;

	switch (msg)
	{
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		// lParam bit 30: key was already down (auto-repeat) -- ignore repeats.
		if (lParam & (1 << 30)) return false;
		mk    = VKToModKey(static_cast<int>(wParam));
		event = EModKeyEvent::Pressed;
		break;

	case WM_KEYUP:
	case WM_SYSKEYUP:
		mk    = VKToModKey(static_cast<int>(wParam));
		event = EModKeyEvent::Released;
		break;

	case WM_LBUTTONDOWN: mk = EModKey::LeftMouseButton;   event = EModKeyEvent::Pressed;  break;
	case WM_LBUTTONUP:   mk = EModKey::LeftMouseButton;   event = EModKeyEvent::Released; break;
	case WM_RBUTTONDOWN: mk = EModKey::RightMouseButton;  event = EModKeyEvent::Pressed;  break;
	case WM_RBUTTONUP:   mk = EModKey::RightMouseButton;  event = EModKeyEvent::Released; break;
	case WM_MBUTTONDOWN: mk = EModKey::MiddleMouseButton; event = EModKeyEvent::Pressed;  break;
	case WM_MBUTTONUP:   mk = EModKey::MiddleMouseButton; event = EModKeyEvent::Released; break;

	case WM_XBUTTONDOWN:
		mk    = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1)
		            ? EModKey::ThumbMouseButton
		            : EModKey::ThumbMouseButton2;
		event = EModKeyEvent::Pressed;
		break;
	case WM_XBUTTONUP:
		mk    = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1)
		            ? EModKey::ThumbMouseButton
		            : EModKey::ThumbMouseButton2;
		event = EModKeyEvent::Released;
		break;

	default:
		return false;
	}

	if (mk == EModKey::Unknown) return false;

	// Do not fire standalone modifier keys as keybinds.
	int vk = ModKeyToVK(mk);
	if (vk == VK_LCONTROL || vk == VK_RCONTROL ||
	    vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
	    vk == VK_LMENU    || vk == VK_RMENU)
		return false;

	EModKeyModifiers mods = SampleCurrentModifiers();

	Dispatch(mk, event);
	DispatchCombo(mk, mods, event);

	// Tell the caller to swallow this message if blocking is enabled.
	return ShouldBlock(mk, mods);
}

// ---------------------------------------------------------------------------
// WndProc subclass -- forwards messages to ImGui, swallows input when UI open
// ---------------------------------------------------------------------------
static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	// Dispatch plugin keybind callbacks unconditionally so that toggle-style
	// keybinds (e.g. F2) can close the UI even while it is open.
	// Only swallow blocking messages when the UI is not capturing input.
	{
		bool blocked = DispatchKeybindMessage(msg, wParam, lParam);
		if (!ShouldCaptureInput() && blocked)
			return 0;
	}

	if (ShouldCaptureInput())
	{
		switch (msg)
		{
		case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
		case WM_CHAR:
		case WM_LBUTTONDOWN: case WM_LBUTTONUP:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP:
		case WM_MOUSEMOVE:   case WM_MOUSEWHEEL:
		case WM_INPUT:      // UE5 uses raw input for camera delta -- swallow it
		case WM_SETCURSOR:  // prevent UE5 hiding the cursor while our window is open
			return 0;
		default:
			break;
		}
	}

	return CallWindowProcW(g_origWndProc, hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Normalise a swap-chain format to something ImGui can render into.
// ImGui's pixel shader outputs float4, so the RTV must be UNORM/FLOAT.
// UINT formats cause DXGI_ERROR_DRIVER_INTERNAL_ERROR on NVIDIA drivers.
// SRGB formats cause a PSO/RTV mismatch.
// ---------------------------------------------------------------------------
static DXGI_FORMAT NormalizeForImGui(DXGI_FORMAT fmt)
{
	switch (fmt)
	{
		// SRGB formats are not valid as RTV -- strip the gamma flag.
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:  return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:  return DXGI_FORMAT_B8G8R8X8_UNORM;
		// UINT formats cause driver errors with ImGui's float4 shader output -- use UNORM.
	case DXGI_FORMAT_R10G10B10A2_UINT:      return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R8G8B8A8_UINT:         return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_R16G16B16A16_UINT:     return DXGI_FORMAT_R16G16B16A16_UNORM;
		// HDR formats that are already valid as RTV -- pass through unchanged.
	case DXGI_FORMAT_R10G10B10A2_UNORM:     // HDR10
	case DXGI_FORMAT_R16G16B16A16_FLOAT:    // scRGB / HDR linear
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return fmt;
	default:
		// Unrecognised format -- pass through and hope for the best.
		// Log so crash reports can identify new cases that need handling.
		LogToFile::Info("[ImGuiBackend] NormalizeForImGui: unrecognised format %u -- "
			"using as-is. Please report this with your GPU/display info.",
			static_cast<unsigned>(fmt));
		return fmt;
	}
}

// ---------------------------------------------------------------------------
// HookedCreateSwapChainForHwnd
//
// Captures the ID3D12CommandQueue* passed by UE5 at swap chain creation.
// That queue is what DXGI implicitly synchronises with on Present -- the
// definitive correct queue for non-Streamline systems.
// Only the first DIRECT queue is captured (UE5 creates exactly one real
// swap chain; Streamline may create additional ones internally).
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
	void* pFactory,
	IUnknown* pDevice,
	HWND         hWnd,
	const void* pDesc,
	const void* pFullscreenDesc,
	void* pRestrictToOutput,
	void* ppSwapChain)
{
	if (pDevice && !g_creationQueue)
	{
		ID3D12CommandQueue* q = nullptr;
		if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&q))))
		{
			if (q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
			{
				g_creationQueue = q;  // borrowed -- UE5 owns lifetime
				q->Release();         // release extra ref from QueryInterface
				LogToFile::Info("[ImGuiBackend] CreateSwapChainForHwnd: captured creation queue 0x%p",
					static_cast<void*>(g_creationQueue));
			}
			else
			{
				q->Release();
			}
		}
	}
	return g_originalCSCFH(pFactory, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
}

// ---------------------------------------------------------------------------
// CreateDeviceD3D
//
// Creates a temporary hardware D3D12 device, command queue, and swap chain
// solely to extract the IDXGISwapChain::Present vtable pointer and patch it
// with HookedPresent.  All temporary COM objects are released before return.
//
// Mirrors the Dear ImGui example_win32_directx12 CreateDeviceD3D pattern:
//   D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, ...)
//
// We previously used a D3D11 WARP adapter here to avoid Steam overlay
// interfering during DLL init.  With vtable patching (instead of MinHook)
// Steam's inline hooks on Present are harmless -- they sit on top of our
// vtable slot and are naturally called via g_originalPresent -- so a plain
// hardware D3D12 device is safe and much simpler.
//
// Actual rendering resources (heaps, allocators, etc.) are created later in
// InitD3D12Resources() using UE5's own device obtained from the back buffer.
// They MUST live on UE5's device because RTVs and back-buffer resources
// must belong to the same device.
// ---------------------------------------------------------------------------
static bool CreateDeviceD3D(HWND hWnd)
{
	IDXGIFactory4* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
	{
		LogToFile::Error("[ImGuiBackend] CreateDXGIFactory1 failed");
		return false;
	}

	// Hardware D3D12 device with D3D11 feature level -- same as the ImGui example.
	ID3D12Device* device = nullptr;
	if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
	{
		LogToFile::Error("[ImGuiBackend] D3D12CreateDevice failed");
		factory->Release();
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC qDesc = {};
	qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	ID3D12CommandQueue* queue = nullptr;
	if (FAILED(device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&queue))))
	{
		LogToFile::Error("[ImGuiBackend] CreateCommandQueue failed");
		device->Release();
		factory->Release();
		return false;
	}

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.BufferCount = 2;
	sd.Width = 8;
	sd.Height = 8;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.SampleDesc.Count = 1;
	IDXGISwapChain1* sc = nullptr;
	if (FAILED(factory->CreateSwapChainForHwnd(queue, hWnd, &sd, nullptr, nullptr, &sc)))
	{
		LogToFile::Error("[ImGuiBackend] CreateSwapChainForHwnd failed");
		queue->Release();
		device->Release();
		factory->Release();
		return false;
	}

	// Log which module owns the Present address so we can verify it is dxgi.dll.
	void* presentAddr = (*reinterpret_cast<void***>(sc))[8];
	{
		char modName[MAX_PATH] = "<unknown>";
		HMODULE hMod = nullptr;
		if (GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<LPCSTR>(presentAddr), &hMod))
			GetModuleFileNameA(hMod, modName, sizeof(modName));
		LogToFile::Info("[ImGuiBackend] IDXGISwapChain::Present vtable[8] = 0x%llX  module: %s",
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(presentAddr)),
			modName);
	}

	// Patch vtable slot 8 in-place.
	// g_originalPresent stores whatever was there (may already be Steam's hook --
	// that is fine; calling it will chain through Steam -> real Present).
	void** vtable = *reinterpret_cast<void***>(sc);
	g_vtableSlot = &vtable[8];
	g_originalPresent = reinterpret_cast<PresentFn>(vtable[8]);

	DWORD oldProtect = 0;
	bool  ok = false;
	if (VirtualProtect(g_vtableSlot, sizeof(void*), PAGE_READWRITE, &oldProtect))
	{
		*g_vtableSlot = reinterpret_cast<void*>(&HookedPresent);
		VirtualProtect(g_vtableSlot, sizeof(void*), oldProtect, &oldProtect);
		LogToFile::Info("[ImGuiBackend] vtable[8] patched -> HookedPresent (original=0x%llX)",
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_originalPresent)));
		ok = true;
	}
	else
	{
		LogToFile::Error("[ImGuiBackend] VirtualProtect failed -- cannot patch Present");
		g_vtableSlot = nullptr;
		g_originalPresent = nullptr;
	}

	// Patch ID3D12CommandQueue vtable[10] = ExecuteCommandLists with HookedECL.
	// The vtable is shared across all instances on the same d3d12.dll, so patching
	// from the temporary queue affects UE5's and Streamline's queues as well.
	if (ok)
	{
		void** qvtable = *reinterpret_cast<void***>(queue);
		g_eclVtableSlot = &qvtable[10];
		g_originalECL = reinterpret_cast<ExecCmdListsFn>(qvtable[10]);

		{
			char eclModName[MAX_PATH] = "<unknown>";
			HMODULE hEclMod = nullptr;
			if (GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCSTR>(static_cast<void*>(g_originalECL)), &hEclMod))
				GetModuleFileNameA(hEclMod, eclModName, sizeof(eclModName));
			LogToFile::Info("[ImGuiBackend] ID3D12CommandQueue::ECL vtable[10] = 0x%llX  module: %s",
				static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_originalECL)), eclModName);
		}

		DWORD eclProtect = 0;
		if (VirtualProtect(g_eclVtableSlot, sizeof(void*), PAGE_READWRITE, &eclProtect))
		{
			*g_eclVtableSlot = reinterpret_cast<void*>(&HookedECL);
			VirtualProtect(g_eclVtableSlot, sizeof(void*), eclProtect, &eclProtect);
			LogToFile::Info("[ImGuiBackend] ECL vtable[10] patched -> HookedECL");
		}
		else
		{
			LogToFile::Error("[ImGuiBackend] VirtualProtect failed for ECL vtable -- cross-queue sync disabled");
			g_eclVtableSlot = nullptr;
			g_originalECL = nullptr;
		}
	}

	// Patch IDXGIFactory2::CreateSwapChainForHwnd vtable[15] with HookedCreateSwapChainForHwnd.
	// The vtable is shared across all IDXGIFactory2 instances from the same dxgi.dll, so
	// patching from our temporary factory intercepts UE5's real swap chain creation.
	if (ok)
	{
		IDXGIFactory2* factory2 = nullptr;
		if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))))
		{
			void** fvtable = *reinterpret_cast<void***>(factory2);
			g_cscfhVtableSlot = &fvtable[15];
			g_originalCSCFH = reinterpret_cast<CreateSCFHFn>(fvtable[15]);

			{
				char cscfhModName[MAX_PATH] = "<unknown>";
				HMODULE hCscfhMod = nullptr;
				if (GetModuleHandleExA(
					GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					static_cast<LPCSTR>(static_cast<void*>(g_originalCSCFH)), &hCscfhMod))
					GetModuleFileNameA(hCscfhMod, cscfhModName, sizeof(cscfhModName));
				LogToFile::Info("[ImGuiBackend] IDXGIFactory2::CSCFH vtable[15] = 0x%llX  module: %s",
					static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_originalCSCFH)), cscfhModName);
			}

			DWORD cscfhProtect = 0;
			if (VirtualProtect(g_cscfhVtableSlot, sizeof(void*), PAGE_READWRITE, &cscfhProtect))
			{
				*g_cscfhVtableSlot = reinterpret_cast<void*>(&HookedCreateSwapChainForHwnd);
				VirtualProtect(g_cscfhVtableSlot, sizeof(void*), cscfhProtect, &cscfhProtect);
				LogToFile::Info("[ImGuiBackend] CSCFH vtable[15] patched -> HookedCreateSwapChainForHwnd");
			}
			else
			{
				LogToFile::Error("[ImGuiBackend] VirtualProtect failed for CSCFH vtable -- creation queue capture disabled");
				g_cscfhVtableSlot = nullptr;
				g_originalCSCFH = nullptr;
			}
			factory2->Release();
		}
		else
		{
			LogToFile::Error("[ImGuiBackend] QueryInterface(IDXGIFactory2) failed -- creation queue capture disabled");
		}
	}

	// Release all temporary objects -- only needed for vtable extraction.
	sc->Release();
	queue->Release();
	device->Release();
	factory->Release();
	return ok;
}

// ---------------------------------------------------------------------------
// RebuildFontAtlas
// Loads the font selected in GlobalSettings::GetFontFamily(), rebuilds the
// ImGui font atlas, and re-uploads the font texture to the GPU.
//
// When called during initial setup (g_initialized == false): skips the
// GPU-idle wait and InvalidateDeviceObjects -- the backend is freshly inited
// so no old texture exists to destroy.
//
// When called mid-session (g_pendingFontRebuild flag): waits for GPU idle,
// invalidates the existing texture, rebuilds, and recreates.
// ---------------------------------------------------------------------------
static void RebuildFontAtlas(bool initialSetup)
{
	if (!initialSetup)
	{
		// Wait for the GPU to finish all pending work before destroying resources.
		UINT64 signalVal = ++g_fenceValue;
		g_cmdQueue->Signal(g_fence, signalVal);
		if (g_fence->GetCompletedValue() < signalVal)
		{
			g_fence->SetEventOnCompletion(signalVal, g_fenceEvent);
			WaitForSingleObject(g_fenceEvent, 1000);
		}
		ImGui_ImplDX12_InvalidateDeviceObjects();
	}

	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();

	const char* family = UI::GlobalSettings::GetFontFamily();

	// Find the matching font entry (fall back to Default if not found).
	const FontEntry* entry = &k_fonts[0];
	for (int i = 0; i < k_fontCount; ++i)
	{
		if (strcmp(k_fonts[i].iniKey, family) == 0)
		{
			entry = &k_fonts[i];
			break;
		}
	}

	// Base font size loaded into the atlas.  FontScaleMain scales on top of this.
	const float kBasePx = 15.0f;

	if (entry->primaryPath == nullptr)
	{
		// Built-in: let ImGui add its default font automatically.
		// (Fonts->Clear() above means it will be re-added by NewFrame if empty.)
		io.Fonts->AddFontDefault();
	}
	else
	{
		// Glyph ranges -- static so they outlive Build().
		static const ImWchar s_latinCyrillic[] =
		{
			0x0020, 0x00FF, // Basic Latin + Latin Supplement
			0x0400, 0x052F, // Cyrillic + Supplement
			0
		};

		// Check file exists before trying to load.
		char narrowPath[MAX_PATH] = {};
		WideCharToMultiByte(CP_UTF8, 0, entry->primaryPath, -1, narrowPath, MAX_PATH, nullptr, nullptr);

		ImFont* baseFont = io.Fonts->AddFontFromFileTTF(narrowPath, kBasePx, nullptr, s_latinCyrillic);
		if (!baseFont)
		{
			LogToFile::Warn("[ImGuiBackend] Failed to load font '%s' -- falling back to built-in", narrowPath);
			io.Fonts->Clear();
			io.Fonts->AddFontDefault();
		}
		else if (entry->mergePath != nullptr)
		{
			// Merge CJK glyphs on top of the base font.
			// Ranges defined inline -- avoids relying on deprecated ImGui helper functions.
			static const ImWchar s_cjkRanges[] =
			{
				0x2000, 0x206F, // General Punctuation
				0x3000, 0x30FF, // CJK Symbols, Hiragana, Katakana
				0x31F0, 0x31FF, // Katakana Phonetic Extensions
				0xFF00, 0xFFEF, // Halfwidth/Fullwidth Forms
				0x4E00, 0x9FAF, // CJK Unified Ideographs (common subset)
				0
			};

			char mergePathNarrow[MAX_PATH] = {};
			WideCharToMultiByte(CP_UTF8, 0, entry->mergePath, -1, mergePathNarrow, MAX_PATH, nullptr, nullptr);

			ImFontConfig mergeCfg;
			mergeCfg.MergeMode = true;
			mergeCfg.PixelSnapH = true;
			io.Fonts->AddFontFromFileTTF(mergePathNarrow, kBasePx, &mergeCfg, s_cjkRanges);
		}
	}

	ImGui_ImplDX12_CreateDeviceObjects();

	// Re-apply the font scale since CreateDeviceObjects resets ImGui internals.
	ImGui::GetStyle().FontScaleMain = UI::GlobalSettings::GetFontScale();

	LogToFile::Info("[ImGuiBackend] Font atlas rebuilt: family='%s'", family);
}

// ---------------------------------------------------------------------------
// InitD3D12Resources
// Called once from HookedPresent after g_renderingReady is set.
// Gets UE5's device from the back buffer and creates all rendering objects on it.
// ---------------------------------------------------------------------------
static bool InitD3D12Resources(IDXGISwapChain* swapChain)
{
	DXGI_SWAP_CHAIN_DESC scDesc = {};
	if (FAILED(swapChain->GetDesc(&scDesc)))
	{
		LogToFile::Error("[ImGuiBackend] Failed to get swap chain desc");
		return false;
	}

	// Skip Streamline's small internal swap chains -- only init on the real game viewport.
	if (scDesc.BufferDesc.Width < 640 || scDesc.BufferDesc.Height < 480)
	{
		LogToFile::Info("[ImGuiBackend] Skipping small swap chain (%ux%u)",
			scDesc.BufferDesc.Width, scDesc.BufferDesc.Height);
		return false;
	}

	g_frameCount = scDesc.BufferCount;
	if (g_frameCount > MAX_FRAMES) g_frameCount = MAX_FRAMES;
	g_hwnd = scDesc.OutputWindow;
	g_swapChain = swapChain;

	// Select the queue to submit ImGui commands on BEFORE touching D3D12 resources.
	// This must happen first to avoid a COM reference leak: if we set g_device via
	// bb->GetDevice() and then return false (queue not ready), the next call would
	// AddRef g_device again without releasing the previous reference.
	//
	// Queue selection strategy:
	//   Streamline active  -> g_capturedQueue (last DIRECT ECL before Present):
	//                         Streamline's composition queue is the one that touched
	//                         the back buffer last; submitting elsewhere causes TDR.
	//   No Streamline      -> g_creationQueue (captured from CreateSwapChainForHwnd):
	//                         This is the queue DXGI implicitly syncs with on Present
	//                         and is always correct for plain UE5 / AMD / Intel setups.
	//   Fallback           -> g_capturedQueue if creation queue is somehow unavailable.
	ID3D12CommandQueue* chosenQueue = nullptr;
	if (g_streamlineActive)
	{
		chosenQueue = g_capturedQueue;
		if (chosenQueue)
			LogToFile::Info("[ImGuiBackend] Streamline active -- using ECL-captured queue 0x%p",
				static_cast<void*>(chosenQueue));
	}
	else
	{
		chosenQueue = g_creationQueue ? g_creationQueue : g_capturedQueue;
		if (chosenQueue)
			LogToFile::Info("[ImGuiBackend] No Streamline -- using creation queue 0x%p",
				static_cast<void*>(chosenQueue));
	}

	if (!chosenQueue)
	{
		// Neither hook has fired yet.  Return false WITHOUT setting g_device so
		// HookedPresent retries next frame instead of permanently disabling ImGui.
		LogToFile::Debug("[ImGuiBackend] Queue not yet captured -- deferring D3D12 init");
		return false;
	}

	// Get UE5's device and the real GPU resource format from back buffer 0.
	// Streamline wraps IDXGISwapChain::GetDesc and may return an internal format
	// rather than the real DXGI_FORMAT.  ID3D12Resource::GetDesc() bypasses the
	// wrapper and gives us the true format that CreateRenderTargetView needs.
	ID3D12Resource* bb = nullptr;
	if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&bb))))
	{
		LogToFile::Error("[ImGuiBackend] Failed to get back buffer 0");
		return false;
	}
	D3D12_RESOURCE_DESC bbResDesc = bb->GetDesc();
	HRESULT hr = bb->GetDevice(IID_PPV_ARGS(&g_device));
	bb->Release();

	g_rtvFormat = NormalizeForImGui(bbResDesc.Format);
	LogToFile::Info("[ImGuiBackend] SwapChain: size=%ux%u buffers=%u fmt=%u->%u effect=%u flags=0x%X",
		scDesc.BufferDesc.Width, scDesc.BufferDesc.Height,
		scDesc.BufferCount,
		static_cast<unsigned>(scDesc.BufferDesc.Format),
		static_cast<unsigned>(g_rtvFormat),
		static_cast<unsigned>(scDesc.SwapEffect),
		scDesc.Flags);

	if (FAILED(hr))
	{
		LogToFile::Error("[ImGuiBackend] Failed to get D3D12 device from back buffer");
		return false;
	}

	g_cmdQueue = chosenQueue;  // borrowed -- do NOT Release in cleanup

	// Log whether the two capture methods agree -- mismatch on a non-Streamline
	// system means the ECL heuristic saw a different queue than DXGI was told to use.
	if (g_creationQueue && g_capturedQueue)
		LogToFile::Info("[ImGuiBackend] Queue check: creation=0x%p  ecl-captured=0x%p  match=%s",
			static_cast<void*>(g_creationQueue),
			static_cast<void*>(g_capturedQueue),
			g_creationQueue == g_capturedQueue ? "YES" : "no");

	// Determine NodeMask from the device's node count.
	// NodeMask=1 is correct for all single-GPU systems (node 0).
	// On multi-GPU setups we log a warning and still use 1 (primary node);
	// multi-node heap creation requires per-node adapter enumeration which
	// is out of scope for now.
	UINT nodeCount = g_device->GetNodeCount();
	UINT nodeMask = 1u;
	LogToFile::Info("[ImGuiBackend] D3D12 node count: %u  NodeMask=0x%X", nodeCount, nodeMask);
	if (nodeCount > 1)
		LogToFile::Info("[ImGuiBackend] Multi-GPU system detected (%u nodes) -- using NodeMask=1 (primary). "
			"If descriptor heap creation fails, report your GPU configuration.", nodeCount);

	// Per-frame fence (prevents reusing an allocator while GPU is still busy).
	if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
	{
		LogToFile::Error("[ImGuiBackend] Failed to create fence");
		return false;
	}
	g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!g_fenceEvent)
	{
		LogToFile::Error("[ImGuiBackend] Failed to create fence event");
		return false;
	}


	// RTV descriptor heap (one slot per back buffer).
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		desc.NumDescriptors = g_frameCount;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		desc.NodeMask = nodeMask;
		if (FAILED(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_rtvHeap))))
		{
			LogToFile::Error("[ImGuiBackend] Failed to create RTV heap");
			return false;
		}
		g_rtvDescSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	}

	// SRV descriptor heap: slot 0 = ImGui font atlas, slots 1..MAX_PLUGIN_TEXTURES = plugin textures.
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.NumDescriptors = 1 + MAX_PLUGIN_TEXTURES;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		desc.NodeMask = nodeMask;
		if (FAILED(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_srvHeap))))
		{
			LogToFile::Error("[ImGuiBackend] Failed to create SRV heap");
			return false;
		}
		g_srvDescSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	// Per-frame command allocators.
	for (UINT i = 0; i < g_frameCount; ++i)
	{
		if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&g_frames[i].commandAllocator))))
		{
			LogToFile::Error("[ImGuiBackend] Failed to create command allocator %u", i);
			return false;
		}
	}

	// Command list (starts closed -- Reset() is called each frame before recording).
	if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		g_frames[0].commandAllocator, nullptr, IID_PPV_ARGS(&g_cmdList))) ||
		FAILED(g_cmdList->Close()))
	{
		LogToFile::Error("[ImGuiBackend] Failed to create command list");
		return false;
	}

	// ImGui context and backends.
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard
		| ImGuiConfigFlags_NoMouseCursorChange; // don't touch OS cursor by default
	io.IniFilename = "modloader_imgui.ini";  // persists all window positions/sizes between sessions

	ImGui::StyleColorsDark();

	ImGui_ImplWin32_Init(g_hwnd);

	// SRV alloc/free callbacks: we own a 1-slot heap; slot 0 is permanently
	// assigned to the ImGui font texture.
	static auto SrvAllocFn = [](ImGui_ImplDX12_InitInfo*,
		D3D12_CPU_DESCRIPTOR_HANDLE* cpu,
		D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
		{
			*cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
			*gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
		};
	static auto SrvFreeFn = [](ImGui_ImplDX12_InitInfo*,
		D3D12_CPU_DESCRIPTOR_HANDLE,
		D3D12_GPU_DESCRIPTOR_HANDLE) {};

	ImGui_ImplDX12_InitInfo dx12info = {};
	dx12info.Device = g_device;
	dx12info.CommandQueue = g_cmdQueue;
	dx12info.NumFramesInFlight = static_cast<int>(g_frameCount);
	dx12info.RTVFormat = g_rtvFormat;
	dx12info.SrvDescriptorHeap = g_srvHeap;
	dx12info.SrvDescriptorAllocFn = SrvAllocFn;
	dx12info.SrvDescriptorFreeFn = SrvFreeFn;
	ImGui_ImplDX12_Init(&dx12info);

	// Build the font atlas for the user's chosen font family.
	RebuildFontAtlas(/*initialSetup=*/true);

	// Subclass the game HWND for mouse/keyboard forwarding.
	g_origWndProc = reinterpret_cast<WNDPROC>(
		SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(HookedWndProc)));

	LogToFile::Info("[ImGuiBackend] D3D12 resources initialized (buffers=%u hwnd=0x%llX)",
		g_frameCount,
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_hwnd)));

	g_initialized = true;
	return true;
}

// ---------------------------------------------------------------------------
// CleanupD3D12Resources
// ---------------------------------------------------------------------------
static void CleanupD3D12Resources()
{
	if (g_origWndProc && g_hwnd)
	{
		SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_origWndProc));
		g_origWndProc = nullptr;
	}

	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	for (UINT i = 0; i < g_frameCount; ++i)
	{
		if (g_frames[i].commandAllocator)
		{
			g_frames[i].commandAllocator->Release();
			g_frames[i].commandAllocator = nullptr;
		}
	}
	if (g_cmdList) { g_cmdList->Release();              g_cmdList = nullptr; }
	g_cmdQueue = nullptr;        // borrowed from g_capturedQueue -- do NOT Release
	if (g_fence) { g_fence->Release();                g_fence = nullptr; }
	if (g_fenceEvent) { CloseHandle(g_fenceEvent);         g_fenceEvent = nullptr; }
	if (g_rtvHeap) { g_rtvHeap->Release();              g_rtvHeap = nullptr; }

	// Release all plugin texture resources before the SRV heap.
	{
		std::lock_guard<std::mutex> lock(g_textureMutex);
		for (int i = 0; i < MAX_PLUGIN_TEXTURES; ++i)
		{
			if (g_pluginTextures[i].inUse && g_pluginTextures[i].resource)
				g_pluginTextures[i].resource->Release();
			g_pluginTextures[i] = {};
		}
	}
	if (g_wicFactory) { g_wicFactory->Release(); g_wicFactory = nullptr; }

	if (g_srvHeap) { g_srvHeap->Release();              g_srvHeap = nullptr; }
	if (g_device) { g_device->Release();               g_device = nullptr; }
	g_capturedQueue = nullptr;   // weak ref -- do NOT Release
	g_swapChain = nullptr;
	g_fenceValue = 0;
	g_rtvFormat = DXGI_FORMAT_UNKNOWN;
	g_srvDescSize = 0;
}

// ---------------------------------------------------------------------------
// ID3D12CommandQueue::ExecuteCommandLists hook
//
// Pure passthrough -- we only use it to capture the first DIRECT command queue
// seen, which is the main rendering queue used by UE5/Streamline.
//
// Why capture here?  Streamline synchronises its DLSS queue back to UE5's main
// queue internally before calling Present, so signalling g_crossFence from
// g_capturedQueue inside HookedPresent is guaranteed to be after all of
// Streamline's GPU work for that frame.
// ---------------------------------------------------------------------------
static void STDMETHODCALLTYPE HookedECL(ID3D12CommandQueue* pQueue,
	UINT                 NumCmdLists,
	ID3D12CommandList* const* ppCmdLists)
{
	// Track the most recently seen DIRECT queue (see g_capturedQueue comment above).
	if (pQueue)
	{
		D3D12_COMMAND_QUEUE_DESC d = pQueue->GetDesc();
		if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
		{
			ID3D12CommandQueue* prev = g_capturedQueue;
			g_capturedQueue = pQueue;

			// Log if the queue changes post-init.
			// On Streamline systems this can happen when the game transitions
			// state (e.g. loading -> main menu), exposing a new internal queue.
			// g_cmdQueue is locked at init time and unaffected -- this log helps
			// confirm whether queue instability is the source of device removal.
			/*
			 * This is really spammy, even for TRACE
			if (g_initialized && prev && prev != pQueue)
				LogToFile::Trace("[ImGuiBackend] ECL queue changed post-init: "
					"old=0x%p  new=0x%p  submit queue (g_cmdQueue=0x%p) unchanged",
					static_cast<void*>(prev),
					static_cast<void*>(pQueue),
					static_cast<void*>(g_cmdQueue));
					*/
		}
	}
	g_originalECL(pQueue, NumCmdLists, ppCmdLists);
}

// ---------------------------------------------------------------------------
// IDXGISwapChain::Present hook
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
{
	if (g_shutdown)
		return g_originalPresent(swapChain, syncInterval, flags);

	// Reentrancy guard: prevents both recursive calls (Streamline calls the real
	// Present from within its own Present wrapper) and simultaneous calls from
	// multiple threads (some middleware and overlay SDKs call Present off the
	// render thread).  An atomic owner-thread check handles both cases.
	static std::atomic<DWORD> s_presentOwnerThread{ 0 };
	DWORD tid = GetCurrentThreadId();
	DWORD expected = 0;
	if (!s_presentOwnerThread.compare_exchange_strong(expected, tid))
		return g_originalPresent(swapChain, syncInterval, flags);

	// Wait for WorldBeginPlay before touching D3D12.
	// Streamline and the UE5 viewport finish their setup only after the first world loads.
	if (!g_initialized)
	{
		if (!g_renderingReady)
		{
			s_presentOwnerThread.store(0, std::memory_order_release);
			return g_originalPresent(swapChain, syncInterval, flags);
		}

		if (!InitD3D12Resources(swapChain))
		{
			if (g_device)
			{
				LogToFile::Error("[ImGuiBackend] Resource init failed -- ImGui disabled");
				g_shutdown = true;
			}
			// else: size filter rejected this swap chain, try again next frame.
		}
		s_presentOwnerThread.store(0, std::memory_order_release);
		return g_originalPresent(swapChain, syncInterval, flags);
	}

	// Only render on the swap chain we initialised on.
	if (swapChain != g_swapChain)
	{
		s_presentOwnerThread.store(0, std::memory_order_release);
		return g_originalPresent(swapChain, syncInterval, flags);
	}

	// s_presentOwnerThread is now set to tid -- release it on every exit path below.

	static std::atomic<UINT64> s_renderFrame{ 0 };
	UINT64 frameIdx = s_renderFrame.fetch_add(1);

	if (frameIdx == 0)
		LogToFile::Info("[ImGuiBackend] First render: device=0x%p submit-queue=0x%p ecl-current=0x%p buffers=%u fmt=%u",
			static_cast<void*>(g_device), static_cast<void*>(g_cmdQueue),
			static_cast<void*>(g_capturedQueue),
			g_frameCount, static_cast<unsigned>(g_rtvFormat));

	// Current back buffer index.
	IDXGISwapChain3* sc3 = nullptr;
	UINT bufferIdx = 0;
	if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3))))
	{
		bufferIdx = sc3->GetCurrentBackBufferIndex() % g_frameCount;
		sc3->Release();
	}
	FrameContext& fc = g_frames[bufferIdx];

	// Wait for the GPU to finish any previous work on this allocator.
	if (g_fence->GetCompletedValue() < fc.fenceValue)
	{
		g_fence->SetEventOnCompletion(fc.fenceValue, g_fenceEvent);
		WaitForSingleObject(g_fenceEvent, 1000);
	}

	// Acquire the back buffer just for this frame -- released before returning
	// so that ResizeBuffers is never blocked by our reference.
	ID3D12Resource* backBuffer = nullptr;
	if (FAILED(swapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(&backBuffer))))
	{
		LogToFile::Error("[ImGuiBackend] Frame %llu  GetBuffer(%u) failed", frameIdx, bufferIdx);
		s_presentOwnerThread.store(0, std::memory_order_release);
		return g_originalPresent(swapChain, syncInterval, flags);
	}

	// RTV for this buffer slot.
	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = {
		g_rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + bufferIdx * g_rtvDescSize
	};
	{
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = g_rtvFormat;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		g_device->CreateRenderTargetView(backBuffer, &rtvDesc, rtvHandle);
	}

	// Record ImGui commands.
	fc.commandAllocator->Reset();
	g_cmdList->Reset(fc.commandAllocator, nullptr);

	// Barrier: COMMON -> RENDER_TARGET.
	// Use COMMON (== PRESENT, value 0) as StateBefore: swap chain back buffers
	// decay to COMMON after each ExecuteCommandLists boundary.  The queue we
	// submit on may not have explicitly transitioned this buffer before, so the
	// runtime tracks it as COMMON.  Using PRESENT as StateBefore on such a queue
	// causes a validation mismatch that crashes the GPU driver.
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = backBuffer;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	g_cmdList->ResourceBarrier(1, &barrier);

	// Cursor management: hand control to ImGui whenever any modloader UI
	// surface is visible (main window OR any open plugin panel).
	// NoMouseCursorChange prevents ImGui's Win32 backend from calling
	// SetCursor() every frame -- without it, it fights UE5's cursor management
	// and causes visible flickering whenever our window is closed.
	bool uiOpen = ShouldCaptureInput();
	ImGuiIO& frameIO = ImGui::GetIO();
	if (uiOpen)
	{
		frameIO.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
		frameIO.MouseDrawCursor = true;
		ClipCursor(nullptr);
	}
	else
	{
		frameIO.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		frameIO.MouseDrawCursor = false;
	}

	// Rebuild font atlas if the user changed the font family.
	// Must happen before NewFrame to avoid using a destroyed font texture.
	if (g_pendingFontRebuild.exchange(false))
		RebuildFontAtlas(/*initialSetup=*/false);

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	UI::Overlay::Render();
	UI::Overlay::RenderHud();
	UI::ModLoaderWindow::Render(&g_imguiAPI);
	UI::PluginPanelRegistry::RenderPanelWindows(&g_imguiAPI);
	UI::PluginWidgetRegistry::RenderWidgets(&g_imguiAPI);

	ImGui::Render();

	g_cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
	g_cmdList->SetDescriptorHeaps(1, &g_srvHeap);
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdList);

	// Barrier: RENDER_TARGET -> COMMON.
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	g_cmdList->ResourceBarrier(1, &barrier);
	g_cmdList->Close();

	// Submit on g_cmdQueue -- the queue locked in at InitD3D12Resources time.
	// Do NOT use the live g_capturedQueue here: it is updated on every ECL call
	// from any thread and can change mid-session (e.g. loading -> main menu
	// transition exposes a new Streamline-internal queue).  Submitting a fence
	// signal from a queue on a different device causes DXGI_ERROR_DEVICE_REMOVED.
	// g_cmdQueue was verified against the swap chain back buffer's device at init
	// and will not change for the lifetime of this swap chain.
	ID3D12CommandQueue* submitQueue = g_cmdQueue;
	ID3D12CommandList* cmdLists[] = { g_cmdList };
	submitQueue->ExecuteCommandLists(1, cmdLists);

	// Signal fence then CPU-wait: ensures our GPU work is complete before DXGI
	// presents.  DXGI only implicitly waits on the swap chain's creation queue;
	// without this wait our RENDER_TARGET->COMMON barrier may still be in flight.
	UINT64 signalVal = ++g_fenceValue;
	submitQueue->Signal(g_fence, signalVal);
	fc.fenceValue = signalVal;
	if (g_fence->GetCompletedValue() < signalVal)
	{
		g_fence->SetEventOnCompletion(signalVal, g_fenceEvent);
		WaitForSingleObject(g_fenceEvent, 500);
	}

	backBuffer->Release();

	HRESULT hr = g_originalPresent(swapChain, syncInterval, flags);

	if (FAILED(hr))
	{
		LogToFile::Error("[ImGuiBackend] g_originalPresent failed: 0x%08X (frame %llu)",
			static_cast<unsigned>(hr), frameIdx);

		// On device removal, shut ImGui down immediately and surface a splash
		// error. Use a flag so this only fires once even if Present keeps firing.
		static std::atomic<bool> s_deviceLostNotified{ false };
		bool expected = false;
		if (s_deviceLostNotified.compare_exchange_strong(expected, true))
		{
			g_shutdown = true;

			// Proactively write [UI] Enabled=0 to modloader.ini so the overlay
			// is disabled automatically on next launch -- no manual editing needed.
			wchar_t iniPath[MAX_PATH]{};
			GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
			wchar_t* lastSlash = wcsrchr(iniPath, L'\\');
			if (lastSlash)
				wcscpy_s(lastSlash + 1,
					static_cast<rsize_t>(MAX_PATH - (lastSlash + 1 - iniPath)),
					L"modloader.ini");
			WritePrivateProfileStringW(L"UI", L"Enabled", L"0", iniPath);

			LogToFile::Error("[ImGuiBackend] ImGui disabled. "
				"[UI] Enabled=0 written to modloader.ini -- overlay off on next launch.");

			// Spawn a dedicated thread to own and pump the error splash.
			// The window MUST be created on the thread that pumps its messages --
			// creating it here on the render thread would leave it unresponsive
			// because the render thread never runs a GetMessage loop.
			CreateThread(nullptr, 0, [](void*) -> DWORD
			{
				Splash::Show();
				Splash::SetErrorMode();
				Splash::SetStatus(L"ImGui error: GPU device lost. Overlay disabled for next launch.");
				// Pump messages until ExitProcess (Close button) kills the process.
				MSG msg;
				while (GetMessageW(&msg, nullptr, 0, 0))
				{
					TranslateMessage(&msg);
					DispatchMessageW(&msg);
				}
				return 0;
			}, nullptr, 0, nullptr);
		}
		else
		{
			g_shutdown = true;
		}
	}

	s_presentOwnerThread.store(0, std::memory_order_release);
	return hr;
}

// ---------------------------------------------------------------------------
// IModLoaderImGui function table implementations
// ---------------------------------------------------------------------------
namespace ImGuiWrappers
{
	static void Text(const char* t) { ImGui::TextUnformatted(t); }
	static void TextColored(float r, float g, float b, float a, const char* t) { ImGui::TextColored(ImVec4(r, g, b, a), "%s", t); }
	static void TextDisabled(const char* t) { ImGui::TextDisabled("%s", t); }
	static void TextWrapped(const char* t) { ImGui::TextWrapped("%s", t); }
	static void LabelText(const char* l, const char* t) { ImGui::LabelText(l, "%s", t); }
	static void SeparatorText(const char* l) { ImGui::SeparatorText(l); }

	static bool InputText(const char* l, char* buf, size_t sz) { return ImGui::InputText(l, buf, sz); }
	static bool InputInt(const char* l, int* v, int s, int sf) { return ImGui::InputInt(l, v, s, sf); }
	static bool InputFloat(const char* l, float* v, float s, float sf, const char* fmt) { return ImGui::InputFloat(l, v, s, sf, fmt ? fmt : "%.3f"); }
	static bool Checkbox(const char* l, bool* v) { return ImGui::Checkbox(l, v); }
	static bool SliderFloat(const char* l, float* v, float mn, float mx, const char* f) { return ImGui::SliderFloat(l, v, mn, mx, f ? f : "%.3f"); }
	static bool SliderInt(const char* l, int* v, int mn, int mx, const char* f) { return ImGui::SliderInt(l, v, mn, mx, f ? f : "%d"); }

	static bool Button(const char* l) { return ImGui::Button(l); }
	static bool SmallButton(const char* l) { return ImGui::SmallButton(l); }

	static void SameLine(float o, float s) { ImGui::SameLine(o, s); }
	static void NewLine() { ImGui::NewLine(); }
	static void Separator() { ImGui::Separator(); }
	static void Spacing() { ImGui::Spacing(); }
	static void Indent(float w) { ImGui::Indent(w); }
	static void Unindent(float w) { ImGui::Unindent(w); }

	static void PushIDStr(const char* s) { ImGui::PushID(s); }
	static void PushIDInt(int i) { ImGui::PushID(i); }
	static void PopID() { ImGui::PopID(); }

	static bool BeginCombo(const char* l, const char* prev) { return ImGui::BeginCombo(l, prev); }
	static bool Selectable(const char* l, bool sel) { return ImGui::Selectable(l, sel); }
	static void EndCombo() { ImGui::EndCombo(); }

	static bool CollapsingHeader(const char* l) { return ImGui::CollapsingHeader(l); }
	static bool TreeNodeStr(const char* l) { return ImGui::TreeNode(l); }
	static void TreePop() { ImGui::TreePop(); }

	static bool ColorEdit3(const char* l, float c[3]) { return ImGui::ColorEdit3(l, c); }
	static bool ColorEdit4(const char* l, float c[4]) { return ImGui::ColorEdit4(l, c); }

	static void SetTooltip(const char* t) { ImGui::SetTooltip("%s", t); }
	static bool IsItemHovered() { return ImGui::IsItemHovered(); }
	static void SetNextItemWidth(float w) { ImGui::SetNextItemWidth(w); }

	static float GetFontSize() { return ImGui::GetFontSize(); }
	static float GetTextLineHeight() { return ImGui::GetTextLineHeight(); }
	static float GetTextLineHeightWithSpacing() { return ImGui::GetTextLineHeightWithSpacing(); }
	static float GetFrameHeight() { return ImGui::GetFrameHeight(); }
	static float GetFrameHeightWithSpacing() { return ImGui::GetFrameHeightWithSpacing(); }
	static void  CalcTextSize(const char* text, float* ox, float* oy, bool hide, float wrap)
	{
		ImVec2 s = ImGui::CalcTextSize(text, nullptr, hide, wrap);
		if (ox) *ox = s.x;
		if (oy) *oy = s.y;
	}
	static void GetContentRegionAvail(float* ox, float* oy)
	{
		ImVec2 v = ImGui::GetContentRegionAvail();
		if (ox) *ox = v.x;
		if (oy) *oy = v.y;
	}
	static void GetDisplaySize(float* ox, float* oy)
	{
		ImGuiIO& io = ImGui::GetIO();
		if (ox) *ox = io.DisplaySize.x;
		if (oy) *oy = io.DisplaySize.y;
	}
	static void SetWindowFontScale(float s) { ImGui::SetWindowFontScale(s); }

	// v35
	// v36 -- window queries
	static bool IsWindowAppearing() { return ImGui::IsWindowAppearing(); }
	static bool IsWindowCollapsed() { return ImGui::IsWindowCollapsed(); }
	static bool IsWindowFocused(int f) { return ImGui::IsWindowFocused((ImGuiFocusedFlags)f); }
	static bool IsWindowHovered(int f) { return ImGui::IsWindowHovered((ImGuiHoveredFlags)f); }
	static void GetWindowPos(float* ox, float* oy) { ImVec2 v = ImGui::GetWindowPos(); if (ox) *ox = v.x; if (oy) *oy = v.y; }
	static void GetWindowSize(float* ox, float* oy) { ImVec2 v = ImGui::GetWindowSize(); if (ox) *ox = v.x; if (oy) *oy = v.y; }
	static void SetNextWindowBgAlpha(float a) { ImGui::SetNextWindowBgAlpha(a); }

	// v36 -- scroll
	static float GetScrollX() { return ImGui::GetScrollX(); }
	static float GetScrollY() { return ImGui::GetScrollY(); }
	static void SetScrollX(float v) { ImGui::SetScrollX(v); }
	static void SetScrollY(float v) { ImGui::SetScrollY(v); }
	static float GetScrollMaxX() { return ImGui::GetScrollMaxX(); }
	static float GetScrollMaxY() { return ImGui::GetScrollMaxY(); }
	static void SetScrollHereX(float r) { ImGui::SetScrollHereX(r); }
	static void SetScrollHereY(float r) { ImGui::SetScrollHereY(r); }

	// v36 -- grouping / alignment
	static void BeginGroup() { ImGui::BeginGroup(); }
	static void EndGroup() { ImGui::EndGroup(); }
	static void AlignTextToFramePadding() { ImGui::AlignTextToFramePadding(); }

	// v36 -- extended cursor
	static float GetCursorPosY() { return ImGui::GetCursorPosY(); }
	static void SetCursorPosY(float y) { ImGui::SetCursorPosY(y); }
	static void SetCursorPos(float x, float y) { ImGui::SetCursorPos(ImVec2(x, y)); }
	static void GetCursorScreenPos(float* ox, float* oy) { ImVec2 v = ImGui::GetCursorScreenPos(); if (ox) *ox = v.x; if (oy) *oy = v.y; }
	static void SetCursorScreenPos(float x, float y) { ImGui::SetCursorScreenPos(ImVec2(x, y)); }
	static void GetCursorStartPos(float* ox, float* oy) { ImVec2 v = ImGui::GetCursorStartPos(); if (ox) *ox = v.x; if (oy) *oy = v.y; }
	static float CalcItemWidth() { return ImGui::CalcItemWidth(); }
	static void PushTextWrapPos(float x) { ImGui::PushTextWrapPos(x); }
	static void PopTextWrapPos() { ImGui::PopTextWrapPos(); }

	// v36 -- style extras
	static void PushStyleVarX(int idx, float x) { ImGui::PushStyleVarX((ImGuiStyleVar)idx, x); }
	static void PushStyleVarY(int idx, float y) { ImGui::PushStyleVarY((ImGuiStyleVar)idx, y); }
	static void PushItemFlag(int opt, bool en) { ImGui::PushItemFlag((ImGuiItemFlags)opt, en); }
	static void PopItemFlag() { ImGui::PopItemFlag(); }

	// v36 -- text helpers
	static void BulletText(const char* t) { ImGui::BulletText("%s", t); }
	static void Bullet() { ImGui::Bullet(); }

	// v36 -- buttons / widgets
	static bool ButtonSized(const char* l, float w, float h) { return ImGui::Button(l, ImVec2(w, h)); }
	static bool InvisibleButton(const char* id, float w, float h) { return ImGui::InvisibleButton(id, ImVec2(w, h)); }
	static bool ArrowButton(const char* id, int dir) { return ImGui::ArrowButton(id, (ImGuiDir)dir); }
	static bool RadioButton(const char* l, bool a) { return ImGui::RadioButton(l, a); }
	static bool RadioButtonInt(const char* l, int* v, int vb) { return ImGui::RadioButton(l, v, vb); }
	static void ProgressBar(float f, float w, float h, const char* ov) { ImGui::ProgressBar(f, ImVec2(w, h), ov); }
	static bool CheckboxFlagsInt(const char* l, int* f, int fv) { return ImGui::CheckboxFlags(l, f, fv); }
	static bool SelectableFull(const char* l, bool s, int f, float w, float h) { return ImGui::Selectable(l, s, (ImGuiSelectableFlags)f, ImVec2(w, h)); }
	static bool TextLink(const char* l) { return ImGui::TextLink(l); }

	// v36 -- input extras
	static bool InputTextMultiline(const char* l, char* buf, size_t sz, float w, float h) { return ImGui::InputTextMultiline(l, buf, sz, ImVec2(w, h)); }
	static bool InputTextWithHint(const char* l, const char* hint, char* buf, size_t sz) { return ImGui::InputTextWithHint(l, hint, buf, sz); }
	static bool InputFloat2(const char* l, float v[2]) { return ImGui::InputFloat2(l, v); }
	static bool InputFloat3(const char* l, float v[3]) { return ImGui::InputFloat3(l, v); }
	static bool InputFloat4(const char* l, float v[4]) { return ImGui::InputFloat4(l, v); }
	static bool InputInt2(const char* l, int v[2]) { return ImGui::InputInt2(l, v); }
	static bool InputInt3(const char* l, int v[3]) { return ImGui::InputInt3(l, v); }
	static bool InputInt4(const char* l, int v[4]) { return ImGui::InputInt4(l, v); }

	// v36 -- drag widgets
	static bool DragFloat(const char* l, float* v, float spd, float mn, float mx, const char* fmt) { return ImGui::DragFloat(l, v, spd, mn, mx, fmt ? fmt : "%.3f"); }
	static bool DragFloat2(const char* l, float v[2], float spd, float mn, float mx, const char* fmt) { return ImGui::DragFloat2(l, v, spd, mn, mx, fmt ? fmt : "%.3f"); }
	static bool DragFloat3(const char* l, float v[3], float spd, float mn, float mx, const char* fmt) { return ImGui::DragFloat3(l, v, spd, mn, mx, fmt ? fmt : "%.3f"); }
	static bool DragFloat4(const char* l, float v[4], float spd, float mn, float mx, const char* fmt) { return ImGui::DragFloat4(l, v, spd, mn, mx, fmt ? fmt : "%.3f"); }
	static bool DragInt(const char* l, int* v, float spd, int mn, int mx, const char* fmt) { return ImGui::DragInt(l, v, spd, mn, mx, fmt ? fmt : "%d"); }
	static bool DragInt2(const char* l, int v[2], float spd, int mn, int mx, const char* fmt) { return ImGui::DragInt2(l, v, spd, mn, mx, fmt ? fmt : "%d"); }
	static bool DragInt3(const char* l, int v[3], float spd, int mn, int mx, const char* fmt) { return ImGui::DragInt3(l, v, spd, mn, mx, fmt ? fmt : "%d"); }
	static bool DragInt4(const char* l, int v[4], float spd, int mn, int mx, const char* fmt) { return ImGui::DragInt4(l, v, spd, mn, mx, fmt ? fmt : "%d"); }
	static bool DragFloatRange2(const char* l, float* vmn, float* vmx, float spd, float mn, float mx, const char* fmt) { return ImGui::DragFloatRange2(l, vmn, vmx, spd, mn, mx, fmt ? fmt : "%.3f"); }
	static bool DragIntRange2(const char* l, int* vmn, int* vmx, float spd, int mn, int mx, const char* fmt) { return ImGui::DragIntRange2(l, vmn, vmx, spd, mn, mx, fmt ? fmt : "%d"); }

	// v36 -- vertical sliders + angle
	static bool VSliderFloat(const char* l, float w, float h, float* v, float mn, float mx, const char* fmt) { return ImGui::VSliderFloat(l, ImVec2(w, h), v, mn, mx, fmt ? fmt : "%.3f"); }
	static bool VSliderInt(const char* l, float w, float h, int* v, int mn, int mx, const char* fmt) { return ImGui::VSliderInt(l, ImVec2(w, h), v, mn, mx, fmt ? fmt : "%d"); }
	static bool SliderAngle(const char* l, float* vr, float dmn, float dmx) { return ImGui::SliderAngle(l, vr, dmn, dmx); }

	// v36 -- color pickers / button
	static bool ColorPicker3(const char* l, float c[3]) { return ImGui::ColorPicker3(l, c); }
	static bool ColorPicker4(const char* l, float c[4]) { return ImGui::ColorPicker4(l, c); }
	static bool ColorButton(const char* id, float r, float g, float b, float a, float w, float h) { return ImGui::ColorButton(id, ImVec4(r, g, b, a), 0, ImVec2(w, h)); }

	// v36 -- plot
	static void PlotLines(const char* l, const float* vs, int cnt, int off, const char* ov, float mn, float mx, float gw, float gh) { ImGui::PlotLines(l, vs, cnt, off, ov, mn, mx, ImVec2(gw, gh)); }
	static void PlotHistogram(const char* l, const float* vs, int cnt, int off, const char* ov, float mn, float mx, float gw, float gh) { ImGui::PlotHistogram(l, vs, cnt, off, ov, mn, mx, ImVec2(gw, gh)); }

	// v36 -- tree extras
	static bool TreeNodeExStr(const char* l, int f) { return ImGui::TreeNodeEx(l, (ImGuiTreeNodeFlags)f); }
	static void TreePushStr(const char* id) { ImGui::TreePush(id); }
	static float GetTreeNodeToLabelSpacing() { return ImGui::GetTreeNodeToLabelSpacing(); }
	static void SetNextItemOpen(bool open, int cond) { ImGui::SetNextItemOpen(open, (ImGuiCond)cond); }

	// v36 -- listbox
	static bool BeginListBox(const char* l, float w, float h) { return ImGui::BeginListBox(l, ImVec2(w, h)); }
	static void EndListBox() { ImGui::EndListBox(); }

	// v36 -- tab bar
	static bool BeginTabBar(const char* id, int f) { return ImGui::BeginTabBar(id, (ImGuiTabBarFlags)f); }
	static void EndTabBar() { ImGui::EndTabBar(); }
	static bool BeginTabItem(const char* l, bool* open, int f) { return ImGui::BeginTabItem(l, open, (ImGuiTabItemFlags)f); }
	static void EndTabItem() { ImGui::EndTabItem(); }
	static bool TabItemButton(const char* l, int f) { return ImGui::TabItemButton(l, (ImGuiTabItemFlags)f); }

	// v36 -- menus
	static bool BeginMenuBar() { return ImGui::BeginMenuBar(); }
	static void EndMenuBar() { ImGui::EndMenuBar(); }
	static bool BeginMenu(const char* l, bool en) { return ImGui::BeginMenu(l, en); }
	static void EndMenu() { ImGui::EndMenu(); }
	static bool MenuItem(const char* l, const char* sc, bool sel, bool en) { return ImGui::MenuItem(l, sc, sel, en); }

	// v36 -- popups
	static bool BeginPopup(const char* id, int f) { return ImGui::BeginPopup(id, (ImGuiWindowFlags)f); }
	static bool BeginPopupModal(const char* name, bool* open, int f) { return ImGui::BeginPopupModal(name, open, (ImGuiWindowFlags)f); }
	static void EndPopup() { ImGui::EndPopup(); }
	static void OpenPopup(const char* id, int f) { ImGui::OpenPopup(id, (ImGuiPopupFlags)f); }
	static void CloseCurrentPopup() { ImGui::CloseCurrentPopup(); }
	static bool BeginPopupContextItem(const char* id, int f) { return ImGui::BeginPopupContextItem(id, (ImGuiPopupFlags)f); }
	static bool BeginPopupContextWindow(const char* id, int f) { return ImGui::BeginPopupContextWindow(id, (ImGuiPopupFlags)f); }
	static bool IsPopupOpen(const char* id, int f) { return ImGui::IsPopupOpen(id, (ImGuiPopupFlags)f); }

	// v36 -- tooltips
	static bool BeginTooltip() { return ImGui::BeginTooltip(); }
	static void EndTooltip() { ImGui::EndTooltip(); }
	static bool BeginItemTooltip() { return ImGui::BeginItemTooltip(); }
	static void SetItemTooltip(const char* t) { ImGui::SetItemTooltip("%s", t); }

	// v36 -- table extras
	static void TableNextRow(int f, float h) { ImGui::TableNextRow((ImGuiTableRowFlags)f, h); }
	static bool TableSetColumnIndex(int col) { return ImGui::TableSetColumnIndex(col); }
	static void TableSetupColumn(const char* l, int f, float w) { ImGui::TableSetupColumn(l, (ImGuiTableColumnFlags)f, w); }
	static void TableSetupScrollFreeze(int cols, int rows) { ImGui::TableSetupScrollFreeze(cols, rows); }
	static void TableHeadersRow() { ImGui::TableHeadersRow(); }
	static void TableHeader(const char* l) { ImGui::TableHeader(l); }
	static int TableGetColumnCount() { return ImGui::TableGetColumnCount(); }
	static int TableGetColumnIndex() { return ImGui::TableGetColumnIndex(); }
	static int TableGetRowIndex() { return ImGui::TableGetRowIndex(); }
	static const char* TableGetColumnName(int col) { return ImGui::TableGetColumnName(col); }
	static void TableSetBgColor(int tgt, unsigned int col, int cn) { ImGui::TableSetBgColor((ImGuiTableBgTarget)tgt, col, cn); }

	// v36 -- item state predicates
	static bool IsItemActive() { return ImGui::IsItemActive(); }
	static bool IsItemFocused() { return ImGui::IsItemFocused(); }
	static bool IsItemVisible() { return ImGui::IsItemVisible(); }
	static bool IsItemEdited() { return ImGui::IsItemEdited(); }
	static bool IsItemActivated() { return ImGui::IsItemActivated(); }
	static bool IsItemDeactivated() { return ImGui::IsItemDeactivated(); }
	static bool IsItemDeactivatedAfterEdit() { return ImGui::IsItemDeactivatedAfterEdit(); }
	static bool IsItemToggledOpen() { return ImGui::IsItemToggledOpen(); }
	static bool IsAnyItemHovered() { return ImGui::IsAnyItemHovered(); }
	static bool IsAnyItemActive() { return ImGui::IsAnyItemActive(); }
	static bool IsAnyItemFocused() { return ImGui::IsAnyItemFocused(); }
	static void GetItemRectMin(float* ox, float* oy) { ImVec2 v = ImGui::GetItemRectMin(); if (ox) *ox = v.x; if (oy) *oy = v.y; }
	static void GetItemRectMax(float* ox, float* oy) { ImVec2 v = ImGui::GetItemRectMax(); if (ox) *ox = v.x; if (oy) *oy = v.y; }
	static void GetItemRectSize(float* ox, float* oy) { ImVec2 v = ImGui::GetItemRectSize(); if (ox) *ox = v.x; if (oy) *oy = v.y; }
	static void SetItemDefaultFocus() { ImGui::SetItemDefaultFocus(); }
	static void SetKeyboardFocusHere(int off) { ImGui::SetKeyboardFocusHere(off); }
	static void SetNextItemAllowOverlap() { ImGui::SetNextItemAllowOverlap(); }

	// v36 -- disabled regions
	static void BeginDisabled(bool d) { ImGui::BeginDisabled(d); }
	static void EndDisabled() { ImGui::EndDisabled(); }

	// v36 -- clip rect
	static void PushClipRect(float x0, float y0, float x1, float y1, bool intersect) { ImGui::PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), intersect); }
	static void PopClipRect() { ImGui::PopClipRect(); }

	// v36 -- mouse
	static bool IsMouseDown(int btn) { return ImGui::IsMouseDown(btn); }
	static bool IsMouseClicked(int btn, bool rep) { return ImGui::IsMouseClicked(btn, rep); }
	static bool IsMouseReleased(int btn) { return ImGui::IsMouseReleased(btn); }
	static bool IsMouseDoubleClicked(int btn) { return ImGui::IsMouseDoubleClicked(btn); }
	static void GetMousePos(float* ox, float* oy) { ImVec2 v = ImGui::GetMousePos(); if (ox) *ox = v.x; if (oy) *oy = v.y; }
	static bool IsMouseDragging(int btn, float thr) { return ImGui::IsMouseDragging(btn, thr); }
	static void GetMouseDragDelta(int btn, float thr, float* ox, float* oy) { ImVec2 v = ImGui::GetMouseDragDelta(btn, thr); if (ox) *ox = v.x; if (oy) *oy = v.y; }
	static void ResetMouseDragDelta(int btn) { ImGui::ResetMouseDragDelta(btn); }
	static void SetMouseCursor(int t) { ImGui::SetMouseCursor((ImGuiMouseCursor)t); }
	static bool IsMouseHoveringRect(float x0, float y0, float x1, float y1, bool clip) { return ImGui::IsMouseHoveringRect(ImVec2(x0, y0), ImVec2(x1, y1), clip); }

	// v36 -- color utilities
	static unsigned int GetColorU32FromCol(int idx, float am) { return ImGui::GetColorU32((ImGuiCol)idx, am); }
	static unsigned int GetColorU32FromVec4(float r, float g, float b, float a) { return ImGui::GetColorU32(ImVec4(r, g, b, a)); }
	static void GetStyleColorVec4(int idx, float* or_, float* og, float* ob, float* oa) { const ImVec4& c = ImGui::GetStyleColorVec4((ImGuiCol)idx); if (or_) *or_ = c.x; if (og) *og = c.y; if (ob) *ob = c.z; if (oa) *oa = c.w; }
	static void ColorConvertRGBtoHSV(float r, float g, float b, float* oh, float* os, float* ov) { ImGui::ColorConvertRGBtoHSV(r, g, b, *oh, *os, *ov); }
	static void ColorConvertHSVtoRGB(float h, float s, float v, float* or_, float* og, float* ob) { ImGui::ColorConvertHSVtoRGB(h, s, v, *or_, *og, *ob); }

	// v36 -- misc
	static double GetTime() { return ImGui::GetTime(); }
	static int GetFrameCount() { return ImGui::GetFrameCount(); }
	static bool IsRectVisible(float w, float h) { return ImGui::IsRectVisible(ImVec2(w, h)); }
	static const char* GetClipboardText() { return ImGui::GetClipboardText(); }
	static void SetClipboardText(const char* t) { ImGui::SetClipboardText(t); }
	static const char* GetStyleColorName(int idx) { return ImGui::GetStyleColorName((ImGuiCol)idx); }

	// v35 child windows (original placement)
	static bool BeginChild(const char* id, float sx, float sy, bool border)
	{
		return ImGui::BeginChild(id, ImVec2(sx, sy), border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None);
	}
	static void EndChild() { ImGui::EndChild(); }
	static void PushStyleColor(int idx, float r, float g, float b, float a) { ImGui::PushStyleColor(idx, ImVec4(r, g, b, a)); }
	static void PopStyleColor(int count) { ImGui::PopStyleColor(count); }
	static void PushStyleVarFloat(int idx, float val) { ImGui::PushStyleVar(idx, val); }
	static void PushStyleVarVec2(int idx, float x, float y) { ImGui::PushStyleVar(idx, ImVec2(x, y)); }
	static void PopStyleVar(int count) { ImGui::PopStyleVar(count); }
	static void PushItemWidth(float w) { ImGui::PushItemWidth(w); }
	static void PopItemWidth() { ImGui::PopItemWidth(); }
	static void SetCursorPosX(float x) { ImGui::SetCursorPosX(x); }
	static float GetCursorPosX() { return ImGui::GetCursorPosX(); }
	static bool BeginTable(const char* id, int cols, int flags) { return ImGui::BeginTable(id, cols, flags); }
	static void TableNextColumn() { ImGui::TableNextColumn(); }
	static void EndTable() { ImGui::EndTable(); }
	static bool IsItemClicked(int btn) { return ImGui::IsItemClicked(btn); }
	static float GetWindowWidth() { return ImGui::GetWindowWidth(); }
	static float GetWindowHeight() { return ImGui::GetWindowHeight(); }
	static void Dummy(float sx, float sy) { ImGui::Dummy(ImVec2(sx, sy)); }
}

// ---------------------------------------------------------------------------
// Plugin texture system (v37)
// ---------------------------------------------------------------------------

// Returns the lazily-created WIC factory; nullptr if WIC is unavailable.
static IWICImagingFactory* GetWICFactory()
{
	if (!g_wicFactory)
		CoCreateInstance(CLSID_WICImagingFactory, nullptr,
			CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wicFactory));
	return g_wicFactory;
}

// Upload RGBA pixels to a new shader-visible D3D12 texture.
// Writes an SRV into srvCpuHandle. Returns the texture resource on success.
// Creates a private temporary command queue so it never conflicts with the
// main render queue, and blocks until the GPU copy is complete before returning.
static ID3D12Resource* UploadRGBAToGPU(
	const uint8_t* rgba, int width, int height,
	D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle)
{
	if (!g_device || !rgba || width <= 0 || height <= 0)
		return nullptr;

	// Default heap texture
	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Width            = (UINT)width;
	texDesc.Height           = (UINT)height;
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels        = 1;
	texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.SampleDesc.Count = 1;
	texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

	ID3D12Resource* texResource = nullptr;
	if (FAILED(g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
		&texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texResource))))
		return nullptr;

	// Upload heap buffer
	UINT rowPitch   = ((UINT)width * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u)
	                  & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
	UINT uploadSize = (UINT)height * rowPitch;

	D3D12_HEAP_PROPERTIES uploadProps = {};
	uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;

	D3D12_RESOURCE_DESC uploadDesc = {};
	uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	uploadDesc.Width            = uploadSize;
	uploadDesc.Height           = 1;
	uploadDesc.DepthOrArraySize = 1;
	uploadDesc.MipLevels        = 1;
	uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
	uploadDesc.SampleDesc.Count = 1;
	uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ID3D12Resource* uploadBuffer = nullptr;
	if (FAILED(g_device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE,
		&uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer))))
	{
		texResource->Release();
		return nullptr;
	}

	// Copy pixels row-by-row to respect pitch alignment
	void* mapped = nullptr;
	D3D12_RANGE mapRange = { 0, uploadSize };
	if (FAILED(uploadBuffer->Map(0, &mapRange, &mapped)))
	{
		uploadBuffer->Release();
		texResource->Release();
		return nullptr;
	}
	for (int y = 0; y < height; ++y)
		memcpy((uint8_t*)mapped + (size_t)y * rowPitch,
		       rgba + (size_t)y * width * 4, (size_t)width * 4);
	uploadBuffer->Unmap(0, &mapRange);

	// Private command queue for the upload
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.NodeMask = 1;

	ID3D12CommandQueue*         tmpQueue = nullptr;
	ID3D12CommandAllocator*     tmpAlloc = nullptr;
	ID3D12GraphicsCommandList*  tmpList  = nullptr;
	ID3D12Fence*                tmpFence = nullptr;
	HANDLE                      tmpEvent = nullptr;

	bool ok =
		SUCCEEDED(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&tmpQueue))) &&
		SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tmpAlloc))) &&
		SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tmpAlloc, nullptr, IID_PPV_ARGS(&tmpList))) &&
		SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tmpFence)));

	if (ok) tmpEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	if (!ok || !tmpEvent)
	{
		if (tmpEvent) CloseHandle(tmpEvent);
		if (tmpFence) tmpFence->Release();
		if (tmpList)  tmpList->Release();
		if (tmpAlloc) tmpAlloc->Release();
		if (tmpQueue) tmpQueue->Release();
		uploadBuffer->Release();
		texResource->Release();
		return nullptr;
	}

	// Record copy + transition
	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource                          = uploadBuffer;
	src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
	src.PlacedFootprint.Footprint.Width    = (UINT)width;
	src.PlacedFootprint.Footprint.Height   = (UINT)height;
	src.PlacedFootprint.Footprint.Depth    = 1;
	src.PlacedFootprint.Footprint.RowPitch = rowPitch;

	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource        = texResource;
	dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource   = texResource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

	tmpList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	tmpList->ResourceBarrier(1, &barrier);
	tmpList->Close();

	tmpQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&tmpList);
	tmpQueue->Signal(tmpFence, 1);
	tmpFence->SetEventOnCompletion(1, tmpEvent);
	WaitForSingleObject(tmpEvent, INFINITE);

	// Tear down temporary infrastructure and upload buffer
	CloseHandle(tmpEvent);
	tmpFence->Release();
	tmpList->Release();
	tmpAlloc->Release();
	tmpQueue->Release();
	uploadBuffer->Release();

	// Create the shader resource view
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format                     = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels        = 1;
	srvDesc.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	g_device->CreateShaderResourceView(texResource, &srvDesc, srvCpuHandle);

	return texResource;
}

// Copy an engine-owned D3D12 texture into a new owned DEFAULT-heap resource.
// Barriers the engine resource to COPY_SOURCE on a private queue (if needed),
// performs CopyResource, restores the engine resource state, then transitions
// the new resource to PIXEL_SHADER_RESOURCE.  Writes an SRV into srvCpuHandle.
// Blocks until the GPU copy is complete.  Returns the new owned resource, or
// nullptr on failure.
//
// NOTE: the barrier transition of srcResource to COPY_SOURCE is issued on a
// private queue with no cross-queue fence against the engine's render queue.
// This is safe only when srcState is COMMON (implicit promotion, no barrier
// emitted) or when the engine has finished all GPU work on that resource for
// the current frame -- i.e. call this after a GPU-idle fence if in doubt.
static ID3D12Resource* CopyEngineTextureToOwned(
	ID3D12Resource*             srcResource,
	D3D12_RESOURCE_STATES       srcState,
	const D3D12_RESOURCE_DESC&  srcDesc,
	DXGI_FORMAT                 srvFormat,
	D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle)
{
	if (!g_device || !srcResource) return nullptr;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC dstDesc = srcDesc;
	dstDesc.Flags = D3D12_RESOURCE_FLAG_NONE;  // we only need to sample it

	ID3D12Resource* dstResource = nullptr;
	if (FAILED(g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE,
		&dstDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dstResource))))
		return nullptr;

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.NodeMask = 1;

	ID3D12CommandQueue*        tmpQueue = nullptr;
	ID3D12CommandAllocator*    tmpAlloc = nullptr;
	ID3D12GraphicsCommandList* tmpList  = nullptr;
	ID3D12Fence*               tmpFence = nullptr;
	HANDLE                     tmpEvent = nullptr;

	bool ok =
		SUCCEEDED(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&tmpQueue))) &&
		SUCCEEDED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tmpAlloc))) &&
		SUCCEEDED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tmpAlloc, nullptr, IID_PPV_ARGS(&tmpList))) &&
		SUCCEEDED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&tmpFence)));

	if (ok) tmpEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	if (!ok || !tmpEvent)
	{
		if (tmpEvent) CloseHandle(tmpEvent);
		if (tmpFence) tmpFence->Release();
		if (tmpList)  tmpList->Release();
		if (tmpAlloc) tmpAlloc->Release();
		if (tmpQueue) tmpQueue->Release();
		dstResource->Release();
		return nullptr;
	}

	// COMMON state resources implicitly promote to COPY_SOURCE -- no barrier needed.
	// Any other state requires an explicit transition and restore.
	const bool needSrcBarrier = (srcState != D3D12_RESOURCE_STATE_COMMON) &&
	                            (srcState != D3D12_RESOURCE_STATE_COPY_SOURCE);

	if (needSrcBarrier)
	{
		D3D12_RESOURCE_BARRIER b = {};
		b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource   = srcResource;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = srcState;
		b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
		tmpList->ResourceBarrier(1, &b);
	}

	tmpList->CopyResource(dstResource, srcResource);

	{
		D3D12_RESOURCE_BARRIER post[2] = {};
		UINT count = 0;

		if (needSrcBarrier)
		{
			post[count].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			post[count].Transition.pResource   = srcResource;
			post[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			post[count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
			post[count].Transition.StateAfter  = srcState;
			++count;
		}

		post[count].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		post[count].Transition.pResource   = dstResource;
		post[count].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		post[count].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		post[count].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		++count;

		tmpList->ResourceBarrier(count, post);
	}

	tmpList->Close();
	tmpQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&tmpList);
	tmpQueue->Signal(tmpFence, 1);
	tmpFence->SetEventOnCompletion(1, tmpEvent);
	WaitForSingleObject(tmpEvent, INFINITE);

	CloseHandle(tmpEvent);
	tmpFence->Release();
	tmpList->Release();
	tmpAlloc->Release();
	tmpQueue->Release();

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping        = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format                         = srvFormat;
	srvDesc.ViewDimension                  = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels            = srcDesc.MipLevels;

	g_device->CreateShaderResourceView(dstResource, &srvDesc, srvCpuHandle);

	return dstResource;
}

// Decode an image from an IWICBitmapSource (any format) to a raw RGBA pixel buffer.
// Returns heap-allocated pixels on success (caller delete[]s it); sets width/height.
static uint8_t* WICToRGBA(IWICBitmapSource* src, int* out_w, int* out_h)
{
	IWICImagingFactory* factory = GetWICFactory();
	if (!factory) return nullptr;

	IWICFormatConverter* conv = nullptr;
	if (FAILED(factory->CreateFormatConverter(&conv))) return nullptr;

	HRESULT hr = conv->Initialize(src, GUID_WICPixelFormat32bppRGBA,
		WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
	if (FAILED(hr)) { conv->Release(); return nullptr; }

	UINT w = 0, h = 0;
	conv->GetSize(&w, &h);

	uint8_t* pixels = new uint8_t[(size_t)w * h * 4];
	hr = conv->CopyPixels(nullptr, w * 4, w * h * 4, pixels);
	conv->Release();

	if (FAILED(hr)) { delete[] pixels; return nullptr; }
	*out_w = (int)w;
	*out_h = (int)h;
	return pixels;
}

// Allocate the next free plugin texture slot (0-based into g_pluginTextures).
// Must be called with g_textureMutex held. Returns -1 when all slots are used.
static int AllocTextureSlot()
{
	for (int i = 0; i < MAX_PLUGIN_TEXTURES; ++i)
		if (!g_pluginTextures[i].inUse)
			return i;
	return -1;
}

// Validate that a PluginTextureHandle points at a live record in g_pluginTextures.
static PluginTextureRecord* ValidateHandle(PluginTextureHandle h)
{
	if (!h) return nullptr;
	auto* rec = static_cast<PluginTextureRecord*>(h);
	if (rec < g_pluginTextures || rec >= g_pluginTextures + MAX_PLUGIN_TEXTURES) return nullptr;
	if (!rec->inUse) return nullptr;
	return rec;
}

// ---------- IPluginImGuiTextures implementation ----------

static PluginTextureHandle TextureLoadFromRGBA(const unsigned char* rgba, int width, int height, const char* name)
{
	if (!g_device || !rgba || width <= 0 || height <= 0)
		return nullptr;

	std::lock_guard<std::mutex> lock(g_textureMutex);
	int slot = AllocTextureSlot();
	if (slot < 0)
	{
		LogToFile::Error("[ImGuiBackend] TextureLoadFromRGBA '%s': all %d slots in use", name ? name : "?", MAX_PLUGIN_TEXTURES);
		throw std::out_of_range("IPluginImGuiTextures: all texture slots are in use -- call FreeTexture or check GetFreeSlotCount");
	}

	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
	cpuHandle.ptr += (UINT64)(slot + 1) * g_srvDescSize;  // slot 0 is ImGui font atlas
	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
	gpuHandle.ptr += (UINT64)(slot + 1) * g_srvDescSize;

	ID3D12Resource* resource = UploadRGBAToGPU(rgba, width, height, cpuHandle);
	if (!resource)
	{
		LogToFile::Error("[ImGuiBackend] TextureLoadFromRGBA '%s': GPU upload failed (%dx%d)", name ? name : "?", width, height);
		return nullptr;
	}

	PluginTextureRecord& rec = g_pluginTextures[slot];
	rec.resource  = resource;
	rec.gpuHandle = gpuHandle;
	rec.width     = width;
	rec.height    = height;
	rec.inUse     = true;
	if (name) strncpy_s(rec.name, name, _TRUNCATE);

	LogToFile::Debug("[ImGuiBackend] TextureLoadFromRGBA '%s': slot=%d %dx%d", rec.name, slot, width, height);
	return &rec;
}

// ---------------------------------------------------------------------------
// UTexture2D -> ImGui handle
//
// Copies the engine's GPU texture into a modloader-owned resource so the
// handle remains valid even if UE5 garbage-collects or evicts the source.
// The handle persists until the plugin calls FreeTexture() -- it is NOT
// auto-expired each frame.  Plugins that call this every frame will leak
// a slot per frame; load once, keep the handle, free when done.
// ---------------------------------------------------------------------------
//
// UE5 internal layout assumptions (verified against Dumper-7 SDK output):
//
//  UTexture::PrivateResource   -- UTexture + 0x120
//    Confirmed: Dumper-7 shows Pad_120[0x20] in UTexture covering exactly
//    PrivateResource(8) + PrivateResourceRenderThread(8) +
//    PrivateAsyncResourceReleaseHandleId(16) = 32 bytes (UE5 source match).
//
//  FTexture::TextureRHI raw pointer -- FTextureResource + 0x10
//    IDA: FTexture : FRenderResource (sizeof=0x48), FRenderResource = 0x10 bytes
//    (vtable=8 + state/flags=8). TextureRHI (TRefCountPtr<FRHITexture>) is the
//    first FTexture member at offset 0x10. FTextureResource : FTexture, single
//    inheritance, so the offset is the same in FTextureResource objects.
//
//  FRHITexture::GetNativeResource() -- vtable slot 5
//    Confirmed via IDA (FD3D12Texture_vtbl): slots 0-4 are dtor,
//    SetTrackedAccessFromContext, GetDesc, GetTextureReference,
//    GetDefaultBindlessHandle.  GetNativeResource is at slot 5.
//
// If any of these offsets change after a game update, adjust the constants below.
// ---------------------------------------------------------------------------
// Returns true if p looks like a valid user-mode pointer on x64 Windows:
//   - non-null
//   - at least 8-byte aligned (COM / C++ objects)
//   - below the canonical hole (user space ends at 0x00007FFFFFFFFFFF)
//   - above the first 64 KB (Windows reserves the low 64 KB; nothing valid lives there)
static bool IsValidUserPtr(const void* p)
{
	uintptr_t v = reinterpret_cast<uintptr_t>(p);
	return v >= 0x10000u &&
	       v <  0x0000800000000000ull &&
	       (v & 7u) == 0;
}

// Attempt to identify an FRHITexture subclass from its vtable pointer.
// Tries MSVC RTTI first (vtable[-1] -> RTTICompleteObjectLocator -> type name).
// Falls back to logging the owning module name and vtable RVA so the caller can
// look it up manually in IDA/Ghidra.  All reads are SEH-guarded.
static void LogRHITextureClass(void** vtable)
{
	if (!vtable) return;

	// --- RTTI attempt ---
	// MSVC x64: vtable[-1] = RTTICompleteObjectLocator*
	//   +0  DWORD signature  (1 = x64)
	//   +4  DWORD offset
	//   +8  DWORD cdOffset
	//   +12 DWORD pTypeDescriptor  (RVA from module base)
	//   +16 DWORD pClassDescriptor (RVA)
	//   +20 DWORD pSelf            (RVA of the COL itself -> module base = col - pSelf)
	// RTTITypeDescriptor layout: void* pVFTable, void* spare, char name[] (decorated)
	const char* rttiName = nullptr;
	__try
	{
		auto* colDwords = reinterpret_cast<const DWORD*>(vtable[-1]);
		if (colDwords && colDwords[0] == 1)
		{
			DWORD selfRVA    = colDwords[5];
			DWORD typeRVA    = colDwords[3];
			uintptr_t modBase = reinterpret_cast<uintptr_t>(colDwords) - selfRVA;
			const char* td   = reinterpret_cast<const char*>(modBase + typeRVA);
			// Skip pVFTable (8) + spare (8) to reach the name string
			rttiName = td + 16;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {}

	if (rttiName)
	{
		LogToFile::Warn("[ImGuiBackend] Unknown FRHITexture subclass (RTTI): %s  vtable=0x%p",
			rttiName, (void*)vtable);
		return;
	}

	// --- Fallback: module + RVA ---
	HMODULE hMod = nullptr;
	char modName[MAX_PATH] = "<unknown>";
	if (GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(vtable), &hMod))
	{
		GetModuleFileNameA(hMod, modName, sizeof(modName));
		if (char* slash = strrchr(modName, '\\')) memmove(modName, slash + 1, strlen(slash));
	}
	uintptr_t rva = reinterpret_cast<uintptr_t>(vtable)
	              - reinterpret_cast<uintptr_t>(hMod);
	LogToFile::Warn("[ImGuiBackend] Unknown FRHITexture subclass (no RTTI): vtable=0x%p  %s+0x%llX",
		(void*)vtable, modName, (unsigned long long)rva);
}

static constexpr ptrdiff_t kUTexture_PrivateResource          = 0x120;
static constexpr ptrdiff_t kFTextureResource_TextureRHI_Ptr   = 0x10;

// FD3D12Texture struct walk to FD3D12Resource::DefaultResourceState:
//   FRHITexture* == FD3D12Texture* (primary base at offset 0)
//   FD3D12BaseShaderResource subobject at FD3D12Texture +0x60 (sizeof FRHITexture)
//   FD3D12ResourceLocation ResourceLocation at FD3D12BaseShaderResource +0x48  => +0xA8 total
//   FD3D12Resource* UnderlyingResource at FD3D12ResourceLocation +0x10          => +0xB8 total (pointer)
//   D3D12_RESOURCE_STATES DefaultResourceState at FD3D12Resource +0x94
static constexpr ptrdiff_t kFD3D12Texture_UnderlyingResource_Ptr = 0xB8;
static constexpr ptrdiff_t kFD3D12Resource_DefaultResourceState  = 0x94;
// FD3D12Texture vtable layout (confirmed via IDA, StarRuptureGameSteam-Win64-Shipping):
//   slot 0 (+0x00)  ~FD3D12Texture
//   slot 1 (+0x08)  SetTrackedAccessFromContext
//   slot 2 (+0x10)  GetDesc              <-- returns FRHITextureDesc* (this+0x20), NOT the D3D12 resource
//   slot 3 (+0x18)  GetTextureReference
//   slot 4 (+0x20)  GetDefaultBindlessHandle
//   slot 5 (+0x28)  GetNativeResource             <-- returns ID3D12Resource* (used for GetDesc/dims only)
//   slot 6 (+0x30)  GetNativeShaderResourceView   <-- returns D3D12_CPU_DESCRIPTOR_HANDLE.ptr cast to void*
//   slot 7 (+0x38)  GetTextureBaseRHI
//   slot 8 (+0x40)  GetWriteMaskProperties
static constexpr int       kFRHITexture_GetNativeResource_Slot          = 5;
static constexpr int       kFRHITexture_GetNativeShaderResourceView_Slot = 6;

// Map a typeless DXGI format to its UNORM (or UF16 for BC6H) typed equivalent.
// Engine resources often use typeless formats internally; SRVs require a typed view.
static DXGI_FORMAT ResolveTypelessForSRV(DXGI_FORMAT fmt)
{
	switch (fmt)
	{
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case DXGI_FORMAT_R32G32B32_TYPELESS:    return DXGI_FORMAT_R32G32B32_FLOAT;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
	case DXGI_FORMAT_R32G32_TYPELESS:       return DXGI_FORMAT_R32G32_FLOAT;
	case DXGI_FORMAT_R32G8X24_TYPELESS:     return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_R16G16_TYPELESS:       return DXGI_FORMAT_R16G16_UNORM;
	case DXGI_FORMAT_R32_TYPELESS:          return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R24G8_TYPELESS:        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_R8G8_TYPELESS:         return DXGI_FORMAT_R8G8_UNORM;
	case DXGI_FORMAT_R16_TYPELESS:          return DXGI_FORMAT_R16_UNORM;
	case DXGI_FORMAT_R8_TYPELESS:           return DXGI_FORMAT_R8_UNORM;
	case DXGI_FORMAT_BC1_TYPELESS:          return DXGI_FORMAT_BC1_UNORM;
	case DXGI_FORMAT_BC2_TYPELESS:          return DXGI_FORMAT_BC2_UNORM;
	case DXGI_FORMAT_BC3_TYPELESS:          return DXGI_FORMAT_BC3_UNORM;
	case DXGI_FORMAT_BC4_TYPELESS:          return DXGI_FORMAT_BC4_UNORM;
	case DXGI_FORMAT_BC5_TYPELESS:          return DXGI_FORMAT_BC5_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:     return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_BC6H_TYPELESS:         return DXGI_FORMAT_BC6H_UF16;
	case DXGI_FORMAT_BC7_TYPELESS:          return DXGI_FORMAT_BC7_UNORM;
	default:                                return fmt;
	}
}

// Isolated SEH helper: no C++ objects with destructors, so __try is legal here.
// Reads the vtable from rhiTexture, calls vtable[5] (GetNativeResource), and calls
// ID3D12Resource::GetDesc().  rhiTexture may be static exe/DLL data that passes
// IsValidUserPtr but is not a real heap FRHITexture; calling through its vtable into
// Streamline with a bad 'this' would corrupt Streamline before the AV fires.
// Returns true and fills out-params on success; logs and returns false on any exception.
static bool GetNativeResourceSEH(
	void*                rhiTexture,
	void***              outVtable,
	ID3D12Resource**     outResource,
	D3D12_RESOURCE_DESC* outDesc)
{
	__try
	{
		void** vtable = *reinterpret_cast<void***>(rhiTexture);
		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: vtable=0x%p vtable[%d]=0x%p",
			(void*)vtable, kFRHITexture_GetNativeResource_Slot,
			vtable ? vtable[kFRHITexture_GetNativeResource_Slot] : nullptr);

		if (!IsValidUserPtr(vtable))
		{
			LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: vtable invalid (0x%p) -- corrupt FRHITexture?",
				(void*)vtable);
			return false;
		}
		*outVtable = vtable;

		using GetNativeResourceFn = void*(__fastcall*)(void*);
		auto getNativeResource = reinterpret_cast<GetNativeResourceFn>(vtable[kFRHITexture_GetNativeResource_Slot]);
		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: calling GetNativeResource fn=0x%p this=0x%p",
			(void*)(uintptr_t)getNativeResource, rhiTexture);

		ID3D12Resource* res = static_cast<ID3D12Resource*>(getNativeResource(rhiTexture));
		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: ID3D12Resource*=0x%p", (void*)res);

		if (!IsValidUserPtr(res))
		{
			LogRHITextureClass(vtable);
			return false;
		}
		*outResource = res;

		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: calling GetDesc() on 0x%p", (void*)res);
		*outDesc = res->GetDesc();
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		LogToFile::Warn("[ImGuiBackend] LoadFromUTexture2D: SEH exception in vtable walk / GetNativeResource / GetDesc"
			" -- rhiTexture=0x%p is likely static data, not a heap FRHITexture; skipping",
			rhiTexture);
		return false;
	}
}

static PluginTextureHandle TextureLoadFromUTexture2D(SDK::UTexture2D* uTexture2D, const char* name)
{
	LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D '%s': entry uTexture2D=0x%p g_device=0x%p",
		name ? name : "?", (void*)uTexture2D, (void*)g_device);

	if (!g_device || !uTexture2D)
	{
		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D '%s': early-out (device=%s texture=%s)",
			name ? name : "?", g_device ? "ok" : "NULL", uTexture2D ? "ok" : "NULL");
		return nullptr;
	}

	// Step 1: UTexture::PrivateResource at offset 0x120
	uint8_t* textureBase = reinterpret_cast<uint8_t*>(uTexture2D);
	LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: reading PrivateResource at 0x%p+0x%zX",
		(void*)textureBase, kUTexture_PrivateResource);

	void* resourcePtr = *reinterpret_cast<void**>(textureBase + kUTexture_PrivateResource);
	LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: FTextureResource*=0x%p", resourcePtr);

	if (!IsValidUserPtr(resourcePtr))
	{
		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: FTextureResource* invalid (0x%p) -- not streamed/resident or corrupt",
			resourcePtr);
		return nullptr;
	}

	// Step 2: FTexture::TextureRHI (TRefCountPtr<FRHITexture>) at offset 0x10
	uint8_t* textureResourceBase = reinterpret_cast<uint8_t*>(resourcePtr);
	LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: reading TextureRHI at 0x%p+0x%zX",
		(void*)textureResourceBase, kFTextureResource_TextureRHI_Ptr);

	void* rhiTexture = *reinterpret_cast<void**>(textureResourceBase + kFTextureResource_TextureRHI_Ptr);
	LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: FRHITexture*=0x%p", rhiTexture);

	if (!IsValidUserPtr(rhiTexture))
	{
		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: FRHITexture* invalid (0x%p) -- RHI not initialized or corrupt",
			rhiTexture);
		return nullptr;
	}

	// Step 3: FRHITexture::GetNativeResource() via vtable slot 5, then GetDesc().
	// Delegated to GetNativeResourceSEH() to isolate __try from C++ object unwinding.
	void** vtable       = nullptr;
	ID3D12Resource* d3dResource = nullptr;
	D3D12_RESOURCE_DESC resDesc = {};
	if (!GetNativeResourceSEH(rhiTexture, &vtable, &d3dResource, &resDesc))
		return nullptr;
	int width  = static_cast<int>(resDesc.Width);
	int height = static_cast<int>(resDesc.Height);
	LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: desc %dx%d format=%u mips=%u dim=%u flags=0x%X",
		width, height,
		(unsigned)resDesc.Format,
		(unsigned)resDesc.MipLevels,
		(unsigned)resDesc.Dimension,
		(unsigned)resDesc.Flags);

	if (width <= 0 || height <= 0)
	{
		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: degenerate dimensions %dx%d -- aborting", width, height);
		return nullptr;
	}

	// Step 4.5: Read FD3D12Resource::DefaultResourceState to confirm the texture is
	// GPU-resident and in a shader-readable state.  Streaming textures start life in
	// COPY_DEST (0x400); we must not create an SRV and try to sample them until UE5
	// has transitioned them to PIXEL_SHADER_RESOURCE.  Returning nullptr lets the
	// plugin retry next frame.  We also keep a live pointer to DefaultResourceState
	// so Image/ImageButton can re-check readiness on every draw call.
	void* underlyingRaw = *reinterpret_cast<void**>(
		static_cast<uint8_t*>(rhiTexture) + kFD3D12Texture_UnderlyingResource_Ptr);
	LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: FD3D12Resource*=0x%p", underlyingRaw);

	if (!underlyingRaw)
	{
		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: UnderlyingResource is null -- not yet allocated");
		return nullptr;
	}

	D3D12_RESOURCE_STATES* pDefaultResState = reinterpret_cast<D3D12_RESOURCE_STATES*>(
		static_cast<uint8_t*>(underlyingRaw) + kFD3D12Resource_DefaultResourceState);

	{
		constexpr D3D12_RESOURCE_STATES kReadable =
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		D3D12_RESOURCE_STATES defaultState = *pDefaultResState;
		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: DefaultResourceState=0x%X", (unsigned)defaultState);
		bool ready = (defaultState == D3D12_RESOURCE_STATE_COMMON) ||
		             ((defaultState & kReadable) != 0);
		if (!ready)
		{
			LogToFile::Warn("[ImGuiBackend] LoadFromUTexture2D: texture not ready "
				"(DefaultResourceState=0x%X, not PSR/COMMON) -- retry next frame",
				(unsigned)defaultState);
			return nullptr;
		}
	}

	// Step 5: Allocate a slot, copy the engine resource to our own owned resource,
	// and create an SRV over it.  We own the copy so it is safe from engine eviction.
	LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: allocating SRV slot");
	std::lock_guard<std::mutex> lock(g_textureMutex);
	int slot = AllocTextureSlot();
	if (slot < 0)
	{
		LogToFile::Error("[ImGuiBackend] LoadFromUTexture2D '%s': all %d slots in use", name ? name : "?", MAX_PLUGIN_TEXTURES);
		throw std::out_of_range("IPluginImGuiTextures: all texture slots are in use -- call FreeTexture or check GetFreeSlotCount");
	}

	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
	cpuHandle.ptr += (UINT64)(slot + 1) * g_srvDescSize;
	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
	gpuHandle.ptr += (UINT64)(slot + 1) * g_srvDescSize;

	DXGI_FORMAT srvFormat = ResolveTypelessForSRV(resDesc.Format);
	if (srvFormat != resDesc.Format)
		LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: typeless format %u -> SRV format %u",
			(unsigned)resDesc.Format, (unsigned)srvFormat);

	D3D12_RESOURCE_STATES srcState = *pDefaultResState;
	LogToFile::Trace("[ImGuiBackend] LoadFromUTexture2D: copying engine texture to owned resource "
		"slot=%d %dx%d srvFormat=%u srcState=0x%X",
		slot, width, height, (unsigned)srvFormat, (unsigned)srcState);

	ID3D12Resource* ownedResource = CopyEngineTextureToOwned(
		d3dResource, srcState, resDesc, srvFormat, cpuHandle);

	if (!ownedResource)
	{
		LogToFile::Error("[ImGuiBackend] LoadFromUTexture2D '%s': CopyEngineTextureToOwned failed", name ? name : "?");
		return nullptr;
	}

	PluginTextureRecord& rec = g_pluginTextures[slot];
	rec.resource  = ownedResource;
	rec.gpuHandle = gpuHandle;
	rec.width     = width;
	rec.height    = height;
	rec.inUse     = true;
	rec.owned     = true;
	if (name) strncpy_s(rec.name, name, _TRUNCATE);

	LogToFile::Debug("[ImGuiBackend] LoadFromUTexture2D '%s': slot=%d %dx%d fmt=%u",
		rec.name, slot, width, height, (unsigned)resDesc.Format);
	return &rec;
}

static PluginTextureHandle TextureLoadFromMemory(const void* data, size_t size, const char* name)
{
	if (!data || size == 0) return nullptr;

	IWICImagingFactory* factory = GetWICFactory();
	if (!factory) return nullptr;

	IWICStream* stream = nullptr;
	if (FAILED(factory->CreateStream(&stream))) return nullptr;
	if (FAILED(stream->InitializeFromMemory((BYTE*)data, (DWORD)size)))
	{
		stream->Release();
		return nullptr;
	}

	IWICBitmapDecoder* decoder = nullptr;
	HRESULT hr = factory->CreateDecoderFromStream(stream, nullptr,
		WICDecodeMetadataCacheOnLoad, &decoder);
	stream->Release();
	if (FAILED(hr)) return nullptr;

	IWICBitmapFrameDecode* frame = nullptr;
	hr = decoder->GetFrame(0, &frame);
	decoder->Release();
	if (FAILED(hr)) return nullptr;

	int w = 0, h = 0;
	uint8_t* pixels = WICToRGBA(frame, &w, &h);
	frame->Release();
	if (!pixels) return nullptr;

	PluginTextureHandle handle = TextureLoadFromRGBA(pixels, w, h, name);
	delete[] pixels;
	return handle;
}

static PluginTextureHandle TextureLoadFromFile(const char* utf8_path, const char* name)
{
	if (!utf8_path || !*utf8_path) return nullptr;

	// UTF-8 -> wide string for WIC
	int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, nullptr, 0);
	if (wlen <= 0) return nullptr;
	wchar_t* wpath = new wchar_t[wlen];
	MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wpath, wlen);

	IWICImagingFactory* factory = GetWICFactory();
	if (!factory) { delete[] wpath; return nullptr; }

	IWICBitmapDecoder* decoder = nullptr;
	HRESULT hr = factory->CreateDecoderFromFilename(wpath, nullptr, GENERIC_READ,
		WICDecodeMetadataCacheOnLoad, &decoder);
	delete[] wpath;
	if (FAILED(hr)) return nullptr;

	IWICBitmapFrameDecode* frame = nullptr;
	hr = decoder->GetFrame(0, &frame);
	decoder->Release();
	if (FAILED(hr)) return nullptr;

	int w = 0, h = 0;
	uint8_t* pixels = WICToRGBA(frame, &w, &h);
	frame->Release();
	if (!pixels) return nullptr;

	PluginTextureHandle handle = TextureLoadFromRGBA(pixels, w, h, name);
	delete[] pixels;
	return handle;
}

static void TextureFreeTexture(PluginTextureHandle handle)
{
	std::lock_guard<std::mutex> lock(g_textureMutex);
	PluginTextureRecord* rec = ValidateHandle(handle);
	if (!rec) return;
	// Only Release resources we uploaded ourselves; engine-owned resources (owned=false) are not ours to free.
	if (rec->owned && rec->resource) { rec->resource->Release(); rec->resource = nullptr; }
	*rec = {};
}

static void TextureGetSize(PluginTextureHandle handle, int* out_w, int* out_h)
{
	PluginTextureRecord* rec = ValidateHandle(handle);
	if (!rec) return;
	if (out_w) *out_w = rec->width;
	if (out_h) *out_h = rec->height;
}

static void TextureImage(PluginTextureHandle handle, float w, float h)
{
	PluginTextureRecord* rec = ValidateHandle(handle);
	if (!rec) return;
	LogToFile::Trace("[ImGuiBackend] TextureImage '%s': handle=0x%p w=%.0f h=%.0f", rec->name, handle, w, h);
	float iw = (w == 0.0f) ? (float)rec->width  : w;
	float ih = (h == 0.0f) ? (float)rec->height : h;
	ImGui::Image((ImTextureID)rec->gpuHandle.ptr, ImVec2(iw, ih));
}

static bool TextureImageButton(const char* str_id, PluginTextureHandle handle, float w, float h)
{
	if (!str_id) return false;
	PluginTextureRecord* rec = ValidateHandle(handle);
	if (!rec) return false;
	LogToFile::Trace("[ImGuiBackend] TextureImageButton '%s': str_id=%s handle=0x%p w=%.0f h=%.0f", rec->name, str_id, handle, w, h);
	float iw = (w == 0.0f) ? (float)rec->width  : w;
	float ih = (h == 0.0f) ? (float)rec->height : h;
	return ImGui::ImageButton(str_id, (ImTextureID)rec->gpuHandle.ptr, ImVec2(iw, ih));
}


static int TextureGetFreeSlotCount()
{
	std::lock_guard<std::mutex> lock(g_textureMutex);
	int free = 0;
	for (int i = 0; i < MAX_PLUGIN_TEXTURES; ++i)
		if (!g_pluginTextures[i].inUse)
			++free;
	return free;
}

static int TextureGetCapacity()
{
	return MAX_PLUGIN_TEXTURES;
}

static void PopulateTextureAPI()
{
	g_textureAPI.LoadFromFile        = TextureLoadFromFile;
	g_textureAPI.LoadFromMemory      = TextureLoadFromMemory;
	g_textureAPI.LoadFromRGBA        = TextureLoadFromRGBA;
	g_textureAPI.LoadFromUTexture2D  = TextureLoadFromUTexture2D;
	g_textureAPI.FreeTexture         = TextureFreeTexture;
	g_textureAPI.GetSize             = TextureGetSize;
	g_textureAPI.Image               = TextureImage;
	g_textureAPI.ImageButton         = TextureImageButton;
	g_textureAPI.GetFreeSlotCount    = TextureGetFreeSlotCount;
	g_textureAPI.GetCapacity         = TextureGetCapacity;
}

// ---------------------------------------------------------------------------

static void PopulateImGuiAPI()
{
	g_imguiAPI.Text = ImGuiWrappers::Text;
	g_imguiAPI.TextColored = ImGuiWrappers::TextColored;
	g_imguiAPI.TextDisabled = ImGuiWrappers::TextDisabled;
	g_imguiAPI.TextWrapped = ImGuiWrappers::TextWrapped;
	g_imguiAPI.LabelText = ImGuiWrappers::LabelText;
	g_imguiAPI.SeparatorText = ImGuiWrappers::SeparatorText;
	g_imguiAPI.InputText = ImGuiWrappers::InputText;
	g_imguiAPI.InputInt = ImGuiWrappers::InputInt;
	g_imguiAPI.InputFloat = ImGuiWrappers::InputFloat;
	g_imguiAPI.Checkbox = ImGuiWrappers::Checkbox;
	g_imguiAPI.SliderFloat = ImGuiWrappers::SliderFloat;
	g_imguiAPI.SliderInt = ImGuiWrappers::SliderInt;
	g_imguiAPI.Button = ImGuiWrappers::Button;
	g_imguiAPI.SmallButton = ImGuiWrappers::SmallButton;
	g_imguiAPI.SameLine = ImGuiWrappers::SameLine;
	g_imguiAPI.NewLine = ImGuiWrappers::NewLine;
	g_imguiAPI.Separator = ImGuiWrappers::Separator;
	g_imguiAPI.Spacing = ImGuiWrappers::Spacing;
	g_imguiAPI.Indent = ImGuiWrappers::Indent;
	g_imguiAPI.Unindent = ImGuiWrappers::Unindent;
	g_imguiAPI.PushIDStr = ImGuiWrappers::PushIDStr;
	g_imguiAPI.PushIDInt = ImGuiWrappers::PushIDInt;
	g_imguiAPI.PopID = ImGuiWrappers::PopID;
	g_imguiAPI.BeginCombo = ImGuiWrappers::BeginCombo;
	g_imguiAPI.Selectable = ImGuiWrappers::Selectable;
	g_imguiAPI.EndCombo = ImGuiWrappers::EndCombo;
	g_imguiAPI.CollapsingHeader = ImGuiWrappers::CollapsingHeader;
	g_imguiAPI.TreeNodeStr = ImGuiWrappers::TreeNodeStr;
	g_imguiAPI.TreePop = ImGuiWrappers::TreePop;
	g_imguiAPI.ColorEdit3 = ImGuiWrappers::ColorEdit3;
	g_imguiAPI.ColorEdit4 = ImGuiWrappers::ColorEdit4;
	g_imguiAPI.SetTooltip = ImGuiWrappers::SetTooltip;
	g_imguiAPI.IsItemHovered = ImGuiWrappers::IsItemHovered;
	g_imguiAPI.SetNextItemWidth = ImGuiWrappers::SetNextItemWidth;
	g_imguiAPI.GetFontSize = ImGuiWrappers::GetFontSize;
	g_imguiAPI.GetTextLineHeight = ImGuiWrappers::GetTextLineHeight;
	g_imguiAPI.GetTextLineHeightWithSpacing = ImGuiWrappers::GetTextLineHeightWithSpacing;
	g_imguiAPI.GetFrameHeight = ImGuiWrappers::GetFrameHeight;
	g_imguiAPI.GetFrameHeightWithSpacing = ImGuiWrappers::GetFrameHeightWithSpacing;
	g_imguiAPI.CalcTextSize = ImGuiWrappers::CalcTextSize;
	g_imguiAPI.GetContentRegionAvail = ImGuiWrappers::GetContentRegionAvail;
	g_imguiAPI.GetDisplaySize = ImGuiWrappers::GetDisplaySize;
	g_imguiAPI.SetWindowFontScale = ImGuiWrappers::SetWindowFontScale;

	// v35
	g_imguiAPI.BeginChild = ImGuiWrappers::BeginChild;
	g_imguiAPI.EndChild = ImGuiWrappers::EndChild;
	g_imguiAPI.PushStyleColor = ImGuiWrappers::PushStyleColor;
	g_imguiAPI.PopStyleColor = ImGuiWrappers::PopStyleColor;
	g_imguiAPI.PushStyleVarFloat = ImGuiWrappers::PushStyleVarFloat;
	g_imguiAPI.PushStyleVarVec2 = ImGuiWrappers::PushStyleVarVec2;
	g_imguiAPI.PopStyleVar = ImGuiWrappers::PopStyleVar;
	g_imguiAPI.PushItemWidth = ImGuiWrappers::PushItemWidth;
	g_imguiAPI.PopItemWidth = ImGuiWrappers::PopItemWidth;
	g_imguiAPI.SetCursorPosX = ImGuiWrappers::SetCursorPosX;
	g_imguiAPI.GetCursorPosX = ImGuiWrappers::GetCursorPosX;
	g_imguiAPI.BeginTable = ImGuiWrappers::BeginTable;
	g_imguiAPI.TableNextColumn = ImGuiWrappers::TableNextColumn;
	g_imguiAPI.EndTable = ImGuiWrappers::EndTable;
	g_imguiAPI.IsItemClicked = ImGuiWrappers::IsItemClicked;
	g_imguiAPI.GetWindowWidth = ImGuiWrappers::GetWindowWidth;
	g_imguiAPI.GetWindowHeight = ImGuiWrappers::GetWindowHeight;
	g_imguiAPI.Dummy = ImGuiWrappers::Dummy;

	// v36
	g_imguiAPI.IsWindowAppearing = ImGuiWrappers::IsWindowAppearing;
	g_imguiAPI.IsWindowCollapsed = ImGuiWrappers::IsWindowCollapsed;
	g_imguiAPI.IsWindowFocused = ImGuiWrappers::IsWindowFocused;
	g_imguiAPI.IsWindowHovered = ImGuiWrappers::IsWindowHovered;
	g_imguiAPI.GetWindowPos = ImGuiWrappers::GetWindowPos;
	g_imguiAPI.GetWindowSize = ImGuiWrappers::GetWindowSize;
	g_imguiAPI.SetNextWindowBgAlpha = ImGuiWrappers::SetNextWindowBgAlpha;
	g_imguiAPI.GetScrollX = ImGuiWrappers::GetScrollX;
	g_imguiAPI.GetScrollY = ImGuiWrappers::GetScrollY;
	g_imguiAPI.SetScrollX = ImGuiWrappers::SetScrollX;
	g_imguiAPI.SetScrollY = ImGuiWrappers::SetScrollY;
	g_imguiAPI.GetScrollMaxX = ImGuiWrappers::GetScrollMaxX;
	g_imguiAPI.GetScrollMaxY = ImGuiWrappers::GetScrollMaxY;
	g_imguiAPI.SetScrollHereX = ImGuiWrappers::SetScrollHereX;
	g_imguiAPI.SetScrollHereY = ImGuiWrappers::SetScrollHereY;
	g_imguiAPI.BeginGroup = ImGuiWrappers::BeginGroup;
	g_imguiAPI.EndGroup = ImGuiWrappers::EndGroup;
	g_imguiAPI.AlignTextToFramePadding = ImGuiWrappers::AlignTextToFramePadding;
	g_imguiAPI.GetCursorPosY = ImGuiWrappers::GetCursorPosY;
	g_imguiAPI.SetCursorPosY = ImGuiWrappers::SetCursorPosY;
	g_imguiAPI.SetCursorPos = ImGuiWrappers::SetCursorPos;
	g_imguiAPI.GetCursorScreenPos = ImGuiWrappers::GetCursorScreenPos;
	g_imguiAPI.SetCursorScreenPos = ImGuiWrappers::SetCursorScreenPos;
	g_imguiAPI.GetCursorStartPos = ImGuiWrappers::GetCursorStartPos;
	g_imguiAPI.CalcItemWidth = ImGuiWrappers::CalcItemWidth;
	g_imguiAPI.PushTextWrapPos = ImGuiWrappers::PushTextWrapPos;
	g_imguiAPI.PopTextWrapPos = ImGuiWrappers::PopTextWrapPos;
	g_imguiAPI.PushStyleVarX = ImGuiWrappers::PushStyleVarX;
	g_imguiAPI.PushStyleVarY = ImGuiWrappers::PushStyleVarY;
	g_imguiAPI.PushItemFlag = ImGuiWrappers::PushItemFlag;
	g_imguiAPI.PopItemFlag = ImGuiWrappers::PopItemFlag;
	g_imguiAPI.BulletText = ImGuiWrappers::BulletText;
	g_imguiAPI.Bullet = ImGuiWrappers::Bullet;
	g_imguiAPI.ButtonSized = ImGuiWrappers::ButtonSized;
	g_imguiAPI.InvisibleButton = ImGuiWrappers::InvisibleButton;
	g_imguiAPI.ArrowButton = ImGuiWrappers::ArrowButton;
	g_imguiAPI.RadioButton = ImGuiWrappers::RadioButton;
	g_imguiAPI.RadioButtonInt = ImGuiWrappers::RadioButtonInt;
	g_imguiAPI.ProgressBar = ImGuiWrappers::ProgressBar;
	g_imguiAPI.CheckboxFlagsInt = ImGuiWrappers::CheckboxFlagsInt;
	g_imguiAPI.SelectableFull = ImGuiWrappers::SelectableFull;
	g_imguiAPI.TextLink = ImGuiWrappers::TextLink;
	g_imguiAPI.InputTextMultiline = ImGuiWrappers::InputTextMultiline;
	g_imguiAPI.InputTextWithHint = ImGuiWrappers::InputTextWithHint;
	g_imguiAPI.InputFloat2 = ImGuiWrappers::InputFloat2;
	g_imguiAPI.InputFloat3 = ImGuiWrappers::InputFloat3;
	g_imguiAPI.InputFloat4 = ImGuiWrappers::InputFloat4;
	g_imguiAPI.InputInt2 = ImGuiWrappers::InputInt2;
	g_imguiAPI.InputInt3 = ImGuiWrappers::InputInt3;
	g_imguiAPI.InputInt4 = ImGuiWrappers::InputInt4;
	g_imguiAPI.DragFloat = ImGuiWrappers::DragFloat;
	g_imguiAPI.DragFloat2 = ImGuiWrappers::DragFloat2;
	g_imguiAPI.DragFloat3 = ImGuiWrappers::DragFloat3;
	g_imguiAPI.DragFloat4 = ImGuiWrappers::DragFloat4;
	g_imguiAPI.DragInt = ImGuiWrappers::DragInt;
	g_imguiAPI.DragInt2 = ImGuiWrappers::DragInt2;
	g_imguiAPI.DragInt3 = ImGuiWrappers::DragInt3;
	g_imguiAPI.DragInt4 = ImGuiWrappers::DragInt4;
	g_imguiAPI.DragFloatRange2 = ImGuiWrappers::DragFloatRange2;
	g_imguiAPI.DragIntRange2 = ImGuiWrappers::DragIntRange2;
	g_imguiAPI.VSliderFloat = ImGuiWrappers::VSliderFloat;
	g_imguiAPI.VSliderInt = ImGuiWrappers::VSliderInt;
	g_imguiAPI.SliderAngle = ImGuiWrappers::SliderAngle;
	g_imguiAPI.ColorPicker3 = ImGuiWrappers::ColorPicker3;
	g_imguiAPI.ColorPicker4 = ImGuiWrappers::ColorPicker4;
	g_imguiAPI.ColorButton = ImGuiWrappers::ColorButton;
	g_imguiAPI.PlotLines = ImGuiWrappers::PlotLines;
	g_imguiAPI.PlotHistogram = ImGuiWrappers::PlotHistogram;
	g_imguiAPI.TreeNodeExStr = ImGuiWrappers::TreeNodeExStr;
	g_imguiAPI.TreePushStr = ImGuiWrappers::TreePushStr;
	g_imguiAPI.GetTreeNodeToLabelSpacing = ImGuiWrappers::GetTreeNodeToLabelSpacing;
	g_imguiAPI.SetNextItemOpen = ImGuiWrappers::SetNextItemOpen;
	g_imguiAPI.BeginListBox = ImGuiWrappers::BeginListBox;
	g_imguiAPI.EndListBox = ImGuiWrappers::EndListBox;
	g_imguiAPI.BeginTabBar = ImGuiWrappers::BeginTabBar;
	g_imguiAPI.EndTabBar = ImGuiWrappers::EndTabBar;
	g_imguiAPI.BeginTabItem = ImGuiWrappers::BeginTabItem;
	g_imguiAPI.EndTabItem = ImGuiWrappers::EndTabItem;
	g_imguiAPI.TabItemButton = ImGuiWrappers::TabItemButton;
	g_imguiAPI.BeginMenuBar = ImGuiWrappers::BeginMenuBar;
	g_imguiAPI.EndMenuBar = ImGuiWrappers::EndMenuBar;
	g_imguiAPI.BeginMenu = ImGuiWrappers::BeginMenu;
	g_imguiAPI.EndMenu = ImGuiWrappers::EndMenu;
	g_imguiAPI.MenuItem = ImGuiWrappers::MenuItem;
	g_imguiAPI.BeginPopup = ImGuiWrappers::BeginPopup;
	g_imguiAPI.BeginPopupModal = ImGuiWrappers::BeginPopupModal;
	g_imguiAPI.EndPopup = ImGuiWrappers::EndPopup;
	g_imguiAPI.OpenPopup = ImGuiWrappers::OpenPopup;
	g_imguiAPI.CloseCurrentPopup = ImGuiWrappers::CloseCurrentPopup;
	g_imguiAPI.BeginPopupContextItem = ImGuiWrappers::BeginPopupContextItem;
	g_imguiAPI.BeginPopupContextWindow = ImGuiWrappers::BeginPopupContextWindow;
	g_imguiAPI.IsPopupOpen = ImGuiWrappers::IsPopupOpen;
	g_imguiAPI.BeginTooltip = ImGuiWrappers::BeginTooltip;
	g_imguiAPI.EndTooltip = ImGuiWrappers::EndTooltip;
	g_imguiAPI.BeginItemTooltip = ImGuiWrappers::BeginItemTooltip;
	g_imguiAPI.SetItemTooltip = ImGuiWrappers::SetItemTooltip;
	g_imguiAPI.TableNextRow = ImGuiWrappers::TableNextRow;
	g_imguiAPI.TableSetColumnIndex = ImGuiWrappers::TableSetColumnIndex;
	g_imguiAPI.TableSetupColumn = ImGuiWrappers::TableSetupColumn;
	g_imguiAPI.TableSetupScrollFreeze = ImGuiWrappers::TableSetupScrollFreeze;
	g_imguiAPI.TableHeadersRow = ImGuiWrappers::TableHeadersRow;
	g_imguiAPI.TableHeader = ImGuiWrappers::TableHeader;
	g_imguiAPI.TableGetColumnCount = ImGuiWrappers::TableGetColumnCount;
	g_imguiAPI.TableGetColumnIndex = ImGuiWrappers::TableGetColumnIndex;
	g_imguiAPI.TableGetRowIndex = ImGuiWrappers::TableGetRowIndex;
	g_imguiAPI.TableGetColumnName = ImGuiWrappers::TableGetColumnName;
	g_imguiAPI.TableSetBgColor = ImGuiWrappers::TableSetBgColor;
	g_imguiAPI.IsItemActive = ImGuiWrappers::IsItemActive;
	g_imguiAPI.IsItemFocused = ImGuiWrappers::IsItemFocused;
	g_imguiAPI.IsItemVisible = ImGuiWrappers::IsItemVisible;
	g_imguiAPI.IsItemEdited = ImGuiWrappers::IsItemEdited;
	g_imguiAPI.IsItemActivated = ImGuiWrappers::IsItemActivated;
	g_imguiAPI.IsItemDeactivated = ImGuiWrappers::IsItemDeactivated;
	g_imguiAPI.IsItemDeactivatedAfterEdit = ImGuiWrappers::IsItemDeactivatedAfterEdit;
	g_imguiAPI.IsItemToggledOpen = ImGuiWrappers::IsItemToggledOpen;
	g_imguiAPI.IsAnyItemHovered = ImGuiWrappers::IsAnyItemHovered;
	g_imguiAPI.IsAnyItemActive = ImGuiWrappers::IsAnyItemActive;
	g_imguiAPI.IsAnyItemFocused = ImGuiWrappers::IsAnyItemFocused;
	g_imguiAPI.GetItemRectMin = ImGuiWrappers::GetItemRectMin;
	g_imguiAPI.GetItemRectMax = ImGuiWrappers::GetItemRectMax;
	g_imguiAPI.GetItemRectSize = ImGuiWrappers::GetItemRectSize;
	g_imguiAPI.SetItemDefaultFocus = ImGuiWrappers::SetItemDefaultFocus;
	g_imguiAPI.SetKeyboardFocusHere = ImGuiWrappers::SetKeyboardFocusHere;
	g_imguiAPI.SetNextItemAllowOverlap = ImGuiWrappers::SetNextItemAllowOverlap;
	g_imguiAPI.BeginDisabled = ImGuiWrappers::BeginDisabled;
	g_imguiAPI.EndDisabled = ImGuiWrappers::EndDisabled;
	g_imguiAPI.PushClipRect = ImGuiWrappers::PushClipRect;
	g_imguiAPI.PopClipRect = ImGuiWrappers::PopClipRect;
	g_imguiAPI.IsMouseDown = ImGuiWrappers::IsMouseDown;
	g_imguiAPI.IsMouseClicked = ImGuiWrappers::IsMouseClicked;
	g_imguiAPI.IsMouseReleased = ImGuiWrappers::IsMouseReleased;
	g_imguiAPI.IsMouseDoubleClicked = ImGuiWrappers::IsMouseDoubleClicked;
	g_imguiAPI.GetMousePos = ImGuiWrappers::GetMousePos;
	g_imguiAPI.IsMouseDragging = ImGuiWrappers::IsMouseDragging;
	g_imguiAPI.GetMouseDragDelta = ImGuiWrappers::GetMouseDragDelta;
	g_imguiAPI.ResetMouseDragDelta = ImGuiWrappers::ResetMouseDragDelta;
	g_imguiAPI.SetMouseCursor = ImGuiWrappers::SetMouseCursor;
	g_imguiAPI.IsMouseHoveringRect = ImGuiWrappers::IsMouseHoveringRect;
	g_imguiAPI.GetColorU32FromCol = ImGuiWrappers::GetColorU32FromCol;
	g_imguiAPI.GetColorU32FromVec4 = ImGuiWrappers::GetColorU32FromVec4;
	g_imguiAPI.GetStyleColorVec4 = ImGuiWrappers::GetStyleColorVec4;
	g_imguiAPI.ColorConvertRGBtoHSV = ImGuiWrappers::ColorConvertRGBtoHSV;
	g_imguiAPI.ColorConvertHSVtoRGB = ImGuiWrappers::ColorConvertHSVtoRGB;
	g_imguiAPI.GetTime = ImGuiWrappers::GetTime;
	g_imguiAPI.GetFrameCount = ImGuiWrappers::GetFrameCount;
	g_imguiAPI.IsRectVisible = ImGuiWrappers::IsRectVisible;
	g_imguiAPI.GetClipboardText = ImGuiWrappers::GetClipboardText;
	g_imguiAPI.SetClipboardText = ImGuiWrappers::SetClipboardText;
	g_imguiAPI.GetStyleColorName = ImGuiWrappers::GetStyleColorName;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace UI::ImGuiBackend
{
	void Initialize()
	{
		// Detect Streamline and common overlays before installing any hooks.
		// Logged once so crash reports immediately show the system configuration.
		g_streamlineActive = GetModuleHandleW(L"sl.interposer.dll") != nullptr;
		LogToFile::Info("[ImGuiBackend] Streamline: %s", g_streamlineActive ? "YES" : "no");

		{
			bool rtss = GetModuleHandleW(L"RTSSHooks64.dll") != nullptr;
			bool discord = GetModuleHandleW(L"DiscordHook64.dll") != nullptr;
			bool steam = GetModuleHandleW(L"gameoverlayrenderer64.dll") != nullptr;
			LogToFile::Info("[ImGuiBackend] Overlays detected: RTSS=%s  Discord=%s  Steam=%s",
				rtss ? "YES" : "no", discord ? "YES" : "no", steam ? "YES" : "no");
			if (rtss)
				LogToFile::Info("[ImGuiBackend] RTSS (RivaTuner/Afterburner) detected -- "
					"if the overlay crashes, disable RTSS and report the issue");
		}

		// Log system diagnostics so crash reports have full hardware/OS context.
		{
			// --- Executable path ---
			char exePath[MAX_PATH] = {};
			GetModuleFileNameA(nullptr, exePath, MAX_PATH);
			LogToFile::Info("[ImGuiBackend] Exe: %s", exePath);

			// --- Windows build (RtlGetVersion bypasses compat shims) ---
			using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
			OSVERSIONINFOEXW osv = { sizeof(osv) };
			auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
				GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
			if (rtlGetVersion) rtlGetVersion(&osv);
			LogToFile::Info("[ImGuiBackend] OS: Windows %lu.%lu build %lu",
				osv.dwMajorVersion, osv.dwMinorVersion, osv.dwBuildNumber);

			// --- CPU ---
			char cpuName[49] = {};
			int  cpuInfo[4] = {};
			__cpuid(cpuInfo, 0x80000002); memcpy(cpuName, cpuInfo, 16);
			__cpuid(cpuInfo, 0x80000003); memcpy(cpuName + 16, cpuInfo, 16);
			__cpuid(cpuInfo, 0x80000004); memcpy(cpuName + 32, cpuInfo, 16);
			const char* cpu = cpuName;
			while (*cpu == ' ') ++cpu;

			SYSTEM_INFO si = {};
			GetSystemInfo(&si);

			MEMORYSTATUSEX mem = { sizeof(mem) };
			GlobalMemoryStatusEx(&mem);
			ULONGLONG ramMB = mem.ullTotalPhys / (1024ull * 1024ull);

			LogToFile::Info("[ImGuiBackend] CPU: %s (%u cores) | RAM: %llu MB",
				cpu, static_cast<unsigned>(si.dwNumberOfProcessors), ramMB);

			// --- GPUs (enumerate all adapters) ---
			IDXGIFactory1* diagFactory = nullptr;
			if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&diagFactory))))
			{
				IDXGIAdapter* adapter = nullptr;
				for (UINT ai = 0; diagFactory->EnumAdapters(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai)
				{
					DXGI_ADAPTER_DESC desc = {};
					if (SUCCEEDED(adapter->GetDesc(&desc)))
					{
						ULONGLONG dedicMB = desc.DedicatedVideoMemory / (1024ull * 1024ull);
						ULONGLONG sharedMB = desc.SharedSystemMemory / (1024ull * 1024ull);

						// Check HDR support via IDXGIOutput6 on the first output of this adapter.
						bool hdr = false;
						int  displayW = 0, displayH = 0, refreshHz = 0;
						IDXGIOutput* output = nullptr;
						if (SUCCEEDED(adapter->EnumOutputs(0, &output)))
						{
							DXGI_OUTPUT_DESC od = {};
							if (SUCCEEDED(output->GetDesc(&od)))
							{
								displayW = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
								displayH = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;

								// Refresh rate from EnumDisplaySettings -- DXGI_OUTPUT_DESC1 doesn't carry it.
								DEVMODEW dm = {};
								dm.dmSize = sizeof(dm);
								if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
									refreshHz = static_cast<int>(dm.dmDisplayFrequency);
							}
							IDXGIOutput6* out6 = nullptr;
							if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&out6))))
							{
								DXGI_OUTPUT_DESC1 od1 = {};
								if (SUCCEEDED(out6->GetDesc1(&od1)))
									hdr = (od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
								out6->Release();
							}
							output->Release();
						}

						LogToFile::Info(
							"[ImGuiBackend] GPU[%u]: %ls (VendorId=0x%04X DevId=0x%04X) | "
							"VRAM: %llu MB dedicated, %llu MB shared | "
							"Display: %dx%d @%dHz | HDR: %s",
							ai, desc.Description,
							desc.VendorId, desc.DeviceId,
							dedicMB, sharedMB,
							displayW, displayH, refreshHz,
							hdr ? "YES" : "no");
					}
					adapter->Release();
				}
				diagFactory->Release();
			}
		}

		// Read the overlay open key from modloader.ini (next to game exe).
		// If the key is absent, write the default so the file self-documents.
		char openKeyName[32] = "F2";
		{
			wchar_t iniPath[MAX_PATH]{};
			GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
			wchar_t* slash = wcsrchr(iniPath, L'\\');
			if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - iniPath), L"modloader.ini");

			constexpr wchar_t kSentinel[] = L"__NOTSET__";
			wchar_t wKey[32]{};
			GetPrivateProfileStringW(L"UI", L"OpenKey", kSentinel, wKey, 32, iniPath);
			if (wcscmp(wKey, kSentinel) == 0)
			{
				// Key absent -- write the default so the INI is self-documenting.
				WritePrivateProfileStringW(L"UI", L"OpenKey", L"F2", iniPath);
				wcscpy_s(wKey, L"F2");
			}
			snprintf(openKeyName, sizeof(openKeyName), "%ls", wKey);
			UI::GlobalSettings::Load(iniPath);
		}

		UI::Overlay::SetOpenKeyName(openKeyName);
		PopulateImGuiAPI();
		PopulateTextureAPI();

		// Register the internal toggle keybind (configurable via modloader.ini [UI] OpenKey).
		{
			EModKey openKey = Hooks::Input::NameToModKey(openKeyName);
			if (openKey == EModKey::Unknown) openKey = EModKey::F2;
			static EModKey s_openKey = openKey;
			Hooks::Input::RegisterKeybind(s_openKey, EModKeyEvent::Pressed,
				[](EModKey, EModKeyEvent) { UI::ModLoaderWindow::Toggle(); });
		}

		// Create a minimal hidden window, then call CreateDeviceD3D to build a
		// temporary D3D12 device + swap chain for vtable extraction.
		// Both the window and all D3D12 objects are fully released before returning.
		WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, DefWindowProcW, 0L, 0L,
						   GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr,
						   L"DX12Tmp_ML", nullptr };
		RegisterClassExW(&wc);
		HWND tmpHwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
			0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);

		if (!CreateDeviceD3D(tmpHwnd))
			LogToFile::Error("[ImGuiBackend] CreateDeviceD3D failed -- ImGui will not render");

		// Temp window no longer needed -- all D3D12 objects were released inside CreateDeviceD3D.
		DestroyWindow(tmpHwnd);
		UnregisterClassW(wc.lpszClassName, GetModuleHandleW(nullptr));
	}

	void Shutdown()
	{
		g_shutdown = true;
		Hooks::InputHook::Remove();

		// Restore the original vtable entry.
		if (g_vtableSlot && g_originalPresent)
		{
			DWORD oldProtect = 0;
			VirtualProtect(g_vtableSlot, sizeof(void*), PAGE_READWRITE, &oldProtect);
			*g_vtableSlot = reinterpret_cast<void*>(g_originalPresent);
			VirtualProtect(g_vtableSlot, sizeof(void*), oldProtect, &oldProtect);
			g_vtableSlot = nullptr;
			g_originalPresent = nullptr;
		}

		// Restore the ECL vtable entry.
		if (g_eclVtableSlot && g_originalECL)
		{
			DWORD oldProtect = 0;
			VirtualProtect(g_eclVtableSlot, sizeof(void*), PAGE_READWRITE, &oldProtect);
			*g_eclVtableSlot = reinterpret_cast<void*>(g_originalECL);
			VirtualProtect(g_eclVtableSlot, sizeof(void*), oldProtect, &oldProtect);
			g_eclVtableSlot = nullptr;
			g_originalECL = nullptr;
		}

		// Restore the CreateSwapChainForHwnd vtable entry.
		if (g_cscfhVtableSlot && g_originalCSCFH)
		{
			DWORD oldProtect = 0;
			VirtualProtect(g_cscfhVtableSlot, sizeof(void*), PAGE_READWRITE, &oldProtect);
			*g_cscfhVtableSlot = reinterpret_cast<void*>(g_originalCSCFH);
			VirtualProtect(g_cscfhVtableSlot, sizeof(void*), oldProtect, &oldProtect);
			g_cscfhVtableSlot = nullptr;
			g_originalCSCFH = nullptr;
		}

		if (g_initialized)
		{
			// Flush all in-flight GPU work before releasing resources.
			if (g_cmdQueue && g_fence && g_fenceEvent)
			{
				g_cmdQueue->Signal(g_fence, ++g_fenceValue);
				g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
				WaitForSingleObject(g_fenceEvent, 500);
			}
			CleanupD3D12Resources();
			g_initialized = false;
		}
	}

	void SetRenderingReady()
	{
		g_renderingReady = true;
		LogToFile::Info("[ImGuiBackend] Rendering ready -- D3D12 init will proceed on next Present");
	}

	IModLoaderImGui* GetImGuiAPI()
	{
		return &g_imguiAPI;
	}

	IPluginImGuiTextures* GetTextureAPI()
	{
		return &g_textureAPI;
	}

	void RequestFontRebuild()
	{
		if (g_initialized)
			g_pendingFontRebuild.store(true, std::memory_order_release);
	}
}

#endif // MODLOADER_CLIENT_BUILD
