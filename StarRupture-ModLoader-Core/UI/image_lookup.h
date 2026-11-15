#pragma once

// The image library's lookups: an item's unique name (or the name of its data
// asset) to the object path of its icon, a building's asset name to the object path of its icon, and a live
// texture to the object path the pump reads it under.
//
// Everything that touches the engine is behind ItemSource and TextureEngine, so
// the rules here (the item index and its budget per tick, the images that count
// as no image, the texture cache and its invalidation) are testable with
// fakes. The loader's own sources are in image_lookup_host.cpp.
//
// Game thread only. Nothing here keeps a pointer into the engine: an item is
// read, and a texture is looked at, inside one call.

#include "UI/image_library.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ImageLookup
{
	// ---- Constants ---------------------------------------------------------

	// The most time one tick spends on the item index. Loading an item's
	// package is the cost: measured at 15 to 35 ms at p95 in the menu, and
	// well under a millisecond in a world. At least one item is read per tick,
	// so a single slow package can run over once.
	inline constexpr uint64_t kItemBudgetUs = 3000;

	// The most textures FromTexture remembers. When it is full the memory is
	// dropped and builds up again: a remembered texture costs one engine path
	// lookup to ask again.
	inline constexpr size_t kTextureCacheMax = 4096;

	// ---- No image ----------------------------------------------------------

	// The engine's own stand-in textures, which an unfinished item or building
	// points its icon at. Drawn, they are a white square, so they are no image.
	// Takes an object path in any form the library accepts.
	bool IsPlaceholderImage(const char* texturePath);

	// ---- Items ---------------------------------------------------------------

	enum class ItemRead
	{
		Icon,          // the item has a texture icon: name and texturePath are filled in
		NoIcon,        // the item has no icon, or an icon that is not a texture: name is filled in
		Skipped,       // this one could not be read; it cannot be asked for
		Unavailable,   // items cannot be read on this build or this session
	};

	struct ItemRecord
	{
		std::string name;          // the item's unique name; "None" or empty when it was never set
		std::string asset;         // the name of its data asset ("I_IronPlate")
		std::string texturePath;   // the object path of its icon texture
	};

	// What the index needs from the engine: a list of assets from the asset
	// registry (the item blueprints, or the building data), and the name and icon
	// of each. For a building the name is its asset name.
	class ItemSource
	{
	public:
		virtual ~ItemSource() = default;

		// Makes the list. Once, before anything is read. False when it cannot
		// be made here.
		virtual bool List() = 0;
		virtual size_t Count() const = 0;
		// Reads one item of the list. Loads its package when it is not in
		// memory, and waits for it.
		virtual ItemRead Read(size_t index, ItemRecord& out) = 0;
	};

	// The items (or buildings) read so far, by the key NormalizeItemName gives:
	// under the unique name when the item has one, and under the name of its data
	// asset. Filled a slice at a time.
	class ItemIndex
	{
	public:
		enum class State { Idle, Scanning, Done, Failed };

		explicit ItemIndex(ItemSource& source);

		State GetState() const { return m_state; }

		// Reads items until budgetUs has passed on the clock, and at least one.
		// The first call makes the list. Does nothing once Done or Failed.
		void Step(uint64_t budgetUs, const std::function<uint64_t()>& nowUs);

		// Whether the item is known. The path is empty when it has no image to
		// show. The key is a unique name, or an asset name: "I_X", the class name
		// "I_X_C" or the default object's name "Default__I_X_C". A unique name wins
		// over an asset name, and the first item read under a unique name is the
		// one that is kept. An asset name is answered once the index is Done, since
		// until then a unique name of an item not read yet could still claim it. An
		// unset name ("None", empty) is never found.
		bool Find(const std::string& key, std::string& texturePath) const;

		// The icon textures known, each once and sorted. What has no image to show,
		// or only a stand-in, is left out.
		std::vector<std::string> TexturePaths() const;

		// How many items read so far have no unique name, how many unique names were
		// met on more than one item, and how many asset names.
		size_t UnnamedCount() const { return m_unnamed; }
		size_t RepeatedNameCount() const { return m_repeated.size(); }
		size_t RepeatedAssetCount() const { return m_repeatedAssets.size(); }

	private:
		ItemSource&                                  m_source;
		State                                        m_state = State::Idle;
		size_t                                       m_next  = 0;
		size_t                                       m_unnamed = 0;
		std::unordered_map<std::string, std::string> m_items;    // by unique name
		std::unordered_map<std::string, std::string> m_assets;   // by asset name
		std::unordered_set<std::string>              m_repeated;
		std::unordered_set<std::string>              m_repeatedAssets;
	};

	// ---- Textures ------------------------------------------------------------

	// Where a live texture sits in the engine's object table. The serial is
	// different for every object that has used the slot, so a slot that was
	// reused for another object does not match.
	struct TextureSlot
	{
		int32_t index  = -1;
		int32_t serial = 0;
	};

	class TextureEngine
	{
	public:
		virtual ~TextureEngine() = default;

		virtual bool OnGameThread() = 0;
		// The slot of the texture at this address. False when the address is
		// not a live texture in its slot.
		virtual bool SlotOf(const void* texture, TextureSlot& out) = 0;
		// The object path of the texture ("/Game/UI/T_X.T_X"). False when the
		// engine has none.
		virtual bool PathOf(const void* texture, std::string& out) = 0;
		virtual void Log(const std::string& line) = 0;
	};

	// A live texture to its image. The answer for a texture is remembered under
	// its slot and serial, and the texture itself is not kept.
	class TextureLookup
	{
	public:
		TextureLookup(TextureEngine& engine, ImageLibrary::Library& library);

		// 0 when called from another thread than the game's (logged once), when
		// the address is not a live texture, or when it has no path.
		PluginImageId FromTexture(const void* texture);

	private:
		TextureEngine&         m_engine;
		ImageLibrary::Library& m_library;
		bool                   m_loggedThread = false;
		std::unordered_map<uint64_t, PluginImageId> m_known;
	};
}
