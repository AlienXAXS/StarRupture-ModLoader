#include "layout_check.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <utility>

namespace LayoutCheck
{
    namespace
    {
        std::string Format(const char* format, ...)
        {
            char buffer[512];
            va_list args;
            va_start(args, format);
            const int written = vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return written < 0 ? std::string() : std::string(buffer);
        }

        const LiveProperty* FindProperty(const LiveStruct& live, const char* name)
        {
            for (const LiveProperty& property : live.properties)
            {
                if (property.name == name)
                    return &property;
            }
            return nullptr;
        }

        // Dumper-7 appends _<n> to a member whose name collides with a C++
        // keyword or a name it reserves (a parameter called Class becomes
        // Class_0), so the table can hold a name the game does not reflect.
        // The member under the name without the suffix stands in for it only
        // where it holds the same bytes: same offset, same known size.
        const LiveProperty* FindRenamedProperty(const LiveStruct& live, const TableRow& row)
        {
            const std::string name = row.name;
            const size_t underscore = name.rfind('_');
            if (row.size == 0 || underscore == std::string::npos || underscore == 0 || underscore + 1 == name.size()
                || name.find_first_not_of("0123456789", underscore + 1) != std::string::npos)
                return nullptr;

            const LiveProperty* property = FindProperty(live, name.substr(0, underscore).c_str());
            if (!property || property->offset != static_cast<int32_t>(row.offset)
                || property->size != static_cast<int32_t>(row.size))
                return nullptr;
            return property;
        }

        void Record(Result& result, const char* className, const char* member, const char* field,
            uint32_t sdk, int32_t game, std::string line)
        {
            result.changes.push_back(Change{ className, member, field, sdk, game });
            result.lines.push_back(std::move(line));
        }
    }

    Result Compare(const std::vector<TableClass>& table,
        const std::function<const LiveStruct*(const TableClass&)>& find,
        const PerBuild& perBuild)
    {
        Result result;
        result.classes = static_cast<int>(table.size());

        for (const TableClass& sdk : table)
        {
            // A Blueprint class is only in memory once its asset loads, so its
            // absence at startup says nothing.
            if (sdk.kind == Kind::Blueprint || sdk.kind == Kind::Unknown)
            {
                ++result.deferred;
                continue;
            }

            const LiveStruct* live = find(sdk);
            if (!live)
            {
                ++result.missing;
                result.changedClasses.push_back(sdk.name);
                Record(result, sdk.name, "", kChangeMissing, 0, 0,
                    Format("%s (%s): not found in the game", sdk.name, sdk.path));
                continue;
            }

            ++result.checked;
            const size_t linesBefore = result.lines.size();

            // A size of 0 is not compared: at granularity 1 the generator
            // writes it for a UClass the sources reach only through pointers.
            if (sdk.sizeOf != 0 && live->size != static_cast<int32_t>(sdk.sizeOf))
                Record(result, sdk.name, "", "size", sdk.sizeOf, live->size,
                    Format("%s: size SDK 0x%X, game 0x%X", sdk.name, sdk.sizeOf, live->size));
            // A native UFunction's MinAlignment is 1 whatever its parameters
            // hold, so only classes and structs have an alignment to compare.
            if (sdk.kind != Kind::FunctionParams && sdk.alignOf != 0
                && live->minAlignment != static_cast<int32_t>(sdk.alignOf))
                Record(result, sdk.name, "", "alignment", sdk.alignOf, live->minAlignment,
                    Format("%s: alignment SDK %u, game %d", sdk.name, sdk.alignOf, live->minAlignment));

            for (const TableRow& row : sdk.rows)
            {
                if (perBuild && perBuild(sdk, row))
                {
                    ++result.perBuild;
                    continue;
                }
                const LiveProperty* property = FindProperty(*live, row.name);
                if (!property)
                    property = FindRenamedProperty(*live, row);
                if (!property)
                {
                    Record(result, sdk.name, row.name, kChangeMissing, 0, 0,
                        Format("%s::%s: not found in the game", sdk.name, row.name));
                    continue;
                }

                if (property->offset != static_cast<int32_t>(row.offset))
                    Record(result, sdk.name, row.name, "offset", row.offset, property->offset,
                        Format("%s::%s: offset SDK 0x%X, game 0x%X", sdk.name, row.name, row.offset, property->offset));
                if (row.size != 0 && property->size != static_cast<int32_t>(row.size))
                    Record(result, sdk.name, row.name, "size", row.size, property->size,
                        Format("%s::%s: size SDK 0x%X, game 0x%X", sdk.name, row.name, row.size, property->size));

                if (row.boolMask != 0 && !property->isBool)
                    Record(result, sdk.name, row.name, "isBool", 1, 0,
                        Format("%s::%s: a bool in the SDK, not in the game", sdk.name, row.name));
                else if (row.boolMask != 0 && property->fieldMask != row.boolMask)
                    Record(result, sdk.name, row.name, "boolMask", row.boolMask, static_cast<int32_t>(property->fieldMask),
                        Format("%s::%s: bool mask SDK 0x%02X, game 0x%02X", sdk.name, row.name, row.boolMask, property->fieldMask));
                else if (row.boolMask == 0 && property->isBool)
                    Record(result, sdk.name, row.name, "isBool", 0, 1,
                        Format("%s::%s: a bool in the game, not in the SDK", sdk.name, row.name));
            }

            if (result.lines.size() != linesBefore)
            {
                ++result.mismatched;
                result.changedClasses.push_back(sdk.name);
            }
        }

        return result;
    }

    bool CheckProcessEventSlot(Result& result, const uintptr_t* vtable, int32_t index,
        uintptr_t resolved, uintptr_t imageBase)
    {
        const uintptr_t slot = vtable[index];
        if (slot == resolved)
            return true;

        ++result.slotMismatched;
        const uint32_t expectedRva = static_cast<uint32_t>(resolved - imageBase);
        const int32_t  slotRva     = static_cast<int32_t>(slot - imageBase);
        Record(result, "UObject", "ProcessEvent", kChangeVtableSlot, expectedRva, slotRva,
            Format("UObject::ProcessEvent: vtable slot 0x%X holds base+0x%X, the function is at base+0x%X",
                static_cast<unsigned>(index), static_cast<uint32_t>(slotRva), expectedRva));
        return false;
    }

    Verdict Decide(const std::vector<TableClass>& table, const Result& result)
    {
        Verdict verdict;
        // A slot mismatch names no class, so a changed class elsewhere must
        // not hide it: every plugin calls ProcessEvent through that slot.
        if (result.slotMismatched > 0)
            verdict.safeOff = true;
        if (!result.lines.empty() && result.changedClasses.empty())
            verdict.safeOff = true;

        for (const std::string& name : result.changedClasses)
        {
            const auto sdk = std::find_if(table.begin(), table.end(),
                [&name](const TableClass& c) { return name == c.name; });
            verdict.changed.push_back(name);
            verdict.changedPaths.push_back(sdk == table.end() ? std::string() : std::string(sdk->path));
            if (sdk == table.end() || sdk->consumers.empty())
            {
                verdict.safeOff = true;
                continue;
            }
            for (const std::string& id : sdk->consumers)
            {
                if (id == kConsumerKeys)
                    continue;
                if (id == kConsumerUiMenus)
                {
                    if (std::find(verdict.featuresOff.begin(), verdict.featuresOff.end(), id) == verdict.featuresOff.end())
                        verdict.featuresOff.push_back(id);
                    continue;
                }
                // kConsumerCore, kConsumerPluginSdk, and any id this loader
                // does not know.
                verdict.safeOff = true;
            }
        }
        return verdict;
    }
}

// The reflection reader below is in both game builds. Run, which compares
// against the client SDK's layout table, is in the client build only.
#if defined(MODLOADER_CLIENT_BUILD) || defined(MODLOADER_SERVER_BUILD)

#include "sdk_globals.h"
#include "../hooks/game/engine_init/engine_init.h"
#include "../hooks/game/game_instance_init/game_instance_init.h"
#include "../hooks/game/object_properties/object_properties.h"
#include "../logging/logger.h"
#include "../memory_scanner/version_map.h"
#include "CoreUObject_classes.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace LayoutCheck
{
    namespace
    {
        // Everything the walk needs and produces. Keys are table paths:
        // Module.Name for a class or struct, Module.Class.Function for a
        // function's parameters.
        struct Search
        {
            std::unordered_map<std::string, SDK::UStruct*> structs;                    // seeded with every wanted path
            std::unordered_map<std::string, std::vector<std::string>> functionOwners;  // class path -> function paths in it
            std::unordered_map<std::string, LiveStruct> live;
            int32_t objects = 0;
            int32_t types = 0;      // classes and structs looked up by name
            double  walkMs = 0.0;
        };

        // "/Script/Chimera" -> "Chimera"; anything that is not a native
        // package -> empty.
        const std::string& ModuleOf(const SDK::UObject* package,
            std::unordered_map<const SDK::UObject*, std::string>& cache)
        {
            auto it = cache.find(package);
            if (it == cache.end())
            {
                constexpr char kScript[] = "/Script/";
                std::string name = package->Name.GetRawString();
                it = cache.emplace(package, name.rfind(kScript, 0) == 0 ? name.substr(sizeof(kScript) - 1) : std::string()).first;
            }
            return it->second;
        }

        LiveProperty Describe(const SDK::FProperty* property)
        {
            const bool isBool = property->ClassPrivate
                && (property->ClassPrivate->CastFlags & static_cast<uint64_t>(SDK::EClassCastFlags::BoolProperty)) != 0;
            return LiveProperty{
                property->Name.GetRawString(),
                property->Offset,
                property->ElementSize * property->ArrayDim,
                isBool,
                isBool ? static_cast<const SDK::FBoolProperty*>(property)->FieldMask : 0u };
        }

        LiveStruct Snapshot(const SDK::UStruct* type)
        {
            LiveStruct live{ type->Size, type->MinAlignment, {} };
            for (const SDK::FField* field = type->ChildProperties; field; field = field->Next)
                live.properties.push_back(Describe(static_cast<const SDK::FProperty*>(field)));
            return live;
        }

        // Every native class and script struct, by short name and by
        // Module.Name, as GObjects indices, for field requests: the objects
        // themselves are looked up again, and checked, on each use.
        struct TypeIndex
        {
            std::unordered_map<std::string, std::vector<int32_t>> byName;
            std::unordered_map<std::string, int32_t> byPath;
            int32_t objects = -1;   // GObjects count when built; -1 = not built
        };

        std::mutex g_typesLock;
        TypeIndex  g_types;

        // One pass over GObjects: every native class and script struct that
        // sits directly in a /Script/ package is named Module.Name, looked up,
        // and (with an index) indexed; nothing else has its name decoded.
        // Function parameter blocks are then found in their owning class's
        // Children.
        void Collect(Search& search, TypeIndex* index)
        {
            std::unordered_map<const SDK::UObject*, std::string> modules;

            const auto walkStart = std::chrono::steady_clock::now();
            SDK::TUObjectArray* objects = SDK::UObject::GObjects.GetTypedPtr();
            search.objects = objects->Num();
            if (index)
                *index = TypeIndex{ {}, {}, search.objects };
            for (int32_t i = 0; i < search.objects; ++i)
            {
                SDK::UObject* object = objects->GetByIndex(i);
                // The SDK's operator& on these flags means "has all bits", so a
                // combined mask would match nothing: test each flag on its own.
                if (!object || !object->Class
                    || !(object->HasTypeFlag(SDK::EClassCastFlags::Class)
                         || object->HasTypeFlag(SDK::EClassCastFlags::ScriptStruct)))
                    continue;

                const SDK::UObject* package = object->Outer;
                if (!package || package->Outer)
                    continue;

                const std::string& module = ModuleOf(package, modules);
                if (module.empty())
                    continue;

                ++search.types;
                std::string name = object->Name.GetRawString();
                std::string path = module + "." + name;
                auto it = search.structs.find(path);
                if (it != search.structs.end() && !it->second)
                    it->second = static_cast<SDK::UStruct*>(object);
                if (index)
                {
                    index->byPath.emplace(std::move(path), i);
                    index->byName[std::move(name)].push_back(i);
                }
            }
            search.walkMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - walkStart).count();

            for (const auto& [classPath, functionPaths] : search.functionOwners)
            {
                const SDK::UStruct* owner = search.structs[classPath];
                if (!owner)
                    continue;
                for (SDK::UField* child = owner->Children; child; child = child->Next)
                {
                    if (!child->HasTypeFlag(SDK::EClassCastFlags::Function))
                        continue;
                    auto it = search.structs.find(classPath + "." + child->Name.GetRawString());
                    if (it != search.structs.end() && !it->second)
                        it->second = static_cast<SDK::UStruct*>(child);
                }
            }

            for (const auto& [path, type] : search.structs)
            {
                if (type)
                    search.live.emplace(path, Snapshot(type));
            }
        }
    }
}

#endif

#if defined(MODLOADER_CLIENT_BUILD)

#include "layout_table.generated.h"

static_assert(SrLayoutTable::kFormatVersion == 1, "layout_check.cpp reads layout table format 1");
static_assert(SrLayoutTable::kDetail == 1, "an embedded layout table needs member sizes and bool masks (generate it with --sdk)");

namespace LayoutCheck
{
    namespace
    {
        // Collect under SEH: reflection data that is not what the SDK says it
        // is ends the check with a log line instead of taking the game down.
        bool CollectGuarded(Search* search, TypeIndex* index, DWORD* exceptionCode)
        {
            __try
            {
                Collect(*search, index);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                *exceptionCode = GetExceptionCode();
                return false;
            }
        }

        // The vtable of the UObject class default object, or null. Under SEH
        // for the same reason as Collect.
        const uintptr_t* ObjectVtable(DWORD* exceptionCode)
        {
            __try
            {
                const SDK::UObject* cdo = SDK::UObject::GetDefaultObj();
                return cdo ? *reinterpret_cast<const uintptr_t* const*>(cdo) : nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                *exceptionCode = GetExceptionCode();
                return nullptr;
            }
        }

        // Every plugin's SDK calls ProcessEvent through a compiled vtable
        // index, so a game whose UObject has other virtuals ahead of it would
        // send each call to another function. Run is on the init thread with
        // the game thread parked (see Run), and the object read is the class
        // default object, which lives as long as the class does; the vtable
        // pointer is read once and nothing is kept.
        void CheckProcessEventIndex(Result& result)
        {
            const uintptr_t resolved = Hooks::GameInstanceInit::ProcessEventAddress();
            if (!resolved)
            {
                ModLoaderLogger::LogWarn(L"[LayoutCheck] UObject::ProcessEvent slot not checked: the loader has no address for the function");
                return;
            }
            DWORD exceptionCode = 0;
            const uintptr_t* vtable = ObjectVtable(&exceptionCode);
            if (!vtable)
            {
                ModLoaderLogger::LogWarn(L"[LayoutCheck] UObject::ProcessEvent slot not checked: no UObject default object (exception 0x%08lX)",
                    exceptionCode);
                return;
            }

            const auto imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
            constexpr int32_t index = SDK::Offsets::ProcessEventIdx;
            if (CheckProcessEventSlot(result, vtable, index, resolved, imageBase))
                ModLoaderLogger::LogInfo(L"[LayoutCheck] UObject::ProcessEvent: vtable slot 0x%X holds the function the map resolved (base+0x%llX)",
                    static_cast<unsigned>(index), static_cast<unsigned long long>(resolved - imageBase));
        }
    }

    bool Run(Result& out)
    {
        const auto start = std::chrono::steady_clock::now();

        if (!SdkGlobals::VerifyGObjects())
        {
            ModLoaderLogger::LogError(L"[LayoutCheck] Skipped: GObjects does not look valid");
            return false;
        }

        const std::vector<TableClass> table = ReadTable(SrLayoutTable::kNames, SrLayoutTable::kClasses, SrLayoutTable::kRows,
            SrLayoutTable::kConsumers, SrLayoutTable::kClassConsumers);

        Search search;
        for (const TableClass& sdk : table)
        {
            if (sdk.kind != Kind::Class && sdk.kind != Kind::ScriptStruct && sdk.kind != Kind::FunctionParams)
                continue;
            search.structs.emplace(sdk.path, nullptr);
            if (sdk.kind == Kind::FunctionParams)
            {
                const std::string path = sdk.path;
                const std::string classPath = path.substr(0, path.rfind('.'));
                search.structs.emplace(classPath, nullptr);
                search.functionOwners[classPath].push_back(path);
            }
        }

        // The same pass indexes every type for field requests (LiveFieldOf).
        TypeIndex index;
        DWORD exceptionCode = 0;
        if (!CollectGuarded(&search, &index, &exceptionCode))
        {
            ModLoaderLogger::LogError(L"[LayoutCheck] Skipped: exception 0x%08lX while reading reflection data", exceptionCode);
            return false;
        }
        ModLoaderLogger::LogInfo(L"[Map] Field index: %zu types from %d objects, in the layout check's GObjects pass",
            index.byPath.size(), index.objects);
        {
            std::lock_guard<std::mutex> lock(g_typesLock);
            g_types = std::move(index);
        }

        Result result = Compare(table, [&search](const TableClass& sdk) -> const LiveStruct*
        {
            const auto it = search.live.find(sdk.path);
            return it == search.live.end() ? nullptr : &it->second;
        }, [](const TableClass& sdk, const TableRow& row)
        {
            return SdkGlobals::IsPerBuildField(sdk.name, row.name);
        });

        CheckProcessEventIndex(result);

        for (const std::string& line : result.lines)
            ModLoaderLogger::LogWarn(L"[LayoutCheck] %S", line.c_str());

        const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        void (*const log)(const wchar_t*, ...) =
            FoundChanges(result) ? ModLoaderLogger::LogWarn : ModLoaderLogger::LogInfo;
        log(
            L"[LayoutCheck] SDK CL %u against the game: %d classes, %d checked, %d mismatched, %d missing, %d deferred,"
            L" %d rows resolved per build -- GObjects pass %.1f ms (%d objects, %d types), %.1f ms in all",
            SrLayoutTable::kBuiltCL, result.classes, result.checked, result.mismatched, result.missing, result.deferred,
            result.perBuild,
            search.walkMs, search.objects, search.types, totalMs);

        result.verdict = Decide(table, result);
        out = std::move(result);
        return true;
    }
}

#endif

#if defined(MODLOADER_CLIENT_BUILD) || defined(MODLOADER_SERVER_BUILD)

namespace LayoutCheck
{
    namespace
    {
        // The layout check's pass builds the index (Run). This builds it on
        // its own only when that pass did not run, or when types registered
        // since leave a request unanswered.
        void BuildTypeIndex(TypeIndex& index)
        {
            Search search;
            Collect(search, &index);
            ModLoaderLogger::LogInfo(L"[Map] Field index: %zu types from %d objects in %.1f ms (a pass of its own)",
                index.byPath.size(), index.objects, search.walkMs);
        }

        // The object still at `i`, if it is still the type `name`.
        const SDK::UStruct* TypeAt(int32_t i, const std::string& name)
        {
            SDK::UObject* object = SDK::UObject::GObjects.GetTypedPtr()->GetByIndex(i);
            if (!object || !(object->HasTypeFlag(SDK::EClassCastFlags::Class)
                             || object->HasTypeFlag(SDK::EClassCastFlags::ScriptStruct))
                || object->Name.GetRawString() != name)
                return nullptr;
            return static_cast<const SDK::UStruct*>(object);
        }

        // className is a Dumper-7 name, "ACrX", or "/Script/Chimera.ACrX".
        // The game's own name has no prefix letter (CrX); the letter says
        // which kind it is (F: a script struct, A or U: a class), which
        // settles a class and a struct that share a name.
        VersionMap::LiveField FindType(const TypeIndex& index, const std::string& className,
            const SDK::UStruct*& type)
        {
            VersionMap::LiveField answer;
            type = nullptr;
            constexpr char kScript[] = "/Script/";
            const bool byPath = className.rfind(kScript, 0) == 0;
            const size_t dot = byPath ? className.rfind('.') : std::string::npos;
            const std::string module = byPath && dot != std::string::npos
                ? className.substr(sizeof(kScript) - 1, dot - (sizeof(kScript) - 1)) : std::string();
            const std::string sdkName = byPath ? (dot == std::string::npos ? std::string() : className.substr(dot + 1)) : className;
            if (sdkName.size() < 2 || !strchr("AUF", sdkName[0]))
            {
                answer.error = className + " is not a Dumper-7 class or struct name (ACrX, UCrX, FCrX)";
                return answer;
            }
            const std::string name = sdkName.substr(1);
            const SDK::EClassCastFlags kind = sdkName[0] == 'F' ? SDK::EClassCastFlags::ScriptStruct : SDK::EClassCastFlags::Class;

            std::vector<const SDK::UStruct*> found;
            if (byPath)
            {
                const auto it = index.byPath.find(module + "." + name);
                if (it != index.byPath.end())
                    if (const SDK::UStruct* t = TypeAt(it->second, name))
                        found.push_back(t);
            }
            else if (const auto it = index.byName.find(name); it != index.byName.end())
            {
                for (int32_t i : it->second)
                    if (const SDK::UStruct* t = TypeAt(i, name))
                        found.push_back(t);
            }
            found.erase(std::remove_if(found.begin(), found.end(),
                [kind](const SDK::UStruct* t) { return !t->HasTypeFlag(kind); }), found.end());

            if (found.size() > 1)
                answer.error = className + " is the name of " + std::to_string(found.size())
                    + " native types; name it by /Script/<Module>." + sdkName;
            else if (found.empty())
            {
                answer.error = "no native " + std::string(sdkName[0] == 'F' ? "struct " : "class ") + className;
                answer.unreflected = true;
            }
            else
                type = found.front();
            return answer;
        }

        void ReadMember(const SDK::UStruct* type, const std::string& member, VersionMap::LiveField& answer)
        {
            if (member.empty())
            {
                answer.size = static_cast<uint32_t>(type->Size);
                return;
            }
            const SDK::FProperty* property = Hooks::ObjectProperties::FindPropertyByName(
                const_cast<SDK::UStruct*>(type), member.c_str());
            // FName compares ignore case; the SDK's spelling can differ from
            // the reflected one only in case (UniqueID / UniqueId).
            for (const SDK::UStruct* current = type; !property && current; current = current->SuperStruct)
                for (const SDK::FField* field = current->ChildProperties; field && !property; field = field->Next)
                    if (_stricmp(field->Name.GetRawString().c_str(), member.c_str()) == 0)
                        property = static_cast<const SDK::FProperty*>(field);
            if (!property)
            {
                answer.error = "no member " + member + " in it or its supers";
                return;
            }
            const LiveProperty live = Describe(property);
            answer.offset   = live.offset;
            answer.size     = static_cast<uint32_t>(live.size);
            answer.isBool   = live.isBool;
            answer.boolMask = live.fieldMask;
        }

        void Lookup(const std::string& className, const std::string& member, VersionMap::LiveField& answer)
        {
            if (g_types.objects < 0)
                BuildTypeIndex(g_types);
            const SDK::UStruct* type = nullptr;
            answer = FindType(g_types, className, type);
            // Types registered since the index was built, or a slot reused:
            // build it again, once.
            if (!type && answer.error.rfind("no native", 0) == 0
                && SDK::UObject::GObjects.GetTypedPtr()->Num() != g_types.objects)
            {
                BuildTypeIndex(g_types);
                answer = FindType(g_types, className, type);
            }
            if (type)
            {
                // The class and each of its supers, as the layout check names
                // them, so a request can tell that a base class changed.
                std::unordered_map<const SDK::UObject*, std::string> modules;
                for (const SDK::UStruct* current = type; current; current = current->SuperStruct)
                {
                    const SDK::UObject* package = current->Outer;
                    if (!package || package->Outer)
                        continue;
                    const std::string& module = ModuleOf(package, modules);
                    if (!module.empty())
                        answer.paths.push_back(module + "." + current->Name.GetRawString());
                }
                ReadMember(type, member, answer);
            }
        }

        bool LookupGuarded(const std::string* className, const std::string* member,
            VersionMap::LiveField* answer, DWORD* exceptionCode)
        {
            __try
            {
                Lookup(*className, *member, *answer);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                *exceptionCode = GetExceptionCode();
                return false;
            }
        }
    }

    VersionMap::LiveField LiveFieldOf(const std::string& className, const std::string& member)
    {
        VersionMap::LiveField answer;
        std::lock_guard<std::mutex> lock(g_typesLock);
        // Every native type is registered once engine init has returned.
        if (g_types.objects < 0 && (!Hooks::EngineInit::IsEngineInitialized() || !SdkGlobals::VerifyGObjects()))
        {
            answer.error = "the game's reflection data is not ready";
            return answer;
        }
        DWORD exceptionCode = 0;
        if (!LookupGuarded(&className, &member, &answer, &exceptionCode))
        {
            g_types = TypeIndex{};
            answer = VersionMap::LiveField{};
            char code[16]{};
            sprintf_s(code, "0x%08lX", exceptionCode);
            answer.error = std::string("exception ") + code + " while reading reflection data";
        }
        return answer;
    }
}

#endif
