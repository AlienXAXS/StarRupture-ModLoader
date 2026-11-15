#include "pch.h"
#include "gobject_walk.h"
#include "logging/logger.h"
#include "hooks/game/engine_init/engine_init.h"
#include "Basic.hpp"
#include <atomic>
#include <fstream>
#include <unordered_map>
#include <string>
#include <cstring>

namespace Hooks::GObjectWalk
{
	bool IsReady()
	{
		return Hooks::EngineInit::IsEngineInitialized();
	}

	// Offset of SerialNumber within FUObjectItem; not exposed by this SDK
	// dump (Basic.hpp collapses it into Pad_8). Verified via IDA Local
	// Types against this game's binary -- see the delegate hook's file header.
	constexpr size_t kSerialNumberOffset = 0x10;

	// Mirrors TUObjectArray::GetByIndex's chunk math (Basic.hpp) but
	// returns the FUObjectItem* instead of ->Object, so we can reach the
	// raw SerialNumber bytes that aren't named in this SDK's struct.
	SDK::FUObjectItem* GetUObjectItem(int32_t index)
	{
		auto* arr = SDK::UObject::GObjects.GetTypedPtr();
		if (!arr)
		{
			ModLoaderLogger::LogError(L"[GObjectWalk] GetUObjectItem: GObjects.GetTypedPtr() returned null");
			return nullptr;
		}
		if (index < 0 || index >= arr->NumElements)
		{
			ModLoaderLogger::LogError(L"[GObjectWalk] GetUObjectItem: index %d out of range (NumElements=%d)", index, arr->NumElements);
			return nullptr;
		}

		const int32_t chunkIndex = index / SDK::TUObjectArray::ElementsPerChunk;
		const int32_t inChunkIdx = index % SDK::TUObjectArray::ElementsPerChunk;
		if (chunkIndex >= arr->NumChunks)
		{
			ModLoaderLogger::LogError(L"[GObjectWalk] GetUObjectItem: chunkIndex %d >= NumChunks %d", chunkIndex, arr->NumChunks);
			return nullptr;
		}

		SDK::FUObjectItem* chunkPtr = arr->GetDecrytedObjPtr()[chunkIndex];
		if (!chunkPtr)
		{
			ModLoaderLogger::LogError(L"[GObjectWalk] GetUObjectItem: null chunk pointer at chunkIndex %d", chunkIndex);
			return nullptr;
		}

		return &chunkPtr[inChunkIdx];
	}

	// Real UE FWeakObjectPtr::Get() treats ObjectSerialNumber == 0 as "this
	// weak pointer was never assigned" and returns null WITHOUT checking the
	// array -- confirmed live: the engine's own Broadcast() never reached our
	// spliced FScriptDelegate because the host object's FUObjectItem had
	// never had a serial number lazily allocated (nothing had taken a weak
	// ptr to it before). The real engine's FUObjectArray::AllocateSerialNumber
	// allocates a fresh nonzero serial ON FIRST USE and writes it back into
	// the live FUObjectItem -- we replicate that here with our own counter.
	// Uniqueness only needs to hold per-slot (the engine matches by
	// {ObjectIndex, ObjectSerialNumber} together), so a separate counter
	// pool from the engine's own is fine as long as we never hand out the
	// same value twice for the same slot.
	int32_t AllocateOrReadSerialNumber(SDK::FUObjectItem* item)
	{
		static std::atomic<int32_t> s_nextSerial{ 0x10000000 }; // pool well clear of the engine's own small serials

		auto* serialSlot = reinterpret_cast<volatile LONG*>(reinterpret_cast<uint8_t*>(item) + kSerialNumberOffset);

		LONG current = *serialSlot;
		if (current != 0)
			return current;

		LONG candidate = s_nextSerial.fetch_add(1);
		LONG prev = InterlockedCompareExchange(serialSlot, candidate, 0);
		return (prev == 0) ? candidate : prev;
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

	static bool PassesLookupMode(SDK::UObject* obj, LookupMode mode)
	{
		const bool isCDO = obj->Flags & SDK::EObjectFlags::ClassDefaultObject;
		const bool isArchetype = obj->Flags & SDK::EObjectFlags::ArchetypeObject;

		switch (mode)
		{
		case LookupMode::InstanceOnly:
			return !isCDO && !isArchetype;
		case LookupMode::CDOOnly:
			return isCDO;
		case LookupMode::Both:
		default:
			return true;
		}
	}

	static void FillInfo(ObjectInfo& info, SDK::UObject* obj, const std::string& className,
		const std::string& objectName, int32_t index)
	{
		info.object = obj;
		strncpy_s(info.className, className.c_str(), _TRUNCATE);
		strncpy_s(info.objectName, objectName.c_str(), _TRUNCATE);
		info.nameNumber = obj->Name.Number;
		info.objectFlags = static_cast<uint32_t>(obj->Flags);
		info.objectIndex = index;
	}

	int WalkAllInto(LookupMode mode, ObjectInfo* outArray, int capacity)
	{
		if (!IsReady())
			return 0;

		int matched = 0;
		const int32_t num = SDK::UObject::GObjects->Num();

		for (int32_t i = 0; i < num; ++i)
		{
			SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
			if (ShouldSkip(obj) || !PassesLookupMode(obj, mode))
				continue;

			if (outArray && matched < capacity)
			{
				std::string className = obj->Class->GetName();
				std::string objectName = obj->GetName();
				FillInfo(outArray[matched], obj, className, objectName, i);
			}

			++matched;
		}

		return matched;
	}

	int FindObjectsByClassNameInto(const char* className, LookupMode mode, ObjectInfo* outArray, int capacity)
	{
		if (!className || !IsReady())
			return 0;

		int matched = 0;
		const int32_t num = SDK::UObject::GObjects->Num();

		for (int32_t i = 0; i < num; ++i)
		{
			SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
			if (ShouldSkip(obj) || !PassesLookupMode(obj, mode))
				continue;

			std::string objClassName = obj->Class->GetName();
			if (objClassName != className)
				continue;

			if (outArray && matched < capacity)
			{
				std::string objectName = obj->GetName();
				FillInfo(outArray[matched], obj, objClassName, objectName, i);
			}

			++matched;
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

	int FindObjectsByNameInto(const char* objectName, LookupMode mode, ObjectInfo* outArray, int capacity)
	{
		if (!objectName || !IsReady())
			return 0;

		int matched = 0;
		const int32_t num = SDK::UObject::GObjects->Num();

		for (int32_t i = 0; i < num; ++i)
		{
			SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
			if (ShouldSkip(obj) || !PassesLookupMode(obj, mode))
				continue;

			std::string thisObjectName = obj->GetName();
			if (thisObjectName != objectName)
				continue;

			if (outArray && matched < capacity)
			{
				std::string className = obj->Class->GetName();
				FillInfo(outArray[matched], obj, className, thisObjectName, i);
			}

			++matched;
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
