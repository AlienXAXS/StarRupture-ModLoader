#include "auto_update/readme_fetcher.h"

#include "auto_update/feed_fetch.h"
#include "auto_update/http_get.h"

#include <chrono>
#include <exception>
#include <thread>
#include <utility>

namespace ModIndex
{
	// Everything the worker touches, held by shared_ptr: the worker keeps its own
	// share, so it never reaches freed memory if the fetcher is destroyed first.
	struct ReadmeFetcher::Shared
	{
		std::mutex mutex;
		std::condition_variable idle;                   // signalled when the worker ends
		ReadmeSnapshot cur;                             // the working copy; Publish() freezes it
		std::shared_ptr<const ReadmeSnapshot> snapshot = std::make_shared<const ReadmeSnapshot>();
		std::string bytes[kMaxImages];                  // fetched pictures not yet taken
		size_t fetchedTotal = 0;                        // bytes accepted for the open README
		uint64_t generation = 0;                        // bumped by Open, Close and Shutdown: older work is discarded
		bool running = false;                           // a worker thread is alive
		bool stopped = false;                           // Shutdown was called
		bool hasJob = false;
		std::string jobEntryId;
		std::string jobUrl;
		uint64_t jobGeneration = 0;
		ReadmeFetchFn fetch;

		// Lock held.
		void Publish() { snapshot = std::make_shared<const ReadmeSnapshot>(cur); }

		// Lock held: forgets the open README and everything fetched for it.
		void Reset()
		{
			++generation;
			hasJob = false;
			fetchedTotal = 0;
			for (std::string& b : bytes)
				std::string().swap(b);
			cur = ReadmeSnapshot();
		}

		// Lock held.
		void Fail(const std::string& why)
		{
			cur.state = ReadmeState::Failed;
			cur.document.reset();
			cur.images.clear();
			cur.error = why;
			Publish();
		}
	};

	namespace
	{
		void Work(std::shared_ptr<ReadmeFetcher::Shared> shared);

		void RunJob(ReadmeFetcher::Shared& s, const std::string& url, uint64_t gen)
		{
			std::string error;
			const std::string text = s.fetch(url, kMaxFetchBytes, error);
			if (text.empty() || text.size() > kMaxFetchBytes)
			{
				std::lock_guard<std::mutex> lock(s.mutex);
				if (gen != s.generation)
					return;
				if (text.empty())
					s.Fail(error.empty() ? "Could not download the readme. Check your connection and try again." : error);
				else
					s.Fail("The readme is too large to show.");
				return;
			}

			// Parse and number the pictures outside the lock.
			auto doc = std::make_shared<Document>(ParseReadme(text, url));
			std::vector<ImageSlot> slots;
			for (Block& b : doc->blocks)
			{
				if (b.kind != BlockKind::Image || b.url.empty() || !IsAllowedImageUrl(b.url))
					continue;
				size_t found = slots.size();
				for (size_t i = 0; i < slots.size(); ++i)
					if (slots[i].url == b.url)
						found = i;
				if (found == slots.size())
				{
					if (slots.size() >= kMaxImages)
						continue;       // a ninth picture: the view draws its alt text
					ImageSlot slot;
					slot.url = b.url;
					slots.push_back(std::move(slot));
				}
				b.imageSlot = static_cast<int>(found);
			}
			const size_t count = slots.size();

			{
				std::lock_guard<std::mutex> lock(s.mutex);
				if (gen != s.generation)
					return;
				s.cur.state = ReadmeState::Ready;
				s.cur.document = std::move(doc);
				s.cur.images = std::move(slots);
				s.Publish();
			}

			for (size_t i = 0; i < count; ++i)
			{
				std::string imageUrl;
				{
					std::lock_guard<std::mutex> lock(s.mutex);
					if (gen != s.generation)
						return;
					imageUrl = s.cur.images[i].url;
					s.cur.images[i].state = ImageState::Loading;
					s.Publish();
				}

				std::string imageError;
				std::string body = s.fetch(imageUrl, kMaxImageBytes, imageError);
				std::string refusal;
				if (body.empty())
					refusal = imageError.empty() ? "Could not download this picture." : imageError;
				else if (body.size() > kMaxImageBytes)
					refusal = "This picture is too large.";
				else if (LooksLikeMarkup(body) || DetectImageFormat(body) == ImageFormat::None)
					refusal = "This is not a picture the loader can show.";

				std::lock_guard<std::mutex> lock(s.mutex);
				if (gen != s.generation)
					return;
				ImageSlot& slot = s.cur.images[i];
				if (refusal.empty() && s.fetchedTotal + body.size() > kMaxImageTotalBytes)
					refusal = "This README has more picture data than the loader will load.";
				if (!refusal.empty())
				{
					slot.state = ImageState::Failed;
					slot.error = refusal;
				}
				else
				{
					slot.state = ImageState::Bytes;
					slot.size = body.size();
					s.fetchedTotal += body.size();
					s.bytes[i] = std::move(body);
				}
				s.Publish();
			}
		}

		void Work(std::shared_ptr<ReadmeFetcher::Shared> shared)
		{
			ReadmeFetcher::Shared& s = *shared;
			for (;;)
			{
				std::string url;
				uint64_t gen = 0;
				{
					std::lock_guard<std::mutex> lock(s.mutex);
					if (s.stopped || !s.hasJob)
					{
						s.running = false;
						s.idle.notify_all();
						return;
					}
					s.hasJob = false;
					url = s.jobUrl;
					gen = s.jobGeneration;
				}

				try
				{
					RunJob(s, url, gen);
				}
				catch (...)
				{
					std::lock_guard<std::mutex> lock(s.mutex);
					if (gen == s.generation)
						s.Fail("Could not read the readme.");
				}
			}
		}
	}

	ReadmeFetcher::ReadmeFetcher(ReadmeFetchFn fetch)
		: m_shared(std::make_shared<Shared>())
	{
		m_shared->fetch = std::move(fetch);
	}

	ReadmeFetcher::~ReadmeFetcher()
	{
		Shutdown(0);
	}

	std::shared_ptr<const ReadmeSnapshot> ReadmeFetcher::Get() const
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		return m_shared->snapshot;
	}

	bool ReadmeFetcher::Open(const std::string& entryId, const std::string& readmeUrl)
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		if (m_shared->stopped)
			return false;

		m_shared->Reset();
		m_shared->cur.entryId = entryId;
		if (!IsAllowedReadmeUrl(readmeUrl))
		{
			m_shared->Fail("This mod has no readme the loader can show.");
			return false;
		}

		m_shared->cur.state = ReadmeState::Loading;
		m_shared->Publish();
		m_shared->hasJob = true;
		m_shared->jobEntryId = entryId;
		m_shared->jobUrl = readmeUrl;
		m_shared->jobGeneration = m_shared->generation;

		if (!m_shared->running)
		{
			m_shared->running = true;
			try
			{
				std::thread(&Work, m_shared).detach();
			}
			catch (const std::exception&)
			{
				m_shared->running = false;
				m_shared->hasJob = false;
				m_shared->Fail("Could not start the download.");
				return false;
			}
		}
		return true;
	}

	void ReadmeFetcher::Close()
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		m_shared->Reset();
		m_shared->Publish();
	}

	std::string ReadmeFetcher::TakeImageBytes(size_t slot)
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		Shared& s = *m_shared;
		if (slot >= s.cur.images.size() || slot >= kMaxImages)
			return {};
		ImageSlot& image = s.cur.images[slot];
		if (image.state != ImageState::Bytes || image.taken)
			return {};
		image.taken = true;
		std::string out = std::move(s.bytes[slot]);
		std::string().swap(s.bytes[slot]);
		s.Publish();
		return out;
	}

	size_t ReadmeFetcher::BytesHeld() const
	{
		std::lock_guard<std::mutex> lock(m_shared->mutex);
		size_t total = 0;
		for (const std::string& b : m_shared->bytes)
			total += b.size();
		return total;
	}

	bool ReadmeFetcher::Shutdown(unsigned waitMs)
	{
		std::unique_lock<std::mutex> lock(m_shared->mutex);
		m_shared->stopped = true;
		m_shared->Reset();
		m_shared->Publish();
		if (waitMs)
			m_shared->idle.wait_for(lock, std::chrono::milliseconds(waitMs), [this] { return !m_shared->running; });
		return !m_shared->running;
	}

	namespace
	{
		std::string LoaderFetch(const std::string& url, size_t maxBytes, std::string& error)
		{
			HttpGetOptions options;
			options.resolveMs = options.connectMs = options.sendMs = options.receiveMs = FeedFetch::kOperationMs;
			options.deadlineMs = FeedFetch::kDeadlineMs;
			options.maxBytes = maxBytes;
			std::string body = HttpGet(url.c_str(), "mod readme", options);
			if (body.empty())
				error.clear();
			return body;
		}
	}

	ReadmeFetcher& ReadmeInstance()
	{
		static ReadmeFetcher instance(&LoaderFetch);
		return instance;
	}

	bool ShutdownReadmeInstance(unsigned waitMs)
	{
		return ReadmeInstance().Shutdown(waitMs);
	}
}
