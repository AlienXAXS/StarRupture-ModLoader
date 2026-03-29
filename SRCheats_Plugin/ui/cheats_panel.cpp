#include "cheats_panel.h"
#include "plugin_helpers.h"
#include "../cheats/player/player_cheats.h"
#include "../cheats/world/world_cheats.h"

// ── Static state ─────────────────────────────────────────────────────────────

PanelHandle         CheatsPanel::s_panelHandle    = nullptr;
IPluginUIEvents*    CheatsPanel::s_ui             = nullptr;
IPluginInputEvents* CheatsPanel::s_input          = nullptr;
char                CheatsPanel::s_itemNameBuf[256] = {};
int                 CheatsPanel::s_itemAmount     = 1;
char                CheatsPanel::s_attrNameBuf[64]  = {};
int                 CheatsPanel::s_attrValue      = 100;

// Panel descriptor must remain valid until UnregisterPanel
static const PluginPanelDesc s_panelDesc = {
    "SRCheats",         // button label in ModLoader Config tab
    "SRCheats##panel",  // ImGui window title (must be globally unique)
    &CheatsPanel::OnRender
};

// ── Lifecycle ────────────────────────────────────────────────────────────────

void CheatsPanel::Init(IPluginHooks* hooks)
{
    s_ui    = hooks->UI;
    s_input = hooks->Input;

    s_panelHandle = hooks->UI->RegisterPanel(&s_panelDesc);
    if (s_panelHandle)
        LOG_INFO("SRCheats panel registered");
    else
        LOG_ERROR("SRCheats panel registration failed");

    // Register F5 as a toggle hotkey for the cheat panel.
    // hooks->Input is non-null on client builds (same guard that wraps Init).
    if (s_input)
    {
        s_input->RegisterKeybind(EModKey::F5, EModKeyEvent::Pressed, &OnHotkey);
        LOG_DEBUG("SRCheats F5 hotkey registered");
    }
}

void CheatsPanel::Shutdown(IPluginHooks* hooks)
{
    if (s_input)
    {
        s_input->UnregisterKeybind(EModKey::F5, EModKeyEvent::Pressed, &OnHotkey);
        s_input = nullptr;
        LOG_DEBUG("SRCheats F5 hotkey unregistered");
    }

    if (s_panelHandle)
    {
        hooks->UI->UnregisterPanel(s_panelHandle);
        s_panelHandle = nullptr;
        LOG_DEBUG("SRCheats panel unregistered");
    }

    s_ui = nullptr;
}

// ── F5 hotkey ─────────────────────────────────────────────────────────────────

// F5 always opens the cheat panel. Closing is handled by:
//   - The X button on the panel window itself
//   - F2 (which calls CloseAllPanels on the registry side)
// Tracking a local open/closed mirror would drift out of sync whenever the
// panel is closed externally, causing a dead press. Simpler to just always open.
void CheatsPanel::OnHotkey(EModKey /*key*/, EModKeyEvent /*event*/)
{
    if (!s_panelHandle || !s_ui) return;
    s_ui->SetPanelOpen(s_panelHandle);
    LOG_INFO("SRCheats panel opened via F5");
}

// ── Render callback ──────────────────────────────────────────────────────────

void CheatsPanel::OnRender(IModLoaderImGui* imgui)
{
    RenderPlayerSection(imgui);
    RenderWorldSection(imgui);
    RenderSurvivalSection(imgui);
}

// ── Section: Player ──────────────────────────────────────────────────────────

void CheatsPanel::RenderPlayerSection(IModLoaderImGui* imgui)
{
    if (!imgui->CollapsingHeader("Player"))
        return;

    // God mode toggle
    if (imgui->Checkbox("God Mode", &PlayerCheats::s_godModeEnabled))
        LOG_INFO(PlayerCheats::s_godModeEnabled ? "God mode enabled" : "God mode disabled");

    // Unlimited ammo toggle
    if (imgui->Checkbox("Unlimited Ammo", &PlayerCheats::s_unlimitedAmmoEnabled))
        LOG_INFO(PlayerCheats::s_unlimitedAmmoEnabled ? "Unlimited ammo enabled" : "Unlimited ammo disabled");

    // Flight toggle + speed slider
    if (imgui->Checkbox("Flight", &PlayerCheats::s_flightEnabled))
    {
        if (PlayerCheats::s_flightEnabled)
            LOG_INFO("Flight enabled");
        else
        {
            LOG_INFO("Flight disabled");
            PlayerCheats::StopFlight();
        }
    }

    if (PlayerCheats::s_flightEnabled)
    {
        imgui->SetNextItemWidth(200.0f);
        imgui->SliderFloat("  Speed##flight", &PlayerCheats::s_flightSpeed,
                           100.0f, 5000.0f, "%.0f");
        if (imgui->IsItemHovered())
            imgui->SetTooltip("Flight speed in UU/s (default 1500, vanilla ~600)");
    }

    imgui->Separator();

    // Give default weapons
    if (imgui->Button("Give Default Weapons"))
        PlayerCheats::GiveDefaultWeapons();

    imgui->Spacing();

    // Give item by name: [item name input] x [amount input] [Give Item]
    imgui->SetNextItemWidth(180.0f);
    imgui->InputText("##itemname", s_itemNameBuf, sizeof(s_itemNameBuf));
    if (imgui->IsItemHovered())
        imgui->SetTooltip("Item name (e.g. IronOre, WoodPlank)");

    imgui->SameLine(0.0f, 4.0f);
    imgui->SetNextItemWidth(60.0f);
    imgui->InputInt("##itemamt", &s_itemAmount, 1, 10);

    imgui->SameLine(0.0f, 4.0f);
    if (imgui->Button("Give Item"))
    {
        if (s_itemNameBuf[0] != '\0')
            PlayerCheats::GiveItem(s_itemNameBuf, s_itemAmount);
    }

    imgui->Spacing();

    if (imgui->Button("Teleport to Camera"))
        PlayerCheats::TeleportToCamera();
}

// ── Section: World ───────────────────────────────────────────────────────────

void CheatsPanel::RenderWorldSection(IModLoaderImGui* imgui)
{
    if (!imgui->CollapsingHeader("World"))
        return;

    if (imgui->Button("Kill All Enemies"))
        WorldCheats::KillAllEnemies();

    imgui->Spacing();

    // No enemy spawns toggle
    if (imgui->Checkbox("No Enemy Spawns", &WorldCheats::s_noEnemySpawns))
        LOG_INFO(WorldCheats::s_noEnemySpawns ? "Enemy spawns blocked" : "Enemy spawns unblocked");

    // Enemies ignore me (stub - not yet implemented)
    bool ignoreMe = WorldCheats::s_enemiesIgnoreMe;
    if (imgui->Checkbox("Enemies Ignore Me", &ignoreMe))
        WorldCheats::SetEnemiesIgnoreMe(ignoreMe);
    if (imgui->IsItemHovered())
        imgui->SetTooltip("Not yet implemented - requires AI perception hook");

    imgui->Separator();

    if (imgui->Button("Destroy All Items"))
        WorldCheats::DestroyAllItems();

    imgui->SameLine(0.0f, 8.0f);

    if (imgui->Button("Unlock All Features"))
        WorldCheats::UnlockAllFeatures();
}

// ── Section: Survival Attributes ─────────────────────────────────────────────

void CheatsPanel::RenderSurvivalSection(IModLoaderImGui* imgui)
{
    if (!imgui->CollapsingHeader("Survival Attributes"))
        return;

    imgui->TextDisabled("Set a named survival stat (Health, Oxygen, Hydration, etc.)");
    imgui->Spacing();

    imgui->SetNextItemWidth(140.0f);
    imgui->InputText("##attrname", s_attrNameBuf, sizeof(s_attrNameBuf));
    if (imgui->IsItemHovered())
        imgui->SetTooltip("Survival attribute name (e.g. Health, Oxygen, Hydration, Calories)");

    imgui->SameLine(0.0f, 4.0f);
    imgui->SetNextItemWidth(80.0f);
    imgui->InputInt("##attrval", &s_attrValue, 1, 10);

    imgui->SameLine(0.0f, 4.0f);
    if (imgui->Button("Set"))
    {
        if (s_attrNameBuf[0] != '\0')
            WorldCheats::SetSurvivalAttr(s_attrNameBuf, s_attrValue);
    }
}
