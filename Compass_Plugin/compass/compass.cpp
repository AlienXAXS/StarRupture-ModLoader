#include "compass.h"
#include "compass_textures.h"
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

	using PostRender_t = void(__fastcall*)(SDK::AHUD* self);

	static PostRender_t g_originalPostRender = nullptr;
	static HookHandle g_hookHandle = nullptr;

	// ---------------------------------------------------------------------------
	// Cardinal direction table
	// ---------------------------------------------------------------------------

	struct Cardinal
	{
		const wchar_t* label;
		float worldYaw;
	};

	static constexpr Cardinal CARDINALS[] = {
		{L"N", 0.0f}, {L"NE", 45.0f}, {L"E", 90.0f},
		{L"SE", 135.0f}, {L"S", 180.0f}, {L"SW", 225.0f},
		{L"W", 270.0f}, {L"NW", 315.0f},
	};

	static SDK::UTexture* GetPoiTexture(Layout::PoiType type)
	{
		switch (type)
		{
		case Layout::PoiType::Antena: return s_tex.antena;
		case Layout::PoiType::AbandonedBase: return s_tex.abandonedBase;
		case Layout::PoiType::Cave: return s_tex.cave;
		case Layout::PoiType::Obelisk: return s_tex.obelisk;
		default: return nullptr;
		}
	}

	// ---------------------------------------------------------------------------
	// Cached draw config -- refreshed every ~2 s to avoid per-frame INI reads.
	// IsEnabled() is the only value kept as a direct read (it's checked once per
	// frame in the hook and benefits from immediate response to disable).
	// ---------------------------------------------------------------------------

	struct DrawConfig
	{
		bool textOnly;
		float scale;
		float posY;
		float widthFraction;
		int entityScanInterval;
		int playerScanInterval;
		SDK::FLinearColor lineColor;
		CompassConfig::EntitySettings players;
		CompassConfig::EntitySettings cores;
		CompassConfig::MarkerSettings markers;
		CompassConfig::FoundableSettings foundables;
		CompassConfig::EntitySettings enemies;
		CompassConfig::EntitySettings customPins;
	};

	static DrawConfig s_cfg = {};
	static int s_cfgTick = 120; // start at max so first frame refreshes

	static void RefreshConfig()
	{
		s_cfg.textOnly = CompassConfig::Config::IsTextOnly();
		s_cfg.scale = CompassConfig::Config::GetScale();
		s_cfg.posY = CompassConfig::Config::GetPosY();
		s_cfg.widthFraction = CompassConfig::Config::GetWidthFraction();
		s_cfg.entityScanInterval = CompassConfig::Config::GetEntityScanInterval();
		s_cfg.playerScanInterval = CompassConfig::Config::GetPlayerScanInterval();
		CompassConfig::Config::GetLineColor(
			s_cfg.lineColor.R, s_cfg.lineColor.G, s_cfg.lineColor.B, s_cfg.lineColor.A);
		s_cfg.players = CompassConfig::Config::GetPlayers();
		s_cfg.cores = CompassConfig::Config::GetBaseCores();
		s_cfg.markers = CompassConfig::Config::GetMarkers();
		s_cfg.foundables = CompassConfig::Config::GetFoundables();
		s_cfg.enemies = CompassConfig::Config::GetEnemies();
		s_cfg.customPins = CompassConfig::Config::GetCustomPins();
	}

	// ---------------------------------------------------------------------------
	// Throttled entity cache
	// ---------------------------------------------------------------------------

	static int s_scanTick = 0;
	static int s_playerScanTick = 0;

	// ---------------------------------------------------------------------------
	// GatherPlayersData -- direct call to UCrMapManuSubsystem::GatherPlayersData
	// ---------------------------------------------------------------------------
	// Calling this before ScanPlayerMarkers forces the subsystem to refresh its
	// PlayersMarkerDataContainer immediately, rather than waiting for the game to
	// set the flag at localPC+3816 and process it on the next gameplay tick.
	// ---------------------------------------------------------------------------

	using GatherPlayersData_t = void(__fastcall*)(void* thisSubsystem);
	static GatherPlayersData_t g_gatherPlayersDataFn = nullptr;
	static void* g_mapManuSubsystem = nullptr;
	static SDK::UWorld* g_mapManuWorld = nullptr;

	static bool CallGatherPlayersData(GatherPlayersData_t fn, void* subsystem)
	{
		__try
		{
			fn(subsystem);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	static void* FindMapManuSubsystem(SDK::UWorld* world)
	{
		SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
		if (!arr) return nullptr;

		try
		{
			for (int32_t i = 0; i < arr->NumElements; ++i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || !obj->Class) continue;
				if (obj->Outer != static_cast<SDK::UObject*>(world)) continue;

				std::string className = obj->Class->GetName();
				if (className == "CrMapManuSubsystem")
					return obj;
			}
		}
		catch (...)
		{
		}

		return nullptr;
	}

	static std::vector<Layout::BaseCoreEntry> s_cores;
	static std::vector<Layout::MarkerEntry> s_markers; // all visible POIs (incl. caves)
	static std::vector<Layout::FoundableEntry> s_foundables;
	static std::vector<Layout::EnemyEntry> s_enemies;
	static std::vector<Layout::PlayerMarkerEntry> s_playerMarkers;
	static std::vector<Layout::CustomPinEntry> s_customPins;
	static std::string s_lastWorldName; // tracks world changes for log-on-change

	static void RefreshEntities(SDK::UWorld* world)
	{
		LOG_TRACE("[Compass] RefreshEntities: world=%p", static_cast<void*>(world));

		LOG_TRACE("[Compass] >> ScanBaseCores...");
		try { s_cores = Layout::ScanBaseCores(world); }
		catch (...)
		{
			LOG_WARN("[Compass] Exception in ScanBaseCores -- cache cleared");
			s_cores.clear();
		}
		LOG_TRACE("[Compass] >> ScanBaseCores done (%d)", static_cast<int>(s_cores.size()));

		LOG_TRACE("[Compass] >> ScanMarkers...");
		try { s_markers = Layout::ScanMarkers(world); }
		catch (...)
		{
			LOG_WARN("[Compass] Exception in ScanMarkers -- cache cleared");
			s_markers.clear();
		}
		LOG_TRACE("[Compass] >> ScanMarkers done (%d)", static_cast<int>(s_markers.size()));

		LOG_TRACE("[Compass] >> ScanFoundables...");
		try { s_foundables = Layout::ScanFoundables(world); }
		catch (...)
		{
			LOG_WARN("[Compass] Exception in ScanFoundables -- cache cleared");
			s_foundables.clear();
		}
		LOG_TRACE("[Compass] >> ScanFoundables done (%d)", static_cast<int>(s_foundables.size()));

		LOG_TRACE("[Compass] >> ScanEnemies...");
		try { s_enemies = Layout::ScanEnemies(world); }
		catch (...)
		{
			LOG_WARN("[Compass] Exception in ScanEnemies -- cache cleared");
			s_enemies.clear();
		}
		LOG_TRACE("[Compass] >> ScanEnemies done (%d)", static_cast<int>(s_enemies.size()));

		LOG_TRACE("[Compass] >> ScanCustomPins...");
		try { s_customPins = Layout::ScanCustomPins(world); }
		catch (...)
		{
			LOG_WARN("[Compass] Exception in ScanCustomPins -- cache cleared");
			s_customPins.clear();
		}
		LOG_TRACE("[Compass] >> ScanCustomPins done (%d)", static_cast<int>(s_customPins.size()));

		// HLOD fallback: appends distant markers/cores not yet in the streaming radius.
		// Deduplicates against already-found real actor entries by proximity.
		LOG_TRACE("[Compass] >> ScanHLOD...");
		try { Layout::ScanHLOD(world, s_markers, s_cores); }
		catch (...) { LOG_WARN("[Compass] Exception in ScanHLOD -- ignored"); }
		LOG_TRACE("[Compass] >> ScanHLOD done (markers=%d cores=%d after merge)",
		          static_cast<int>(s_markers.size()), static_cast<int>(s_cores.size()));

		int caveCount = 0;
		for (const auto& m : s_markers)
			if (m.type == Layout::PoiType::Cave) ++caveCount;

		int bodyCount = 0, droneCount = 0;
		for (const auto& f : s_foundables)
		{
			if (f.type == Layout::FoundableType::DeadBody) ++bodyCount;
			else if (f.type == Layout::FoundableType::Drone) ++droneCount;
		}
		LOG_TRACE(
			"[Compass] Scan complete: %d players, %d cores, %d POIs (%d caves), %d bodies, %d drones, %d enemies, %d custompins",
			static_cast<int>(s_playerMarkers.size()), static_cast<int>(s_cores.size()),
			static_cast<int>(s_markers.size()), caveCount, bodyCount, droneCount, static_cast<int>(s_enemies.size()),
			static_cast<int>(s_customPins.size()));

		// Signal the subsystem AFTER reading so the next gameplay tick (which runs
		// before our next PostRender) sees the flag and refreshes its data in time
		// for our next scan. Setting it before the reads would only affect the tick
		// AFTER this frame anyway, making it equivalent but less clear in intent.
		SDK::APlayerController* localPC = SDK::UGameplayStatics::GetPlayerController(world, 0);
		if (localPC)
			*(uint8_t*)((char*)localPC + 3816) = 1;
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

		SDK::APawn* localPawn = pc->K2_GetPawn();
		SDK::FVector playerLoc = localPawn ? localPawn->K2_GetActorLocation() : SDK::FVector{};

		SDK::FRotator controlRot = pc->GetControlRotation();
		// rawYaw: UE-space (0 = East). Used for entity bearing math.
		// yaw:    compass-convention (0 = North). Used for cardinal placement.
		const float rawYaw = static_cast<float>(controlRot.Yaw);
		const float yaw = rawYaw + 90.0f;

		const float scale = s_cfg.scale;
		const float posY = s_cfg.posY;
		const float halfWidth = canvas->SizeX * 0.5f * s_cfg.widthFraction * scale;
		const float centerX = canvas->SizeX * 0.5f;
		const float left = centerX - halfWidth;
		const float right = centerX + halfWidth;

		// One-time config dump to help diagnose sizing/position issues
		static bool s_firstDraw = true;
		if (s_firstDraw)
		{
			s_firstDraw = false;
			LOG_INFO("[Compass] First draw -- scale=%.2f posY=%.1f widthFraction=%.2f scanInterval=%d canvas=%dx%d",
			         scale, posY, s_cfg.widthFraction, s_cfg.entityScanInterval,
			         static_cast<int>(canvas->SizeX), static_cast<int>(canvas->SizeY));
		}

		SDK::UFont* labelFont = SDK::UEngine::GetEngine() ? SDK::UEngine::GetEngine()->LargeFont : nullptr;
		SDK::UFont* entityFont = labelFont; // same font as cardinals for bigger entity text

		// --- Throttled player scan (fast) ---
		if (++s_playerScanTick >= s_cfg.playerScanInterval)
		{
			s_playerScanTick = 0;

			// Force an immediate refresh of PlayersMarkerDataContainer before reading it.
			// Without this, the container only updates when the game processes the flag
			// at localPC+3816, which happens at the slow entityScanInterval rate.
			if (g_gatherPlayersDataFn)
			{
				if (world != g_mapManuWorld)
				{
					g_mapManuSubsystem = nullptr;
					g_mapManuWorld = world;
				}
				if (!g_mapManuSubsystem)
					g_mapManuSubsystem = FindMapManuSubsystem(world);

				if (g_mapManuSubsystem)
				{
					if (!CallGatherPlayersData(g_gatherPlayersDataFn, g_mapManuSubsystem))
					{
						LOG_WARN("[Compass] Exception in GatherPlayersData -- resetting subsystem cache");
						g_mapManuSubsystem = nullptr;
					}
				}
			}

			try { s_playerMarkers = Layout::ScanPlayerMarkers(world); }
			catch (...)
			{
				LOG_WARN("[Compass] Exception in ScanPlayerMarkers -- cache cleared");
				s_playerMarkers.clear();
			}
		}

		// --- Throttled full entity scan (slow) ---
		if (++s_scanTick >= s_cfg.entityScanInterval)
		{
			s_scanTick = 0;
			RefreshEntities(world);
		}

		// --- Helper: world position -> compass screenX ---
		auto ToScreenX = [&](const SDK::FVector& pos) -> float
		{
			const float dx = pos.X - playerLoc.X;
			const float dy = pos.Y - playerLoc.Y;
			float delta = atan2f(dy, dx) * (180.0f / 3.14159265358979f) - rawYaw;
			while (delta > 180.0f) delta -= 360.0f;
			while (delta < -180.0f) delta += 360.0f;
			return centerX + (delta / 90.0f) * halfWidth;
		};

		auto InBounds = [&](float x) { return x >= left && x <= right; };

		// --- Colors ---
		static constexpr SDK::FLinearColor white{1.0f, 1.0f, 1.0f, 0.9f};
		static constexpr SDK::FLinearColor dimWhite{1.0f, 1.0f, 1.0f, 0.4f};
		static constexpr SDK::FLinearColor yellow{1.0f, 0.85f, 0.0f, 1.0f};
		static constexpr SDK::FLinearColor colPlayer{0.3f, 0.8f, 1.0f, 1.0f}; // cyan
		static constexpr SDK::FLinearColor colCore{1.0f, 0.5f, 0.0f, 1.0f}; // orange
		static constexpr SDK::FLinearColor colMarker{1.0f, 1.0f, 0.0f, 1.0f}; // yellow
		static constexpr SDK::FLinearColor colBody{0.7f, 0.7f, 0.7f, 1.0f}; // grey
		static constexpr SDK::FLinearColor colDrone{0.4f, 0.9f, 0.5f, 1.0f}; // green
		static constexpr SDK::FLinearColor colEnemy{1.0f, 0.15f, 0.15f, 1.0f}; // red
		static constexpr SDK::FLinearColor black1{0.0f, 0.0f, 0.0f, 1.0f};

		// --- Horizontal compass line ---
		// DrawLine ignores alpha (uses FBatchedElements which has no translucent blend).
		// DrawRect uses FCanvasTileItem with SE_BLEND_Translucent, so alpha works.
		const float lineH = 1.5f * scale;
		hud->DrawRect(s_cfg.lineColor, left, posY - lineH * 0.5f, right - left, lineH);

		// --- Cardinal ticks + labels (above the line) ---
		for (const auto& c : CARDINALS)
		{
			float delta = c.worldYaw - yaw;
			while (delta > 180.0f) delta -= 360.0f;
			while (delta < -180.0f) delta += 360.0f;

			const float screenX = centerX + (delta / 90.0f) * halfWidth;
			if (!InBounds(screenX)) continue;

			// Tick crossing the line
			hud->DrawLine(screenX, posY - 6.0f * scale, screenX, posY + 4.0f * scale, white, 1.0f * scale);

			// Label above the tick
			SDK::FString label(c.label);
			SDK::FVector2D sz = canvas->K2_TextSize(labelFont, label, {scale, scale});
			canvas->K2_DrawText(labelFont, label,
			                    {screenX - sz.X * 0.5f, posY - 9.0f * scale - sz.Y},
			                    {scale, scale}, white, 0.0f,
			                    {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f},
			                    false, false, true, black1);
		}

		// --- Center notch (current heading, above the line) ---
		hud->DrawLine(centerX, posY - 22.0f * scale, centerX, posY + 6.0f * scale, yellow, 2.5f * scale);

		// --- Config: per-entity settings (from cache) ---
		const auto& cfgPlayers = s_cfg.players;
		const auto& cfgCores = s_cfg.cores;
		const auto& cfgMarkers = s_cfg.markers;
		const auto& cfgFoundables = s_cfg.foundables;
		const auto& cfgEnemies = s_cfg.enemies;
		const auto& cfgCustomPins = s_cfg.customPins;

		// --- Helper: distance-based alpha (fade starts at 80% of max) ---
		auto DistAlpha = [](float dist, float maxDist) -> float
		{
			if (maxDist <= 0.0f) return 1.0f;
			const float fadeStart = maxDist * 0.8f;
			if (dist <= fadeStart) return 1.0f;
			if (dist >= maxDist) return 0.0f;
			return (maxDist - dist) / (maxDist - fadeStart);
		};

		// --- Entity markers (below the line) ---

		// Shared edge-fade calculation
		auto EdgeAlpha = [&](float screenX) -> float
		{
			const float edgeFadeZone = halfWidth * 0.12f;
			return fminf(
				fminf((screenX - left) / edgeFadeZone, 1.0f),
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
				SDK::FLinearColor tint{1.0f, 1.0f, 1.0f, finalAlpha};
				hud->DrawTexture(tex,
				                 screenX - iconSize * 0.5f, posY + 10.0f * scale, iconSize, iconSize,
				                 0.0f, 0.0f, 1.0f, 1.0f,
				                 tint, SDK::EBlendMode::BLEND_Translucent,
				                 1.0f, false, 0.0f, {0.5f, 0.5f});
			}
			else
			{
				colour.A *= finalAlpha;
				const float es = scale * 1.25f;
				SDK::FString s(fallbackSym);
				SDK::FVector2D sz = canvas->K2_TextSize(entityFont, s, {es, es});
				SDK::FLinearColor outline{0.0f, 0.0f, 0.0f, finalAlpha};
				canvas->K2_DrawText(entityFont, s,
				                    {screenX - sz.X * 0.5f, posY + 16.0f * scale},
				                    {es, es}, colour, 0.0f,
				                    {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f},
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
				SDK::FLinearColor tint{1.0f, 1.0f, 1.0f, finalAlpha};
				hud->DrawTexture(tex,
				                 screenX - iconSize * 0.5f, posY + 10.0f * scale, iconSize, iconSize,
				                 0.0f, 0.0f, 1.0f, 1.0f,
				                 tint, SDK::EBlendMode::BLEND_Translucent,
				                 1.0f, false, 0.0f, {0.5f, 0.5f});
			}

			// Name label below the icon (or below stem if no icon)
			colour.A *= finalAlpha;
			const float es = scale * 1.0f;
			SDK::FString s(label);
			SDK::FVector2D sz = canvas->K2_TextSize(entityFont, s, {es, es});
			SDK::FLinearColor outline{0.0f, 0.0f, 0.0f, finalAlpha};
			const float labelY = posY + 10.0f * scale + (showIcon ? iconSize : 6.0f * scale);
			canvas->K2_DrawText(entityFont, s,
			                    {screenX - sz.X * 0.5f, labelY},
			                    {es, es}, colour, 0.0f,
			                    {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f},
			                    false, false, true, outline);
		};

		// --- POI helpers (used when building the draw queue below) ---
		auto PoiTypeEnabled = [&](Layout::PoiType t) -> bool
		{
			switch (t)
			{
			case Layout::PoiType::Antena: return cfgMarkers.showAntena;
			case Layout::PoiType::AbandonedBase: return cfgMarkers.showAbandonedBase;
			case Layout::PoiType::Cave: return cfgMarkers.showCave;
			case Layout::PoiType::Obelisk: return cfgMarkers.showObelisk;
			default: return false;
			}
		};
		auto PoiSymbol = [](Layout::PoiType t) -> const wchar_t*
		{
			switch (t)
			{
			case Layout::PoiType::Antena: return L"A";
			case Layout::PoiType::AbandonedBase: return L"Ab";
			case Layout::PoiType::Cave: return L"C";
			case Layout::PoiType::Obelisk: return L"Ob";
			default: return L"?";
			}
		};
		auto PoiDistance = [&](Layout::PoiType t) -> float
		{
			switch (t)
			{
			case Layout::PoiType::Antena: return cfgMarkers.antenaDistance;
			case Layout::PoiType::AbandonedBase: return cfgMarkers.abandonedBaseDistance;
			case Layout::PoiType::Cave: return cfgMarkers.caveDistance;
			case Layout::PoiType::Obelisk: return cfgMarkers.obeliskDistance;
			default: return 0.0f;
			}
		};

		// ---------------------------------------------------------------------------
		// Unified draw queue -- all visible entities sorted by distance descending
		// so that the closest entity is always drawn last (rendered on top).
		// ---------------------------------------------------------------------------
		struct DrawCall
		{
			float distSq;
			std::function<void()> fn;
		};
		std::vector<DrawCall> drawQueue;
		drawQueue.reserve(
			s_playerMarkers.size() + s_cores.size() +
			s_markers.size() + s_foundables.size() + s_customPins.size());

		// Players -- sourced from PlayersMarkerDataContainer (map subsystem), not actor scan.
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
				const std::wstring wn = p.playerName;
				drawQueue.push_back({
					distSq, [=] { DrawEntityIconWithLabel(sx, s_tex.player, wn.c_str(), colPlayer, alpha); }
				});
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
				const std::wstring wn = c.name;
				drawQueue.push_back({
					distSq, [=] { DrawEntityIconWithLabel(sx, s_tex.baseCore, wn.c_str(), colCore, alpha); }
				});
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
				drawQueue.push_back({distSq, [=] { DrawEntityIcon(sx, tex, sym, colMarker, alpha); }});
			}
		}
		if (cfgFoundables.enabled)
		{
			for (const auto& f : s_foundables)
			{
				const bool isBody = f.type == Layout::FoundableType::DeadBody;
				const bool isDrone = f.type == Layout::FoundableType::Drone;

				if (isBody && !cfgFoundables.showDeadBody) continue;
				if (isDrone && !cfgFoundables.showDrone) continue;

				const float maxDist = isBody ? cfgFoundables.deadBodyDistance : cfgFoundables.droneDistance;
				const float dx = f.location.X - playerLoc.X, dy = f.location.Y - playerLoc.Y;
				const float distSq = dx * dx + dy * dy;
				const float alpha = DistAlpha(sqrtf(distSq), maxDist);
				if (alpha <= 0.0f) continue;
				const float sx = ToScreenX(f.location);
				if (isBody)
					drawQueue.push_back({distSq, [=] { DrawEntityIcon(sx, s_tex.body, L"D", colBody, alpha); }});
				else
					drawQueue.push_back({distSq, [=] { DrawEntityIcon(sx, s_tex.drone, L"Dr", colDrone, alpha); }});
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
				const std::wstring label = pin.playerName.empty() ? L"Pin" : pin.playerName;
				drawQueue.push_back({
					distSq, [=] { DrawEntityIconWithLabel(sx, s_tex.customPin, label.c_str(), col, alpha); }
				});
			}
		}

		// Sort furthest-first so closest entities paint on top.
		std::sort(drawQueue.begin(), drawQueue.end(),
		          [](const DrawCall& a, const DrawCall& b) { return a.distSq > b.distSq; });

		for (const auto& dc : drawQueue)
			dc.fn();

		// --- Enemy dots (drawn on the compass line itself, on top of everything else) ---
		// Two-pass per dot: outer glow rect then bright core rect.
		if (cfgEnemies.enabled)
		{
			const float dotCore = 3.0f * scale;
			const float dotGlow = dotCore * 2.5f;
			const float halfCore = dotCore * 0.5f;
			const float halfGlow = dotGlow * 0.5f;

			for (const auto& e : s_enemies)
			{
				const float dx = e.location.X - playerLoc.X, dy = e.location.Y - playerLoc.Y;
				const float dist = sqrtf(dx * dx + dy * dy);
				const float alpha = DistAlpha(dist, cfgEnemies.distance);
				if (alpha <= 0.0f) continue;

				const float sx = ToScreenX(e.location);
				if (!InBounds(sx)) continue;

				const float edgeAlpha = EdgeAlpha(sx);
				const float finalAlpha = alpha * edgeAlpha;
				if (finalAlpha <= 0.0f) continue;

				// Glow halo
				SDK::FLinearColor glow{colEnemy.R, colEnemy.G, colEnemy.B, 0.25f * finalAlpha};
				hud->DrawRect(glow, sx - halfGlow, posY - halfGlow, dotGlow, dotGlow);

				// Bright core
				SDK::FLinearColor core{colEnemy.R, colEnemy.G, colEnemy.B, finalAlpha};
				hud->DrawRect(core, sx - halfCore, posY - halfCore, dotCore, dotCore);
			}
		}
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

		try
		{
			world = SDK::UWorld::GetWorld();
			if (!world)
			{
				LOG_TRACE("[Compass] PostRender: no world");
				return;
			}
		}
		catch (...)
		{
			LOG_ERROR("[Compass] Exception in GetWorld -- skipping compass draw");
			return;
		}

		try
		{
			worldName = world->GetName();
			if (worldName != s_lastWorldName)
			{
				LOG_INFO("[Compass] World changed: '%s'", worldName.c_str());
				s_lastWorldName = worldName;
			}
		}
		catch (...)
		{
			LOG_ERROR("[Compass] Exception in GetName");
			return;
		}

		if (worldName != "ChimeraMain")
			return;

		try { DrawCompass(self, self->Canvas, world); }
		catch (...) { LOG_ERROR("[Compass] Exception in DrawCompass -- suppressed to avoid crash loop"); }
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

		uintptr_t gatherAddr = scanner->FindPatternInMainModule(
			"40 55 41 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 89 9C 24");
		if (gatherAddr)
		{
			g_gatherPlayersDataFn = reinterpret_cast<GatherPlayersData_t>(gatherAddr);
			LOG_INFO("[Compass] GatherPlayersData found at 0x%llX", static_cast<unsigned long long>(gatherAddr));
		}
		else
		{
			LOG_WARN("[Compass] GatherPlayersData pattern not found -- player markers may not update in real time");
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
			LOG_ERROR("[Compass] Failed to install hook on AHUD::PostRender at 0x%llX",
			          static_cast<unsigned long long>(addr));
			return false;
		}

		LOG_INFO("[Compass] Hooked AHUD::PostRender at 0x%llX", static_cast<unsigned long long>(addr));
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
