// Image library table test: ids and key normalisation, the four states, the
// order requests are served in, the byte and count caps with least-recently-seen
// eviction, set interest, and State and Get read from other threads.
//
// A console program: no game, no GPU. UI/image_library.cpp is the loader's own
// source; a texture here is a number standing in for the handle the pump makes.
#include "UI/image_library.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

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

using namespace ImageLibrary;

// The texture handle the pump would have made for an image.
static PluginTextureHandle Texture(PluginImageId id)
{
    return reinterpret_cast<PluginTextureHandle>(static_cast<uintptr_t>(id) * 16);
}

// Takes the next request and publishes a texture of the given size for it.
static PluginImageId ReadNext(Library& lib, uint64_t bytes, std::vector<Released>& evicted)
{
    Request request;
    if (!lib.TakeNext(request))
        return 0;
    lib.Publish(request.id, Texture(request.id), bytes, evicted);
    return request.id;
}

// A plugin draws the image behind an id: Get is what asks for it.
static PluginImageId Wanted(Library& lib, PluginImageId id)
{
    lib.Get(id);
    return id;
}

static const IPluginSelf* Self(int i)
{
    static char selves[4];
    return reinterpret_cast<const IPluginSelf*>(&selves[i]);
}

// ---------------------------------------------------------------------------

static void NormalisesItemNames()
{
    CHECK(NormalizeItemName("Iron_Plate") == "iron_plate");
    CHECK(NormalizeItemName("  Iron_Plate\t") == "iron_plate");
    CHECK(NormalizeItemName("") == "");
    CHECK(NormalizeItemName("   ") == "");
    CHECK(NormalizeItemName(nullptr) == "");
    CHECK(NormalizeItemName(std::string(300, 'a').c_str()) == "");
    CHECK(NormalizeItemName("a\x01" "b") == "");
}

static void NormalisesObjectPaths()
{
    CHECK(NormalizeObjectPath("/Game/UI/T_X.T_X") == "/game/ui/t_x.t_x");
    CHECK(NormalizeObjectPath("/Game/UI/T_X") == "/game/ui/t_x.t_x");
    CHECK(NormalizeObjectPath(" /Game/UI/T_X.T_X ") == "/game/ui/t_x.t_x");
    CHECK(NormalizeObjectPath("Texture2D'/Game/UI/T_X.T_X'") == "/game/ui/t_x.t_x");
    CHECK(NormalizeObjectPath("Texture2D'/Game/UI/T_X'") == "/game/ui/t_x.t_x");
    CHECK(NormalizeObjectPath("") == "");
    CHECK(NormalizeObjectPath(nullptr) == "");
    CHECK(NormalizeObjectPath("Game/UI/T_X.T_X") == "");
    CHECK(NormalizeObjectPath("/Game/UI/") == "");
    CHECK(NormalizeObjectPath("/") == "");
    CHECK(NormalizeObjectPath("Texture2D'/Game/UI/T_X.T_X") == "");
    CHECK(NormalizeObjectPath(std::string(2000, '/').c_str()) == "");
}

static void IdsAreStableAndNeverZero()
{
    Library lib;
    const PluginImageId a = lib.Item("Iron_Plate");
    const PluginImageId b = lib.Item("  iron_plate ");
    const PluginImageId c = lib.Item("Copper_Plate");
    CHECK(a != 0);
    CHECK(a == b);
    CHECK(c != 0 && c != a);

    const PluginImageId p = lib.Path("/Game/UI/T_X.T_X");
    CHECK(p != 0 && p != a && p != c);
    CHECK(lib.Path("Texture2D'/game/ui/t_x'") == p);

    // An item and a path with the same text are different keys.
    CHECK(lib.Item("/Game/UI/T_X.T_X") != p);

    CHECK(lib.Item("") == 0);
    CHECK(lib.Path("not a path") == 0);
    CHECK(lib.State(0) == PLUGIN_IMAGE_NONE);
    CHECK(lib.Get(0) == nullptr);
    CHECK(lib.State(9999) == PLUGIN_IMAGE_NONE);
    CHECK(lib.Get(9999) == nullptr);
}

static void TheRequestKeepsTheNameAsAskedFor()
{
    Library lib;
    const PluginImageId id = Wanted(lib, lib.Path("Texture2D'/Game/UI/T_Icon.T_Icon'"));
    const PluginImageId item = Wanted(lib, lib.Item("  Iron_Plate "));
    (void)lib.Item("iron_plate");

    Request first;
    Request second;
    CHECK(lib.TakeNext(first) && lib.TakeNext(second));
    const Request& pathRequest = first.id == id ? first : second;
    const Request& itemRequest = first.id == id ? second : first;
    CHECK(pathRequest.kind == Kind::Path && pathRequest.name == "/Game/UI/T_Icon.T_Icon");
    CHECK(itemRequest.id == item && itemRequest.kind == Kind::Item && itemRequest.name == "Iron_Plate");
}

static void IdsRunOutAtTheTableSize()
{
    Library lib;
    uint32_t last = 0;
    for (uint32_t i = 0; i < kMaxIds; ++i)
        last = lib.Item(("item_" + std::to_string(i)).c_str());
    CHECK(last == kMaxIds);
    CHECK(lib.Item("one_too_many") == 0);
    CHECK(lib.Item("item_5") == 6);
    CHECK(lib.ImageCount() == kMaxIds);
}

static void StatesFollowTheRead()
{
    Library lib;
    std::vector<Released> evicted;

    const PluginImageId a = Wanted(lib, lib.Item("a"));
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);
    CHECK(lib.Get(a) == nullptr);

    Request request;
    CHECK(lib.TakeNext(request) && request.id == a);
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);   // being read
    CHECK(!lib.TakeNext(request));                 // not handed out twice

    CHECK(lib.Publish(a, Texture(a), 100, evicted));
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY);
    CHECK(lib.Get(a) == Texture(a));
    CHECK(lib.BytesKept() == 100 && lib.CountKept() == 1);
    CHECK(evicted.empty());

    // Only a taken request can be answered, and only once.
    CHECK(!lib.Publish(a, Texture(a), 100, evicted));
    CHECK(lib.BytesKept() == 100);

    const PluginImageId none = Wanted(lib, lib.Item("none"));
    CHECK(lib.TakeNext(request) && request.id == none);
    lib.PublishNoImage(none);
    CHECK(lib.State(none) == PLUGIN_IMAGE_NO_IMAGE);
    CHECK(lib.Get(none) == nullptr);

    const PluginImageId bad = Wanted(lib, lib.Item("bad"));
    CHECK(lib.TakeNext(request) && request.id == bad);
    lib.Fail(bad);
    CHECK(lib.State(bad) == PLUGIN_IMAGE_NONE);
    CHECK(lib.Get(bad) == nullptr);
    CHECK(lib.State(bad) == PLUGIN_IMAGE_NONE);   // Get does not ask for a failed image again
    CHECK(!lib.TakeNext(request));

    // A read that cannot be done now goes back in the queue, still waiting.
    const PluginImageId later = Wanted(lib, lib.Item("later"));
    CHECK(lib.TakeNext(request) && request.id == later);
    lib.Requeue(later);
    CHECK(lib.State(later) == PLUGIN_IMAGE_WAITING);
    CHECK(lib.TakeNext(request) && request.id == later);

    // A null texture is a failed read.
    CHECK(!lib.Publish(later, nullptr, 10, evicted));
    CHECK(lib.State(later) == PLUGIN_IMAGE_NONE);
    CHECK(lib.BytesKept() == 100);
}

static void StateDoesNotMarkSeen()
{
    Library lib;
    lib.SetNow(0);
    const PluginImageId a = Wanted(lib, lib.Item("a"));
    lib.SetNow(kSeenLatelyMs + 1);
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);
    Request request;
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(a) == PLUGIN_IMAGE_NONE);
}

static void SeenImagesAreReadFirst()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = Wanted(lib, lib.Item("a"));
    const PluginImageId b = Wanted(lib, lib.Item("b"));
    const PluginImageId c = Wanted(lib, lib.Item("c"));
    const PluginImageId d = Wanted(lib, lib.Item("d"));

    lib.SetNow(100100);
    lib.Get(a);
    lib.SetNow(100200);
    lib.Get(c);
    lib.SetNow(100300);

    // c was seen last, then a. b and d were only asked for, at the start; the
    // lower id goes first among equals.
    Request request;
    CHECK(lib.TakeNext(request) && request.id == c);
    CHECK(lib.TakeNext(request) && request.id == a);
    CHECK(lib.TakeNext(request) && request.id == b);
    CHECK(lib.TakeNext(request) && request.id == d);
    CHECK(!lib.TakeNext(request));
}

static void RequestsNobodyDrewAreDropped()
{
    Library lib;
    const PluginImageId seen = Wanted(lib, lib.Item("seen"));
    const PluginImageId unseen = Wanted(lib, lib.Item("unseen"));

    lib.SetNow(kSeenLatelyMs);
    lib.Get(seen);
    lib.SetNow(kSeenLatelyMs + 1);   // unseen was asked for at 0: just past "lately"

    Request request;
    CHECK(lib.TakeNext(request) && request.id == seen);
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(unseen) == PLUGIN_IMAGE_NONE);
    CHECK(lib.State(seen) == PLUGIN_IMAGE_WAITING);

    // Drawing it again asks again.
    CHECK(lib.Get(unseen) == nullptr);
    CHECK(lib.State(unseen) == PLUGIN_IMAGE_WAITING);
    CHECK(lib.TakeNext(request) && request.id == unseen);
    CHECK(!lib.TakeNext(request));

    // Dropped, drawn, dropped, drawn: always back to one request.
    lib.Requeue(unseen);
    lib.SetNow(10 * kSeenLatelyMs);
    CHECK(!lib.TakeNext(request));
    lib.Get(unseen);
    lib.Get(unseen);
    CHECK(lib.TakeNext(request) && request.id == unseen);
    CHECK(!lib.TakeNext(request));
}

static void SetMembersAreKeptAndServedAfterSeenOnes()
{
    Library lib;
    const PluginImageId member = lib.Item("member");
    const PluginImageId other = lib.Item("other");
    const PluginImageId seen = lib.Item("seen");
    lib.AddToSets(member, PLUGIN_IMAGE_SET_ITEMS);

    lib.SetNow(10 * kSeenLatelyMs);
    lib.Get(seen);

    // No plugin uses the item set yet: member is dropped like other.
    Request request;
    CHECK(lib.TakeNext(request) && request.id == seen);
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(member) == PLUGIN_IMAGE_NONE);

    // Asked for again, then nobody draws it, but a plugin uses its set.
    lib.Get(member);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    lib.Requeue(seen);
    lib.SetNow(20 * kSeenLatelyMs);
    lib.Get(seen);
    CHECK(lib.TakeNext(request) && request.id == seen);   // seen lately, first
    CHECK(lib.TakeNext(request) && request.id == member); // kept: its set is used
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(other) == PLUGIN_IMAGE_NONE);
}

static void SetInterestIsRecordedAndDropped()
{
    Library lib;
    const PluginImageId member = lib.Item("member");
    lib.AddToSets(member, PLUGIN_IMAGE_SET_BUILDINGS);
    lib.SetNow(10 * kSeenLatelyMs);

    Request request;

    // Items are not what member belongs to.
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    CHECK(!lib.TakeNext(request));
    lib.Get(member);

    // Two plugins use it; the second is unloaded, the first still does.
    lib.UseSets(Self(1), PLUGIN_IMAGE_SET_BUILDINGS);
    lib.UseSets(Self(2), PLUGIN_IMAGE_SET_BUILDINGS);
    lib.SetNow(20 * kSeenLatelyMs);
    lib.ForgetPlugin(Self(1));
    CHECK(lib.TakeNext(request) && request.id == member);
    lib.Requeue(member);

    // 0 clears.
    lib.UseSets(Self(2), 0);
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(member) == PLUGIN_IMAGE_NONE);

    // Set again and unloaded without clearing: nothing is left.
    lib.Get(member);
    lib.UseSets(Self(2), PLUGIN_IMAGE_SET_BUILDINGS);
    lib.ForgetPlugin(Self(2));
    lib.SetNow(30 * kSeenLatelyMs);
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(member) == PLUGIN_IMAGE_NONE);

    // Bits that are not sets are ignored.
    lib.Get(member);
    lib.UseSets(Self(2), 0x80000000u);
    lib.SetNow(40 * kSeenLatelyMs);
    CHECK(!lib.TakeNext(request));
}

static void ByteCapEvictsLeastRecentlySeen()
{
    Library lib;
    lib.SetCaps(300, 100);
    std::vector<Released> evicted;

    lib.SetNow(10000);
    const PluginImageId a = Wanted(lib, lib.Item("a"));
    const PluginImageId b = Wanted(lib, lib.Item("b"));
    const PluginImageId c = Wanted(lib, lib.Item("c"));
    const PluginImageId d = Wanted(lib, lib.Item("d"));

    // Read in id order, then draw them in the order c, a, b so that c is the
    // oldest by the time the cap is hit. The stamps are further apart than the
    // window of "seen lately", which nothing is evicted inside.
    for (int i = 0; i < 3; ++i)
        ReadNext(lib, 100, evicted);
    CHECK(evicted.empty() && lib.BytesKept() == 300);
    lib.SetNow(20000); lib.Get(c);
    lib.SetNow(30000); lib.Get(a);
    lib.SetNow(40000); lib.Get(b);

    lib.SetNow(50000);
    lib.Get(d);
    CHECK(ReadNext(lib, 100, evicted) == d);
    CHECK(evicted.size() == 1);
    CHECK(evicted[0].id == c && evicted[0].handle == Texture(c) && evicted[0].bytes == 100);
    CHECK(lib.State(c) == PLUGIN_IMAGE_NONE);
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY && lib.State(b) == PLUGIN_IMAGE_READY && lib.State(d) == PLUGIN_IMAGE_READY);
    CHECK(lib.BytesKept() == 300 && lib.CountKept() == 3);

    // A big one pushes out as many as it takes, oldest first: a (30), b (40), d (50).
    lib.SetNow(60000);
    const PluginImageId big = Wanted(lib, lib.Item("big"));
    evicted.clear();
    CHECK(ReadNext(lib, 250, evicted) == big);
    CHECK(evicted.size() == 3 && evicted[0].id == a && evicted[1].id == b && evicted[2].id == d);
    CHECK(lib.State(big) == PLUGIN_IMAGE_READY);
    CHECK(lib.BytesKept() == 250 && lib.CountKept() == 1);
}

static void CountCapEvictsLeastRecentlySeen()
{
    Library lib;
    lib.SetCaps(1000000, 2);
    std::vector<Released> evicted;

    lib.SetNow(1000);
    const PluginImageId a = Wanted(lib, lib.Item("a"));
    const PluginImageId b = Wanted(lib, lib.Item("b"));
    const PluginImageId c = Wanted(lib, lib.Item("c"));
    ReadNext(lib, 10, evicted);
    ReadNext(lib, 10, evicted);
    lib.SetNow(2000); lib.Get(b);
    lib.SetNow(3000); lib.Get(a);
    lib.SetNow(4000); lib.Get(c);
    ReadNext(lib, 10, evicted);

    CHECK(evicted.size() == 1 && evicted[0].id == b);
    CHECK(lib.CountKept() == 2 && lib.BytesKept() == 20);
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY && lib.State(c) == PLUGIN_IMAGE_READY);
}

static void EqualStampsEvictTheLowerIdFirst()
{
    Library lib;
    lib.SetCaps(1000000, 2);
    std::vector<Released> evicted;
    const PluginImageId a = Wanted(lib, lib.Item("a"));
    Wanted(lib, lib.Item("b"));
    Wanted(lib, lib.Item("c"));
    for (int i = 0; i < 3; ++i)
        ReadNext(lib, 10, evicted);
    CHECK(evicted.size() == 1 && evicted[0].id == a);
}

static void AnImageSeenLatelyIsNeverEvicted()
{
    Library lib;
    lib.SetCaps(1000000, 2);
    std::vector<Released> evicted;
    lib.SetNow(1000);
    const PluginImageId a = Wanted(lib, lib.Item("a"));
    const PluginImageId b = Wanted(lib, lib.Item("b"));
    const PluginImageId c = Wanted(lib, lib.Item("c"));
    for (int i = 0; i < 3; ++i)
        ReadNext(lib, 10, evicted);

    // All three are drawn now: the cap is passed rather than a drawn image taken away.
    CHECK(evicted.empty());
    CHECK(lib.CountKept() == 3);
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY && lib.State(b) == PLUGIN_IMAGE_READY && lib.State(c) == PLUGIN_IMAGE_READY);

    // Once they have not been drawn for longer than the window the next copy brings the count back.
    lib.SetNow(1000 + kSeenLatelyMs + 1);
    lib.Get(a);
    const PluginImageId d = Wanted(lib, lib.Item("d"));
    CHECK(ReadNext(lib, 10, evicted) == d);
    CHECK(lib.CountKept() == 2);
    CHECK(evicted.size() == 2 && evicted[0].id == b && evicted[1].id == c);
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY && lib.State(d) == PLUGIN_IMAGE_READY);
}

static void RoomForCountsFreeSlotsAndWhatCanBeEvicted()
{
    Library lib;
    lib.SetCaps(1000000000, 3);
    std::vector<Released> evicted;
    lib.SetNow(1000);
    CHECK(lib.RoomFor(8) == 3);   // an empty table: the count cap
    CHECK(lib.RoomFor(2) == 2);   // never more than asked about

    Wanted(lib, lib.Item("a"));
    Wanted(lib, lib.Item("b"));
    const PluginImageId c = Wanted(lib, lib.Item("c"));
    for (int i = 0; i < 3; ++i)
        ReadNext(lib, 10, evicted);
    CHECK(lib.RoomFor(8) == 0);   // full, and all three drawn lately

    lib.SetNow(1000 + kSeenLatelyMs + 1);
    lib.Get(c);
    CHECK(lib.RoomFor(8) == 2);   // a and b can go; c is drawn
    CHECK(lib.RoomFor(1) == 1);
}

static void RoomForFollowsTheByteCap()
{
    // One full-size image is 349524 bytes: three fit under 1.1 MB.
    Library lib;
    lib.SetCaps(1100000, 2048);
    CHECK(lib.RoomFor(8) == 3);

    // A cap under one image: with nothing kept, one is let through so that it is refused at publish.
    Library tiny;
    tiny.SetCaps(1000, 2048);
    CHECK(tiny.RoomFor(8) == 1);
}

static void HasQueuedSeesAskedForImages()
{
    Library lib;
    CHECK(!lib.HasQueued());
    const PluginImageId a = Wanted(lib, lib.Item("a"));
    CHECK(lib.HasQueued());
    Request request;
    CHECK(lib.TakeNext(request) && request.id == a);
    CHECK(!lib.HasQueued());
}

static void ACopyOverTheByteCapIsRefused()
{
    Library lib;
    lib.SetCaps(100, 10);
    std::vector<Released> evicted;
    const PluginImageId small = Wanted(lib, lib.Item("small"));
    const PluginImageId huge = Wanted(lib, lib.Item("huge"));
    ReadNext(lib, 50, evicted);

    Request request;
    CHECK(lib.TakeNext(request) && request.id == huge);
    CHECK(!lib.Publish(huge, Texture(huge), 101, evicted));
    CHECK(evicted.empty());                       // nothing else is sacrificed for it
    CHECK(lib.State(huge) == PLUGIN_IMAGE_NONE);  // and it is not asked for again
    lib.Get(huge);
    CHECK(lib.State(huge) == PLUGIN_IMAGE_NONE);
    CHECK(lib.State(small) == PLUGIN_IMAGE_READY && lib.BytesKept() == 50);
}

static void AnEvictedImageIsAskedForAgainByGet()
{
    Library lib;
    lib.SetCaps(100, 1);
    std::vector<Released> evicted;

    lib.SetNow(1000);
    const PluginImageId a = Wanted(lib, lib.Item("a"));
    const PluginImageId b = Wanted(lib, lib.Item("b"));
    ReadNext(lib, 10, evicted);
    lib.SetNow(2000); lib.Get(b);
    ReadNext(lib, 10, evicted);
    CHECK(evicted.size() == 1 && evicted[0].id == a);
    CHECK(lib.State(a) == PLUGIN_IMAGE_NONE);

    Request request;
    CHECK(!lib.TakeNext(request));   // nobody asked yet

    lib.SetNow(3000);
    CHECK(lib.Get(a) == nullptr);
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);
    evicted.clear();
    CHECK(ReadNext(lib, 10, evicted) == a);
    CHECK(lib.Get(a) == Texture(a));
    CHECK(evicted.size() == 1 && evicted[0].id == b);

    // The id is the same one throughout.
    CHECK(lib.Item("a") == a);
}

static void ReleaseAllHandsBackEveryKeptCopy()
{
    Library lib;
    std::vector<Released> evicted;
    const PluginImageId a = Wanted(lib, lib.Item("a"));
    const PluginImageId b = Wanted(lib, lib.Item("b"));
    const PluginImageId c = Wanted(lib, lib.Item("c"));
    lib.SetNow(1);
    lib.Get(a); lib.Get(b); lib.Get(c);
    ReadNext(lib, 10, evicted);
    ReadNext(lib, 20, evicted);   // c stays queued
    CHECK(lib.CountKept() == 2);

    std::vector<Released> released;
    lib.ReleaseAll(released);
    CHECK(released.size() == 2);
    CHECK(lib.CountKept() == 0);
    CHECK(lib.BytesKept() == 0);
    CHECK(lib.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(lib.State(b) == PLUGIN_IMAGE_NONE);
    CHECK(lib.Get(a) == nullptr);

    // A second call finds nothing to give back.
    released.clear();
    lib.ReleaseAll(released);
    CHECK(released.empty());
}

static void ANewCapEvictsOnTheNextPublish()
{
    Library lib;
    std::vector<Released> evicted;
    Wanted(lib, lib.Item("a"));
    Wanted(lib, lib.Item("b"));
    ReadNext(lib, 100, evicted);
    ReadNext(lib, 100, evicted);
    CHECK(evicted.empty());
    lib.SetCaps(150, 10);
    Wanted(lib, lib.Item("c"));
    ReadNext(lib, 10, evicted);
    CHECK(evicted.size() == 1 && evicted[0].id == 1);   // equal stamps: the lower id
    CHECK(lib.BytesKept() == 110 && lib.CountKept() == 2);
}

static void ByteCapFollowsTheVideoBudget()
{
    CHECK(ByteCapForBudget(0) == kByteCapMax);
    CHECK(ByteCapForBudget(2ull << 30) == (2ull << 30) / 100 * 4);
    CHECK(ByteCapForBudget(100ull << 30) == kByteCapMax);
    CHECK(ByteCapForBudget(6ull << 30) == (6ull << 30) / 100 * 4);   // just under 256 MB
    CHECK(ByteCapForBudget(8ull << 30) == kByteCapMax);
}

static void AliasedIdsAreOneImage()
{
    Library lib;
    std::vector<Released> evicted;

    const PluginImageId item = Wanted(lib, lib.Item("iron_plate"));
    const PluginImageId path = Wanted(lib, lib.Path("/Game/UI/T_Iron.T_Iron"));
    CHECK(lib.Alias(item, path));
    CHECK(!lib.Alias(item, path));   // once

    // One request, not two.
    Request request;
    CHECK(lib.TakeNext(request) && request.id == path);
    CHECK(!lib.TakeNext(request));
    CHECK(lib.State(item) == PLUGIN_IMAGE_WAITING);
    CHECK(lib.Publish(path, Texture(path), 100, evicted));

    CHECK(lib.State(item) == PLUGIN_IMAGE_READY && lib.State(path) == PLUGIN_IMAGE_READY);
    CHECK(lib.Get(item) == Texture(path) && lib.Get(path) == Texture(path));
    CHECK(lib.BytesKept() == 100 && lib.CountKept() == 1);

    // Drawing through either id keeps the one image seen.
    lib.SetCaps(100, 10);
    lib.SetNow(5000);
    const PluginImageId other = Wanted(lib, lib.Item("other"));
    lib.SetNow(6000); lib.Get(item);
    lib.SetNow(7000); lib.Get(other);
    CHECK(ReadNext(lib, 100, evicted) == other);   // the image seen through item is the older
    CHECK(evicted.size() == 1 && evicted[0].id == path);
    CHECK(lib.State(item) == PLUGIN_IMAGE_NONE);

    // Asking again through the alias asks for the target.
    lib.Get(item);
    CHECK(lib.State(path) == PLUGIN_IMAGE_WAITING);
    CHECK(lib.TakeNext(request) && request.id == path);
    CHECK(!lib.TakeNext(request));
}

static void CanonicalNamesTheOneImage()
{
    Library lib;
    std::vector<Released> evicted;
    const PluginImageId item = lib.Item("iron_plate");
    const PluginImageId path = lib.Path("/Game/UI/T_Iron.T_Iron");
    const PluginImageId other = lib.Item("other");

    // Before the item is resolved to its texture, every id stands for itself.
    CHECK(lib.Canonical(item) == item);
    CHECK(lib.Canonical(path) == path);
    CHECK(lib.Canonical(0) == 0);
    CHECK(lib.Canonical(777) == 777);
    CHECK(lib.Canonical(0xFFFFFFFFu) == 0xFFFFFFFFu);

    // Resolved: the item names the path's image.
    lib.Get(item);
    Request request;
    CHECK(lib.TakeNext(request) && request.id == item);
    lib.Requeue(item);
    CHECK(lib.Alias(item, path));
    CHECK(lib.Canonical(item) == path);
    CHECK(lib.Canonical(path) == path);
    CHECK(lib.Canonical(other) == other);

    // A path id from the other side agrees: the same asset is the same id.
    CHECK(lib.Canonical(item) == lib.Canonical(lib.Path("texture2d'/game/ui/t_iron'")));

    // Reading and keeping the image change nothing.
    CHECK(lib.TakeNext(request) && request.id == path);
    CHECK(lib.Publish(path, Texture(path), 100, evicted));
    CHECK(lib.Canonical(item) == path && lib.Canonical(path) == path);

    // An alias of an alias lands on the end of the chain.
    const PluginImageId second = lib.Item("iron_plate_2");
    lib.Get(second);
    CHECK(lib.TakeNext(request) && request.id == second);
    lib.Requeue(second);
    CHECK(lib.Alias(second, item));
    CHECK(lib.Canonical(second) == path);

    // Through the table plugins hold.
    IPluginImages* table = GetInterface();
    const PluginImageId tItem = table->Item(Self(0), "Canonical_Item");
    const PluginImageId tPath = table->Path(Self(0), "/Game/UI/T_Canonical");
    CHECK(table->Canonical(tItem) == tItem && table->Canonical(tPath) == tPath);
    CHECK(table->Canonical(0) == 0 && table->Canonical(0x7FFFFFFFu) == 0x7FFFFFFFu);
    Library& shared = Instance();
    shared.Get(tItem);
    CHECK(shared.TakeNext(request) && request.id == tItem);
    shared.Requeue(tItem);
    CHECK(shared.Alias(tItem, tPath));
    CHECK(table->Canonical(tItem) == tPath);
}

static void RegisteringIsNotRequesting()
{
    Library lib;
    std::vector<PluginImageId> ids;
    for (int i = 0; i < 40; ++i)
    {
        lib.SetNow(100000 + i);
        ids.push_back(lib.Item(("item_" + std::to_string(i)).c_str()));
        ids.push_back(lib.Path(("/Game/UI/T_" + std::to_string(i)).c_str()));
    }
    CHECK(lib.ImageCount() == 80);

    // Pump for a while: nothing is queued, nothing is read.
    Request request;
    for (int tick = 0; tick < 200; ++tick)
    {
        lib.SetNow(100000 + 10 * tick);
        CHECK(!lib.TakeNext(request));
        CHECK(!lib.TakePreload(request));
        CHECK(!lib.SeenWaiting());
    }
    for (const PluginImageId id : ids)
        CHECK(lib.State(id) == PLUGIN_IMAGE_NONE);   // State does not ask either

    // Asking again for a registered name is still only an id.
    CHECK(lib.Item("item_3") == ids[6] && lib.Path("/Game/UI/T_3") == ids[7]);
    CHECK(!lib.TakeNext(request));

    // Only Get asks.
    lib.SetNow(200000);
    CHECK(lib.Get(ids[6]) == nullptr);
    CHECK(lib.TakeNext(request) && request.id == ids[6] && request.kind == Kind::Item);
    CHECK(!lib.TakeNext(request));
}

static void AliasRefusals()
{
    Library lib;
    std::vector<Released> evicted;
    const PluginImageId a = Wanted(lib, lib.Item("a"));
    const PluginImageId b = Wanted(lib, lib.Item("b"));
    const PluginImageId c = Wanted(lib, lib.Item("c"));
    CHECK(!lib.Alias(a, a));
    CHECK(!lib.Alias(a, 0));
    CHECK(!lib.Alias(0, a));
    CHECK(!lib.Alias(a, 777));

    // Already being read: too late.
    Request request;
    CHECK(lib.TakeNext(request) && request.id == a);
    CHECK(!lib.Alias(a, b));
    lib.Publish(a, Texture(a), 10, evicted);
    // Already kept.
    CHECK(!lib.Alias(a, b));

    // An alias of an alias lands on the end of the chain.
    CHECK(lib.Alias(b, c));
    const PluginImageId d = Wanted(lib, lib.Item("d"));
    CHECK(lib.Alias(d, b));
    CHECK(lib.TakeNext(request) && request.id == c);
    CHECK(lib.Publish(c, Texture(c), 10, evicted));
    CHECK(lib.Get(d) == Texture(c));
    CHECK(!lib.Alias(c, d));   // d is c
    CHECK(!lib.Alias(b, a));   // b is an alias already
}

static void InterfaceReachesTheSameTable()
{
    IPluginImages* table = GetInterface();
    CHECK(table != nullptr);
    CHECK(table == GetInterface());

    const PluginImageId id = table->Item(Self(0), "Interface_Item");
    CHECK(id != 0 && id == table->Item(Self(1), "interface_item"));
    CHECK(id == Instance().Item("interface_item"));
    CHECK(table->Path(Self(0), "/Game/UI/T_I") == Instance().Path("/game/ui/t_i.t_i"));
    CHECK(table->State(id) == PLUGIN_IMAGE_NONE);   // handing out an id asks for nothing
    CHECK(table->Get(id) == nullptr);
    CHECK(table->State(id) == PLUGIN_IMAGE_WAITING);
    CHECK(table->Get(0) == nullptr && table->State(0) == PLUGIN_IMAGE_NONE);
    CHECK(table->FromTexture(Self(0), nullptr) == 0);

    table->UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    table->UseSets(Self(0), 0);
    ImageLibrary::ForgetPlugin(Self(0));
}

// A reader that only ever calls Get and State, while the table creates ids,
// publishes and evicts under it. Every handle Get returns must be the one that
// belongs to that image, and the state must be one of the four.
static void ReadersSeeConsistentStatesWhileTheTableChanges()
{
    Library lib;
    lib.SetCaps(40 * 10, 40);
    constexpr uint32_t kImages = 200;
    std::vector<PluginImageId> ids;
    for (uint32_t i = 0; i < kImages; ++i)
        ids.push_back(lib.Item(("img_" + std::to_string(i)).c_str()));

    std::atomic<bool> stop{ false };
    std::atomic<int> wrong{ 0 };
    std::atomic<long> reads{ 0 };
    // The images the readers draw: a window that moves on, so the older ones can be evicted.
    constexpr uint32_t kWindow = 30;
    std::atomic<uint32_t> windowStart{ 0 };

    auto reader = [&]()
    {
        uint32_t n = 0;
        while (!stop.load())
        {
            const PluginImageId id = ids[(windowStart.load() + n++ % kWindow) % kImages];
            const PluginTextureHandle handle = lib.Get(id);
            const int state = lib.State(id);
            if (handle && handle != Texture(id))
                ++wrong;
            if (state < PLUGIN_IMAGE_NONE || state > PLUGIN_IMAGE_NO_IMAGE)
                ++wrong;
            // An image id that does not exist reads as nothing.
            if (lib.Get(kMaxIds + 1 + n % 7) != nullptr || lib.State(kMaxIds + 1) != PLUGIN_IMAGE_NONE)
                ++wrong;
            ++reads;
        }
    };

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
        readers.emplace_back(reader);

    while (reads.load() == 0)
        std::this_thread::yield();

    // The pump: serves what the readers ask for, evicting as it goes, while
    // more ids appear.
    std::vector<Released> evicted;
    uint64_t now = 0;
    for (int round = 0; round < 20000; ++round)
    {
        now += 100;
        lib.SetNow(now);
        windowStart.store(static_cast<uint32_t>(round / 500) * kWindow);
        if (round % 50 == 0)
            lib.Item(("extra_" + std::to_string(round)).c_str());
        Request request;
        if (lib.TakeNext(request))
        {
            if (request.id % 7 == 0)
                lib.PublishNoImage(request.id);
            else if (request.id % 11 == 0)
                lib.Fail(request.id);
            else
                lib.Publish(request.id, Texture(request.id), 10, evicted);
        }
    }

    stop.store(true);
    for (auto& t : readers)
        t.join();

    CHECK(wrong.load() == 0);
    CHECK(reads.load() > 0);
    // The last window was drawn for long enough that every older image could go.
    CHECK(lib.CountKept() <= 40 && lib.BytesKept() <= 400);
    CHECK(!evicted.empty());
}

// ---------------------------------------------------------------------------
// Sets and preload

static const char* kPathA = "/Game/UI/T_A.T_A";
static const char* kPathB = "/Game/UI/T_B.T_B";
static const char* kPathC = "/Game/UI/T_C.T_C";

static void AMemberOfASetIsIdleUntilPreloadReachesIt()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    CHECK(a != 0);
    // The same image under another spelling, in another set: one id, in both sets.
    CHECK(lib.PathInSet("Texture2D'/game/ui/t_a'", PLUGIN_IMAGE_SET_BUILDINGS) == a);
    CHECK(lib.Path(kPathA) == a);
    CHECK(lib.PathInSet("not a path", PLUGIN_IMAGE_SET_ITEMS) == 0);

    // It is not asked for.
    CHECK(lib.State(a) == PLUGIN_IMAGE_NONE);
    Request request;
    CHECK(!lib.TakeNext(request));
    // No plugin uses a set: nothing to preload.
    CHECK(!lib.TakePreload(request));

    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_BUILDINGS);
    CHECK(lib.TakePreload(request));
    CHECK(request.id == a && request.kind == Kind::Path && request.name == kPathA);
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);
    CHECK(!lib.TakePreload(request));   // taken: not handed out twice

    std::vector<Released> evicted;
    CHECK(lib.Publish(a, Texture(a), 100, evicted));
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY);
    CHECK(!lib.TakePreload(request));   // kept
}

static void UsedSetsAreTheUnionAndFollowThePlugins()
{
    Library lib;
    CHECK(lib.UsedSets() == 0);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    CHECK(lib.UsedSets() == PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(1), PLUGIN_IMAGE_SET_BUILDINGS);
    CHECK(lib.UsedSets() == (PLUGIN_IMAGE_SET_ITEMS | PLUGIN_IMAGE_SET_BUILDINGS));
    lib.UseSets(Self(1), PLUGIN_IMAGE_SET_ITEMS);
    CHECK(lib.UsedSets() == PLUGIN_IMAGE_SET_ITEMS);

    // A plugin that unloads drops its interest; so does one that clears it.
    lib.UseSets(Self(1), PLUGIN_IMAGE_SET_BUILDINGS);
    lib.ForgetPlugin(Self(0));
    CHECK(lib.UsedSets() == PLUGIN_IMAGE_SET_BUILDINGS);
    lib.UseSets(Self(1), 0);
    CHECK(lib.UsedSets() == 0);

    // Bits that are not sets are not kept.
    lib.UseSets(Self(2), 0xF0u | PLUGIN_IMAGE_SET_ITEMS);
    CHECK(lib.UsedSets() == PLUGIN_IMAGE_SET_ITEMS);
}

static void PreloadOnlyTakesMembersOfSetsInUse()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId items = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    const PluginImageId buildings = lib.PathInSet(kPathB, PLUGIN_IMAGE_SET_BUILDINGS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_BUILDINGS);

    Request request;
    CHECK(lib.TakePreload(request) && request.id == buildings);
    CHECK(!lib.TakePreload(request));   // the items are not in use

    // The plugin changes its mind: the items now, and the buildings are left.
    lib.Defer(buildings);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    CHECK(lib.TakePreload(request) && request.id == items);
    CHECK(!lib.TakePreload(request));
}

static void PreloadGoesRoundTheMembersAndDeferGivesOneBack()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    const PluginImageId b = lib.PathInSet(kPathB, PLUGIN_IMAGE_SET_ITEMS);
    const PluginImageId c = lib.PathInSet(kPathC, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);

    // In the order added.
    Request request;
    CHECK(lib.TakePreload(request) && request.id == a);
    // One that cannot be read now goes back to idle, and does not block the rest.
    lib.Defer(a);
    CHECK(lib.State(a) == PLUGIN_IMAGE_NONE);
    CHECK(lib.TakePreload(request) && request.id == b);
    CHECK(lib.TakePreload(request) && request.id == c);
    CHECK(lib.TakePreload(request) && request.id == a);   // round the list
    CHECK(!lib.TakePreload(request));

    // Deferring what is not taken does nothing.
    lib.Defer(a);
    lib.Defer(a);
    lib.Defer(0);
    CHECK(lib.TakePreload(request) && request.id == a);
}

static void ADeferredImageThatWasDrawnGoesToTheQueue()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);

    Request request;
    CHECK(lib.TakePreload(request));
    lib.SetNow(100010);
    CHECK(lib.Get(a) == nullptr);   // drawn while it is being read
    lib.Defer(a);
    CHECK(lib.State(a) == PLUGIN_IMAGE_WAITING);
    CHECK(!lib.TakePreload(request));   // it is the on-demand queue's now
    CHECK(lib.TakeNext(request) && request.id == a);
}

static void DrawingAMemberAsksForItAndJumpsTheQueue()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    const PluginImageId b = lib.PathInSet(kPathB, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);

    lib.SetNow(100500);
    CHECK(lib.Get(b) == nullptr);
    Request request;
    CHECK(lib.TakeNext(request) && request.id == b);
    CHECK(!lib.TakeNext(request));
    // The rest is still preload's.
    CHECK(lib.TakePreload(request) && request.id == a);
}

static void SeenWaitingCountsOnlyImagesDrawnLately()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId member = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    CHECK(!lib.SeenWaiting());   // idle

    Request request;
    CHECK(lib.TakePreload(request));
    CHECK(!lib.SeenWaiting());   // being preloaded, never drawn

    lib.SetNow(100100);
    lib.Get(member);
    CHECK(lib.SeenWaiting());
    lib.SetNow(100100 + kSeenLatelyMs + 1);
    CHECK(!lib.SeenWaiting());   // not drawn for a while

    // A request that was asked for and drawn counts until it is answered.
    const PluginImageId drawn = Wanted(lib, lib.Path(kPathB));
    CHECK(lib.SeenWaiting());
    CHECK(lib.TakeNext(request) && request.id == drawn);
    CHECK(lib.SeenWaiting());   // taken, not answered
    std::vector<Released> evicted;
    CHECK(lib.Publish(drawn, Texture(drawn), 100, evicted));
    CHECK(!lib.SeenWaiting());
}

static void PreloadStopsWhenAnotherImageWouldNotFit()
{
    // Room for two full images (349524 bytes each) and not three.
    Library lib;
    lib.SetCaps(1000000, 100);
    CHECK(lib.PreloadHasRoom());
    std::vector<Released> evicted;
    lib.SetNow(100000);
    const PluginImageId a = Wanted(lib, lib.Path(kPathA));
    const PluginImageId b = Wanted(lib, lib.Path(kPathB));
    Request request;
    CHECK(lib.TakeNext(request) && lib.Publish(request.id, Texture(request.id), 349524, evicted));
    CHECK(lib.PreloadHasRoom());
    CHECK(lib.TakeNext(request) && lib.Publish(request.id, Texture(request.id), 349524, evicted));
    CHECK(!lib.PreloadHasRoom());
    CHECK(a != b && evicted.empty());

    // The count cap too.
    Library few;
    few.SetCaps(1000000000, 1);
    few.SetNow(100000);
    Wanted(few, few.Path(kPathA));
    CHECK(few.TakeNext(request) && few.Publish(request.id, Texture(request.id), 100, evicted));
    CHECK(!few.PreloadHasRoom());
}

static void PreloadStaysStoppedOnceAnImageHasBeenEvicted()
{
    Request request;
    std::vector<Released> evicted;

    // Two kept under a count cap of two: room is gone while the table is full, and comes back with a higher cap.
    Library roomy;
    roomy.SetCaps(1000000000, 2);
    roomy.SetNow(100000);
    Wanted(roomy, roomy.Path(kPathA));
    roomy.SetNow(100001);
    Wanted(roomy, roomy.Path(kPathB));
    CHECK(roomy.TakeNext(request) && roomy.Publish(request.id, Texture(request.id), 100, evicted));
    CHECK(roomy.TakeNext(request) && roomy.Publish(request.id, Texture(request.id), 100, evicted));
    roomy.SetCaps(1000000000, 100);
    CHECK(roomy.PreloadHasRoom());

    // The same, but an image had to be evicted on the way: preload stays stopped, so it never churns.
    Library full;
    full.SetCaps(1000000000, 2);
    full.SetNow(100000);
    Wanted(full, full.Path(kPathA));
    full.SetNow(100001);
    Wanted(full, full.Path(kPathB));
    full.SetNow(100002);
    Wanted(full, full.Path(kPathC));
    // The most recently asked is read first, so the third read is the oldest and is the one pushed out,
    // once none of them is seen lately any more.
    PluginImageId taken[3] = {};
    for (PluginImageId& id : taken)
    {
        CHECK(full.TakeNext(request));
        id = request.id;
    }
    full.SetNow(101000);
    for (PluginImageId id : taken)
        full.Publish(id, Texture(id), 100, evicted);
    CHECK(evicted.size() == 1);
    CHECK(full.CountKept() == 2);
    full.SetCaps(1000000000, 100);
    CHECK(!full.PreloadHasRoom());
}

static void PreloadedImagesNobodyDrewAreEvictedFirst()
{
    Library lib;
    lib.SetCaps(1000000, 2);
    lib.SetNow(100000);
    const PluginImageId drawn = Wanted(lib, lib.Path(kPathA));
    const PluginImageId member = lib.PathInSet(kPathB, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    std::vector<Released> evicted;

    Request request;
    CHECK(lib.TakeNext(request) && request.id == drawn);
    CHECK(lib.Publish(drawn, Texture(drawn), 100, evicted));
    CHECK(lib.TakePreload(request) && request.id == member);
    CHECK(lib.Publish(member, Texture(member), 100, evicted));

    lib.SetNow(100100);
    const PluginImageId newer = Wanted(lib, lib.Path(kPathC));
    CHECK(lib.TakeNext(request) && request.id == newer);
    CHECK(lib.Publish(newer, Texture(newer), 100, evicted));
    CHECK(evicted.size() == 1 && evicted[0].id == member);   // never drawn: first out, though read later
}

static void PreloadProgressCountsTheMembersOfSetsInUse()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    const PluginImageId b = lib.PathInSet(kPathB, PLUGIN_IMAGE_SET_ITEMS);
    lib.PathInSet(kPathC, PLUGIN_IMAGE_SET_BUILDINGS);

    Library::PreloadProgress none = lib.GetPreloadProgress();
    CHECK(none.queued == 0 && none.done == 0);

    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    Library::PreloadProgress progress = lib.GetPreloadProgress();
    CHECK(progress.queued == 2 && progress.done == 0);

    std::vector<Released> evicted;
    Request request;
    CHECK(lib.TakePreload(request) && request.id == a);
    progress = lib.GetPreloadProgress();
    CHECK(progress.queued == 2 && progress.done == 0);   // being read
    CHECK(lib.Publish(a, Texture(a), 100, evicted));
    progress = lib.GetPreloadProgress();
    CHECK(progress.queued == 1 && progress.done == 1);

    CHECK(lib.TakePreload(request) && request.id == b);
    lib.PublishNoImage(b);
    progress = lib.GetPreloadProgress();
    CHECK(progress.queued == 0 && progress.done == 2);

    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS | PLUGIN_IMAGE_SET_BUILDINGS);
    progress = lib.GetPreloadProgress();
    CHECK(progress.queued == 1 && progress.done == 2);
}

static void AMemberThatFailsIsNotTakenAgain()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId a = lib.PathInSet(kPathA, PLUGIN_IMAGE_SET_ITEMS);
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    Request request;
    CHECK(lib.TakePreload(request));
    lib.Fail(a);
    CHECK(!lib.TakePreload(request));
    CHECK(lib.GetPreloadProgress().queued == 0);
    CHECK(lib.GetPreloadProgress().done == 0);
}

static void AnAliasCarriesItsSetsToTheImage()
{
    Library lib;
    lib.SetNow(100000);
    const PluginImageId item = Wanted(lib, lib.Item("iron_plate"));
    lib.AddToSets(item, PLUGIN_IMAGE_SET_ITEMS);
    Request request;
    CHECK(lib.TakeNext(request) && request.id == item);
    const PluginImageId path = Wanted(lib, lib.Path(kPathA));
    CHECK(lib.TakeNext(request) && request.id == path);
    lib.Requeue(path);
    lib.Requeue(item);
    CHECK(lib.Alias(item, path));
    lib.UseSets(Self(0), PLUGIN_IMAGE_SET_ITEMS);
    // The image is now in the set.
    CHECK(lib.GetPreloadProgress().queued == 1);
}

// ---------------------------------------------------------------------------
// Size-exact copies

// The texture handle the pump would have made for the copy of an image at a size.
static PluginTextureHandle ExactTexture(PluginImageId id, uint32_t size)
{
    return reinterpret_cast<PluginTextureHandle>((static_cast<uintptr_t>(id) * 4096 + size) * 16 + 8);
}

// A path image that is read and kept, asked for at `now`.
static PluginImageId KeptImage(Library& lib, const char* path, uint64_t now, uint64_t bytes = 100)
{
    lib.SetNow(now);
    const PluginImageId id = Wanted(lib, lib.Path(path));
    std::vector<Released> evicted;
    ReadNext(lib, bytes, evicted);
    return id;
}

// The plugin draws the image at the size once per 10 ms frame, from `from` to `to`.
static void DrawAt(Library& lib, PluginImageId id, int size, uint64_t from, uint64_t to)
{
    for (uint64_t t = from; t <= to; t += 10)
    {
        lib.SetNow(t);
        lib.GetSized(id, size, nullptr, nullptr);
    }
}

static void ExactAnswersTheStoredPictureFirst()
{
    Library lib;
    lib.SetNow(1000);
    const PluginImageId id = Wanted(lib, lib.Path("/Game/UI/T_A.T_A"));
    int w = 7, h = 7;
    CHECK(lib.GetSized(id, 32, &w, &h) == nullptr);   // not kept yet: what Get gives
    CHECK(w == 0 && h == 0);

    std::vector<Released> evicted;
    ReadNext(lib, 100, evicted);
    CHECK(lib.GetSized(id, 32, &w, &h) == Texture(id));
    CHECK(w == 0 && h == 0);
    CHECK(lib.GetSized(id, 32, nullptr, nullptr) == Texture(id));

    // A size that is 0, negative or not under the stored size is the stored picture and asks for nothing.
    CHECK(lib.GetSized(id, 0, &w, &h) == Texture(id));
    CHECK(lib.GetSized(id, -5, &w, &h) == Texture(id));
    CHECK(lib.GetSized(id, static_cast<int>(kStoredSize), &w, &h) == Texture(id));
    CHECK(lib.GetSized(id, 4000, &w, &h) == Texture(id));
    DrawAt(lib, id, 256, 1000, 3000);
    DrawAt(lib, id, 0, 1000, 3000);
    ExactRequest request;
    CHECK(!lib.TakeExact(request));
    CHECK(lib.GetSized(0, 32, &w, &h) == nullptr);
    CHECK(lib.GetSized(9999, 32, &w, &h) == nullptr);
}

static void ExactIsMadeForASizeAskedForSteadily()
{
    Library lib;
    const PluginImageId id = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    ExactRequest request;

    DrawAt(lib, id, 36, 1000, 1000 + kExactSteadyMs - 10);
    CHECK(!lib.TakeExact(request));   // not long enough yet

    DrawAt(lib, id, 36, 1000 + kExactSteadyMs, 1000 + kExactSteadyMs + 20);
    CHECK(lib.TakeExact(request));
    CHECK(request.id == id && request.size == 36);
    CHECK(request.name == "/Game/UI/T_A.T_A");
    CHECK(!lib.TakeExact(request));   // handed out once

    // Under way: the stored picture is still what is drawn, and nothing is asked twice.
    int w = 0, h = 0;
    DrawAt(lib, id, 36, 1500, 1800);
    CHECK(lib.GetSized(id, 36, &w, &h) == Texture(id));
    CHECK(!lib.TakeExact(request));
    CHECK(lib.ExactCount() == 1);

    std::vector<Released> dropped;
    CHECK(lib.PublishExact(id, 36, ExactTexture(id, 36), 36, 30, 36 * 30 * 4, dropped));
    CHECK(dropped.empty());
    CHECK(lib.BytesKept() == 100 + 36 * 30 * 4);   // the copy counts against the cap
    CHECK(lib.CountKept() == 1);                   // and is not an image

    CHECK(lib.GetSized(id, 36, &w, &h) == ExactTexture(id, 36));
    CHECK(w == 36 && h == 30);
    CHECK(lib.GetSized(id, 36, nullptr, nullptr) == ExactTexture(id, 36));
    CHECK(lib.Get(id) == Texture(id));             // Get is unchanged
    CHECK(lib.State(id) == PLUGIN_IMAGE_READY);

    // Another size is still the stored picture.
    CHECK(lib.GetSized(id, 48, &w, &h) == Texture(id));
    CHECK(w == 0 && h == 0);
}

static void ASizeThatKeepsChangingIsNeverMade()
{
    Library lib;
    const PluginImageId id = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    // A window dragged: a new size every frame for ten seconds.
    int size = 20;
    for (uint64_t t = 1000; t < 11000; t += 10)
    {
        lib.SetNow(t);
        lib.GetSized(id, size, nullptr, nullptr);
        size = size >= 60 ? 20 : size + 1;
    }
    ExactRequest request;
    CHECK(!lib.TakeExact(request));
    CHECK(lib.ExactCount() == 0);
}

static void TwoSizesDrawnTogetherAreBothMade()
{
    Library lib;
    const PluginImageId id = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    for (uint64_t t = 1000; t <= 1500; t += 10)
    {
        lib.SetNow(t);
        lib.GetSized(id, 20, nullptr, nullptr);
        lib.GetSized(id, 48, nullptr, nullptr);
    }
    ExactRequest a, b, c;
    CHECK(lib.TakeExact(a));
    CHECK(lib.TakeExact(b));
    CHECK(!lib.TakeExact(c));
    CHECK((a.size == 20 && b.size == 48) || (a.size == 48 && b.size == 20));
}

static void AGapRestartsTheSteadyCount()
{
    Library lib;
    const PluginImageId id = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    DrawAt(lib, id, 36, 1000, 1300);
    const uint64_t resume = 1300 + kExactGapMs + 50;
    DrawAt(lib, id, 36, resume, resume + 300);   // 300 ms, a gap, 300 ms: never 400 steady
    ExactRequest request;
    CHECK(!lib.TakeExact(request));
    DrawAt(lib, id, 36, resume + 310, resume + 500);
    CHECK(lib.TakeExact(request));
}

static void AFailedCopyIsNotAskedForAgainAtOnce()
{
    Library lib;
    const PluginImageId id = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    DrawAt(lib, id, 36, 1000, 1500);
    ExactRequest request;
    CHECK(lib.TakeExact(request));
    lib.SetNow(1500);
    lib.ExactEnded(id, 36, ExactEnd::Failed);
    CHECK(lib.ExactCount() == 0);

    // Drawn every frame for a while: the stored picture, and no new request.
    int w = 3, h = 3;
    DrawAt(lib, id, 36, 1510, 1500 + kExactRetryMs - 100);
    CHECK(lib.GetSized(id, 36, &w, &h) == Texture(id));
    CHECK(w == 0 && h == 0);
    CHECK(!lib.TakeExact(request));

    // Later it is tried again.
    DrawAt(lib, id, 36, 1500 + kExactRetryMs, 1500 + kExactRetryMs + 500);
    CHECK(lib.TakeExact(request) && request.size == 36);
    std::vector<Released> dropped;
    CHECK(lib.PublishExact(id, 36, ExactTexture(id, 36), 36, 36, 36 * 36 * 4, dropped));
}

static void ACopyThatIsNotNeededIsNeverAskedForAgain()
{
    Library lib;
    const PluginImageId id = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    DrawAt(lib, id, 200, 1000, 1500);
    ExactRequest request;
    CHECK(lib.TakeExact(request));
    lib.ExactEnded(id, 200, ExactEnd::NotNeeded);
    DrawAt(lib, id, 200, 1510, 1510 + 3 * kExactRetryMs);
    CHECK(!lib.TakeExact(request));
    CHECK(lib.GetSized(id, 200, nullptr, nullptr) == Texture(id));
}

static void ARequestThatCouldNotStartIsOfferedAgain()
{
    Library lib;
    const PluginImageId id = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    DrawAt(lib, id, 36, 1000, 1500);
    ExactRequest request;
    CHECK(lib.TakeExact(request));
    lib.ExactEnded(id, 36, ExactEnd::Retry);
    CHECK(lib.ExactCount() == 1);
    ExactRequest again;
    CHECK(lib.TakeExact(again) && again.id == id && again.size == 36);
    CHECK(!lib.TakeExact(again));
}

static void ACopyCountPerImageIsLimited()
{
    Library lib;
    const PluginImageId id = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    std::vector<Released> dropped;
    const int sizes[] = { 20, 28, 36, 48 };
    uint64_t t = 1000;
    for (int i = 0; i < 3; ++i)
    {
        DrawAt(lib, id, sizes[i], t, t + 450);
        t += 500;
        ExactRequest request;
        CHECK(lib.TakeExact(request) && request.size == static_cast<uint32_t>(sizes[i]));
        CHECK(lib.PublishExact(id, request.size, ExactTexture(id, request.size), request.size, request.size, 400, dropped));
    }
    CHECK(lib.ExactCount() == kExactPerImage);

    // All three are drawn lately: a fourth size is refused.
    for (uint64_t u = t; u <= t + 450; u += 10)
    {
        lib.SetNow(u);
        for (int i = 0; i < 4; ++i)
            lib.GetSized(id, sizes[i], nullptr, nullptr);
    }
    ExactRequest request;
    CHECK(!lib.TakeExact(request));
    CHECK(lib.ExactCount() == kExactPerImage);
}

static void ACopyCountAcrossImagesIsLimited()
{
    Library lib;
    lib.SetCaps(1ull << 40, 4096);
    std::vector<PluginImageId> ids;
    for (uint32_t i = 0; i < kExactCountCap + 10; ++i)
        ids.push_back(KeptImage(lib, ("/Game/UI/T_" + std::to_string(i) + ".T_" + std::to_string(i)).c_str(), 1000, 10));
    for (uint64_t t = 1000; t <= 1450; t += 10)
    {
        lib.SetNow(t);
        for (PluginImageId id : ids)
            lib.GetSized(id, 32, nullptr, nullptr);
    }
    uint32_t taken = 0;
    ExactRequest request;
    while (lib.TakeExact(request))
        ++taken;
    CHECK(taken == kExactCountCap);
    CHECK(lib.ExactCount() == kExactCountCap);
}

static void CopiesCountAgainstTheByteCap()
{
    Library lib;
    lib.SetCaps(1100, 100);
    const PluginImageId a = KeptImage(lib, "/Game/UI/T_A.T_A", 1000, 400);
    KeptImage(lib, "/Game/UI/T_B.T_B", 1000, 400);
    DrawAt(lib, a, 8, 1000, 1450);
    ExactRequest request;
    CHECK(lib.TakeExact(request) && request.id == a);

    std::vector<Released> dropped;
    CHECK(lib.PublishExact(a, 8, ExactTexture(a, 8), 8, 8, 256, dropped));
    CHECK(lib.BytesKept() == 400 + 400 + 256);

    // A second copy would leave the total over the cap with every stored image drawn lately: it
    // is asked for, and refused when it is finished, for the pump to free.
    lib.SetNow(1500);
    lib.Get(a);
    lib.Get(a + 1);
    for (uint64_t t = 1500; t <= 1950; t += 10)
    {
        lib.SetNow(t);
        lib.GetSized(a, 8, nullptr, nullptr);   // the first copy stays in use
        lib.GetSized(a, 9, nullptr, nullptr);
    }
    CHECK(lib.TakeExact(request));
    CHECK(!lib.PublishExact(a, 9, ExactTexture(a, 9), 9, 9, 324, dropped));
    CHECK(dropped.empty());
    CHECK(lib.BytesKept() == 400 + 400 + 256);
    CHECK(lib.GetSized(a, 9, nullptr, nullptr) == Texture(a));
}

static void CopiesNotDrawnLatelyGoBeforeStoredImages()
{
    Library lib;
    lib.SetCaps(1000, 100);
    const PluginImageId a = KeptImage(lib, "/Game/UI/T_A.T_A", 1000, 400);
    DrawAt(lib, a, 8, 1000, 1450);
    ExactRequest request;
    CHECK(lib.TakeExact(request));
    std::vector<Released> dropped;
    CHECK(lib.PublishExact(a, 8, ExactTexture(a, 8), 8, 8, 256, dropped));

    // Much later a newcomer needs room; the stored image was drawn since, the copy was not.
    lib.SetNow(5000);
    lib.Get(a);
    Wanted(lib, lib.Path("/Game/UI/T_B.T_B"));
    std::vector<Released> evicted;
    ReadNext(lib, 700, evicted);
    CHECK(evicted.size() == 1 && evicted[0].exact && evicted[0].handle == ExactTexture(a, 8));
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY);
    CHECK(lib.BytesKept() == 400 + 700);
    CHECK(lib.GetSized(a, 8, nullptr, nullptr) == Texture(a));
}

// A full byte cap does not stop a copy for an image that is being drawn: the pictures not seen
// lately make room, as they would for a newcomer.
static void AFullByteCapStillTakesACopyForADrawnImage()
{
    Library lib;
    lib.SetCaps(1000, 100);
    const PluginImageId drawn = KeptImage(lib, "/Game/UI/T_A.T_A", 1000, 400);
    const PluginImageId idle  = KeptImage(lib, "/Game/UI/T_B.T_B", 1000, 400);   // preload filled the cap with it
    CHECK(lib.BytesKept() == 800);

    DrawAt(lib, drawn, 8, 5000, 5450);
    ExactRequest request;
    CHECK(lib.TakeExact(request) && request.id == drawn && request.size == 8);
    std::vector<Released> dropped;
    CHECK(lib.PublishExact(drawn, 8, ExactTexture(drawn, 8), 8, 8, 256, dropped));
    CHECK(lib.State(drawn) == PLUGIN_IMAGE_READY);
    CHECK(lib.State(idle) != PLUGIN_IMAGE_READY);
    CHECK(dropped.size() == 1 && !dropped[0].exact && dropped[0].handle == Texture(idle));
    CHECK(lib.BytesKept() == 400 + 256);
    int w = 0, h = 0;
    CHECK(lib.GetSized(drawn, 8, &w, &h) == ExactTexture(drawn, 8));
    CHECK(w == 8 && h == 8);
}

// Images seen lately are still never evicted for a copy.
static void ACopyNeverEvictsAnImageSeenLately()
{
    Library lib;
    lib.SetCaps(1000, 100);
    const PluginImageId a = KeptImage(lib, "/Game/UI/T_A.T_A", 1000, 400);
    const PluginImageId b = KeptImage(lib, "/Game/UI/T_B.T_B", 1000, 400);
    for (uint64_t t = 5000; t <= 5450; t += 10)
    {
        lib.SetNow(t);
        lib.Get(b);
        lib.GetSized(a, 8, nullptr, nullptr);
    }
    ExactRequest request;
    CHECK(lib.TakeExact(request));
    std::vector<Released> dropped;
    CHECK(!lib.PublishExact(a, 8, ExactTexture(a, 8), 8, 8, 256, dropped));
    CHECK(dropped.empty());
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY && lib.State(b) == PLUGIN_IMAGE_READY);
}

// At the copy count cap a refused size costs the render thread one lock for an offer, then waits
// kExactRefusedMs; the room comes from the pump's sweep, which drops a copy not drawn lately.
static void ARefusedOfferBacksOffAndTheSweepMakesRoom()
{
    Library lib;
    lib.SetCaps(1ull << 40, 4096);
    std::vector<PluginImageId> ids;
    for (uint32_t i = 0; i < kExactCountCap + 1; ++i)
        ids.push_back(KeptImage(lib, ("/Game/UI/T_" + std::to_string(i) + ".T_" + std::to_string(i)).c_str(), 1000, 10));
    for (uint64_t t = 1000; t <= 1450; t += 10)
    {
        lib.SetNow(t);
        for (uint32_t i = 0; i < kExactCountCap; ++i)
            lib.GetSized(ids[i], 32, nullptr, nullptr);
    }
    std::vector<Released> dropped;
    ExactRequest request;
    while (lib.TakeExact(request))
        CHECK(lib.PublishExact(request.id, request.size, ExactTexture(request.id, request.size), 32, 32, 40, dropped));
    CHECK(lib.ExactCount() == kExactCountCap);

    // The last image is drawn steadily from 3000; the copies were last drawn at 1450.
    const PluginImageId last = ids[kExactCountCap];
    DrawAt(lib, last, 32, 3000, 3400);   // the offer is made at 3400 and refused: no room
    CHECK(!lib.TakeExact(request));
    CHECK(lib.ExactCount() == kExactCountCap);

    lib.SetNow(3410);
    dropped.clear();
    lib.ReapExact(dropped);
    CHECK(dropped.size() == 1 && dropped[0].exact);
    CHECK(lib.ExactCount() == kExactCountCap - 1);

    // Room now, but the size is not offered again until it has been drawn another kExactRefusedMs and a steady stretch.
    DrawAt(lib, last, 32, 3410, 3400 + kExactRefusedMs - 10);
    CHECK(!lib.TakeExact(request));
    DrawAt(lib, last, 32, 3400 + kExactRefusedMs, 3400 + kExactRefusedMs + kExactSteadyMs + 20);
    CHECK(lib.TakeExact(request) && request.id == last && request.size == 32);
}

// With every copy drawn lately there is nobody to drop: the sweep leaves them all.
static void TheSweepKeepsCopiesDrawnLately()
{
    Library lib;
    lib.SetCaps(1ull << 40, 4096);
    std::vector<PluginImageId> ids;
    for (uint32_t i = 0; i < kExactCountCap + 1; ++i)
        ids.push_back(KeptImage(lib, ("/Game/UI/T_" + std::to_string(i) + ".T_" + std::to_string(i)).c_str(), 1000, 10));
    for (uint64_t t = 1000; t <= 1450; t += 10)
    {
        lib.SetNow(t);
        for (uint32_t i = 0; i < kExactCountCap + 1; ++i)
            lib.GetSized(ids[i], 32, nullptr, nullptr);
    }
    std::vector<Released> dropped;
    ExactRequest request;
    while (lib.TakeExact(request))
        lib.PublishExact(request.id, request.size, ExactTexture(request.id, request.size), 32, 32, 40, dropped);
    lib.SetNow(1500);
    lib.ReapExact(dropped);
    CHECK(dropped.empty());
    CHECK(lib.ExactCount() == kExactCountCap);
}

// Three plugins draw one item at 36, 32 and 28 pixels in the same frame: each size gets a copy.
static void ThreeSizesDrawnTogetherAreAllMade()
{
    Library lib;
    const PluginImageId id = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    for (uint64_t t = 1000; t <= 1500; t += 10)
    {
        lib.SetNow(t);
        lib.GetSized(id, 36, nullptr, nullptr);
        lib.GetSized(id, 32, nullptr, nullptr);
        lib.GetSized(id, 28, nullptr, nullptr);
    }
    bool got[3] = { false, false, false };
    ExactRequest request;
    uint32_t taken = 0;
    while (lib.TakeExact(request))
    {
        ++taken;
        got[request.size == 36 ? 0 : request.size == 32 ? 1 : 2] |= request.size == 36 || request.size == 32 || request.size == 28;
    }
    CHECK(taken == 3);
    CHECK(got[0] && got[1] && got[2]);
}

// Three steady sizes and a fourth that changes every frame: the three get copies, the changing one none.
static void ASizeThatChangesAmongSteadySizesIsNeverMade()
{
    Library lib;
    const PluginImageId id = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    int moving = 40;
    for (uint64_t t = 1000; t <= 3000; t += 10)
    {
        lib.SetNow(t);
        lib.GetSized(id, 36, nullptr, nullptr);
        lib.GetSized(id, 32, nullptr, nullptr);
        lib.GetSized(id, 28, nullptr, nullptr);
        lib.GetSized(id, moving, nullptr, nullptr);
        moving = moving >= 90 ? 40 : moving + 1;
    }
    uint32_t taken = 0;
    ExactRequest request;
    while (lib.TakeExact(request))
    {
        ++taken;
        CHECK(request.size == 36 || request.size == 32 || request.size == 28);
    }
    CHECK(taken == 3);
}

static void TheInterfaceNumberCoversTheSizedEntry()
{
    // The table grew from 56 to 64 bytes: a loader with the short table must refuse a plugin built for it.
    CHECK(PLUGIN_INTERFACE_VERSION == 77);
    CHECK(PLUGIN_INTERFACE_VERSION_MAX == 77);
    CHECK(PLUGIN_INTERFACE_VERSION_MIN == 70);
    CHECK(sizeof(IPluginImages) == 64);
}

static void EvictingTheStoredPictureTakesItsCopies()
{
    Library lib;
    lib.SetCaps(1000, 100);
    const PluginImageId a = KeptImage(lib, "/Game/UI/T_A.T_A", 1000, 500);
    DrawAt(lib, a, 8, 1000, 1450);
    ExactRequest request;
    CHECK(lib.TakeExact(request));
    std::vector<Released> dropped;
    CHECK(lib.PublishExact(a, 8, ExactTexture(a, 8), 8, 8, 100, dropped));

    // a is not drawn again; the newcomer needs its room.
    lib.SetNow(9000);
    Wanted(lib, lib.Path("/Game/UI/T_B.T_B"));
    std::vector<Released> evicted;
    ReadNext(lib, 900, evicted);
    CHECK(lib.State(a) != PLUGIN_IMAGE_READY);
    bool storedGone = false, copyGone = false;
    for (const Released& r : evicted)
    {
        storedGone |= r.handle == Texture(a) && !r.exact;
        copyGone   |= r.handle == ExactTexture(a, 8) && r.exact;
    }
    CHECK(storedGone && copyGone);
    CHECK(lib.ExactCount() == 0);
    CHECK(lib.BytesKept() == 900);
    int w = 1, h = 1;
    CHECK(lib.GetSized(a, 8, &w, &h) == nullptr);
    CHECK(w == 0 && h == 0);
}

static void ACopyFinishedForAnEvictedImageIsRefused()
{
    Library lib;
    lib.SetCaps(1000, 100);
    const PluginImageId a = KeptImage(lib, "/Game/UI/T_A.T_A", 1000, 500);
    DrawAt(lib, a, 8, 1000, 1450);
    ExactRequest request;
    CHECK(lib.TakeExact(request));

    lib.SetNow(9000);
    Wanted(lib, lib.Path("/Game/UI/T_B.T_B"));
    std::vector<Released> evicted;
    ReadNext(lib, 900, evicted);
    CHECK(lib.State(a) != PLUGIN_IMAGE_READY);

    // The worker finishes after that: the pump is told to free the texture itself.
    std::vector<Released> dropped;
    CHECK(!lib.PublishExact(a, 8, ExactTexture(a, 8), 8, 8, 256, dropped));
    CHECK(dropped.empty());
    CHECK(lib.BytesKept() == 900);
    lib.ExactEnded(a, 8, ExactEnd::Failed);
    CHECK(lib.ExactCount() == 0);
}

static void ClearingTheCacheTakesEveryCopy()
{
    Library lib;
    const PluginImageId a = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    const PluginImageId b = KeptImage(lib, "/Game/UI/T_B.T_B", 1000);
    for (uint64_t t = 1000; t <= 1450; t += 10)
    {
        lib.SetNow(t);
        lib.GetSized(a, 20, nullptr, nullptr);
        lib.GetSized(b, 20, nullptr, nullptr);
    }
    ExactRequest ra, rb;
    CHECK(lib.TakeExact(ra) && lib.TakeExact(rb));
    std::vector<Released> dropped;
    CHECK(lib.PublishExact(ra.id, 20, ExactTexture(ra.id, 20), 20, 20, 1600, dropped));
    // rb is still being made when the cache is cleared.

    lib.ReleaseExact(dropped);
    CHECK(dropped.size() == 1 && dropped[0].exact && dropped[0].handle == ExactTexture(ra.id, 20));
    CHECK(lib.ExactCount() == 0);
    CHECK(lib.BytesKept() == 200);
    CHECK(lib.State(a) == PLUGIN_IMAGE_READY && lib.State(b) == PLUGIN_IMAGE_READY);
    CHECK(lib.GetSized(a, 20, nullptr, nullptr) == Texture(a));

    // The unfinished one arrives afterwards and is refused.
    dropped.clear();
    CHECK(!lib.PublishExact(rb.id, 20, ExactTexture(rb.id, 20), 20, 20, 1600, dropped));
    CHECK(dropped.empty());

    // And a copy can be asked for again.
    DrawAt(lib, a, 20, 2000, 2500);
    ExactRequest again;
    CHECK(lib.TakeExact(again) && again.size == 20);
}

static void ACopyNotDrawnForAWhileIsDropped()
{
    Library lib;
    const PluginImageId a = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    DrawAt(lib, a, 20, 1000, 1450);
    ExactRequest request;
    CHECK(lib.TakeExact(request));
    std::vector<Released> dropped;
    CHECK(lib.PublishExact(a, 20, ExactTexture(a, 20), 20, 20, 1600, dropped));

    // Drawn every second, so it stays.
    const uint64_t last = 2000 + 2 * kExactIdleMs;
    for (uint64_t t = 2000; t <= last; t += 1000)
    {
        lib.SetNow(t);
        lib.GetSized(a, 20, nullptr, nullptr);
        lib.ReapExact(dropped);
    }
    CHECK(dropped.empty() && lib.ExactCount() == 1);

    // Not drawn for kExactIdleMs: the next sweep drops it.
    lib.SetNow(last + kExactIdleMs - 1000);
    lib.ReapExact(dropped);
    CHECK(dropped.empty());
    lib.SetNow(last + kExactIdleMs + 2000);
    lib.ReapExact(dropped);
    CHECK(dropped.size() == 1 && dropped[0].exact && dropped[0].handle == ExactTexture(a, 20));
    CHECK(lib.ExactCount() == 0);
    CHECK(lib.BytesKept() == 100);
}

static void ReleaseAllHandsBackTheCopiesToo()
{
    Library lib;
    const PluginImageId a = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    DrawAt(lib, a, 20, 1000, 1450);
    ExactRequest request;
    CHECK(lib.TakeExact(request));
    std::vector<Released> dropped;
    CHECK(lib.PublishExact(a, 20, ExactTexture(a, 20), 20, 20, 1600, dropped));

    std::vector<Released> all;
    lib.ReleaseAll(all);
    CHECK(all.size() == 2);
    bool copy = false;
    for (const Released& r : all)
        copy |= r.exact && r.handle == ExactTexture(a, 20);
    CHECK(copy);
    CHECK(lib.ExactCount() == 0 && lib.BytesKept() == 0);
    CHECK(lib.GetSized(a, 20, nullptr, nullptr) == nullptr);
}

static void ACopyWithNoTextureEndsTheRequest()
{
    Library lib;
    const PluginImageId a = KeptImage(lib, "/Game/UI/T_A.T_A", 1000);
    DrawAt(lib, a, 20, 1000, 1450);
    ExactRequest request;
    CHECK(lib.TakeExact(request));
    std::vector<Released> dropped;
    lib.ExactEnded(a, 20, ExactEnd::Failed);
    CHECK(lib.ExactCount() == 0);
    CHECK(lib.GetSized(a, 20, nullptr, nullptr) == Texture(a));
}

static void TheInterfaceEntryReachesGetSized()
{
    const IPluginImages* table = GetInterface();
    CHECK(table->GetSized != nullptr);
    int w = 5, h = 5;
    CHECK(table->GetSized(0, 32, &w, &h) == nullptr);
    CHECK(w == 0 && h == 0);
    CHECK(table->GetSized(0, 32, nullptr, nullptr) == nullptr);
}

// A reader draws at sizes while the table makes and drops copies under it. Every handle it gets is
// the stored picture or a copy of that very image at that very size.
static void ReadersSeeOnlyTheirOwnCopiesWhileCopiesChange()
{
    Library lib;
    lib.SetCaps(1ull << 30, 1000);
    constexpr uint32_t kImages = 20;
    std::vector<PluginImageId> ids;
    for (uint32_t i = 0; i < kImages; ++i)
        ids.push_back(KeptImage(lib, ("/Game/UI/T_R" + std::to_string(i) + ".T_R" + std::to_string(i)).c_str(), 1000));

    std::atomic<bool> stop{ false };
    std::atomic<int> bad{ 0 };
    std::thread reader([&]
    {
        while (!stop.load())
            for (PluginImageId id : ids)
                for (int size = 16; size <= 48; size += 16)
                {
                    int w = 0, h = 0;
                    const PluginTextureHandle t = lib.GetSized(id, size, &w, &h);
                    if (t == Texture(id))
                    {
                        if (w != 0 || h != 0)
                            ++bad;
                    }
                    else if (t != nullptr && !(t == ExactTexture(id, static_cast<uint32_t>(size)) && w == size && h == size))
                        ++bad;
                }
    });

    std::vector<Released> dropped;
    uint64_t now = 1000;
    for (int round = 0; round < 400; ++round)
    {
        now += 60;
        lib.SetNow(now);
        ExactRequest request;
        while (lib.TakeExact(request))
            lib.PublishExact(request.id, request.size, ExactTexture(request.id, request.size), request.size, request.size, 100, dropped);
        if (round % 50 == 49)
            lib.ReleaseExact(dropped);
        lib.ReapExact(dropped);
        dropped.clear();
    }
    stop.store(true);
    reader.join();
    CHECK(bad.load() == 0);
}

int main()
{
    RunTest("normalises item names", NormalisesItemNames);
    RunTest("normalises object paths", NormalisesObjectPaths);
    RunTest("ids are stable and never zero", IdsAreStableAndNeverZero);
    RunTest("the request keeps the name as asked for", TheRequestKeepsTheNameAsAskedFor);
    RunTest("ids run out at the table size", IdsRunOutAtTheTableSize);
    RunTest("states follow the read", StatesFollowTheRead);
    RunTest("State does not mark an image seen", StateDoesNotMarkSeen);
    RunTest("seen images are read first", SeenImagesAreReadFirst);
    RunTest("requests nobody drew are dropped", RequestsNobodyDrewAreDropped);
    RunTest("set members are kept and served after seen ones", SetMembersAreKeptAndServedAfterSeenOnes);
    RunTest("set interest is recorded and dropped", SetInterestIsRecordedAndDropped);
    RunTest("the byte cap evicts the least recently seen", ByteCapEvictsLeastRecentlySeen);
    RunTest("the count cap evicts the least recently seen", CountCapEvictsLeastRecentlySeen);
    RunTest("equal stamps evict the lower id first", EqualStampsEvictTheLowerIdFirst);
    RunTest("an image seen lately is never evicted", AnImageSeenLatelyIsNeverEvicted);
    RunTest("room counts free slots and what can be evicted", RoomForCountsFreeSlotsAndWhatCanBeEvicted);
    RunTest("room follows the byte cap", RoomForFollowsTheByteCap);
    RunTest("has queued sees asked for images", HasQueuedSeesAskedForImages);
    RunTest("a copy over the byte cap is refused", ACopyOverTheByteCapIsRefused);
    RunTest("an evicted image is asked for again by Get", AnEvictedImageIsAskedForAgainByGet);
    RunTest("a lower cap evicts on the next publish", ANewCapEvictsOnTheNextPublish);
    RunTest("ReleaseAll hands back every kept copy", ReleaseAllHandsBackEveryKeptCopy);
    RunTest("the byte cap follows the video budget", ByteCapFollowsTheVideoBudget);
    RunTest("aliased ids are one image", AliasedIdsAreOneImage);
    RunTest("Canonical names the one image", CanonicalNamesTheOneImage);
    RunTest("registering an id is not requesting the image", RegisteringIsNotRequesting);
    RunTest("alias refusals", AliasRefusals);
    RunTest("the interface reaches the same table", InterfaceReachesTheSameTable);
    RunTest("readers see consistent states while the table changes", ReadersSeeConsistentStatesWhileTheTableChanges);
    RunTest("a member of a set is idle until preload reaches it", AMemberOfASetIsIdleUntilPreloadReachesIt);
    RunTest("used sets are the union and follow the plugins", UsedSetsAreTheUnionAndFollowThePlugins);
    RunTest("preload only takes members of sets in use", PreloadOnlyTakesMembersOfSetsInUse);
    RunTest("preload goes round the members and defer gives one back", PreloadGoesRoundTheMembersAndDeferGivesOneBack);
    RunTest("a deferred image that was drawn goes to the queue", ADeferredImageThatWasDrawnGoesToTheQueue);
    RunTest("drawing a member asks for it and jumps the queue", DrawingAMemberAsksForItAndJumpsTheQueue);
    RunTest("seen waiting counts only images drawn lately", SeenWaitingCountsOnlyImagesDrawnLately);
    RunTest("preload stops when another image would not fit", PreloadStopsWhenAnotherImageWouldNotFit);
    RunTest("preload stays stopped once an image has been evicted", PreloadStaysStoppedOnceAnImageHasBeenEvicted);
    RunTest("preloaded images nobody drew are evicted first", PreloadedImagesNobodyDrewAreEvictedFirst);
    RunTest("preload progress counts the members of sets in use", PreloadProgressCountsTheMembersOfSetsInUse);
    RunTest("a member that fails is not taken again", AMemberThatFailsIsNotTakenAgain);
    RunTest("an alias carries its sets to the image", AnAliasCarriesItsSetsToTheImage);
    RunTest("exact: the stored picture answers first", ExactAnswersTheStoredPictureFirst);
    RunTest("exact: made for a size asked for steadily", ExactIsMadeForASizeAskedForSteadily);
    RunTest("exact: a changing size is never made", ASizeThatKeepsChangingIsNeverMade);
    RunTest("exact: two sizes drawn together are both made", TwoSizesDrawnTogetherAreBothMade);
    RunTest("exact: a gap restarts the steady count", AGapRestartsTheSteadyCount);
    RunTest("exact: a failed copy is not asked again at once", AFailedCopyIsNotAskedForAgainAtOnce);
    RunTest("exact: a copy not needed is never asked again", ACopyThatIsNotNeededIsNeverAskedForAgain);
    RunTest("exact: a request that could not start is offered again", ARequestThatCouldNotStartIsOfferedAgain);
    RunTest("exact: copies per image are limited", ACopyCountPerImageIsLimited);
    RunTest("exact: copies across images are limited", ACopyCountAcrossImagesIsLimited);
    RunTest("exact: copies count against the byte cap", CopiesCountAgainstTheByteCap);
    RunTest("exact: copies not drawn lately go first", CopiesNotDrawnLatelyGoBeforeStoredImages);
    RunTest("exact: a full byte cap still takes a copy for a drawn image", AFullByteCapStillTakesACopyForADrawnImage);
    RunTest("exact: a copy never evicts an image seen lately", ACopyNeverEvictsAnImageSeenLately);
    RunTest("exact: a refused offer backs off and the sweep makes room", ARefusedOfferBacksOffAndTheSweepMakesRoom);
    RunTest("exact: the sweep keeps copies drawn lately", TheSweepKeepsCopiesDrawnLately);
    RunTest("exact: three sizes drawn together are all made", ThreeSizesDrawnTogetherAreAllMade);
    RunTest("exact: a changing size among steady ones is never made", ASizeThatChangesAmongSteadySizesIsNeverMade);
    RunTest("exact: the interface number covers the sized entry", TheInterfaceNumberCoversTheSizedEntry);
    RunTest("exact: evicting the stored picture takes its copies", EvictingTheStoredPictureTakesItsCopies);
    RunTest("exact: a copy for an evicted image is refused", ACopyFinishedForAnEvictedImageIsRefused);
    RunTest("exact: clearing takes every copy", ClearingTheCacheTakesEveryCopy);
    RunTest("exact: an idle copy is dropped", ACopyNotDrawnForAWhileIsDropped);
    RunTest("exact: ReleaseAll hands back copies", ReleaseAllHandsBackTheCopiesToo);
    RunTest("exact: a failed request leaves the stored picture", ACopyWithNoTextureEndsTheRequest);
    RunTest("exact: the interface entry reaches GetSized", TheInterfaceEntryReachesGetSized);
    RunTest("exact: readers see only their own copies", ReadersSeeOnlyTheirOwnCopiesWhileCopiesChange);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
