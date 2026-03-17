#pragma once

#include "Engine_classes.hpp"
#include "plugin_helpers.h"

// ---------------------------------------------------------------------------
// compass_textures.h — texture cache, GC pinning, and asset loading.
//
// Intended to be included only from compass.cpp.
// All symbols are in namespace Compass and have internal linkage (static).
// ---------------------------------------------------------------------------

namespace Compass
{

// ---------------------------------------------------------------------------
// POI icon texture cache
// Textures are resolved once from GObjects via UObject::FindObject and then
// held as raw pointers. They remain valid for the lifetime of the process
// because the game never unloads these map-marker assets at runtime.
// ---------------------------------------------------------------------------

struct CompassTextures
{
	// Entity types
	SDK::UTexture* player        = nullptr;
	SDK::UTexture* baseCore      = nullptr;
	SDK::UTexture* body          = nullptr;
	SDK::UTexture* drone         = nullptr;
	// POI marker types
	SDK::UTexture* antena        = nullptr;
	SDK::UTexture* abandonedBase = nullptr;
	SDK::UTexture* cave          = nullptr;
	SDK::UTexture* obelisk       = nullptr;
	SDK::UTexture* customPin     = nullptr;
};

static CompassTextures s_tex;

// ---------------------------------------------------------------------------
// StaticLoadObject -- resolved via pattern scan at plugin init.
// Signature matches CoreUObject's free function:
//   UObject* StaticLoadObject(UClass*, UObject* Outer, const wchar_t* Name,
//                             const wchar_t* Filename, uint32 LoadFlags,
//                             UPackageMap*, bool bAllowReconciliation,
//                             const FLinkerInstancingContext*)
// ---------------------------------------------------------------------------
using StaticLoadObject_fn = SDK::UObject* (*)(
	SDK::UClass*,    // Class  (may be nullptr -- engine will resolve)
	SDK::UObject*,   // Outer  (nullptr = global)
	const wchar_t*,  // Name   (full asset path)
	const wchar_t*,  // Filename (nullptr)
	uint32_t,        // LoadFlags (0 = LOAD_None)
	void*,           // Sandbox UPackageMap* (nullptr)
	bool,            // bAllowObjectReconciliation
	const void*      // FLinkerInstancingContext* (nullptr)
);

static StaticLoadObject_fn g_StaticLoadObject = nullptr;

void SetStaticLoadObject(uintptr_t addr)
{
	g_StaticLoadObject = reinterpret_cast<StaticLoadObject_fn>(addr);
	LOG_INFO("[Compass] StaticLoadObject registered at 0x%llX", (unsigned long long)addr);
}

// ---------------------------------------------------------------------------
// PinToRoot -- equivalent to UObject::AddToRoot() without SDK support.
//
// Three layers of protection against GC and streaming eviction:
//
// 1. EObjectFlags::MarkAsRootSet (0x80) on UObject::Flags at +0x08 -- checked
//    by some UE5.4 GC builds as an additional root indicator.
//
// 2. EInternalObjectFlags::RootSet (1 << 30 = 0x40000000) on FUObjectItem::Flags
//    at +0x08 of the FUObjectItem -- this is what UObject::AddToRoot() actually
//    sets and is the primary GC root flag checked by FReachabilityAnalysis.
//    FUObjectItem layout: Object +0x00, Flags +0x08 (Dumper-7 pads this out).
//
// 3. UStreamableRenderAsset::NeverStream (bit 0 at +0x00C0) -- prevents the
//    texture streaming system from evicting the GPU resource independently of GC.
// ---------------------------------------------------------------------------
static void PinToRoot(SDK::UObject* obj)
{
	if (!obj) return;

	// Layer 1: EObjectFlags::MarkAsRootSet on the UObject itself
	int32_t* objFlags = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + 0x08);
	const int32_t before = *objFlags;
	*objFlags |= 0x00000080; // RF_MarkAsRootSet
	LOG_DEBUG("[Compass] PinToRoot: %p objFlags 0x%X -> 0x%X", (void*)obj, before, *objFlags);

	// Layer 2: EInternalObjectFlags::RootSet on the FUObjectItem
	// Read InternalIndex from UObject at +0x0C, then locate the FUObjectItem.
	const int32_t idx = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(obj) + 0x0C);
	auto* arr = SDK::UObject::GObjects.GetTypedPtr();
	if (arr && idx >= 0 && idx < arr->NumElements)
	{
		const int32_t chunkIdx   = idx / SDK::TUObjectArray::ElementsPerChunk;
		const int32_t inChunkIdx = idx % SDK::TUObjectArray::ElementsPerChunk;
		auto* decryptedObjs = arr->GetDecrytedObjPtr();
		if (decryptedObjs && decryptedObjs[chunkIdx])
		{
			SDK::FUObjectItem* item = &decryptedObjs[chunkIdx][inChunkIdx];
			if (item->Object == obj)
			{
				// Flags field is at +0x08 of FUObjectItem (hidden as Pad_8 in SDK)
				int32_t* itemFlags = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(item) + 0x08);
				const int32_t beforeItem = *itemFlags;
				*itemFlags |= 0x40000000; // EInternalObjectFlags::RootSet = 1 << 30
				LOG_DEBUG("[Compass] PinToRoot: FUObjectItem[%d] itemFlags 0x%X -> 0x%X", idx, beforeItem, *itemFlags);
			}
			else
			{
				LOG_WARN("[Compass] PinToRoot: FUObjectItem[%d] Object mismatch -- skipping itemFlags", idx);
			}
		}
	}

	// Layer 3: NeverStream on UStreamableRenderAsset at +0x00C0
	uint8_t* streamFlags = reinterpret_cast<uint8_t*>(obj) + 0x00C0;
	*streamFlags |= 0x01; // NeverStream
	LOG_DEBUG("[Compass] PinToRoot: NeverStream set (streamFlags=0x%X)", *streamFlags);
}

// ---------------------------------------------------------------------------
// IsValidTexture -- checks EObjectFlags on the UObject itself.
//
// ProcessEvent (which DrawTexture goes through) calls IsValid() before
// passing params to the native function.  In UE5.4, IsValid() checks:
//   RF_BeginDestroyed  (0x00008000) -- object is mid-destruction
//   RF_FinishDestroyed (0x00010000) -- object destruction complete
//   MirroredGarbage    (0x40000000) -- UE5.4 GC garbage mirror flag
//
// If any of these are set, IsValid() returns false, ProcessEvent nullifies
// the texture pointer, and FCanvasTileItem asserts InTexture != nullptr.
//
// Flag values are taken directly from EObjectFlags in this game's SDK.
// ---------------------------------------------------------------------------
static bool IsValidTexture(SDK::UTexture* tex)
{
	if (!tex) return false;
	const uint8_t* base = reinterpret_cast<const uint8_t*>(tex);

	// EObjectFlags at +0x08: check for destruction / garbage states
	static constexpr int32_t RF_BeginDestroyed  = 0x00008000;
	static constexpr int32_t RF_FinishDestroyed = 0x00010000;
	static constexpr int32_t RF_MirroredGarbage = 0x40000000;
	const int32_t objFlags = *reinterpret_cast<const int32_t*>(base + 0x08);
	if (objFlags & (RF_BeginDestroyed | RF_FinishDestroyed | RF_MirroredGarbage))
		return false;

	// UTexture::bAsyncResourceReleaseHasBeenStarted at +0x010C, bit 2.
	// Set when the GPU resource is being released asynchronously (mid-eviction).
	// Drawing with an in-flight resource release crashes the render thread.
	const uint8_t texFlags = *(base + 0x010C);
	if (texFlags & 0x04) // bit 2
		return false;

	return true;
}

// ---------------------------------------------------------------------------
// Texture cache -- resolved once (or re-resolved if invalidated).
// When g_StaticLoadObject is available it force-loads the asset from the
// package cache so the player never needs to open the map first.
// Throttled to one attempt per second to avoid per-frame overhead.
//
// GC protection strategy:
//   Primary  -- EngineArrayAdd() pushes each texture into
//               UWorld::ExtraReferencedObjects using the engine's own
//               FMemory allocator (via hooks->Memory->Alloc/Free).
//               The GC traverses this UPROPERTY TArray as a live reference,
//               keeping textures alive for as long as the world lives.
//   Fallback -- PinToRoot() sets EInternalObjectFlags::RootSet +
//               RF_MarkAsRootSet + NeverStream; guards the gap when the
//               world pointer is null during a level transition.
// ---------------------------------------------------------------------------

// Add obj into a game-owned TArray<UObject*> using FMemory so the engine heap
// owns the buffer. The SDK's TArray::Add() uses the DLL's CRT allocator which
// the game never sees; this helper bypasses the template entirely.
// Returns true if added, false if already present or allocator not ready.
static bool EngineArrayAdd(SDK::TArray<SDK::UObject*>& arr, SDK::UObject* obj)
{
	for (int32_t i = 0; i < arr.Num(); ++i)
		if (arr[i] == obj) return false; // duplicate guard

	auto* mem = GetHooks() ? GetHooks()->Memory : nullptr;
	if (!mem || !mem->IsAllocatorAvailable()) return false;

	// Standard UE5 TArray x64 layout: Data@+0x00, Num@+0x08, Max@+0x0C
	SDK::UObject*** dataField = reinterpret_cast<SDK::UObject***>(&arr);
	int32_t*        numField  = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&arr) + 0x08);
	int32_t*        maxField  = reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&arr) + 0x0C);

	if (*numField >= *maxField)
	{
		int32_t newMax    = (*maxField == 0) ? 4 : (*maxField * 2);
		auto*   newData   = static_cast<SDK::UObject**>(
			mem->Alloc(static_cast<size_t>(newMax) * sizeof(SDK::UObject*), 8u));
		if (!newData) return false;

		if (*dataField && *numField > 0)
			memcpy(newData, *dataField, static_cast<size_t>(*numField) * sizeof(SDK::UObject*));
		if (*dataField)
			mem->Free(*dataField);

		*dataField = newData;
		*maxField  = newMax;
	}

	(*dataField)[(*numField)++] = obj;
	return true;
}

static SDK::UWorld* s_lastPinnedWorld = nullptr;

static void AnchorInWorld(SDK::UWorld* world, SDK::UObject* obj)
{
	if (!world || !obj) return;
	if (EngineArrayAdd(world->ExtraReferencedObjects, obj))
		LOG_DEBUG("[Compass] AnchorInWorld: texture %p anchored in world %p (count now %d)",
			(void*)obj, (void*)world, world->ExtraReferencedObjects.Num());
}

static void EnsureTextures(SDK::UWorld* world)
{
	// Without StaticLoadObject there is no way to load textures -- use text fallback.
	if (!g_StaticLoadObject) return;

	// On world change: re-anchor all loaded textures in the new world's reference list.
	if (world && world != s_lastPinnedWorld)
	{
		s_lastPinnedWorld = world;
		SDK::UObject* slots[] = {
			s_tex.player, s_tex.baseCore, s_tex.body, s_tex.drone,
			s_tex.antena, s_tex.abandonedBase, s_tex.cave,
			s_tex.obelisk, s_tex.customPin,
		};
		int reanchored = 0;
		for (auto* obj : slots)
			if (obj) { AnchorInWorld(world, obj); ++reanchored; }
		LOG_DEBUG("[Compass] World changed -- re-anchored %d textures in ExtraReferencedObjects", reanchored);
	}

	struct TexEntry { SDK::UTexture*& slot; const wchar_t* fullPath; };
	TexEntry entries[] = {
		{ s_tex.player,        L"/Game/Chimera/UI/Map/Markers/T_UI_player_mapIcon.T_UI_player_mapIcon"               },
		{ s_tex.baseCore,      L"/Game/Chimera/UI/Map/Markers/T_UI_baseCore_mapIcon.T_UI_baseCore_mapIcon"           },
		{ s_tex.body,          L"/Game/Chimera/UI/Map/Markers/T_UI_deadBody_mapIcon.T_UI_deadBody_mapIcon"           },
		{ s_tex.drone,         L"/Game/Chimera/UI/Map/Markers/T_UI_drone_mapIcon.T_UI_drone_mapIcon"                 },
		{ s_tex.antena,        L"/Game/Chimera/UI/Map/Markers/T_UI_antenna_mapIcon.T_UI_antenna_mapIcon"             },
		{ s_tex.abandonedBase, L"/Game/Chimera/UI/Map/Markers/T_UI_abandonedBase_mapIcon.T_UI_abandonedBase_mapIcon" },
		{ s_tex.cave,          L"/Game/Chimera/UI/Map/Markers/T_UI_cave_mapIcon.T_UI_cave_mapIcon"                   },
		{ s_tex.obelisk,       L"/Game/Chimera/UI/Map/Markers/T_UI_obelisk_mapIcon.T_UI_obelisk_mapIcon"             },
		{ s_tex.customPin,     L"/Game/Chimera/UI/Map/Markers/T_UI_marker_mapIcon.T_UI_marker_mapIcon"               },
	};

	// Per-frame pass: if a previously-loaded slot is now invalid, reload it immediately
	// so there's no visible gap this frame. StaticLoadObject is cheap when the asset
	// is still resident in the package cache.
	bool anyNull = false;
	for (auto& e : entries)
	{
		if (!e.slot) { anyNull = true; continue; }
		if (IsValidTexture(e.slot)) continue;

		LOG_WARN("[Compass] Texture %p invalidated -- reloading immediately", (void*)e.slot);
		e.slot = nullptr;
		auto* obj = g_StaticLoadObject(nullptr, nullptr, e.fullPath, nullptr, 0, nullptr, true, nullptr);
		if (obj)
		{
			e.slot = static_cast<SDK::UTexture*>(obj);
			PinToRoot(obj);
			AnchorInWorld(world, obj);
		}
		if (!e.slot) anyNull = true;
	}

	// Throttled pass: initial load of slots that have never been loaded (or whose
	// immediate reload above failed). ~1 attempt per second at 60 fps.
	if (!anyNull) return;

	static int s_retryTick = 60; // start at 60 so first attempt fires immediately
	if (++s_retryTick < 60) return;
	s_retryTick = 0;

	for (auto& e : entries)
	{
		if (e.slot) continue;
		auto* obj = g_StaticLoadObject(nullptr, nullptr, e.fullPath, nullptr, 0, nullptr, true, nullptr);
		if (obj)
		{
			e.slot = static_cast<SDK::UTexture*>(obj);
			PinToRoot(obj);
			AnchorInWorld(world, obj);
		}
	}

	LOG_DEBUG("[Compass] Texture resolve: player=%p core=%p body=%p drone=%p antenna=%p abandonedBase=%p cave=%p obelisk=%p customPin=%p",
		(void*)s_tex.player,   (void*)s_tex.baseCore, (void*)s_tex.body,   (void*)s_tex.drone,
		(void*)s_tex.antena,   (void*)s_tex.abandonedBase,
		(void*)s_tex.cave,     (void*)s_tex.obelisk,  (void*)s_tex.customPin);
}

} // namespace Compass
