#pragma once

// The image library's pump: once per engine tick it takes the next request
// from the table, finds or loads the texture, starts the read, and publishes
// the copy when the GPU has finished it.
//
// Everything that touches the engine or the GPU is behind Host, so the rules
// here (the budget per tick, what each read result does, eviction, device loss,
// shutdown) are testable with fakes. The loader's own Host is in
// image_pump_host.cpp.
//
// An item request waits in the pump until the item index knows its icon, and
// then becomes the image of the icon's texture path (Library::Alias).
//
// The sets (items, buildings): when a plugin uses a set and the loader's
// preload setting is on, the pump reads the set's list from the game, a slice
// per tick, and registers its images with the library. Preload then reads them
// in the background under the rules in the constants below.
//
// The disk cache (image_disk.h): the pump asks the source for every image first. An image with
// a file is read, checked and uploaded on the source's worker, never on the game thread, and is
// published like a shrunk copy. No game image is read and no package is loaded for it. Saved item
// names are tied to their texture paths when the folder is opened, and a saved set list fills the
// set without reading the game's list. While the folder is being listed the game side waits.
//
// Game thread only. The pump keeps no pointer into the engine: a texture is
// found, read and forgotten inside one Host::Read call.

#include "UI/image_disk.h"
#include "UI/image_library.h"
#include "UI/image_lookup.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ImagePump
{
	// ---- Constants ---------------------------------------------------------

	// Per tick. A read was measured at over 1 ms on the game thread at p95, so
	// one image goes per tick. Package loads are synchronous and cost far more
	// (15 to 35 ms at p95 in the menu), so at most one runs per tick: a texture's
	// package, or a slice of an item list, whichever comes first.
	inline constexpr uint32_t kReadsPerTick    = 1;
	inline constexpr uint32_t kLoadsPerTick    = 1;
	// Requests looked at per tick, whatever came of them. Keeps a queue of
	// images that are not readable yet from costing a tick.
	inline constexpr uint32_t kAttemptsPerTick = 8;

	inline constexpr uint64_t kSummaryIntervalMs = 60 * 1000;

	// A tick over this long names its slowest phase in the log, at most kSlowTickLogs times a session.
	inline constexpr uint64_t kSlowTickUs   = 50 * 1000;
	inline constexpr uint32_t kSlowTickLogs = 10;

	// Preload asks for up to this many images that have files on a tick it runs: a file costs the
	// game thread nothing but the request.
	inline constexpr uint32_t kDiskPreloadPerTick = 8;

	// Size-exact copies handed to the cache folder's worker on a tick: each is a queue push and no more.
	inline constexpr uint32_t kExactPerTick = 4;

	// ---- Preload -----------------------------------------------------------
	// Preload reads the members of the sets in use in the background. It runs
	// only while every rule below holds, and only on a tick that has no
	// on-demand read to make.

	// Preload is tried on one tick in this many: half the on-demand pace, since
	// even one read per tick was measured at over 1 ms on the game thread.
	inline constexpr uint32_t kPreloadTickInterval = 2;

	// The game releases the images the library made it load in sweeps, measured at
	// 44 to 50 seconds after the load, and some it kept for the whole run of five
	// minutes. The full-size images the library caused and the game has not
	// let go of, taken to be those loaded within the window, may not pass the
	// limit. An image counts at 4 bytes a pixel of its full size, which is more
	// than the game's compressed copy takes.
	inline constexpr uint64_t kPreloadHeldWindowMs   = 120 * 1000;
	inline constexpr uint64_t kPreloadHeldLimitBytes = 64ull * 1024 * 1024;

	// Preload waits while the adapter's local video memory in use is at or over
	// this share of its budget, checked at most once in this interval.
	inline constexpr uint32_t kPreloadVideoPercent  = 90;
	inline constexpr uint64_t kVideoCheckIntervalMs = 1000;

	// A texture that is loaded but not readable yet is asked again every tick for this long, and then
	// ends as NONE.
	inline constexpr uint64_t kNotYetGiveUpMs = 10 * 1000;

	// Item requests moved into the wait for the item index on one tick.
	inline constexpr uint32_t kItemsPerTick = 64;

	// ---- What the pump needs from the engine and the GPU ---------------------

	enum class ReadResult
	{
		Started,       // the copy is on the GPU; the Copy is filled in
		NotYet,        // loaded, but not readable yet: ask again on a later tick
		Busy,          // the read path is full: ask again, and not again this tick
		Failed,        // this texture cannot be copied
		Unavailable,   // no read can run on this build or this session
		Fault,         // an engine call faulted: nothing more is read from the game this session
		NotLoaded,     // no texture at that path is in memory
	};

	enum class LoadResult
	{
		Loaded,        // a texture is in memory now
		Missing,       // there is no texture at that path
		Fault,         // the load faulted
		Unsupported,   // this build cannot load by path
	};

	enum class CopyState { Pending, Done, Gone };

	enum class ShrinkResult
	{
		Started,   // the shrink is on the GPU; the source may be freed now
		Busy,      // no room to start it this tick: ask again
		Failed,    // the shrink cannot be made
	};

	// What a started shrink made: a texture slot of the texture API, at the stored
	// size, with the full chain of smaller levels.
	struct Shrunk
	{
		PluginTextureHandle texture  = nullptr;
		int                 width    = 0;
		int                 height   = 0;
		uint32_t            levels   = 0;
		uint32_t            readback = 0;   // 0: the stored copy is not read back
	};

	// What a started read made: a texture slot of the texture API, holding one
	// level of the game image.
	struct Copy
	{
		PluginTextureHandle texture = nullptr;
		int                 width   = 0;
		int                 height  = 0;
		int                 sourceWidth  = 0;   // level 0 of the game image
		int                 sourceHeight = 0;
		uint32_t            sourceFormat = 0;
	};

	class Host
	{
	public:
		virtual ~Host() = default;

		// The overlay's GPU side is up. False before the first frame, and again
		// once the device is lost or the overlay is shut down.
		virtual bool GpuReady() = 0;
		// The adapter's local video memory budget in bytes; 0 when unknown.
		virtual uint64_t LocalVideoBudget() = 0;
		// A steady clock, in microseconds.
		virtual uint64_t NowUs() = 0;
		// The loader setting for preloading images from the game. Read every tick,
		// so a change takes effect at once.
		virtual bool PreloadEnabled() = 0;
		// The adapter's local video memory in use and its budget, in bytes. False
		// when it cannot be read.
		virtual bool VideoMemory(uint64_t& usage, uint64_t& budget) = 0;

		// Finds the texture at an object path and starts reading one level of it
		// at the stored size. Never waits.
		virtual ReadResult Read(const std::string& objectPath, Copy& out) = 0;
		// Loads the texture at an object path into memory, and waits for it. The
		// one step an asynchronous load would replace: Read finds the result.
		virtual LoadResult Load(const std::string& objectPath) = 0;

		virtual CopyState StateOf(PluginTextureHandle texture) = 0;
		// The first game read is about to be asked for: lets the host get the shrink ready on a
		// thread of its own, so it is done by the time the first copy is. Never waits.
		virtual void PrepareShrink() {}
		// Draws a finished copy into a new texture at the stored size, with every smaller
		// level. Never waits. The source is kept alive on the GPU until the shrink has
		// run, so the caller frees it right after Started.
		virtual ShrinkResult Shrink(PluginTextureHandle source, Shrunk& out) = 0;
		// Frees a texture. The texture API holds it until the GPU is past every
		// frame that may still draw it.
		virtual void Free(PluginTextureHandle texture) = 0;
		virtual void Log(const std::string& line) = 0;
	};

	struct Stats
	{
		uint64_t published = 0;
		uint64_t evicted   = 0;
		uint64_t failed    = 0;
		uint64_t noImage   = 0;
		uint64_t fromDisk  = 0;   // published from a file
		uint64_t fromGame  = 0;   // published from a read of the game
		uint64_t exactMade = 0;   // size-exact copies made from stored files
		uint64_t written   = 0;   // files the saver's worker wrote; filled in when logged
		uint64_t rejected  = 0;   // files that failed a check; filled in when logged
		uint64_t longestTickUs = 0;   // since the last summary line
	};

	class Pump
	{
	public:
		Pump(ImageLibrary::Library& library, Host& host, ImageLookup::ItemSource& items, ImageLookup::ItemSource& buildings);

		// Where the stored copies are saved. None by default; the pump does not own it.
		void SetSaver(ImageDisk::Saver* saver) { m_saver = saver; }
		// Where the saved files are read from. None by default; the pump does not own it.
		void SetSource(ImageDisk::Source* source) { m_source = source; }

		// Once per engine tick, after the plugin callbacks.
		void Tick();

		// The engine is shutting down: every copy the library keeps is freed
		// while the texture API is still up. After this the pump does nothing.
		void Shutdown();

		const Stats& GetStats() const { return m_stats; }

	private:
		// A read that started and has not been published.
		struct InFlight
		{
			PluginImageId id = 0;
			Copy          copy;
			std::string   path;
		};

		// What a read becomes before it is published: the texture to keep and the bytes
		// its levels take.
		struct Stored
		{
			PluginTextureHandle texture = nullptr;
			uint64_t            bytes   = 0;
		};

		enum class StoreResult { Stored, Later, Failed };

		// The parts of a tick, timed one after the other.
		enum class Phase { Free, Collect, Save, Files, Serve, Items, Preload };
		static constexpr size_t kPhases = 7;

		// An item request the index cannot answer yet.
		struct HeldItem
		{
			PluginImageId id = 0;
			std::string   key;
		};

		enum class Outcome { Started, Disk, Done, Later, Busy };

		// Why preload did not read on the last tick it was tried.
		enum class Pause { Running, Off, NoSets, Listing, Screen, GameHolds, VideoMemory, Full, GameOff };

		// A full-size game image the library made the game load.
		struct HeldLoad
		{
			uint64_t atMs  = 0;
			uint64_t bytes = 0;
		};

		void    Start();
		void    GpuGone();
		void    LogSlowTick(uint64_t tickUs, const uint64_t (&phaseUs)[kPhases]);
		void    Collect();
		void    CollectFiles();
		void    CollectExact(const ImageDisk::FileResult& result);
		void    TendExact();
		bool    PollSource(uint64_t nowMs);
		void    ApplyLists(ImageDisk::Lists&& lists);
		bool    ApplyFileSet(uint32_t set, bool& listed, const char* what);
		bool    FileFor(const ImageLibrary::Request& request, std::string& key);
		bool    PublishStored(PluginImageId id, PluginTextureHandle texture, uint64_t bytes);
		bool    Serve(uint32_t& loads);
		void    StepItems(uint32_t& loads);
		void    StepIndex(ImageLookup::ItemIndex& index, const char* what);
		void    StepSets(uint32_t used, uint32_t& loads);
		void    StepSet(ImageLookup::ItemIndex& index, uint32_t set, bool& listed, uint32_t& loads, const char* what);
		void    Preload(bool idle, uint32_t& loads);
		void    GameReadsOff(const char* why);
		bool    GameHoldsTooMuch();
		void    PruneHeldLoads(uint64_t nowMs);
		bool    VideoFull();
		void    ResolveHeld();
		bool    Resolved(const HeldItem& item);
		void    FailQueued();
		void    FailHeld();
		Outcome Attempt(const ImageLibrary::Request& request, uint32_t& loads);
		StoreResult Store(const InFlight& read, Stored& out);
		void    FreeDeferred();
		void    LogSummary();

		ImageLibrary::Library& m_library;
		Host&                  m_host;
		ImageLookup::ItemIndex m_items;
		ImageLookup::ItemIndex m_buildings;
		ImageDisk::Saver*      m_saver = nullptr;
		ImageDisk::Source*     m_source = nullptr;
		bool                   m_sourceOff = false;   // the source gave up: the game is read for everything

		bool m_started = false;   // the GPU side was up at least once
		bool m_off     = false;   // the game is not read any more this session; files still are
		bool m_gone    = false;   // the GPU side went away: nothing may be freed
		bool m_stopped = false;
		bool m_shrinkPrepared = false;   // the host was told the first game read is coming
		uint32_t m_slowTickLogs = 0;

		std::vector<InFlight>                 m_inFlight;
		std::vector<ImageLibrary::Released>   m_scratch;
		std::vector<PluginImageId>            m_later;
		std::vector<HeldItem>                 m_held;
		// Requests the source is reading from a file, by image id.
		std::unordered_set<PluginImageId>     m_onDisk;
		std::unordered_set<PluginImageId>     m_preloadOnDisk;   // the ones of m_onDisk that preload asked for
		std::unordered_map<PluginImageId, uint64_t> m_notYetSince;   // first time a read was not ready, in ms
		bool                                  m_fullLogged = false;
		std::vector<ImageDisk::FileResult>    m_files;
		bool                                  m_listsTaken = false;
		uint32_t                              m_clearSeen = 0;   // the saver's clear count when the copies were last released
		ImageCache::SetList                   m_fileSets;   // saved set lists not used yet
		// Preload.
		bool                                  m_itemsListed     = false;
		bool                                  m_buildingsListed = false;
		uint32_t                              m_preloadTick     = 0;
		Pause                                 m_pause           = Pause::Off;
		std::vector<HeldLoad>                 m_heldLoads;
		std::unordered_set<PluginImageId>     m_causedLoad;     // loaded by us, copy not started yet
		uint64_t                              m_nextVideoCheckMs = 0;
		bool                                  m_videoFull        = false;
		// Evicted copies, freed at the start of the next tick: the frame that
		// may still draw them has ended by then.
		std::vector<PluginTextureHandle>      m_toFree;

		struct PreloadLogged
		{
			uint32_t queued = 0;
			uint32_t done   = 0;
			Pause    pause  = Pause::Off;
		};

		Stats         m_stats;
		Stats         m_lastLogged;
		PreloadLogged m_lastPreload;
		uint64_t m_nextSummaryMs = 0;
	};

	// ---- The loader's pump ---------------------------------------------------

	// Engine tick, after the plugin callbacks. Game thread.
	void Tick();
	// Engine shutdown, after the plugin callbacks and before the overlay goes. Game thread.
	void Shutdown();
}
