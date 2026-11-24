#include "mod_index_fetcher.h"
#include "feed_fetch.h"
#include "http_get.h"

#include <chrono>
#include <exception>
#include <thread>

namespace ModIndex
{
	// Everything the worker touches, held by shared_ptr: the worker keeps its own
	// share, so it never reaches freed memory if the Fetcher is destroyed first.
	struct Fetcher::Shared
	{
		std::mutex mutex;
		std::condition_variable idle;                   // signalled when the worker ends
		std::shared_ptr<const Snapshot> snapshot = std::make_shared<const Snapshot>();
		bool running = false;                           // a worker thread is alive
		bool stopped = false;                           // Shutdown was called
		std::string url;
		FetchFn fetch;
	};

	namespace
	{
		std::string DefaultFetch(const std::string& url)
		{
			HttpGetOptions options;
			options.resolveMs = options.connectMs = options.sendMs = options.receiveMs = FeedFetch::kOperationMs;
			options.deadlineMs = FeedFetch::kDeadlineMs;
			options.maxBytes = kMaxIndexBytes;
			return HttpGet(url.c_str(), "mod index", options);
		}

		std::shared_ptr<const Snapshot> Failed(const char* why)
		{
			auto s = std::make_shared<Snapshot>();
			s->state = State::Failed;
			s->error = why;
			return s;
		}

		// The worker. Runs one request and publishes the answer.
		void Work(std::shared_ptr<Fetcher::Shared> shared);
	}

	Fetcher::Fetcher(std::string url, FetchFn fetch)
		: m_shared(std::make_shared<Shared>())
	{
		m_shared->url = std::move(url);
		m_shared->fetch = fetch ? std::move(fetch) : FetchFn(&DefaultFetch);
	}

	Fetcher::~Fetcher()
	{
		Shutdown(0);
	}

	std::shared_ptr<const Snapshot> Fetcher::Get() const
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		return m_shared->snapshot;
	}

	void Fetcher::RequestFirstLoad()
	{
		Start(false);
	}

	void Fetcher::Retry()
	{
		Start(true);
	}

	// Checks the state, moves to Loading and starts the worker, all under one
	// lock, so a request that ends in between cannot be fetched over again and
	// two callers cannot both start one.
	void Fetcher::Start(bool retry)
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		if (m_shared->stopped || m_shared->running)
			return;

		const Snapshot& s = *m_shared->snapshot;
		if (retry)
		{
			if (!(s.state == State::Failed || (s.state == State::Ready && s.entries.empty())))
				return;
		}
		else if (s.state != State::Idle)
		{
			return;
		}

		auto loading = std::make_shared<Snapshot>();
		loading->state = State::Loading;
		m_shared->snapshot = loading;
		m_shared->running = true;

		try
		{
			std::thread(&Work, m_shared).detach();
		}
		catch (const std::exception&)
		{
			m_shared->running = false;
			m_shared->snapshot = Failed("Could not start the download.");
		}
	}

	bool Fetcher::Shutdown(unsigned waitMs)
	{
		std::unique_lock<std::mutex> lock(m_shared->mutex);
		m_shared->stopped = true;
		if (waitMs)
			m_shared->idle.wait_for(lock, std::chrono::milliseconds(waitMs), [this] { return !m_shared->running; });
		return !m_shared->running;
	}

	namespace
	{
		void Work(std::shared_ptr<Fetcher::Shared> shared)
		{
			std::shared_ptr<const Snapshot> answer;
			try
			{
				const std::string body = shared->fetch(shared->url);
				if (body.empty())
				{
					answer = Failed("Could not download the mod list. Check your connection and try again.");
				}
				else
				{
					ParseResult parsed = ParseModIndex(body);
					if (!parsed.ok)
					{
						answer = Failed(parsed.error.c_str());
					}
					else
					{
						auto ready = std::make_shared<Snapshot>();
						ready->state = State::Ready;
						ready->entries = std::move(parsed.entries);
						ready->skipped = parsed.skipped;
						ready->truncated = parsed.truncated;
						answer = ready;
					}
				}
			}
			catch (...)
			{
				answer = Failed("Could not read the mod list.");
			}

			std::lock_guard<std::mutex> lock(shared->mutex);
			shared->snapshot = std::move(answer);
			shared->running = false;
			shared->idle.notify_all();
		}
	}
}
