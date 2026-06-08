#include "pch.h"
#include "object_lookup.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"

// Documented function-pointer signatures for the addresses resolved below.
// SDK types are not referenced here so this module stays independent of the
// SDK include chain -- callers cast the raw address to the typedef they need.
//
//   StaticFindObject_ByPath_t      = UObject*(__fastcall*)(UClass*, FTopLevelAssetPath*, bool)
//   StaticFindObject_ByName_t      = UObject*(__fastcall*)(UClass*, UObject*, const wchar_t*, bool)
//   StaticFindObjectSafe_ByPath_t  = UObject*(__fastcall*)(UClass*, FTopLevelAssetPath*, bool)
//   StaticFindObjectSafe_ByName_t  = UObject*(__fastcall*)(UClass*, UObject*, const wchar_t*, bool)
//   StaticFindObjectFast_t         = UObject*(__fastcall*)(UClass*, UObject*, FName, bool, EObjectFlags, EInternalObjectFlags)
//   FindPackage_t                  = UPackage*(__fastcall*)(UObject*, const wchar_t*)
//   UPackage_FullyLoad_t           = void(__fastcall*)(UPackage*)
//   LoadPackage_t                  = UPackage*(__fastcall*)(UPackage*, FScriptContainerElement*, unsigned int, FArchive*, const FLinkerInstancingContext*)
//   FAssetData_FastGetAsset_t      = UObject*(__fastcall*)(FAssetData*, bool, TMap<FName,FName,FDefaultSetAllocator,TDefaultMapHashableKeyFuncs<FName,FName,0>>*)

namespace Hooks::ObjectLookup
{
    static uintptr_t g_staticFindObject_ByPath = 0;
    static uintptr_t g_staticFindObject_ByName = 0;
    static uintptr_t g_staticFindObjectSafe_ByPath = 0;
    static uintptr_t g_staticFindObjectSafe_ByName = 0;
    static uintptr_t g_staticFindObjectFast = 0;
    static uintptr_t g_findPackage = 0;
    static uintptr_t g_upackage_FullyLoad = 0;
    static uintptr_t g_loadPackage = 0;
    static uintptr_t g_fassetData_FastGetAsset = 0;

    static bool Resolve(const char* name, const char* pattern, uintptr_t& out)
    {
        out = Scanner::FindPatternInMainModule(name, pattern);
        if (!out)
        {
            ModLoaderLogger::LogWarn(L"[ObjectLookup] Failed to find %S pattern", name);
            return false;
        }

        ModLoaderLogger::LogInfo(L"[ObjectLookup] Resolved %S at 0x%llX", name, out);
        return true;
    }

    bool Install()
    {
        ModLoaderLogger::LogInfo(L"[ObjectLookup] Scanning for object/package lookup functions...");

        bool ok = true;
        ok &= Resolve("StaticFindObject (ByPath)", ScanPatterns::StaticFindObject_ByPath, g_staticFindObject_ByPath);
        ok &= Resolve("StaticFindObject (ByName)", ScanPatterns::StaticFindObject_ByName, g_staticFindObject_ByName);
        ok &= Resolve("StaticFindObjectSafe (ByPath)", ScanPatterns::StaticFindObjectSafe_ByPath, g_staticFindObjectSafe_ByPath);
        ok &= Resolve("StaticFindObjectSafe (ByName)", ScanPatterns::StaticFindObjectSafe_ByName, g_staticFindObjectSafe_ByName);
        ok &= Resolve("StaticFindObjectFast", ScanPatterns::StaticFindObjectFast, g_staticFindObjectFast);
        ok &= Resolve("FindPackage", ScanPatterns::FindPackage, g_findPackage);
        ok &= Resolve("UPackage::FullyLoad", ScanPatterns::UPackage_FullyLoad, g_upackage_FullyLoad);
        ok &= Resolve("LoadPackage", ScanPatterns::LoadPackage, g_loadPackage);
        ok &= Resolve("FAssetData::FastGetAsset", ScanPatterns::FAssetData_FastGetAsset, g_fassetData_FastGetAsset);

        return ok;
    }

    uintptr_t GetStaticFindObject_ByPathAddress()     { return g_staticFindObject_ByPath; }
    uintptr_t GetStaticFindObject_ByNameAddress()     { return g_staticFindObject_ByName; }
    uintptr_t GetStaticFindObjectSafe_ByPathAddress() { return g_staticFindObjectSafe_ByPath; }
    uintptr_t GetStaticFindObjectSafe_ByNameAddress() { return g_staticFindObjectSafe_ByName; }
    uintptr_t GetStaticFindObjectFastAddress()        { return g_staticFindObjectFast; }
    uintptr_t GetFindPackageAddress()                 { return g_findPackage; }
    uintptr_t GetUPackage_FullyLoadAddress()          { return g_upackage_FullyLoad; }
    uintptr_t GetLoadPackageAddress()                 { return g_loadPackage; }
    uintptr_t GetFAssetData_FastGetAssetAddress()     { return g_fassetData_FastGetAsset; }
}
