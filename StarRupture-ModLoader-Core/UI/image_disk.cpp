#include "UI/image_disk.h"
#include "UI/image_shrink.h"

#include <algorithm>
#include <memory>

namespace ImageDisk
{
	DiskSaver::DiskSaver(ImageLibrary::Library& library, Host& host, ImageCache::Sink& sink)
		: m_library(library), m_host(host), m_sink(sink)
	{
	}

	void DiskSaver::Begin(const Started& started)
	{
		// No ticket: every readback buffer was taken, and this image stays in memory only.
		if (started.ticket == 0 || m_gone || m_stopped)
			return;
		Waiting waiting;
		waiting.started = started;
		// A run-time object's name is handed out again for another texture next session, so only
		// its readback is freed.
		waiting.wanted = !ImageCache::IsTransientPath(ImageLibrary::NormalizeObjectPath(started.path.c_str()));
		m_pending.push_back(std::move(waiting));
	}

	void DiskSaver::Poll(uint64_t nowMs)
	{
		if (m_gone || m_stopped)
			return;

		// The pak set is looked at when the first image is filed in a tick and the answer serves the rest of
		// the tick: a pak is only mounted on the game thread, between ticks of this loop.
		std::function<std::wstring()> folder;
		bool folderAsked = false;

		for (size_t i = 0; i < m_pending.size();)
		{
			const Started& started = m_pending[i].started;
			const size_t bytes = static_cast<size_t>(ImageShrink::Bytes({ started.storedWidth, started.storedHeight }, started.levels));

			// With no use for the pixels (the disk part is off, or this image is not saved) the
			// readback is only freed.
			const bool wanted = m_pending[i].wanted && !m_folderOff && !m_sink.Off();
			if (wanted && m_scratch.size() < bytes)
				m_scratch.resize(bytes);

			const Readback state = m_host.Take(started.ticket, wanted ? m_scratch.data() : nullptr, bytes);
			if (state == Readback::Pending)
			{
				++i;
				continue;
			}
			if (state == Readback::Done && wanted)
			{
				// An empty function means the session's key is gone: nothing is queued, and nothing more is
				// copied.
				if (!folderAsked)
				{
					folder = m_host.CacheFolder();
					folderAsked = true;
				}
				if (folder)
					Finish(started, std::vector<uint8_t>(m_scratch.begin(), m_scratch.begin() + static_cast<std::ptrdiff_t>(bytes)), folder);
				else
					m_folderOff = true;
			}
			m_pending.erase(m_pending.begin() + static_cast<std::ptrdiff_t>(i));
		}

		if (nowMs >= m_nextSetCheckMs)
		{
			m_nextSetCheckMs = nowMs + kSetListIntervalMs;
			// A cleared cache has lost both lists: they are written again.
			const uint32_t cleared = m_host.ClearCount();
			if (cleared != m_clearSeen)
			{
				m_clearSeen = cleared;
				m_lastSets.clear();
				m_namesChanged = !m_names.empty();
			}
			WriteSetList();
			WriteNameList();
		}
	}

	void DiskSaver::Name(const std::string& name, const std::string& path)
	{
		if (m_stopped || name.empty() || path.empty() || ImageCache::IsTransientPath(ImageLibrary::NormalizeObjectPath(path.c_str())))
			return;
		std::string& kept = m_names[name];
		if (kept != path)
		{
			kept = path;
			m_namesChanged = true;
		}
	}

	void DiskSaver::Finish(const Started& started, std::vector<uint8_t>&& pixels, std::function<std::wstring()> folder)
	{
		const std::string key = ImageLibrary::NormalizeObjectPath(started.path.c_str());
		if (key.empty())
			return;

		ImageCache::Info info;
		info.sourceFormat = started.sourceFormat;
		info.sourceWidth  = started.sourceWidth;
		info.sourceHeight = started.sourceHeight;
		info.storedWidth  = started.storedWidth;
		info.storedHeight = started.storedHeight;
		info.levels       = started.levels;
		info.path         = started.path;

		const auto data = std::make_shared<std::vector<uint8_t>>(std::move(pixels));
		ImageCache::Job job;
		job.folder = std::move(folder);
		job.name   = ImageCache::FileName(key);
		job.make   = [info, data] { return ImageCache::Serialize(info, data->data(), data->size()); };
		m_sink.Submit(std::move(job));
	}

	// Writes the list of images in each set when it is not the one last written.
	void DiskSaver::WriteSetList()
	{
		std::vector<ImageLibrary::SetMember> members;
		m_library.CollectSets(members);

		ImageCache::SetList sets;
		for (uint32_t bit = 0; bit < 32; ++bit)
		{
			ImageCache::SetEntry entry;
			entry.set = 1u << bit;
			for (const ImageLibrary::SetMember& member : members)
				if ((member.sets & entry.set) && !ImageCache::IsTransientPath(ImageLibrary::NormalizeObjectPath(member.path.c_str())))
					entry.paths.push_back(member.path);
			if (entry.paths.empty())
				continue;
			std::sort(entry.paths.begin(), entry.paths.end());
			entry.paths.erase(std::unique(entry.paths.begin(), entry.paths.end()), entry.paths.end());
			sets.push_back(std::move(entry));
		}
		if (sets.empty() || sets == m_lastSets)
			return;

		ImageCache::Job job;
		job.folder  = m_host.CacheFolder();
		if (!job.folder)
			return;
		job.name    = ImageCache::SetListFileName();
		job.replace = true;
		job.make    = [sets] { return ImageCache::SerializeSets(sets); };
		if (m_sink.Submit(std::move(job)))
			m_lastSets = std::move(sets);
	}

	// Writes the item names that resolved to a texture when there are new ones.
	void DiskSaver::WriteNameList()
	{
		if (!m_namesChanged)
			return;

		ImageCache::NameList names;
		names.reserve(m_names.size());
		for (const auto& entry : m_names)
			names.push_back({ entry.first, entry.second });

		ImageCache::Job job;
		job.folder  = m_host.CacheFolder();
		if (!job.folder)
			return;
		job.name    = ImageCache::NameListFileName();
		job.replace = true;
		job.make    = [names] { return ImageCache::SerializeNames(names); };
		if (m_sink.Submit(std::move(job)))
			m_namesChanged = false;
	}

	void DiskSaver::Abandon()
	{
		m_gone = true;
		m_pending.clear();
	}

	void DiskSaver::Stop()
	{
		m_stopped = true;
		m_pending.clear();
		m_sink.Stop();
	}

	// ---- The session's folder key ---------------------------------------------

	struct PakSession::State
	{
		Stamp    stamp;
		FolderOf folderOf;
		Log      log;

		std::mutex                        lock;
		bool                              off = false;
		std::vector<ImageCache::PakEntry> paks;   // set before the state is shared; the worker adds the stamps

		std::mutex keyLock;                       // held by the worker that reads the file stamps
		bool       keyed = false;
		uint64_t   key   = 0;

		bool IsOff()
		{
			std::lock_guard<std::mutex> guard(lock);
			return off;
		}

		void SwitchOff(const std::string& why)
		{
			{
				std::lock_guard<std::mutex> guard(lock);
				if (off)
					return;
				off = true;
			}
			if (log)
				log("[Images] " + why + ": the image cache is not used for the rest of this session");
		}

		// "x.pak" -> "x.utoc": the files an IoStore pak keeps beside it.
		static std::string Sibling(const std::string& pak, const char* extension)
		{
			const size_t dot = pak.find_last_of('.');
			const size_t slash = pak.find_last_of("/\\");
			const bool hasExtension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
			return (hasExtension ? pak.substr(0, dot) : pak) + extension;
		}

		// Worker only: reads the file stamps the first time.
		std::wstring Folder()
		{
			if (IsOff())
				return {};
			{
				std::lock_guard<std::mutex> guard(keyLock);
				if (!keyed)
				{
					for (ImageCache::PakEntry& pak : paks)
					{
						if (!stamp(pak.path, pak.pak))
						{
							SwitchOff("A mounted pak file could not be read (" + pak.path + ")");
							return {};
						}
						stamp(Sibling(pak.path, ".utoc"), pak.utoc);
						stamp(Sibling(pak.path, ".ucas"), pak.ucas);
					}
					key   = ImageCache::HashPakSet(paks);
					keyed = true;
				}
			}
			return IsOff() ? std::wstring() : folderOf(key);
		}
	};

	PakSession::PakSession(List list, Stamp stamp, FolderOf folderOf, Log log, uint64_t recheckMs)
		: m_list(std::move(list)), m_stamp(std::move(stamp)), m_folderOf(std::move(folderOf)), m_log(std::move(log)), m_recheckMs(recheckMs)
	{
	}

	std::shared_ptr<PakSession::State> PakSession::Current() const
	{
		std::lock_guard<std::mutex> guard(m_lock);
		return m_state;
	}

	std::function<std::wstring()> PakSession::Folder(uint64_t nowMs, bool recheck)
	{
		const std::shared_ptr<State> state = Current();
		if (state && state->IsOff())
			return {};

		std::vector<ImageCache::PakEntry> paks;
		if (!state)
		{
			const auto fresh = std::make_shared<State>();
			fresh->stamp    = m_stamp;
			fresh->folderOf = m_folderOf;
			fresh->log      = m_log;
			m_checkedMs = nowMs;
			const bool listed = m_list && m_list(paks) && !paks.empty();
			if (listed)
			{
				m_shallow   = ImageCache::HashPakSet(paks);
				fresh->paks = std::move(paks);
				m_folder    = [fresh] { return fresh->Folder(); };
			}
			else
				fresh->SwitchOff("The mounted paks cannot be listed");

			// Shared only now: a worker never sees a state without its pak set.
			{
				std::lock_guard<std::mutex> guard(m_lock);
				m_state = fresh;
			}
			if (!listed)
				return {};
		}
		else if (recheck || nowMs - m_checkedMs >= m_recheckMs)
		{
			m_checkedMs = nowMs;
			if (!m_list(paks) || ImageCache::HashPakSet(paks) != m_shallow)
			{
				state->SwitchOff("The mounted paks changed");
				return {};
			}
		}
		return m_folder;
	}

	std::wstring PakSession::CurrentFolder()
	{
		const std::shared_ptr<State> state = Current();
		return state ? state->Folder() : std::wstring();
	}
}
