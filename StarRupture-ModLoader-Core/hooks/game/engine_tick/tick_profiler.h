#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// TickProfiler
//
// Stores per-tick timing data captured by the EngineTick hook's stutter
// detector (see engine_tick.cpp) so the client UI can show a live graph of
// recent tick times plus a detailed breakdown of every tick flagged as a
// stutter: time spent in the engine's own Tick() call, in
// GameThreadDispatch::Drain(), and in each registered plugin tick callback.
//
// Build-agnostic (server builds still record data, nothing reads it there --
// keeps the timing/storage logic in one place regardless of build target).
// All public functions are thread-safe: RecordTick() runs on the game
// thread, the snapshot getters are called from the render thread.
// ---------------------------------------------------------------------------
namespace Hooks::EngineTick::TickProfiler
{
	// How many recent ticks the live graph keeps (oldest dropped as new ones arrive).
	constexpr size_t kLiveBufferCapacity = 300;
	// How many stutter events are retained (oldest dropped as new ones arrive).
	constexpr size_t kMaxStutterEvents = 200;

	struct SegmentTiming
	{
		std::string name;
		double ms = 0.0;
	};

	struct TickSample
	{
		uint64_t tickIndex = 0;
		double totalMs = 0.0;
	};

	// One node of the merged call tree built from every stack sample taken
	// while a stutter tick's engine call was in flight -- root (outermost
	// captured caller) at the top, drilling down through real callers to
	// the leaf (innermost/hottest) function. Every sample that passed
	// through a node (at this depth or deeper) increments its sampleCount,
	// so this is real aggregated data across the whole window, not one
	// representative example. children is sorted by sampleCount descending.
	//
	// Resolved to the *containing function's* start address (via a real PDB
	// symbol if one covers it, else OS unwind metadata -- see
	// stack_sampler.cpp's ResolveIdentity), not the exact sampled PC, so
	// samples landing at different instructions inside the same function
	// merge into one node instead of fragmenting. symbolName is populated
	// from whichever resolved it (a PDB symbol, or -- lacking a PDB for
	// that module -- a native UFUNCTION's reflection name, see
	// function_symbols.h); empty means neither source covered it and
	// module+offset is all there is.
	struct CallTreeNode
	{
		uintptr_t functionStart = 0;
		std::string module; // e.g. "StarRupture-Win64-Shipping.exe", or "<unknown>"
		uintptr_t offset = 0; // functionStart - module base
		std::string symbolName; // resolved name if any, else empty
		int sampleCount = 0; // samples where this function was on the stack at or below this depth
		std::vector<CallTreeNode> children; // what this function called next, sorted by sampleCount desc
	};

	struct StutterEvent
	{
		uint64_t tickIndex = 0;
		double totalMs = 0.0;
		double avgMsAtTime = 0.0; // rolling average immediately before this tick
		float deltaSeconds = 0.0f;
		std::string timestamp; // "HH:MM:SS" wall clock at capture time
		std::vector<SegmentTiming> breakdown; // engine, dispatch, then per-callback

		// Stack-sampling results for the engine-tick portion (empty unless
		// stack sampling was enabled for this tick -- see engine_tick.h).
		// A forest, not a single tree: different samples can bottom out at
		// different outermost captured frames (call depth varies, or the
		// unwind ran out of frames/metadata at different points).
		std::vector<CallTreeNode> callTree;
		size_t totalSamples = 0; // total raw samples collected during the window
	};

	// Clears all recorded samples and stutter events. Called when (re-)entering
	// ChimeraMain so a new session starts with an empty history.
	void Reset();

	// Records one profiled tick. Always appended to the live ring buffer;
	// additionally stored (with a full breakdown) in the stutter list when
	// isStutter is true. engineMs/dispatchMs/callbackTimings/callTree are
	// only kept when isStutter is true -- pass whatever was measured either
	// way (callTree/totalSamples may be empty/0 if stack sampling was off).
	void RecordTick(uint64_t tickIndex, double totalMs, double avgMsAtTime, float deltaSeconds,
	                 bool isStutter, double engineMs, double dispatchMs,
	                 const std::vector<SegmentTiming>& callbackTimings,
	                 const std::vector<CallTreeNode>& callTree, size_t totalSamples);

	// Thread-safe snapshots for the UI to render. Return copies -- cheap
	// enough at UI framerates, avoids holding a lock across ImGui calls.
	std::vector<TickSample> GetLiveSamples();
	std::vector<StutterEvent> GetStutterEvents(); // oldest first
}
