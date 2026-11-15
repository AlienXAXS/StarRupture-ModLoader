// Image lookup test: the rules of UI/image_lookup.cpp on a fake game. The images
// that count as no image, the item index and its budget per tick, and the
// texture lookup with its cache and how it is invalidated.
//
// A console program: no game. The lookups and the library are the loader's own
// sources; a texture here is an address that stands in for a texture object.
#include "UI/image_lookup.h"

#include <cstdio>
#include <map>
#include <string>
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

using namespace ImageLookup;
using ImageLibrary::Library;

// ---------------------------------------------------------------------------
// No image

static void PlaceholdersAreNoImage()
{
    CHECK(IsPlaceholderImage("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
    CHECK(IsPlaceholderImage("/Engine/EngineMaterials/DefaultWhiteGrid.DefaultWhiteGrid"));
    CHECK(IsPlaceholderImage("/Game/Scene_UnfinishedBuilding/Textures/WhitePlaceholder.WhitePlaceholder"));
    CHECK(IsPlaceholderImage("/Game/UI/Icons/T_NoItem_White_Icon.T_NoItem_White_Icon"));
}

static void PlaceholdersAreRecognisedInEveryFormAPathComesIn()
{
    CHECK(IsPlaceholderImage("/ENGINE/engineresources/whitesquaretexture.WHITESQUARETEXTURE"));
    CHECK(IsPlaceholderImage("/Engine/EngineResources/WhiteSquareTexture"));   // the package path alone
    CHECK(IsPlaceholderImage("Texture2D'/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture'"));
    CHECK(IsPlaceholderImage("  /Engine/EngineMaterials/DefaultWhiteGrid.DefaultWhiteGrid  "));
}

static void OtherTexturesAreNotPlaceholders()
{
    CHECK(!IsPlaceholderImage("/Game/UI/Icons/T_IronPlate_Icon.T_IronPlate_Icon"));
    // The name has to be the asset's, not part of it or of the folder.
    CHECK(!IsPlaceholderImage("/Game/UI/T_WhiteSquareTextureIcon.T_WhiteSquareTextureIcon"));
    CHECK(!IsPlaceholderImage("/Game/WhiteSquareTexture/T_A.T_A"));
    CHECK(!IsPlaceholderImage("/Game/UI/T_A.WhiteSquareTexture_Extra"));
    CHECK(!IsPlaceholderImage("/Game/UI/T_NoItem_White_Icon_Big.T_NoItem_White_Icon_Big"));
    CHECK(!IsPlaceholderImage(""));
    CHECK(!IsPlaceholderImage("WhiteSquareTexture"));   // not a path
    CHECK(!IsPlaceholderImage(nullptr));
}

// ---------------------------------------------------------------------------
// Items

class FakeItems : public ItemSource
{
public:
    struct Item
    {
        std::string name;
        std::string path;
        ItemRead    read = ItemRead::Icon;
        std::string asset;
    };

    std::vector<Item>   items;
    bool                listable   = true;
    uint64_t            clockUs    = 0;
    uint64_t            readCostUs = 0;
    int                 lists      = 0;
    std::vector<size_t> reads;

    bool List() override
    {
        ++lists;
        return listable;
    }
    size_t Count() const override { return items.size(); }
    ItemRead Read(size_t index, ItemRecord& out) override
    {
        reads.push_back(index);
        clockUs += readCostUs;
        out.name = items[index].name;
        out.asset = items[index].asset;
        out.texturePath = items[index].path;
        return items[index].read;
    }

    void Add(const std::string& name, const std::string& path, ItemRead read = ItemRead::Icon, const std::string& asset = std::string())
    {
        items.push_back({ name, path, read, asset });
    }

    // An item the way the game reports one that was never given a unique name.
    void AddUnnamed(const std::string& asset, const std::string& path)
    {
        Add("None", path, ItemRead::Icon, asset);
    }
};

struct IndexRig
{
    FakeItems items;
    ItemIndex index;
    IndexRig() : index(items) {}

    void Step(uint64_t budgetUs = kItemBudgetUs)
    {
        index.Step(budgetUs, [this] { return items.clockUs; });
    }
};

static void AnItemIsFoundByItsNormalizedName()
{
    IndexRig rig;
    rig.items.Add("Item_IronPlate", "/Game/UI/T_Iron.T_Iron");
    rig.Step();

    std::string path;
    CHECK(rig.index.Find("item_ironplate", path) && path == "/Game/UI/T_Iron.T_Iron");
    CHECK(!rig.index.Find("item_copper", path));
    // The key is the library's: the same name in any case finds it.
    CHECK(rig.index.Find(ImageLibrary::NormalizeItemName("  ITEM_ironPLATE"), path));
}

static void AnItemWithNoIconIsKnownWithNoPath()
{
    IndexRig rig;
    rig.items.Add("Item_Empty", "", ItemRead::NoIcon);
    rig.Step();

    std::string path = "left over";
    CHECK(rig.index.Find("item_empty", path));
    CHECK(path.empty());
}

static void SkippedItemsAreNotInTheIndex()
{
    IndexRig rig;
    rig.items.Add("Item_Broken", "", ItemRead::Skipped);
    rig.items.Add("Item_Fine", "/Game/UI/T_A.T_A");
    rig.Step();

    std::string path;
    CHECK(!rig.index.Find("item_broken", path));
    CHECK(rig.index.Find("item_fine", path));
    CHECK(rig.index.GetState() == ItemIndex::State::Done);
}

static void TheFirstItemUnderANameIsKept()
{
    IndexRig rig;
    rig.items.Add("Item_X", "/Game/UI/T_First.T_First");
    rig.items.Add("item_x", "/Game/UI/T_Second.T_Second");
    rig.Step();

    std::string path;
    CHECK(rig.index.Find("item_x", path) && path == "/Game/UI/T_First.T_First");
}

static void AnItemWithoutAUsableNameIsNotKept()
{
    IndexRig rig;
    rig.items.Add("   ", "/Game/UI/T_A.T_A");
    rig.Step();

    std::string path;
    CHECK(!rig.index.Find("", path));
    CHECK(rig.index.GetState() == ItemIndex::State::Done);
}

static void AnUnsetNameIsNeverFound()
{
    IndexRig rig;
    rig.items.AddUnnamed("I_AmmoItem", "/Game/UI/T_Bullets.T_Bullets");
    rig.items.Add("", "/Game/UI/T_B.T_B", ItemRead::Icon, "I_Other");
    rig.Step();

    std::string path;
    CHECK(!rig.index.Find("none", path));
    CHECK(!rig.index.Find("", path));
    // Unset in any case is "none" once it is a key.
    CHECK(!rig.index.Find(ImageLibrary::NormalizeItemName("None"), path));
    CHECK(!rig.index.Find(ImageLibrary::NormalizeItemName("NONE"), path));
    CHECK(!rig.index.Find(ImageLibrary::NormalizeItemName("  nOnE "), path));
    CHECK(ImageLibrary::IsUnsetItemKey(ImageLibrary::NormalizeItemName("None")));
    CHECK(ImageLibrary::IsUnsetItemKey(ImageLibrary::NormalizeItemName("")));
    CHECK(!ImageLibrary::IsUnsetItemKey(ImageLibrary::NormalizeItemName("None_Of_Them")));
    CHECK(rig.index.UnnamedCount() == 2);
}

static void AnUnsetNameIsNotAnAssetNameEither()
{
    IndexRig rig;
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A", ItemRead::Icon, "None");
    rig.Step();

    std::string path;
    CHECK(!rig.index.Find("none", path));
    CHECK(rig.index.Find("item_a", path));
}

static void AnItemIsFoundByItsAssetNameClassNameAndDefaultObjectName()
{
    IndexRig rig;
    rig.items.AddUnnamed("I_AmmoItem", "/Game/UI/T_Bullets.T_Bullets");
    rig.items.AddUnnamed("I_BuildingBlueprint_Furnace", "/Game/UI/T_Blueprint.T_Blueprint");
    rig.Step();

    const char* const forms[] = { "I_AmmoItem", "i_ammoitem", "I_AmmoItem_C", "I_AMMOITEM_c", "Default__I_AmmoItem_C", "default__i_ammoitem_c", "  I_AmmoItem  " };
    for (const char* form : forms)
    {
        std::string path;
        CHECK(rig.index.Find(ImageLibrary::NormalizeItemName(form), path) && path == "/Game/UI/T_Bullets.T_Bullets");
    }

    std::string path;
    CHECK(rig.index.Find(ImageLibrary::NormalizeItemName("Default__I_BuildingBlueprint_Furnace_C"), path) && path == "/Game/UI/T_Blueprint.T_Blueprint");
    CHECK(!rig.index.Find(ImageLibrary::NormalizeItemName("I_AmmoItem_X"), path));
    CHECK(!rig.index.Find(ImageLibrary::NormalizeItemName("Default__I_Nobody_C"), path));
}

static void AnItemWithAUniqueNameIsAlsoFoundByItsAssetName()
{
    IndexRig rig;
    rig.items.Add("Item_IronPlate", "/Game/UI/T_Iron.T_Iron", ItemRead::Icon, "I_IronPlate");
    rig.Step();

    std::string path;
    CHECK(rig.index.Find("item_ironplate", path) && path == "/Game/UI/T_Iron.T_Iron");
    CHECK(rig.index.Find("i_ironplate", path) && path == "/Game/UI/T_Iron.T_Iron");
    CHECK(rig.index.Find("default__i_ironplate_c", path) && path == "/Game/UI/T_Iron.T_Iron");
}

static void AUniqueNameBeatsAnAssetNameInEitherOrder()
{
    // One item's unique name is another item's asset name.
    for (int unnamedFirst = 0; unnamedFirst < 2; ++unnamedFirst)
    {
        IndexRig rig;
        if (unnamedFirst)
            rig.items.AddUnnamed("I_Gold", "/Game/UI/T_AssetOwner.T_AssetOwner");
        rig.items.Add("I_Gold", "/Game/UI/T_NameOwner.T_NameOwner", ItemRead::Icon, "I_Other");
        if (!unnamedFirst)
            rig.items.AddUnnamed("I_Gold", "/Game/UI/T_AssetOwner.T_AssetOwner");
        rig.Step();

        std::string path;
        CHECK(rig.index.Find("i_gold", path) && path == "/Game/UI/T_NameOwner.T_NameOwner");
        // Only the plain text is a unique name: the asset's class name still finds the asset.
        CHECK(rig.index.Find("i_gold_c", path) && path == "/Game/UI/T_AssetOwner.T_AssetOwner");
    }
}

static void AnAssetNameWaitsForTheWholeIndex()
{
    // One item at a time: the unique name belongs to an item that is read after the asset name's.
    {
        IndexRig rig;
        rig.items.readCostUs = kItemBudgetUs;
        rig.items.AddUnnamed("I_Gold", "/Game/UI/T_AssetOwner.T_AssetOwner");
        rig.items.Add("I_Gold", "/Game/UI/T_NameOwner.T_NameOwner", ItemRead::Icon, "I_Other");
        rig.items.Add("X_C", "/Game/UI/T_XName.T_XName", ItemRead::Icon, "I_Y");
        rig.items.AddUnnamed("X", "/Game/UI/T_XAsset.T_XAsset");

        std::string path;
        rig.Step();
        CHECK(rig.index.GetState() == ItemIndex::State::Scanning);
        CHECK(!rig.index.Find("i_gold", path));          // not answered from the asset name yet
        CHECK(!rig.index.Find("default__i_gold_c", path));
        rig.Step();
        CHECK(rig.index.GetState() == ItemIndex::State::Scanning);
        CHECK(rig.index.Find("i_gold", path) && path == "/Game/UI/T_NameOwner.T_NameOwner");   // a unique name is final at once
        CHECK(!rig.index.Find("i_gold_c", path));
        rig.Step();
        rig.Step();
        CHECK(rig.index.GetState() == ItemIndex::State::Done);
        CHECK(rig.index.Find("i_gold", path) && path == "/Game/UI/T_NameOwner.T_NameOwner");
        CHECK(rig.index.Find("i_gold_c", path) && path == "/Game/UI/T_AssetOwner.T_AssetOwner");
        CHECK(rig.index.Find("x_c", path) && path == "/Game/UI/T_XName.T_XName");
        CHECK(rig.index.Find("x", path) && path == "/Game/UI/T_XAsset.T_XAsset");
    }

    // The asset name is read first, and the unique name of an item not read yet is asked for.
    {
        IndexRig rig;
        rig.items.readCostUs = kItemBudgetUs;
        rig.items.AddUnnamed("X", "/Game/UI/T_XAsset.T_XAsset");
        rig.items.Add("X_C", "/Game/UI/T_XName.T_XName", ItemRead::Icon, "I_Y");

        std::string path;
        rig.Step();
        CHECK(!rig.index.Find("x_c", path));
        CHECK(!rig.index.Find("x", path));
        rig.Step();
        CHECK(rig.index.Find("x_c", path) && path == "/Game/UI/T_XName.T_XName");
    }

    // A failed index answers nothing from an asset name.
    {
        IndexRig rig;
        rig.items.AddUnnamed("I_A", "/Game/UI/T_A.T_A");
        rig.items.items.push_back({ "Item_B", "", ItemRead::Unavailable, "I_B" });
        rig.Step();
        CHECK(rig.index.GetState() == ItemIndex::State::Failed);
        std::string path;
        CHECK(!rig.index.Find("i_a", path));
    }
}

static void AUniqueNameThatLooksLikeAClassOrDefaultObjectIsFoundAsItself()
{
    for (int unnamedFirst = 0; unnamedFirst < 2; ++unnamedFirst)
    {
        IndexRig rig;
        if (unnamedFirst)
        {
            rig.items.AddUnnamed("Shield", "/Game/UI/T_ShieldAsset.T_ShieldAsset");
            rig.items.AddUnnamed("Gun", "/Game/UI/T_GunAsset.T_GunAsset");
        }
        rig.items.Add("Default__Shield", "/Game/UI/T_ShieldName.T_ShieldName", ItemRead::Icon, "I_Shield");
        rig.items.Add("Gun_C", "/Game/UI/T_GunName.T_GunName", ItemRead::Icon, "I_Gun");
        if (!unnamedFirst)
        {
            rig.items.AddUnnamed("Shield", "/Game/UI/T_ShieldAsset.T_ShieldAsset");
            rig.items.AddUnnamed("Gun", "/Game/UI/T_GunAsset.T_GunAsset");
        }
        rig.Step();

        // Each is found as itself, never trimmed to the asset it looks like.
        std::string path;
        CHECK(rig.index.Find("default__shield", path) && path == "/Game/UI/T_ShieldName.T_ShieldName");
        CHECK(rig.index.Find("gun_c", path) && path == "/Game/UI/T_GunName.T_GunName");
        // The asset names are still theirs.
        CHECK(rig.index.Find("shield", path) && path == "/Game/UI/T_ShieldAsset.T_ShieldAsset");
        CHECK(rig.index.Find("gun", path) && path == "/Game/UI/T_GunAsset.T_GunAsset");
        CHECK(rig.index.Find("default__gun_c", path) && path == "/Game/UI/T_GunAsset.T_GunAsset");
    }
}

static void TwoUnnamedItemsHaveTheirOwnPictures()
{
    IndexRig rig;
    rig.items.AddUnnamed("I_AmmoItem", "/Game/UI/T_Bullets.T_Bullets");
    rig.items.AddUnnamed("I_BuildingBlueprint_Furnace", "/Game/UI/T_Blueprint.T_Blueprint");
    rig.items.AddUnnamed("I_BuildingBlueprint_Smelter", "/Game/UI/T_Blueprint.T_Blueprint");
    rig.Step();

    std::string ammo, furnace, smelter;
    CHECK(rig.index.Find("i_ammoitem", ammo));
    CHECK(rig.index.Find("i_buildingblueprint_furnace", furnace));
    CHECK(rig.index.Find("i_buildingblueprint_smelter", smelter));
    CHECK(ammo == "/Game/UI/T_Bullets.T_Bullets");
    CHECK(furnace == "/Game/UI/T_Blueprint.T_Blueprint");
    CHECK(smelter == furnace);   // the blueprints share one texture in the game's data
    CHECK(rig.index.UnnamedCount() == 3);
}

static void TheTexturePathsCoverTheUnnamedItems()
{
    IndexRig rig;
    rig.items.AddUnnamed("I_AmmoItem", "/Game/UI/T_Bullets.T_Bullets");
    rig.items.AddUnnamed("I_BuildingBlueprint_Furnace", "/Game/UI/T_Blueprint.T_Blueprint");
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A", ItemRead::Icon, "I_A");
    rig.Step();

    const std::vector<std::string> paths = rig.index.TexturePaths();
    CHECK(paths.size() == 3);
    CHECK(paths.size() == 3 && paths[0] == "/Game/UI/T_A.T_A" && paths[1] == "/Game/UI/T_Blueprint.T_Blueprint" && paths[2] == "/Game/UI/T_Bullets.T_Bullets");
}

static void TheIndexCountsUnnamedItemsAndRepeatedNames()
{
    IndexRig rig;
    rig.items.AddUnnamed("I_A", "/Game/UI/T_A.T_A");
    rig.items.AddUnnamed("I_B", "/Game/UI/T_B.T_B");
    rig.items.Add("Item_X", "/Game/UI/T_X.T_X");
    rig.items.Add("item_x", "/Game/UI/T_Y.T_Y");
    rig.items.Add("ITEM_X", "/Game/UI/T_Z.T_Z");
    rig.items.Add("Item_W", "/Game/UI/T_W.T_W");
    rig.items.Add("Item_W", "/Game/UI/T_W2.T_W2");
    rig.items.Add("Item_Once", "/Game/UI/T_O.T_O");
    rig.Step();

    CHECK(rig.index.UnnamedCount() == 2);
    CHECK(rig.index.RepeatedNameCount() == 2);   // item_x and item_w, each once however many times it came again
    CHECK(rig.index.RepeatedAssetCount() == 0);
}

static void TheIndexCountsAssetNamesMetOnMoreThanOneItem()
{
    IndexRig rig;
    rig.items.AddUnnamed("I_A", "/Game/UI/T_A.T_A");
    rig.items.AddUnnamed("i_a", "/Game/UI/T_B.T_B");
    rig.items.Add("Item_C", "/Game/UI/T_C.T_C", ItemRead::Icon, "I_A");
    rig.items.AddUnnamed("I_D", "/Game/UI/T_D.T_D");
    rig.Step();

    CHECK(rig.index.RepeatedAssetCount() == 1);   // i_a, once however many times it came again
}

static void TheListIsMadeOnTheFirstStepOnly()
{
    IndexRig rig;
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A");
    CHECK(rig.index.GetState() == ItemIndex::State::Idle);
    CHECK(rig.items.lists == 0);

    rig.Step();
    rig.Step();
    CHECK(rig.items.lists == 1);
    CHECK(rig.index.GetState() == ItemIndex::State::Done);
    CHECK(rig.items.reads.size() == 1);   // nothing is read again once done
}

static void AnIndexWhoseListCannotBeMadeFailsOnce()
{
    IndexRig rig;
    rig.items.listable = false;
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A");
    rig.Step();
    CHECK(rig.index.GetState() == ItemIndex::State::Failed);
    rig.Step();
    CHECK(rig.items.lists == 1);
    CHECK(rig.items.reads.empty());
}

static void AnUnavailableReadFailsTheIndex()
{
    IndexRig rig;
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A");
    rig.items.Add("Item_B", "", ItemRead::Unavailable);
    rig.items.Add("Item_C", "/Game/UI/T_C.T_C");
    rig.Step();
    CHECK(rig.index.GetState() == ItemIndex::State::Failed);
    CHECK(rig.items.reads.size() == 2);   // nothing after it

    // What was read before stays known.
    std::string path;
    CHECK(rig.index.Find("item_a", path));
    CHECK(!rig.index.Find("item_c", path));
}

static void AStepStopsWhenTheBudgetIsSpent()
{
    IndexRig rig;
    rig.items.readCostUs = 1000;
    for (int i = 0; i < 10; ++i)
        rig.items.Add("Item_" + std::to_string(i), "/Game/UI/T_A.T_A");

    rig.Step(3000);
    CHECK(rig.items.reads.size() == 3);
    CHECK(rig.index.GetState() == ItemIndex::State::Scanning);
    rig.Step(3000);
    CHECK(rig.items.reads.size() == 6);
    rig.Step(3000);
    rig.Step(3000);
    CHECK(rig.items.reads.size() == 10);
    CHECK(rig.index.GetState() == ItemIndex::State::Done);

    std::string path;
    CHECK(rig.index.Find("item_9", path));
}

static void AStepReadsAtLeastOneItem()
{
    IndexRig rig;
    rig.items.readCostUs = 50 * 1000;   // far over the budget
    for (int i = 0; i < 3; ++i)
        rig.items.Add("Item_" + std::to_string(i), "/Game/UI/T_A.T_A");

    rig.Step();
    CHECK(rig.items.reads.size() == 1);
    rig.Step(0);
    CHECK(rig.items.reads.size() == 2);
}

static void TheBudgetIsTheLoadersThreeMilliseconds()
{
    CHECK(kItemBudgetUs == 3000);
}

static void AStepKnowsWhenTheListIsDone()
{
    IndexRig rig;
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A");
    rig.items.Add("Item_B", "/Game/UI/T_B.T_B");
    rig.Step();
    CHECK(rig.index.GetState() == ItemIndex::State::Done);   // both fit in one slice

    IndexRig empty;
    empty.Step();
    CHECK(empty.index.GetState() == ItemIndex::State::Done);
}

static void TheTexturePathsAreSortedOnceAndSkipTheEmptyAndPlaceholders()
{
    IndexRig rig;
    rig.items.Add("Item_B", "/Game/UI/T_B.T_B");
    rig.items.Add("Item_A", "/Game/UI/T_A.T_A");
    rig.items.Add("Item_A2", "/Game/UI/T_A.T_A");
    rig.items.Add("Item_Empty", "", ItemRead::NoIcon);
    rig.items.Add("Item_White", "/Game/UI/Icons/T_NoItem_White_Icon.T_NoItem_White_Icon");
    rig.items.Add("Item_Broken", "", ItemRead::Skipped);
    rig.Step();

    const std::vector<std::string> paths = rig.index.TexturePaths();
    CHECK(paths.size() == 2);
    CHECK(paths.size() == 2 && paths[0] == "/Game/UI/T_A.T_A" && paths[1] == "/Game/UI/T_B.T_B");

    IndexRig none;
    none.Step();
    CHECK(none.index.TexturePaths().empty());
}

// ---------------------------------------------------------------------------
// Textures

class FakeEngine : public TextureEngine
{
public:
    struct Texture
    {
        TextureSlot slot;
        std::string path;
    };

    bool                           gameThread = true;
    std::map<const void*, Texture> textures;
    int                            slotCalls = 0, pathCalls = 0;
    std::vector<std::string>       logs;

    bool OnGameThread() override { return gameThread; }
    bool SlotOf(const void* texture, TextureSlot& out) override
    {
        ++slotCalls;
        const auto it = textures.find(texture);
        if (it == textures.end())
            return false;
        out = it->second.slot;
        return true;
    }
    bool PathOf(const void* texture, std::string& out) override
    {
        ++pathCalls;
        const auto it = textures.find(texture);
        if (it == textures.end() || it->second.path.empty())
            return false;
        out = it->second.path;
        return true;
    }
    void Log(const std::string& line) override { logs.push_back(line); }
};

struct TextureRig
{
    Library      library;
    FakeEngine   engine;
    TextureLookup lookup;
    int          a = 0, b = 0, c = 0;   // addresses of three textures

    TextureRig() : lookup(engine, library) {}

    void Put(const void* texture, int32_t index, int32_t serial, const char* path)
    {
        engine.textures[texture] = { { index, serial }, path };
    }
};

static void ATextureIsTheImageOfItsPath()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    const PluginImageId id = rig.lookup.FromTexture(&rig.a);
    CHECK(id != 0);
    CHECK(id == rig.library.Path("/Game/UI/T_A.T_A"));   // the id a plugin asking by path gets
    CHECK(id == rig.library.Path("/Game/UI/T_A"));
    CHECK(id == rig.library.Path("Texture2D'/Game/UI/T_A.T_A'"));
    CHECK(id == rig.lookup.FromTexture(&rig.a));   // from the cache too
    CHECK(rig.library.Canonical(id) == id);
    CHECK(rig.library.State(id) == PLUGIN_IMAGE_NONE);   // an id, not a request
    ImageLibrary::Request request;
    CHECK(!rig.library.TakeNext(request));
}

static void ATextureIsAskedForOnceInTheEngine()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    const PluginImageId first = rig.lookup.FromTexture(&rig.a);
    const PluginImageId second = rig.lookup.FromTexture(&rig.a);
    CHECK(first == second);
    CHECK(rig.engine.pathCalls == 1);
    CHECK(rig.engine.slotCalls == 2);   // the slot is checked every time: the pointer is not trusted
}

static void ASlotUsedByAnotherObjectIsLookedUpAgain()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    const PluginImageId before = rig.lookup.FromTexture(&rig.a);

    // The texture died and its slot went to another one: same index, new serial.
    rig.engine.textures.erase(&rig.a);
    rig.Put(&rig.b, 7, 101, "/Game/UI/T_B.T_B");
    const PluginImageId after = rig.lookup.FromTexture(&rig.b);
    CHECK(after != 0 && after != before);
    CHECK(rig.engine.pathCalls == 2);
    CHECK(after == rig.library.Path("/Game/UI/T_B.T_B"));
}

static void ASlotUsedAgainByTheSameAddressIsLookedUpAgain()
{
    // The address came back for another texture, in the same slot, with a new serial.
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    const PluginImageId before = rig.lookup.FromTexture(&rig.a);
    rig.Put(&rig.a, 7, 101, "/Game/UI/T_B.T_B");
    const PluginImageId after = rig.lookup.FromTexture(&rig.a);
    CHECK(after != before);
    CHECK(rig.engine.pathCalls == 2);
}

static void TwoSlotsWithTheSamePathAreOneImage()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    rig.Put(&rig.b, 9, 200, "/Game/UI/T_A.T_A");
    CHECK(rig.lookup.FromTexture(&rig.a) == rig.lookup.FromTexture(&rig.b));
}

static void ADeadTextureGivesNothing()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    CHECK(rig.lookup.FromTexture(&rig.a) != 0);

    // The slot no longer holds it: the answer from before is not given.
    rig.engine.textures.erase(&rig.a);
    CHECK(rig.lookup.FromTexture(&rig.a) == 0);
    CHECK(rig.lookup.FromTexture(nullptr) == 0);
    CHECK(rig.lookup.FromTexture(&rig.c) == 0);   // never a texture
}

static void ATextureWithNoPathGivesNothingAndIsAskedAgain()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "");
    CHECK(rig.lookup.FromTexture(&rig.a) == 0);
    CHECK(rig.lookup.FromTexture(&rig.a) == 0);
    CHECK(rig.engine.pathCalls == 2);   // nothing was remembered

    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    CHECK(rig.lookup.FromTexture(&rig.a) != 0);
}

static void OtherThreadsGetZeroAndOneLogLine()
{
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Game/UI/T_A.T_A");
    rig.engine.gameThread = false;
    CHECK(rig.lookup.FromTexture(&rig.a) == 0);
    CHECK(rig.lookup.FromTexture(&rig.a) == 0);
    CHECK(rig.engine.logs.size() == 1);
    CHECK(rig.engine.slotCalls == 0 && rig.engine.pathCalls == 0);   // the engine was not touched

    rig.engine.gameThread = true;
    CHECK(rig.lookup.FromTexture(&rig.a) != 0);
    CHECK(rig.engine.logs.size() == 1);
}

static void APlaceholderTextureIsStillAnImageIdForThePumpToEnd()
{
    // FromTexture names the texture; ending it as no image is the pump's rule.
    TextureRig rig;
    rig.Put(&rig.a, 7, 100, "/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
    CHECK(rig.lookup.FromTexture(&rig.a) != 0);
}

static void TheCacheIsBoundedAndStillAnswers()
{
    TextureRig rig;
    std::vector<int> textures(kTextureCacheMax + 10);
    for (size_t i = 0; i < textures.size(); ++i)
        rig.Put(&textures[i], static_cast<int32_t>(i), 1, ("/Game/UI/T_" + std::to_string(i) + ".T_" + std::to_string(i)).c_str());
    for (size_t i = 0; i < textures.size(); ++i)
        CHECK(rig.lookup.FromTexture(&textures[i]) != 0);
    CHECK(rig.engine.pathCalls == static_cast<int>(textures.size()));

    // The oldest were dropped from the memory; they are looked up again, and the id is the same.
    const PluginImageId again = rig.lookup.FromTexture(&textures[0]);
    CHECK(again == rig.library.Path("/Game/UI/T_0.T_0"));
    CHECK(rig.engine.pathCalls == static_cast<int>(textures.size()) + 1);
}

// ---------------------------------------------------------------------------

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    RunTest("placeholders are no image", PlaceholdersAreNoImage);
    RunTest("placeholders are recognised in every form a path comes in", PlaceholdersAreRecognisedInEveryFormAPathComesIn);
    RunTest("other textures are not placeholders", OtherTexturesAreNotPlaceholders);
    RunTest("an item is found by its normalized name", AnItemIsFoundByItsNormalizedName);
    RunTest("an item with no icon is known with no path", AnItemWithNoIconIsKnownWithNoPath);
    RunTest("skipped items are not in the index", SkippedItemsAreNotInTheIndex);
    RunTest("the first item under a name is kept", TheFirstItemUnderANameIsKept);
    RunTest("an item without a usable name is not kept", AnItemWithoutAUsableNameIsNotKept);
    RunTest("an unset name is never found", AnUnsetNameIsNeverFound);
    RunTest("an unset name is not an asset name either", AnUnsetNameIsNotAnAssetNameEither);
    RunTest("an item is found by its asset name, class name and default object name", AnItemIsFoundByItsAssetNameClassNameAndDefaultObjectName);
    RunTest("an item with a unique name is also found by its asset name", AnItemWithAUniqueNameIsAlsoFoundByItsAssetName);
    RunTest("a unique name beats an asset name in either order", AUniqueNameBeatsAnAssetNameInEitherOrder);
    RunTest("an asset name waits for the whole index", AnAssetNameWaitsForTheWholeIndex);
    RunTest("a unique name that looks like a class or default object is found as itself", AUniqueNameThatLooksLikeAClassOrDefaultObjectIsFoundAsItself);
    RunTest("two unnamed items have their own pictures", TwoUnnamedItemsHaveTheirOwnPictures);
    RunTest("the texture paths cover the unnamed items", TheTexturePathsCoverTheUnnamedItems);
    RunTest("the index counts unnamed items and repeated names", TheIndexCountsUnnamedItemsAndRepeatedNames);
    RunTest("the index counts asset names met on more than one item", TheIndexCountsAssetNamesMetOnMoreThanOneItem);
    RunTest("the list is made on the first step only", TheListIsMadeOnTheFirstStepOnly);
    RunTest("an index whose list cannot be made fails once", AnIndexWhoseListCannotBeMadeFailsOnce);
    RunTest("an unavailable read fails the index", AnUnavailableReadFailsTheIndex);
    RunTest("a step stops when the budget is spent", AStepStopsWhenTheBudgetIsSpent);
    RunTest("a step reads at least one item", AStepReadsAtLeastOneItem);
    RunTest("the budget is three milliseconds", TheBudgetIsTheLoadersThreeMilliseconds);
    RunTest("a step knows when the list is done", AStepKnowsWhenTheListIsDone);
    RunTest("the texture paths are sorted once and skip the empty and placeholders", TheTexturePathsAreSortedOnceAndSkipTheEmptyAndPlaceholders);
    RunTest("a texture is the image of its path", ATextureIsTheImageOfItsPath);
    RunTest("a texture is asked for once in the engine", ATextureIsAskedForOnceInTheEngine);
    RunTest("a slot used by another object is looked up again", ASlotUsedByAnotherObjectIsLookedUpAgain);
    RunTest("a slot used again by the same address is looked up again", ASlotUsedAgainByTheSameAddressIsLookedUpAgain);
    RunTest("two slots with the same path are one image", TwoSlotsWithTheSamePathAreOneImage);
    RunTest("a dead texture gives nothing", ADeadTextureGivesNothing);
    RunTest("a texture with no path gives nothing and is asked again", ATextureWithNoPathGivesNothingAndIsAskedAgain);
    RunTest("other threads get zero and one log line", OtherThreadsGetZeroAndOneLogLine);
    RunTest("a placeholder texture is still an image id for the pump to end", APlaceholderTextureIsStillAnImageIdForThePumpToEnd);
    RunTest("the cache is bounded and still answers", TheCacheIsBoundedAndStillAnswers);

    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
