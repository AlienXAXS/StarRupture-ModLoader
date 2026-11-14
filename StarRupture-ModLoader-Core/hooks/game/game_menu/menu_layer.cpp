#include "pch.h"

#ifdef MODLOADER_CLIENT_BUILD

// The menu-layer read behind the plugins' menu bits (UIState). It has a file
// of its own because the layout table tags the classes read here with the
// ui-menus consumer (tools\Update-LayoutTable.ps1): when one of them changes
// on a game build, only this read is switched off (UIState::DisableMenuRead),
// not the plugins.

#include "game_menu.h"
#include "core/ui_state.h"

#include "ChimeraUI_classes.hpp"
#include "CommonGame_classes.hpp"
#include "CommonUI_classes.hpp"

namespace Hooks::GameMenu
{
	namespace
	{
		// The layer UCrUIManagerSubsystem opens the map, the inventory, the
		// build menu and every building window on; the HUD pushes its escape
		// menu there too. The HUD itself sits on another layer.
		constexpr const char* kMenuLayerTag = "UI.Layer.Menu";

		// The layer the build menu sits on, in menu mode and in building
		// placement alike. The widget leaves it when the menu closes in menu
		// mode, and stays on it for seconds while a building is being placed.
		constexpr const char* kGameMenuLayerTag = "UI.Layer.GameMenu";

		// A tag's FName comparison index once a layer has matched it by
		// string, so later ticks compare an int. 0 until then.
		int32_t g_menuLayerTagIndex     = 0;
		int32_t g_gameMenuLayerTagIndex = 0;

		bool IsLayerTag(const SDK::FName& tag, const char* name, int32_t& cachedIndex)
		{
			if (cachedIndex != 0)
				return tag.ComparisonIndex == cachedIndex && tag.Number == 0;
			if (tag.ToString() != name)
				return false;
			cachedIndex = tag.ComparisonIndex;
			return true;
		}

		// The local player's layer with that tag, or null. Valid for the
		// current tick only.
		SDK::UCommonActivatableWidgetContainerBase* FindLayer(SDK::APlayerController* pc, const char* name, int32_t& cachedIndex)
		{
			auto* ui = static_cast<SDK::UGameUIManagerSubsystem*>(
				SDK::USubsystemBlueprintLibrary::GetGameInstanceSubsystem(pc, SDK::UCrUIManagerSubsystem::StaticClass()));
			SDK::UGameUIPolicy* policy = ui ? ui->CurrentPolicy : nullptr;
			if (!policy)
				return nullptr;

			for (int32_t i = 0; i < policy->RootViewportLayouts.Num(); ++i)
			{
				const SDK::FRootViewportLayoutInfo& info = policy->RootViewportLayouts[i];
				if (!info.RootLayout || info.LocalPlayer != pc->Player)
					continue;

				// The class is named so the layout table lists it and the
				// check covers Layers. Not const: the SDK's const TMap indexer
				// does not compile.
				SDK::UPrimaryGameLayout* root = info.RootLayout;
				auto& layers = root->Layers;
				for (int32_t l = 0; l < layers.NumAllocated(); ++l)
				{
					if (!layers.IsValidIndex(l))
						continue;
					SDK::UCommonActivatableWidgetContainerBase* stack = layers[l].Value();
					if (stack && IsLayerTag(layers[l].Key().TagName, name, cachedIndex))
						return stack;
				}
			}
			return nullptr;
		}

		// The local player's menu layer, or null. Valid for the current tick only.
		SDK::UCommonActivatableWidgetContainerBase* FindMenuLayer(SDK::APlayerController* pc)
		{
			return FindLayer(pc, kMenuLayerTag, g_menuLayerTagIndex);
		}

		// Whether the build menu is open for input: its widget is on the
		// game-menu layer, active, and the game's cursor flag is up. While a
		// building is being placed the widget stays on the layer with the flag
		// off, and the menu is not open then.
		bool BuildMenuOpenOnGameLayer(SDK::APlayerController* pc)
		{
			SDK::UCommonActivatableWidgetContainerBase* layer = FindLayer(pc, kGameMenuLayerTag, g_gameMenuLayerTagIndex);
			if (!layer)
				return false;

			constexpr int32_t kMaxWidgets = 16;
			const int32_t     count       = layer->WidgetList.Num();
			const int32_t     read        = count < kMaxWidgets ? count : kMaxWidgets;
			for (int32_t i = 0; i < read; ++i)
			{
				SDK::UCommonActivatableWidget* w = layer->WidgetList[i];
				if (w && w->IsA(SDK::UCrUW_BuildingMenu::StaticClass())
				      && UIState::BuildMenuOpen(w->bIsActive, pc->bShowMouseCursor))
					return true;
			}
			return false;
		}
	}

	uint64_t ReadMenuLayer(SDK::APlayerController* pc, bool* buildMenuActive)
	{
		*buildMenuActive = false;
		if (!pc)
			return 0;
		SDK::UCommonActivatableWidgetContainerBase* layer = FindMenuLayer(pc);

		// Every widget on the stack's list counts, closing ones too: a widget
		// stays on WidgetList until its close transition has ended, long after
		// it stopped being the active one (bIsActive). So the bits stay set
		// from the moment a screen is pushed until it is fully gone.
		constexpr int32_t kMaxKinds = 16;
		UIState::MenuKind kinds[kMaxKinds + 1];  // one spare for the build menu below
		int32_t           count = layer ? layer->WidgetList.Num() : 0;
		const int32_t     read  = count < kMaxKinds ? count : kMaxKinds;
		int32_t           kept  = read;
		for (int32_t i = 0; i < read; ++i)
		{
			SDK::UCommonActivatableWidget* w = layer->WidgetList[i];
			if (!w)
				kinds[i] = UIState::MenuKind::Other;
			else if (w->IsA(SDK::UCrUW_MapMenu::StaticClass()))
				kinds[i] = UIState::MenuKind::Map;
			else if (w->IsA(SDK::UCrUW_InventoryScreen::StaticClass()))
				kinds[i] = UIState::MenuKind::Inventory;
			else if (w->IsA(SDK::UCrUW_BuildingMenu::StaticClass()))
			{
				kinds[i] = UIState::MenuKind::BuildMenu;
				if (w->bIsActive)
					*buildMenuActive = true;
			}
			else if (w->IsA(SDK::UCrUW_ResearchTerminalPanel::StaticClass())
			      || w->IsA(SDK::UCrUW_UpgradeStationPanel::StaticClass())
			      || w->IsA(SDK::UCrUW_ArmoryPanel::StaticClass()))
				kinds[i] = UIState::MenuKind::StationMenu;
			else
				kinds[i] = UIState::MenuKind::Other;
		}

		// The build menu is on the game-menu layer: one more screen.
		if (BuildMenuOpenOnGameLayer(pc))
		{
			*buildMenuActive = true;
			kinds[kept++]    = UIState::MenuKind::BuildMenu;
			++count;
		}

		return UIState::MenuBits(static_cast<size_t>(count), kinds, static_cast<size_t>(kept));
	}

	bool ReadMenuHover(SDK::APlayerController* pc)
	{
		// Without a menu layer to read, or with nothing on it while a cursor
		// screen is up (the screen is on some other layer), the loader cannot
		// tell where the menu's widgets are: true, so a widget stays click-through.
		if (!pc)
			return true;
		SDK::UCommonActivatableWidgetContainerBase* layer = FindMenuLayer(pc);
		if (!layer || layer->WidgetList.Num() == 0)
			return true;

		// A widget is hovered only while the cursor is over a hit-testable
		// part of it, so a hidden or hit-test-invisible screen reads false.
		// Slate hovers the whole path down to the widget under the cursor, so
		// a screen's root counts as soon as one of its buttons or panels does.
		constexpr int32_t kMaxWidgets = 16;
		const int32_t     count       = layer->WidgetList.Num();
		const int32_t     read        = count < kMaxWidgets ? count : kMaxWidgets;
		for (int32_t i = 0; i < read; ++i)
		{
			SDK::UCommonActivatableWidget* w = layer->WidgetList[i];
			if (w && w->IsHovered())
				return true;
		}
		return false;
	}
}

#endif // MODLOADER_CLIENT_BUILD
