#pragma once

#include "../../hooks_common.h"

// ---------------------------------------------------------------------------
// One-shot UObject::ProcessEvent hook for plugin initialization timing,
// plus a general ProcessEvent dispatch system.
//
// ProcessEvent is only dispatched once the engine is genuinely running game
// logic, which guarantees GObjects is fully populated.  On the very first
// call the hook triggers InitAllLoadedPlugins() then latches off.
//
// Other systems (e.g. ClientMessage) that need to observe ProcessEvent calls
// register a callback here instead of installing their own hook on the same
// address.
// ---------------------------------------------------------------------------

namespace Hooks::GameInstanceInit
{
	// Callback signature: (obj, UFunction*, params) -- all void* to avoid
	// dragging SDK headers into every consumer.
	using ProcessEventCallback = void(*)(void* obj, void* fn, void* params);

	bool Install();
	void Remove();

	void RegisterProcessEventCallback(ProcessEventCallback cb);
	void UnregisterProcessEventCallback(ProcessEventCallback cb);
}
