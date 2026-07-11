#pragma once

#include "tick_profiler.h"
#include <cstddef>
#include <vector>

// ---------------------------------------------------------------------------
// StackSampler
//
// A crude "what is the game thread actually doing right now" profiler for
// the one call we can't otherwise see inside: UGameEngine::Tick() itself.
// EngineTick::Detour() already times named segments (the engine call as one
// lump, GameThreadDispatch::Drain, each plugin callback) -- this fills in
// the lump by periodically suspending the game thread *while it is inside
// that call*, capturing its call stack, and resuming it, the same technique
// Visual Studio's sampling profiler / Very Sleepy / Superluminal use.
//
// Samples are merged into a call tree (root/outermost captured caller at
// the top, drilling down to the leaf), not a flat "one example per hotspot"
// list -- every count is real aggregated data across the whole window. Each
// node is resolved via a real PDB symbol if one covers it (see
// hooks/symbol_resolver.h -- this is what a user-supplied
// StarRupture-Win64-Shipping.pdb plugs into), else a native UFUNCTION's
// reflection name (function_symbols.h) if that matches instead, else plain
// Module+Offset as the last resort.
//
// Safety: the actual SuspendThread -> GetThreadContext -> unwind -> Resume
// sequence never touches the heap or takes any lock (hand-rolled unwind via
// RtlLookupFunctionEntry/RtlVirtualUnwind, writing into a fixed-size stack
// buffer) specifically so it can't deadlock against the suspended thread
// holding the CRT heap lock or the loader lock. All allocation (copying the
// captured frames into a vector, resolving module names) happens only after
// ResumeThread has been called.
//
// Opt-in and heavier than the base stutter detector: only runs when
// explicitly enabled (see EngineTick::SetStackSamplingEnabled), because it
// suspends/resumes the game thread at ~1kHz for the full duration of every
// profiled tick's engine call, not just when a stutter is later detected
// (there's no way to know in advance which tick will stutter).
// ---------------------------------------------------------------------------
namespace Hooks::EngineTick::StackSampler
{
	// Duplicates the calling thread's handle so the background sampler
	// thread can suspend/resume/read-context on it, and lazily starts that
	// thread. Must be called from the game thread; safe to call every tick
	// (no-op after the first call).
	void EnsureStarted();

	// Bracket the region of game-thread code to sample. Not reentrant --
	// call from the game thread only, and never nest a Begin/End pair inside
	// another. Begin clears samples left over from the previous window.
	void BeginWindow();
	void EndWindow();

	// Merges the raw samples captured during the last Begin/End window into
	// a call tree (root-first; see tick_profiler.h's CallTreeNode) --
	// consumer-side post-processing, does no new sampling. Call after
	// EndWindow(). maxChildrenPerNode caps branching at each level (sorted
	// by sampleCount descending first) so a pathologically bushy capture
	// can't blow up the UI.
	std::vector<TickProfiler::CallTreeNode> GetLastWindowCallTree(size_t maxChildrenPerNode = 10);

	// Total raw samples collected during the last window (denominator for
	// the % shown alongside each hotspot's sampleCount).
	size_t GetLastWindowSampleCount();
}
