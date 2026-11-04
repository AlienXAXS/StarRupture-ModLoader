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

		// The tag's FName comparison index once a layer has matched it by
		// string, so later ticks compare an int. 0 until then.
		int32_t g_menuLayerTagIndex = 0;

		bool IsMenuLayerTag(const SDK::FName& tag)
		{
			if (g_menuLayerTagIndex != 0)
				return tag.ComparisonIndex == g_menuLayerTagIndex && tag.Number == 0;
			if (tag.ToString() != kMenuLayerTag)
				return false;
			g_menuLayerTagIndex = tag.ComparisonIndex;
			return true;
		}

		// The local player's menu layer, or null. Valid for the current tick only.
		SDK::UCommonActivatableWidgetContainerBase* FindMenuLayer(SDK::APlayerController* pc)
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
					if (stack && IsMenuLayerTag(layers[l].Key().TagName))
						return stack;
				}
			}
			return nullptr;
		}
	}

	uint64_t ReadMenuLayer(SDK::APlayerController* pc)
	{
		if (!pc)
			return 0;
		SDK::UCommonActivatableWidgetContainerBase* layer = FindMenuLayer(pc);
		if (!layer)
			return 0;

		// Every widget on the stack's list counts, closing ones too: a widget
		// stays on WidgetList until its close transition has ended, long after
		// it stopped being the active one (bIsActive). So the bits stay set
		// from the moment a screen is pushed until it is fully gone.
		constexpr int32_t kMaxKinds = 16;
		UIState::MenuKind kinds[kMaxKinds];
		const int32_t     count = layer->WidgetList.Num();
		const int32_t     kept  = count < kMaxKinds ? count : kMaxKinds;
		for (int32_t i = 0; i < kept; ++i)
		{
			SDK::UCommonActivatableWidget* w = layer->WidgetList[i];
			if (!w)
				kinds[i] = UIState::MenuKind::Other;
			else if (w->IsA(SDK::UCrUW_MapMenu::StaticClass()))
				kinds[i] = UIState::MenuKind::Map;
			else if (w->IsA(SDK::UCrUW_InventoryScreen::StaticClass()))
				kinds[i] = UIState::MenuKind::Inventory;
			else if (w->IsA(SDK::UCrUW_BuildingMenu::StaticClass()))
				kinds[i] = UIState::MenuKind::BuildMenu;
			else if (w->IsA(SDK::UCrUW_ResearchTerminalPanel::StaticClass())
			      || w->IsA(SDK::UCrUW_UpgradeStationPanel::StaticClass())
			      || w->IsA(SDK::UCrUW_ArmoryPanel::StaticClass()))
				kinds[i] = UIState::MenuKind::StationMenu;
			else
				kinds[i] = UIState::MenuKind::Other;
		}

		return UIState::MenuBits(static_cast<size_t>(count), kinds, static_cast<size_t>(kept));
	}
}

#endif // MODLOADER_CLIENT_BUILD
