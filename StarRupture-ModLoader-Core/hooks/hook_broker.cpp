#include "hooks/hook_broker.h"
#include "hooks/hooks_common.h"
#include "logging/log.h"

#include <mutex>

namespace Hooks::Broker
{
	namespace
	{
		// --- Thunk arena --------------------------------------------------------
		//
		// 16 bytes each, 16-byte aligned:
		//
		//   +0  FF 25 02 00 00 00     jmp qword [rip+2]
		//   +6  90 90                 padding
		//   +8  <8-byte slot>         where the jump actually goes
		//
		// The padding is the point. `jmp qword [rip+0]` would put the slot at +6,
		// which is never 8-byte aligned, and an unaligned 8-byte store is not
		// atomic -- a thread mid-call could read half of an old pointer and half
		// of a new one. Displacement 2 moves the slot to +8, and at a 16-aligned
		// base that is 8-aligned, so rewiring a link is a single atomic store.
		//
		// Nothing here is ever freed. A thread can be executing inside a thunk at
		// the moment its link is detached, and there is no way to know when it has
		// left. 16 bytes per hook is not worth a race nobody can reproduce.
		constexpr size_t kThunkSize    = 16;
		constexpr size_t kThunkSlotOff = 8;
		constexpr size_t kArenaSize    = 64 * 1024;

		uint8_t* g_arena       = nullptr;
		size_t   g_arenaUsed   = 0;

		uint8_t* AllocateThunk()
		{
			if (!g_arena || g_arenaUsed + kThunkSize > kArenaSize)
			{
				g_arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, kArenaSize,
					MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
				g_arenaUsed = 0;

				if (!g_arena)
				{
					LogToFile::Error("[HookBroker] Failed to allocate thunk arena (%lu)", GetLastError());
					return nullptr;
				}
			}

			uint8_t* thunk = g_arena + g_arenaUsed;
			g_arenaUsed += kThunkSize;

			thunk[0] = 0xFF;          // jmp qword [rip+2]
			thunk[1] = 0x25;
			thunk[2] = 0x02;
			thunk[3] = 0x00;
			thunk[4] = 0x00;
			thunk[5] = 0x00;
			thunk[6] = 0x90;          // padding, so the slot lands 8-byte aligned
			thunk[7] = 0x90;
			*reinterpret_cast<uint64_t*>(thunk + kThunkSlotOff) = 0;

			return thunk;
		}

		// The one write that happens while the game may be running through it.
		// Aligned, 8 bytes, so a thread reads either the old destination or the
		// new one -- both of which are a valid place to go.
		void SetThunkTarget(uint8_t* thunk, uintptr_t destination)
		{
			InterlockedExchange64(
				reinterpret_cast<volatile LONG64*>(thunk + kThunkSlotOff),
				static_cast<LONG64>(destination));
		}

		uintptr_t GetThunkTarget(const uint8_t* thunk)
		{
			return static_cast<uintptr_t>(
				*reinterpret_cast<const volatile LONG64*>(thunk + kThunkSlotOff));
		}

		// --- Chains -------------------------------------------------------------

		struct Link
		{
			uint64_t    id     = 0;
			std::string owner;
			std::string name;
			uintptr_t   detour = 0;
			uint8_t*    thunk  = nullptr;   // this link's `original`
		};

		struct Chain
		{
			uintptr_t         target     = 0;
			uint8_t*          headThunk  = nullptr;  // what the prologue JMP points at
			uintptr_t         trampoline = 0;        // the real original
			Hooks::Hook       prologue;              // owns the patch, one per target
			std::vector<Link> links;                 // links[0] runs first
		};

		std::mutex         g_mutex;
		std::vector<Chain> g_chains;
		uint64_t           g_nextLinkId = 1;

		Chain* FindChain(uintptr_t target)
		{
			for (Chain& chain : g_chains)
				if (chain.target == target)
					return &chain;
			return nullptr;
		}

		// Where the link before `index` currently sends control. For index 0 that
		// is the head thunk the prologue jumps to; otherwise the previous link's
		// own thunk. Rewriting this slot is how a link is spliced in or out.
		uint8_t* PredecessorSlot(Chain& chain, size_t index)
		{
			return index == 0 ? chain.headThunk : chain.links[index - 1].thunk;
		}
	}

	LinkId Attach(uintptr_t target, void* detour, void** outOriginal,
	              const char* owner, const char* name)
	{
		if (outOriginal)
			*outOriginal = nullptr;

		if (!target || !detour || !outOriginal)
		{
			LogToFile::Error("[HookBroker] Attach refused: target=0x%llX detour=0x%llX out=%s",
				static_cast<unsigned long long>(target),
				static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(detour)),
				outOriginal ? "ok" : "null");
			return LinkId{};
		}

		std::lock_guard<std::mutex> lock(g_mutex);

		Chain* chain = FindChain(target);

		if (!chain)
		{
			// First link on this address: this is the only time game code is
			// written on the way in.
			Chain fresh;
			fresh.target    = target;
			fresh.headThunk = AllocateThunk();
			if (!fresh.headThunk)
				return LinkId{};

			void* trampoline = nullptr;
			if (!fresh.prologue.InstallRaw(target, fresh.headThunk, &trampoline))
			{
				LogToFile::Error("[HookBroker] Could not install the prologue patch at 0x%llX",
					static_cast<unsigned long long>(target));
				return LinkId{};
			}

			fresh.trampoline = reinterpret_cast<uintptr_t>(trampoline);
			SetThunkTarget(fresh.headThunk, fresh.trampoline);

			g_chains.push_back(std::move(fresh));
			chain = &g_chains.back();

			LogToFile::Debug("[HookBroker] New chain at 0x%llX (trampoline 0x%llX)",
				static_cast<unsigned long long>(target),
				static_cast<unsigned long long>(chain->trampoline));
		}

		uint8_t* linkThunk = AllocateThunk();
		if (!linkThunk)
			return LinkId{};

		// Splice at the tail: the new link runs last, so the first thing
		// installed on an address is the first thing called. That is what keeps
		// PreloadInfo::priority meaning what it says.
		SetThunkTarget(linkThunk, chain->trampoline);
		SetThunkTarget(PredecessorSlot(*chain, chain->links.size()),
			reinterpret_cast<uintptr_t>(detour));

		Link link;
		link.id     = g_nextLinkId++;
		link.owner  = owner ? owner : "modloader";
		link.name   = name  ? name  : "(unnamed)";
		link.detour = reinterpret_cast<uintptr_t>(detour);
		link.thunk  = linkThunk;

		const LinkId id{ link.id };
		chain->links.push_back(std::move(link));

		if (chain->links.size() > 1)
		{
			LogToFile::Info("[HookBroker] 0x%llX now has %zu hooks -- '%s' (%s) joined the chain behind %s",
				static_cast<unsigned long long>(target), chain->links.size(),
				name ? name : "(unnamed)", owner ? owner : "modloader",
				chain->links[chain->links.size() - 2].owner.c_str());
		}

		*outOriginal = linkThunk;
		return id;
	}

	bool Detach(LinkId id)
	{
		if (!id.Valid())
			return false;

		std::lock_guard<std::mutex> lock(g_mutex);

		for (size_t c = 0; c < g_chains.size(); ++c)
		{
			Chain& chain = g_chains[c];

			for (size_t k = 0; k < chain.links.size(); ++k)
			{
				if (chain.links[k].id != id.value)
					continue;

				// Route the predecessor past this link, to wherever this link was
				// going. One aligned store; anyone already inside the removed
				// detour finishes normally through its own thunk, which still
				// points somewhere valid because thunks are never freed.
				SetThunkTarget(PredecessorSlot(chain, k), GetThunkTarget(chain.links[k].thunk));

				LogToFile::Debug("[HookBroker] Detached '%s' (%s) from 0x%llX -- %zu left",
					chain.links[k].name.c_str(), chain.links[k].owner.c_str(),
					static_cast<unsigned long long>(chain.target),
					chain.links.size() - 1);

				chain.links.erase(chain.links.begin() + static_cast<ptrdiff_t>(k));

				// Last one out restores the prologue. This is the whole reason
				// the broker exists: one plugin unhooking must not unhook
				// everybody else, and the loader is the only thing in a position
				// to know which case this is.
				if (chain.links.empty())
				{
					LogToFile::Info("[HookBroker] Last hook removed from 0x%llX -- restoring the original prologue",
						static_cast<unsigned long long>(chain.target));
					chain.prologue.RemoveRaw();
					g_chains.erase(g_chains.begin() + static_cast<ptrdiff_t>(c));
				}

				return true;
			}
		}

		return false;
	}

	bool IsHooked(uintptr_t target)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return FindChain(target) != nullptr;
	}

	int GetLinkCount(uintptr_t target)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		const Chain* chain = FindChain(target);
		return chain ? static_cast<int>(chain->links.size()) : 0;
	}

	std::vector<PatchedRange> GetPatchedRanges()
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		std::vector<PatchedRange> out;
		out.reserve(g_chains.size());

		for (const Chain& chain : g_chains)
		{
			if (!chain.prologue.installed || chain.prologue.patchSize == 0)
				continue;

			PatchedRange range;
			range.address = chain.target;
			range.originalBytes.assign(chain.prologue.originalBytes,
				chain.prologue.originalBytes + chain.prologue.patchSize);
			out.push_back(std::move(range));
		}

		return out;
	}

	std::vector<ChainInfo> Snapshot()
	{
		std::lock_guard<std::mutex> lock(g_mutex);

		std::vector<ChainInfo> out;
		out.reserve(g_chains.size());

		for (const Chain& chain : g_chains)
		{
			ChainInfo info;
			info.target     = chain.target;
			info.trampoline = chain.trampoline;

			for (const Link& link : chain.links)
				info.links.push_back(LinkInfo{ link.owner, link.name, link.detour });

			out.push_back(std::move(info));
		}

		return out;
	}
}
