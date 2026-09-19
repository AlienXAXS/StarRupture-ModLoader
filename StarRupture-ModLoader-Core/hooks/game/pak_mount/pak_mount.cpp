#include "pch.h"
#include "pak_mount.h"

#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "utils/mem_utils.h"
#include "../scan_patterns.h"

#include <windows.h>
#include <psapi.h>
#include <cstring>

namespace Hooks::PakMount
{
    // -----------------------------------------------------------------------
    // Engine ABI, as read from the 5.6.1 binaries (see scan_patterns.h and
    // the IDA notes in pak_mount.h). Every raw offset the module depends on
    // is here, in one place, so a game update that moves one has one place
    // to fix.
    // -----------------------------------------------------------------------

    // FString: TArray<wchar_t> -- { Data, Num (incl. terminator), Max }.
    struct FStringView
    {
        const wchar_t* Data;
        int32_t        Num;
        int32_t        Max;
    };

    // FPakPlatformFile::FPakListEntry -- { uint32 ReadOrder; FPakFile* PakFile }
    // (TRefCountPtr<FPakFile> is one pointer). 16 bytes.
    struct FPakListEntry
    {
        uint32_t ReadOrder;
        uint32_t pad_;
        void*    PakFile;
    };

    struct TArrayView
    {
        void*   Data;
        int32_t Num;
        int32_t Max;
    };

    // FPakPlatformFile layout.
    constexpr size_t kPakFiles_Offset        = 0x10;  // TArray<FPakListEntry>
    constexpr size_t kPakListCritical_Offset = 0x38;  // FWindowsRecursiveMutex == CRITICAL_SECTION

    // FPakFile layout. Only the IPakFile sub-object is used; everything else
    // is reached through its vtable so the rest of the class can shift freely.
    constexpr size_t kFPakFile_IPakFile_Offset = 0x10;

    // IPakFile vtable slots.
    constexpr size_t kIPakFile_PakGetPakFilename   = 0;
    constexpr size_t kIPakFile_PakGetPakchunkIndex = 2;
    constexpr size_t kIPakFile_PakGetMountPoint    = 4;
    constexpr size_t kIPakFile_GetNumFiles         = 5;

    using GetManager_t       = void* (__fastcall*)();
    using FindPlatformFile_t = void* (__fastcall*)(void* manager, const wchar_t* name);
    using HandleMount_t      = void* (__fastcall*)(void* pakPlatformFile, const FStringView* path, int order);
    using HandleUnmount_t    = bool  (__fastcall*)(void* pakPlatformFile, const FStringView* path);

    using PakGetFString_t    = const FStringView* (__fastcall*)(void* iPakFile);
    using PakGetInt_t        = int (__fastcall*)(void* iPakFile);

    static GetManager_t       g_getManager       = nullptr;
    static FindPlatformFile_t g_findPlatformFile = nullptr;
    static HandleMount_t      g_handleMount      = nullptr;
    static HandleUnmount_t    g_handleUnmount    = nullptr;
    static void*              g_pakPlatformFile  = nullptr;
    static bool               g_scanAttempted    = false;
    static bool               g_patternsOk       = false;
    static bool               g_waitLogged       = false;

    static uintptr_t g_mainBase = 0;
    static size_t    g_mainSize = 0;

    static bool InMainModule(const void* p)
    {
        const uintptr_t a = reinterpret_cast<uintptr_t>(p);
        return g_mainBase && a >= g_mainBase && a < g_mainBase + g_mainSize;
    }

    // -----------------------------------------------------------------------
    // Resolution
    // -----------------------------------------------------------------------
    static bool DecodeAnchor(uintptr_t anchor)
    {
        // Both call sites are E8 rel32 and the pattern proves the opcode is
        // there; decode the targets and insist they land inside the exe.
        const uintptr_t getCall  = anchor + ScanPatterns::FPakFileModule_ShutdownModule_GetCallOffset;
        const uintptr_t findCall = anchor + ScanPatterns::FPakFileModule_ShutdownModule_FindCallOffset;

        if (*reinterpret_cast<const uint8_t*>(getCall) != 0xE8 ||
            *reinterpret_cast<const uint8_t*>(findCall) != 0xE8)
        {
            ModLoaderLogger::LogWarn(L"[PakMount] [FAIL] ShutdownModule anchor matched but the call opcodes are not where expected");
            return false;
        }

        const uintptr_t getTarget  = MemUtils::ResolveRelCall(getCall);
        const uintptr_t findTarget = MemUtils::ResolveRelCall(findCall);

        if (!InMainModule(reinterpret_cast<void*>(getTarget)) || !InMainModule(reinterpret_cast<void*>(findTarget)))
        {
            ModLoaderLogger::LogWarn(L"[PakMount] [FAIL] decoded call targets fall outside the game module");
            return false;
        }

        g_getManager       = reinterpret_cast<GetManager_t>(getTarget);
        g_findPlatformFile = reinterpret_cast<FindPlatformFile_t>(findTarget);
        return true;
    }

    // SEH-only helpers: no C++ objects in scope (C2712).
    //
    // FindPlatformFile check()s TopmostPlatformFile != nullptr before it
    // walks the chain, and a failed check in a shipping build is not an
    // exception SEH can swallow -- it goes through the engine's crash handler
    // and the process is gone. TopmostPlatformFile is null until
    // FEngineLoop::PreInit sets the chain up, which is long after the
    // loader's Stage 1 runs, so the manager's first field (the topmost
    // pointer) is read here first and a null means "not yet", never a call.
    // This is what crashed the first build of this module on every launch.
    static bool LocatePakPlatformFileSEH(void** out, bool* notReady)
    {
        __try
        {
            *notReady = false;
            void* manager = g_getManager();
            if (!manager)
                return false;
            if (*static_cast<void**>(manager) == nullptr)   // FPlatformFileManager::TopmostPlatformFile
            {
                *notReady = true;
                return false;
            }
            *out = g_findPlatformFile(manager, L"PakFile");
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ResolvePatterns()
    {
        if (g_patternsOk)    return true;
        if (g_scanAttempted) return false;
        g_scanAttempted = true;

        HMODULE mainModule = GetModuleHandleW(nullptr);
        MODULEINFO mi{};
        if (GetModuleInformation(GetCurrentProcess(), mainModule, &mi, sizeof(mi)))
        {
            g_mainBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            g_mainSize = mi.SizeOfImage;
        }

        const uintptr_t anchor = Scanner::FindPatternInMainModule(
            "FPakFileModule::ShutdownModule", ScanPatterns::FPakFileModule_ShutdownModule);
        const uintptr_t mount = Scanner::FindPatternInMainModule(
            "FPakPlatformFile::HandleMountPakDelegate", ScanPatterns::FPakPlatformFile_HandleMountPakDelegate);
        const uintptr_t unmount = Scanner::FindPatternInMainModule(
            "FPakPlatformFile::HandleUnmountPakDelegate", ScanPatterns::FPakPlatformFile_HandleUnmountPakDelegate);

        if (!anchor || !mount || !unmount)
        {
            ModLoaderLogger::LogWarn(
                L"[PakMount] [FAIL] pattern(s) not found (anchor=%d mount=%d unmount=%d) -- runtime pak mounting unavailable",
                anchor ? 1 : 0, mount ? 1 : 0, unmount ? 1 : 0);
            return false;
        }

        if (!DecodeAnchor(anchor))
            return false;

        g_handleMount   = reinterpret_cast<HandleMount_t>(mount);
        g_handleUnmount = reinterpret_cast<HandleUnmount_t>(unmount);
        g_patternsOk    = true;

        ModLoaderLogger::LogInfo(
            L"[PakMount] [OK] FPlatformFileManager::Get at base+0x%llX, FindPlatformFile at base+0x%llX, "
            L"HandleMountPakDelegate at base+0x%llX, HandleUnmountPakDelegate at base+0x%llX "
            L"(FPakPlatformFile is looked up on first use, once the engine has built its file chain)",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_getManager) - g_mainBase),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_findPlatformFile) - g_mainBase),
            static_cast<unsigned long long>(mount - g_mainBase),
            static_cast<unsigned long long>(unmount - g_mainBase));
        return true;
    }

    bool Resolve()
    {
        if (g_pakPlatformFile) return true;
        if (!ResolvePatterns()) return false;

        // Retried on every call until it succeeds: cheap (one call that
        // returns a static's address, one pointer read) and the only way to
        // be correct for a caller that arrives before FEngineLoop::PreInit.
        void* pakFile  = nullptr;
        bool  notReady = false;
        if (!LocatePakPlatformFileSEH(&pakFile, &notReady))
        {
            if (notReady)
            {
                if (!g_waitLogged)
                {
                    g_waitLogged = true;
                    ModLoaderLogger::LogDebug(L"[PakMount] Platform file chain not built yet -- FPakPlatformFile lookup deferred");
                }
                return false;
            }
            ModLoaderLogger::LogError(L"[PakMount] Exception while locating the PakFile platform layer");
            return false;
        }
        if (!pakFile)
        {
            // No "PakFile" layer means the engine is reading loose files (an
            // uncooked or -pak-less launch); nothing to mount into. Final:
            // the chain does not change once built.
            ModLoaderLogger::LogWarn(L"[PakMount] [FAIL] FPlatformFileManager has no PakFile layer -- runtime pak mounting unavailable");
            g_patternsOk = false;
            return false;
        }

        g_pakPlatformFile = pakFile;
        ModLoaderLogger::LogInfo(L"[PakMount] [OK] FPakPlatformFile located at 0x%llX",
                                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_pakPlatformFile)));
        return true;
    }

    bool  IsAvailable()        { return Resolve(); }
    void* GetPakPlatformFile() { Resolve(); return g_pakPlatformFile; }

    // -----------------------------------------------------------------------
    // Paths
    // -----------------------------------------------------------------------
    std::wstring NormalizePath(const std::wstring& path)
    {
        if (path.empty())
            return path;

        std::wstring in = path;
        for (wchar_t& c : in)
            if (c == L'/') c = L'\\';

        const bool relative = !(in.size() >= 2 && (in[1] == L':' || (in[0] == L'\\' && in[1] == L'\\')));
        if (relative)
        {
            wchar_t exe[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            wchar_t* slash = wcsrchr(exe, L'\\');
            if (slash) *(slash + 1) = L'\0';
            in = std::wstring(exe) + in;
        }

        wchar_t full[MAX_PATH * 2]{};
        const DWORD n = GetFullPathNameW(in.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
        std::wstring out = (n && n < std::size(full)) ? std::wstring(full) : in;
        for (wchar_t& c : out)
            if (c == L'\\') c = L'/';
        return out;
    }

    // -----------------------------------------------------------------------
    // Reading an IPakFile through its vtable
    // -----------------------------------------------------------------------
    static const void* const* VTableOf(void* iPakFile)
    {
        if (!iPakFile) return nullptr;
        const void* const* vt = *reinterpret_cast<const void* const**>(iPakFile);
        // A vtable that is not in the game module is not an IPakFile vtable.
        return InMainModule(vt) ? vt : nullptr;
    }

    static void CopyFString(const FStringView* s, std::wstring& out)
    {
        out.clear();
        if (!s || !s->Data || s->Num <= 1)
            return;
        out.assign(s->Data, static_cast<size_t>(s->Num - 1));
    }

    // Fills `info` from a live IPakFile*. SEH-only body; strings are filled
    // through pointers so no C++ object lives in the __try scope.
    static bool ReadPakInfoSEH(void* iPakFile, std::wstring* filename, std::wstring* mountPoint,
                               int* pakchunk, int* numFiles)
    {
        __try
        {
            const void* const* vt = VTableOf(iPakFile);
            if (!vt) return false;

            const FStringView* fn = reinterpret_cast<PakGetFString_t>(vt[kIPakFile_PakGetPakFilename])(iPakFile);
            CopyFString(fn, *filename);
            const FStringView* mp = reinterpret_cast<PakGetFString_t>(vt[kIPakFile_PakGetMountPoint])(iPakFile);
            CopyFString(mp, *mountPoint);
            *pakchunk = reinterpret_cast<PakGetInt_t>(vt[kIPakFile_PakGetPakchunkIndex])(iPakFile);
            *numFiles = reinterpret_cast<PakGetInt_t>(vt[kIPakFile_GetNumFiles])(iPakFile);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // -----------------------------------------------------------------------
    // Mount / Unmount
    // -----------------------------------------------------------------------
    static bool CallMountSEH(const FStringView* path, int order, void** outPak, unsigned long* code)
    {
        __try
        {
            *outPak = g_handleMount(g_pakPlatformFile, path, order);
            return true;
        }
        __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool CallUnmountSEH(const FStringView* path, bool* outOk, unsigned long* code)
    {
        __try
        {
            *outOk = g_handleUnmount(g_pakPlatformFile, path);
            return true;
        }
        __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    Status Mount(const std::wstring& pakPath, int order, MountedPak* outInfo)
    {
        if (!Resolve())
            return Status::Unavailable;

        // The handler reads the FString and copies what it needs into the new
        // FPakFile; it never frees or keeps our buffer, so a view over our
        // own string is enough. Num counts the terminator, as FString does.
        FStringView view{ pakPath.c_str(), static_cast<int32_t>(pakPath.size() + 1), static_cast<int32_t>(pakPath.size() + 1) };

        void*         pak  = nullptr;
        unsigned long code = 0;
        if (!CallMountSEH(&view, order, &pak, &code))
        {
            ModLoaderLogger::LogError(L"[PakMount] Exception 0x%08lX inside FPakPlatformFile::Mount for %s", code, pakPath.c_str());
            return Status::Fault;
        }
        if (!pak)
        {
            ModLoaderLogger::LogWarn(L"[PakMount] Engine refused to mount %s (order %d) -- see the engine log (LogPakFile) for why", pakPath.c_str(), order);
            return Status::Refused;
        }

        if (outInfo)
        {
            outInfo->pakFile   = pak;
            outInfo->readOrder = order;
            if (!ReadPakInfoSEH(pak, &outInfo->pakFilename, &outInfo->mountPoint, &outInfo->pakchunkIndex, &outInfo->numFiles))
                ModLoaderLogger::LogWarn(L"[PakMount] Mounted %s but could not read its IPakFile details", pakPath.c_str());
        }

        ModLoaderLogger::LogInfo(L"[PakMount] Mounted %s (order %d, IPakFile=0x%llX)",
                                 pakPath.c_str(), order, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pak)));
        return Status::Ok;
    }

    Status Unmount(const std::wstring& pakPath)
    {
        if (!Resolve())
            return Status::Unavailable;

        FStringView view{ pakPath.c_str(), static_cast<int32_t>(pakPath.size() + 1), static_cast<int32_t>(pakPath.size() + 1) };

        bool          ok   = false;
        unsigned long code = 0;
        if (!CallUnmountSEH(&view, &ok, &code))
        {
            ModLoaderLogger::LogError(L"[PakMount] Exception 0x%08lX inside FPakPlatformFile::Unmount for %s", code, pakPath.c_str());
            return Status::Fault;
        }
        if (!ok)
        {
            ModLoaderLogger::LogWarn(L"[PakMount] Engine reported failure unmounting %s", pakPath.c_str());
            return Status::Refused;
        }

        ModLoaderLogger::LogInfo(L"[PakMount] Unmounted %s", pakPath.c_str());
        return Status::Ok;
    }

    // -----------------------------------------------------------------------
    // Enumeration
    //
    // Walks FPakPlatformFile::PakFiles under PakListCritical, the same lock
    // Mount/Unmount take, so the array cannot be resized underneath us. The
    // FPakFile pointers are refcounted by the array entries and stay valid
    // for as long as the lock is held.
    // -----------------------------------------------------------------------
    static bool SnapshotEntriesSEH(std::vector<FPakListEntry>* entries)
    {
        __try
        {
            auto* cs  = reinterpret_cast<CRITICAL_SECTION*>(static_cast<char*>(g_pakPlatformFile) + kPakListCritical_Offset);
            auto* arr = reinterpret_cast<TArrayView*>(static_cast<char*>(g_pakPlatformFile) + kPakFiles_Offset);

            EnterCriticalSection(cs);
            const int num = arr->Num;
            if (num > 0 && num < 100000 && arr->Data)
            {
                const auto* data = static_cast<const FPakListEntry*>(arr->Data);
                for (int i = 0; i < num; ++i)
                    entries->push_back(data[i]);
            }
            LeaveCriticalSection(cs);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool EnumerateMounted(std::vector<MountedPak>& out)
    {
        out.clear();
        if (!Resolve())
            return false;

        // Two passes: copy the entries under the engine lock (no allocation
        // beyond the vector, no virtual calls), then read each pak's details
        // outside it. A pak unmounted between the two passes is the only
        // hazard, and the loader is the only thing that unmounts at runtime,
        // so a caller holding the registry lock is safe.
        std::vector<FPakListEntry> entries;
        entries.reserve(16);
        if (!SnapshotEntriesSEH(&entries))
        {
            ModLoaderLogger::LogError(L"[PakMount] Exception while walking FPakPlatformFile::PakFiles");
            return false;
        }

        for (const FPakListEntry& e : entries)
        {
            if (!e.PakFile)
                continue;

            MountedPak info;
            info.readOrder = static_cast<int>(e.ReadOrder);
            info.pakFile   = static_cast<char*>(e.PakFile) + kFPakFile_IPakFile_Offset;
            if (!ReadPakInfoSEH(info.pakFile, &info.pakFilename, &info.mountPoint, &info.pakchunkIndex, &info.numFiles))
            {
                ModLoaderLogger::LogWarn(L"[PakMount] Could not read a mounted pak's details (FPakFile=0x%llX) -- skipped",
                                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(e.PakFile)));
                continue;
            }
            out.push_back(std::move(info));
        }
        return true;
    }

    bool FindMounted(const std::wstring& pakPath, MountedPak* out)
    {
        std::vector<MountedPak> all;
        if (!EnumerateMounted(all))
            return false;

        const std::wstring key = NormalizePath(pakPath);
        for (MountedPak& p : all)
        {
            if (_wcsicmp(NormalizePath(p.pakFilename).c_str(), key.c_str()) == 0)
            {
                if (out) *out = std::move(p);
                return true;
            }
        }
        return false;
    }
}
