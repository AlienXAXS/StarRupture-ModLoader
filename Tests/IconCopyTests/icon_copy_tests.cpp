// Icon copy bookkeeping test: the limits on engine-texture copies, the choice
// of protection, and that every reference a copy takes is given back.
//
// A console program: no game, no GPU. UI/icon_copy.h is the overlay's own
// header; the COM Release and the device's Evict are a counting fake (FakeOps),
// so each test sees exactly which objects were released and evicted.
#include "UI/icon_copy.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                        \
    } while (0)

static void RunTest(const char* name, void (*fn)())
{
    const int before = g_failed;
    printf("[ RUN  ] %s\n", name);
    fn();
    printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", name);
}

// Counts Release and Evict calls per object.
struct FakeOps
{
    static std::map<void*, int> released;
    static std::map<void*, int> evicted;
    static void Reset() { released.clear(); evicted.clear(); }
    static void Release(void* object) { ++released[object]; }
    static void Evict(void* pageable) { ++evicted[pageable]; }
};
std::map<void*, int> FakeOps::released;
std::map<void*, int> FakeOps::evicted;

static int Released(void* object) { auto it = FakeOps::released.find(object); return it == FakeOps::released.end() ? 0 : it->second; }
static int Evicted(void* object)  { auto it = FakeOps::evicted.find(object);  return it == FakeOps::evicted.end()  ? 0 : it->second; }

using IconCopy::Hold;
using IconCopy::PendingHold;
using IconCopy::Ring;

// Distinct addresses to stand for COM objects.
static char g_objects[64];
static void* Object(int i) { return &g_objects[i]; }

static Hold FullHold(int base, bool heap)
{
    Hold hold;
    hold.source   = Object(base);
    hold.heap     = heap ? Object(base + 1) : nullptr;
    hold.dest     = Object(base + 2);
    hold.resident = heap ? hold.heap : hold.source;
    return hold;
}

// ---------------------------------------------------------------------------

static void BudgetAllowsEightPerTick()
{
    IconCopy::TickBudget budget;
    for (int i = 0; i < IconCopy::kMaxPerTick; ++i)
    {
        CHECK(budget.Available(5));
        budget.Take(5);
    }
    CHECK(!budget.Available(5));
    CHECK(IconCopy::kMaxPerTick == 8);
}

static void BudgetResetsOnTheNextTick()
{
    IconCopy::TickBudget budget;
    for (int i = 0; i < IconCopy::kMaxPerTick; ++i)
        budget.Take(5);
    CHECK(!budget.Available(5));
    CHECK(budget.Available(6));
    budget.Take(6);
    for (int i = 1; i < IconCopy::kMaxPerTick; ++i)
        budget.Take(6);
    CHECK(!budget.Available(6));
}

static void BudgetFreshAtTickZero()
{
    // The first tick the loader reports may be 0; a new budget must allow it.
    IconCopy::TickBudget budget;
    CHECK(budget.Available(0));
    budget.Take(0);
    CHECK(budget.Available(0));
}

static void BudgetRefusalTakesNothing()
{
    // Asking takes nothing: only a copy that starts calls Take.
    IconCopy::TickBudget budget;
    for (int i = 0; i < 100; ++i)
        CHECK(budget.Available(9));
}

static void RingCapsAtEight()
{
    FakeOps::Reset();
    Ring<FakeOps> ring;
    CHECK(IconCopy::kMaxInFlight == 8);
    for (int i = 0; i < IconCopy::kMaxInFlight; ++i)
    {
        const int slot = ring.FreeSlot();
        CHECK(slot >= 0);
        ring.Start(slot, static_cast<uint64_t>(i + 1), Hold{ Object(i), nullptr, nullptr, nullptr });
    }
    CHECK(ring.InFlight() == 8);
    CHECK(ring.FreeSlot() == -1);
    CHECK(ring.LastFence() == 8);
    CHECK(FakeOps::released.empty());
}

static void RingReclaimsOnlyPassedFences()
{
    FakeOps::Reset();
    Ring<FakeOps> ring;
    for (int i = 0; i < IconCopy::kMaxInFlight; ++i)
        ring.Start(ring.FreeSlot(), static_cast<uint64_t>(i + 1), Hold{ Object(i), nullptr, nullptr, nullptr });

    CHECK(ring.Reclaim(0) == 0);
    CHECK(ring.Reclaim(3) == 3);
    CHECK(ring.InFlight() == 5);
    CHECK(Released(Object(0)) == 1 && Released(Object(1)) == 1 && Released(Object(2)) == 1);
    CHECK(Released(Object(3)) == 0);
    CHECK(ring.FreeSlot() >= 0);

    // A reclaimed entry is not released twice.
    CHECK(ring.Reclaim(3) == 0);
    CHECK(Released(Object(0)) == 1);

    // A lost device completes every fence at UINT64_MAX: everything goes.
    CHECK(ring.Reclaim(UINT64_MAX) == 5);
    CHECK(ring.InFlight() == 0);
    for (int i = 0; i < IconCopy::kMaxInFlight; ++i)
        CHECK(Released(Object(i)) == 1);
}

static void RingFreesEntryForTheNinth()
{
    FakeOps::Reset();
    Ring<FakeOps> ring;
    for (int i = 0; i < IconCopy::kMaxInFlight; ++i)
        ring.Start(ring.FreeSlot(), static_cast<uint64_t>(i + 1), Hold{});
    CHECK(ring.FreeSlot() == -1);
    ring.Reclaim(1);
    const int slot = ring.FreeSlot();
    CHECK(slot >= 0);
    ring.Start(slot, 9, Hold{});
    CHECK(ring.FreeSlot() == -1);
    CHECK(ring.LastFence() == 9);
}

static void ReleaseOrderAndEvict()
{
    // A placed source: the heap is the resident pageable, evicted once; the
    // heap, source and copy each released once.
    FakeOps::Reset();
    Hold placed = FullHold(10, true);
    IconCopy::Release<FakeOps>(placed);
    CHECK(Evicted(Object(11)) == 1);
    CHECK(Evicted(Object(10)) == 0);
    CHECK(Released(Object(10)) == 1 && Released(Object(11)) == 1 && Released(Object(12)) == 1);
    CHECK(placed.source == nullptr && placed.heap == nullptr && placed.dest == nullptr && placed.resident == nullptr);

    // A committed source: the resource itself is evicted, and released once.
    FakeOps::Reset();
    Hold committed = FullHold(20, false);
    IconCopy::Release<FakeOps>(committed);
    CHECK(Evicted(Object(20)) == 1);
    CHECK(Released(Object(20)) == 1 && Released(Object(22)) == 1);
    CHECK(Released(Object(21)) == 0);

    // Reduced protection: no residency count, so no Evict.
    FakeOps::Reset();
    Hold reduced{ Object(30), nullptr, Object(32), nullptr };
    IconCopy::Release<FakeOps>(reduced);
    CHECK(FakeOps::evicted.empty());
    CHECK(Released(Object(30)) == 1 && Released(Object(32)) == 1);

    // Releasing an emptied hold again does nothing.
    IconCopy::Release<FakeOps>(reduced);
    CHECK(Released(Object(30)) == 1);
}

static void PendingHoldReleasesOnEarlyReturn()
{
    FakeOps::Reset();
    {
        PendingHold<FakeOps> hold;
        hold.Get() = FullHold(40, true);
        // goes out of scope without a copy, as on every early return
    }
    CHECK(Released(Object(40)) == 1 && Released(Object(41)) == 1 && Released(Object(42)) == 1);
    CHECK(Evicted(Object(41)) == 1);
}

static void PendingHoldReleasesPartial()
{
    // A return between taking the source and the heap releases only the source.
    FakeOps::Reset();
    {
        PendingHold<FakeOps> hold;
        hold.Get().source = Object(45);
    }
    CHECK(Released(Object(45)) == 1);
    CHECK(FakeOps::released.size() == 1);
    CHECK(FakeOps::evicted.empty());
}

static void PendingHoldReleasesOnThrow()
{
    // The slot table full throws out of the Load call with the hold alive.
    FakeOps::Reset();
    bool caught = false;
    try
    {
        PendingHold<FakeOps> hold;
        hold.Get() = FullHold(50, false);
        throw std::out_of_range("all texture slots are in use");
    }
    catch (const std::out_of_range&)
    {
        caught = true;
    }
    CHECK(caught);
    CHECK(Released(Object(50)) == 1 && Released(Object(52)) == 1);
    CHECK(Evicted(Object(50)) == 1);
}

static void PendingHoldTakeHandsOverOnce()
{
    FakeOps::Reset();
    Ring<FakeOps> ring;
    {
        PendingHold<FakeOps> hold;
        hold.Get() = FullHold(54, true);
        ring.Start(ring.FreeSlot(), 1, hold.Take());
    }
    // The pending hold gave it to the ring: nothing released yet.
    CHECK(FakeOps::released.empty());
    CHECK(FakeOps::evicted.empty());
    ring.Reclaim(1);
    CHECK(Released(Object(54)) == 1 && Released(Object(55)) == 1 && Released(Object(56)) == 1);
    CHECK(Evicted(Object(55)) == 1);
}

static void PendingHoldAbandonLeaks()
{
    // A copy the GPU may still run with no fence to say when: nothing released.
    FakeOps::Reset();
    {
        PendingHold<FakeOps> hold;
        hold.Get() = FullHold(57, true);
        hold.Abandon();
    }
    CHECK(FakeOps::released.empty());
    CHECK(FakeOps::evicted.empty());
}

static void RingReleaseAllAtShutdown()
{
    FakeOps::Reset();
    Ring<FakeOps> ring;
    ring.Start(ring.FreeSlot(), 1, FullHold(0, true));
    ring.Start(ring.FreeSlot(), 2, FullHold(3, false));
    ring.ReleaseAll();
    CHECK(ring.InFlight() == 0);
    CHECK(Released(Object(0)) == 1 && Released(Object(1)) == 1 && Released(Object(2)) == 1);
    CHECK(Released(Object(3)) == 1 && Released(Object(5)) == 1);
    CHECK(Evicted(Object(1)) == 1 && Evicted(Object(3)) == 1);
    ring.ReleaseAll();
    CHECK(Released(Object(0)) == 1);
}

static void RingAbandonAllReleasesNothing()
{
    FakeOps::Reset();
    Ring<FakeOps> ring;
    ring.Start(ring.FreeSlot(), 1, FullHold(0, true));
    ring.AbandonAll();
    CHECK(ring.InFlight() == 0);
    CHECK(FakeOps::released.empty());
    CHECK(FakeOps::evicted.empty());
    ring.Reclaim(UINT64_MAX);
    CHECK(FakeOps::released.empty());
}

static void ChooseFullWithRows()
{
    const IconCopy::Protection committed = IconCopy::Choose(true, IconCopy::HeapProbe::Committed);
    CHECK(committed.residency == IconCopy::Residency::Resource);
    CHECK(committed.reduced == nullptr);

    const IconCopy::Protection placed = IconCopy::Choose(true, IconCopy::HeapProbe::Heap);
    CHECK(placed.residency == IconCopy::Residency::Heap);
    CHECK(placed.reduced == nullptr);
}

static void ChooseReducedWithoutRows()
{
    // No rows for the running build: whatever the probe says (it is not run),
    // the copy goes ahead, the source itself is made resident (which holds a
    // committed one; D3D12 refuses a placed one), and it says why.
    for (IconCopy::HeapProbe probe : { IconCopy::HeapProbe::Committed, IconCopy::HeapProbe::Heap, IconCopy::HeapProbe::Invalid })
    {
        const IconCopy::Protection p = IconCopy::Choose(false, probe);
        CHECK(p.residency == IconCopy::Residency::Resource);
        CHECK(p.reduced != nullptr && std::strstr(p.reduced, "version map") != nullptr);
    }
}

static void ChooseReducedOnInvalidHeap()
{
    // The pointer is not followed: only the source itself is made resident.
    const IconCopy::Protection p = IconCopy::Choose(true, IconCopy::HeapProbe::Invalid);
    CHECK(p.residency == IconCopy::Residency::Resource);
    CHECK(p.reduced != nullptr && std::strstr(p.reduced, "heap") != nullptr);
}

static void RingOldestFence()
{
    FakeOps::Reset();
    Ring<FakeOps> ring;
    CHECK(ring.OldestFence() == 0);
    ring.Start(ring.FreeSlot(), 7, Hold{});
    ring.Start(ring.FreeSlot(), 5, Hold{});
    ring.Start(ring.FreeSlot(), 9, Hold{});
    CHECK(ring.OldestFence() == 5);
    ring.Reclaim(5);
    CHECK(ring.OldestFence() == 7);
    ring.Reclaim(UINT64_MAX);
    CHECK(ring.OldestFence() == 0);
}

static void StartsAtOnceWithinBothLimits()
{
    FakeOps::Reset();
    Ring<FakeOps> ring;
    IconCopy::TickBudget budget;
    for (int i = 0; i < IconCopy::kMaxPerTick; ++i)
    {
        CHECK(IconCopy::WaitBeforeStart(ring, budget, true, 3) == 0);
        budget.Take(3);
        ring.Start(ring.FreeSlot(), static_cast<uint64_t>(i + 1), Hold{});
    }
}

static void OverTheTickBudgetWaitsForTheOldest()
{
    // A plugin that loads a whole list in one tick: the ninth copy is not
    // refused, it waits for the oldest one in flight.
    FakeOps::Reset();
    Ring<FakeOps> ring;
    IconCopy::TickBudget budget;
    for (int i = 0; i < IconCopy::kMaxPerTick; ++i)
    {
        budget.Take(3);
        ring.Start(ring.FreeSlot(), static_cast<uint64_t>(i + 1), Hold{});
    }
    CHECK(IconCopy::WaitBeforeStart(ring, budget, true, 3) == 1);

    // Once that copy is done the entry is free, but the tick's budget is still
    // spent, so the next one waits for the new oldest.
    ring.Reclaim(1);
    CHECK(IconCopy::WaitBeforeStart(ring, budget, true, 3) == 2);

    // With nothing in flight there is nothing to wait for: it starts.
    ring.Reclaim(UINT64_MAX);
    CHECK(IconCopy::WaitBeforeStart(ring, budget, true, 3) == 0);

    // The next tick starts at once again.
    ring.Start(ring.FreeSlot(), 20, Hold{});
    CHECK(IconCopy::WaitBeforeStart(ring, budget, true, 4) == 0);
}

static void FullRingWaitsEvenWithoutATick()
{
    // No tick count (the tick hook is not in): no per-tick budget, but a full
    // ring still makes the next copy wait for its oldest.
    FakeOps::Reset();
    Ring<FakeOps> ring;
    IconCopy::TickBudget budget;
    for (int i = 0; i < IconCopy::kMaxInFlight; ++i)
    {
        CHECK(IconCopy::WaitBeforeStart(ring, budget, false, 0) == 0);
        ring.Start(ring.FreeSlot(), static_cast<uint64_t>(i + 10), Hold{});
    }
    CHECK(IconCopy::WaitBeforeStart(ring, budget, false, 0) == 10);
    ring.Reclaim(10);
    CHECK(IconCopy::WaitBeforeStart(ring, budget, false, 0) == 0);
}

// A parked hold keeps its residency count until its request has finished: an
// Evict while it may still be paging in could race it.
static void ParkedReleasesOnlyOnceTheRequestFinished()
{
    FakeOps::Reset();
    IconCopy::Parked<FakeOps> parked;
    CHECK(parked.Empty());
    {
        PendingHold<FakeOps> hold;
        hold.Get() = FullHold(0, true);
        CHECK(parked.Park(7, hold, 1000));
        CHECK(hold.Get().source == nullptr);  // the hold is the parked list's now
    }
    CHECK(Evicted(Object(1)) == 0);
    CHECK(Released(Object(0)) == 0);
    CHECK(parked.Count() == 1);

    CHECK(!parked.Reclaim(6, 1100, 10000));
    CHECK(Evicted(Object(1)) == 0);
    CHECK(!parked.AllDone(6));

    CHECK(!parked.Reclaim(7, 1200, 10000));
    CHECK(Evicted(Object(1)) == 1);
    CHECK(Released(Object(0)) == 1);
    CHECK(Released(Object(1)) == 1);
    CHECK(Released(Object(2)) == 1);
    CHECK(parked.Empty());
    CHECK(parked.AllDone(7));

    // A lost device completes every fence.
    PendingHold<FakeOps> hold;
    hold.Get() = FullHold(10, false);
    CHECK(parked.Park(8, hold, 1300));
    parked.Reclaim(UINT64_MAX, 1300, 10000);
    CHECK(Evicted(Object(10)) == 1);
    CHECK(parked.Empty());
}

static void ParkedRefusesWhenFull()
{
    FakeOps::Reset();
    IconCopy::Parked<FakeOps, 2> parked;
    for (int i = 0; i < 2; ++i)
    {
        PendingHold<FakeOps> hold;
        hold.Get() = FullHold(i * 4, false);
        CHECK(parked.Park(static_cast<uint64_t>(i + 1), hold, 0));
    }
    {
        PendingHold<FakeOps> hold;
        hold.Get() = FullHold(20, false);
        CHECK(!parked.Park(3, hold, 0));
        CHECK(hold.Get().source == Object(20));  // left with the caller
        hold.Abandon();
    }
    CHECK(parked.Count() == 2);
    CHECK(Evicted(Object(20)) == 0);
}

// Expired: some still parked and none has finished for the limit, counted from
// the oldest one's park or the last one that finished.
static void ParkedExpiresWithoutProgress()
{
    FakeOps::Reset();
    IconCopy::Parked<FakeOps> parked;
    CHECK(!parked.Reclaim(0, 50000, 10000));  // empty never expires

    PendingHold<FakeOps> a;
    a.Get() = FullHold(0, false);
    CHECK(parked.Park(1, a, 100000));
    PendingHold<FakeOps> b;
    b.Get() = FullHold(4, false);
    CHECK(parked.Park(2, b, 105000));  // the clock stays at the oldest

    CHECK(!parked.Reclaim(0, 109999, 10000));
    CHECK(parked.Reclaim(0, 110000, 10000));

    // The first finishes: the clock restarts for the second.
    CHECK(!parked.Reclaim(1, 111000, 10000));
    CHECK(parked.Count() == 1);
    CHECK(!parked.Reclaim(1, 120999, 10000));
    CHECK(parked.Reclaim(1, 121000, 10000));

    parked.AbandonAll();
    CHECK(parked.Empty());
    CHECK(Evicted(Object(4)) == 0);
    CHECK(Released(Object(4)) == 0);
}

// A park into an empty list starts the clock afresh, however long ago the
// list was last looked at.
static void ParkedClockStartsAtThePark()
{
    FakeOps::Reset();
    IconCopy::Parked<FakeOps> parked;
    CHECK(!parked.Reclaim(0, 1000, 10000));
    PendingHold<FakeOps> hold;
    hold.Get() = FullHold(0, false);
    CHECK(parked.Park(1, hold, 500000));
    CHECK(!parked.Reclaim(0, 500001, 10000));
    parked.ReleaseAll();
    CHECK(Evicted(Object(0)) == 1);
    CHECK(parked.Empty());
}

int main()
{
    RunTest("TickBudget: eight copies per tick", BudgetAllowsEightPerTick);
    RunTest("TickBudget: a new tick has a new budget", BudgetResetsOnTheNextTick);
    RunTest("TickBudget: tick 0 is a tick like any other", BudgetFreshAtTickZero);
    RunTest("TickBudget: refused calls take nothing", BudgetRefusalTakesNothing);
    RunTest("Ring: at most eight copies in flight", RingCapsAtEight);
    RunTest("Ring: reclaims only copies whose fence passed, once", RingReclaimsOnlyPassedFences);
    RunTest("Ring: a reclaimed entry takes the next copy", RingFreesEntryForTheNinth);
    RunTest("Release: evicts the resident pageable, releases each reference once", ReleaseOrderAndEvict);
    RunTest("PendingHold: released on an early return", PendingHoldReleasesOnEarlyReturn);
    RunTest("PendingHold: a partial hold releases what it has", PendingHoldReleasesPartial);
    RunTest("PendingHold: released when the call throws", PendingHoldReleasesOnThrow);
    RunTest("PendingHold: Take hands the hold to the ring", PendingHoldTakeHandsOverOnce);
    RunTest("PendingHold: Abandon keeps everything", PendingHoldAbandonLeaks);
    RunTest("Ring: ReleaseAll at shutdown", RingReleaseAllAtShutdown);
    RunTest("Ring: AbandonAll releases nothing", RingAbandonAllReleasesNothing);
    RunTest("Choose: full protection with the rows", ChooseFullWithRows);
    RunTest("Choose: reduced without the rows, the copy still runs", ChooseReducedWithoutRows);
    RunTest("Choose: reduced when the heap does not check out", ChooseReducedOnInvalidHeap);
    RunTest("Ring: the oldest fence in flight", RingOldestFence);
    RunTest("WaitBeforeStart: within both limits a copy starts at once", StartsAtOnceWithinBothLimits);
    RunTest("WaitBeforeStart: past the tick budget, wait for the oldest copy", OverTheTickBudgetWaitsForTheOldest);
    RunTest("WaitBeforeStart: a full ring waits, with or without a tick", FullRingWaitsEvenWithoutATick);
    RunTest("Parked: released only once the residency request finished", ParkedReleasesOnlyOnceTheRequestFinished);
    RunTest("Parked: refuses when full, the hold stays with the caller", ParkedRefusesWhenFull);
    RunTest("Parked: expires when none finishes for the limit", ParkedExpiresWithoutProgress);
    RunTest("Parked: the clock starts at the park", ParkedClockStartsAtThePark);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
