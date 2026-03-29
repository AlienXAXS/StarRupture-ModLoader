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
	std::atomic<bool> g_renderingReady = false;

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
// WndProc subclass -- forwards messages to ImGui, swallows input when UI open
// ---------------------------------------------------------------------------
static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
		return true;

	if (UI::ModLoaderWindow::IsOpen())
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

	// SRV descriptor heap (1 slot for ImGui font texture).
	{
		D3D12_DESCRIPTOR_HEAP_DESC desc = {};
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.NumDescriptors = 1;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		desc.NodeMask = nodeMask;
		if (FAILED(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_srvHeap))))
		{
			LogToFile::Error("[ImGuiBackend] Failed to create SRV heap");
			return false;
		}
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
	if (g_srvHeap) { g_srvHeap->Release();              g_srvHeap = nullptr; }
	if (g_device) { g_device->Release();               g_device = nullptr; }
	g_capturedQueue = nullptr;   // weak ref -- do NOT Release
	g_swapChain = nullptr;
	g_fenceValue = 0;
	g_rtvFormat = DXGI_FORMAT_UNKNOWN;
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

	// Cursor management: only active while the modloader window is open.
	// Cursor management: hand control to ImGui only while the modloader window
	// is open.  NoMouseCursorChange prevents ImGui's Win32 backend from calling
	// SetCursor() every frame -- without it, it fights UE5's cursor management
	// and causes visible flickering whenever our window is closed.
	bool uiOpen = UI::ModLoaderWindow::IsOpen();
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
}

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
}

#endif // MODLOADER_CLIENT_BUILD
