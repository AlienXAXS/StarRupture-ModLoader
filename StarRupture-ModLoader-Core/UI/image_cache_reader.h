#pragma once

// The worker that reads the image library's files back. Every file read of the library is done
// here, on a thread of its own: the game thread only asks for a file and collects the answers.
//
// At the start of a session the worker lists the folder's image files, and reads the saved name
// list and set lists. From then on the game thread knows from the listing, with no I/O, whether
// an image has a file (Has). A request for a file is read, checked (header, sizes, CRC, and that
// it holds the path asked for) and uploaded by the Uploader on this same thread; a file that
// fails a check is deleted, so the game is read and the file saved again. Nothing here waits on
// the game thread, and nothing the game thread does waits on the worker.
//
// The folder is decided by the session (image_disk_host.cpp): the key of the pak set is taken
// when the disk part is first used, and the part goes off for the session if it changes.
//
// Stop joins the thread. Nothing is read after Stop has been called: the job in hand finishes,
// queued jobs are dropped.

#include "UI/image_cache_file.h"
#include "UI/image_disk.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace ImageCache
{
	// Makes a texture of the stored copy's pixels. Called on the reader's worker thread only.
	class Uploader
	{
	public:
		virtual ~Uploader() = default;
		// pixels hold every level tightly packed (8-bit RGBA, level 0 first), bytes of them.
		// False when no texture could be made now.
		virtual bool Upload(uint32_t width, uint32_t height, uint32_t levels, const uint8_t* pixels, size_t bytes,
			PluginTextureHandle& texture) = 0;
	};

	class Reader final : public ImageDisk::Source
	{
	public:
		using FolderFn = std::function<std::wstring()>;
		// Game thread. The function that gives the session's folder (it is called by the worker), or an
		// empty function once the disk part is off for the session.
		using Session = std::function<FolderFn()>;
		using Log = std::function<void(const std::string&)>;

		// How long the game side waits for the folder to be listed, in milliseconds.
		static constexpr uint64_t kOpenWaitMs = 5000;
		static constexpr size_t   kMaxQueued  = 64;
		// A file bigger than this is not read.
		static constexpr uint64_t kMaxFileBytes = 16ull * 1024 * 1024;

		Reader(Session session, Uploader& uploader, Log log);
		~Reader();
		Reader(const Reader&) = delete;
		Reader& operator=(const Reader&) = delete;

		State Poll(uint64_t nowMs) override;
		bool  Has(const std::string& key) override;
		bool  Request(PluginImageId id, const std::string& key) override;
		ExactAnswer RequestExact(PluginImageId id, const std::string& key, uint32_t size) override;
		void  Collect(std::vector<ImageDisk::FileResult>& out) override;
		bool  TakeLists(ImageDisk::Lists& out) override;
		uint64_t Rejected() const override { return m_rejected; }
		void  Abandon() override;
		void  Stop() override;

	private:
		struct Job
		{
			bool          open = false;
			FolderFn      folder;    // open only
			PluginImageId id = 0;    // otherwise
			uint32_t      exactSize = 0;   // a copy for this size is wanted, not the stored one
			std::string   key;
		};

		bool Enqueue(Job&& job);
		void Run();
		void Open(const Job& job);
		void Load(const Job& job);
		void LoadExact(const Job& job);
		const char* Check(const std::vector<uint8_t>& bytes, const std::string& key, Info& info, const uint8_t*& pixels) const;
		void Finish(PluginImageId id, const std::string& key, ImageDisk::FileOutcome outcome, PluginTextureHandle texture, uint64_t bytes);
		void FinishExact(PluginImageId id, uint32_t size, ImageDisk::FileOutcome outcome, PluginTextureHandle texture, uint64_t bytes, uint32_t width, uint32_t height);
		void Reject(const std::wstring& path, const std::string& key, const char* why);

		Session   m_session;
		Uploader& m_uploader;
		Log       m_log;

		// Game thread.
		State     m_state       = State::Loading;
		bool      m_opening     = false;
		uint64_t  m_openedAtMs  = 0;

		mutable std::mutex      m_lock;
		std::condition_variable m_wake;
		std::deque<Job>         m_queue;
		std::thread             m_thread;
		bool                    m_stop      = false;
		bool                    m_abandoned = false;
		bool                    m_opened    = false;   // the listing is in
		bool                    m_listsTaken = false;
		std::unordered_set<std::string> m_index;       // file names that are in the folder
		ImageDisk::Lists                m_lists;
		std::vector<ImageDisk::FileResult> m_results;
		std::atomic<uint64_t>   m_rejected{ 0 };

		// Worker thread only, after the open job.
		std::wstring m_folder;
	};
}
