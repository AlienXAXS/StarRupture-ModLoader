#pragma once

#ifdef MODLOADER_CLIENT_BUILD
void InitClientUI();
void ShutdownClientUI();

// The two input-arbitration predicates, as the ImGui host asks them every frame.
//
// Exposed rather than left as lambdas so the debug HUD can report the same answer
// the backend acts on. A readout that recomputed the condition itself would be
// worse than none at all: the one bug it exists to catch is exactly the case
// where the arbitration says something the reader does not expect, and a second
// copy of the logic would quietly agree with the reader instead of the loader.
bool ShouldCaptureInputNow();
bool ShouldPassthroughInputNow();
#endif
