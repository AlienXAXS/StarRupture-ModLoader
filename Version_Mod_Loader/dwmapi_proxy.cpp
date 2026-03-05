#include "dwmapi_proxy.h"
#include "logging/log.h"
#include <cstdio>

// ---------------------------------------------------------------------------
// The real dwmapi.dll handle and original function pointers
// ---------------------------------------------------------------------------

static HMODULE g_realDwmapiDll = nullptr;

// One function pointer per export
static FARPROC g_origDwmpDxGetWindowSharedSurface = nullptr;
static FARPROC g_origDwmpDxUpdateWindowSharedSurface = nullptr;
static FARPROC g_origDwmEnableComposition = nullptr;
static FARPROC g_origDllCanUnloadNow = nullptr;
static FARPROC g_origDllGetClassObject = nullptr;
static FARPROC g_origDwmAttachMilContent = nullptr;
static FARPROC g_origDwmDefWindowProc = nullptr;
static FARPROC g_origDwmDetachMilContent = nullptr;
static FARPROC g_origDwmEnableBlurBehindWindow = nullptr;
static FARPROC g_origDwmEnableMMCSS = nullptr;
static FARPROC g_origDwmExtendFrameIntoClientArea = nullptr;
static FARPROC g_origDwmFlush = nullptr;
static FARPROC g_origDwmGetColorizationColor = nullptr;
static FARPROC g_origDwmGetCompositionTimingInfo = nullptr;
static FARPROC g_origDwmGetGraphicsStreamClient = nullptr;
static FARPROC g_origDwmpGetColorizationParameters = nullptr;
static FARPROC g_origDwmpDxgiIsThreadDesktopComposited = nullptr;
static FARPROC g_origDwmGetGraphicsStreamTransformHint = nullptr;
static FARPROC g_origDwmGetTransportAttributes = nullptr;
static FARPROC g_origDwmpSetColorizationParameters = nullptr;
static FARPROC g_origDwmGetUnmetTabRequirements = nullptr;
static FARPROC g_origDwmGetWindowAttribute = nullptr;
static FARPROC g_origDwmpRenderFlick = nullptr;
static FARPROC g_origDwmpAllocateSecurityDescriptor = nullptr;
static FARPROC g_origDwmpFreeSecurityDescriptor = nullptr;
static FARPROC g_origDwmpEnableDDASupport = nullptr;
static FARPROC g_origDwmInvalidateIconicBitmaps = nullptr;
static FARPROC g_origDwmTetherTextContact = nullptr;
static FARPROC g_origDwmpUpdateProxyWindowForCapture = nullptr;
static FARPROC g_origDwmIsCompositionEnabled = nullptr;
static FARPROC g_origDwmModifyPreviousDxFrameDuration = nullptr;
static FARPROC g_origDwmQueryThumbnailSourceSize = nullptr;
static FARPROC g_origDwmRegisterThumbnail = nullptr;
static FARPROC g_origDwmRenderGesture = nullptr;
static FARPROC g_origDwmSetDxFrameDuration = nullptr;
static FARPROC g_origDwmSetIconicLivePreviewBitmap = nullptr;
static FARPROC g_origDwmSetIconicThumbnail = nullptr;
static FARPROC g_origDwmSetPresentParameters = nullptr;
static FARPROC g_origDwmSetWindowAttribute = nullptr;
static FARPROC g_origDwmShowContact = nullptr;
static FARPROC g_origDwmTetherContact = nullptr;
static FARPROC g_origDwmTransitionOwnedWindow = nullptr;
static FARPROC g_origDwmUnregisterThumbnail = nullptr;
static FARPROC g_origDwmUpdateThumbnailProperties = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int g_resolvedCount = 0;
static int g_failedCount = 0;

static bool ResolveExport(const char* name, FARPROC& out)
{
	LogToFile::Trace("  Resolving export: %s", name);

	out = GetProcAddress(g_realDwmapiDll, name);
	if (!out)
	{
		++g_failedCount;
		LogToFile::Warn("  Not found: %s (error %lu) — will return 0 if called", name, GetLastError());
		return false;
	}

	++g_resolvedCount;
	LogToFile::Debug("  Resolved: %-40s -> 0x%llX", name,
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(out)));
	return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool DwmapiProxy::Initialize()
{
	LogToFile::Info("DwmapiProxy::Initialize() starting");

	// Build the path to the real dwmapi.dll in System32
	wchar_t systemDir[MAX_PATH]{};
	GetSystemDirectoryW(systemDir, MAX_PATH);
	LogToFile::Debug("System directory: %ls", systemDir);

	wchar_t realPath[MAX_PATH]{};
	swprintf_s(realPath, L"%s\\dwmapi.dll", systemDir);
	LogToFile::Info("Loading real dwmapi.dll from: %ls", realPath);

	g_realDwmapiDll = LoadLibraryW(realPath);
	if (!g_realDwmapiDll)
	{
		LogToFile::Error("LoadLibraryW failed for real dwmapi.dll (error %lu)", GetLastError());
		LogToFile::LogWin32Error("LoadLibraryW(dwmapi.dll)");
		return false;
	}

	LogToFile::Info("Real dwmapi.dll loaded at 0x%llX",
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_realDwmapiDll)));

	// Resolve every export — failures are warnings, not fatal (some are undocumented internals)
	LogToFile::Info("Resolving all 44 dwmapi.dll exports...");
	g_resolvedCount = 0;
	g_failedCount = 0;

	ResolveExport("DwmpDxGetWindowSharedSurface",       g_origDwmpDxGetWindowSharedSurface);
	ResolveExport("DwmpDxUpdateWindowSharedSurface",    g_origDwmpDxUpdateWindowSharedSurface);
	ResolveExport("DwmEnableComposition",               g_origDwmEnableComposition);
	ResolveExport("DllCanUnloadNow",                    g_origDllCanUnloadNow);
	ResolveExport("DllGetClassObject",                  g_origDllGetClassObject);
	ResolveExport("DwmAttachMilContent",                g_origDwmAttachMilContent);
	ResolveExport("DwmDefWindowProc",                   g_origDwmDefWindowProc);
	ResolveExport("DwmDetachMilContent",                g_origDwmDetachMilContent);
	ResolveExport("DwmEnableBlurBehindWindow",          g_origDwmEnableBlurBehindWindow);
	ResolveExport("DwmEnableMMCSS",                     g_origDwmEnableMMCSS);
	ResolveExport("DwmExtendFrameIntoClientArea",       g_origDwmExtendFrameIntoClientArea);
	ResolveExport("DwmFlush",                           g_origDwmFlush);
	ResolveExport("DwmGetColorizationColor",            g_origDwmGetColorizationColor);
	ResolveExport("DwmGetCompositionTimingInfo",        g_origDwmGetCompositionTimingInfo);
	ResolveExport("DwmGetGraphicsStreamClient",         g_origDwmGetGraphicsStreamClient);
	ResolveExport("DwmpGetColorizationParameters",      g_origDwmpGetColorizationParameters);
	ResolveExport("DwmpDxgiIsThreadDesktopComposited",  g_origDwmpDxgiIsThreadDesktopComposited);
	ResolveExport("DwmGetGraphicsStreamTransformHint",  g_origDwmGetGraphicsStreamTransformHint);
	ResolveExport("DwmGetTransportAttributes",          g_origDwmGetTransportAttributes);
	ResolveExport("DwmpSetColorizationParameters",      g_origDwmpSetColorizationParameters);
	ResolveExport("DwmGetUnmetTabRequirements",         g_origDwmGetUnmetTabRequirements);
	ResolveExport("DwmGetWindowAttribute",              g_origDwmGetWindowAttribute);
	ResolveExport("DwmpRenderFlick",                    g_origDwmpRenderFlick);
	ResolveExport("DwmpAllocateSecurityDescriptor",     g_origDwmpAllocateSecurityDescriptor);
	ResolveExport("DwmpFreeSecurityDescriptor",         g_origDwmpFreeSecurityDescriptor);
	ResolveExport("DwmpEnableDDASupport",               g_origDwmpEnableDDASupport);
	ResolveExport("DwmInvalidateIconicBitmaps",         g_origDwmInvalidateIconicBitmaps);
	ResolveExport("DwmTetherTextContact",               g_origDwmTetherTextContact);
	ResolveExport("DwmpUpdateProxyWindowForCapture",    g_origDwmpUpdateProxyWindowForCapture);
	ResolveExport("DwmIsCompositionEnabled",            g_origDwmIsCompositionEnabled);
	ResolveExport("DwmModifyPreviousDxFrameDuration",   g_origDwmModifyPreviousDxFrameDuration);
	ResolveExport("DwmQueryThumbnailSourceSize",        g_origDwmQueryThumbnailSourceSize);
	ResolveExport("DwmRegisterThumbnail",               g_origDwmRegisterThumbnail);
	ResolveExport("DwmRenderGesture",                   g_origDwmRenderGesture);
	ResolveExport("DwmSetDxFrameDuration",              g_origDwmSetDxFrameDuration);
	ResolveExport("DwmSetIconicLivePreviewBitmap",      g_origDwmSetIconicLivePreviewBitmap);
	ResolveExport("DwmSetIconicThumbnail",              g_origDwmSetIconicThumbnail);
	ResolveExport("DwmSetPresentParameters",            g_origDwmSetPresentParameters);
	ResolveExport("DwmSetWindowAttribute",              g_origDwmSetWindowAttribute);
	ResolveExport("DwmShowContact",                     g_origDwmShowContact);
	ResolveExport("DwmTetherContact",                   g_origDwmTetherContact);
	ResolveExport("DwmTransitionOwnedWindow",           g_origDwmTransitionOwnedWindow);
	ResolveExport("DwmUnregisterThumbnail",             g_origDwmUnregisterThumbnail);
	ResolveExport("DwmUpdateThumbnailProperties",       g_origDwmUpdateThumbnailProperties);

	LogToFile::Info("Export resolution complete: %d resolved, %d not found", g_resolvedCount, g_failedCount);

	return true;
}

void DwmapiProxy::Shutdown()
{
	LogToFile::Info("DwmapiProxy::Shutdown() starting");

	if (g_realDwmapiDll)
	{
		LogToFile::Debug("Freeing real dwmapi.dll (handle 0x%llX)",
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_realDwmapiDll)));
		FreeLibrary(g_realDwmapiDll);
		g_realDwmapiDll = nullptr;
		LogToFile::Info("Real dwmapi.dll unloaded");
	}
	else
	{
		LogToFile::Debug("Real dwmapi.dll was already null — nothing to free");
	}

	LogToFile::Info("DwmapiProxy::Shutdown() complete");
}

// ---------------------------------------------------------------------------
// Forwarded exports
// ---------------------------------------------------------------------------

extern "C"
{

	typedef DWORD_PTR(WINAPI* GenericFunc)(
		DWORD_PTR a1, DWORD_PTR a2, DWORD_PTR a3, DWORD_PTR a4,
		DWORD_PTR a5, DWORD_PTR a6, DWORD_PTR a7, DWORD_PTR a8);

#define FORWARD(vpName, origPtr)                                                    \
    __declspec(dllexport) DWORD_PTR WINAPI vpName(                                  \
        DWORD_PTR a1, DWORD_PTR a2, DWORD_PTR a3, DWORD_PTR a4,                    \
        DWORD_PTR a5, DWORD_PTR a6, DWORD_PTR a7, DWORD_PTR a8)                    \
    {                                                                               \
        if (!origPtr) return 0;                                                     \
        return reinterpret_cast<GenericFunc>(origPtr)(a1,a2,a3,a4,a5,a6,a7,a8);     \
    }

	FORWARD(vp_DwmpDxGetWindowSharedSurface,      g_origDwmpDxGetWindowSharedSurface)
	FORWARD(vp_DwmpDxUpdateWindowSharedSurface,   g_origDwmpDxUpdateWindowSharedSurface)
	FORWARD(vp_DwmEnableComposition,              g_origDwmEnableComposition)
	FORWARD(vp_DllCanUnloadNow,                   g_origDllCanUnloadNow)
	FORWARD(vp_DllGetClassObject,                 g_origDllGetClassObject)
	FORWARD(vp_DwmAttachMilContent,               g_origDwmAttachMilContent)
	FORWARD(vp_DwmDefWindowProc,                  g_origDwmDefWindowProc)
	FORWARD(vp_DwmDetachMilContent,               g_origDwmDetachMilContent)
	FORWARD(vp_DwmEnableBlurBehindWindow,         g_origDwmEnableBlurBehindWindow)
	FORWARD(vp_DwmEnableMMCSS,                    g_origDwmEnableMMCSS)
	FORWARD(vp_DwmExtendFrameIntoClientArea,      g_origDwmExtendFrameIntoClientArea)
	FORWARD(vp_DwmFlush,                          g_origDwmFlush)
	FORWARD(vp_DwmGetColorizationColor,           g_origDwmGetColorizationColor)
	FORWARD(vp_DwmGetCompositionTimingInfo,       g_origDwmGetCompositionTimingInfo)
	FORWARD(vp_DwmGetGraphicsStreamClient,        g_origDwmGetGraphicsStreamClient)
	FORWARD(vp_DwmpGetColorizationParameters,     g_origDwmpGetColorizationParameters)
	FORWARD(vp_DwmpDxgiIsThreadDesktopComposited, g_origDwmpDxgiIsThreadDesktopComposited)
	FORWARD(vp_DwmGetGraphicsStreamTransformHint, g_origDwmGetGraphicsStreamTransformHint)
	FORWARD(vp_DwmGetTransportAttributes,         g_origDwmGetTransportAttributes)
	FORWARD(vp_DwmpSetColorizationParameters,     g_origDwmpSetColorizationParameters)
	FORWARD(vp_DwmGetUnmetTabRequirements,        g_origDwmGetUnmetTabRequirements)
	FORWARD(vp_DwmGetWindowAttribute,             g_origDwmGetWindowAttribute)
	FORWARD(vp_DwmpRenderFlick,                   g_origDwmpRenderFlick)
	FORWARD(vp_DwmpAllocateSecurityDescriptor,    g_origDwmpAllocateSecurityDescriptor)
	FORWARD(vp_DwmpFreeSecurityDescriptor,        g_origDwmpFreeSecurityDescriptor)
	FORWARD(vp_DwmpEnableDDASupport,              g_origDwmpEnableDDASupport)
	FORWARD(vp_DwmInvalidateIconicBitmaps,        g_origDwmInvalidateIconicBitmaps)
	FORWARD(vp_DwmTetherTextContact,              g_origDwmTetherTextContact)
	FORWARD(vp_DwmpUpdateProxyWindowForCapture,   g_origDwmpUpdateProxyWindowForCapture)
	FORWARD(vp_DwmIsCompositionEnabled,           g_origDwmIsCompositionEnabled)
	FORWARD(vp_DwmModifyPreviousDxFrameDuration,  g_origDwmModifyPreviousDxFrameDuration)
	FORWARD(vp_DwmQueryThumbnailSourceSize,       g_origDwmQueryThumbnailSourceSize)
	FORWARD(vp_DwmRegisterThumbnail,              g_origDwmRegisterThumbnail)
	FORWARD(vp_DwmRenderGesture,                  g_origDwmRenderGesture)
	FORWARD(vp_DwmSetDxFrameDuration,             g_origDwmSetDxFrameDuration)
	FORWARD(vp_DwmSetIconicLivePreviewBitmap,     g_origDwmSetIconicLivePreviewBitmap)
	FORWARD(vp_DwmSetIconicThumbnail,             g_origDwmSetIconicThumbnail)
	FORWARD(vp_DwmSetPresentParameters,           g_origDwmSetPresentParameters)
	FORWARD(vp_DwmSetWindowAttribute,             g_origDwmSetWindowAttribute)
	FORWARD(vp_DwmShowContact,                    g_origDwmShowContact)
	FORWARD(vp_DwmTetherContact,                  g_origDwmTetherContact)
	FORWARD(vp_DwmTransitionOwnedWindow,          g_origDwmTransitionOwnedWindow)
	FORWARD(vp_DwmUnregisterThumbnail,            g_origDwmUnregisterThumbnail)
	FORWARD(vp_DwmUpdateThumbnailProperties,      g_origDwmUpdateThumbnailProperties)

} // extern "C"
