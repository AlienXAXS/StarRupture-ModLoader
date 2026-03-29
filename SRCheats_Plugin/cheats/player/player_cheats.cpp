#include "player_cheats.h"
#include "plugin_helpers.h"

#include "Util.cpp"
#include "Engine_classes.hpp"
#include "Chimera_classes.hpp"

using namespace SDK;

// ── Static state ────────────────────────────────────────────────────────────

bool  PlayerCheats::s_godModeEnabled      = false;
bool  PlayerCheats::s_unlimitedAmmoEnabled = false;
bool  PlayerCheats::s_flightEnabled        = false;
float PlayerCheats::s_flightSpeed          = 1500.0f;

// Original MaxFlySpeed saved when flight is first applied so we can restore it.
// Negative sentinel = not yet saved.
static float s_originalFlySpeed = -1.0f;

// ── Internal helpers ─────────────────────────────────────────────────────────

static ACrPlayerControllerBase* GetLocalPC()
{
    auto* s_world = SDK::UWorld::GetWorld();
    if (!s_world) return nullptr;
    return static_cast<ACrPlayerControllerBase*>(
        UGameplayStatics::GetPlayerController(s_world, 0));
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void PlayerCheats::Init(IPluginHooks* hooks)
{
    hooks->Engine->RegisterOnTick(&OnTick);
    LOG_DEBUG("PlayerCheats registered world and tick callbacks");
}

void PlayerCheats::Shutdown(IPluginHooks* hooks)
{
    hooks->Engine->UnregisterOnTick(&OnTick);
    s_godModeEnabled      = false;
    s_unlimitedAmmoEnabled = false;
    s_flightEnabled        = false;
    s_originalFlySpeed    = -1.0f;
    LOG_DEBUG("PlayerCheats unregistered callbacks");
}

// ── Callbacks ────────────────────────────────────────────────────────────────

void PlayerCheats::OnTick(float /*deltaSeconds*/)
{
    if ((!s_godModeEnabled && !s_unlimitedAmmoEnabled && !s_flightEnabled) || !Util::IsInChimeraMain())
        return;

    ACrPlayerControllerBase* crPC = GetLocalPC();
    if (!crPC) return;

    ACrCharacterPlayerBase* crChar = crPC->CrChar;
    if (!crChar) return;

    if (s_godModeEnabled)
    {
        UCrHealthAttributeSet* health = crChar->HealthAttributes;
        if (health)
        {
            float maxVal = health->MaxHealth.CurrentValue;
            if (health->CurrentHealth.CurrentValue < maxVal)
            {
                health->CurrentHealth.CurrentValue = maxVal;
                health->CurrentHealth.BaseValue    = maxVal;
            }
        }
    }

    if (s_unlimitedAmmoEnabled)
    {
        UCrWeaponComponent* weapon = crChar->WeaponSystem;
        if (weapon)
        {
            float maxAmmo = weapon->GetEquippedWeaponMaxAmmo();
            if (maxAmmo > 0.0f)
                weapon->SetEquippedWeaponCurrentAmmo(maxAmmo);
        }
    }

    if (s_flightEnabled)
    {
        UCrCharacterMovementComponent* movement = crChar->CrCharacterMovementComponent;
        if (movement)
        {
            // Save the original before we touch it for the first time this session
            if (s_originalFlySpeed < 0.0f)
            {
                s_originalFlySpeed = movement->MaxFlySpeed;
                LOG_DEBUG("Flight: saved original MaxFlySpeed=%.1f", s_originalFlySpeed);
            }

            movement->MovementMode = EMovementMode::MOVE_Flying;
            movement->MaxFlySpeed  = s_flightSpeed;
        }
    }
}

// ── One-shot actions ─────────────────────────────────────────────────────────

void PlayerCheats::GiveDefaultWeapons()
{
    // If we're not in the main game world, dont run cheats
    if (!Util::IsInChimeraMain())
        return;

    ACrPlayerControllerBase* crPC = GetLocalPC();
    if (!crPC)
    {
        LOG_WARN("Give default weapons: no local player controller");
        return;
    }
    LOG_INFO("Giving default weapons");
    crPC->CheatGiveDefaultWeapons();
}

void PlayerCheats::GiveItem(const char* name, int amount)
{
    // If we're not in the main game world, dont run cheats
    if (!Util::IsInChimeraMain())
        return;

    ACrPlayerControllerBase* crPC = GetLocalPC();
    if (!crPC)
    {
        LOG_WARN("Give item: no local player controller");
        return;
    }

    LOG_DEBUG("GiveItem: name=%s amount=%d", name, amount);

    wchar_t wBuf[256] = {};
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wBuf, 256);
    FString sdkName(wBuf);  // FString points to stack buffer - valid for duration of call

    TArray<UCrItemDataBase*> outItems = {};
    crPC->DebugAddItems(sdkName, amount, &outItems);
}

void PlayerCheats::TeleportToCamera()
{
    // If we're not in the main game world, dont run cheats
    if (!Util::IsInChimeraMain())
        return;

    ACrPlayerControllerBase* crPC = GetLocalPC();
    if (!crPC)
    {
        LOG_WARN("Teleport to camera: no local player controller");
        return;
    }
    LOG_INFO("Teleporting to camera");
    crPC->DebugTeleportPlayerToCamera();
}

void PlayerCheats::StopFlight()
{
    // If we're not in the main game world, dont run cheats
    if (!Util::IsInChimeraMain())
        return;

    ACrPlayerControllerBase* crPC = GetLocalPC();
    if (!crPC) return;
    ACrCharacterPlayerBase* crChar = crPC->CrChar;
    if (!crChar) return;
    UCrCharacterMovementComponent* movement = crChar->CrCharacterMovementComponent;
    if (!movement) return;

    // Restore the original fly speed so we don't permanently change it
    if (s_originalFlySpeed >= 0.0f)
    {
        movement->MaxFlySpeed  = s_originalFlySpeed;
        s_originalFlySpeed     = -1.0f;
        LOG_DEBUG("Flight stopped - MaxFlySpeed restored");
    }

    // Let physics take over naturally
    movement->MovementMode = EMovementMode::MOVE_Walking;
    LOG_DEBUG("Flight stopped - movement restored to Falling");
}
