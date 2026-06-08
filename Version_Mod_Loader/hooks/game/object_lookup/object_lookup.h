#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// Object/Package Lookup Resolution
//
// Resolves (but does not detour) a set of CoreUObject object/package lookup
// and loading functions via AOB scan during early modloader startup, so the
// resolved addresses are ready before plugins start calling into them.
//
// All functions here are native C++ engine internals -- not UFUNCTIONs --
// and must be located via AOB scan and called directly as trampolines.
// Raw addresses are exposed; callers cast to the appropriate function-pointer
// typedef themselves (see object_lookup.cpp for the documented signatures).
// ---------------------------------------------------------------------------

namespace Hooks::ObjectLookup
{
    bool Install();

    // UObject* __fastcall StaticFindObject(UClass* Class, FTopLevelAssetPath* ObjectPath, bool ExactClass)
    uintptr_t GetStaticFindObject_ByPathAddress();

    // UObject* __fastcall StaticFindObject(UClass* ObjectClass, UObject* InObjectPackage, const wchar_t* OrigInName, bool bExactClass)
    uintptr_t GetStaticFindObject_ByNameAddress();

    // UObject* __fastcall StaticFindObjectSafe(UClass* Class, FTopLevelAssetPath* ObjectPath, bool ExactClass)
    uintptr_t GetStaticFindObjectSafe_ByPathAddress();

    // UObject* __fastcall StaticFindObjectSafe(UClass* ObjectClass, UObject* ObjectParent, const wchar_t* InName, bool bExactClass)
    uintptr_t GetStaticFindObjectSafe_ByNameAddress();

    // UObject* __fastcall StaticFindObjectFast(UClass* ObjectClass, UObject* ObjectPackage, FName ObjectName,
    //   bool bExactClass, EObjectFlags ExclusiveFlags, EInternalObjectFlags ExclusiveInternalFlags)
    uintptr_t GetStaticFindObjectFastAddress();

    // UPackage* __fastcall FindPackage(UObject* InOuter, const wchar_t* PackageName)
    uintptr_t GetFindPackageAddress();

    // void __fastcall UPackage::FullyLoad(UPackage* this)
    uintptr_t GetUPackage_FullyLoadAddress();

    // UPackage* __fastcall LoadPackage(UPackage* InOuter, FScriptContainerElement* InLongPackageNameOrFilename,
    //   unsigned int LoadFlags, FArchive* InReaderOverride, const FLinkerInstancingContext* InstancingContext)
    uintptr_t GetLoadPackageAddress();

    // UObject* __fastcall FAssetData::FastGetAsset(FAssetData* this, bool bLoad,
    //   TMap<FName,FName,FDefaultSetAllocator,TDefaultMapHashableKeyFuncs<FName,FName,0>>* LoadTags)
    uintptr_t GetFAssetData_FastGetAssetAddress();
}
