#include "layout_check.h"

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
}

#if defined(MODLOADER_CLIENT_BUILD)

#include "layout_table.generated.h"
#include "sdk_globals.h"
#include "../hooks/game/engine_init/engine_init.h"
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

static_assert(SrLayoutTable::kFormatVersion == 1, "layout_check.cpp reads layout table format 1");
static_assert(SrLayoutTable::kDetail == 1, "an embedded layout table needs member sizes and bool masks (generate it with --sdk)");

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
    }

    bool Run(Result& out)
    {
        const auto start = std::chrono::steady_clock::now();

        if (!SdkGlobals::VerifyGObjects())
        {
            ModLoaderLogger::LogError(L"[LayoutCheck] Skipped: GObjects does not look valid");
            return false;
        }

        const std::vector<TableClass> table = ReadTable(SrLayoutTable::kNames, SrLayoutTable::kClasses, SrLayoutTable::kRows);

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

        for (const std::string& line : result.lines)
            ModLoaderLogger::LogWarn(L"[LayoutCheck] %S", line.c_str());

        const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        void (*const log)(const wchar_t*, ...) =
            result.mismatched == 0 && result.missing == 0 ? ModLoaderLogger::LogInfo : ModLoaderLogger::LogWarn;
        log(
            L"[LayoutCheck] SDK CL %u against the game: %d classes, %d checked, %d mismatched, %d missing, %d deferred,"
            L" %d rows resolved per build -- GObjects pass %.1f ms (%d objects, %d types), %.1f ms in all",
            SrLayoutTable::kBuiltCL, result.classes, result.checked, result.mismatched, result.missing, result.deferred,
            result.perBuild,
            search.walkMs, search.objects, search.types, totalMs);

        out = std::move(result);
        return true;
    }
}

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
                answer.error = "no native " + std::string(sdkName[0] == 'F' ? "struct " : "class ") + className;
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
                ReadMember(type, member, answer);
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
