#pragma once

// The image library's table: which game images plugins have asked for, their
// state, what is kept in memory and in what order the rest are read.
//
// No engine and no GPU here. The pump that reads images, and the code that
// turns an item name into a texture path, call the members marked "pump" below
// and hand back what they made. That keeps every rule here testable with fakes.
//
// Threads. Item, Path, Get, State, UseSets and ForgetPlugin can be called from
// any thread. Get, State and Canonical take no lock: they read per-image atomics
// from a table that never moves. Item, Path and UseSets take the table's mutex
// briefly, and so does everything the pump calls, so a plugin that asks for an id
// every frame should ask once and keep it: ids stay the same for the session.

#include "plugins/plugin_interface.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ImageLibrary
{
	// ---- Constants ----------------------------------------------------------

	// The one size an image is stored at, in pixels.
	inline constexpr uint32_t kStoredSize = 256;

	// The most memory the stored copies may take, and the share of the video
	// memory budget that may be spent on them if that is smaller.
	inline constexpr uint64_t kByteCapMax          = 256ull * 1024 * 1024;
	inline constexpr uint64_t kByteCapBudgetPercent = 4;

	// The most images kept at once. The texture table the copies go into holds
	// 4096 slots for every plugin together.
	inline constexpr uint32_t kCountCap = 2048;

	// How long after it was last drawn (Get) an image counts as seen lately, in
	// milliseconds. A request that has not been seen lately is dropped, unless
	// the image belongs to a set a plugin uses.
	inline constexpr uint64_t kSeenLatelyMs = 500;

	// ---- Size-exact copies ---------------------------------------------------
	// A copy of the stored picture made for one pixel size (the longer side), drawn 1:1. It is
	// made only for a size asked for steadily: asked again and again over kExactSteadyMs, with no
	// gap over kExactGapMs between two asks. A window being dragged or a scale slider being
	// moved keeps changing the size and never qualifies.
	inline constexpr uint64_t kExactSteadyMs = 400;
	inline constexpr uint64_t kExactGapMs    = 200;

	// A copy not drawn for this long is dropped. The sweep that does it runs once in kExactSweepMs.
	inline constexpr uint64_t kExactIdleMs  = 30 * 1000;
	inline constexpr uint64_t kExactSweepMs = 1000;

	// A size whose copy could not be made is not asked for again for this long.
	inline constexpr uint64_t kExactRetryMs = 30 * 1000;

	// A size whose copy was refused for room is offered again no sooner than this.
	inline constexpr uint64_t kExactRefusedMs = 2000;

	// The most copies one image keeps, and the most kept or being made for all images together.
	// Their bytes count against the byte cap with the stored copies.
	inline constexpr uint32_t kExactPerImage = 3;
	inline constexpr uint32_t kExactCountCap = 256;

	// ---- Table geometry ---------------------------------------------------
	// Ids index chunks of images that are allocated once and never move, so a
	// reader finds an image without a lock. A plugin that asks for more
	// different images than this gets id 0.
	inline constexpr uint32_t kIdsPerChunk = 512;
	inline constexpr uint32_t kMaxChunks   = 128;
	inline constexpr uint32_t kMaxIds      = kIdsPerChunk * kMaxChunks;

	// min(kByteCapMax, a share of the adapter's local video memory budget). A
	// budget of 0 (unknown) gives kByteCapMax.
	uint64_t ByteCapForBudget(uint64_t localBudgetBytes);

	// The key an item name is looked up under: trimmed, ASCII lower case.
	// Empty when there is nothing to look up.
	std::string NormalizeItemName(const char* name);

	// Whether a key from NormalizeItemName is no item name: empty, or "none", which is
	// what the game prints for a name that was never set. Such a key names no item.
	bool IsUnsetItemKey(const std::string& key);

	// The key a texture object path is looked up under. Accepts the object path
	// ("/Game/UI/T_X.T_X"), the package path alone ("/Game/UI/T_X"), and the
	// reference form ("Texture2D'/Game/UI/T_X.T_X'"); the package path is
	// completed to an object path, and the case is folded. Empty when it is not
	// a path.
	std::string NormalizeObjectPath(const char* path);

	enum class Kind : uint8_t { Item, Path };

	// What the pump is handed to read.
	struct Request
	{
		PluginImageId id   = 0;
		Kind          kind = Kind::Item;
		std::string   name;   // as first asked for, trimmed, case kept
	};

	// A stored copy that left the table. The pump frees the texture.
	struct Released
	{
		PluginImageId       id     = 0;
		PluginTextureHandle handle = nullptr;
		uint64_t            bytes  = 0;
		bool                exact  = false;   // a size-exact copy; id is the image it was made from
	};

	// A size-exact copy the pump is to have made.
	struct ExactRequest
	{
		PluginImageId id   = 0;
		uint32_t      size = 0;   // the longer side, in pixels
		std::string   name;       // the image's texture path, as first asked for
	};

	// How a request for a copy ended without one.
	enum class ExactEnd
	{
		Retry,       // could not be started now: it is offered again
		Failed,      // the copy could not be made: the size is not asked for again for kExactRetryMs
		NotNeeded,   // no copy is wanted or possible (the stored picture is no larger than the size, or has no file): the stored picture is what is drawn
	};

	// An image that belongs to sets, by the object path it is read from.
	struct SetMember
	{
		uint32_t    sets = 0;   // PluginImageSet bits
		std::string path;
	};

	class Library
	{
	public:
		Library();
		~Library();
		Library(const Library&) = delete;
		Library& operator=(const Library&) = delete;

		// ---- Plugin side ---------------------------------------------------

		// Any thread. The same name or path always gives the same id. Only hands
		// out the id: the image is asked for by Get, not by these. 0 for an empty
		// name or when the table is full.
		PluginImageId Item(const char* uniqueItemName);
		PluginImageId Path(const char* textureObjectPath);

		// Any thread, no lock. The one id of the image id names: the id of the
		// texture path for an item once the item has been resolved to its
		// texture, id itself before that and for any id that is not an alias
		// (a path id, 0, an unknown id).
		PluginImageId Canonical(PluginImageId id) const;

		// Any thread, no lock. Marks the image as seen, and asks for it again
		// if it is not kept (evicted, or its request was dropped).
		PluginTextureHandle Get(PluginImageId id);

		// Any thread, no lock. Get for an image drawn at pixelSize pixels on its longer side. Once a
		// copy made for exactly that size is ready (see the constants above) it is returned, and
		// *width and *height (either may be null) are the copy's size: draw it 1:1. Until then, and
		// for a size that is 0, negative or not under kStoredSize, the answer is what Get gives and
		// *width and *height are 0. Asking marks the image seen, and asks for the copy once the size
		// has been asked for steadily.
		PluginTextureHandle GetSized(PluginImageId id, int pixelSize, int* width, int* height);

		// Any thread, no lock. A PluginImageState; does not mark the image seen.
		int State(PluginImageId id) const;

		// Any thread. The sets this plugin draws from; 0 clears.
		void UseSets(const IPluginSelf* self, uint32_t sets);

		// The plugin manager, before the plugin's module is freed.
		void ForgetPlugin(const IPluginSelf* self);

		// ---- Pump side -----------------------------------------------------

		// The time stamps of Get and of new requests are taken from this, in
		// milliseconds. Once per tick, before TakeNext.
		void SetNow(uint64_t nowMs);

		// The next image to read, or false. The most recently seen first;
		// requests not seen lately and outside every used set are dropped on
		// the way. The image stays WAITING until one of the four calls below.
		bool TakeNext(Request& out);

		// How many more images, up to wanted, can be kept: the room under both caps,
		// plus the kept images that were not seen lately and can be evicted for a
		// newcomer. The pump takes no request that has no room, so a screen that
		// draws more images than the caps hold keeps what it has and the rest wait.
		uint32_t RoomFor(uint32_t wanted) const;

		// Whether any request is queued (WAITING and not taken yet).
		bool HasQueued();

		// The read finished. Takes the copy and its size into the table, then
		// evicts the images seen least recently until the caps hold again;
		// those are appended to evicted for the pump to free. An image seen lately
		// is never evicted, so the caps can be passed by the images in flight.
		// Returns whether
		// the image is READY now. False means the pump frees handle itself:
		// the copy alone is over the byte cap, or the image was no longer
		// waiting for it.
		bool Publish(PluginImageId id, PluginTextureHandle handle, uint64_t bytes,
			std::vector<Released>& evicted);

		// The image resolved and has no image to show.
		void PublishNoImage(PluginImageId id);

		// The read failed for good. The image stays NONE until the session ends.
		void Fail(PluginImageId id);

		// The read cannot be done now (not yet resident, the pump is busy).
		// The image goes back in the queue, still WAITING.
		void Requeue(PluginImageId id);

		// The image whose texture path the item lookup found is the same game
		// image as the one Path(thatPath) names: from then on both ids are
		// that one image, and it is read and kept once. Refused (false) when
		// from has been read already or is being read.
		bool Alias(PluginImageId from, PluginImageId to);

		// Adds the image to the sets (PluginImageSet bits). Requests for an
		// image in a set some plugin uses are not dropped for being unseen.
		void AddToSets(PluginImageId id, uint32_t sets);

		// ---- Size-exact copies (pump) ---------------------------------------------

		// The next copy to make, or false. It stays pending until PublishExact or ExactEnded.
		bool TakeExact(ExactRequest& out);

		// The copy for (id, size) was made and uploaded: width x height pixels, bytes of them. It
		// counts against the byte cap, and older copies and stored images leave as Trim says; those
		// are appended to dropped for the pump to free. Returns whether the copy is kept. False
		// means the pump frees handle itself, unless it is among dropped (the image it belongs to
		// was evicted in the same call).
		bool PublishExact(PluginImageId id, uint32_t size, PluginTextureHandle handle, uint32_t width, uint32_t height,
			uint64_t bytes, std::vector<Released>& dropped);

		// The request for (id, size) ended without a copy.
		void ExactEnded(PluginImageId id, uint32_t size, ExactEnd how);

		// Copies not drawn for kExactIdleMs leave the table, at most once in kExactSweepMs.
		void ReapExact(std::vector<Released>& dropped);

		// Every copy and every pending request leaves the table (the cache was cleared). The stored
		// pictures stay.
		void ReleaseExact(std::vector<Released>& dropped);

		// Copies kept or being made.
		uint32_t ExactCount() const;

		// ---- Sets and preload (pump) -----------------------------------------

		// The union of the sets the plugins use now.
		uint32_t UsedSets() const;

		// A member of the sets: the image at a texture path, in the sets. It waits
		// idle until preload reaches it or a plugin draws it. The same path always
		// gives the same id as Path.
		PluginImageId PathInSet(const char* textureObjectPath, uint32_t sets);

		// The next member of a used set that is not kept and not asked for, in the
		// order the members were added and round the list, or false. It becomes
		// WAITING and taken, and is answered like a request from TakeNext. An
		// image no one has drawn is stamped as never seen, so it is the first to
		// be evicted.
		bool TakePreload(Request& out);

		// A preload read that cannot be done now. The image goes back to idle, or
		// into the queue if something has drawn it since.
		void Defer(PluginImageId id);

		// Whether any image drawn lately is WAITING (queued, being read, or held).
		bool SeenWaiting() const;

		// Whether preload may add another image: no image has had to be evicted
		// yet, and one more full image fits under both caps.
		bool PreloadHasRoom() const;

		struct PreloadProgress
		{
			uint32_t queued = 0;   // members of used sets not kept yet
			uint32_t done   = 0;   // members of used sets that are READY or have no image
		};
		PreloadProgress GetPreloadProgress() const;

		// Every kept copy leaves the table, for the pump to free or drop (the
		// loader is shutting down, or the device is gone). Those images read as
		// NONE from then on. Appended to released.
		void ReleaseAll(std::vector<Released>& released);

		// ---- Limits and accounting ----------------------------------------

		// Appends every texture path that is in a set, with its set bits.
		void CollectSets(std::vector<SetMember>& out) const;

		void SetCaps(uint64_t byteCap, uint32_t countCap);
		uint64_t BytesKept() const;
		uint32_t CountKept() const;
		uint32_t ImageCount() const;   // ids handed out

	private:
		enum : int32_t
		{
			kIdle    = 0,   // not kept, not asked for: Get asks
			kWaiting = 1,
			kReady   = 2,
			kNoImage = 3,
			kFailed  = 4,   // reads as NONE, and Get does not ask again
		};

		enum : int32_t
		{
			kExactFree    = 0,
			kExactPending = 1,   // asked for, being made
			kExactReady   = 2,
			kExactClosed  = 3,   // could not be made: asked for again after closedUntil
		};

		// One size-exact copy of an image. Read without the lock, except for the last two.
		struct ExactSlot
		{
			std::atomic<uint32_t>            size{ 0 };
			std::atomic<int32_t>             state{ kExactFree };
			std::atomic<PluginTextureHandle> handle{ nullptr };
			std::atomic<uint32_t>            width{ 0 };
			std::atomic<uint32_t>            height{ 0 };
			std::atomic<uint64_t>            lastDrawn{ 0 };
			std::atomic<uint64_t>            closedUntil{ 0 };
			uint64_t                         bytes = 0;      // under the lock
			bool                             taken = false;  // handed to the pump, not answered yet
		};

		// A size lately asked for that has no copy: since when, and last asked.
		struct ExactTrial
		{
			std::atomic<uint32_t> size{ 0 };
			std::atomic<uint64_t> first{ 0 };
			std::atomic<uint64_t> last{ 0 };
		};

		struct Entry
		{
			// Read without the lock.
			std::atomic<int32_t>             state{ kIdle };
			std::atomic<PluginTextureHandle> handle{ nullptr };
			std::atomic<uint64_t>            lastSeen{ 0 };
			std::atomic<PluginImageId>       target{ 0 };   // the image this id is an alias of
			ExactSlot                        exact[kExactPerImage];
			ExactTrial                       trial[kExactPerImage];

			// Under the lock.
			Kind        kind = Kind::Item;
			std::string name;
			uint32_t    sets   = 0;
			uint64_t    bytes  = 0;
			bool        queued = false;
			bool        taken  = false;   // handed to the pump, not answered yet
		};

		struct Chunk
		{
			Entry entries[kIdsPerChunk];
		};

		Entry*       Find(PluginImageId id);
		const Entry* Find(PluginImageId id) const;
		Entry*       Resolve(Entry* e);
		const Entry* Resolve(const Entry* e) const;
		PluginImageId IdFor(Kind kind, const std::string& key, const char* name);
		void  Ask(Entry& e, PluginImageId id);   // lock held
		void  Drain();                           // lock held
		void  Trim(std::vector<Released>& evicted);   // lock held
		bool  InSetUse(const Entry& e) const;    // lock held
		uint32_t UsedSetsLocked() const;         // lock held
		void  TagSets(Entry& e, PluginImageId id, uint32_t sets);   // lock held
		static bool SeenLately(const Entry& e, uint64_t now);
		void  AskExact(Entry& e, PluginImageId id, uint32_t size);         // not under the lock
		bool  OfferExact(Entry& e, PluginImageId id, uint32_t size);       // lock held; whether the copy was queued
		void  DropExact(Entry& e, PluginImageId id, std::vector<Released>& dropped);   // lock held
		struct ExactRef { PluginImageId id; uint32_t slot; uint64_t lastDrawn; };
		std::vector<ExactRef> IdleExact(uint64_t idleMs) const;            // lock held; least recently drawn first
		void  DropSlot(ExactSlot& slot, std::vector<Released>& dropped, PluginImageId id);   // lock held

		std::atomic<Chunk*>               m_chunks[kMaxChunks];
		std::atomic<uint32_t>             m_count{ 0 };
		std::atomic<uint64_t>             m_now{ 0 };
		std::atomic<bool>                 m_askPending{ false };

		mutable std::mutex                m_lock;
		std::unordered_map<std::string, PluginImageId> m_ids;
		std::vector<PluginImageId>        m_queue;
		std::vector<PluginImageId>        m_setMembers;   // images tagged with a set, in the order tagged
		size_t                            m_preloadCursor = 0;
		bool                              m_capped = false;   // an image has been evicted for room
		std::unordered_map<const IPluginSelf*, uint32_t> m_used;
		uint64_t                          m_byteCap  = kByteCapMax;
		uint32_t                          m_countCap = kCountCap;
		uint64_t                          m_bytes    = 0;
		uint32_t                          m_kept     = 0;
		std::deque<std::pair<PluginImageId, uint32_t>> m_exactQueue;   // image and size, oldest first
		uint32_t                          m_exactCount = 0;   // slots pending or ready
		uint64_t                          m_nextSweepMs = 0;
		std::atomic<uint32_t>             m_exactWaiting{ 0 };   // offers refused at the count cap since the last sweep
		std::vector<Released>             m_exactDropped;   // copies that left for room, until the pump takes them
	};

	// The loader's table. Handed to plugins as hooks->Images.
	Library& Instance();
	IPluginImages* GetInterface();

	// What FromTexture calls: a texture to its image. Set once by the code that
	// can read the engine; until then FromTexture gives 0.
	using FromTextureFn = PluginImageId (*)(SDK::UTexture2D* texture);
	void SetFromTexture(FromTextureFn fn);

	// Drop what a plugin's set interest recorded. Plugin manager, before FreeLibrary.
	void ForgetPlugin(const IPluginSelf* self);
}
