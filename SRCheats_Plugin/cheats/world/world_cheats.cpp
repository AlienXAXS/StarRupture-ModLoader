#include "world_cheats.h"
#include "plugin_helpers.h"

#include "Util.cpp"

#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"
#include "MassAIPrototypeEnemyRuntime_classes.hpp"

using namespace SDK;

// ── Static state ─────────────────────────────────────────────────────────────

bool WorldCheats::s_noEnemySpawns   = false;
bool WorldCheats::s_enemiesIgnoreMe = false;


// ── Internal helpers ─────────────────────────────────────────────────────────

static ACrPlayerControllerBase* GetLocalPC()
{
    auto* s_world = SDK::UWorld::GetWorld();
    if (!s_world) return nullptr;
    return static_cast<ACrPlayerControllerBase*>(
        UGameplayStatics::GetPlayerController(s_world, 0));
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void WorldCheats::Init(IPluginHooks* hooks)
{
    hooks->Spawner->RegisterOnBeforeActivate(&OnBeforeActivateSpawner);
    LOG_DEBUG("WorldCheats registered world and spawner callbacks");
}

void WorldCheats::Shutdown(IPluginHooks* hooks)
{
    hooks->Spawner->UnregisterOnBeforeActivate(&OnBeforeActivateSpawner);
    s_noEnemySpawns   = false;
    s_enemiesIgnoreMe = false;
    LOG_DEBUG("WorldCheats unregistered callbacks");
}

bool WorldCheats::OnBeforeActivateSpawner(void* /*spawner*/, bool /*bDisableAggroLock*/)
{
    // Returning true cancels the spawn and suppresses After callbacks
    return s_noEnemySpawns;
}

// ── One-shot actions ─────────────────────────────────────────────────────────

void WorldCheats::KillAllEnemies()
{
    // If we're not in the main game world, dont run cheats
    if (!Util::IsInChimeraMain())
        return;

    auto* s_world = SDK::UWorld::GetWorld();

    // Collect every live enemy actor in the world
    TArray<AActor*> actors;
    UGameplayStatics::GetAllActorsOfClass(
        s_world,
        AMassEnemyCharacterBase::StaticClass(),
        &actors);

    int32 count = actors.Num();
    if (count == 0)
    {
        LOG_INFO("Kill all enemies: no enemies found in world");
        return;
    }

    LOG_INFO("Killing %d enemies", count);

    // Use the local PC as instigator so kills are attributed correctly.
    // crPC may be null on server builds; ApplyDamage tolerates a null instigator.
    ACrPlayerControllerBase* crPC = GetLocalPC();

    int32 killed = 0;
    for (int32 i = 0; i < count; ++i)
    {
        AMassEnemyCharacterBase* enemy =
            static_cast<AMassEnemyCharacterBase*>(actors[i]);
        if (!enemy) continue;

        // Apply a lethal amount of damage through the game's standard damage
        // pipeline so death animations, loot drops, and scoring all fire normally.
        UGameplayStatics::ApplyDamage(enemy, 999999.0f, crPC, crPC, nullptr);
        ++killed;
    }

    LOG_DEBUG("KillAllEnemies: applied damage to %d / %d actors", killed, count);
}

void WorldCheats::DestroyAllItems()
{
    // If we're not in the main game world, dont run cheats
    if (!Util::IsInChimeraMain())
        return;

    ACrPlayerControllerBase* crPC = GetLocalPC();
    if (!crPC)
    {
        LOG_WARN("Destroy all items: no local player controller");
        return;
    }
    LOG_INFO("Destroying all world items");
    crPC->DebugDestroyAllItems();
}

void WorldCheats::UnlockAllFeatures()
{
    // If we're not in the main game world, dont run cheats
    if (!Util::IsInChimeraMain())
        return;

    ACrPlayerControllerBase* crPC = GetLocalPC();
    if (!crPC)
    {
        LOG_WARN("Unlock all features: no local player controller");
        return;
    }
    LOG_INFO("Unlocking all features");
    crPC->DebugUnlockAllFeatures();
}

void WorldCheats::SetSurvivalAttr(const char* name, int value)
{
    // If we're not in the main game world, dont run cheats
    if (!Util::IsInChimeraMain())
        return;

    ACrPlayerControllerBase* crPC = GetLocalPC();
    if (!crPC)
    {
        LOG_WARN("Set survival attribute: no local player controller");
        return;
    }

    LOG_DEBUG("SetSurvivalAttribute: name=%s value=%d", name, value);

    wchar_t wBuf[64] = {};
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wBuf, 64);
    FString sdkName(wBuf);  // points to stack buffer - valid for duration of call

    crPC->DebugSetSurvivalAttribute(sdkName, value);
}

void WorldCheats::SetEnemiesIgnoreMe(bool enable)
{
    // If we're not in the main game world, dont run cheats
    if (!Util::IsInChimeraMain())
        return;

    s_enemiesIgnoreMe = enable;
    if (enable)
    {
        // AI perception hooking requires additional pattern scanning work
        LOG_WARN("Enemies Ignore Me is not yet implemented - requires AI perception hook");
    }
}
