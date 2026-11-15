#include "UI/image_library.h"
#include "UI/image_shrink.h"

#include <algorithm>
#include <cstring>

namespace ImageLibrary
{
	namespace
	{
		constexpr size_t   kMaxNameLength = 256;
		constexpr size_t   kMaxPathLength = 1024;
		constexpr uint32_t kKnownSets     = PLUGIN_IMAGE_SET_ITEMS | PLUGIN_IMAGE_SET_BUILDINGS;

		bool IsSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n';
		}

		char Lower(char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}

		std::string Trimmed(const char* s)
		{
			if (!s)
				return {};
			size_t begin = 0;
			size_t end = strlen(s);
			while (begin < end && IsSpace(s[begin]))
				++begin;
			while (end > begin && IsSpace(s[end - 1]))
				--end;
			return std::string(s + begin, end - begin);
		}

		bool HasControlChars(const std::string& s)
		{
			return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
		}

		void FoldCase(std::string& s)
		{
			for (char& c : s)
				c = Lower(c);
		}

		std::string CanonicalName(const char* name)
		{
			std::string s = Trimmed(name);
			if (s.size() > kMaxNameLength || HasControlChars(s))
				return {};
			return s;
		}

		// "/Game/UI/T_X" and "Texture2D'/Game/UI/T_X.T_X'" both become
		// "/Game/UI/T_X.T_X". Case is kept.
		std::string CanonicalPath(const char* path)
		{
			std::string s = Trimmed(path);

			const size_t open = s.find('\'');
			if (open != std::string::npos)
			{
				const size_t close = s.rfind('\'');
				if (close <= open)
					return {};
				s = s.substr(open + 1, close - open - 1);
			}

			if (s.size() < 2 || s.size() > kMaxPathLength || s[0] != '/' || HasControlChars(s))
				return {};

			const size_t slash = s.rfind('/');
			if (slash + 1 >= s.size())
				return {};
			if (s.find('.', slash) == std::string::npos)
				s += '.' + s.substr(slash + 1);
			return s;
		}

		int PublicState(int32_t state)
		{
			switch (state)
			{
			case 1:  return PLUGIN_IMAGE_WAITING;
			case 2:  return PLUGIN_IMAGE_READY;
			case 3:  return PLUGIN_IMAGE_NO_IMAGE;
			default: return PLUGIN_IMAGE_NONE;
			}
		}
	}

	uint64_t ByteCapForBudget(uint64_t localBudgetBytes)
	{
		if (localBudgetBytes == 0)
			return kByteCapMax;
		return (std::min)(kByteCapMax, localBudgetBytes / 100 * kByteCapBudgetPercent);
	}

	std::string NormalizeItemName(const char* name)
	{
		std::string s = CanonicalName(name);
		FoldCase(s);
		return s;
	}

	bool IsUnsetItemKey(const std::string& key)
	{
		return key.empty() || key == "none";
	}

	std::string NormalizeObjectPath(const char* path)
	{
		std::string s = CanonicalPath(path);
		FoldCase(s);
		return s;
	}

	Library::Library()
	{
		for (auto& chunk : m_chunks)
			chunk.store(nullptr, std::memory_order_relaxed);
	}

	Library::~Library()
	{
		for (auto& chunk : m_chunks)
			delete chunk.load(std::memory_order_relaxed);
	}

	Library::Entry* Library::Find(PluginImageId id)
	{
		if (id == 0 || id > m_count.load(std::memory_order_acquire))
			return nullptr;
		const uint32_t index = id - 1;
		Chunk* chunk = m_chunks[index / kIdsPerChunk].load(std::memory_order_acquire);
		return chunk ? &chunk->entries[index % kIdsPerChunk] : nullptr;
	}

	const Library::Entry* Library::Find(PluginImageId id) const
	{
		return const_cast<Library*>(this)->Find(id);
	}

	Library::Entry* Library::Resolve(Entry* e)
	{
		const PluginImageId target = e->target.load(std::memory_order_acquire);
		return target ? Find(target) : e;
	}

	const Library::Entry* Library::Resolve(const Entry* e) const
	{
		return const_cast<Library*>(this)->Resolve(const_cast<Entry*>(e));
	}

	PluginImageId Library::IdFor(Kind kind, const std::string& key, const char* name)
	{
		if (key.empty())
			return 0;

		std::lock_guard<std::mutex> lock(m_lock);

		const std::string mapKey = (kind == Kind::Item ? "i:" : "p:") + key;
		const auto found = m_ids.find(mapKey);
		if (found != m_ids.end())
			return found->second;

		const uint32_t index = m_count.load(std::memory_order_relaxed);
		if (index >= kMaxIds)
			return 0;
		Chunk* chunk = m_chunks[index / kIdsPerChunk].load(std::memory_order_relaxed);
		if (!chunk)
		{
			chunk = new Chunk();
			m_chunks[index / kIdsPerChunk].store(chunk, std::memory_order_release);
		}

		const PluginImageId id = index + 1;
		Entry& e = chunk->entries[index % kIdsPerChunk];
		e.kind = kind;
		e.name = kind == Kind::Item ? CanonicalName(name) : CanonicalPath(name);
		e.lastSeen.store(0, std::memory_order_relaxed);
		e.state.store(kIdle, std::memory_order_relaxed);
		e.queued = false;
		m_ids.emplace(mapKey, id);
		m_count.store(id, std::memory_order_release);
		return id;
	}

	PluginImageId Library::Item(const char* uniqueItemName)
	{
		return IdFor(Kind::Item, NormalizeItemName(uniqueItemName), uniqueItemName);
	}

	PluginImageId Library::Path(const char* textureObjectPath)
	{
		return IdFor(Kind::Path, NormalizeObjectPath(textureObjectPath), textureObjectPath);
	}

	PluginImageId Library::Canonical(PluginImageId id) const
	{
		const Entry* e = Find(id);
		if (!e)
			return id;
		const PluginImageId target = e->target.load(std::memory_order_acquire);
		return target ? target : id;
	}

	PluginTextureHandle Library::Get(PluginImageId id)
	{
		Entry* e = Find(id);
		if (!e)
			return nullptr;
		e = Resolve(e);
		if (!e)
			return nullptr;

		e->lastSeen.store(m_now.load(std::memory_order_relaxed), std::memory_order_relaxed);

		int32_t state = e->state.load(std::memory_order_acquire);
		if (state == kReady)
			return e->handle.load(std::memory_order_acquire);
		if (state == kIdle && e->state.compare_exchange_strong(state, kWaiting, std::memory_order_acq_rel))
			m_askPending.store(true, std::memory_order_release);
		return nullptr;
	}

	PluginTextureHandle Library::GetSized(PluginImageId id, int pixelSize, int* width, int* height)
	{
		if (width)
			*width = 0;
		if (height)
			*height = 0;

		const PluginTextureHandle stored = Get(id);
		if (!stored || pixelSize <= 0 || pixelSize >= static_cast<int>(kStoredSize))
			return stored;

		const PluginImageId imageId = Canonical(id);
		Entry* e = Find(imageId);
		if (!e)
			return stored;

		const uint32_t size = static_cast<uint32_t>(pixelSize);
		const uint64_t now = m_now.load(std::memory_order_relaxed);
		for (ExactSlot& slot : e->exact)
		{
			if (slot.size.load(std::memory_order_acquire) != size)
				continue;
			const int32_t state = slot.state.load(std::memory_order_acquire);
			if (state == kExactReady)
			{
				const PluginTextureHandle copy = slot.handle.load(std::memory_order_acquire);
				const uint32_t copyWidth = slot.width.load(std::memory_order_relaxed);
				const uint32_t copyHeight = slot.height.load(std::memory_order_relaxed);
				// The slot may have been dropped and made again for another size since size was read.
				if (!copy || slot.size.load(std::memory_order_acquire) != size)
					return stored;
				slot.lastDrawn.store(now, std::memory_order_relaxed);
				if (width)
					*width = static_cast<int>(copyWidth);
				if (height)
					*height = static_cast<int>(copyHeight);
				return copy;
			}
			if (state == kExactPending || (state == kExactClosed && now < slot.closedUntil.load(std::memory_order_relaxed)))
				return stored;
		}

		AskExact(*e, imageId, size);
		return stored;
	}

	// Notes that the size was asked for. Once it has been asked for steadily, queues its copy.
	void Library::AskExact(Entry& e, PluginImageId id, uint32_t size)
	{
		const uint64_t now = m_now.load(std::memory_order_relaxed);
		ExactTrial* trial = nullptr;
		for (ExactTrial& t : e.trial)
			if (t.size.load(std::memory_order_relaxed) == size)
				trial = &t;

		if (!trial)
		{
			// Takes the cell asked for least recently among those not asked for within kExactGapMs: a
			// size drawn every frame keeps its cell, and a size that is asked for once and changes is
			// not tracked while every cell is in use.
			for (ExactTrial& t : e.trial)
			{
				const uint64_t last = t.last.load(std::memory_order_relaxed);
				if (t.size.load(std::memory_order_relaxed) != 0 && (now <= last || now - last <= kExactGapMs))
					continue;
				if (!trial || last < trial->last.load(std::memory_order_relaxed))
					trial = &t;
			}
			if (!trial)
				return;
			trial->size.store(size, std::memory_order_relaxed);
			trial->first.store(now, std::memory_order_relaxed);
			trial->last.store(now, std::memory_order_relaxed);
			return;
		}

		const uint64_t last = trial->last.load(std::memory_order_relaxed);
		if (now > last && now - last > kExactGapMs)
			trial->first.store(now, std::memory_order_relaxed);
		trial->last.store(now, std::memory_order_relaxed);

		const uint64_t first = trial->first.load(std::memory_order_relaxed);
		if (now <= first || now - first < kExactSteadyMs)
			return;

		bool queued;
		{
			std::lock_guard<std::mutex> lock(m_lock);
			queued = OfferExact(e, id, size);
		}
		// A refused offer is made again after kExactRefusedMs more of steady asking.
		trial->first.store(queued ? now : now + kExactRefusedMs, std::memory_order_relaxed);
	}

	int Library::State(PluginImageId id) const
	{
		const Entry* e = Find(id);
		if (!e)
			return PLUGIN_IMAGE_NONE;
		e = Resolve(e);
		return e ? PublicState(e->state.load(std::memory_order_acquire)) : PLUGIN_IMAGE_NONE;
	}

	void Library::UseSets(const IPluginSelf* self, uint32_t sets)
	{
		if (!self)
			return;
		sets &= kKnownSets;
		std::lock_guard<std::mutex> lock(m_lock);
		if (sets == 0)
			m_used.erase(self);
		else
			m_used[self] = sets;
	}

	void Library::ForgetPlugin(const IPluginSelf* self)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		m_used.erase(self);
	}

	void Library::SetNow(uint64_t nowMs)
	{
		m_now.store(nowMs, std::memory_order_relaxed);
	}

	uint32_t Library::UsedSetsLocked() const
	{
		uint32_t used = 0;
		for (const auto& use : m_used)
			used |= use.second;
		return used;
	}

	uint32_t Library::UsedSets() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return UsedSetsLocked();
	}

	bool Library::InSetUse(const Entry& e) const
	{
		return (e.sets & UsedSetsLocked()) != 0;
	}

	bool Library::SeenLately(const Entry& e, uint64_t now)
	{
		const uint64_t seen = e.lastSeen.load(std::memory_order_relaxed);
		return seen != 0 && (seen >= now || now - seen <= kSeenLatelyMs);
	}

	void Library::TagSets(Entry& e, PluginImageId id, uint32_t sets)
	{
		sets &= kKnownSets;
		if (sets == 0)
			return;
		if (e.sets == 0)
			m_setMembers.push_back(id);
		e.sets |= sets;
	}

	void Library::Ask(Entry& e, PluginImageId id)
	{
		int32_t idle = kIdle;
		if (e.state.compare_exchange_strong(idle, kWaiting, std::memory_order_acq_rel) && !e.queued)
		{
			e.queued = true;
			m_queue.push_back(id);
		}
	}

	// Images that Get found not kept and moved to WAITING without the lock.
	void Library::Drain()
	{
		if (!m_askPending.exchange(false, std::memory_order_acq_rel))
			return;
		const uint32_t count = m_count.load(std::memory_order_relaxed);
		for (PluginImageId id = 1; id <= count; ++id)
		{
			Entry* e = Find(id);
			if (e->target.load(std::memory_order_relaxed) == 0
				&& e->state.load(std::memory_order_acquire) == kWaiting && !e->queued && !e->taken)
			{
				e->queued = true;
				m_queue.push_back(id);
			}
		}
	}

	bool Library::TakeNext(Request& out)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Drain();

		const uint64_t now = m_now.load(std::memory_order_relaxed);
		struct Best { size_t slot; bool lately; uint64_t seen; PluginImageId id; };
		bool haveBest = false;
		Best best{};

		for (size_t i = 0; i < m_queue.size();)
		{
			const PluginImageId id = m_queue[i];
			Entry* e = Find(id);
			const uint64_t seen = e->lastSeen.load(std::memory_order_relaxed);
			const bool lately = seen >= now || now - seen <= kSeenLatelyMs;

			if (!lately && !InSetUse(*e))
			{
				e->queued = false;
				e->state.store(kIdle, std::memory_order_release);
				m_queue[i] = m_queue.back();
				m_queue.pop_back();
				continue;
			}

			bool better;
			if (!haveBest)
				better = true;
			else if (lately != best.lately)
				better = lately;
			else if (seen != best.seen)
				better = seen > best.seen;
			else
				better = id < best.id;
			if (better)
			{
				best = { i, lately, seen, id };
				haveBest = true;
			}
			++i;
		}

		if (!haveBest)
			return false;

		Entry* e = Find(best.id);
		m_queue[best.slot] = m_queue.back();
		m_queue.pop_back();
		e->queued = false;
		e->taken = true;
		out.id = best.id;
		out.kind = e->kind;
		out.name = e->name;
		return true;
	}

	bool Library::Publish(PluginImageId id, PluginTextureHandle handle, uint64_t bytes,
		std::vector<Released>& evicted)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e || !e->taken)
			return false;
		e->taken = false;

		if (!handle || bytes > m_byteCap)
		{
			e->state.store(kFailed, std::memory_order_release);
			return false;
		}

		e->bytes = bytes;
		e->handle.store(handle, std::memory_order_relaxed);
		e->state.store(kReady, std::memory_order_release);
		m_bytes += bytes;
		++m_kept;

		Trim(evicted);
		return e->state.load(std::memory_order_relaxed) == kReady;
	}

	// Evicts the kept images seen least recently, never one seen lately: what is drawn stays,
	// and the caps may be passed for as long as everything over them is in use.
	void Library::Trim(std::vector<Released>& evicted)
	{
		if (m_bytes <= m_byteCap && m_kept <= m_countCap)
			return;

		// The size-exact copies not drawn lately go first: they are made again from the stored picture.
		if (m_bytes > m_byteCap && m_exactCount != 0)
		{
			for (const ExactRef& ref : IdleExact(kSeenLatelyMs))
			{
				if (m_bytes <= m_byteCap)
					break;
				DropSlot(Find(ref.id)->exact[ref.slot], evicted, ref.id);
			}
			if (m_bytes <= m_byteCap && m_kept <= m_countCap)
				return;
		}

		struct Kept { uint64_t seen; PluginImageId id; };
		std::vector<Kept> kept;
		const uint64_t now = m_now.load(std::memory_order_relaxed);
		const uint32_t count = m_count.load(std::memory_order_relaxed);
		for (PluginImageId id = 1; id <= count; ++id)
		{
			const Entry* e = Find(id);
			if (e->state.load(std::memory_order_relaxed) == kReady && !SeenLately(*e, now))
				kept.push_back({ e->lastSeen.load(std::memory_order_relaxed), id });
		}
		std::sort(kept.begin(), kept.end(), [](const Kept& a, const Kept& b)
		{
			return a.seen != b.seen ? a.seen < b.seen : a.id < b.id;
		});

		for (const Kept& k : kept)
		{
			if (m_bytes <= m_byteCap && m_kept <= m_countCap)
				break;
			Entry* e = Find(k.id);
			m_capped = true;
			DropExact(*e, k.id, evicted);
			evicted.push_back({ k.id, e->handle.load(std::memory_order_relaxed), e->bytes });
			m_bytes -= e->bytes;
			--m_kept;
			e->bytes = 0;
			e->handle.store(nullptr, std::memory_order_relaxed);
			e->state.store(kIdle, std::memory_order_release);
		}
	}

	uint32_t Library::RoomFor(uint32_t wanted) const
	{
		static const uint64_t kImageBytes = ImageShrink::Bytes({ kStoredSize, kStoredSize }, ImageShrink::LevelCount(kStoredSize, kStoredSize));
		std::lock_guard<std::mutex> lock(m_lock);

		uint64_t fit = m_kept < m_countCap ? m_countCap - m_kept : 0;
		uint64_t byteFit = m_bytes + kImageBytes <= m_byteCap ? (m_byteCap - m_bytes) / kImageBytes : 0;
		// A cap under one image: the image is refused when it is published, and nothing is waiting for room.
		if (m_kept == 0 && byteFit == 0)
			byteFit = 1;
		fit = (std::min)(fit, byteFit);
		if (fit >= wanted)
			return wanted;

		const uint64_t now = m_now.load(std::memory_order_relaxed);
		const uint32_t count = m_count.load(std::memory_order_relaxed);
		for (PluginImageId id = 1; id <= count && fit < wanted; ++id)
		{
			const Entry* e = Find(id);
			if (e->state.load(std::memory_order_relaxed) == kReady && !SeenLately(*e, now))
				++fit;
		}
		return static_cast<uint32_t>(fit);
	}

	bool Library::HasQueued()
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Drain();
		return !m_queue.empty();
	}

	void Library::PublishNoImage(PluginImageId id)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e || !e->taken)
			return;
		e->taken = false;
		e->state.store(kNoImage, std::memory_order_release);
	}

	void Library::Fail(PluginImageId id)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e || !e->taken)
			return;
		e->taken = false;
		e->state.store(kFailed, std::memory_order_release);
	}

	void Library::Requeue(PluginImageId id)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e || !e->taken)
			return;
		e->taken = false;
		e->queued = true;
		m_queue.push_back(id);
	}

	bool Library::Alias(PluginImageId from, PluginImageId to)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* source = Find(from);
		Entry* dest = Find(to);
		if (!source || !dest)
			return false;
		const PluginImageId forwarded = dest->target.load(std::memory_order_relaxed);
		const PluginImageId destId = forwarded ? forwarded : to;
		dest = Find(destId);
		if (!dest || destId == from || source->target.load(std::memory_order_relaxed) != 0)
			return false;
		if (source->taken || source->state.load(std::memory_order_relaxed) == kReady)
			return false;

		if (source->queued)
		{
			m_queue.erase(std::find(m_queue.begin(), m_queue.end(), from));
			source->queued = false;
		}
		TagSets(*dest, destId, source->sets);
		const uint64_t seen = source->lastSeen.load(std::memory_order_relaxed);
		if (seen > dest->lastSeen.load(std::memory_order_relaxed))
			dest->lastSeen.store(seen, std::memory_order_relaxed);
		const bool wanted = source->state.load(std::memory_order_relaxed) == kWaiting;
		source->target.store(destId, std::memory_order_release);
		if (wanted)
			Ask(*dest, destId);
		return true;
	}

	void Library::AddToSets(PluginImageId id, uint32_t sets)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e)
			return;
		const PluginImageId target = e->target.load(std::memory_order_relaxed);
		if (target)
		{
			id = target;
			e = Find(id);
		}
		if (e)
			TagSets(*e, id, sets);
	}

	PluginImageId Library::PathInSet(const char* textureObjectPath, uint32_t sets)
	{
		const PluginImageId id = IdFor(Kind::Path, NormalizeObjectPath(textureObjectPath), textureObjectPath);
		if (id != 0)
			AddToSets(id, sets);
		return id;
	}

	bool Library::TakePreload(Request& out)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		const uint32_t used = UsedSetsLocked();
		const size_t count = m_setMembers.size();
		for (size_t step = 0; step < count; ++step)
		{
			const size_t slot = (m_preloadCursor + step) % count;
			const PluginImageId id = m_setMembers[slot];
			Entry* e = Find(id);
			if ((e->sets & used) == 0 || e->target.load(std::memory_order_relaxed) != 0 || e->queued || e->taken)
				continue;
			int32_t idle = kIdle;
			if (!e->state.compare_exchange_strong(idle, kWaiting, std::memory_order_acq_rel))
				continue;

			m_preloadCursor = slot + 1;
			e->taken = true;
			out.id = id;
			out.kind = e->kind;
			out.name = e->name;
			return true;
		}
		return false;
	}

	void Library::Defer(PluginImageId id)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e || !e->taken)
			return;
		e->taken = false;
		if (SeenLately(*e, m_now.load(std::memory_order_relaxed)))
		{
			e->queued = true;
			m_queue.push_back(id);
		}
		else
		{
			e->state.store(kIdle, std::memory_order_release);
		}
	}

	bool Library::SeenWaiting() const
	{
		const uint64_t now = m_now.load(std::memory_order_relaxed);
		const uint32_t count = m_count.load(std::memory_order_acquire);
		for (PluginImageId id = 1; id <= count; ++id)
		{
			const Entry* e = Find(id);
			if (e->state.load(std::memory_order_acquire) == kWaiting && e->target.load(std::memory_order_relaxed) == 0
				&& SeenLately(*e, now))
				return true;
		}
		return false;
	}

	bool Library::PreloadHasRoom() const
	{
		static const uint64_t kImageBytes = ImageShrink::Bytes({ kStoredSize, kStoredSize }, ImageShrink::LevelCount(kStoredSize, kStoredSize));
		std::lock_guard<std::mutex> lock(m_lock);
		return !m_capped && m_kept < m_countCap && m_bytes + kImageBytes <= m_byteCap;
	}

	Library::PreloadProgress Library::GetPreloadProgress() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		const uint32_t used = UsedSetsLocked();
		PreloadProgress progress;
		for (const PluginImageId id : m_setMembers)
		{
			const Entry* e = Find(id);
			if ((e->sets & used) == 0 || e->target.load(std::memory_order_relaxed) != 0)
				continue;
			switch (e->state.load(std::memory_order_relaxed))
			{
			case kIdle:
			case kWaiting:  ++progress.queued; break;
			case kReady:
			case kNoImage:  ++progress.done; break;
			default: break;
			}
		}
		return progress;
	}

	void Library::ReleaseAll(std::vector<Released>& released)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		const uint32_t count = m_count.load(std::memory_order_relaxed);
		for (PluginImageId id = 1; id <= count; ++id)
		{
			Entry* e = Find(id);
			DropExact(*e, id, released);
			if (e->state.load(std::memory_order_relaxed) != kReady)
				continue;
			released.push_back({ id, e->handle.load(std::memory_order_relaxed), e->bytes });
			e->bytes = 0;
			e->handle.store(nullptr, std::memory_order_relaxed);
			e->state.store(kFailed, std::memory_order_release);
		}
		released.insert(released.end(), m_exactDropped.begin(), m_exactDropped.end());
		m_exactDropped.clear();
		m_exactQueue.clear();
		m_bytes = 0;
		m_kept = 0;
	}

	// ---- Size-exact copies -------------------------------------------------

	// Empties a slot. A copy that was ready is appended to dropped, and its bytes leave the total.
	void Library::DropSlot(ExactSlot& slot, std::vector<Released>& dropped, PluginImageId id)
	{
		const int32_t state = slot.state.load(std::memory_order_relaxed);
		if (state == kExactFree)
			return;
		slot.state.store(kExactFree, std::memory_order_release);
		if (state == kExactReady)
		{
			dropped.push_back({ id, slot.handle.load(std::memory_order_relaxed), slot.bytes, true });
			m_bytes -= slot.bytes;
		}
		if (state == kExactReady || state == kExactPending)
			--m_exactCount;
		slot.handle.store(nullptr, std::memory_order_relaxed);
		slot.size.store(0, std::memory_order_relaxed);
		slot.bytes = 0;
		slot.taken = false;
	}

	void Library::DropExact(Entry& e, PluginImageId id, std::vector<Released>& dropped)
	{
		for (ExactSlot& slot : e.exact)
			DropSlot(slot, dropped, id);
	}

	// The ready copies not drawn for idleMs, the least recently drawn first.
	std::vector<Library::ExactRef> Library::IdleExact(uint64_t idleMs) const
	{
		std::vector<ExactRef> idle;
		const uint64_t now = m_now.load(std::memory_order_relaxed);
		const uint32_t count = m_count.load(std::memory_order_relaxed);
		for (PluginImageId id = 1; id <= count; ++id)
		{
			const Entry* e = Find(id);
			for (uint32_t i = 0; i < kExactPerImage; ++i)
			{
				const ExactSlot& slot = e->exact[i];
				const uint64_t drawn = slot.lastDrawn.load(std::memory_order_relaxed);
				if (slot.state.load(std::memory_order_relaxed) == kExactReady && now > drawn && now - drawn >= idleMs)
					idle.push_back({ id, i, drawn });
			}
		}
		std::sort(idle.begin(), idle.end(), [](const ExactRef& a, const ExactRef& b)
		{
			return a.lastDrawn != b.lastDrawn ? a.lastDrawn < b.lastDrawn : a.id < b.id;
		});
		return idle;
	}

	// Takes a slot for a copy of the image at the size and queues it, when the image is kept, the
	// limits allow it and a copy is not made or under way already. Where there is no free slot the
	// copy drawn least recently and not drawn lately is replaced.
	bool Library::OfferExact(Entry& e, PluginImageId id, uint32_t size)
	{
		if (e.state.load(std::memory_order_relaxed) != kReady)
			return false;
		const uint64_t now = m_now.load(std::memory_order_relaxed);

		// Refused when the copy alone does not fit under the byte cap. Whether the rest does is for
		// PublishExact: Trim makes room there, by the rules that make room for any newcomer.
		if (static_cast<uint64_t>(size) * size * ImageShrink::kBytesPerPixel > m_byteCap)
			return false;

		ExactSlot* use = nullptr;
		ExactSlot* closed = nullptr;
		ExactSlot* oldest = nullptr;
		ExactSlot* empty = nullptr;
		for (ExactSlot& slot : e.exact)
		{
			const int32_t state = slot.state.load(std::memory_order_relaxed);
			if (state == kExactFree)
				empty = empty ? empty : &slot;
			else if (slot.size.load(std::memory_order_relaxed) == size)
			{
				if (state != kExactClosed || now < slot.closedUntil.load(std::memory_order_relaxed))
					return false;   // made, under way, or lately failed
				use = &slot;
			}
			else if (state == kExactClosed)
				closed = closed ? closed : &slot;
			else if (state == kExactReady)
			{
				const uint64_t drawn = slot.lastDrawn.load(std::memory_order_relaxed);
				if ((now > drawn && now - drawn > kSeenLatelyMs)
					&& (!oldest || drawn < oldest->lastDrawn.load(std::memory_order_relaxed)))
					oldest = &slot;
			}
		}
		if (!use)
			use = empty ? empty : closed ? closed : oldest;
		if (!use)
			return false;

		// A slot taken from a ready copy makes room for the new one; any other adds to the count.
		const bool replacing = use->state.load(std::memory_order_relaxed) == kExactReady;
		if (!replacing && m_exactCount >= kExactCountCap)
		{
			// No scan here: this runs on the render thread. The pump's sweep makes room for the sizes waiting.
			m_exactWaiting.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		DropSlot(*use, m_exactDropped, id);

		use->width.store(0, std::memory_order_relaxed);
		use->height.store(0, std::memory_order_relaxed);
		use->lastDrawn.store(now, std::memory_order_relaxed);
		use->size.store(size, std::memory_order_relaxed);
		use->state.store(kExactPending, std::memory_order_release);
		++m_exactCount;
		m_exactQueue.push_back({ id, size });
		return true;
	}

	bool Library::TakeExact(ExactRequest& out)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		while (!m_exactQueue.empty())
		{
			const std::pair<PluginImageId, uint32_t> next = m_exactQueue.front();
			m_exactQueue.pop_front();
			Entry* e = Find(next.first);
			if (!e || e->state.load(std::memory_order_relaxed) != kReady)
				continue;
			for (ExactSlot& slot : e->exact)
			{
				if (slot.state.load(std::memory_order_relaxed) != kExactPending || slot.taken
					|| slot.size.load(std::memory_order_relaxed) != next.second)
					continue;
				slot.taken = true;
				out.id   = next.first;
				out.size = next.second;
				out.name = e->name;
				return true;
			}
		}
		return false;
	}

	bool Library::PublishExact(PluginImageId id, uint32_t size, PluginTextureHandle handle, uint32_t width, uint32_t height,
		uint64_t bytes, std::vector<Released>& dropped)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e)
			return false;
		ExactSlot* slot = nullptr;
		for (ExactSlot& s : e->exact)
			if (s.state.load(std::memory_order_relaxed) == kExactPending && s.taken && s.size.load(std::memory_order_relaxed) == size)
				slot = &s;
		if (!slot)
			return false;   // the image left, or the copies were cleared, while it was made
		slot->taken = false;

		const uint64_t now = m_now.load(std::memory_order_relaxed);
		const auto close = [&]
		{
			slot->closedUntil.store(now + kExactRetryMs, std::memory_order_relaxed);
			slot->state.store(kExactClosed, std::memory_order_release);
			--m_exactCount;
		};
		if (!handle || width == 0 || height == 0)
		{
			close();
			return false;
		}

		slot->bytes = bytes;
		slot->handle.store(handle, std::memory_order_relaxed);
		slot->width.store(width, std::memory_order_relaxed);
		slot->height.store(height, std::memory_order_relaxed);
		slot->lastDrawn.store(now, std::memory_order_relaxed);
		m_bytes += bytes;
		slot->state.store(kExactReady, std::memory_order_release);

		Trim(dropped);
		if (slot->state.load(std::memory_order_relaxed) != kExactReady)
			return false;   // the image it belongs to was evicted: the copy is among dropped
		if (m_bytes > m_byteCap)
		{
			// Over the cap with everything else in use: the copy is refused.
			m_bytes -= bytes;
			slot->handle.store(nullptr, std::memory_order_relaxed);
			slot->bytes = 0;
			close();
			return false;
		}
		return true;
	}

	void Library::ExactEnded(PluginImageId id, uint32_t size, ExactEnd how)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		Entry* e = Find(id);
		if (!e)
			return;
		for (ExactSlot& slot : e->exact)
		{
			if (slot.state.load(std::memory_order_relaxed) != kExactPending || !slot.taken
				|| slot.size.load(std::memory_order_relaxed) != size)
				continue;
			slot.taken = false;
			if (how == ExactEnd::Retry)
			{
				m_exactQueue.push_back({ id, size });
				return;
			}
			const uint64_t now = m_now.load(std::memory_order_relaxed);
			slot.closedUntil.store(how == ExactEnd::NotNeeded ? UINT64_MAX : now + kExactRetryMs, std::memory_order_relaxed);
			slot.state.store(kExactClosed, std::memory_order_release);
			--m_exactCount;
			return;
		}
	}

	void Library::ReapExact(std::vector<Released>& dropped)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		dropped.insert(dropped.end(), m_exactDropped.begin(), m_exactDropped.end());
		m_exactDropped.clear();

		const uint64_t now = m_now.load(std::memory_order_relaxed);
		if (now < m_nextSweepMs || m_exactCount == 0)
			return;
		m_nextSweepMs = now + kExactSweepMs;
		for (const ExactRef& ref : IdleExact(kExactIdleMs))
			DropSlot(Find(ref.id)->exact[ref.slot], dropped, ref.id);

		// At the count cap, copies not drawn lately make room for the sizes that were refused.
		uint32_t waiting = m_exactWaiting.exchange(0, std::memory_order_relaxed);
		if (waiting != 0 && m_exactCount >= kExactCountCap)
			for (const ExactRef& ref : IdleExact(kSeenLatelyMs))
			{
				if (waiting-- == 0)
					break;
				DropSlot(Find(ref.id)->exact[ref.slot], dropped, ref.id);
			}
	}

	void Library::ReleaseExact(std::vector<Released>& dropped)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		const uint32_t count = m_count.load(std::memory_order_relaxed);
		for (PluginImageId id = 1; id <= count; ++id)
			DropExact(*Find(id), id, dropped);
		dropped.insert(dropped.end(), m_exactDropped.begin(), m_exactDropped.end());
		m_exactDropped.clear();
		m_exactQueue.clear();
	}

	uint32_t Library::ExactCount() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return m_exactCount;
	}

	void Library::CollectSets(std::vector<SetMember>& out) const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		const uint32_t count = m_count.load(std::memory_order_relaxed);
		for (PluginImageId id = 1; id <= count; ++id)
		{
			const Entry* e = Find(id);
			// An item that resolved to a texture is an alias, and its sets moved to that texture.
			if (e->sets != 0 && e->kind == Kind::Path && e->target.load(std::memory_order_relaxed) == 0)
				out.push_back({ e->sets, e->name });
		}
	}

	void Library::SetCaps(uint64_t byteCap, uint32_t countCap)
	{
		std::lock_guard<std::mutex> lock(m_lock);
		m_byteCap = byteCap;
		m_countCap = countCap;
	}

	uint64_t Library::BytesKept() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return m_bytes;
	}

	uint32_t Library::CountKept() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return m_kept;
	}

	uint32_t Library::ImageCount() const
	{
		return m_count.load(std::memory_order_acquire);
	}

	// ---- The table handed to plugins --------------------------------------

	Library& Instance()
	{
		// Never destroyed: a render callback can still be reading at exit.
		static Library* const library = new Library();
		return *library;
	}

	namespace
	{
		PluginImageId ItemThunk(const IPluginSelf*, const char* name)  { return Instance().Item(name); }
		PluginImageId PathThunk(const IPluginSelf*, const char* path)  { return Instance().Path(path); }
		PluginTextureHandle GetThunk(PluginImageId id)                 { return Instance().Get(id); }
		PluginTextureHandle GetSizedThunk(PluginImageId id, int pixelSize, int* width, int* height) { return Instance().GetSized(id, pixelSize, width, height); }
		int StateThunk(PluginImageId id)                               { return Instance().State(id); }
		void UseSetsThunk(const IPluginSelf* self, uint32_t sets)      { Instance().UseSets(self, sets); }
		PluginImageId CanonicalThunk(PluginImageId id)                 { return Instance().Canonical(id); }

		// A texture is named by its object path, which needs the engine to read.
		std::atomic<FromTextureFn> s_fromTexture{ nullptr };

		PluginImageId FromTextureThunk(const IPluginSelf*, SDK::UTexture2D* texture)
		{
			const FromTextureFn fn = s_fromTexture.load(std::memory_order_acquire);
			return fn ? fn(texture) : 0;
		}
	}

	void SetFromTexture(FromTextureFn fn)
	{
		s_fromTexture.store(fn, std::memory_order_release);
	}

	IPluginImages* GetInterface()
	{
		static IPluginImages table = {
			ItemThunk,
			PathThunk,
			FromTextureThunk,
			GetThunk,
			StateThunk,
			UseSetsThunk,
			CanonicalThunk,
			GetSizedThunk,
		};
		return &table;
	}

	void ForgetPlugin(const IPluginSelf* self)
	{
		Instance().ForgetPlugin(self);
	}
}
