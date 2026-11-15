#pragma once

// The worker that writes the image library's files. Every file write of the library is
// done here, on a thread of its own: the game thread and the render thread only queue a job.
//
// A job names a folder and a file and holds a function that makes the file's bytes, which
// the worker calls (so the CRC and the copy are not paid on the game thread). The worker
// writes <name>.<pid>.tmp and renames it over the file, so two game instances never see a
// half-written file. There is no flush to the device: the file's checksum catches a torn one.
// A file that is already there with the same size is left alone. A job whose folder function
// gives an empty path (its key is no longer the session's) is dropped unwritten.
//
// The queue is bounded: when it is full a job is refused and that image is not saved now.
// A folder that cannot be made or written (read-only, full disk, gone) switches the whole disk
// part off for the session, with one log line. One file that cannot be written because something
// else holds it (a virus scanner, a reader) is tried a few times with a short pause and then
// skipped; only when a fresh probe file cannot be made in the folder either does the disk part
// go off. A job marked runWhenOff is still taken then, and kept when the queue is dropped: it
// writes nothing, it only deletes (the "Clear image cache" option, which is what a player presses
// when the disk is full).
//
// Stop joins the thread. Nothing is written after Stop has been called: the job in hand
// finishes, queued jobs are dropped.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ImageCache
{
	struct Job
	{
		std::function<std::wstring()>         folder;   // the full path of the folder the file goes in; called by the worker
		std::string                           name;     // file name inside it
		std::function<std::vector<uint8_t>()> make;     // the bytes; empty means nothing to write
		bool                                  replace = false;   // write even when a file of that size is there
		bool                                  runWhenOff = false;   // taken even after the disk part went off (deleting jobs)
	};

	// What the saver queues jobs on.
	class Sink
	{
	public:
		virtual ~Sink() = default;
		// Any thread. False when the job was not queued (full, off, or stopped).
		virtual bool Submit(Job&& job) = 0;
		virtual bool Off() const = 0;
		// How many files the worker has written (renamed into place) so far.
		virtual uint64_t Written() const { return 0; }
		// Joins the worker; nothing is written after this. Idempotent.
		virtual void Stop() = 0;
	};

	class Writer final : public Sink
	{
	public:
		using Log = std::function<void(const std::string&)>;

		explicit Writer(Log log, size_t maxQueued = kDefaultQueue);
		~Writer();
		Writer(const Writer&) = delete;
		Writer& operator=(const Writer&) = delete;

		bool Submit(Job&& job) override;
		bool Off() const override;
		uint64_t Written() const override { return m_written; }

		void Stop() override;

		static constexpr size_t kDefaultQueue = 32;

	private:
		void Run();
		bool Write(const Job& job);   // false: the disk part must go off
		void SwitchOff(const char* step, unsigned long error, const std::wstring& folder);

		Log                     m_log;
		size_t                  m_maxQueued;
		mutable std::mutex      m_lock;
		std::condition_variable m_wake;
		std::deque<Job>         m_queue;
		std::thread             m_thread;
		bool                    m_stop = false;
		bool                    m_off  = false;
		std::atomic<uint64_t>   m_written{ 0 };
	};
}
