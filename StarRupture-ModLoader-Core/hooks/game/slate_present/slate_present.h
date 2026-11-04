#pragma once

#ifdef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// SlatePresent
//
// Draws the overlay at FSlateRHIRenderer::PresentWindow_RenderThread: the
// point where Slate has drawn every widget of a window into its back buffer
// (HUD, menus, the pause menu's background blur) and nothing has read it yet.
// The first thing that function does is broadcast OnBackBufferReadyToPresent,
// which is where frame generation picks the UI up (UE's Streamline plugin
// extracts its UI buffer there), and then it presents. So the overlay is part
// of the finished UI image for every frame generator, and for none.
//
// Nothing is recorded on UE's command lists. The overlay is recorded on the
// ImGui host's own command list and submitted on its own queue, ordered with
// UE's queue by two fences UE itself signals and waits on
// (RHISignalManualFence / RHIWaitManualFence), from an RHI command so it runs
// in order with Slate's work. The back buffer goes to RTV through UE's own
// RHICmdList.Transition, so UE's resource tracking stays true.
//
// Render thread (the detour) and RHI thread (the command). No UObjects.
// ---------------------------------------------------------------------------

namespace Hooks::SlatePresent
{
	// Resolves the engine functions and detours PresentWindow_RenderThread.
	// Call once the ImGui host is loaded. False leaves the overlay on the
	// Present hook alone.
	bool Install();

	void Remove();
}

#endif // MODLOADER_CLIENT_BUILD
