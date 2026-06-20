#include "pch.h"
#include "gobject_walk.h"
#include "logging/logger.h"
#include "hooks/game/engine_init/engine_init.h"
#include "Basic.hpp"
#include <fstream>
#include <unordered_map>
#include <string>

namespace Hooks::GObjectWalk
{
	bool IsReady()
	{
		return Hooks::EngineInit::IsEngineInitialized();
	}

	static bool ShouldSkip(SDK::UObject* obj)
	{
		if (!obj || !obj->Class)
			return true;

		if (obj->Flags & SDK::EObjectFlags::BeginDestroyed)
			return true;
		if (obj->Flags & SDK::EObjectFlags::FinishDestroyed)
			return true;

		return false;
	}

	int WalkAll(ObjectVisitor visitor, void* userContext)
	{
		if (!visitor || !IsReady())
			return 0;

		int visited = 0;
		const int32_t num = SDK::UObject::GObjects->Num();

		for (int32_t i = 0; i < num; ++i)
		{
			SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
			if (ShouldSkip(obj))
				continue;

			std::string className = obj->Class->GetName();
			std::string objectName = obj->GetName();

			ObjectInfo info{};
			info.object = obj;
			info.className = className.c_str();
			info.objectName = objectName.c_str();
			info.nameNumber = obj->Name.Number;
			info.objectFlags = static_cast<uint32_t>(obj->Flags);
			info.objectIndex = i;

			++visited;

			try
			{
				visitor(info, userContext);
			}
			catch (const std::exception& e)
			{
				ModLoaderLogger::LogError(L"[GObjectWalk] Exception in visitor: %S", e.what());
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[GObjectWalk] Unknown exception in visitor");
			}
		}

		return visited;
	}

	int FindObjectsByClassName(const char* className, ObjectVisitor visitor, void* userContext)
	{
		if (!className || !visitor || !IsReady())
			return 0;

		int matched = 0;
		const int32_t num = SDK::UObject::GObjects->Num();

		for (int32_t i = 0; i < num; ++i)
		{
			SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
			if (ShouldSkip(obj))
				continue;

			std::string objClassName = obj->Class->GetName();
			if (objClassName != className)
				continue;

			std::string objectName = obj->GetName();

			ObjectInfo info{};
			info.object = obj;
			info.className = objClassName.c_str();
			info.objectName = objectName.c_str();
			info.nameNumber = obj->Name.Number;
			info.objectFlags = static_cast<uint32_t>(obj->Flags);
			info.objectIndex = i;

			++matched;

			bool keepGoing = true;
			try
			{
				keepGoing = visitor(info, userContext);
			}
			catch (const std::exception& e)
			{
				ModLoaderLogger::LogError(L"[GObjectWalk] Exception in visitor: %S", e.what());
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[GObjectWalk] Unknown exception in visitor");
			}

			if (!keepGoing)
				break;
		}

		return matched;
	}

	SDK::UObject* FindFirstObjectByName(const char* objectName)
	{
		if (!objectName || !IsReady())
			return nullptr;

		const int32_t num = SDK::UObject::GObjects->Num();
		for (int32_t i = 0; i < num; ++i)
		{
			SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
			if (ShouldSkip(obj))
				continue;

			if (obj->GetName() == objectName)
				return obj;
		}

		return nullptr;
	}

	int FindObjectsByName(const char* objectName, ObjectVisitor visitor, void* userContext)
	{
		if (!objectName || !visitor || !IsReady())
			return 0;

		int matched = 0;
		const int32_t num = SDK::UObject::GObjects->Num();

		for (int32_t i = 0; i < num; ++i)
		{
			SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
			if (ShouldSkip(obj))
				continue;

			std::string thisObjectName = obj->GetName();
			if (thisObjectName != objectName)
				continue;

			std::string className = obj->Class->GetName();

			ObjectInfo info{};
			info.object = obj;
			info.className = className.c_str();
			info.objectName = thisObjectName.c_str();
			info.nameNumber = obj->Name.Number;
			info.objectFlags = static_cast<uint32_t>(obj->Flags);
			info.objectIndex = i;

			++matched;

			bool keepGoing = true;
			try
			{
				keepGoing = visitor(info, userContext);
			}
			catch (const std::exception& e)
			{
				ModLoaderLogger::LogError(L"[GObjectWalk] Exception in visitor: %S", e.what());
			}
			catch (...)
			{
				ModLoaderLogger::LogError(L"[GObjectWalk] Unknown exception in visitor");
			}

			if (!keepGoing)
				break;
		}

		return matched;
	}

	SDK::UFunction* ResolveUFunction(const char* className, const char* funcName)
	{
		if (!className || !funcName)
			return nullptr;

		SDK::UClass* cls = SDK::UObject::FindClassFast(className);
		if (!cls)
		{
			ModLoaderLogger::LogWarn(L"[GObjectWalk] ResolveUFunction: class '%S' not found", className);
			return nullptr;
		}

		SDK::UFunction* fn = cls->GetFunction(className, funcName);
		if (!fn)
		{
			ModLoaderLogger::LogWarn(L"[GObjectWalk] ResolveUFunction: '%S::%S' not found", className, funcName);
			return nullptr;
		}

		return fn;
	}

	bool InvokeResolvedUFunction(SDK::UObject* object, SDK::UFunction* fn, void* paramsBuffer)
	{
		if (!object || !fn)
			return false;

		object->ProcessEvent(fn, paramsBuffer);
		return true;
	}

	bool InvokeUFunctionByName(SDK::UObject* object, const char* className, const char* funcName, void* paramsBuffer)
	{
		if (!object)
			return false;

		SDK::UFunction* fn = ResolveUFunction(className, funcName);
		if (!fn)
			return false;

		object->ProcessEvent(fn, paramsBuffer);
		return true;
	}

	int ExportKnownClassesAndFunctions(const char* outputPath)
	{
		if (!outputPath || !IsReady())
			return -1;

		std::ofstream out(outputPath, std::ios::out | std::ios::trunc);
		if (!out.is_open())
		{
			ModLoaderLogger::LogError(L"[GObjectWalk] ExportKnownClassesAndFunctions: failed to open '%S'", outputPath);
			return -1;
		}

		// Each UClass appears exactly once in GObjects, so walk only the
		// functions it directly declares (cls->Children) -- NOT the
		// SuperStruct chain, since every ancestor class is itself a separate
		// UClass entry in GObjects and contributes its own functions when
		// visited on its own iteration. Walking SuperStruct here would
		// duplicate every inherited function once per descendant class.
		std::unordered_map<std::string, bool> emitted;
		int written = 0;

		const int32_t num = SDK::UObject::GObjects->Num();
		for (int32_t i = 0; i < num; ++i)
		{
			SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
			if (ShouldSkip(obj))
				continue;

			if (!obj->IsA(SDK::EClassCastFlags::Class))
				continue;

			auto* cls = static_cast<SDK::UClass*>(obj);
			std::string className = cls->GetName();

			bool wroteAnyFunction = false;
			for (SDK::UField* field = cls->Children; field; field = field->Next)
			{
				if (!field->HasTypeFlag(SDK::EClassCastFlags::Function))
					continue;

				std::string funcName = field->GetName();
				std::string line = className + "." + funcName;

				if (emitted.find(line) != emitted.end())
					continue;
				emitted[line] = true;

				out << line << "\n";
				++written;
				wroteAnyFunction = true;
			}

			if (!wroteAnyFunction && emitted.find(className) == emitted.end())
			{
				emitted[className] = true;
				out << className << "\n";
				++written;
			}
		}

		out.close();
		ModLoaderLogger::LogInfo(L"[GObjectWalk] ExportKnownClassesAndFunctions: wrote %d entries to '%S'", written, outputPath);
		return written;
	}
}
