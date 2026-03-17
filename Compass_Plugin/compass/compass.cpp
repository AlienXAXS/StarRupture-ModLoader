#include "compass.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "compass_patterns.h"
#include "Engine_classes.hpp"
#include "ChimeraUI_classes.hpp"
#include "../layout/layout.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace Compass
{
	// ---------------------------------------------------------------------------
	// Hook state
	// ---------------------------------------------------------------------------

	typedef void(__fastcall* PostRender_t)(SDK::AHUD* self);

	static PostRender_t g_originalPostRender = nullptr;
	static HookHandle   g_hookHandle         = nullptr;

	// ---------------------------------------------------------------------------
	// Cardinal direction table
	// ---------------------------------------------------------------------------

	struct Cardinal { const wchar_t* label; float worldYaw; };
	static constexpr Cardinal CARDINALS[] = {
		{ L"N",   0.0f  }, { L"NE",  45.0f }, { L"E",  90.0f  },
		{ L"SE", 135.0f }, { L"S",  180.0f }, { L"SW", 225.0f },
		{ L"W",  270.0f }, { L"NW", 315.0f },
	};

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
		// POI marker types
		SDK::UTexture* antena        = nullptr;
		SDK::UTexture* abandonedBase = nullptr;
		SDK::UTexture* cave          = nullptr;
		SDK::UTexture* obelisk       = nullptr;
		SDK::UTexture* customPin     = nullptr;
	};

	static CompassTextures s_tex;

	// ---------------------------------------------------------------------------
	// StaticLoadObject — resolved via pattern scan at plugin init.
	// Signature matches CoreUObject's free function:
	//   UObject* StaticLoadObject(UClass*, UObject* Outer, const wchar_t* Name,
	//                             const wchar_t* Filename, uint32 LoadFlags,
	//                             UPackageMap*, bool bAllowReconciliation,
	//                             const FLinkerInstancingContext*)
	// ---------------------------------------------------------------------------
	using StaticLoadObject_fn = SDK::UObject* (*)(
		SDK::UClass*,    // Class  (may be nullptr — engine will resolve)
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
	// PinToRoot — equivalent to UObject::AddToRoot() without SDK support.
	//
	// Three layers of protection against GC and streaming eviction:
	//
	// 1. EObjectFlags::MarkAsRootSet (0x80) on UObject::Flags at +0x08 — checked
	//    by some UE5.4 GC builds as an additional root indicator.
	//
	// 2. EInternalObjectFlags::RootSet (1 << 30 = 0x40000000) on FUObjectItem::Flags
	//    at +0x08 of the FUObjectItem — this is what UObject::AddToRoot() actually
	//    sets and is the primary GC root flag checked by FReachabilityAnalysis.
	//    FUObjectItem layout: Object +0x00, Flags +0x08 (Dumper-7 pads this out).
	//
	// 3. UStreamableRenderAsset::NeverStream (bit 0 at +0x00C0) — prevents the
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
					LOG_WARN("[Compass] PinToRoot: FUObjectItem[%d] Object mismatch — skipping itemFlags", idx);
				}
			}
		}

		// Layer 3: NeverStream on UStreamableRenderAsset at +0x00C0
		uint8_t* streamFlags = reinterpret_cast<uint8_t*>(obj) + 0x00C0;
		*streamFlags |= 0x01; // NeverStream
		LOG_DEBUG("[Compass] PinToRoot: NeverStream set (streamFlags=0x%X)", *streamFlags);
	}

	// ---------------------------------------------------------------------------
	// IsValidTexture — checks EObjectFlags on the UObject itself.
	//
	// ProcessEvent (which DrawTexture goes through) calls IsValid() before
	// passing params to the native function.  In UE5.4, IsValid() checks:
	//   RF_BeginDestroyed  (0x00008000) — object is mid-destruction
	//   RF_FinishDestroyed (0x00010000) — object destruction complete
	//   MirroredGarbage    (0x40000000) — UE5.4 GC garbage mirror flag
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
				s_tex.player, s_tex.baseCore, s_tex.body,
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

		LOG_DEBUG("[Compass] Texture resolve: player=%p core=%p body=%p antenna=%p abandonedBase=%p cave=%p obelisk=%p customPin=%p",
			(void*)s_tex.player,   (void*)s_tex.baseCore, (void*)s_tex.body,
			(void*)s_tex.antena,   (void*)s_tex.abandonedBase,
			(void*)s_tex.cave,     (void*)s_tex.obelisk,  (void*)s_tex.customPin);
	}

	static SDK::UTexture* GetPoiTexture(Layout::PoiType type)
	{
		switch (type)
		{
		case Layout::PoiType::Antena:        return s_tex.antena;
		case Layout::PoiType::AbandonedBase: return s_tex.abandonedBase;
		case Layout::PoiType::Cave:          return s_tex.cave;
		case Layout::PoiType::Obelisk:       return s_tex.obelisk;
		default:                             return nullptr;
		}
	}

	// ---------------------------------------------------------------------------
	// Cached draw config — refreshed every ~2 s to avoid per-frame INI reads.
	// IsEnabled() is the only value kept as a direct read (it's checked once per
	// frame in the hook and benefits from immediate response to disable).
	// ---------------------------------------------------------------------------

	struct DrawConfig
	{
		bool     textOnly;
		float    scale;
		float    posY;
		float    widthFraction;
		int      entityScanInterval;
		CompassConfig::EntitySettings  players;
		CompassConfig::EntitySettings  cores;
		CompassConfig::MarkerSettings  markers;
		CompassConfig::EntitySettings  bodies;
		CompassConfig::EntitySettings  customPins;
	};

	static DrawConfig s_cfg = {};
	static int s_cfgTick    = 120; // start at max so first frame refreshes

	static void RefreshConfig()
	{
		s_cfg.textOnly            = CompassConfig::Config::IsTextOnly();
		s_cfg.scale               = CompassConfig::Config::GetScale();
		s_cfg.posY                = CompassConfig::Config::GetPosY();
		s_cfg.widthFraction       = CompassConfig::Config::GetWidthFraction();
		s_cfg.entityScanInterval  = CompassConfig::Config::GetEntityScanInterval();
		s_cfg.players             = CompassConfig::Config::GetPlayers();
		s_cfg.cores               = CompassConfig::Config::GetBaseCores();
		s_cfg.markers             = CompassConfig::Config::GetMarkers();
		s_cfg.bodies              = CompassConfig::Config::GetBodies();
		s_cfg.customPins          = CompassConfig::Config::GetCustomPins();
	}

	// ---------------------------------------------------------------------------
	// Throttled entity cache
	// ---------------------------------------------------------------------------

	static int  s_scanTick = 0;
	static std::vector<Layout::BaseCoreEntry>      s_cores;
	static std::vector<Layout::MarkerEntry>        s_markers; // all visible POIs (incl. caves)
	static std::vector<Layout::BodyEntry>          s_bodies;
	static std::vector<Layout::PlayerMarkerEntry>  s_playerMarkers;
	static std::vector<Layout::CustomPinEntry>     s_customPins;
	static std::string s_lastWorldName; // tracks world changes for log-on-change

	static void RefreshEntities(SDK::UWorld* world)
	{
		LOG_TRACE("[Compass] RefreshEntities: world=%p", (void*)world);

		LOG_TRACE("[Compass] >> ScanBaseCores...");
		try { s_cores = Layout::ScanBaseCores(world); }
		catch (...) { LOG_WARN("[Compass] Exception in ScanBaseCores — cache cleared"); s_cores.clear(); }
		LOG_TRACE("[Compass] >> ScanBaseCores done (%d)", (int)s_cores.size());

		LOG_TRACE("[Compass] >> ScanMarkers...");
		try { s_markers = Layout::ScanMarkers(world); }
		catch (...) { LOG_WARN("[Compass] Exception in ScanMarkers — cache cleared"); s_markers.clear(); }
		LOG_TRACE("[Compass] >> ScanMarkers done (%d)", (int)s_markers.size());

		LOG_TRACE("[Compass] >> ScanBodies...");
		try { s_bodies = Layout::ScanBodies(world); }
		catch (...) { LOG_WARN("[Compass] Exception in ScanBodies — cache cleared"); s_bodies.clear(); }
		LOG_TRACE("[Compass] >> ScanBodies done (%d)", (int)s_bodies.size());

		LOG_TRACE("[Compass] >> ScanPlayerMarkers...");
		try { s_playerMarkers = Layout::ScanPlayerMarkers(world); }
		catch (...) { LOG_WARN("[Compass] Exception in ScanPlayerMarkers — cache cleared"); s_playerMarkers.clear(); }
		LOG_TRACE("[Compass] >> ScanPlayerMarkers done (%d)", (int)s_playerMarkers.size());

		LOG_TRACE("[Compass] >> ScanCustomPins...");
		try { s_customPins = Layout::ScanCustomPins(world); }
		catch (...) { LOG_WARN("[Compass] Exception in ScanCustomPins — cache cleared"); s_customPins.clear(); }
		LOG_TRACE("[Compass] >> ScanCustomPins done (%d)", (int)s_customPins.size());

		// HLOD fallback: appends distant markers/cores not yet in the streaming radius.
		// Deduplicates against already-found real actor entries by proximity.
		LOG_TRACE("[Compass] >> ScanHLOD...");
		try { Layout::ScanHLOD(world, s_markers, s_cores); }
		catch (...) { LOG_WARN("[Compass] Exception in ScanHLOD — ignored"); }
		LOG_TRACE("[Compass] >> ScanHLOD done (markers=%d cores=%d after merge)",
			(int)s_markers.size(), (int)s_cores.size());

		int caveCount = 0;
		for (const auto& m : s_markers)
			if (m.type == Layout::PoiType::Cave) ++caveCount;

		LOG_DEBUG("[Compass] Scan complete: %d players, %d cores, %d POIs (%d caves), %d bodies, %d custompins",
			(int)s_playerMarkers.size(), (int)s_cores.size(),
			(int)s_markers.size(), caveCount, (int)s_bodies.size(), (int)s_customPins.size());
	}

	// ---------------------------------------------------------------------------
	// Compass drawing
	// ---------------------------------------------------------------------------

	static void DrawCompass(SDK::AHUD* hud, SDK::UCanvas* canvas, SDK::UWorld* world)
	{
		// Refresh cached config ~every 2 s (120 frames). Far cheaper than reading
		// the INI file on every frame; still fast enough to feel live when editing.
		if (++s_cfgTick >= 120)
		{
			s_cfgTick = 0;
			RefreshConfig();
		}

		const bool textOnly = s_cfg.textOnly;
		if (!textOnly)
			EnsureTextures(world);

		SDK::APlayerController* pc = hud->GetOwningPlayerController();
		if (!pc)
			return;

		SDK::APawn*  localPawn = pc->K2_GetPawn();
		SDK::FVector playerLoc = localPawn ? localPawn->K2_GetActorLocation() : SDK::FVector{};

		SDK::FRotator controlRot = pc->GetControlRotation();
		// rawYaw: UE-space (0 = East). Used for entity bearing math.
		// yaw:    compass-convention (0 = North). Used for cardinal placement.
		const float rawYaw = static_cast<float>(controlRot.Yaw);
		const float yaw    = rawYaw + 90.0f;

		const float scale     = s_cfg.scale;
		const float posY      = s_cfg.posY;
		const float halfWidth = canvas->SizeX * 0.5f * s_cfg.widthFraction * scale;
		const float centerX   = canvas->SizeX * 0.5f;
		const float left      = centerX - halfWidth;
		const float right     = centerX + halfWidth;

		// One-time config dump to help diagnose sizing/position issues
		static bool s_firstDraw = true;
		if (s_firstDraw)
		{
			s_firstDraw = false;
			LOG_INFO("[Compass] First draw — scale=%.2f posY=%.1f widthFraction=%.2f scanInterval=%d canvas=%dx%d",
				scale, posY, s_cfg.widthFraction, s_cfg.entityScanInterval,
				(int)canvas->SizeX, (int)canvas->SizeY);
		}

		SDK::UFont* labelFont  = SDK::UEngine::GetEngine() ? SDK::UEngine::GetEngine()->LargeFont : nullptr;
		SDK::UFont* entityFont = labelFont; // same font as cardinals for bigger entity text

		// --- Throttled entity scan ---
		if (++s_scanTick >= s_cfg.entityScanInterval)
		{
			s_scanTick = 0;
			RefreshEntities(world);
		}

		// --- Helper: world position → compass screenX ---
		auto ToScreenX = [&](const SDK::FVector& pos) -> float {
			const float dx = pos.X - playerLoc.X;
			const float dy = pos.Y - playerLoc.Y;
			float delta = atan2f(dy, dx) * (180.0f / 3.14159265358979f) - rawYaw;
			while (delta >  180.0f) delta -= 360.0f;
			while (delta < -180.0f) delta += 360.0f;
			return centerX + (delta / 90.0f) * halfWidth;
		};

		auto InBounds = [&](float x) { return x >= left && x <= right; };

		// --- Colors ---
		static constexpr SDK::FLinearColor white    { 1.0f, 1.0f, 1.0f, 0.9f };
		static constexpr SDK::FLinearColor dimWhite { 1.0f, 1.0f, 1.0f, 0.4f };
		static constexpr SDK::FLinearColor yellow   { 1.0f, 0.85f, 0.0f, 1.0f };
		static constexpr SDK::FLinearColor colPlayer{ 0.3f, 0.8f,  1.0f, 1.0f }; // cyan
		static constexpr SDK::FLinearColor colCore  { 1.0f, 0.5f,  0.0f, 1.0f }; // orange
		static constexpr SDK::FLinearColor colMarker{ 1.0f, 1.0f,  0.0f, 1.0f }; // yellow
		static constexpr SDK::FLinearColor colBody  { 0.7f, 0.7f,  0.7f, 1.0f }; // grey
		static constexpr SDK::FLinearColor black1   { 0.0f, 0.0f,  0.0f, 1.0f };

		// --- Horizontal compass line ---
		hud->DrawLine(left, posY, right, posY, dimWhite, 1.5f * scale);

		// --- Cardinal ticks + labels (above the line) ---
		for (const auto& c : CARDINALS)
		{
			float delta = c.worldYaw - yaw;
			while (delta >  180.0f) delta -= 360.0f;
			while (delta < -180.0f) delta += 360.0f;

			const float screenX = centerX + (delta / 90.0f) * halfWidth;
			if (!InBounds(screenX)) continue;

			// Tick crossing the line
			hud->DrawLine(screenX, posY - 6.0f * scale, screenX, posY + 4.0f * scale, white, 1.0f * scale);

			// Label above the tick
			SDK::FString   label(c.label);
			SDK::FVector2D sz = canvas->K2_TextSize(labelFont, label, { scale, scale });
			canvas->K2_DrawText(labelFont, label,
				{ screenX - sz.X * 0.5f, posY - 9.0f * scale - sz.Y },
				{ scale, scale }, white, 0.0f,
				{ 0.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f },
				false, false, true, black1);
		}

		// --- Center notch (current heading, above the line) ---
		hud->DrawLine(centerX, posY - 22.0f * scale, centerX, posY + 6.0f * scale, yellow, 2.5f * scale);

		// --- Config: per-entity settings (from cache) ---
		const auto& cfgPlayers    = s_cfg.players;
		const auto& cfgCores      = s_cfg.cores;
		const auto& cfgMarkers    = s_cfg.markers;
		const auto& cfgBodies     = s_cfg.bodies;
		const auto& cfgCustomPins = s_cfg.customPins;

		// --- Helper: distance-based alpha (fade starts at 80% of max) ---
		auto DistAlpha = [](float dist, float maxDist) -> float {
			if (maxDist <= 0.0f) return 1.0f;
			const float fadeStart = maxDist * 0.8f;
			if (dist <= fadeStart) return 1.0f;
			if (dist >= maxDist)   return 0.0f;
			return (maxDist - dist) / (maxDist - fadeStart);
		};

		// --- Entity markers (below the line) ---

		// Shared edge-fade calculation
		auto EdgeAlpha = [&](float screenX) -> float {
			const float edgeFadeZone = halfWidth * 0.12f;
			return fminf(
				fminf((screenX - left)  / edgeFadeZone, 1.0f),
				fminf((right - screenX) / edgeFadeZone, 1.0f)
			);
		};

		// Draw icon texture (with text fallback). White tint preserves art colours.
		auto DrawEntityIcon = [&](float screenX, SDK::UTexture* tex,
			const wchar_t* fallbackSym, SDK::FLinearColor colour, float alpha)
		{
			if (!InBounds(screenX)) return;
			const float finalAlpha = alpha * EdgeAlpha(screenX);
			if (finalAlpha <= 0.0f) return;

			hud->DrawLine(screenX, posY + 4.0f * scale, screenX, posY + 10.0f * scale, colour, 1.5f * scale);

			if (!textOnly && tex && IsValidTexture(tex))
			{
				const float iconSize = 22.0f * scale;
				SDK::FLinearColor tint{ 1.0f, 1.0f, 1.0f, finalAlpha };
				hud->DrawTexture(tex,
					screenX - iconSize * 0.5f, posY + 10.0f * scale, iconSize, iconSize,
					0.0f, 0.0f, 1.0f, 1.0f,
					tint, SDK::EBlendMode::BLEND_Translucent,
					1.0f, false, 0.0f, { 0.5f, 0.5f });
			}
			else
			{
				colour.A *= finalAlpha;
				const float es = scale * 1.25f;
				SDK::FString   s(fallbackSym);
				SDK::FVector2D sz = canvas->K2_TextSize(entityFont, s, { es, es });
				SDK::FLinearColor outline{ 0.0f, 0.0f, 0.0f, finalAlpha };
				canvas->K2_DrawText(entityFont, s,
					{ screenX - sz.X * 0.5f, posY + 16.0f * scale },
					{ es, es }, colour, 0.0f,
					{ 0.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f },
					false, false, true, outline);
			}
		};

		// Draw icon + name label below (used for base cores which have custom names).
		auto DrawEntityIconWithLabel = [&](float screenX, SDK::UTexture* tex,
			const wchar_t* label, SDK::FLinearColor colour, float alpha)
		{
			if (!InBounds(screenX)) return;
			const float finalAlpha = alpha * EdgeAlpha(screenX);
			if (finalAlpha <= 0.0f) return;

			hud->DrawLine(screenX, posY + 4.0f * scale, screenX, posY + 10.0f * scale, colour, 1.5f * scale);

			const float iconSize = 22.0f * scale;

			const bool showIcon = !textOnly && tex && IsValidTexture(tex);
			if (showIcon)
			{
				SDK::FLinearColor tint{ 1.0f, 1.0f, 1.0f, finalAlpha };
				hud->DrawTexture(tex,
					screenX - iconSize * 0.5f, posY + 10.0f * scale, iconSize, iconSize,
					0.0f, 0.0f, 1.0f, 1.0f,
					tint, SDK::EBlendMode::BLEND_Translucent,
					1.0f, false, 0.0f, { 0.5f, 0.5f });
			}

			// Name label below the icon (or below stem if no icon)
			colour.A *= finalAlpha;
			const float es = scale * 1.0f;
			SDK::FString   s(label);
			SDK::FVector2D sz = canvas->K2_TextSize(entityFont, s, { es, es });
			SDK::FLinearColor outline{ 0.0f, 0.0f, 0.0f, finalAlpha };
			const float labelY = posY + 10.0f * scale + (showIcon ? iconSize : 6.0f * scale);
			canvas->K2_DrawText(entityFont, s,
				{ screenX - sz.X * 0.5f, labelY },
				{ es, es }, colour, 0.0f,
				{ 0.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f },
				false, false, true, outline);
		};

		// --- POI helpers (used when building the draw queue below) ---
		auto PoiTypeEnabled = [&](Layout::PoiType t) -> bool {
			switch (t)
			{
			case Layout::PoiType::Antena:        return cfgMarkers.showAntena;
			case Layout::PoiType::AbandonedBase: return cfgMarkers.showAbandonedBase;
			case Layout::PoiType::Cave:          return cfgMarkers.showCave;
			case Layout::PoiType::Obelisk:       return cfgMarkers.showObelisk;
			default:                             return false;
			}
		};
		auto PoiSymbol = [](Layout::PoiType t) -> const wchar_t* {
			switch (t)
			{
			case Layout::PoiType::Antena:        return L"A";
			case Layout::PoiType::AbandonedBase: return L"Ab";
			case Layout::PoiType::Cave:          return L"C";
			case Layout::PoiType::Obelisk:       return L"Ob";
			default:                             return L"?";
			}
		};
		auto PoiDistance = [&](Layout::PoiType t) -> float {
			switch (t)
			{
			case Layout::PoiType::Antena:        return cfgMarkers.antenaDistance;
			case Layout::PoiType::AbandonedBase: return cfgMarkers.abandonedBaseDistance;
			case Layout::PoiType::Cave:          return cfgMarkers.caveDistance;
			case Layout::PoiType::Obelisk:       return cfgMarkers.obeliskDistance;
			default:                             return 0.0f;
			}
		};

		// ---------------------------------------------------------------------------
		// Unified draw queue — all visible entities sorted by distance descending
		// so that the closest entity is always drawn last (rendered on top).
		// ---------------------------------------------------------------------------
		struct DrawCall { float distSq; std::function<void()> fn; };
		std::vector<DrawCall> drawQueue;
		drawQueue.reserve(
			s_playerMarkers.size() + s_cores.size() +
			s_markers.size() + s_bodies.size() + s_customPins.size());

		// Players — sourced from PlayersMarkerDataContainer (map subsystem), not actor scan.
		// Covers all online players regardless of streaming range.
		if (cfgPlayers.enabled)
		{
			for (const auto& p : s_playerMarkers)
			{
				const float dx = p.location.X - playerLoc.X, dy = p.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), cfgPlayers.distance);
				if (alpha <= 0.0f) continue;
				const float sx = ToScreenX(p.location);
				std::wstring wn(p.playerName.begin(), p.playerName.end());
				drawQueue.push_back({ distSq, [=]{ DrawEntityIconWithLabel(sx, s_tex.player, wn.c_str(), colPlayer, alpha); } });
			}
		}
		if (cfgCores.enabled)
		{
			for (const auto& c : s_cores)
			{
				const float dx = c.location.X - playerLoc.X, dy = c.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), cfgCores.distance);
				if (alpha <= 0.0f) continue;
				const float sx = ToScreenX(c.location);
				std::wstring wn(c.name.begin(), c.name.end());
				drawQueue.push_back({ distSq, [=]{ DrawEntityIconWithLabel(sx, s_tex.baseCore, wn.c_str(), colCore, alpha); } });
			}
		}
		// ForgottenEngine and OrbitalLander are filtered out in ScanMarkers.
		if (cfgMarkers.enabled)
		{
			for (const auto& m : s_markers)
			{
				if (!PoiTypeEnabled(m.type)) continue;
				const float dx = m.location.X - playerLoc.X, dy = m.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), PoiDistance(m.type));
				if (alpha <= 0.0f) continue;
				const float sx = ToScreenX(m.location);
				SDK::UTexture* tex = GetPoiTexture(m.type);
				const wchar_t* sym = PoiSymbol(m.type);
				drawQueue.push_back({ distSq, [=]{ DrawEntityIcon(sx, tex, sym, colMarker, alpha); } });
			}
		}
		if (cfgBodies.enabled)
		{
			for (const auto& b : s_bodies)
			{
				const float dx = b.location.X - playerLoc.X, dy = b.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), cfgBodies.distance);
				if (alpha <= 0.0f) continue;
				const float sx = ToScreenX(b.location);
				drawQueue.push_back({ distSq, [=]{ DrawEntityIcon(sx, s_tex.body, L"D", colBody, alpha); } });
			}
		}
		// --- Personal map pins (ACrGameStateBase::PlayerPersonalMarkers) ---
		if (cfgCustomPins.enabled)
		{
			for (const auto& pin : s_customPins)
			{
				const float dx = pin.location.X - playerLoc.X, dy = pin.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), cfgCustomPins.distance);
				if (alpha <= 0.0f) continue;
				const float sx = ToScreenX(pin.location);
				const SDK::FLinearColor col = {
					pin.color.R, pin.color.G, pin.color.B,
					pin.color.A > 0.0f ? pin.color.A : 1.0f
				};
				std::wstring label = pin.playerName.empty()
					? L"Pin"
					: std::wstring(pin.playerName.begin(), pin.playerName.end());
				drawQueue.push_back({ distSq, [=]{ DrawEntityIconWithLabel(sx, s_tex.customPin, label.c_str(), col, alpha); } });
			}
		}

		// Sort furthest-first so closest entities paint on top.
		std::sort(drawQueue.begin(), drawQueue.end(),
			[](const DrawCall& a, const DrawCall& b) { return a.distSq > b.distSq; });

		for (const auto& dc : drawQueue)
			dc.fn();
	}

	// ---------------------------------------------------------------------------
	// Detour
	// ---------------------------------------------------------------------------

	static void __fastcall Hooked_PostRender(SDK::AHUD* self)
	{
		std::string worldName;
		SDK::UWorld* world;

		if (g_originalPostRender)
			g_originalPostRender(self);

		if (!self || !self->Canvas)
			return;

		if (!CompassConfig::Config::IsEnabled())
			return;

		try {
			world = SDK::UWorld::GetWorld();
			if (!world)
			{
				LOG_TRACE("[Compass] PostRender: no world");
				return;
			}
		} catch (...) {
			LOG_ERROR("[Compass] Exception in GetWorld — skipping compass draw");
			return;
		}

		try {
			worldName = world->GetName();
			if (worldName != s_lastWorldName)
			{
				LOG_INFO("[Compass] World changed: '%s'", worldName.c_str());
				s_lastWorldName = worldName;
			}
		} catch (...) {
			LOG_ERROR("[Compass] Exception in GetName");
			return;
		}

		if (worldName != "ChimeraMain")
			return;

		try { DrawCompass(self, self->Canvas, world); }
		catch (...) { LOG_ERROR("[Compass] Exception in DrawCompass — suppressed to avoid crash loop"); }
	}

	// ---------------------------------------------------------------------------
	// Install / Remove
	// ---------------------------------------------------------------------------

	bool Install(IPluginScanner* scanner, IPluginHooks* hooks)
	{
		if (!scanner || !hooks || !hooks->Hooks)
		{
			LOG_ERROR("[Compass] Install called with null interfaces");
			return false;
		}

		uintptr_t addr = scanner->FindPatternInMainModule(CompassPatterns::AHUD_PostRender);
		if (!addr)
		{
			LOG_ERROR("[Compass] AHUD::PostRender pattern not found in main module");
			return false;
		}

		g_hookHandle = hooks->Hooks->Install(addr, (void*)Hooked_PostRender, (void**)&g_originalPostRender);
		if (!g_hookHandle)
		{
			LOG_ERROR("[Compass] Failed to install hook on AHUD::PostRender at 0x%llX", (unsigned long long)addr);
			return false;
		}

		LOG_INFO("[Compass] Hooked AHUD::PostRender at 0x%llX", (unsigned long long)addr);
		return true;
	}

	void Remove(IPluginHooks* hooks)
	{
		if (g_hookHandle && hooks && hooks->Hooks)
		{
			hooks->Hooks->Remove(g_hookHandle);
			g_hookHandle = nullptr;
			LOG_INFO("[Compass] PostRender hook removed");
		}

		g_originalPostRender = nullptr;
	}
}
