#include "UI/image_pump.h"
#include "UI/image_shrink.h"

#include <algorithm>
#include <cstdio>

namespace ImagePump
{
	namespace
	{
		const char* PauseText(int pause)
		{
			static const char* const kText[] = { "no", "setting off", "no set in use", "reading the list", "image waiting on screen",
				"game holds loaded images", "video memory", "cache full", "game reads off" };
			return kText[pause];
		}
	}

	Pump::Pump(ImageLibrary::Library& library, Host& host, ImageLookup::ItemSource& items, ImageLookup::ItemSource& buildings)
		: m_library(library), m_host(host), m_items(items), m_buildings(buildings)
	{
	}

	void Pump::Tick()
	{
		if (m_stopped)
			return;

		const bool ready = m_host.GpuReady();
		if (!m_started)
		{
			if (!ready)
				return;
			Start();
		}
		else if (!ready && !m_gone)
		{
			GpuGone();
		}

		// One clock read ends each phase and starts the next, so a slow tick can say which one it was.
		const uint64_t begin = m_host.NowUs();
		uint64_t phaseUs[kPhases] = {};
		uint64_t mark = begin;
		const auto endPhase = [&](Phase phase)
		{
			const uint64_t now = m_host.NowUs();
			phaseUs[static_cast<size_t>(phase)] = now - mark;
			mark = now;
		};
		m_library.SetNow(begin / 1000);

		FreeDeferred();
		endPhase(Phase::Free);
		if (!m_gone)
		{
			Collect();
			CollectFiles();
			TendExact();
		}
		endPhase(Phase::Collect);
		if (m_saver)
			m_saver->Poll(begin / 1000);
		endPhase(Phase::Save);
		// The saved names and sets come in before anything is served, so no image is read from the
		// game that a file would have answered.
		const bool filesLoading = PollSource(begin / 1000);
		endPhase(Phase::Files);
		uint32_t loads = 0;
		bool idle = false;
		if (m_gone)
			FailQueued();
		else if (!filesLoading)
			idle = Serve(loads);
		endPhase(Phase::Serve);
		if (!m_gone && !filesLoading)
		{
			StepItems(loads);
			endPhase(Phase::Items);
			Preload(idle, loads);
			endPhase(Phase::Preload);
		}

		const uint64_t end = mark;
		m_stats.longestTickUs = std::max(m_stats.longestTickUs, end - begin);
		if (end - begin > kSlowTickUs)
			LogSlowTick(end - begin, phaseUs);
		if (end / 1000 >= m_nextSummaryMs)
		{
			m_nextSummaryMs = end / 1000 + kSummaryIntervalMs;
			LogSummary();
		}
	}

	// Names the phase that took most of a tick that ran long. Capped, so a bad stretch cannot fill the log.
	void Pump::LogSlowTick(uint64_t tickUs, const uint64_t (&phaseUs)[kPhases])
	{
		if (m_slowTickLogs >= kSlowTickLogs)
			return;
		++m_slowTickLogs;
		size_t longest = 0;
		for (size_t i = 1; i < kPhases; ++i)
			if (phaseUs[i] > phaseUs[longest])
				longest = i;
		static const char* const kNames[kPhases] = { "freeing images", "collecting reads", "saving files", "reading the cache folder",
			"serving requests", "listing items", "preload" };
		char line[160];
		snprintf(line, sizeof(line), "[Images] Slow pump tick: %.1f ms, %.1f ms of it in %s",
			static_cast<double>(tickUs) / 1000.0, static_cast<double>(phaseUs[longest]) / 1000.0, kNames[longest]);
		m_host.Log(line);
	}

	void Pump::Start()
	{
		m_started = true;
		m_library.SetCaps(ImageLibrary::ByteCapForBudget(m_host.LocalVideoBudget()), ImageLibrary::kCountCap);
	}

	// The device was lost or the overlay shut down under us. The texture API
	// releases every texture it holds when it tears down, so the pump frees none.
	void Pump::GpuGone()
	{
		m_gone = true;
		m_off  = true;
		if (m_saver)
			m_saver->Abandon();
		if (m_source)
			m_source->Abandon();

		size_t dropped = m_inFlight.size() + m_onDisk.size();
		for (const InFlight& read : m_inFlight)
			m_library.Fail(read.id);
		for (PluginImageId id : m_onDisk)
			m_library.Fail(id);
		m_stats.failed += m_inFlight.size() + m_onDisk.size();
		m_inFlight.clear();
		m_onDisk.clear();
		m_preloadOnDisk.clear();

		m_scratch.clear();
		m_library.ReleaseAll(m_scratch);
		dropped += m_scratch.size();
		dropped += m_toFree.size();
		m_toFree.clear();
		m_scratch.clear();

		m_host.Log("[Images] The GPU side is gone: " + std::to_string(dropped) + " images dropped, no more reads this session");
	}

	// The reads that finished on the GPU are published; the evicted copies they
	// push out wait for the next tick to be freed.
	void Pump::Collect()
	{
		for (size_t i = 0; i < m_inFlight.size();)
		{
			const InFlight read = m_inFlight[i];
			const CopyState state = m_host.StateOf(read.copy.texture);
			if (state == CopyState::Pending)
			{
				++i;
				continue;
			}

			Stored stored;
			StoreResult made = StoreResult::Failed;
			if (state == CopyState::Done)
				made = Store(read, stored);
			if (made == StoreResult::Later)
			{
				// The shrink has no room yet: the copy is kept and tried again next tick.
				++i;
				continue;
			}

			m_inFlight[i] = m_inFlight.back();
			m_inFlight.pop_back();

			if (made == StoreResult::Failed)
			{
				// A copy that is gone is not ours to free.
				if (state == CopyState::Done)
					m_host.Free(read.copy.texture);
				m_library.Fail(read.id);
				++m_stats.failed;
				continue;
			}

			if (PublishStored(read.id, stored.texture, stored.bytes))
				++m_stats.fromGame;
		}
	}

	// The stored copy goes into the library, whether it came from a shrink or from a file. True when
	// it is kept.
	bool Pump::PublishStored(PluginImageId id, PluginTextureHandle texture, uint64_t bytes)
	{
		m_scratch.clear();
		const bool kept = m_library.Publish(id, texture, bytes, m_scratch);
		if (kept)
			++m_stats.published;
		else
		{
			// Refused, or evicted again at once (then it is among the evicted).
			const bool evictedAgain = std::any_of(m_scratch.begin(), m_scratch.end(),
				[&](const ImageLibrary::Released& r) { return r.handle == texture; });
			if (!evictedAgain)
			{
				m_host.Free(texture);
				++m_stats.failed;
			}
		}
		for (const ImageLibrary::Released& released : m_scratch)
			m_toFree.push_back(released.handle);
		m_stats.evicted += std::count_if(m_scratch.begin(), m_scratch.end(), [](const ImageLibrary::Released& r) { return !r.exact; });
		return kept;
	}

	// A size-exact copy a worker finished: published, or its texture freed when the library does not
	// keep it. A copy that was not made leaves the stored picture as what is drawn.
	void Pump::CollectExact(const ImageDisk::FileResult& result)
	{
		if (result.outcome != ImageDisk::FileOutcome::Texture)
		{
			m_library.ExactEnded(result.id, result.exactSize,
				result.outcome == ImageDisk::FileOutcome::Same ? ImageLibrary::ExactEnd::NotNeeded : ImageLibrary::ExactEnd::Failed);
			return;
		}

		m_scratch.clear();
		const bool kept = m_library.PublishExact(result.id, result.exactSize, result.texture, result.width, result.height,
			result.bytes, m_scratch);
		if (kept)
			++m_stats.exactMade;
		else
		{
			// Refused, or dropped again at once (then it is among the dropped).
			const bool droppedAgain = std::any_of(m_scratch.begin(), m_scratch.end(),
				[&](const ImageLibrary::Released& r) { return r.handle == result.texture; });
			if (!droppedAgain)
				m_host.Free(result.texture);
		}
		for (const ImageLibrary::Released& released : m_scratch)
			m_toFree.push_back(released.handle);
	}

	// Once per tick: the copies the table wants made go to the worker, and the ones it let go are freed
	// on the next tick. A cleared cache folder takes the copies with it.
	void Pump::TendExact()
	{
		m_scratch.clear();
		if (m_saver && m_saver->Cleared() != m_clearSeen)
		{
			m_clearSeen = m_saver->Cleared();
			m_library.ReleaseExact(m_scratch);
		}
		m_library.ReapExact(m_scratch);
		for (const ImageLibrary::Released& released : m_scratch)
			m_toFree.push_back(released.handle);
		m_scratch.clear();

		ImageLibrary::ExactRequest request;
		for (uint32_t taken = 0; taken < kExactPerTick && m_library.TakeExact(request); ++taken)
		{
			// Without a folder to read the stored picture from there is nothing to make a copy of.
			if (!m_source || m_sourceOff)
			{
				m_library.ExactEnded(request.id, request.size, ImageLibrary::ExactEnd::Failed);
				continue;
			}
			const ImageDisk::Source::ExactAnswer answer =
				m_source->RequestExact(request.id, ImageLibrary::NormalizeObjectPath(request.name.c_str()), request.size);
			if (answer == ImageDisk::Source::ExactAnswer::Refused)
			{
				// No copy can ever be made for this one: the stored picture is what is drawn, and it is not asked again.
				m_library.ExactEnded(request.id, request.size, ImageLibrary::ExactEnd::NotNeeded);
				continue;
			}
			if (answer == ImageDisk::Source::ExactAnswer::Busy)
			{
				m_library.ExactEnded(request.id, request.size, ImageLibrary::ExactEnd::Retry);
				break;
			}
		}
	}

	// The files the source has finished with. A good one is published; one that failed leaves the
	// image queued, and the game is read for it (the source no longer lists its file).
	void Pump::CollectFiles()
	{
		if (!m_source)
			return;
		m_files.clear();
		m_source->Collect(m_files);
		for (const ImageDisk::FileResult& result : m_files)
		{
			if (result.exactSize != 0)
			{
				CollectExact(result);
				continue;
			}
			m_onDisk.erase(result.id);
			const bool fromPreload = m_preloadOnDisk.erase(result.id) != 0;
			if (result.outcome != ImageDisk::FileOutcome::Texture)
			{
				// A preload request goes back to preload, under its rules; one that was drawn goes back in the queue.
				if (fromPreload)
					m_library.Defer(result.id);
				else
					m_library.Requeue(result.id);
				continue;
			}
			if (PublishStored(result.id, result.texture, result.bytes))
				++m_stats.fromDisk;
		}
	}

	// Gives the source its turn. True while the folder is being listed: nothing is served then.
	// Once it is listed, its saved names and set lists are taken in.
	bool Pump::PollSource(uint64_t nowMs)
	{
		if (!m_source || m_gone)
			return false;
		const ImageDisk::Source::State state = m_source->Poll(nowMs);
		if (state == ImageDisk::Source::State::Loading)
			return true;
		m_sourceOff = state == ImageDisk::Source::State::Off;
		if (state == ImageDisk::Source::State::Ready && !m_listsTaken)
		{
			ImageDisk::Lists lists;
			if (m_source->TakeLists(lists))
			{
				m_listsTaken = true;
				ApplyLists(std::move(lists));
			}
		}
		return false;
	}

	// Ties each saved item name to its texture path. An item that is already being read is left to
	// the index.
	void Pump::ApplyLists(ImageDisk::Lists&& lists)
	{
		size_t tied = 0;
		for (const ImageCache::NamePair& pair : lists.names)
		{
			// A list saved before unset names were refused can hold one: it is dropped here.
			if (ImageLibrary::IsUnsetItemKey(ImageLibrary::NormalizeItemName(pair.name.c_str())))
				continue;
			const PluginImageId item  = m_library.Item(pair.name.c_str());
			const PluginImageId image = m_library.Path(pair.path.c_str());
			if (item == 0 || image == 0)
				continue;
			tied += m_library.Alias(item, image);
			if (m_saver)
				m_saver->Name(pair.name, pair.path);
		}
		m_fileSets = std::move(lists.sets);
		if (tied != 0 || !m_fileSets.empty())
			m_host.Log("[Images] Cache folder: " + std::to_string(tied) + " item names and " + std::to_string(m_fileSets.size()) + " set lists read");
	}

	// The hand-over from the read to the library. The read made one level of the
	// game image in a temporary texture; the shrink draws it at the stored size with
	// every smaller level, and the temporary texture is let go at once (the host
	// keeps it until the shrink has run). Later: the shrink has no room, the copy
	// stays as it is. Failed: the image cannot be made, and the caller frees the copy.
	Pump::StoreResult Pump::Store(const InFlight& read, Stored& out)
	{
		if (!read.copy.texture || read.copy.width <= 0 || read.copy.height <= 0)
			return StoreResult::Failed;

		Shrunk shrunk;
		switch (m_host.Shrink(read.copy.texture, shrunk))
		{
		case ShrinkResult::Started:
			break;
		case ShrinkResult::Busy:
			return StoreResult::Later;
		default:
			return StoreResult::Failed;
		}

		m_host.Free(read.copy.texture);
		if (m_saver)
			m_saver->Begin({ read.path, read.copy.sourceFormat, static_cast<uint32_t>(read.copy.sourceWidth), static_cast<uint32_t>(read.copy.sourceHeight),
				static_cast<uint32_t>(shrunk.width), static_cast<uint32_t>(shrunk.height), shrunk.levels, shrunk.readback });
		out.texture = shrunk.texture;
		out.bytes   = ImageShrink::Bytes({ static_cast<uint32_t>(shrunk.width), static_cast<uint32_t>(shrunk.height) }, shrunk.levels);
		return StoreResult::Stored;
	}

	// True when no on-demand request was looked at, so the tick is free for preload.
	bool Pump::Serve(uint32_t& loads)
	{
		uint32_t reads = 0, attempts = 0, items = 0;
		m_later.clear();

		// A request is taken only when there is room for its image: under the caps, or in a kept
		// image that was not drawn lately. Reads and files already under way count against it. When
		// the images drawn fill the cache the rest wait, and nothing is read for them.
		const uint32_t pending = static_cast<uint32_t>(m_inFlight.size() + m_onDisk.size());
		const uint32_t room = m_library.RoomFor(pending + kAttemptsPerTick);
		uint32_t admit = room > pending ? room - pending : 0;
		if (admit == 0)
		{
			if (!m_fullLogged && m_library.HasQueued())
			{
				m_fullLogged = true;
				m_host.Log("[Images] The image cache is full of images in use: other images wait until there is room");
			}
			return true;
		}

		ImageLibrary::Request request;
		while (reads < kReadsPerTick && attempts < kAttemptsPerTick && admit > 0 && m_library.TakeNext(request))
		{
			// An item waits for the index; it costs no read, so it is no attempt.
			if (request.kind == ImageLibrary::Kind::Item)
			{
				// An unset name ("None") names no item: nothing is looked up for it.
				std::string key = ImageLibrary::NormalizeItemName(request.name.c_str());
				if (ImageLibrary::IsUnsetItemKey(key))
				{
					m_library.PublishNoImage(request.id);
					++m_stats.noImage;
					continue;
				}
				if (++items > kItemsPerTick)
				{
					m_later.push_back(request.id);
					break;
				}
				m_held.push_back({ request.id, std::move(key) });
				continue;
			}

			++attempts;
			const Outcome outcome = Attempt(request, loads);
			if (outcome == Outcome::Started)
				++reads;
			else if (outcome == Outcome::Later || outcome == Outcome::Busy)
				m_later.push_back(request.id);
			if (outcome == Outcome::Started || outcome == Outcome::Disk)
				--admit;

			if (outcome == Outcome::Busy)
				break;
		}

		// Only now: a request put back at once would be handed out again.
		for (PluginImageId id : m_later)
			m_library.Requeue(id);
		return attempts == 0;
	}

	// Answers the waiting items the index knows, and reads the next slice of the index while an item
	// is still waiting. The slice loads packages, so it is the tick's one load.
	void Pump::StepItems(uint32_t& loads)
	{
		if (m_held.empty())
			return;

		ResolveHeld();
		if (m_held.empty())
			return;
		// The index is part of the game's reads: with those off, what it already knows has answered.
		if (m_off)
		{
			FailHeld();
			return;
		}
		if (loads >= kLoadsPerTick)
			return;

		const ImageLookup::ItemIndex::State before = m_items.GetState();
		StepIndex(m_items, "item");
		++loads;
		if (before != ImageLookup::ItemIndex::State::Failed && m_items.GetState() == ImageLookup::ItemIndex::State::Failed)
			m_host.Log("[Images] No item can be read on this build or in this session: item images stay unavailable");
		ResolveHeld();
	}

	// ---- Preload ---------------------------------------------------------------

	// One tick of preload: the lists of the sets in use are read (a slice a tick,
	// one list at a time), and then, on one tick in kPreloadTickInterval and only
	// while every rule holds, the next member of a set is read.
	void Pump::Preload(bool idle, uint32_t& loads)
	{
		if (!m_host.PreloadEnabled())
		{
			m_pause = Pause::Off;
			return;
		}
		const uint32_t used = m_library.UsedSets();
		if (used == 0)
		{
			m_pause = Pause::NoSets;
			return;
		}

		StepSets(used, loads);

		if (++m_preloadTick % kPreloadTickInterval != 0)
			return;
		if (!idle || m_library.SeenWaiting())
			m_pause = Pause::Screen;
		else if (!m_library.PreloadHasRoom())
			m_pause = Pause::Full;
		else if (VideoFull())
			m_pause = Pause::VideoMemory;
		else
		{
			// What the game holds loaded is the game's own memory: it limits the reads of the game, not
			// the files, which load nothing.
			const bool holds = GameHoldsTooMuch();
			for (uint32_t taken = 0; taken < kDiskPreloadPerTick; ++taken)
			{
				ImageLibrary::Request request;
				if (taken != 0 && !m_library.PreloadHasRoom())
					break;
				if (!m_library.TakePreload(request))
				{
					if (taken != 0)
						break;
					const bool unlisted = ((used & PLUGIN_IMAGE_SET_ITEMS) && !m_itemsListed)
						|| ((used & PLUGIN_IMAGE_SET_BUILDINGS) && !m_buildingsListed);
					m_pause = unlisted ? Pause::Listing : Pause::Running;
					return;
				}

				std::string key;
				if ((holds || m_off) && !FileFor(request, key))
				{
					m_library.Defer(request.id);
					if (holds)
					{
						m_pause = Pause::GameHolds;
						return;
					}
					// Only the files are read with the game off: go on to the next member.
					m_pause = Pause::GameOff;
					continue;
				}

				m_pause = Pause::Running;
				const Outcome outcome = Attempt(request, loads);
				if (outcome == Outcome::Later || outcome == Outcome::Busy)
					m_library.Defer(request.id);
				if (outcome == Outcome::Disk)
					m_preloadOnDisk.insert(request.id);
				else
					break;
			}
		}
	}

	// Reads a slice of the list of one set that is in use and not listed yet. A slice loads packages,
	// so it is read for one set a tick and only on a tick that has made no load.
	void Pump::StepSets(uint32_t used, uint32_t& loads)
	{
		if ((used & PLUGIN_IMAGE_SET_ITEMS) && !m_itemsListed)
			StepSet(m_items, PLUGIN_IMAGE_SET_ITEMS, m_itemsListed, loads, "item");
		if ((used & PLUGIN_IMAGE_SET_BUILDINGS) && !m_buildingsListed)
			StepSet(m_buildings, PLUGIN_IMAGE_SET_BUILDINGS, m_buildingsListed, loads, "building");
	}

	// Reads the next slice of an index. The tick the item index is whole, one line says how many items
	// have no unique name, how many unique names were met on more than one item (the first one is kept)
	// and how many asset names. A building has no unique name, so its index has nothing to count.
	void Pump::StepIndex(ImageLookup::ItemIndex& index, const char* what)
	{
		const bool wasDone = index.GetState() == ImageLookup::ItemIndex::State::Done;
		index.Step(ImageLookup::kItemBudgetUs, [this] { return m_host.NowUs(); });
		if (!wasDone && &index == &m_items && index.GetState() == ImageLookup::ItemIndex::State::Done)
			m_host.Log(std::string("[Images] The ") + what + " list is read: " + std::to_string(index.UnnamedCount())
				+ " without a unique name, " + std::to_string(index.RepeatedNameCount()) + " unique names on more than one item, "
				+ std::to_string(index.RepeatedAssetCount()) + " asset names on more than one item");
	}

	// When the list is whole its images are added to the library as members of the set; when it
	// cannot be made the set is left empty for the session.
	void Pump::StepSet(ImageLookup::ItemIndex& index, uint32_t set, bool& listed, uint32_t& loads, const char* what)
	{
		using State = ImageLookup::ItemIndex::State;
		if (ApplyFileSet(set, listed, what))
			return;
		if (!m_off && loads < kLoadsPerTick && (index.GetState() == State::Idle || index.GetState() == State::Scanning))
		{
			StepIndex(index, what);
			++loads;
		}

		if (index.GetState() == State::Done)
		{
			size_t members = 0;
			for (const std::string& path : index.TexturePaths())
				members += m_library.PathInSet(path.c_str(), set) != 0;
			listed = true;
			m_host.Log(std::string("[Images] Preload set: ") + std::to_string(members) + " " + what + " images");
		}
		else if (index.GetState() == State::Failed)
		{
			listed = true;
			m_host.Log(std::string("[Images] The ") + what + " list cannot be read here: " + what + " images are not preloaded");
		}
	}

	// Fills the set from the saved set list, when the folder had one for it: the game's list is not
	// read, and no package is loaded to build it. True when it did.
	bool Pump::ApplyFileSet(uint32_t set, bool& listed, const char* what)
	{
		const auto saved = std::find_if(m_fileSets.begin(), m_fileSets.end(),
			[set](const ImageCache::SetEntry& entry) { return entry.set == set && !entry.paths.empty(); });
		if (saved == m_fileSets.end())
			return false;

		size_t members = 0;
		for (const std::string& path : saved->paths)
			members += m_library.PathInSet(path.c_str(), set) != 0;
		m_fileSets.erase(saved);
		listed = true;
		m_host.Log(std::string("[Images] Preload set: ") + std::to_string(members) + " " + what + " images (from the cache folder)");
		return true;
	}

	// Whether the full-size images the library made the game load, and the game
	// is taken to hold still, have reached the limit.
	bool Pump::GameHoldsTooMuch()
	{
		PruneHeldLoads(m_host.NowUs() / 1000);
		uint64_t total = 0;
		for (const HeldLoad& load : m_heldLoads)
			total += load.bytes;
		return total >= kPreloadHeldLimitBytes;
	}

	void Pump::PruneHeldLoads(uint64_t nowMs)
	{
		m_heldLoads.erase(std::remove_if(m_heldLoads.begin(), m_heldLoads.end(),
			[&](const HeldLoad& load) { return nowMs - load.atMs >= kPreloadHeldWindowMs; }), m_heldLoads.end());
	}

	// Whether the adapter's local video memory in use is at the share of its
	// budget that preload leaves alone. Asked once in kVideoCheckIntervalMs. An
	// adapter that cannot be asked is taken to have room: the byte cap still holds.
	bool Pump::VideoFull()
	{
		const uint64_t nowMs = m_host.NowUs() / 1000;
		if (nowMs >= m_nextVideoCheckMs)
		{
			m_nextVideoCheckMs = nowMs + kVideoCheckIntervalMs;
			uint64_t usage = 0, budget = 0;
			m_videoFull = m_host.VideoMemory(usage, budget) && budget > 0 && usage * 100 >= budget * kPreloadVideoPercent;
		}
		return m_videoFull;
	}

	void Pump::ResolveHeld()
	{
		m_held.erase(std::remove_if(m_held.begin(), m_held.end(), [this](const HeldItem& item) { return Resolved(item); }),
			m_held.end());
	}

	// True when the item is answered. An item whose icon is a texture becomes
	// the image of that texture's path: one id's worth of copy for both.
	bool Pump::Resolved(const HeldItem& item)
	{
		std::string path;
		if (!m_items.Find(item.key, path))
		{
			switch (m_items.GetState())
			{
			case ImageLookup::ItemIndex::State::Done:   // there is no item of that name
				m_library.Fail(item.id);
				++m_stats.failed;
				return true;
			case ImageLookup::ItemIndex::State::Failed:
				m_library.Fail(item.id);
				++m_stats.failed;
				return true;
			default:
				return false;
			}
		}

		if (path.empty() || ImageLookup::IsPlaceholderImage(path.c_str()))
		{
			m_library.PublishNoImage(item.id);
			++m_stats.noImage;
			return true;
		}

		const PluginImageId image = m_library.Path(path.c_str());
		if (image == 0)   // the table is full
		{
			m_library.Fail(item.id);
			++m_stats.failed;
			return true;
		}
		m_library.Requeue(item.id);
		m_library.Alias(item.id, image);
		if (m_saver)
			m_saver->Name(item.key, path);
		return true;
	}

	// The normalised path of the file this request would be read from, when the source has one.
	bool Pump::FileFor(const ImageLibrary::Request& request, std::string& key)
	{
		if (!m_source || m_sourceOff)
			return false;
		key =ImageLibrary::NormalizeObjectPath(request.name.c_str());
		return !key.empty() && m_source->Has(key);
	}

	Pump::Outcome Pump::Attempt(const ImageLibrary::Request& request, uint32_t& loads)
	{
		if (ImageLookup::IsPlaceholderImage(request.name.c_str()))
		{
			m_library.PublishNoImage(request.id);
			++m_stats.noImage;
			return Outcome::Done;
		}

		// A file first: no game image is read and no package is loaded for it.
		std::string key;
		if (FileFor(request, key))
		{
			if (!m_source->Request(request.id, key))
				return Outcome::Later;
			m_onDisk.insert(request.id);
			return Outcome::Disk;
		}

		// With the game's reads off, only an image that has a file can be served.
		if (m_off)
		{
			m_library.Fail(request.id);
			++m_stats.failed;
			return Outcome::Done;
		}

		// The first image that has to come from the game: the shrink is set up from here on.
		if (!m_shrinkPrepared)
		{
			m_shrinkPrepared = true;
			m_host.PrepareShrink();
		}

		Copy copy;
		ReadResult result = m_host.Read(request.name, copy);

		if (result == ReadResult::NotLoaded)
		{
			if (loads >= kLoadsPerTick)
				return Outcome::Later;
			++loads;
			switch (m_host.Load(request.name))
			{
			case LoadResult::Loaded:
				m_causedLoad.insert(request.id);
				result = m_host.Read(request.name, copy);
				break;
			case LoadResult::Missing:
				m_library.PublishNoImage(request.id);
				++m_stats.noImage;
				return Outcome::Done;
			case LoadResult::Fault:
				GameReadsOff("An engine call faulted");
				m_library.Fail(request.id);
				++m_stats.failed;
				return Outcome::Done;
			case LoadResult::Unsupported:
				m_library.Fail(request.id);
				++m_stats.failed;
				return Outcome::Done;
			}
		}

		// Still to be read again later: the load it caused stays on record until it is.
		bool causedLoad = false;
		if (result != ReadResult::NotYet && result != ReadResult::Busy)
		{
			causedLoad = m_causedLoad.erase(request.id) != 0;
			m_notYetSince.erase(request.id);
		}

		switch (result)
		{
		case ReadResult::Started:
			m_inFlight.push_back({ request.id, copy, request.name });
			if (causedLoad)
			{
				const uint64_t nowMs  = m_host.NowUs() / 1000;
				const uint64_t width  = static_cast<uint64_t>(copy.sourceWidth > 0 ? copy.sourceWidth : copy.width);
				const uint64_t height = static_cast<uint64_t>(copy.sourceHeight > 0 ? copy.sourceHeight : copy.height);
				PruneHeldLoads(nowMs);
				m_heldLoads.push_back({ nowMs, width * height * ImageShrink::kBytesPerPixel });
			}
			return Outcome::Started;
		case ReadResult::NotYet:
		{
			const uint64_t nowMs = m_host.NowUs() / 1000;
			const auto since = m_notYetSince.emplace(request.id, nowMs).first;
			if (nowMs - since->second < kNotYetGiveUpMs)
				return Outcome::Later;
			m_notYetSince.erase(since);
			m_causedLoad.erase(request.id);
			m_library.Fail(request.id);
			++m_stats.failed;
			return Outcome::Done;
		}
		case ReadResult::Busy:
			return Outcome::Busy;
		case ReadResult::Unavailable:
			GameReadsOff("No image can be read from the game on this build or in this session");
			m_library.Fail(request.id);
			++m_stats.failed;
			return Outcome::Done;
		case ReadResult::Fault:
			GameReadsOff("An engine call faulted");
			m_library.Fail(request.id);
			++m_stats.failed;
			return Outcome::Done;
		case ReadResult::NotLoaded:   // loaded a moment ago and not found: no texture there
			m_library.PublishNoImage(request.id);
			++m_stats.noImage;
			return Outcome::Done;
		case ReadResult::Failed:
		default:
			m_library.Fail(request.id);
			++m_stats.failed;
			return Outcome::Done;
		}
	}

	// From now on the game is not read for images this session; the images that have a file in the
	// cache folder are still served.
	void Pump::GameReadsOff(const char* why)
	{
		if (m_off)
			return;
		m_off = true;
		m_host.Log(std::string("[Images] ") + why + ": images are not read from the game for the rest of this session"
			", only the ones in the cache folder");
	}

	// The GPU side is gone: every request ends as NONE instead of waiting for ever.
	void Pump::FailQueued()
	{
		FailHeld();
		ImageLibrary::Request request;
		while (m_library.TakeNext(request))
		{
			m_library.Fail(request.id);
			++m_stats.failed;
		}
	}

	void Pump::FailHeld()
	{
		for (const HeldItem& item : m_held)
			m_library.Fail(item.id);
		m_stats.failed += m_held.size();
		m_held.clear();
	}

	void Pump::FreeDeferred()
	{
		if (!m_gone)
			for (PluginTextureHandle texture : m_toFree)
				m_host.Free(texture);
		m_toFree.clear();
	}

	void Pump::Shutdown()
	{
		if (m_stopped)
			return;
		m_stopped = true;
		if (m_saver)
			m_saver->Stop();
		if (m_source)
		{
			// What the worker finished and nobody collected is freed, and what it never got to fails.
			m_source->Stop();
			m_files.clear();
			m_source->Collect(m_files);
			for (const ImageDisk::FileResult& result : m_files)
				if (!m_gone && result.texture)
					m_host.Free(result.texture);
			m_files.clear();
			for (PluginImageId id : m_onDisk)
				m_library.Fail(id);
			m_onDisk.clear();
			m_preloadOnDisk.clear();
		}
		LogSummary();
		FailHeld();

		for (const InFlight& read : m_inFlight)
		{
			m_library.Fail(read.id);
			if (!m_gone)
				m_host.Free(read.copy.texture);
		}
		m_inFlight.clear();

		m_scratch.clear();
		m_library.ReleaseAll(m_scratch);
		if (!m_gone)
			for (const ImageLibrary::Released& released : m_scratch)
				m_host.Free(released.handle);
		m_scratch.clear();

		FreeDeferred();
	}

	void Pump::LogSummary()
	{
		const ImageLibrary::Library::PreloadProgress preload = m_library.GetPreloadProgress();
		m_stats.written  = m_saver ? m_saver->Written() : 0;
		m_stats.rejected = m_source ? m_source->Rejected() : 0;
		const bool changed = m_stats.published != m_lastLogged.published || m_stats.evicted != m_lastLogged.evicted
			|| m_stats.fromDisk != m_lastLogged.fromDisk || m_stats.fromGame != m_lastLogged.fromGame || m_stats.exactMade != m_lastLogged.exactMade
			|| m_stats.written != m_lastLogged.written || m_stats.rejected != m_lastLogged.rejected
			|| m_stats.failed != m_lastLogged.failed || m_stats.noImage != m_lastLogged.noImage
			|| preload.queued != m_lastPreload.queued || preload.done != m_lastPreload.done || m_pause != m_lastPreload.pause;
		if (!changed)
			return;
		const uint64_t longestUs = m_stats.longestTickUs;
		m_stats.longestTickUs = 0;
		m_lastLogged = m_stats;
		m_lastPreload = { preload.queued, preload.done, m_pause };

		char line[512];
		snprintf(line, sizeof(line),
			"[Images] requests=%u published=%llu evicted=%llu failed=%llu noimage=%llu kept=%u (%.1f MB) longest pump tick=%.3f ms"
			" preload queued=%u done=%u paused=%s"
			" cache: from disk=%llu from game=%llu written=%llu rejected=%llu exact=%llu",
			m_library.ImageCount(),
			static_cast<unsigned long long>(m_stats.published), static_cast<unsigned long long>(m_stats.evicted),
			static_cast<unsigned long long>(m_stats.failed), static_cast<unsigned long long>(m_stats.noImage),
			m_library.CountKept(), static_cast<double>(m_library.BytesKept()) / (1024.0 * 1024.0),
			static_cast<double>(longestUs) / 1000.0,
			preload.queued, preload.done, PauseText(static_cast<int>(m_pause)),
			static_cast<unsigned long long>(m_stats.fromDisk), static_cast<unsigned long long>(m_stats.fromGame),
			static_cast<unsigned long long>(m_stats.written), static_cast<unsigned long long>(m_stats.rejected),
			static_cast<unsigned long long>(m_stats.exactMade));
		m_host.Log(line);
	}
}
