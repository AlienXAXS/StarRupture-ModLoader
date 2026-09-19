# Pak Loading Plugin API (v67, all builds)

`IPluginPak` (`hooks->Pak`) lets a plugin mount pak files -- and the IoStore containers that go
with them -- into the running engine, keep track of what is mounted so nothing is mounted twice,
and load or spawn the assets inside. It is available from interface version 67 onwards and is
non-null on every build; `IsAvailable()` says whether the engine entry points resolved on the
binary you are running in.

---

## Table of Contents

- [What a mod pak has to be](#what-a-mod-pak-has-to-be)
- [Mounting from disk](#mounting-from-disk)
- [Mounting from memory or from your DLL](#mounting-from-memory-or-from-your-dll)
- [Loading and spawning](#loading-and-spawning)
- [The ModActor convention](#the-modactor-convention)
- [Ownership, duplicates and unload](#ownership-duplicates-and-unload)
- [Unmounting](#unmounting)
- [Threading](#threading)
- [Order](#order)
- [The `pak` console command](#the-pak-console-command)
- [How it works underneath](#how-it-works-underneath)
- [Reference](#reference)
- [Full Example Plugin](#full-example-plugin)
- [Common Mistakes](#common-mistakes)

---

## What a mod pak has to be

StarRupture ships its content as **IoStore containers**: `pakchunk0-Windows.pak` is a thin index
and the packages live in `pakchunk0-Windows.utoc` / `.ucas` next to it. In UE5 the package loader
finds packages through the package store, and the package store only knows about IoStore
containers. The consequence for a mod:

- A **`.pak` + `.utoc` + `.ucas` triplet** (same base name, same folder) is a full mod. The loader
  mounts the pak, and the engine opens the container next to it as part of the same call. Assets
  inside load by path.
- A **bare `.pak`** with `.uasset` files inside mounts without complaint and is useless for
  assets: `LoadObject` will not find anything in it. The pak's own index still serves loose files
  (`.ini`, `.txt`, `.json`, anything that is not a package), which is a legitimate use.

Cook with IoStore enabled (the project default for UE5 games), or convert a legacy pak with a
tool such as `retoc`. Package paths inside must be under a mount point the engine knows, which for
mods means `/Game/...` -- `../../../StarRupture/Content/...` on disk inside the pak. The UE4SS
convention of `/Game/Mods/<ModName>/...` is a good one to keep.

Signed and encrypted paks are not supported; the game does not use them either.

---

## Mounting from disk

```cpp
PluginPakHandle g_pak = nullptr;

static bool MountMyPak(IPluginSelf* self)
{
    IPluginPak* pak = self->hooks->Pak;
    if (!pak->IsAvailable())
    {
        self->logger->Warn(self, "Pak mounting unavailable on this build");
        return false;
    }

    PluginPakMountOptions opts{};
    opts.order = PLUGIN_PAK_ORDER_DEFAULT;

    // Relative paths resolve against Plugins\<your plugin>\.
    PluginPakResult r = pak->MountFile(self, "MyMod.pak", &opts, &g_pak);
    if (r < 0)
    {
        self->logger->Error(self, "MountFile: %s", pak->ResultToString(r));
        return false;
    }
    // r is PLUGIN_PAK_OK or PLUGIN_PAK_ALREADY_MOUNTED; both leave g_pak valid.
    return true;
}
```

`PLUGIN_PAK_ALREADY_MOUNTED` is a **success**. It means the engine already had that file --
mounted earlier by you, by another plugin, by an earlier load of your plugin before a reload, or
by the game itself at startup (anything under `Content\Paks\`, including `~mods` and `LogicMods`,
is mounted by the engine before any plugin runs). The handle you get back names the existing
mount, and nothing was mounted a second time. Treat `r < 0` as failure and everything else as
mounted.

`MountFile` from `PluginInit` is the normal case. Mounting from a tick or a console command works
too; see [Threading](#threading) for what changes.

---

## Mounting from memory or from your DLL

The engine can only read a pak through its own file layer -- there is no "mount from a buffer".
`MountMemory` and `MountResource` therefore write the bytes to
`ModLoader\PakCache\<your plugin>\<name>.pak` (plus `.utoc` / `.ucas`) and mount from there.
A hash sidecar records what was written, so the next launch with the same embedded bytes skips
the write. A cached file that is currently mounted is never overwritten -- the engine holds it
open -- and a mount of changed bytes over a live mount logs a warning and returns
`PLUGIN_PAK_ALREADY_MOUNTED` with the old mount; a restart picks the new bytes up.

### From resources

Put the three files in your `.rc` as `RCDATA`:

```rc
MYMOD_PAK  RCDATA "MyMod.pak"
MYMOD_UTOC RCDATA "MyMod.utoc"
MYMOD_UCAS RCDATA "MyMod.ucas"
```

Save your module handle in `DllMain` and mount:

```cpp
static HMODULE g_self = nullptr;
BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) g_self = h; return TRUE; }

PluginPakResult r = pak->MountResource(self, g_self,
                                       "MYMOD_PAK", "MYMOD_UTOC", "MYMOD_UCAS",
                                       "MyMod",      // cache base name; null uses the pak resource name
                                       &opts, &g_pak);
```

Resource names are the string or decimal id the `.rc` used. Pass null for the utoc/ucas names for
a legacy pak (both or neither). A `.ucas` can be hundreds of megabytes; it is resource data, so it
is mapped from your DLL rather than copied, and written to the cache once.

### From memory

```cpp
PluginPakMemoryImage img{};
img.name = "MyMod";
img.pak = pakBytes;   img.pakSize = pakLen;
img.utoc = utocBytes; img.utocSize = utocLen;
img.ucas = ucasBytes; img.ucasSize = ucasLen;
pak->MountMemory(self, &img, &opts, &g_pak);
```

The buffers are only read during the call.

---

## Loading and spawning

```cpp
// A texture, a data asset, a sound -- anything by full object path.
void* tex = pak->LoadObject("/Game/Mods/MyMod/T_Icon.T_Icon");             // UObject*

// A Blueprint class. Note the _C suffix on the generated class.
void* cls = pak->LoadClass("/Game/Mods/MyMod/BP_Turret.BP_Turret_C");       // UClass*

// Spawn it. Either pointer may be null for origin / identity rotation.
PluginDebugVector  at  { 1000.0, 2000.0, 300.0 };
PluginDebugRotator rot { 0.0, 90.0, 0.0 };
void* actor = pak->SpawnActor(cls, &at, &rot);                                // AActor*
```

`LoadObject` and `LoadClass` wrap `StaticLoadObject`, so they return an object that is already
loaded just as readily as one that is not. `SpawnActor` goes through
`UGameplayStatics::BeginDeferredActorSpawnFromClass` / `FinishSpawningActor` with
`AlwaysSpawn` collision handling, in the current world. All three are game-thread operations;
see [Threading](#threading).

The returned pointers are the SDK's `SDK::UObject*` / `SDK::UClass*` / `SDK::AActor*`. Cast them
if you link the SDK; otherwise `hooks->ObjectWalker` and `hooks->ObjectProperties` work on them
as on any other object.

---

## The ModActor convention

UE4SS's BPModLoader gives Blueprint mods a fixed entry point: a class called `ModActor` in the
mod's folder, spawned into every world, with `PreBeginPlay` and `PostBeginPlay` events the mod
implements. `PluginPakMountOptions::modActorClass` does the same thing here:

```cpp
static void OnModActor(PluginPakHandle, void* actor, void* world, void* userData)
{
    // Runs on the game thread, after PostBeginPlay. Keep a reference if you need one.
}

PluginPakMountOptions opts{};
opts.order             = PLUGIN_PAK_ORDER_DEFAULT;
opts.modActorClass     = "/Game/Mods/MyMod/ModActor.ModActor_C";
opts.onModActorSpawned = &OnModActor;
opts.userData          = nullptr;
pak->MountFile(self, "MyMod.pak", &opts, &g_pak);
```

While the pak is mounted, every world that begins play gets one instance of that class, spawned
at the origin. `PreBeginPlay` is called before the actor's `BeginPlay` and `PostBeginPlay` after,
when the class (or a parent) defines them; a class without them just gets spawned. If a world is
already in play when you mount, the actor is spawned immediately as well.

A Blueprint mod built for UE4SS's `LogicMods` folder therefore runs under this loader unchanged:
mount its pak and name its `ModActor_C`. The class must load -- which means the pak must be an
IoStore container; a failure to load is logged once per mount, not per world.

The class name stays with the mount across a reload of your plugin (it is content in the pak,
not code in your DLL). Your callback does not: it is dropped when your plugin unloads and set
again by the options you pass when you mount after reloading.

---

## Ownership, duplicates and unload

Every mount has an owner -- the plugin that made it, or `console`. The registry is keyed on the
normalized absolute path, and it is what turns a second mount of the same file into
`PLUGIN_PAK_ALREADY_MOUNTED` instead of a duplicate. The engine itself would mount the same pak
twice and serve whichever copy sorted first.

**Unloading a plugin does not unmount its paks.** The mount is marked orphaned (`ownerUnloaded`
in `PluginPakInfo`, `[unloaded]` in `pak list`) and stays exactly as it was. The next mount of
that path by anyone -- the same plugin after a reload, usually -- adopts it and gets
`PLUGIN_PAK_ALREADY_MOUNTED`. This is deliberate, and it is the reason the registry exists at
all: unmounting under live objects is unsafe (next section), and a plugin reload is the most
common time a pak would otherwise get mounted twice.

`GetMountedInto` lists everything the engine has mounted, with the loader's bookkeeping where
there is any. Paks the game mounted at startup have no handle and an empty owner.

---

## Unmounting

```cpp
PluginPakResult r = pak->Unmount(self, g_pak);
```

The engine drops the pak from its list, unmounts the IoStore container from the I/O dispatcher
and the package store, and tells the precacher. Files in it stop resolving. What it **cannot** do
is un-load anything already created from its packages. Those objects stay alive with their bulk
data -- texture mips, audio, streamed mesh LODs -- now unreachable, and the next streaming request
into them is undefined behaviour. The loader has no way to know which live objects came from
which container, so it does not try to decide for you.

Unmount when nothing you loaded from the pak is still referenced: between sessions, after your
own actors are destroyed and a garbage collection has run, never with a ModActor still in the
world. If that is hard to guarantee, leave the pak mounted -- a mounted pak nobody reads costs
nothing.

A plugin can only unmount its own mounts (`PLUGIN_PAK_NOT_OWNER` otherwise). Nobody can unmount a
pak the game mounted at startup. The console's `pak unmount` can unmount anything the loader
mounted, as the operator's override.

---

## Threading

Mounting broadcasts `OnPakFileMounted2`, whose engine-side handlers (localization, shader
library) are only ever run on the game thread; loading and spawning are game-thread only outright.
Every `IPluginPak` function therefore runs its work on the game thread:

- From `PluginInit`, the main thread is parked in the loader's init hook and nothing else is
  touching engine state: the call runs inline.
- From a game-thread callback (a world-begin-play hook, a `gameThread` console command, a
  ModActor callback): inline.
- From anywhere else (an ImGui panel callback, your own thread): the request is queued to the
  next engine tick and the call blocks for up to 10 seconds. `PLUGIN_PAK_TIMEOUT` (or a null
  pointer from the asset helpers) means the request is **still queued**, not that it failed; it
  will run, and a later `IsMounted` / `GetMountedInto` shows the result. A loading screen is the
  usual reason for a stall.

Prefer `hooks->Engine->PostToGameThread` and calling from there when you are off the game thread
and cannot afford to block.

---

## Order

Paks are searched highest order first, so order decides who wins when two paks carry the same
file. The engine derives an order from the path when asked -- 3 for a pak under
`Content\Paks`, 0 for anywhere else -- which would put a pak mounted from your plugin folder
*below* every stock pak. `PLUGIN_PAK_ORDER_DEFAULT` therefore resolves to **100**, above every
stock pak, matching the engine's own `_P` patch-pak convention. Pass an explicit value to sit
elsewhere; a mod that only adds new content does not care.

---

## The `pak` console command

Available in both console front-ends (ImGui developer console on the client, `-console` window on
any build). Everything runs on the game thread.

```
pak                              list every mounted pak with order, owner and file count
pak mount <path> [order]         mount a pak (absolute, or relative to Binaries\Win64)
pak unmount <#|path>             unmount a loader-mounted pak (# from the list)
pak load </Game/Path/Asset.Asset>
pak loadclass </Game/Path/BP.BP_C>
pak spawn </Game/Path/BP.BP_C> [x y z]
pak spawnmesh </Game/Path/SM_Thing.SM_Thing> [x y z]
pak widget </Game/Path/WBP_Thing.WBP_Thing_C> [zorder]    (client only)
pak widget close
```

`pak widget` creates a UMG widget class for the local player and adds it to the viewport, with
the mouse cursor shown and game-and-UI input so it can be clicked; `pak widget close` removes it
and restores game-only input. It exists to try debug and QA widgets the developers left in the
content, such as `WBP_Debug_Enviro_QA_Tool`, or a widget from your own pak, without wiring anything
up. Close the mod loader console before interacting with the widget.

`pak load` is the quickest way to find out whether a pak is an IoStore container: a bare pak
mounts fine and then loads nothing.

`pak spawn` and `pak spawnmesh` place the result three metres in front of the local player,
facing the same way, unless coordinates are given; on a dedicated server, which has no local
player, they use the world origin. `spawnmesh` takes a static or skeletal mesh asset rather than
an actor class and spawns an engine `StaticMeshActor` / `SkeletalMeshActor` to carry it, which is
the quickest way to eyeball a mesh from a freshly mounted pak without authoring a Blueprint for it.

---

## How it works underneath

The engine has a runtime mount path of its own, used by chunk downloaders and game feature
plugins: `FCoreDelegates::MountPak`, bound to `FPakPlatformFile::HandleMountPakDelegate`. That
function takes a path and an order, calls `FPakPlatformFile::Mount`, and returns the `IPakFile*`.
`Mount` also opens the `.utoc` next to the pak (it changes the extension and looks) and registers
the container with the I/O dispatcher and the package store, which is what makes the packages
loadable. `HandleUnmountPakDelegate` is the mirror. The loader resolves both by pattern and calls
them on the `FPakPlatformFile` instance found through `FPlatformFileManager::FindPlatformFile`,
exactly as the engine's own code does.

Nothing about this is a hook: no engine function is detoured, so a pattern that stops matching
after a game update costs `IsAvailable()` and nothing else. All three patterns are the same in the
client and dedicated-server binaries, and are optional at the loader's preflight.

The loader's list of mounted paks is read from `FPakPlatformFile::PakFiles` under the engine's
own lock, so `pak list` shows what the engine has, not what the loader remembers.

---

## Reference

```cpp
typedef void* PluginPakHandle;

enum PluginPakResult : int
{
    PLUGIN_PAK_OK                 =  0,   // mounted / unmounted by this call
    PLUGIN_PAK_ALREADY_MOUNTED    =  1,   // success: existing mount returned
    PLUGIN_PAK_UNAVAILABLE        = -1,   // IsAvailable() is false
    PLUGIN_PAK_INVALID_ARGUMENT   = -2,
    PLUGIN_PAK_FILE_NOT_FOUND     = -3,
    PLUGIN_PAK_ENGINE_REFUSED     = -4,   // FPakPlatformFile::Mount failed; LogPakFile says why
    PLUGIN_PAK_CACHE_WRITE_FAILED = -5,   // MountMemory/MountResource could not write the cache
    PLUGIN_PAK_RESOURCE_NOT_FOUND = -6,
    PLUGIN_PAK_NOT_MOUNTED        = -7,   // Unmount: dead handle
    PLUGIN_PAK_NOT_OWNER          = -8,   // Unmount: not yours, or a startup pak
    PLUGIN_PAK_TIMEOUT            = -9,   // still queued for the game thread
    PLUGIN_PAK_FAULT              = -10,  // engine call raised; details in the log
};

#define PLUGIN_PAK_ORDER_DEFAULT (-1)   // resolves to 100

typedef void (*PluginPakModActorCallback)(PluginPakHandle pak, void* actor, void* world, void* userData);

struct PluginPakMountOptions
{
    int                       order;
    const char*               modActorClass;       // optional
    PluginPakModActorCallback onModActorSpawned;   // optional
    void*                     userData;
};

struct PluginPakMemoryImage
{
    const char* name;
    const void* pak;  size_t pakSize;
    const void* utoc; size_t utocSize;   // optional, with ucas
    const void* ucas; size_t ucasSize;
};

struct PluginPakInfo
{
    PluginPakHandle handle;          // null for a pak the loader did not mount
    char pakPath[512];
    char mountPoint[256];
    char owner[64];                  // "" when the game mounted it
    int  order;
    int  pakchunkIndex;
    int  numFiles;                   // pak index entries; IoStore packages not counted
    bool ownedByYou;
    bool ownerUnloaded;
};

struct IPluginPak
{
    bool            (*IsAvailable)();
    PluginPakResult (*MountFile)(const IPluginSelf* self, const char* pakPath,
                                 const PluginPakMountOptions* options, PluginPakHandle* outHandle);
    PluginPakResult (*MountMemory)(const IPluginSelf* self, const PluginPakMemoryImage* image,
                                   const PluginPakMountOptions* options, PluginPakHandle* outHandle);
    PluginPakResult (*MountResource)(const IPluginSelf* self, void* module,
                                     const char* pakResource, const char* utocResource, const char* ucasResource,
                                     const char* cacheName, const PluginPakMountOptions* options,
                                     PluginPakHandle* outHandle);
    PluginPakResult (*Unmount)(const IPluginSelf* self, PluginPakHandle handle);
    bool            (*IsMounted)(const char* pakPath);
    int             (*GetMountedInto)(const IPluginSelf* self, PluginPakInfo* out, int maxOut);
    void*           (*LoadObject)(const char* objectPath);
    void*           (*LoadClass)(const char* classPath);
    void*           (*SpawnActor)(void* actorClass, const PluginDebugVector* location, const PluginDebugRotator* rotation);
    const char*     (*ResultToString)(PluginPakResult result);
};
```

---

## Full Example Plugin

A plugin that ships a Blueprint mod inside its own DLL and spawns its ModActor in every world.

```cpp
#include "plugin_interface.h"
#include <windows.h>

static HMODULE         g_module = nullptr;
static IPluginSelf*    g_self   = nullptr;
static PluginPakHandle g_pak    = nullptr;

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) g_module = h;
    return TRUE;
}

static void OnModActor(PluginPakHandle, void* actor, void*, void*)
{
    g_self->logger->Info(g_self, "ModActor spawned at %p", actor);
}

extern "C" __declspec(dllexport) PluginInfo* GetPluginInfo()
{
    static PluginInfo info{ "MyBlueprintMod", "1.0.0", "me", "A Blueprint mod in a DLL",
                            PLUGIN_INTERFACE_VERSION, PLUGIN_TARGET_CLIENT };
    return &info;
}

extern "C" __declspec(dllexport) bool PluginInit(IPluginSelf* self)
{
    g_self = self;
    IPluginPak* pak = self->hooks->Pak;
    if (!pak->IsAvailable())
    {
        self->logger->Error(self, "pak mounting unavailable");
        return false;
    }

    PluginPakMountOptions opts{};
    opts.order             = PLUGIN_PAK_ORDER_DEFAULT;
    opts.modActorClass     = "/Game/Mods/MyBlueprintMod/ModActor.ModActor_C";
    opts.onModActorSpawned = &OnModActor;

    PluginPakResult r = pak->MountResource(self, g_module,
                                           "MOD_PAK", "MOD_UTOC", "MOD_UCAS", "MyBlueprintMod",
                                           &opts, &g_pak);
    if (r < 0)
    {
        self->logger->Error(self, "mount failed: %s", pak->ResultToString(r));
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) void PluginShutdown()
{
    // Deliberately no Unmount: the ModActor may be in the world, and the pak
    // is adopted again on reload.
    g_self = nullptr;
}
```

---

## Common Mistakes

- **A bare `.pak` and nothing loads.** The game is IoStore; packages must be in a `.utoc`/`.ucas`
  next to the pak. `pak load` on the console tells you in one line.
- **Treating `PLUGIN_PAK_ALREADY_MOUNTED` as an error.** It is the normal result on a plugin
  reload and whenever the pak lives under `Content\Paks`. Check `r < 0`.
- **Unmounting in `PluginShutdown`.** Your actors are probably still in the world. Leave the pak
  mounted; a reload adopts it.
- **Calling from an ImGui callback and blocking.** That is the render thread; the call queues and
  waits. Post to the game thread instead if the wait matters.
- **Passing a stack `PluginPakMountOptions` and expecting later edits to apply.** The loader copies
  the options at mount time.
- **Forgetting `_C`** on a Blueprint class path. `/Game/Mods/X/BP_Y.BP_Y` is the Blueprint asset;
  `/Game/Mods/X/BP_Y.BP_Y_C` is the class you spawn.
