#pragma once

// Saves the image library's stored copies to disk. The pump tells the saver when a shrink
// has started; the shrink's command list also copied the finished pixels into a readback
// buffer, and the saver polls for them each tick. A finished copy becomes a job for the
// worker (image_cache_writer.h), which writes the file. The list of images in each set is
// written the same way when it changes.
//
// The read side (Source, below) is a worker that finds out which files the folder holds, reads
// and checks a file, and uploads its pixels on a copy queue of its own (image_cache_reader.h).
// The pump asks it first for every image and falls back to the game when a file is missing,
// rejected or cannot be uploaded.
//
// Game thread only. No file I/O here: everything that touches the disk is a Job or the reader's.

#include "UI/image_cache_file.h"
#include "UI/image_cache_writer.h"
#include "UI/image_library.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ImageDisk
{
	// Milliseconds between looks at the sets. A change is written at most this often.
	inline constexpr uint64_t kSetListIntervalMs = 10 * 1000;

	// What the pump knows when a shrink has started.
	struct Started
	{
		std::string path;                 // the object path the image was asked for
		uint32_t    sourceFormat = 0;     // DXGI_FORMAT of the game image
		uint32_t    sourceWidth  = 0;
		uint32_t    sourceHeight = 0;
		uint32_t    storedWidth  = 0;
		uint32_t    storedHeight = 0;
		uint32_t    levels       = 0;
		uint32_t    ticket       = 0;     // the readback of the stored copy; 0 when none was made
	};

	// What the pump calls. The pump holds one, or none.
	class Saver
	{
	public:
		virtual ~Saver() = default;
		virtual void Begin(const Started& started) = 0;
		// An item name (as the library keys it) that resolved to a texture path. The saver keeps
		// the pairs and writes them as a name list when they change.
		virtual void Name(const std::string& name, const std::string& path) { (void)name; (void)path; }
		// Files the worker has written so far.
		virtual uint64_t Written() const { return 0; }
		// How many times the cache folder was cleared so far.
		virtual uint32_t Cleared() const { return 0; }
		// Once per tick after the pump has collected its reads.
		virtual void Poll(uint64_t nowMs) = 0;
		// The GPU side is gone: the readbacks will never finish.
		virtual void Abandon() = 0;
		// The engine is shutting down: stops the worker. Nothing is saved after this.
		virtual void Stop() = 0;
	};

	enum class Readback { Pending, Done, Gone };

	// What the saver needs from the engine and the GPU.
	class Host
	{
	public:
		virtual ~Host() = default;
		// Takes the pixels of a readback into dest (bytes long), or only frees the readback
		// when dest is null. Done and Gone free it; Pending leaves it.
		virtual Readback Take(uint32_t ticket, uint8_t* dest, size_t bytes) = 0;
		// The folder the files of the current game build and pak set go in. The pak set is checked now
		// (an empty function when it is not the session's any more); the returned function is called
		// by the worker, which is where the exe and the pak files are read, and gives an empty path
		// once the session's key is gone.
		virtual std::function<std::wstring()> CacheFolder() = 0;
		// How many times the cache was cleared so far. The two lists are written again after a change.
		virtual uint32_t ClearCount() const { return 0; }
	};

	// The session's folder key. The mounted pak set is listed on the game thread (no file I/O);
	// the sizes and times of the pak files are read by whoever asks for the folder, which is a
	// worker. The set taken on the first call is the session's: a later look that finds another set
	// (a pak mounted or unmounted, a different priority) switches the disk part off for the rest
	// of the session, as does a set that cannot be listed or a pak file that cannot be read. There
	// is never a key for part of the set.
	class PakSession
	{
	public:
		// Lists the mounted paks with path, mount point, order and file count; false when it cannot.
		using List = std::function<bool(std::vector<ImageCache::PakEntry>&)>;
		// The size and last-write time of a file; false when it is not there.
		using Stamp = std::function<bool(const std::string& path, ImageCache::FileStamp&)>;
		// The folder for a pak set hash.
		using FolderOf = std::function<std::wstring(uint64_t pakHash)>;
		using Log = std::function<void(const std::string&)>;

		PakSession(List list, Stamp stamp, FolderOf folderOf, Log log, uint64_t recheckMs = 250);

		// Game thread. The function that gives the folder (called by a worker), or an empty one when
		// the disk part is off. The first call takes the key. A later call looks at the pak set again
		// when `recheck` is set or recheckMs have passed since the last look.
		std::function<std::wstring()> Folder(uint64_t nowMs, bool recheck);

		// Worker. The folder in use, or empty before the key is taken and once the disk part is off.
		std::wstring CurrentFolder();

	private:
		struct State;
		std::shared_ptr<State> Current() const;

		List     m_list;
		Stamp    m_stamp;
		FolderOf m_folderOf;
		Log      m_log;
		uint64_t m_recheckMs;

		mutable std::mutex     m_lock;
		std::shared_ptr<State> m_state;         // written by the game thread, read by any
		uint64_t               m_shallow = 0;   // game thread only: the hash of the set without file stamps
		uint64_t               m_checkedMs = 0;
		std::function<std::wstring()> m_folder;
	};

	// ---- Reading -------------------------------------------------------------

	// How a file request ended.
	enum class FileOutcome
	{
		Texture,    // the file was good and its pixels are a texture now
		Rejected,   // the file failed a check and was deleted; the game is read instead
		Failed,     // the file could not be used now (read or upload failed); the game is read instead
		Same,       // an exact-size request only: the stored picture is no larger than the size asked for
	};

	struct FileResult
	{
		PluginImageId       id      = 0;
		FileOutcome         outcome = FileOutcome::Failed;
		PluginTextureHandle texture = nullptr;
		uint64_t            bytes   = 0;   // what the levels of the texture take

		// Set for the answer to RequestExact: the size asked for, and the texture's own size.
		uint32_t            exactSize = 0;
		uint32_t            width     = 0;
		uint32_t            height    = 0;
	};

	// What a session's worker finds in the folder when it opens it.
	struct Lists
	{
		ImageCache::NameList names;
		ImageCache::SetList  sets;
	};

	// What the pump calls to read the files back. The pump holds one, or none. The calls
	// are on the game thread; the file reads and the uploads are on the source's worker.
	class Source
	{
	public:
		enum class State
		{
			Loading,   // the folder is being listed; the game side waits
			Ready,
			Off,       // no file is used any more this session
		};

		virtual ~Source() = default;
		// Once per tick. The first call takes the session's key and starts the worker.
		virtual State Poll(uint64_t nowMs) = 0;
		// Whether a file for this normalised object path was in the folder and has not been
		// rejected since. No I/O.
		virtual bool Has(const std::string& key) = 0;
		// Reads, checks and uploads the file for the path; the answer comes through Collect.
		// False when the worker has no room: ask again later.
		virtual bool Request(PluginImageId id, const std::string& key) = 0;
		// Makes a copy of the stored picture in the file for the path at the size (the longer side, in
		// pixels): the worker reads and checks the file, resamples level 0 and uploads the result as a
		// texture of one level. The answer comes through Collect with exactSize set. The file stays
		// as it is and the listing is not changed, whatever comes of it. Busy when the worker has no
		// room or the folder is still being listed: ask again later. Refused when no copy can ever be
		// made for the path (a run-time texture has no file) or the files are off: do not ask again.
		enum class ExactAnswer { Started, Busy, Refused };
		virtual ExactAnswer RequestExact(PluginImageId id, const std::string& key, uint32_t size) { (void)id; (void)key; (void)size; return ExactAnswer::Refused; }
		// Appends the requests that have ended since the last call.
		virtual void Collect(std::vector<FileResult>& out) = 0;
		// Once after the folder was opened: the saved names and set lists. False before, and after.
		virtual bool TakeLists(Lists& out) = 0;
		// Files rejected so far.
		virtual uint64_t Rejected() const = 0;
		// The GPU side is gone: finished textures will never be collected.
		virtual void Abandon() = 0;
		// The engine is shutting down: joins the worker. What it had finished stays for Collect.
		virtual void Stop() = 0;
	};

	// ---- Writing -------------------------------------------------------------

	class DiskSaver final : public Saver
	{
	public:
		DiskSaver(ImageLibrary::Library& library, Host& host, ImageCache::Sink& sink);

		void Begin(const Started& started) override;
		void Name(const std::string& name, const std::string& path) override;
		uint64_t Written() const override { return m_sink.Written(); }
		uint32_t Cleared() const override { return m_host.ClearCount(); }
		void Poll(uint64_t nowMs) override;
		void Abandon() override;
		void Stop() override;

	private:
		void Finish(const Started& started, std::vector<uint8_t>&& pixels, std::function<std::wstring()> folder);
		void WriteSetList();
		void WriteNameList();

		ImageLibrary::Library& m_library;
		Host&                  m_host;
		ImageCache::Sink&      m_sink;

		struct Waiting
		{
			Started started;
			bool    wanted = true;   // false for an image that is not saved (a run-time object)
		};

		std::vector<Waiting>   m_pending;
		std::vector<uint8_t>   m_scratch;     // what a readback is taken into; grows, never shrinks
		uint32_t               m_clearSeen = 0;
		bool                   m_folderOff = false;   // the session's key is gone: nothing is copied or queued
		ImageCache::SetList    m_lastSets;
		std::map<std::string, std::string> m_names;   // name -> texture path, sorted so the file is stable
		bool                   m_namesChanged = false;
		uint64_t               m_nextSetCheckMs = 0;
		bool                   m_gone    = false;
		bool                   m_stopped = false;
	};

	// The loader's saver, on the loader's worker. Client builds only (image_disk_host.cpp).
	Saver* EngineSaver();
	// The loader's source, on its own worker, for the same folder. Client builds only.
	Source* EngineSource();

	// The "Clear image cache" option: queues deleting everything in the cache folder on the same
	// worker, also after the writer went off (disk full, read-only). Any thread, and no file I/O
	// here. False when the job was not queued (queue full, or the loader is shutting down). Images
	// already in memory stay; the next launch saves them again.
	bool ClearCache();
}
