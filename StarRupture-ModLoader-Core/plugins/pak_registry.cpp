#include "pch.h"
#include "pak_registry.h"

#include "plugins/plugin_interface.h"
#include "logging/logger.h"
#include "hooks/game/pak_mount/pak_mount.h"
#include "hooks/game/world_begin_play/world_begin_play.h"
#include "hooks/game/scan_patterns.h"
#include "memory_scanner/scanner.h"
#include "utils/game_thread_dispatch.h"
#include "core/startup_utils.h"

#include "Engine_classes.hpp"

#include <windows.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace PakRegistry
{
    // -----------------------------------------------------------------------
    // Constants
    // -----------------------------------------------------------------------

    // What PLUGIN_PAK_ORDER_DEFAULT resolves to. Above every stock pak (the
    // engine gives Content/Paks entries 3, and a "_P" patch pak +100), so a
    // mod pak wins a file collision by default, which is what a mod expects.
    constexpr int kDefaultOrder = 100;

    // How long a caller off the game thread waits for its request to run. A
    // loading screen can hold ticks for a while; past this the request is
    // still queued and the caller is told so.
    constexpr DWORD kGameThreadWaitMs = 10000;

    // -----------------------------------------------------------------------
    // Small helpers
    // -----------------------------------------------------------------------
    static std::wstring Utf8ToWide(const char* s)
    {
        if (!s || !*s) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        if (n <= 0) return std::wstring();
        std::wstring out(static_cast<size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
        return out;
    }

    static std::string WideToUtf8(const std::wstring& s)
    {
        if (s.empty()) return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string out(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
        return out;
    }

    static void CopyTo(char* dst, size_t cap, const std::string& src)
    {
        if (!cap) return;
        const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
        memcpy(dst, src.data(), n);
        dst[n] = '\0';
    }

    // Case-folded normalized path: the registry key.
    static std::wstring KeyOf(const std::wstring& normalizedPath)
    {
        std::wstring k = normalizedPath;
        for (wchar_t& c : k) c = static_cast<wchar_t>(towlower(c));
        return k;
    }

    static bool FileExistsW(const std::wstring& path)
    {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    static uint64_t FileSizeW(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
            return 0;
        return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }

    static uint64_t Fnv1a64(const void* data, size_t size)
    {
        uint64_t h = 1469598103934665603ULL;
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
        return h;
    }

    static const char* OwnerName(const IPluginSelf* self)
    {
        return (self && self->name && self->name[0]) ? self->name : "console";
    }

    // A callback address that no longer belongs to a loaded module cannot be
    // called, whatever the bookkeeping says. Same check as the keybind and
    // console registries, for the same crash class.
    static bool CallbackStillMapped(const void* fn)
    {
        if (!fn) return false;
        HMODULE owner = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCSTR>(fn), &owner))
            return false;
        return owner != nullptr;
    }

    // -----------------------------------------------------------------------
    // Game-thread execution
    //
    // Work is wrapped in a shared job so a request that times out still has
    // somewhere valid to write its result when it eventually runs; the caller
    // has long since returned by then.
    // -----------------------------------------------------------------------
    static bool RunOnGameThread(std::function<void()> fn, DWORD timeoutMs)
    {
        if (GameThreadDispatch::IsGameThread() || !GameThreadDispatch::HasTicked())
        {
            fn();
            return true;
        }

        std::future<std::string> fut = GameThreadDispatch::PostString([fn = std::move(fn)]() mutable
        {
            fn();
            return std::string();
        });
        return fut.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::ready;
    }

    // -----------------------------------------------------------------------
    // The registry
    // -----------------------------------------------------------------------
    struct Entry
    {
        bool         live = false;
        std::string  owner;
        bool         ownerUnloaded = false;
        bool         mountedByLoader = false;   // false: the engine had it before we looked (startup pak)
        std::wstring path;                      // normalized, as passed to the engine
        std::wstring key;
        std::wstring mountPoint;
        int          order = 0;
        int          pakchunkIndex = -1;
        int          numFiles = 0;
        bool         fromCache = false;

        std::string               modActorClass;
        PluginPakModActorCallback onModActor = nullptr;
        void*                     userData   = nullptr;
        std::string               modActorOwner;   // whose callback that is
        bool                      modActorWarned = false;

        // The world-begin-play generation this entry last spawned its
        // ModActor in, so a remount (a plugin reload, typically) does not put
        // a second one into a world that already has it.
        uint64_t                  modActorGeneration = 0;
    };

    // Recursive: a mod actor's BeginPlay can call back into a plugin that
    // calls IsMounted from inside it.
    static std::recursive_mutex   s_mutex;
    static std::vector<Entry>     s_entries;
    static bool                   s_initialized = false;
    static SDK::UWorld*           s_lastWorldBegun = nullptr;
    static uint64_t               s_worldGeneration = 0;   // bumped on every world begin play

    static PluginPakHandle SlotToHandle(size_t slot) { return reinterpret_cast<PluginPakHandle>(slot + 1); }
    static int HandleToSlot(PluginPakHandle h)
    {
        const uintptr_t raw = reinterpret_cast<uintptr_t>(h);
        if (raw == 0 || raw > s_entries.size()) return -1;
        return static_cast<int>(raw - 1);
    }

    static int FindByKeyLocked(const std::wstring& key)
    {
        for (size_t i = 0; i < s_entries.size(); ++i)
            if (s_entries[i].live && s_entries[i].key == key)
                return static_cast<int>(i);
        return -1;
    }

    static void ApplyOptionsLocked(Entry& e, const char* owner, const PluginPakMountOptions* options)
    {
        if (!options) return;
        if (options->modActorClass && options->modActorClass[0])
        {
            e.modActorClass  = options->modActorClass;
            e.modActorWarned = false;
        }
        e.onModActor    = options->onModActorSpawned;
        e.userData      = options->userData;
        e.modActorOwner = owner;
    }

    static void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName);

    static void EnsureInit()
    {
        {
            std::lock_guard<std::recursive_mutex> lk(s_mutex);
            if (s_initialized) return;
            s_initialized = true;
        }
        // Outside the registry lock: the hook has a lock of its own that it
        // may hold while dispatching into OnAnyWorldBeginPlay, which takes
        // ours. Never take those two in the other order.
        Hooks::PakMount::Resolve();
        Hooks::WorldBeginPlay::RegisterAnyWorldCallback(&OnAnyWorldBeginPlay);
    }

    // -----------------------------------------------------------------------
    // Asset loading and spawning (game thread)
    // -----------------------------------------------------------------------
    using StaticLoadObject_t = SDK::UObject* (__fastcall*)(SDK::UClass* objectClass, SDK::UObject* outer,
                                                           const wchar_t* name, const wchar_t* filename,
                                                           uint32_t loadFlags, void* sandbox,
                                                           bool allowObjectReconciliation, const void* instancingContext);

    static StaticLoadObject_t GetStaticLoadObject()
    {
        static StaticLoadObject_t s_fn = nullptr;
        static bool s_scanned = false;
        if (!s_scanned)
        {
            s_scanned = true;
            s_fn = reinterpret_cast<StaticLoadObject_t>(
                Scanner::FindPatternInMainModule("StaticLoadObject", ScanPatterns::StaticLoadObject));
        }
        return s_fn;
    }

    static bool LoadObjectSEH(StaticLoadObject_t fn, SDK::UClass* cls, const wchar_t* path,
                              SDK::UObject** out, unsigned long* code)
    {
        __try
        {
            *out = fn(cls, nullptr, path, nullptr, 0, nullptr, false, nullptr);
            return true;
        }
        __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Game thread only.
    static SDK::UObject* LoadObjectNow(const wchar_t* path, bool asClass)
    {
        StaticLoadObject_t fn = GetStaticLoadObject();
        if (!fn)
        {
            ModLoaderLogger::LogError(L"[Pak] StaticLoadObject is unresolved -- cannot load %s", path);
            return nullptr;
        }

        SDK::UClass* cls = asClass ? SDK::UClass::StaticClass() : SDK::UObject::StaticClass();
        SDK::UObject* obj = nullptr;
        unsigned long code = 0;
        if (!LoadObjectSEH(fn, cls, path, &obj, &code))
        {
            ModLoaderLogger::LogError(L"[Pak] Exception 0x%08lX inside StaticLoadObject for %s", code, path);
            return nullptr;
        }
        if (!obj)
            ModLoaderLogger::LogDebug(L"[Pak] StaticLoadObject found nothing at %s", path);
        else
            ModLoaderLogger::LogDebug(L"[Pak] Loaded %s -> %S", path, obj->GetFullName().c_str());
        return obj;
    }

    // Walks the class chain for a parameterless UFunction by name. Written
    // out rather than using the SDK's GetFunction(ClassName, FuncName) because
    // the ModActor events may be declared on a parent Blueprint class.
    static SDK::UFunction* FindFunction(SDK::UClass* cls, const char* name)
    {
        for (SDK::UStruct* s = cls; s; s = s->SuperStruct)
        {
            for (SDK::UField* f = s->Children; f; f = f->Next)
            {
                if (!f->IsA(SDK::EClassCastFlags::Function))
                    continue;
                if (f->GetName() == name)
                    return static_cast<SDK::UFunction*>(f);
            }
        }
        return nullptr;
    }

    static bool CallNoArgSEH(SDK::UObject* obj, SDK::UFunction* fn, unsigned long* code)
    {
        __try
        {
            obj->ProcessEvent(fn, nullptr);
            return true;
        }
        __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void CallNoArgFunction(SDK::AActor* actor, const char* name)
    {
        SDK::UFunction* fn = FindFunction(actor->Class, name);
        if (!fn) return;
        unsigned long code = 0;
        if (!CallNoArgSEH(actor, fn, &code))
            ModLoaderLogger::LogError(L"[Pak] Exception 0x%08lX inside %S on %S", code, name, actor->GetFullName().c_str());
    }

    static SDK::FTransform MakeTransform(const PluginDebugVector* location, const PluginDebugRotator* rotation)
    {
        SDK::FTransform xf{};
        xf.Translation.X = location ? location->x : 0.0;
        xf.Translation.Y = location ? location->y : 0.0;
        xf.Translation.Z = location ? location->z : 0.0;
        xf.Scale3D.X = xf.Scale3D.Y = xf.Scale3D.Z = 1.0;

        // FRotator::Quaternion, degrees in, UE axis conventions.
        const double d2 = 3.14159265358979323846 / 360.0;
        const double p = rotation ? rotation->pitch * d2 : 0.0;
        const double y = rotation ? rotation->yaw   * d2 : 0.0;
        const double r = rotation ? rotation->roll  * d2 : 0.0;
        const double sp = std::sin(p), cp = std::cos(p);
        const double sy = std::sin(y), cy = std::cos(y);
        const double sr = std::sin(r), cr = std::cos(r);
        xf.Rotation.X =  cr * sp * sy - sr * cp * cy;
        xf.Rotation.Y = -cr * sp * cy - sr * cp * sy;
        xf.Rotation.Z =  cr * cp * sy - sr * sp * cy;
        xf.Rotation.W =  cr * cp * cy + sr * sp * sy;
        return xf;
    }

    static bool SpawnSEH(SDK::UClass* cls, SDK::UWorld* world, const SDK::FTransform* xf, bool modActorEvents,
                         SDK::AActor** out, unsigned long* code)
    {
        __try
        {
            SDK::AActor* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
                world, SDK::TSubclassOf<SDK::AActor>(cls), *xf,
                SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr,
                SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
            if (!actor)
            {
                *out = nullptr;
                return true;
            }
            if (modActorEvents)
                CallNoArgFunction(actor, "PreBeginPlay");
            actor = SDK::UGameplayStatics::FinishSpawningActor(actor, *xf, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
            if (actor && modActorEvents)
                CallNoArgFunction(actor, "PostBeginPlay");
            *out = actor;
            return true;
        }
        __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Game thread only.
    static SDK::AActor* SpawnActorNow(SDK::UClass* cls, SDK::UWorld* world, const SDK::FTransform& xf, bool modActorEvents)
    {
        if (!cls || !world) return nullptr;
        SDK::AActor* actor = nullptr;
        unsigned long code = 0;
        if (!SpawnSEH(cls, world, &xf, modActorEvents, &actor, &code))
        {
            ModLoaderLogger::LogError(L"[Pak] Exception 0x%08lX spawning %S", code, cls->GetFullName().c_str());
            return nullptr;
        }
        return actor;
    }

    // -----------------------------------------------------------------------
    // ModActor spawning
    // -----------------------------------------------------------------------
    struct ModActorJob
    {
        int                       slot;
        uint64_t                  generation;
        std::string               owner;
        std::wstring              pakPath;
        std::string               modActorClass;
        PluginPakModActorCallback callback;
        void*                     userData;
        bool                      warned;
    };

    static void SpawnModActors(SDK::UWorld* world, const char* worldName, int onlySlot)
    {
        std::vector<ModActorJob> jobs;
        {
            std::lock_guard<std::recursive_mutex> lk(s_mutex);
            for (size_t i = 0; i < s_entries.size(); ++i)
            {
                Entry& e = s_entries[i];
                if (!e.live || e.modActorClass.empty()) continue;
                if (onlySlot >= 0 && static_cast<int>(i) != onlySlot) continue;
                if (e.modActorGeneration == s_worldGeneration) continue;   // already in this world
                e.modActorGeneration = s_worldGeneration;
                jobs.push_back({ static_cast<int>(i), s_worldGeneration, e.owner, e.path, e.modActorClass, e.onModActor, e.userData, e.modActorWarned });
            }
        }

        for (ModActorJob& job : jobs)
        {
            const std::wstring wcls = Utf8ToWide(job.modActorClass.c_str());
            SDK::UClass* cls = static_cast<SDK::UClass*>(LoadObjectNow(wcls.c_str(), true));
            if (!cls)
            {
                if (!job.warned)
                {
                    ModLoaderLogger::LogWarn(L"[Pak] ModActor class %s (pak %s, owner %S) did not load -- check the path and that the pak is an IoStore container",
                                             wcls.c_str(), job.pakPath.c_str(), job.owner.c_str());
                    std::lock_guard<std::recursive_mutex> lk(s_mutex);
                    if (job.slot < static_cast<int>(s_entries.size()))
                        s_entries[static_cast<size_t>(job.slot)].modActorWarned = true;
                }
                continue;
            }

            SDK::AActor* actor = SpawnActorNow(cls, world, MakeTransform(nullptr, nullptr), true);
            if (!actor)
            {
                ModLoaderLogger::LogWarn(L"[Pak] ModActor %s failed to spawn in %S", wcls.c_str(), worldName ? worldName : "?");
                continue;
            }
            ModLoaderLogger::LogInfo(L"[Pak] Spawned ModActor %S in %S (pak %s)",
                                     actor->GetFullName().c_str(), worldName ? worldName : "?", job.pakPath.c_str());

            if (job.callback)
            {
                if (!CallbackStillMapped(reinterpret_cast<const void*>(job.callback)))
                {
                    ModLoaderLogger::LogError(L"[Pak] ModActor callback for %s points into an unloaded module -- not called", job.pakPath.c_str());
                    continue;
                }
                try
                {
                    job.callback(SlotToHandle(static_cast<size_t>(job.slot)), actor, world, job.userData);
                }
                catch (...)
                {
                    ModLoaderLogger::LogError(L"[Pak] ModActor callback for %s threw", job.pakPath.c_str());
                }
            }
        }
    }

    static void OnAnyWorldBeginPlay(SDK::UWorld* world, const char* worldName)
    {
        {
            std::lock_guard<std::recursive_mutex> lk(s_mutex);
            s_lastWorldBegun = world;
            ++s_worldGeneration;
        }
        SpawnModActors(world, worldName, -1);
    }

    // Called on the game thread right after a mount. If the world that last
    // began play is still the current one, a mounted-mid-session pak gets its
    // ModActor now rather than at the next map load.
    static void SpawnModActorIfWorldLive(int slot)
    {
        SDK::UWorld* current = SDK::UWorld::GetWorld();
        SDK::UWorld* last = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lk(s_mutex);
            last = s_lastWorldBegun;
        }
        if (current && current == last)
            SpawnModActors(current, "current world", slot);
    }

    // -----------------------------------------------------------------------
    // Mount / unmount (game thread)
    // -----------------------------------------------------------------------
    static PluginPakResult MountNow(const char* owner, const std::wstring& normalizedPath,
                                    const PluginPakMountOptions* options, bool fromCache,
                                    PluginPakHandle* outHandle)
    {
        if (!Hooks::PakMount::IsAvailable())
            return PLUGIN_PAK_UNAVAILABLE;

        if (!FileExistsW(normalizedPath))
        {
            ModLoaderLogger::LogWarn(L"[Pak] %S asked to mount %s, which does not exist", owner, normalizedPath.c_str());
            return PLUGIN_PAK_FILE_NOT_FOUND;
        }

        const std::wstring key = KeyOf(normalizedPath);
        // Any negative value is "default": the engine reads order as unsigned,
        // and -1 in particular means "derive from the path", which puts a pak
        // outside Content/Paks below every stock one.
        const int order = (options && options->order >= 0) ? options->order : kDefaultOrder;

        int spawnSlot = -1;
        PluginPakResult result = PLUGIN_PAK_OK;
        {
            std::lock_guard<std::recursive_mutex> lk(s_mutex);

            // Already tracked: hand back the record, adopting it if orphaned.
            int slot = FindByKeyLocked(key);
            if (slot >= 0)
            {
                Entry& e = s_entries[static_cast<size_t>(slot)];
                if (e.ownerUnloaded)
                {
                    ModLoaderLogger::LogInfo(L"[Pak] %S adopts %s (mounted earlier by %S, since unloaded)",
                                             owner, e.path.c_str(), e.owner.c_str());
                    e.owner = owner;
                    e.ownerUnloaded = false;
                }
                else
                {
                    ModLoaderLogger::LogInfo(L"[Pak] %S asked to mount %s, already mounted by %S -- returning the existing mount",
                                             owner, e.path.c_str(), e.owner.c_str());
                }
                ApplyOptionsLocked(e, owner, options);
                if (outHandle) *outHandle = SlotToHandle(static_cast<size_t>(slot));
                spawnSlot = slot;
                result = PLUGIN_PAK_ALREADY_MOUNTED;
            }
            else
            {
                // Not ours: does the engine already have it (a ~mods or
                // LogicMods pak it mounted at startup)? Then track it without
                // mounting it a second time.
                Hooks::PakMount::MountedPak existing;
                if (Hooks::PakMount::FindMounted(normalizedPath, &existing))
                {
                    Entry e;
                    e.live = true;
                    e.owner = owner;
                    e.mountedByLoader = false;
                    e.path = Hooks::PakMount::NormalizePath(existing.pakFilename);
                    e.key = key;
                    e.mountPoint = existing.mountPoint;
                    e.order = existing.readOrder;
                    e.pakchunkIndex = existing.pakchunkIndex;
                    e.numFiles = existing.numFiles;
                    ApplyOptionsLocked(e, owner, options);
                    s_entries.push_back(std::move(e));
                    slot = static_cast<int>(s_entries.size() - 1);
                    ModLoaderLogger::LogInfo(L"[Pak] %S asked to mount %s, which the engine mounted at startup (order %d) -- tracking it, not mounting again",
                                             owner, normalizedPath.c_str(), existing.readOrder);
                    if (outHandle) *outHandle = SlotToHandle(static_cast<size_t>(slot));
                    spawnSlot = slot;
                    result = PLUGIN_PAK_ALREADY_MOUNTED;
                }
                else
                {
                    Hooks::PakMount::MountedPak info;
                    const Hooks::PakMount::Status st = Hooks::PakMount::Mount(normalizedPath, order, &info);
                    switch (st)
                    {
                    case Hooks::PakMount::Status::Ok: break;
                    case Hooks::PakMount::Status::Unavailable: return PLUGIN_PAK_UNAVAILABLE;
                    case Hooks::PakMount::Status::Refused:     return PLUGIN_PAK_ENGINE_REFUSED;
                    case Hooks::PakMount::Status::Fault:       return PLUGIN_PAK_FAULT;
                    }

                    Entry e;
                    e.live = true;
                    e.owner = owner;
                    e.mountedByLoader = true;
                    e.path = normalizedPath;
                    e.key = key;
                    e.mountPoint = info.mountPoint;
                    e.order = order;
                    e.pakchunkIndex = info.pakchunkIndex;
                    e.numFiles = info.numFiles;
                    e.fromCache = fromCache;
                    ApplyOptionsLocked(e, owner, options);
                    s_entries.push_back(std::move(e));
                    slot = static_cast<int>(s_entries.size() - 1);

                    ModLoaderLogger::LogInfo(L"[Pak] %S mounted %s (order %d, mount point %s, %d indexed file(s))",
                                             owner, normalizedPath.c_str(), order, info.mountPoint.c_str(), info.numFiles);
                    if (outHandle) *outHandle = SlotToHandle(static_cast<size_t>(slot));
                    spawnSlot = slot;
                    result = PLUGIN_PAK_OK;
                }
            }
        }

        // Outside the registry lock: this runs BeginPlay, which runs plugin code.
        if (spawnSlot >= 0 && options && options->modActorClass && options->modActorClass[0])
            SpawnModActorIfWorldLive(spawnSlot);

        return result;
    }

    static PluginPakResult UnmountNow(const IPluginSelf* self, PluginPakHandle handle)
    {
        const char* who = OwnerName(self);
        std::wstring path;
        {
            std::lock_guard<std::recursive_mutex> lk(s_mutex);
            const int slot = HandleToSlot(handle);
            if (slot < 0 || !s_entries[static_cast<size_t>(slot)].live)
                return PLUGIN_PAK_NOT_MOUNTED;

            Entry& e = s_entries[static_cast<size_t>(slot)];
            if (!e.mountedByLoader)
            {
                ModLoaderLogger::LogWarn(L"[Pak] %S asked to unmount %s, which the engine mounted at startup -- refused",
                                         who, e.path.c_str());
                return PLUGIN_PAK_NOT_OWNER;
            }
            // A plugin may only unmount its own; the console (no self) is the
            // operator's override for anything the loader mounted.
            if (self && _stricmp(e.owner.c_str(), who) != 0)
            {
                ModLoaderLogger::LogWarn(L"[Pak] %S asked to unmount %s, which belongs to %S -- refused",
                                         who, e.path.c_str(), e.owner.c_str());
                return PLUGIN_PAK_NOT_OWNER;
            }
            path = e.path;

            const Hooks::PakMount::Status st = Hooks::PakMount::Unmount(path);
            switch (st)
            {
            case Hooks::PakMount::Status::Ok: break;
            case Hooks::PakMount::Status::Unavailable: return PLUGIN_PAK_UNAVAILABLE;
            case Hooks::PakMount::Status::Refused:     return PLUGIN_PAK_ENGINE_REFUSED;
            case Hooks::PakMount::Status::Fault:       return PLUGIN_PAK_FAULT;
            }
            e.live = false;
        }
        ModLoaderLogger::LogInfo(L"[Pak] %S unmounted %s", who, path.c_str());
        return PLUGIN_PAK_OK;
    }

    // -----------------------------------------------------------------------
    // The pak cache (memory / resource mounts)
    // -----------------------------------------------------------------------
    static std::wstring SanitizeName(const std::string& s)
    {
        std::wstring w = Utf8ToWide(s.c_str());
        for (wchar_t& c : w)
        {
            if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
                c == L'<' || c == L'>' || c == L'|' || c < 32)
                c = L'_';
        }
        if (w.empty()) w = L"pak";
        return w;
    }

    static std::wstring CacheDirFor(const char* owner)
    {
        std::wstring root = GetModLoaderDirPath(L"PakCache\\");
        CreateDirectoryW(root.c_str(), nullptr);
        std::wstring dir = root + SanitizeName(owner) + L"\\";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }

    static bool WriteFileAtomic(const std::wstring& path, const void* data, size_t size)
    {
        const std::wstring tmp = path + L".tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            ModLoaderLogger::LogError(L"[Pak] Cannot create %s (error %lu)", tmp.c_str(), GetLastError());
            return false;
        }
        const auto* p = static_cast<const uint8_t*>(data);
        size_t left = size;
        while (left > 0)
        {
            const DWORD chunk = left > (64u << 20) ? (64u << 20) : static_cast<DWORD>(left);
            DWORD written = 0;
            if (!WriteFile(h, p, chunk, &written, nullptr) || written != chunk)
            {
                ModLoaderLogger::LogError(L"[Pak] Write to %s failed (error %lu)", tmp.c_str(), GetLastError());
                CloseHandle(h);
                DeleteFileW(tmp.c_str());
                return false;
            }
            p += chunk;
            left -= chunk;
        }
        CloseHandle(h);
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            ModLoaderLogger::LogError(L"[Pak] Cannot replace %s (error %lu)", path.c_str(), GetLastError());
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    }

    static std::string ReadSmallFile(const std::wstring& path)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::string();
        char buf[512]{};
        DWORD n = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
        CloseHandle(h);
        return std::string(buf, n);
    }

    // The three files of one image and the sidecar that says what they hold.
    struct CacheSet
    {
        std::wstring base;      // dir + name, no extension
        std::wstring pak, utoc, ucas, sidecar;
        std::string  expected;  // sidecar content for this image
    };

    static CacheSet DescribeCache(const char* owner, const PluginPakMemoryImage& img)
    {
        CacheSet c;
        c.base    = CacheDirFor(owner) + SanitizeName(img.name);
        c.pak     = c.base + L".pak";
        c.utoc    = c.base + L".utoc";
        c.ucas    = c.base + L".ucas";
        c.sidecar = c.base + L".pakhash";

        char line[256];
        snprintf(line, sizeof(line), "pak=%016llx:%llu\nutoc=%016llx:%llu\nucas=%016llx:%llu\n",
                 static_cast<unsigned long long>(Fnv1a64(img.pak, img.pakSize)),   static_cast<unsigned long long>(img.pakSize),
                 static_cast<unsigned long long>(img.utoc ? Fnv1a64(img.utoc, img.utocSize) : 0), static_cast<unsigned long long>(img.utocSize),
                 static_cast<unsigned long long>(img.ucas ? Fnv1a64(img.ucas, img.ucasSize) : 0), static_cast<unsigned long long>(img.ucasSize));
        c.expected = line;
        return c;
    }

    static bool CacheIsCurrent(const CacheSet& c, const PluginPakMemoryImage& img)
    {
        if (ReadSmallFile(c.sidecar) != c.expected) return false;
        if (!FileExistsW(c.pak) || FileSizeW(c.pak) != img.pakSize) return false;
        if (img.utoc)
        {
            if (!FileExistsW(c.utoc) || FileSizeW(c.utoc) != img.utocSize) return false;
            if (!FileExistsW(c.ucas) || FileSizeW(c.ucas) != img.ucasSize) return false;
        }
        return true;
    }

    static bool WriteCache(const CacheSet& c, const PluginPakMemoryImage& img)
    {
        // Sidecar first goes stale, then files, then the sidecar is rewritten:
        // a crash mid-way leaves a mismatch, which is treated as "rewrite".
        DeleteFileW(c.sidecar.c_str());
        if (!WriteFileAtomic(c.pak, img.pak, img.pakSize)) return false;
        if (img.utoc)
        {
            if (!WriteFileAtomic(c.utoc, img.utoc, img.utocSize)) return false;
            if (!WriteFileAtomic(c.ucas, img.ucas, img.ucasSize)) return false;
        }
        else
        {
            // A legacy image replacing an earlier IoStore one must not leave
            // a stale container next to the pak: the engine would open it.
            DeleteFileW(c.utoc.c_str());
            DeleteFileW(c.ucas.c_str());
        }
        return WriteFileAtomic(c.sidecar, c.expected.data(), c.expected.size());
    }

    // Game thread. Prepares the cache and mounts from it.
    static PluginPakResult MountImageNow(const char* owner, const PluginPakMemoryImage& img,
                                         const PluginPakMountOptions* options, PluginPakHandle* outHandle)
    {
        const CacheSet c = DescribeCache(owner, img);
        const std::wstring normalized = Hooks::PakMount::NormalizePath(c.pak);

        if (!CacheIsCurrent(c, img))
        {
            bool mounted = false;
            {
                std::lock_guard<std::recursive_mutex> lk(s_mutex);
                mounted = FindByKeyLocked(KeyOf(normalized)) >= 0;
            }
            if (mounted)
            {
                // The engine holds the file open; the bytes on disk are what
                // is being served and will be until the next launch.
                ModLoaderLogger::LogWarn(L"[Pak] %S's embedded pak '%S' differs from the mounted cache copy %s -- the running game keeps the old one; restart to pick up the new build",
                                         owner, img.name, normalized.c_str());
                return MountNow(owner, normalized, options, true, outHandle);
            }

            ModLoaderLogger::LogInfo(L"[Pak] Writing %S's embedded pak '%S' to %s (%llu bytes%s)",
                                     owner, img.name, c.pak.c_str(), static_cast<unsigned long long>(img.pakSize),
                                     img.utoc ? L" + IoStore container" : L"");
            if (!WriteCache(c, img))
                return PLUGIN_PAK_CACHE_WRITE_FAILED;
        }
        else
        {
            ModLoaderLogger::LogDebug(L"[Pak] Cache for %S's '%S' is current at %s", owner, img.name, c.pak.c_str());
        }

        return MountNow(owner, normalized, options, true, outHandle);
    }

    // -----------------------------------------------------------------------
    // Resources
    // -----------------------------------------------------------------------
    static bool FindRcData(HMODULE module, const char* name, const void** outData, size_t* outSize)
    {
        *outData = nullptr;
        *outSize = 0;
        if (!name || !*name) return false;

        LPCSTR id = name;
        bool numeric = true;
        for (const char* p = name; *p; ++p)
            if (*p < '0' || *p > '9') { numeric = false; break; }
        if (numeric)
            id = MAKEINTRESOURCEA(atoi(name));

        HRSRC res = FindResourceA(module, id, MAKEINTRESOURCEA(10) /* RT_RCDATA, narrow */);
        if (!res) return false;
        HGLOBAL h = LoadResource(module, res);
        if (!h) return false;
        const void* data = LockResource(h);
        const DWORD size = SizeofResource(module, res);
        if (!data || !size) return false;
        *outData = data;
        *outSize = size;
        return true;
    }

    // -----------------------------------------------------------------------
    // Plugin-facing entry points
    // -----------------------------------------------------------------------
    static bool Api_IsAvailable()
    {
        EnsureInit();
        return Hooks::PakMount::IsAvailable();
    }

    static std::wstring ResolvePluginRelative(const IPluginSelf* self, const std::wstring& path)
    {
        const bool absolute = path.size() >= 2 && (path[1] == L':' || (path[0] == L'\\' && path[1] == L'\\') || (path[0] == L'/' && path[1] == L'/'));
        if (absolute || !self || !self->name)
            return Hooks::PakMount::NormalizePath(path);
        return Hooks::PakMount::NormalizePath(GetExeDirPath(L"Plugins\\") + Utf8ToWide(self->name) + L"\\" + path);
    }

    struct MountJob
    {
        std::string           owner;
        std::wstring          path;
        PluginPakMountOptions options{};
        bool                  hasOptions = false;
        std::string           modActorClassCopy;
        PluginPakHandle       handle = nullptr;
        PluginPakResult       result = PLUGIN_PAK_TIMEOUT;
    };

    // Options are copied: the plugin's struct may live on its stack, and a
    // timed-out request runs after the caller returned.
    template <class Job>
    static void CopyOptions(Job& job, const PluginPakMountOptions* options)
    {
        if (!options) return;
        job.hasOptions = true;
        job.options = *options;
        if (options->modActorClass)
        {
            job.modActorClassCopy = options->modActorClass;
            job.options.modActorClass = job.modActorClassCopy.c_str();
        }
    }

    static PluginPakResult Api_MountFile(const IPluginSelf* self, const char* pakPath,
                                         const PluginPakMountOptions* options, PluginPakHandle* outHandle)
    {
        EnsureInit();
        if (!pakPath || !*pakPath) return PLUGIN_PAK_INVALID_ARGUMENT;

        auto job = std::make_shared<MountJob>();
        job->owner = OwnerName(self);
        job->path  = ResolvePluginRelative(self, Utf8ToWide(pakPath));
        CopyOptions(*job, options);

        const bool ran = RunOnGameThread([job]()
        {
            job->result = MountNow(job->owner.c_str(), job->path, job->hasOptions ? &job->options : nullptr, false, &job->handle);
        }, kGameThreadWaitMs);

        if (!ran)
        {
            ModLoaderLogger::LogWarn(L"[Pak] %S's mount of %s is still queued for the game thread after %lu ms",
                                     job->owner.c_str(), job->path.c_str(), kGameThreadWaitMs);
            return PLUGIN_PAK_TIMEOUT;
        }
        if (outHandle) *outHandle = job->handle;
        return job->result;
    }

    struct ImageJob
    {
        std::string           owner;
        std::vector<uint8_t>  pak, utoc, ucas;
        std::string           name;
        PluginPakMountOptions options{};
        bool                  hasOptions = false;
        std::string           modActorClassCopy;
        PluginPakHandle       handle = nullptr;
        PluginPakResult       result = PLUGIN_PAK_TIMEOUT;
    };

    static PluginPakResult MountImage(const IPluginSelf* self, const PluginPakMemoryImage* image,
                                      const PluginPakMountOptions* options, PluginPakHandle* outHandle)
    {
        if (!image || !image->name || !image->name[0] || !image->pak || !image->pakSize)
            return PLUGIN_PAK_INVALID_ARGUMENT;
        if ((image->utoc && !image->ucas) || (!image->utoc && image->ucas))
            return PLUGIN_PAK_INVALID_ARGUMENT;

        // Inline when possible: no copy of what may be a multi-hundred-MB
        // container. Only a cross-thread request has to own its bytes until
        // it runs.
        if (GameThreadDispatch::IsGameThread() || !GameThreadDispatch::HasTicked())
        {
            MountJob tmp;
            CopyOptions(tmp, options);
            return MountImageNow(OwnerName(self), *image, tmp.hasOptions ? &tmp.options : nullptr, outHandle);
        }

        auto job = std::make_shared<ImageJob>();
        job->owner = OwnerName(self);
        job->name  = image->name;
        job->pak.assign(static_cast<const uint8_t*>(image->pak), static_cast<const uint8_t*>(image->pak) + image->pakSize);
        if (image->utoc)
        {
            job->utoc.assign(static_cast<const uint8_t*>(image->utoc), static_cast<const uint8_t*>(image->utoc) + image->utocSize);
            job->ucas.assign(static_cast<const uint8_t*>(image->ucas), static_cast<const uint8_t*>(image->ucas) + image->ucasSize);
        }
        CopyOptions(*job, options);

        const bool ran = RunOnGameThread([job]()
        {
            PluginPakMemoryImage img{};
            img.name    = job->name.c_str();
            img.pak     = job->pak.data();
            img.pakSize = job->pak.size();
            if (!job->utoc.empty())
            {
                img.utoc     = job->utoc.data();
                img.utocSize = job->utoc.size();
                img.ucas     = job->ucas.data();
                img.ucasSize = job->ucas.size();
            }
            job->result = MountImageNow(job->owner.c_str(), img, job->hasOptions ? &job->options : nullptr, &job->handle);
        }, kGameThreadWaitMs);

        if (!ran)
        {
            ModLoaderLogger::LogWarn(L"[Pak] %S's mount of embedded pak '%S' is still queued for the game thread after %lu ms",
                                     job->owner.c_str(), job->name.c_str(), kGameThreadWaitMs);
            return PLUGIN_PAK_TIMEOUT;
        }
        if (outHandle) *outHandle = job->handle;
        return job->result;
    }

    static PluginPakResult Api_MountMemory(const IPluginSelf* self, const PluginPakMemoryImage* image,
                                           const PluginPakMountOptions* options, PluginPakHandle* outHandle)
    {
        EnsureInit();
        return MountImage(self, image, options, outHandle);
    }

    static PluginPakResult Api_MountResource(const IPluginSelf* self, void* module,
                                             const char* pakResource, const char* utocResource, const char* ucasResource,
                                             const char* cacheName, const PluginPakMountOptions* options,
                                             PluginPakHandle* outHandle)
    {
        EnsureInit();
        if (!module || !pakResource || !*pakResource) return PLUGIN_PAK_INVALID_ARGUMENT;
        if ((utocResource && !ucasResource) || (!utocResource && ucasResource)) return PLUGIN_PAK_INVALID_ARGUMENT;

        HMODULE hmod = static_cast<HMODULE>(module);
        PluginPakMemoryImage img{};
        img.name = (cacheName && *cacheName) ? cacheName : pakResource;

        if (!FindRcData(hmod, pakResource, &img.pak, &img.pakSize))
        {
            ModLoaderLogger::LogWarn(L"[Pak] %S: RCDATA resource '%S' not found in module 0x%llX",
                                     OwnerName(self), pakResource, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(module)));
            return PLUGIN_PAK_RESOURCE_NOT_FOUND;
        }
        if (utocResource)
        {
            if (!FindRcData(hmod, utocResource, &img.utoc, &img.utocSize) ||
                !FindRcData(hmod, ucasResource, &img.ucas, &img.ucasSize))
            {
                ModLoaderLogger::LogWarn(L"[Pak] %S: RCDATA resource '%S' or '%S' not found in module 0x%llX",
                                         OwnerName(self), utocResource, ucasResource,
                                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(module)));
                return PLUGIN_PAK_RESOURCE_NOT_FOUND;
            }
        }
        return MountImage(self, &img, options, outHandle);
    }

    static PluginPakResult Api_Unmount(const IPluginSelf* self, PluginPakHandle handle)
    {
        EnsureInit();
        struct Job { const IPluginSelf* self; PluginPakHandle handle; PluginPakResult result; };
        auto job = std::make_shared<Job>(Job{ self, handle, PLUGIN_PAK_TIMEOUT });
        if (!RunOnGameThread([job]() { job->result = UnmountNow(job->self, job->handle); }, kGameThreadWaitMs))
            return PLUGIN_PAK_TIMEOUT;
        return job->result;
    }

    static bool Api_IsMounted(const char* pakPath)
    {
        EnsureInit();
        if (!pakPath || !*pakPath) return false;
        const std::wstring normalized = Hooks::PakMount::NormalizePath(Utf8ToWide(pakPath));
        {
            std::lock_guard<std::recursive_mutex> lk(s_mutex);
            if (FindByKeyLocked(KeyOf(normalized)) >= 0)
                return true;
        }
        return Hooks::PakMount::FindMounted(normalized, nullptr);
    }

    static int Api_GetMountedInto(const IPluginSelf* self, PluginPakInfo* out, int maxOut)
    {
        EnsureInit();
        std::vector<Hooks::PakMount::MountedPak> all;
        Hooks::PakMount::EnumerateMounted(all);

        const char* me = self ? OwnerName(self) : nullptr;
        int written = 0;
        std::lock_guard<std::recursive_mutex> lk(s_mutex);
        for (const Hooks::PakMount::MountedPak& p : all)
        {
            if (out && written < maxOut)
            {
                PluginPakInfo& info = out[written];
                memset(&info, 0, sizeof(info));
                const std::wstring normalized = Hooks::PakMount::NormalizePath(p.pakFilename);
                CopyTo(info.pakPath, sizeof(info.pakPath), WideToUtf8(normalized));
                CopyTo(info.mountPoint, sizeof(info.mountPoint), WideToUtf8(p.mountPoint));
                info.order = p.readOrder;
                info.pakchunkIndex = p.pakchunkIndex;
                info.numFiles = p.numFiles;

                const int slot = FindByKeyLocked(KeyOf(normalized));
                if (slot >= 0)
                {
                    const Entry& e = s_entries[static_cast<size_t>(slot)];
                    info.handle = SlotToHandle(static_cast<size_t>(slot));
                    CopyTo(info.owner, sizeof(info.owner), e.owner);
                    info.ownerUnloaded = e.ownerUnloaded;
                    info.ownedByYou = me && !e.ownerUnloaded && _stricmp(e.owner.c_str(), me) == 0;
                }
            }
            ++written;
        }
        return static_cast<int>(all.size());
    }

    static void* LoadViaGameThread(const char* path, bool asClass)
    {
        EnsureInit();
        if (!path || !*path) return nullptr;
        struct Job { std::wstring path; bool asClass; SDK::UObject* result; };
        auto job = std::make_shared<Job>(Job{ Utf8ToWide(path), asClass, nullptr });
        if (!RunOnGameThread([job]() { job->result = LoadObjectNow(job->path.c_str(), job->asClass); }, kGameThreadWaitMs))
        {
            ModLoaderLogger::LogWarn(L"[Pak] Load of %s is still queued for the game thread after %lu ms -- returning null", job->path.c_str(), kGameThreadWaitMs);
            return nullptr;
        }
        return job->result;
    }

    static void* Api_LoadObject(const char* objectPath) { return LoadViaGameThread(objectPath, false); }
    static void* Api_LoadClass(const char* classPath)   { return LoadViaGameThread(classPath, true); }

    static void* Api_SpawnActor(void* actorClass, const PluginDebugVector* location, const PluginDebugRotator* rotation)
    {
        EnsureInit();
        if (!actorClass) return nullptr;
        struct Job { SDK::UClass* cls; SDK::FTransform xf; SDK::AActor* result; };
        auto job = std::make_shared<Job>(Job{ static_cast<SDK::UClass*>(actorClass), MakeTransform(location, rotation), nullptr });
        if (!RunOnGameThread([job]()
        {
            SDK::UWorld* world = SDK::UWorld::GetWorld();
            if (!world)
            {
                ModLoaderLogger::LogWarn(L"[Pak] SpawnActor: no world");
                return;
            }
            job->result = SpawnActorNow(job->cls, world, job->xf, false);
        }, kGameThreadWaitMs))
        {
            ModLoaderLogger::LogWarn(L"[Pak] SpawnActor is still queued for the game thread after %lu ms -- returning null", kGameThreadWaitMs);
            return nullptr;
        }
        return job->result;
    }

    static const char* Api_ResultToString(PluginPakResult r)
    {
        switch (r)
        {
        case PLUGIN_PAK_OK:                 return "ok";
        case PLUGIN_PAK_ALREADY_MOUNTED:    return "already mounted";
        case PLUGIN_PAK_UNAVAILABLE:        return "pak mounting unavailable on this build";
        case PLUGIN_PAK_INVALID_ARGUMENT:   return "invalid argument";
        case PLUGIN_PAK_FILE_NOT_FOUND:     return "file not found";
        case PLUGIN_PAK_ENGINE_REFUSED:     return "engine refused the pak";
        case PLUGIN_PAK_CACHE_WRITE_FAILED: return "could not write the pak cache";
        case PLUGIN_PAK_RESOURCE_NOT_FOUND: return "resource not found in module";
        case PLUGIN_PAK_NOT_MOUNTED:        return "not a live mount";
        case PLUGIN_PAK_NOT_OWNER:          return "not the owner of that mount";
        case PLUGIN_PAK_TIMEOUT:            return "still queued for the game thread";
        case PLUGIN_PAK_FAULT:              return "engine call faulted";
        }
        return "unknown";
    }

    static IPluginPak g_interface =
    {
        &Api_IsAvailable,
        &Api_MountFile,
        &Api_MountMemory,
        &Api_MountResource,
        &Api_Unmount,
        &Api_IsMounted,
        &Api_GetMountedInto,
        &Api_LoadObject,
        &Api_LoadClass,
        &Api_SpawnActor,
        &Api_ResultToString,
    };

    IPluginPak* GetInterface()
    {
        EnsureInit();
        return &g_interface;
    }

    void ForgetPlugin(const char* pluginName)
    {
        if (!pluginName || !*pluginName) return;
        std::lock_guard<std::recursive_mutex> lk(s_mutex);
        for (Entry& e : s_entries)
        {
            if (!e.live) continue;

            // The callback is an address in the module about to go; the
            // mount and its ModActor class are content, and stay.
            if (_stricmp(e.modActorOwner.c_str(), pluginName) == 0)
            {
                e.onModActor    = nullptr;
                e.userData      = nullptr;
            }
            if (_stricmp(e.owner.c_str(), pluginName) == 0 && !e.ownerUnloaded)
            {
                e.ownerUnloaded = true;
                ModLoaderLogger::LogInfo(L"[Pak] %S unloaded; %s stays mounted (orphaned) -- a later mount of the same path adopts it",
                                         pluginName, e.path.c_str());
            }
        }
    }
}
