#include "version_proxy.h"
#include "log.h"
#include <cstdio>

// ---------------------------------------------------------------------------
// The real version.dll handle and original function pointers
// ---------------------------------------------------------------------------

static HMODULE g_realVersionDll = nullptr;

// One function pointer per export
static FARPROC g_origGetFileVersionInfoA = nullptr;
static FARPROC g_origGetFileVersionInfoByHandle = nullptr;
static FARPROC g_origGetFileVersionInfoExA = nullptr;
static FARPROC g_origGetFileVersionInfoExW = nullptr;
static FARPROC g_origGetFileVersionInfoSizeA = nullptr;
static FARPROC g_origGetFileVersionInfoSizeExA = nullptr;
static FARPROC g_origGetFileVersionInfoSizeExW = nullptr;
static FARPROC g_origGetFileVersionInfoSizeW = nullptr;
static FARPROC g_origGetFileVersionInfoW = nullptr;
static FARPROC g_origVerFindFileA = nullptr;
static FARPROC g_origVerFindFileW = nullptr;
static FARPROC g_origVerInstallFileA = nullptr;
static FARPROC g_origVerInstallFileW = nullptr;
static FARPROC g_origVerLanguageNameA = nullptr;
static FARPROC g_origVerLanguageNameW = nullptr;
static FARPROC g_origVerQueryValueA = nullptr;
static FARPROC g_origVerQueryValueW = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int g_resolvedCount = 0;
static int g_failedCount = 0;

static bool ResolveExport(const char* name, FARPROC& out)
{
	Log::Trace("  Resolving export: %s", name);

	out = GetProcAddress(g_realVersionDll, name);
	if (!out)
	{
		++g_failedCount;
		Log::Error("  FAILED to resolve: %s (error %lu)", name, GetLastError());
		return false;
	}

	++g_resolvedCount;
	Log::Debug("  Resolved: %-30s -> 0x%llX", name,
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(out)));
	return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool VersionProxy::Initialize()
{
	Log::Info("VersionProxy::Initialize() starting");

	// Build the path to the real version.dll in System32
	wchar_t systemDir[MAX_PATH]{};
	GetSystemDirectoryW(systemDir, MAX_PATH);
	Log::Debug("System directory: %ls", systemDir);

	wchar_t realPath[MAX_PATH]{};
	swprintf_s(realPath, L"%s\\version.dll", systemDir);
	Log::Info("Loading real version.dll from: %ls", realPath);

	g_realVersionDll = LoadLibraryW(realPath);
	if (!g_realVersionDll)
	{
		Log::Error("LoadLibraryW failed for real version.dll (error %lu)", GetLastError());
		Log::LogWin32Error("LoadLibraryW(version.dll)");
		return false;
	}

	Log::Info("Real version.dll loaded at 0x%llX",
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_realVersionDll)));

	// Resolve every export
	Log::Info("Resolving all 17 version.dll exports...");
	g_resolvedCount = 0;
	g_failedCount = 0;

	bool ok = true;
	ok &= ResolveExport("GetFileVersionInfoA", g_origGetFileVersionInfoA);
	ok &= ResolveExport("GetFileVersionInfoExA", g_origGetFileVersionInfoExA);
	ok &= ResolveExport("GetFileVersionInfoExW", g_origGetFileVersionInfoExW);
	ok &= ResolveExport("GetFileVersionInfoSizeA", g_origGetFileVersionInfoSizeA);
	ok &= ResolveExport("GetFileVersionInfoSizeExA", g_origGetFileVersionInfoSizeExA);
	ok &= ResolveExport("GetFileVersionInfoSizeExW", g_origGetFileVersionInfoSizeExW);
	ok &= ResolveExport("GetFileVersionInfoSizeW", g_origGetFileVersionInfoSizeW);
	ok &= ResolveExport("GetFileVersionInfoW", g_origGetFileVersionInfoW);
	ok &= ResolveExport("VerFindFileA", g_origVerFindFileA);
	ok &= ResolveExport("VerFindFileW", g_origVerFindFileW);
	ok &= ResolveExport("VerInstallFileA", g_origVerInstallFileA);
	ok &= ResolveExport("VerInstallFileW", g_origVerInstallFileW);
	ok &= ResolveExport("VerLanguageNameA", g_origVerLanguageNameA);
	ok &= ResolveExport("VerLanguageNameW", g_origVerLanguageNameW);
	ok &= ResolveExport("VerQueryValueA", g_origVerQueryValueA);
	ok &= ResolveExport("VerQueryValueW", g_origVerQueryValueW);

	// GetFileVersionInfoByHandle is an undocumented Windows-internal stub.
	// Wine/Proton does not implement it. Nothing actually calls it, so treat
	// it as optional — log a warning but don't fail initialization.
	if (!ResolveExport("GetFileVersionInfoByHandle", g_origGetFileVersionInfoByHandle))
		Log::Warn("GetFileVersionInfoByHandle not found (expected on Wine/Proton) — skipping");

	Log::Info("Export resolution complete: %d resolved, %d failed", g_resolvedCount, g_failedCount);

	if (!ok)
		Log::Error("Some exports failed to resolve — proxy may not function correctly!");

	return ok;
}

void VersionProxy::Shutdown()
{
	Log::Info("VersionProxy::Shutdown() starting");

	if (g_realVersionDll)
	{
		Log::Debug("Freeing real version.dll (handle 0x%llX)",
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_realVersionDll)));
		FreeLibrary(g_realVersionDll);
		g_realVersionDll = nullptr;
		Log::Info("Real version.dll unloaded");
	}
	else
	{
		Log::Debug("Real version.dll was already null — nothing to free");
	}

	Log::Info("VersionProxy::Shutdown() complete");
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

	FORWARD(vp_GetFileVersionInfoA, g_origGetFileVersionInfoA)
		FORWARD(vp_GetFileVersionInfoByHandle, g_origGetFileVersionInfoByHandle)
		FORWARD(vp_GetFileVersionInfoExA, g_origGetFileVersionInfoExA)
		FORWARD(vp_GetFileVersionInfoExW, g_origGetFileVersionInfoExW)
		FORWARD(vp_GetFileVersionInfoSizeA, g_origGetFileVersionInfoSizeA)
		FORWARD(vp_GetFileVersionInfoSizeExA, g_origGetFileVersionInfoSizeExA)
		FORWARD(vp_GetFileVersionInfoSizeExW, g_origGetFileVersionInfoSizeExW)
		FORWARD(vp_GetFileVersionInfoSizeW, g_origGetFileVersionInfoSizeW)
		FORWARD(vp_GetFileVersionInfoW, g_origGetFileVersionInfoW)
		FORWARD(vp_VerFindFileA, g_origVerFindFileA)
		FORWARD(vp_VerFindFileW, g_origVerFindFileW)
		FORWARD(vp_VerInstallFileA, g_origVerInstallFileA)
		FORWARD(vp_VerInstallFileW, g_origVerInstallFileW)
		FORWARD(vp_VerLanguageNameA, g_origVerLanguageNameA)
		FORWARD(vp_VerLanguageNameW, g_origVerLanguageNameW)
		FORWARD(vp_VerQueryValueA, g_origVerQueryValueA)
		FORWARD(vp_VerQueryValueW, g_origVerQueryValueW)

} // extern "C"
