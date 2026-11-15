#pragma once

// Bookkeeping for copies of engine textures that are still on the GPU.
// No D3D12 here: imgui_backend.cpp supplies the COM calls through an Ops type
// (static Release(void*) and Evict(void* pageable)), so the rules below can be
// tested without a GPU.

#include <cstdint>
#include <utility>

namespace IconCopy
{
	// At most this many copies on the GPU at once, and at most this many started
	// in one engine tick without waiting. A call over either limit first waits for
	// the oldest copy in flight (WaitBeforeStart), so a plugin that loads a whole
	// list in one pass still gets every icon.
	constexpr int kMaxInFlight = 8;
	constexpr int kMaxPerTick  = 8;

	// A parked hold whose residency request has finished is kept this long for the retry of the
	// same source, so the retry copies at once. A count given back too soon lets the engine page
	// the source out again, and the retry then pays the page-in again.
	constexpr uint64_t kParkGraceMs = 5000;

	// After a residency wait runs out, later requests are not waited for this long.
	constexpr uint64_t kResidentBackoffMs = 5000;

	class TickBudget
	{
	public:
		bool Available(uint64_t tick) const
		{
			return tick != m_tick || m_used < kMaxPerTick;
		}

		void Take(uint64_t tick)
		{
			if (tick != m_tick)
			{
				m_tick = tick;
				m_used = 0;
			}
			++m_used;
		}

	private:
		uint64_t m_tick = UINT64_MAX;
		int      m_used = 0;
	};

	// Set when a wait for a residency request ran out: for a while after, a request that has not
	// finished is parked without being waited for, so a card that is paging heavily costs one
	// wait, not one per source.
	class ResidentBackoff
	{
	public:
		void NoteTimeout(uint64_t nowMs)
		{
			m_until = nowMs + kResidentBackoffMs;
		}

		bool Active(uint64_t nowMs) const
		{
			return nowMs < m_until;
		}

	private:
		uint64_t m_until = 0;
	};

	// What keeps the source's memory resident while the copy runs.
	enum class Residency
	{
		Resource,  // the resource itself is made resident (a committed source)
		Heap,      // a placed source: the heap it lives in is made resident
	};

	// What the source's FD3D12Resource said about its heap.
	enum class HeapProbe
	{
		Committed,  // heap pointer null
		Heap,       // heap pointer checked out as the ID3D12Heap holding the resource
		Invalid,    // something was there but did not look like that heap
	};

	struct Protection
	{
		Residency   residency;
		const char* reduced;  // why protection is reduced, or nullptr for full protection
	};

	// Without the rows, or with a heap pointer that does not check out, the source
	// itself is still made resident: that works for a committed source, and D3D12
	// refuses it for a placed one, which is then held by reference and queue order.
	inline Protection Choose(bool rowsKnown, HeapProbe probe)
	{
		if (!rowsKnown)
			return { Residency::Resource, "the version map has no heap rows for this game build" };
		switch (probe)
		{
		case HeapProbe::Committed: return { Residency::Resource, nullptr };
		case HeapProbe::Heap:      return { Residency::Heap, nullptr };
		default:                   return { Residency::Resource, "the source's heap pointer did not look like a D3D12 heap" };
		}
	}

	// The references one copy holds until its fence passes. resident is the
	// pageable with a residency count of ours on it (the heap or the source),
	// not a separate reference.
	// The class check for an object found through a version map row: its vtable must be in
	// expectedModule, the module that implements the interface for objects made on our own
	// device. Ops::ModuleOf(address) gives the module an address is in, or null. Nothing is
	// called on the object. An unknown expected module refuses everything.
	template <class Ops>
	inline bool VtableIsFrom(const void* object, const void* expectedModule)
	{
		if (!object || !expectedModule)
			return false;
		const void* const vtable = *static_cast<const void* const*>(object);
		return vtable && Ops::ModuleOf(vtable) == expectedModule;
	}

	struct Hold
	{
		void* source   = nullptr;
		void* heap     = nullptr;
		void* dest     = nullptr;
		void* resident = nullptr;
	};

	template <class Ops>
	void Release(Hold& hold)
	{
		if (hold.resident) Ops::Evict(hold.resident);
		if (hold.heap)     Ops::Release(hold.heap);
		if (hold.source)   Ops::Release(hold.source);
		if (hold.dest)     Ops::Release(hold.dest);
		hold = {};
	}

	// A hold still being set up. Released when it goes out of scope, unless it was
	// handed to the ring (Take) or deliberately leaked (Abandon), so every early
	// return gives back what it took.
	template <class Ops>
	class PendingHold
	{
	public:
		PendingHold() = default;
		PendingHold(const PendingHold&) = delete;
		PendingHold& operator=(const PendingHold&) = delete;
		~PendingHold() { Release<Ops>(m_hold); }

		Hold& Get() { return m_hold; }
		Hold  Take() { return std::exchange(m_hold, Hold{}); }
		void  Abandon() { m_hold = {}; }

	private:
		Hold m_hold;
	};

	// The copies on the GPU, each with the fence value that marks it done.
	template <class Ops, int N = kMaxInFlight>
	class Ring
	{
	public:
		// Releases every copy whose fence value has passed. Returns how many.
		int Reclaim(uint64_t completed)
		{
			int released = 0;
			for (Entry& e : m_entries)
			{
				if (!e.busy || e.fence > completed)
					continue;
				Release<Ops>(e.hold);
				e = {};
				++released;
			}
			return released;
		}

		// A free entry's index, or -1 when all N are in flight.
		int FreeSlot() const
		{
			for (int i = 0; i < N; ++i)
				if (!m_entries[i].busy)
					return i;
			return -1;
		}

		void Start(int slot, uint64_t fence, Hold hold)
		{
			Entry& e = m_entries[slot];
			e.busy  = true;
			e.fence = fence;
			e.hold  = hold;
			if (fence > m_lastFence)
				m_lastFence = fence;
		}

		void ReleaseAll()
		{
			for (Entry& e : m_entries)
			{
				if (e.busy)
					Release<Ops>(e.hold);
				e = {};
			}
		}

		// Forgets every copy without releasing anything, for when the GPU may
		// still be reading them and nothing will say when it stops.
		void AbandonAll()
		{
			for (Entry& e : m_entries)
				e = {};
		}

		int InFlight() const
		{
			int n = 0;
			for (const Entry& e : m_entries)
				n += e.busy ? 1 : 0;
			return n;
		}

		uint64_t LastFence() const { return m_lastFence; }

		// The lowest fence value still in flight, or 0 when none is.
		uint64_t OldestFence() const
		{
			uint64_t oldest = 0;
			for (const Entry& e : m_entries)
				if (e.busy && (oldest == 0 || e.fence < oldest))
					oldest = e.fence;
			return oldest;
		}

	private:
		struct Entry
		{
			bool     busy  = false;
			uint64_t fence = 0;
			Hold     hold;
		};
		Entry    m_entries[N];
		uint64_t m_lastFence = 0;
	};

	// The holds of sources whose residency request had not finished in time (or, in try
	// mode, had not when asked). A residency count is given back (Evict) only after its
	// request has finished, so each hold waits here until the residency fence passes its
	// request's value. Requests finish in order.
	//
	// A hold whose request has finished stays for a grace period, keyed by its source: the
	// retry of the same source takes it (TakeFinished) and copies at once, with no second
	// request and no wait. A hold keeps no destination texture; the retry makes its own.
	template <class Ops, int N = kMaxInFlight>
	class Parked
	{
	public:
		enum class Find
		{
			None,      // nothing parked for the source
			Pending,   // its request has not finished
			Finished,  // its request has finished: TakeFinished gives the hold
		};

		bool Empty() const { return Count() == 0; }

		int Count() const
		{
			int n = 0;
			for (const Entry& e : m_entries)
				n += e.busy ? 1 : 0;
			return n;
		}

		// How many requests have not finished at residentDone.
		int Pending(uint64_t residentDone) const
		{
			int n = 0;
			for (const Entry& e : m_entries)
				n += (e.busy && e.value > residentDone) ? 1 : 0;
			return n;
		}

		// Takes the hold, to be released once the fence reaches residentValue and the grace
		// has passed. Its destination, if any, is released now. False, with the hold left as
		// it was, when all N entries are taken.
		bool Park(uint64_t residentValue, PendingHold<Ops>& hold, uint64_t nowMs)
		{
			Entry* free = nullptr;
			bool anyPending = false;
			for (Entry& e : m_entries)
			{
				if (!e.busy)
					free = free ? free : &e;
				else if (!e.finished)
					anyPending = true;
			}
			if (!free)
				return false;
			if (!anyPending)
				m_sinceMs = nowMs;
			Hold taken = hold.Take();
			if (taken.dest)
			{
				Ops::Release(taken.dest);
				taken.dest = nullptr;
			}
			*free = Entry{ true, false, residentValue, 0, taken };
			if (residentValue > m_lastValue)
				m_lastValue = residentValue;
			return true;
		}

		Find Lookup(const void* source, uint64_t residentDone) const
		{
			for (const Entry& e : m_entries)
				if (e.busy && e.hold.source == source)
					return e.value > residentDone ? Find::Pending : Find::Finished;
			return Find::None;
		}

		// Moves the finished hold for source into hold (which must be empty). False when
		// there is none, or its request has not finished.
		bool TakeFinished(const void* source, uint64_t residentDone, PendingHold<Ops>& hold)
		{
			for (Entry& e : m_entries)
			{
				if (!e.busy || e.hold.source != source)
					continue;
				if (e.value > residentDone)
					return false;
				hold.Get() = e.hold;
				e = {};
				return true;
			}
			return false;
		}

		// Releases the finished holds whose grace has passed. True when some requests are
		// still pending and none has finished for limitMs, counted from when the oldest
		// pending was parked or from the last one that finished.
		bool Reclaim(uint64_t residentDone, uint64_t nowMs, uint64_t limitMs, uint64_t graceMs)
		{
			bool anyPending = false;
			for (Entry& e : m_entries)
			{
				if (!e.busy)
					continue;
				if (e.value > residentDone)
				{
					anyPending = true;
					continue;
				}
				if (!e.finished)
				{
					e.finished = true;
					e.doneMs   = nowMs;
					m_sinceMs  = nowMs;
				}
				if (nowMs - e.doneMs >= graceMs)
				{
					Release<Ops>(e.hold);
					e = {};
				}
			}
			return anyPending && nowMs - m_sinceMs >= limitMs;
		}

		// True when every request ever parked has finished.
		bool AllDone(uint64_t residentDone) const { return residentDone >= m_lastValue; }

		void ReleaseAll()
		{
			for (Entry& e : m_entries)
			{
				if (e.busy)
					Release<Ops>(e.hold);
				e = {};
			}
		}

		void AbandonAll()
		{
			for (Entry& e : m_entries)
				e = {};
		}

	private:
		struct Entry
		{
			bool     busy     = false;
			bool     finished = false;  // seen finished by Reclaim, at doneMs
			uint64_t value    = 0;
			uint64_t doneMs   = 0;
			Hold     hold;
		};
		Entry    m_entries[N];
		uint64_t m_lastValue = 0;
		uint64_t m_sinceMs   = 0;
	};

	// The level a library copy reads: the smallest whose larger side is at least minSize, or
	// level 0 when there is none (an image smaller than that is read at its own size, never
	// scaled up). blockAlign is 4 for a block-compressed format, else 1: a level whose sides
	// are not a multiple of it cannot be made as a texture of its own, so it is skipped.
	struct LevelPick
	{
		uint32_t level;
		uint32_t width;
		uint32_t height;
	};

	inline LevelPick PickCopyLevel(uint32_t width, uint32_t height, uint32_t mipLevels, uint32_t minSize, uint32_t blockAlign)
	{
		LevelPick pick{ 0, width, height };
		for (uint32_t k = 1; k < mipLevels && k < 32; ++k)
		{
			const uint32_t w = (width >> k) > 1 ? (width >> k) : 1;
			const uint32_t h = (height >> k) > 1 ? (height >> k) : 1;
			if ((w > h ? w : h) < minSize)
				break;
			if (w % blockAlign != 0 || h % blockAlign != 0)
				continue;
			pick = { k, w, h };
		}
		return pick;
	}

	// The fence value a new copy waits for before it starts, or 0 to start now.
	// Within this tick's budget (haveTick false: no budget) and with an entry free
	// it starts at once; over either limit it waits for the oldest copy in flight.
	// Call after reclaiming the finished copies.
	template <class Ops, int N>
	uint64_t WaitBeforeStart(const Ring<Ops, N>& ring, const TickBudget& budget, bool haveTick, uint64_t tick)
	{
		const bool overTick = haveTick && !budget.Available(tick);
		if (!overTick && ring.FreeSlot() >= 0)
			return 0;
		return ring.OldestFence();
	}
}
