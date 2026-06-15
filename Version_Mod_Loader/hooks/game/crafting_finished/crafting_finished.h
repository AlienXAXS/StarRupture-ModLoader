#pragma once

#include "../../hooks_common.h"
#include <cstdint>

// ---------------------------------------------------------------------------
// ACrCrafter::NativeOnItemCraftingComplete Hook
//
// Purpose: Fires whenever any crafting building (Crafter, Forge, Refinery,
//          Factory, Assembler, Exporter, FoodProcessor, ItemPrinter, etc.)
//          finishes crafting an item. NativeOnItemCraftingComplete is the
//          Mass Entity signal handler shared by all of them -- it plays the
//          crafting-complete sounds and then calls the (script-only)
//          ACrCrafter::OnItemCraftingComplete BlueprintEvent, which has no
//          usable native ExecFunction and cannot be hooked directly.
//
// Hook point (found via AOB scan, not a UFUNCTION):
//   ACrCrafter::NativeOnItemCraftingComplete(ACrCrafter* this,
//                                             FMassEntityHandle EntityHandle,
//                                             FName SignalName)
//
// Performance note: this fires once per completed craft across every
// crafting building -- keep callbacks fast.
// ---------------------------------------------------------------------------

namespace Hooks::CraftingFinished
{
	// Callback signature for plugins.
	// crafter           : the ACrCrafter* (or subclass) that finished crafting, as void*
	// craftingComponent : the building's UCrCraftingComponent*, as void* (may be null)
	// entityIndex       : FMassEntityHandle::Index for the crafter's Mass entity
	// entitySerial      : FMassEntityHandle::SerialNumber for the crafter's Mass entity
	using PluginCraftingFinishedCallback = void(*)(void* crafter, void* craftingComponent,
	                                                int32_t entityIndex, int32_t entitySerial);

	// Install the hook
	bool Install();

	// Remove the hook
	void Remove();

	// Returns true if the hook is currently installed
	bool IsInstalled();

	// Register a plugin callback to be notified when any crafting building finishes crafting
	void RegisterPluginCallback(PluginCraftingFinishedCallback callback);

	// Unregister a plugin callback
	void UnregisterPluginCallback(PluginCraftingFinishedCallback callback);

	// Returns the trampoline address of the original ACrCrafter::NativeOnItemCraftingComplete, or 0 if not yet installed.
	// Cast to: void(__fastcall*)(void* thisPtr, uint64_t entityHandle, uint64_t signalName)
	uintptr_t GetOriginalPtr();
}
