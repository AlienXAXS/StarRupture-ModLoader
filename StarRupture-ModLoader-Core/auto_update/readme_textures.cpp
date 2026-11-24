#include "auto_update/readme_textures.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <utility>
#include <vector>

namespace ModIndex
{
	// Shared with the posted job, so a job that runs after the owner is gone
	// still touches live memory.
	struct ReadmeTextures::State
	{
		mutable std::mutex mutex;
		std::condition_variable idle;       // signalled when a load that was running ends
		Picture pictures[kMaxPictures];
		uint64_t generation = 0;            // bumped by BeginPage, EndPage and Shutdown
		bool inFlight = false;              // a job is posted or running
		bool loading = false;               // the job is inside take/load/size right now
		bool stopped = false;
	};

	namespace
	{
		// Takes every held texture out of the state. The caller frees them after
		// the lock is released.
		void Collect(ReadmeTextures::State& s, std::vector<void*>& out)
		{
			for (Picture& p : s.pictures)
			{
				if (p.state == PictureState::Ready && p.handle)
					out.push_back(p.handle);
				p = Picture();
			}
		}

		void FreeAll(const TextureOps& ops, const std::vector<void*>& handles)
		{
			for (void* h : handles)
				if (ops.free)
					ops.free(h);
		}

		// The posted job. Game thread.
		void RunJob(const std::shared_ptr<ReadmeTextures::State>& st, const TextureOps& ops, uint64_t generation, size_t slot)
		{
			{
				std::lock_guard<std::mutex> lock(st->mutex);
				if (st->stopped || st->generation != generation)
				{
					// The page is gone: the bytes are not taken, and nothing is made.
					st->inFlight = false;
					return;
				}
				st->loading = true;
			}

			void* handle = nullptr;
			int width = 0;
			int height = 0;

			const std::string bytes = ops.take ? ops.take(slot) : std::string();
			if (!bytes.empty() && ops.load)
			{
				char name[48];
				snprintf(name, sizeof(name), "ModLoader.Readme.%llu.%zu", static_cast<unsigned long long>(generation), slot);
				try
				{
					handle = ops.load(bytes.data(), bytes.size(), name);
				}
				catch (const std::exception&)
				{
					handle = nullptr;   // no texture slot left, or the decode threw
				}
			}
			if (handle)
			{
				if (ops.size)
					ops.size(handle, &width, &height);
				if (width <= 0 || height <= 0 || width > kMaxTextureSide || height > kMaxTextureSide)
				{
					if (ops.free)
						ops.free(handle);
					handle = nullptr;
				}
			}

			void* discard = nullptr;
			{
				std::lock_guard<std::mutex> lock(st->mutex);
				if (!st->stopped && st->generation == generation)
				{
					Picture& p = st->pictures[slot];
					if (handle)
					{
						p.state = PictureState::Ready;
						p.handle = handle;
						p.width = width;
						p.height = height;
					}
					else
					{
						p.state = PictureState::Failed;
					}
				}
				else
				{
					discard = handle;   // closed while decoding: never shown
				}
				st->loading = false;
				st->inFlight = false;
			}
			st->idle.notify_all();
			if (discard && ops.free)
				ops.free(discard);
		}
	}

	ReadmeTextures::ReadmeTextures(TextureOps ops)
		: m_ops(std::move(ops))
		, m_state(std::make_shared<State>())
	{
	}

	ReadmeTextures::~ReadmeTextures()
	{
		Shutdown(1000);
	}

	void ReadmeTextures::NewGeneration()
	{
		std::vector<void*> handles;
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			++m_state->generation;
			Collect(*m_state, handles);
		}
		FreeAll(m_ops, handles);
	}

	void ReadmeTextures::BeginPage() { NewGeneration(); }

	void ReadmeTextures::EndPage() { NewGeneration(); }

	void ReadmeTextures::Request(size_t slot)
	{
		if (slot >= kMaxPictures)
			return;
		uint64_t generation = 0;
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			if (m_state->stopped || m_state->inFlight || m_state->pictures[slot].state != PictureState::None)
				return;
			m_state->pictures[slot].state = PictureState::Decoding;
			m_state->inFlight = true;
			generation = m_state->generation;
		}
		if (!m_ops.post)
		{
			std::lock_guard<std::mutex> lock(m_state->mutex);
			m_state->pictures[slot].state = PictureState::Failed;
			m_state->inFlight = false;
			return;
		}
		std::shared_ptr<State> st = m_state;
		TextureOps ops = m_ops;
		m_ops.post([st, ops, generation, slot]() { RunJob(st, ops, generation, slot); });
	}

	Picture ReadmeTextures::Get(size_t slot) const
	{
		if (slot >= kMaxPictures)
			return Picture();
		std::lock_guard<std::mutex> lock(m_state->mutex);
		return m_state->pictures[slot];
	}

	size_t ReadmeTextures::Held() const
	{
		std::lock_guard<std::mutex> lock(m_state->mutex);
		size_t n = 0;
		for (const Picture& p : m_state->pictures)
			if (p.state == PictureState::Ready)
				++n;
		return n;
	}

	bool ReadmeTextures::Shutdown(unsigned waitMs)
	{
		std::vector<void*> handles;
		bool idle = false;
		{
			std::unique_lock<std::mutex> lock(m_state->mutex);
			m_state->stopped = true;
			++m_state->generation;
			Collect(*m_state, handles);
			idle = m_state->idle.wait_for(lock, std::chrono::milliseconds(waitMs), [this] { return !m_state->loading; });
		}
		FreeAll(m_ops, handles);
		return idle;
	}
}
