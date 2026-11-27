// The real broker on real prologue patches. Each target is a small function the
// test writes into its own page; the cut query is a table the test sets. After
// every change the thunk slots are read back and checked against the rule in
// hook_broker.h.

#include "hooks/hook_broker.h"
#include "hooks/hooks_common.h"
#include "logging/logger.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

// What the head slot held when InstallRaw was about to write the patch. The log
// line just before the write is the one place to look from outside.
static const uint8_t* g_patchHead = nullptr;
static uintptr_t      g_slotBeforePatch = 0;
static int            g_patchesSeen = 0;

namespace ModLoaderLogger
{
	void LogDebug(const wchar_t* format, ...)
	{
		va_list args;
		va_start(args, format);
		if (wcsstr(format, L"InstallRaw: target=") && wcsstr(format, L"jumpTo="))
		{
			va_arg(args, unsigned long long);
			g_patchHead = reinterpret_cast<const uint8_t*>(va_arg(args, unsigned long long));
		}
		else if (wcsstr(format, L"writing JMP patch") && g_patchHead)
		{
			g_slotBeforePatch = *reinterpret_cast<const volatile uintptr_t*>(g_patchHead + 8);
			++g_patchesSeen;
		}
		va_end(args);
	}
	void LogInfo(const wchar_t*, ...) {}
	void LogWarn(const wchar_t*, ...) {}
	void LogError(const wchar_t*, ...) {}
	void LogMessage(const wchar_t*, ...) {}
}

namespace Broker = Hooks::Broker;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(x) Check((x), #x, __LINE__)
static void Check(bool ok, const char* what, int line)
{
	++g_checks;
	if (!ok)
	{
		++g_failures;
		printf("  FAIL line %d: %s\n", line, what);
	}
}

// --- Fake modules and the cut query ------------------------------------------

constexpr int kModules = 4;
static char              g_moduleBytes[kModules];
static std::atomic<bool> g_denied[kModules];
static std::atomic<bool> g_askedAboutNull{ false };

static HMODULE Module(int m) { return m < 0 ? nullptr : reinterpret_cast<HMODULE>(&g_moduleBytes[m]); }

// Answers true for a null module on purpose: the broker must never ask.
static bool CutQueryDouble(const void* address)
{
	if (!address)
	{
		g_askedAboutNull = true;
		return true;
	}
	for (int m = 0; m < kModules; ++m)
		if (address == &g_moduleBytes[m])
			return g_denied[m].load();
	return false;
}

static void AllowAll()
{
	for (std::atomic<bool>& d : g_denied)
		d = false;
}

// --- Detours -----------------------------------------------------------------

using Fn = uint64_t (*)(uint64_t);

constexpr int kDetours = 6;
static void*                 g_original[kDetours];
static std::atomic<uint64_t> g_calls[kDetours];
static bool                  g_tracing = false;
static std::string           g_trace;       // the detours a call went through, in order

template <int N>
static uint64_t Detour(uint64_t x)
{
	g_calls[N].fetch_add(1, std::memory_order_relaxed);
	if (g_tracing)
		g_trace += static_cast<char>('0' + N);
	return reinterpret_cast<Fn>(g_original[N])(x);
}

static void* const kDetour[kDetours] = {
	reinterpret_cast<void*>(&Detour<0>), reinterpret_cast<void*>(&Detour<1>),
	reinterpret_cast<void*>(&Detour<2>), reinterpret_cast<void*>(&Detour<3>),
	reinterpret_cast<void*>(&Detour<4>), reinterpret_cast<void*>(&Detour<5>),
};

// --- Targets -----------------------------------------------------------------

constexpr size_t kCodeBytes = 32;

struct Target
{
	uint8_t* code = nullptr;
	uint8_t  shipped[kCodeBytes]{};

	// uint64_t f(uint64_t x) { return x + 1000; } with room for the patch.
	Target()
	{
		code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		memset(code, 0xCC, 4096);
		const uint8_t lea[] = { 0x48, 0x8D, 0x81, 0xE8, 0x03, 0x00, 0x00 };   // lea rax, [rcx+1000]
		memcpy(code, lea, sizeof(lea));
		memset(code + sizeof(lea), 0x90, 16);
		code[sizeof(lea) + 16] = 0xC3;
		FlushInstructionCache(GetCurrentProcess(), code, 4096);
		memcpy(shipped, code, kCodeBytes);
	}

	uintptr_t Address() const { return reinterpret_cast<uintptr_t>(code); }

	uint64_t Call(uint64_t x) const
	{
		const volatile Fn fn = reinterpret_cast<Fn>(code);
		return fn(x);
	}

	// The detours one call goes through.
	std::string Trace(uint64_t x = 7) const
	{
		g_trace.clear();
		g_tracing = true;
		const uint64_t result = Call(x);
		g_tracing = false;
		CHECK(result == x + 1000);
		return g_trace;
	}
};

static uintptr_t Slot(const void* thunk)
{
	return *reinterpret_cast<const volatile uintptr_t*>(static_cast<const uint8_t*>(thunk) + 8);
}

// What the prologue JMP points at: FF 25 00000000 <address>.
static const void* HeadThunk(const Target& t)
{
	const void* head = nullptr;
	memcpy(&head, t.code + 6, sizeof(head));
	return head;
}

// One chain the test built: which detour each link is, in install order.
struct Built
{
	Target                      target;
	std::vector<int>            detours;
	std::vector<Broker::LinkId> ids;
	uint8_t                     patched[kCodeBytes]{};

	void Add(int detour, int module)
	{
		const Broker::LinkId id = Broker::Attach(target.Address(), kDetour[detour], &g_original[detour],
		                                         "test", "link", Module(module));
		CHECK(id.Valid());
		detours.push_back(detour);
		ids.push_back(id);
		if (ids.size() == 1)
			memcpy(patched, target.code, kCodeBytes);
	}

	void Remove(int detour)
	{
		for (size_t i = 0; i < detours.size(); ++i)
		{
			if (detours[i] != detour) continue;
			CHECK(Broker::Detach(ids[i]));
			detours.erase(detours.begin() + i);
			ids.erase(ids.begin() + i);
			return;
		}
		CHECK(!"no such link");
	}

	Broker::ChainInfo Info() const
	{
		for (const Broker::ChainInfo& c : Broker::Snapshot())
			if (c.target == target.Address())
				return c;
		return {};
	}

	// "1" for each cut link, "0" for each live one, in chain order.
	std::string Cuts() const
	{
		std::string s;
		for (const Broker::LinkInfo& l : Info().links)
			s += l.cut ? '1' : '0';
		return s;
	}

	std::vector<uintptr_t> Slots() const
	{
		std::vector<uintptr_t> s{ Slot(HeadThunk(target)) };
		for (int d : detours)
			s.push_back(Slot(g_original[d]));
		return s;
	}

	// The whole rule, plus what must never change while the chain lives.
	void Verify(int line) const
	{
		const Broker::ChainInfo info = Info();
		Check(info.links.size() == detours.size(), "link count", line);
		if (info.links.size() != detours.size())
			return;

		const std::vector<uintptr_t> slots = Slots();
		for (size_t position = 0; position < slots.size(); ++position)   // 0 is the head thunk
		{
			uintptr_t expected = info.trampoline;
			for (size_t later = position; later < info.links.size(); ++later)
				if (!info.links[later].cut) { expected = info.links[later].detour; break; }
			Check(slots[position] == expected, "slot goes to the next live link", line);
		}

		std::string live;
		for (size_t i = 0; i < detours.size(); ++i)
		{
			Check(info.links[i].detour == reinterpret_cast<uintptr_t>(kDetour[detours[i]]), "link order", line);
			if (!info.links[i].cut)
				live += static_cast<char>('0' + detours[i]);
		}
		Check(target.Trace() == live, "a call runs the live detours in order", line);
		Check(memcmp(target.code, patched, kCodeBytes) == 0, "game code untouched since the first patch", line);
	}

	// Remove every link; the shipped bytes must come back.
	void Close()
	{
		while (!detours.empty())
		{
			Remove(detours.front());
			if (!detours.empty())
				Verify(__LINE__);
		}
		CHECK(!Broker::IsHooked(target.Address()));
		CHECK(memcmp(target.code, target.shipped, kCodeBytes) == 0);
		CHECK(target.Trace().empty());
	}
};

#define VERIFY(chain) (chain).Verify(__LINE__)

static void Deny(int module, bool denied) { g_denied[module] = denied; }

// --- Tests -------------------------------------------------------------------

static void Test_NothingIsCutWithoutAQuery()
{
	AllowAll();
	Deny(0, true);
	Built c;
	c.Add(0, 0);
	CHECK(Broker::SyncCuts() == 0);
	CHECK(c.Cuts() == "0");
	VERIFY(c);
	c.Close();
}

// A thread can call the target as soon as the patch is in, so the head slot must
// lead somewhere before then.
static void Test_TheHeadSlotIsSetBeforeThePatch()
{
	AllowAll();
	g_patchHead = nullptr;
	g_slotBeforePatch = 0;
	const int seen = g_patchesSeen;

	Built c;
	c.Add(0, 0);
	CHECK(g_patchesSeen == seen + 1);
	CHECK(g_patchHead == HeadThunk(c.target));
	uintptr_t trampoline = 0;
	for (const Broker::ChainInfo& info : Broker::Snapshot())
		if (info.target == c.target.Address())
			trampoline = info.trampoline;
	CHECK(trampoline != 0);
	CHECK(g_slotBeforePatch == trampoline);
	c.Close();
}

static void Test_CutAndRestoreFirstMiddleLastAll()
{
	AllowAll();
	Built c;
	c.Add(0, 0);
	c.Add(1, 1);
	c.Add(2, 2);
	VERIFY(c);
	const std::vector<uintptr_t> before = c.Slots();
	const size_t patchedRanges = Broker::GetPatchedRanges().size();

	const char* const kExpected[] = { "100", "010", "001" };
	for (int m = 0; m < 3; ++m)
	{
		Deny(m, true);
		CHECK(Broker::SyncCuts() == 1);
		CHECK(Broker::SyncCuts() == 0);          // a repeated sync changes nothing
		CHECK(c.Cuts() == kExpected[m]);
		VERIFY(c);
		Deny(m, false);
		CHECK(Broker::SyncCuts() == 1);
		CHECK(c.Cuts() == "000");
		CHECK(c.Slots() == before);
		VERIFY(c);
	}

	Deny(0, true); Deny(1, true); Deny(2, true);
	CHECK(Broker::SyncCuts() == 3);
	CHECK(c.Cuts() == "111");
	VERIFY(c);
	for (uintptr_t slot : c.Slots())
		CHECK(slot == c.Info().trampoline);
	CHECK(Broker::IsHooked(c.target.Address()));
	CHECK(Broker::GetLinkCount(c.target.Address()) == 3);
	CHECK(Broker::GetPatchedRanges().size() == patchedRanges);

	AllowAll();
	CHECK(Broker::SyncCuts() == 3);
	CHECK(c.Slots() == before);
	CHECK(c.target.Trace() == "012");
	VERIFY(c);
	c.Close();
}

static void Test_CallsAreCountedOnlyOnLiveDetours()
{
	AllowAll();
	Built c;
	c.Add(0, 0);
	c.Add(1, 1);
	c.Add(2, 2);
	Deny(1, true);
	Broker::SyncCuts();
	for (auto& n : g_calls) n = 0;
	for (uint64_t i = 0; i < 100; ++i)
		CHECK(c.target.Call(i) == i + 1000);
	CHECK(g_calls[0] == 100);
	CHECK(g_calls[1] == 0);
	CHECK(g_calls[2] == 100);
	AllowAll();
	Broker::SyncCuts();
	c.Close();
}

// A call already inside a cut detour carries on through its own `original`.
static void Test_ACutLinksOwnOriginalSkipsToTheNextLiveLink()
{
	AllowAll();
	Built c;
	c.Add(0, 0);
	c.Add(1, 1);
	c.Add(2, 2);
	c.Add(3, 3);
	Deny(1, true); Deny(2, true);
	Broker::SyncCuts();
	VERIFY(c);
	g_trace.clear();
	g_tracing = true;
	CHECK(reinterpret_cast<Fn>(g_original[1])(5) == 1005);   // from inside cut link 1
	g_tracing = false;
	CHECK(g_trace == "3");
	AllowAll();
	Broker::SyncCuts();
	c.Close();
}

static void Test_CutBeforeAttach()
{
	AllowAll();
	Deny(0, true);
	Built c;
	c.Add(0, 0);                 // first link of a new chain, already cut
	CHECK(c.Cuts() == "1");
	CHECK(Slot(HeadThunk(c.target)) == c.Info().trampoline);
	VERIFY(c);
	CHECK(Broker::SyncCuts() == 0);
	AllowAll();
	CHECK(Broker::SyncCuts() == 1);
	CHECK(c.target.Trace() == "0");
	VERIFY(c);
	c.Close();
}

static void Test_AttachWhileCut()
{
	AllowAll();
	Built c;
	c.Add(0, 0);
	Deny(1, true);
	c.Add(1, 1);                 // cut link behind a live one
	CHECK(c.Cuts() == "01");
	VERIFY(c);
	c.Add(2, 2);                 // live link behind a cut one
	CHECK(c.Cuts() == "010");
	VERIFY(c);
	c.Add(3, 1);                 // cut again, at the tail
	CHECK(c.Cuts() == "0101");
	VERIFY(c);
	Deny(0, true); Deny(2, true);
	CHECK(Broker::SyncCuts() == 2);
	c.Add(4, 3);                 // live link behind nothing but cut ones
	CHECK(c.Cuts() == "11110");
	CHECK(c.target.Trace() == "4");
	VERIFY(c);
	AllowAll();
	CHECK(Broker::SyncCuts() == 4);
	CHECK(c.target.Trace() == "01234");
	VERIFY(c);
	c.Close();
}

// A sync reads the answer as it is when it runs.
static void Test_ALateSyncLeavesNoStaleCut()
{
	AllowAll();
	Built c;
	c.Add(0, 0);
	Deny(0, true);               // denied, then allowed again before any sync ran
	Deny(0, false);
	CHECK(Broker::SyncCuts() == 0);
	CHECK(c.Cuts() == "0");
	Deny(0, true);
	c.Add(1, 0);                 // attached while denied, before the sync for it
	CHECK(c.Cuts() == "01");
	VERIFY(c);
	CHECK(Broker::SyncCuts() == 1);
	CHECK(c.Cuts() == "11");
	VERIFY(c);
	AllowAll();
	Broker::SyncCuts();
	c.Close();
}

// Every choice of cut links on a chain of four, then every one of them removed.
static void Test_DetachWithEveryCutPattern()
{
	for (int pattern = 0; pattern < 16; ++pattern)
	{
		for (int victim = 0; victim < 4; ++victim)
		{
			AllowAll();
			Built c;
			for (int i = 0; i < 4; ++i)
				c.Add(i, i);
			for (int i = 0; i < 4; ++i)
				Deny(i, (pattern & (1 << i)) != 0);
			Broker::SyncCuts();
			VERIFY(c);
			c.Remove(victim);                // cut or live, with cut or live neighbours
			VERIFY(c);
			Broker::SyncCuts();              // nothing is stale afterwards
			VERIFY(c);
			AllowAll();
			Broker::SyncCuts();
			VERIFY(c);
			c.Close();
		}

		// And the whole chain taken down while still cut.
		AllowAll();
		Built c;
		for (int i = 0; i < 4; ++i)
			c.Add(i, i);
		for (int i = 0; i < 4; ++i)
			Deny(i, (pattern & (1 << i)) != 0);
		Broker::SyncCuts();
		c.Close();
	}
	AllowAll();
}

static void Test_TwoModulesCutOnOneChainAndTheLoaderNever()
{
	AllowAll();
	g_askedAboutNull = false;
	Built c;
	c.Add(0, 0);
	c.Add(1, -1);                // the loader's own, or a preload hook
	c.Add(2, 1);
	c.Add(3, 0);
	c.Add(4, 2);                 // a third plugin that stays allowed
	Built other;                 // a sync reaches every chain
	other.Add(5, 1);
	const std::vector<uintptr_t> before = c.Slots();

	Deny(0, true); Deny(1, true);
	CHECK(Broker::SyncCuts() == 4);
	CHECK(c.Cuts() == "10110");
	CHECK(other.Cuts() == "1");
	CHECK(c.target.Trace() == "14");
	CHECK(other.target.Trace().empty());
	VERIFY(c);
	VERIFY(other);

	Deny(0, false);
	CHECK(Broker::SyncCuts() == 2);
	CHECK(c.Cuts() == "00100");
	VERIFY(c);

	AllowAll();
	CHECK(Broker::SyncCuts() == 2);
	CHECK(c.Slots() == before);
	CHECK(c.target.Trace() == "01234");
	VERIFY(c);
	VERIFY(other);
	CHECK(!g_askedAboutNull);
	c.Close();
	other.Close();
}

// The way a plugin's hook is installed: the module handed to Hook::Install is
// the one its link is cut with.
static void Test_AnInstalledHookCarriesItsModule()
{
	AllowAll();
	Target target;
	Hooks::Hook hook;
	CHECK(hook.Install(target.Address(), kDetour[0], &g_original[0], "test", "link", Module(0)));
	CHECK(target.Trace() == "0");
	Deny(0, true);
	CHECK(Broker::SyncCuts() == 1);
	CHECK(target.Trace().empty());
	AllowAll();
	CHECK(Broker::SyncCuts() == 1);
	CHECK(target.Trace() == "0");
	hook.Remove();
	CHECK(memcmp(target.code, target.shipped, kCodeBytes) == 0);
}

// Retires fake module `m`. Its image is the one byte that stands for it, which
// no detour is inside.
static size_t Retire(int m) { return Broker::RetireModule(Module(m), 1); }

// A module freed while cut: neither an allow nor anything later brings it back.
static void Test_ARetiredLinkStaysCut()
{
	AllowAll();
	Built c;
	c.Add(0, 0);
	c.Add(1, 1);
	c.Add(2, 0);
	Deny(0, true);
	CHECK(Broker::SyncCuts() == 2);
	CHECK(Retire(0) == 2);
	CHECK(c.Cuts() == "101");
	VERIFY(c);
	AllowAll();                  // what the cut query says once it is untracked
	CHECK(Broker::SyncCuts() == 0);
	CHECK(c.Cuts() == "101");
	VERIFY(c);
	CHECK(Retire(0) == 0);

	c.Add(3, 2);                 // a later install
	CHECK(c.Cuts() == "1010");
	VERIFY(c);
	c.Remove(1);                 // a later detach
	CHECK(c.Cuts() == "110");
	VERIFY(c);
	Deny(2, true);               // later syncs that switch another module
	CHECK(Broker::SyncCuts() == 1);
	VERIFY(c);
	AllowAll();
	CHECK(Broker::SyncCuts() == 1);
	CHECK(c.Cuts() == "110");
	CHECK(c.target.Trace() == "3");
	VERIFY(c);
	c.Close();                   // retired links still detach
}

// Retiring cuts a live link at once, with no sync and with no query answer.
static void Test_RetiringCutsLiveLinksAtOnce()
{
	AllowAll();
	Built c;
	c.Add(0, 0);
	c.Add(1, 1);
	Built other;
	other.Add(2, 0);
	CHECK(Retire(0) == 2);       // every chain
	CHECK(c.Cuts() == "10");
	CHECK(other.Cuts() == "1");
	CHECK(c.target.Trace() == "1");
	CHECK(other.target.Trace().empty());
	VERIFY(c);
	VERIFY(other);
	CHECK(Broker::SyncCuts() == 0);
	CHECK(Retire(1) == 1);       // all retired: calls reach the function
	CHECK(c.target.Trace().empty());
	VERIFY(c);
	c.Close();
	other.Close();
}

static void Test_AModuleAtTheSameAddressStartsClean()
{
	AllowAll();
	Built c;
	c.Add(0, 0);
	CHECK(Retire(0) == 1);
	c.Add(1, 0);                 // the next module loaded there
	CHECK(c.Cuts() == "10");
	VERIFY(c);
	Deny(0, true);
	CHECK(Broker::SyncCuts() == 1);
	CHECK(c.Cuts() == "11");
	AllowAll();
	CHECK(Broker::SyncCuts() == 1);
	CHECK(c.Cuts() == "10");
	CHECK(c.target.Trace() == "1");
	VERIFY(c);
	c.Close();
}

// A hook another module (or no module) is named for, with its detour in the
// freed image.
static void Test_ADetourInsideAFreedImageIsRetired()
{
	AllowAll();
	Built c;
	c.Add(0, 1);
	c.Add(1, -1);
	c.Add(2, -1);
	// No module names nothing, even with every address in range.
	CHECK(Broker::RetireModule(nullptr, ~static_cast<size_t>(0)) == 0);
	// An image that ends at the third detour, and one that starts just past it.
	const auto third = reinterpret_cast<uintptr_t>(kDetour[2]);
	CHECK(Broker::RetireModule(reinterpret_cast<HMODULE>(third - 1), 1) == 0);
	CHECK(Broker::RetireModule(reinterpret_cast<HMODULE>(third + 1), 1) == 0);
	CHECK(c.Cuts() == "000");
	for (int d : { 0, 1 })
		CHECK(Broker::RetireModule(reinterpret_cast<HMODULE>(kDetour[d]), 1) == 1);
	CHECK(c.Cuts() == "110");
	CHECK(Broker::SyncCuts() == 0);
	CHECK(c.target.Trace() == "2");
	VERIFY(c);
	c.Close();
}

// Callers hammer the target while the cuts are switched 10,000 times. The
// loader's two links are never cut, so a call that misses one was misrouted.
static void Test_CallingWhileCutsToggle()
{
	AllowAll();
	Built c;
	c.Add(0, 0);
	c.Add(1, -1);
	c.Add(2, 1);
	c.Add(3, 0);
	c.Add(4, -1);
	c.Add(5, 2);
	for (auto& n : g_calls) n = 0;

	constexpr int kCallers = 3;
	constexpr int kToggles = 10000;
	std::atomic<bool>     stop{ false };
	std::atomic<int>      running{ 0 };
	std::atomic<uint64_t> total{ 0 };
	std::atomic<uint64_t> wrong{ 0 };
	std::vector<std::thread> callers;
	for (int t = 0; t < kCallers; ++t)
	{
		callers.emplace_back([&] {
			++running;
			for (uint64_t i = 0; !stop.load(std::memory_order_relaxed); ++i)
			{
				if (c.target.Call(i) != i + 1000)
					++wrong;
				total.fetch_add(1, std::memory_order_relaxed);
			}
		});
	}
	while (running != kCallers || total == 0)
		std::this_thread::yield();

	const uint64_t atStart = total;
	size_t changed = 0;
	for (int i = 1; i <= kToggles; ++i)
	{
		// Walks all eight deny sets, often changing several links at once.
		const int pattern = (i * 5) & 7;
		for (int m = 0; m < 3; ++m)
			Deny(m, (pattern & (1 << m)) != 0);
		changed += Broker::SyncCuts();
		if (i % 100 == 0)        // let the callers in between switches
		{
			const uint64_t seen = total;
			while (total == seen)
				std::this_thread::yield();
		}
	}
	const uint64_t during = total - atStart;
	stop = true;
	for (std::thread& t : callers)
		t.join();

	printf("  %d toggles, %zu link changes, %llu calls (%llu while toggling)\n", kToggles, changed,
	       static_cast<unsigned long long>(total.load()), static_cast<unsigned long long>(during));
	CHECK(changed > kToggles);
	CHECK(during > 0);
	CHECK(wrong == 0);
	CHECK(g_calls[1] == total);
	CHECK(g_calls[4] == total);
	for (int d : { 0, 2, 3, 5 })
		CHECK(g_calls[d] <= total);

	AllowAll();
	Broker::SyncCuts();
	CHECK(c.target.Trace() == "012345");
	VERIFY(c);
	c.Close();
}

int main()
{
	// A wrong slot crashes or loops: end the run with an exit code, no dialog.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	std::thread([] {
		Sleep(120000);
		printf("FAIL: timed out\n");
		fflush(stdout);
		TerminateProcess(GetCurrentProcess(), 3);
	}).detach();
	setvbuf(stdout, nullptr, _IONBF, 0);

	struct { const char* name; void (*run)(); } const kTests[] = {
#define T(f) { #f, f }
		T(Test_NothingIsCutWithoutAQuery),       // first: runs before the query is set
		T(Test_TheHeadSlotIsSetBeforeThePatch),
		T(Test_CutAndRestoreFirstMiddleLastAll),
		T(Test_CallsAreCountedOnlyOnLiveDetours),
		T(Test_ACutLinksOwnOriginalSkipsToTheNextLiveLink),
		T(Test_CutBeforeAttach),
		T(Test_AttachWhileCut),
		T(Test_ALateSyncLeavesNoStaleCut),
		T(Test_DetachWithEveryCutPattern),
		T(Test_TwoModulesCutOnOneChainAndTheLoaderNever),
		T(Test_AnInstalledHookCarriesItsModule),
		T(Test_ARetiredLinkStaysCut),
		T(Test_RetiringCutsLiveLinksAtOnce),
		T(Test_AModuleAtTheSameAddressStartsClean),
		T(Test_ADetourInsideAFreedImageIsRetired),
		T(Test_CallingWhileCutsToggle),
#undef T
	};

	for (size_t i = 0; i < std::size(kTests); ++i)
	{
		if (i == 1)
			Broker::SetCutQuery(&CutQueryDouble);
		printf("%s\n", kTests[i].name);
		kTests[i].run();
	}

	printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
