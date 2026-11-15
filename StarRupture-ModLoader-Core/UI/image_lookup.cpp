#include "UI/image_lookup.h"

#include <algorithm>
#include <set>

namespace ImageLookup
{
	bool IsPlaceholderImage(const char* texturePath)
	{
		// The key is an object path in lower case, so the asset is what follows the last dot.
		static const char* const kPlaceholders[] = { "whitesquaretexture", "defaultwhitegrid", "whiteplaceholder", "t_noitem_white_icon" };

		const std::string key = ImageLibrary::NormalizeObjectPath(texturePath);
		const size_t dot = key.rfind('.');
		if (dot == std::string::npos)
			return false;
		const std::string asset = key.substr(dot + 1);
		return std::any_of(std::begin(kPlaceholders), std::end(kPlaceholders),
			[&](const char* placeholder) { return asset == placeholder; });
	}

	// ---- Items -----------------------------------------------------------------

	namespace
	{
		// The asset name inside the name of the item's class ("i_x_c") or of its default
		// object ("default__i_x_c"). A key in lower case, as NormalizeItemName gives it.
		std::string AssetNameOf(std::string key)
		{
			static const std::string kDefaultPrefix = "default__";
			static const std::string kClassSuffix = "_c";

			if (key.compare(0, kDefaultPrefix.size(), kDefaultPrefix) == 0)
				key.erase(0, kDefaultPrefix.size());
			if (key.size() > kClassSuffix.size() && key.compare(key.size() - kClassSuffix.size(), kClassSuffix.size(), kClassSuffix) == 0)
				key.erase(key.size() - kClassSuffix.size());
			return key;
		}
	}

	ItemIndex::ItemIndex(ItemSource& source)
		: m_source(source)
	{
	}

	void ItemIndex::Step(uint64_t budgetUs, const std::function<uint64_t()>& nowUs)
	{
		if (m_state == State::Done || m_state == State::Failed)
			return;

		const uint64_t begin = nowUs();
		if (m_state == State::Idle)
		{
			if (!m_source.List())
			{
				m_state = State::Failed;
				return;
			}
			m_state = State::Scanning;
		}

		do
		{
			if (m_next >= m_source.Count())
			{
				m_state = State::Done;
				return;
			}

			ItemRecord record;
			const ItemRead read = m_source.Read(m_next++, record);
			if (read == ItemRead::Unavailable)
			{
				m_state = State::Failed;
				return;
			}
			if (read == ItemRead::Skipped)
				continue;

			// An item with no icon is known all the same: it has no image to show. An item
			// with no unique name is known by its asset name only.
			std::string path = read == ItemRead::Icon ? std::move(record.texturePath) : std::string();
			const std::string key = ImageLibrary::NormalizeItemName(record.name.c_str());
			if (ImageLibrary::IsUnsetItemKey(key))
				++m_unnamed;
			else if (!m_items.emplace(key, path).second)
				m_repeated.insert(key);

			const std::string asset = ImageLibrary::NormalizeItemName(record.asset.c_str());
			if (!ImageLibrary::IsUnsetItemKey(asset) && !m_assets.emplace(asset, std::move(path)).second)
				m_repeatedAssets.insert(asset);
		}
		while (nowUs() - begin < budgetUs);

		if (m_next >= m_source.Count())
			m_state = State::Done;
	}

	bool ItemIndex::Find(const std::string& key, std::string& texturePath) const
	{
		if (ImageLibrary::IsUnsetItemKey(key))
			return false;

		// A unique name first, so it wins over the asset name of another item. The first
		// item read under a unique name keeps it, so that answer is final at once.
		auto it = m_items.find(key);
		if (it == m_items.end())
		{
			// An asset name answers only when every item has been read: a unique name that
			// belongs to an item not read yet would still win over it.
			if (m_state != State::Done)
				return false;
			it = m_assets.find(key);
			if (it == m_assets.end())
				it = m_assets.find(AssetNameOf(key));
			if (it == m_assets.end())
				return false;
		}
		texturePath = it->second;
		return true;
	}

	std::vector<std::string> ItemIndex::TexturePaths() const
	{
		std::set<std::string> paths;
		for (const auto* known : { &m_items, &m_assets })
			for (const auto& item : *known)
				if (!item.second.empty() && !IsPlaceholderImage(item.second.c_str()))
					paths.insert(item.second);
		return std::vector<std::string>(paths.begin(), paths.end());
	}

	// ---- Textures --------------------------------------------------------------

	TextureLookup::TextureLookup(TextureEngine& engine, ImageLibrary::Library& library)
		: m_engine(engine), m_library(library)
	{
	}

	PluginImageId TextureLookup::FromTexture(const void* texture)
	{
		if (!texture)
			return 0;
		if (!m_engine.OnGameThread())
		{
			if (!m_loggedThread)
			{
				m_loggedThread = true;
				m_engine.Log("[Images] FromTexture was called from a thread other than the game's: it gives 0 there");
			}
			return 0;
		}

		TextureSlot slot;
		if (!m_engine.SlotOf(texture, slot))
			return 0;
		const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(slot.index)) << 32) | static_cast<uint32_t>(slot.serial);
		const auto known = m_known.find(key);
		if (known != m_known.end())
			return known->second;

		std::string path;
		if (!m_engine.PathOf(texture, path))
			return 0;
		const PluginImageId id = m_library.Path(path.c_str());
		if (id == 0)
			return 0;

		if (m_known.size() >= kTextureCacheMax)
			m_known.clear();
		m_known.emplace(key, id);
		return id;
	}
}
