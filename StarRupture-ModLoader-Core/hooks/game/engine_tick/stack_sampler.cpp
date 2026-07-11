#include "pch.h"
#include "stack_sampler.h"
#include "function_symbols.h"
#include "../../symbol_resolver.h"
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Hooks::EngineTick::StackSampler
{
	// How many frames a single raw sample can capture. Bounds both the
	// per-sample stack buffer (fixed-size, no heap use while suspended) and
	// how deep the merged call tree can reach back towards its true root.
	// UE5 tick call chains nest deeply (tick groups -> actor tick ->
	// component tick -> ...), so this is generous; the buffer itself is
	// just stack space on the sampler thread, free to size up.
	static constexpr int kMaxFrames = 48;
	// How often the sampler thread takes a sample while a window is active.
	// Best-effort -- actual cadence depends on the process's timer
	// resolution, which this module does not itself raise; if the game
	// hasn't called timeBeginPeriod(1) already, samples land closer to
	// ~15ms apart than 1ms. Either way, slower ticks naturally accumulate
	// more samples, which is the property that actually matters here.
	static constexpr DWORD kSampleSleepMs = 1;

	static std::once_flag g_startOnce;
	static HANDLE g_gameThreadHandle = nullptr;
	static std::atomic<bool> g_windowActive{ false };

	static std::mutex g_samplesMutex;
	static std::vector<std::vector<uintptr_t>> g_rawSamples; // guarded by g_samplesMutex

	// Hand-rolled x64 stack walk using the same unwind metadata the OS uses
	// for SEH -- no dbghelp, no heap allocation, no locks. Safe to run while
	// the thread being walked is suspended.
	static int WalkStackRaw(CONTEXT ctx, uintptr_t* outFrames, int maxFrames)
	{
		int count = 0;
		while (count < maxFrames && ctx.Rip != 0)
		{
			outFrames[count++] = static_cast<uintptr_t>(ctx.Rip);

			DWORD64 imageBase = 0;
			PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
			if (!rf)
				break; // no unwind info here (leaf stub) -- can't safely go further

			void* handlerData = nullptr;
			DWORD64 establisherFrame = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, rf,
			                  &ctx, &handlerData, &establisherFrame, nullptr);
		}
		return count;
	}

	static void SamplerThreadProc()
	{
		for (;;)
		{
			if (!g_windowActive.load(std::memory_order_acquire) || !g_gameThreadHandle)
			{
				Sleep(kSampleSleepMs);
				continue;
			}

			HANDLE h = g_gameThreadHandle;

			if (SuspendThread(h) == static_cast<DWORD>(-1))
			{
				Sleep(kSampleSleepMs);
				continue;
			}

			CONTEXT ctx{};
			ctx.ContextFlags = CONTEXT_FULL;
			uintptr_t frames[kMaxFrames];
			int frameCount = 0;

			if (GetThreadContext(h, &ctx))
				frameCount = WalkStackRaw(ctx, frames, kMaxFrames);

			// Resume immediately -- only heap-free, lock-free work happens
			// above this line while the game thread was stopped.
			ResumeThread(h);

			if (frameCount > 0 && g_windowActive.load(std::memory_order_acquire))
			{
				std::lock_guard<std::mutex> lock(g_samplesMutex);
				g_rawSamples.emplace_back(frames, frames + frameCount);
			}

			Sleep(kSampleSleepMs);
		}
	}

	void EnsureStarted()
	{
		std::call_once(g_startOnce, []()
		{
			HANDLE dup = nullptr;
			DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
			                 GetCurrentProcess(), &dup,
			                 THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
			                 FALSE, 0);
			g_gameThreadHandle = dup;

			std::thread(&SamplerThreadProc).detach();
		});
	}

	void BeginWindow()
	{
		EnsureStarted();
		{
			std::lock_guard<std::mutex> lock(g_samplesMutex);
			g_rawSamples.clear();
		}
		g_windowActive.store(true, std::memory_order_release);
	}

	void EndWindow()
	{
		g_windowActive.store(false, std::memory_order_release);
	}

	size_t GetLastWindowSampleCount()
	{
		std::lock_guard<std::mutex> lock(g_samplesMutex);
		return g_rawSamples.size();
	}

	static void ResolveModuleOffset(uintptr_t addr, std::string& outModule, uintptr_t& outOffset)
	{
		HMODULE hMod = nullptr;
		if (addr && GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(addr), &hMod) && hMod)
		{
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(hMod, path, MAX_PATH);
			const wchar_t* slash = wcsrchr(path, L'\\');
			const wchar_t* name = slash ? slash + 1 : path;

			char buf[MAX_PATH]{};
			WideCharToMultiByte(CP_ACP, 0, name, -1, buf, MAX_PATH, nullptr, nullptr);
			outModule = buf;
			outOffset = addr - reinterpret_cast<uintptr_t>(hMod);
		}
		else
		{
			outModule = "<unknown>";
			outOffset = addr;
		}
	}

	// Resolves any address to the start of its containing function, using
	// the same OS unwind metadata WalkStackRaw uses (.pdata) -- present for
	// essentially all compiled x64 functions regardless of symbols. Used as
	// the grouping key when no PDB symbol is available (see ResolveIdentity):
	// still turns "17 samples scattered across 17 different exact PCs inside
	// one hot loop" into "17 samples in the same function" even unsymbolized.
	// Safe to call any time (reads read-only mapped PE data, no suspension
	// required here).
	static uintptr_t ResolveFunctionStart(uintptr_t addr)
	{
		DWORD64 imageBase = 0;
		PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(static_cast<DWORD64>(addr), &imageBase, nullptr);
		if (!rf)
			return addr; // no unwind info -- treat the raw address as its own identity

		return static_cast<uintptr_t>(imageBase) + rf->BeginAddress;
	}

	struct ResolvedIdentity
	{
		uintptr_t functionStart = 0;
		std::string module;
		uintptr_t offset = 0;
		std::string symbolName; // bare name (no "+0xN"), empty if nothing resolved it
	};

	// Resolution priority: a real PDB symbol (DbgHelp -- covers anything
	// with a matching PDB on its search path, including engine internals if
	// the user has supplied StarRupture-Win64-Shipping.pdb) beats the
	// UFUNCTION-reflection lookup (only ever covers native UFUNCTIONs) beats
	// plain Module+Offset. Whichever source resolves it also supplies the
	// grouping key: a PDB symbol's own start address is more precise than
	// the unwind-based function start (matters for hot/cold-split or
	// folded-identical-code functions where they can differ).
	static ResolvedIdentity ResolveIdentity(uintptr_t addr)
	{
		ResolvedIdentity id;

		uintptr_t displacement = 0;
		std::string pdbName = Hooks::SymbolResolver::Resolve(addr, &displacement);
		if (!pdbName.empty())
		{
			id.functionStart = addr - displacement;
			id.symbolName = std::move(pdbName);
		}
		else
		{
			id.functionStart = ResolveFunctionStart(addr);
			id.symbolName = FunctionSymbols::Resolve(id.functionStart);
		}

		ResolveModuleOffset(id.functionStart, id.module, id.offset);
		return id;
	}

	// A window's samples routinely repeat the same handful of addresses
	// hundreds of times (a hot loop's leaf, a handful of stable call
	// sites), and building a full call tree resolves every frame of every
	// sample -- so memoize within one GetLastWindowCallTree() call to avoid
	// redundant DbgHelp round-trips (each one taking the process-wide
	// DbgHelp lock).
	static const ResolvedIdentity& ResolveIdentityCached(uintptr_t addr,
		std::unordered_map<uintptr_t, ResolvedIdentity>& cache)
	{
		auto it = cache.find(addr);
		if (it != cache.end())
			return it->second;

		return cache.emplace(addr, ResolveIdentity(addr)).first->second;
	}

	// One node of the tree while it's being built/merged: keyed by the
	// child's functionStart at each level, distinct from the final
	// TickProfiler::CallTreeNode (a plain vector-of-children tree) so
	// merging doesn't need linear child search.
	struct BuildNode
	{
		ResolvedIdentity identity;
		int sampleCount = 0;
		std::unordered_map<uintptr_t, BuildNode> children;
	};

	static TickProfiler::CallTreeNode ToCallTreeNode(const BuildNode& build, size_t maxChildrenPerNode)
	{
		TickProfiler::CallTreeNode node;
		node.functionStart = build.identity.functionStart;
		node.module = build.identity.module;
		node.offset = build.identity.offset;
		node.symbolName = build.identity.symbolName;
		node.sampleCount = build.sampleCount;

		std::vector<const BuildNode*> childPtrs;
		childPtrs.reserve(build.children.size());
		for (const auto& kv : build.children)
			childPtrs.push_back(&kv.second);

		std::sort(childPtrs.begin(), childPtrs.end(),
			[](const BuildNode* a, const BuildNode* b) { return a->sampleCount > b->sampleCount; });

		const size_t keep = childPtrs.size() < maxChildrenPerNode ? childPtrs.size() : maxChildrenPerNode;
		node.children.reserve(keep);
		for (size_t i = 0; i < keep; ++i)
			node.children.push_back(ToCallTreeNode(*childPtrs[i], maxChildrenPerNode));

		return node;
	}

	std::vector<TickProfiler::CallTreeNode> GetLastWindowCallTree(size_t maxChildrenPerNode)
	{
		std::vector<std::vector<uintptr_t>> samplesCopy;
		{
			std::lock_guard<std::mutex> lock(g_samplesMutex);
			samplesCopy = g_rawSamples;
		}

		std::unordered_map<uintptr_t, ResolvedIdentity> resolveCache;
		std::unordered_map<uintptr_t, BuildNode> roots;

		// Raw samples are captured leaf-first (index 0 = innermost). Walk
		// each one in reverse -- root/outermost captured frame first -- so
		// every sample merges into the same top-down tree: each frame
		// becomes (or matches) a child of the previous frame's node, and
		// every node on the path gets its sampleCount bumped once. Two
		// samples sharing the same first N callers merge into one branch
		// that only splits where their paths actually diverge.
		for (const std::vector<uintptr_t>& stack : samplesCopy)
		{
			if (stack.empty())
				continue;

			std::unordered_map<uintptr_t, BuildNode>* level = &roots;
			for (auto it = stack.rbegin(); it != stack.rend(); ++it)
			{
				const ResolvedIdentity& id = ResolveIdentityCached(*it, resolveCache);
				BuildNode& node = (*level)[id.functionStart];
				if (node.sampleCount == 0)
					node.identity = id;
				++node.sampleCount;
				level = &node.children;
			}
		}

		std::vector<const BuildNode*> rootPtrs;
		rootPtrs.reserve(roots.size());
		for (const auto& kv : roots)
			rootPtrs.push_back(&kv.second);

		std::sort(rootPtrs.begin(), rootPtrs.end(),
			[](const BuildNode* a, const BuildNode* b) { return a->sampleCount > b->sampleCount; });

		std::vector<TickProfiler::CallTreeNode> result;
		result.reserve(rootPtrs.size());
		for (const BuildNode* root : rootPtrs)
			result.push_back(ToCallTreeNode(*root, maxChildrenPerNode));

		return result;
	}
}
