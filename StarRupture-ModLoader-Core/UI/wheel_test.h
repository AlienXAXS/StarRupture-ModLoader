#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// Mouse wheel test (v70, debug builds)
//
// Exercises IPluginInputEvents::RegisterMouseWheel from the ModLoader window's
// Debug section, through the same IPluginHooks table a plugin gets.
//
//   Live hook   -- registers a real wheel handler and shows what it receives
//                  in a small overlay that stays up during gameplay, so it can
//                  be checked with the ModLoader window closed (the only state
//                  where consuming means anything). Modes: observe, consume
//                  Ctrl+wheel, consume everything.
//   Self-test   -- feeds synthetic WM_MOUSEWHEEL / WM_MOUSEHWHEEL messages
//                  straight into the dispatcher and checks registration,
//                  payload, chain order, consume rules and unload cleanup.
// ---------------------------------------------------------------------------

namespace UI::WheelTest
{
	// Controls and results, drawn inside the ModLoader window's Debug section.
	void DrawControls();

	// The gameplay readout. Call every frame from the render callback; draws
	// nothing unless the live hook is on.
	void RenderOverlay();
}

#endif // MODLOADER_CLIENT_BUILD
