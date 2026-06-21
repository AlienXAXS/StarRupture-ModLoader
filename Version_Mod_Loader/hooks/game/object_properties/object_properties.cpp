#include "pch.h"
#include "object_properties.h"
#include "logging/logger.h"
#include "hooks/game/engine_init/engine_init.h"
#include "Basic.hpp"
#include <fstream>
#include <unordered_map>
#include <string>
#include <cstring>

namespace Hooks::ObjectProperties
{
	bool IsReady()
	{
		return Hooks::EngineInit::IsEngineInitialized();
	}

	static bool HasCastFlag(SDK::FProperty* property, SDK::EClassCastFlags flag)
	{
		if (!property || !property->ClassPrivate)
			return false;

		return static_cast<SDK::EClassCastFlags>(property->ClassPrivate->CastFlags) & flag;
	}

	SDK::FProperty* FindPropertyByName(SDK::UStruct* cls, const char* propertyName)
	{
		if (!cls || !propertyName)
			return nullptr;

		for (SDK::UStruct* current = cls; current; current = current->SuperStruct)
		{
			for (SDK::FField* field = current->ChildProperties; field; field = field->Next)
			{
				if (field->Name.ToString() == propertyName)
					return static_cast<SDK::FProperty*>(field);
			}
		}

		return nullptr;
	}

	SDK::FProperty* FindPropertyOnObject(SDK::UObject* object, const char* propertyName)
	{
		if (!object || !object->Class)
			return nullptr;

		return FindPropertyByName(object->Class, propertyName);
	}

	SDK::FProperty* ResolveProperty(const char* className, const char* propertyName)
	{
		if (!className || !propertyName)
			return nullptr;

		SDK::UClass* cls = SDK::UObject::FindClassFast(className);
		if (!cls)
		{
			ModLoaderLogger::LogWarn(L"[ObjectProperties] ResolveProperty: class '%S' not found", className);
			return nullptr;
		}

		SDK::FProperty* prop = FindPropertyByName(cls, propertyName);
		if (!prop)
		{
			ModLoaderLogger::LogWarn(L"[ObjectProperties] ResolveProperty: '%S::%S' not found", className, propertyName);
			return nullptr;
		}

		return prop;
	}

	PropertyKind GetPropertyKind(SDK::FProperty* property)
	{
		if (!property || !property->ClassPrivate)
			return PropertyKind::Unknown;

		auto flags = static_cast<SDK::EClassCastFlags>(property->ClassPrivate->CastFlags);

		if (flags & SDK::EClassCastFlags::BoolProperty)   return PropertyKind::Bool;
		if (flags & SDK::EClassCastFlags::FloatProperty)  return PropertyKind::Float;
		if (flags & SDK::EClassCastFlags::DoubleProperty) return PropertyKind::Float;
		if (flags & SDK::EClassCastFlags::NameProperty)   return PropertyKind::Name;
		if (flags & SDK::EClassCastFlags::StrProperty)    return PropertyKind::Str;
		if (flags & SDK::EClassCastFlags::ObjectProperty) return PropertyKind::Object;
		if (flags & SDK::EClassCastFlags::StructProperty) return PropertyKind::Struct;
		if (flags & SDK::EClassCastFlags::ArrayProperty)  return PropertyKind::Array;
		if (flags & SDK::EClassCastFlags::EnumProperty)   return PropertyKind::Enum;

		if (flags & SDK::EClassCastFlags::ByteProperty   || flags & SDK::EClassCastFlags::Int8Property ||
			flags & SDK::EClassCastFlags::Int16Property  || flags & SDK::EClassCastFlags::UInt16Property ||
			flags & SDK::EClassCastFlags::IntProperty    || flags & SDK::EClassCastFlags::UInt32Property ||
			flags & SDK::EClassCastFlags::Int64Property  || flags & SDK::EClassCastFlags::UInt64Property)
			return PropertyKind::Int;

		return PropertyKind::Unsupported;
	}

	static const char* PropertyKindName(PropertyKind kind)
	{
		switch (kind)
		{
		case PropertyKind::Bool:        return "Bool";
		case PropertyKind::Int:         return "Int";
		case PropertyKind::Float:       return "Float";
		case PropertyKind::Name:        return "Name";
		case PropertyKind::Str:         return "Str";
		case PropertyKind::Object:      return "Object";
		case PropertyKind::Struct:      return "Struct";
		case PropertyKind::Array:       return "Array";
		case PropertyKind::Enum:        return "Enum";
		case PropertyKind::Unsupported: return "Unsupported";
		default:                        return "Unknown";
		}
	}

	size_t GetPropertySize(SDK::FProperty* property)
	{
		if (!property)
			return 0;

		return static_cast<size_t>(property->ElementSize) * static_cast<size_t>(property->ArrayDim);
	}

	int32_t GetPropertyArrayDim(SDK::FProperty* property)
	{
		return property ? property->ArrayDim : 0;
	}

	void* GetPropertyRawPtr(void* container, SDK::FProperty* property)
	{
		if (!container || !property)
			return nullptr;

		return reinterpret_cast<uint8_t*>(container) + property->Offset;
	}

	bool GetBoolProperty(void* container, SDK::FProperty* property, bool* outValue)
	{
		if (!container || !property || !outValue || !HasCastFlag(property, SDK::EClassCastFlags::BoolProperty))
			return false;

		auto* boolProp = static_cast<SDK::FBoolProperty*>(property);
		uint8_t* ptr = reinterpret_cast<uint8_t*>(container) + property->Offset + boolProp->ByteOffset;
		*outValue = (*ptr & boolProp->FieldMask) != 0;
		return true;
	}

	bool SetBoolProperty(void* container, SDK::FProperty* property, bool value)
	{
		if (!container || !property || !HasCastFlag(property, SDK::EClassCastFlags::BoolProperty))
			return false;

		auto* boolProp = static_cast<SDK::FBoolProperty*>(property);
		uint8_t* ptr = reinterpret_cast<uint8_t*>(container) + property->Offset + boolProp->ByteOffset;
		if (value)
			*ptr |= boolProp->FieldMask;
		else
			*ptr &= static_cast<uint8_t>(~boolProp->FieldMask);
		return true;
	}

	bool GetIntProperty(void* container, SDK::FProperty* property, int64_t* outValue)
	{
		if (!container || !property || !outValue || !property->ClassPrivate)
			return false;

		auto flags = static_cast<SDK::EClassCastFlags>(property->ClassPrivate->CastFlags);
		uint8_t* ptr = reinterpret_cast<uint8_t*>(container) + property->Offset;

		if (flags & SDK::EClassCastFlags::Int8Property)   { *outValue = *reinterpret_cast<int8_t*>(ptr);   return true; }
		if (flags & SDK::EClassCastFlags::ByteProperty)   { *outValue = *reinterpret_cast<uint8_t*>(ptr);  return true; }
		if (flags & SDK::EClassCastFlags::Int16Property)  { *outValue = *reinterpret_cast<int16_t*>(ptr);  return true; }
		if (flags & SDK::EClassCastFlags::UInt16Property) { *outValue = *reinterpret_cast<uint16_t*>(ptr); return true; }
		if (flags & SDK::EClassCastFlags::IntProperty)    { *outValue = *reinterpret_cast<int32_t*>(ptr);  return true; }
		if (flags & SDK::EClassCastFlags::UInt32Property) { *outValue = *reinterpret_cast<uint32_t*>(ptr); return true; }
		if (flags & SDK::EClassCastFlags::Int64Property)  { *outValue = *reinterpret_cast<int64_t*>(ptr);  return true; }
		if (flags & SDK::EClassCastFlags::UInt64Property) { *outValue = static_cast<int64_t>(*reinterpret_cast<uint64_t*>(ptr)); return true; }

		return false;
	}

	bool SetIntProperty(void* container, SDK::FProperty* property, int64_t value)
	{
		if (!container || !property || !property->ClassPrivate)
			return false;

		auto flags = static_cast<SDK::EClassCastFlags>(property->ClassPrivate->CastFlags);
		uint8_t* ptr = reinterpret_cast<uint8_t*>(container) + property->Offset;

		if (flags & SDK::EClassCastFlags::Int8Property)   { *reinterpret_cast<int8_t*>(ptr)   = static_cast<int8_t>(value);   return true; }
		if (flags & SDK::EClassCastFlags::ByteProperty)   { *reinterpret_cast<uint8_t*>(ptr)  = static_cast<uint8_t>(value);  return true; }
		if (flags & SDK::EClassCastFlags::Int16Property)  { *reinterpret_cast<int16_t*>(ptr)  = static_cast<int16_t>(value);  return true; }
		if (flags & SDK::EClassCastFlags::UInt16Property) { *reinterpret_cast<uint16_t*>(ptr) = static_cast<uint16_t>(value); return true; }
		if (flags & SDK::EClassCastFlags::IntProperty)    { *reinterpret_cast<int32_t*>(ptr)  = static_cast<int32_t>(value);  return true; }
		if (flags & SDK::EClassCastFlags::UInt32Property) { *reinterpret_cast<uint32_t*>(ptr) = static_cast<uint32_t>(value); return true; }
		if (flags & SDK::EClassCastFlags::Int64Property)  { *reinterpret_cast<int64_t*>(ptr)  = value; return true; }
		if (flags & SDK::EClassCastFlags::UInt64Property) { *reinterpret_cast<uint64_t*>(ptr) = static_cast<uint64_t>(value); return true; }

		return false;
	}

	bool GetFloatProperty(void* container, SDK::FProperty* property, double* outValue)
	{
		if (!container || !property || !outValue || !property->ClassPrivate)
			return false;

		auto flags = static_cast<SDK::EClassCastFlags>(property->ClassPrivate->CastFlags);
		uint8_t* ptr = reinterpret_cast<uint8_t*>(container) + property->Offset;

		if (flags & SDK::EClassCastFlags::FloatProperty)  { *outValue = static_cast<double>(*reinterpret_cast<float*>(ptr)); return true; }
		if (flags & SDK::EClassCastFlags::DoubleProperty) { *outValue = *reinterpret_cast<double*>(ptr); return true; }

		return false;
	}

	bool SetFloatProperty(void* container, SDK::FProperty* property, double value)
	{
		if (!container || !property || !property->ClassPrivate)
			return false;

		auto flags = static_cast<SDK::EClassCastFlags>(property->ClassPrivate->CastFlags);
		uint8_t* ptr = reinterpret_cast<uint8_t*>(container) + property->Offset;

		if (flags & SDK::EClassCastFlags::FloatProperty)  { *reinterpret_cast<float*>(ptr) = static_cast<float>(value); return true; }
		if (flags & SDK::EClassCastFlags::DoubleProperty) { *reinterpret_cast<double*>(ptr) = value; return true; }

		return false;
	}

	bool GetObjectProperty(void* container, SDK::FProperty* property, SDK::UObject** outValue)
	{
		if (!container || !property || !outValue || !HasCastFlag(property, SDK::EClassCastFlags::ObjectProperty))
			return false;

		uint8_t* ptr = reinterpret_cast<uint8_t*>(container) + property->Offset;
		*outValue = *reinterpret_cast<SDK::UObject**>(ptr);
		return true;
	}

	bool SetObjectProperty(void* container, SDK::FProperty* property, SDK::UObject* value)
	{
		if (!container || !property || !HasCastFlag(property, SDK::EClassCastFlags::ObjectProperty))
			return false;

		uint8_t* ptr = reinterpret_cast<uint8_t*>(container) + property->Offset;
		*reinterpret_cast<SDK::UObject**>(ptr) = value;
		return true;
	}

	bool GetStringProperty(void* container, SDK::FProperty* property, char* outBuffer, int bufferSize)
	{
		if (!container || !property || !outBuffer || bufferSize <= 0 || !HasCastFlag(property, SDK::EClassCastFlags::StrProperty))
			return false;

		uint8_t* ptr = reinterpret_cast<uint8_t*>(container) + property->Offset;
		auto* str = reinterpret_cast<SDK::FString*>(ptr);
		std::string value = str->ToString();
		strncpy_s(outBuffer, static_cast<size_t>(bufferSize), value.c_str(), _TRUNCATE);
		return true;
	}

	bool GetNameProperty(void* container, SDK::FProperty* property, char* outBuffer, int bufferSize)
	{
		if (!container || !property || !outBuffer || bufferSize <= 0 || !HasCastFlag(property, SDK::EClassCastFlags::NameProperty))
			return false;

		uint8_t* ptr = reinterpret_cast<uint8_t*>(container) + property->Offset;
		auto* name = reinterpret_cast<SDK::FName*>(ptr);
		std::string value = name->ToString();
		strncpy_s(outBuffer, static_cast<size_t>(bufferSize), value.c_str(), _TRUNCATE);
		return true;
	}

	SDK::UStruct* GetPropertyStructType(SDK::FProperty* property)
	{
		if (!property || !HasCastFlag(property, SDK::EClassCastFlags::StructProperty))
			return nullptr;

		return static_cast<SDK::FStructProperty*>(property)->Struct;
	}

	int ExportKnownProperties(const char* outputPath)
	{
		if (!outputPath || !IsReady())
			return -1;

		std::ofstream out(outputPath, std::ios::out | std::ios::trunc);
		if (!out.is_open())
		{
			ModLoaderLogger::LogError(L"[ObjectProperties] ExportKnownProperties: failed to open '%S'", outputPath);
			return -1;
		}

		// Each UClass appears exactly once in GObjects, so walk only the
		// properties it directly declares (cls->ChildProperties) -- NOT the
		// SuperStruct chain, for the same reason ExportKnownClassesAndFunctions
		// doesn't walk cls->Children's ancestors: every ancestor class is
		// itself a separate UClass entry in GObjects and contributes its own
		// properties when visited on its own iteration.
		std::unordered_map<std::string, bool> emitted;
		int written = 0;

		const int32_t num = SDK::UObject::GObjects->Num();
		for (int32_t i = 0; i < num; ++i)
		{
			SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
			if (!obj || !obj->Class)
				continue;
			if (obj->Flags & SDK::EObjectFlags::BeginDestroyed)
				continue;
			if (obj->Flags & SDK::EObjectFlags::FinishDestroyed)
				continue;
			if (!obj->IsA(SDK::EClassCastFlags::Class))
				continue;

			auto* cls = static_cast<SDK::UClass*>(obj);
			std::string className = cls->GetName();

			bool wroteAnyProperty = false;
			for (SDK::FField* field = cls->ChildProperties; field; field = field->Next)
			{
				auto* prop = static_cast<SDK::FProperty*>(field);
				std::string propName = field->Name.ToString();
				std::string line = className + "." + propName;

				if (emitted.find(line) != emitted.end())
					continue;
				emitted[line] = true;

				out << line << " (" << PropertyKindName(GetPropertyKind(prop)) << ")\n";
				++written;
				wroteAnyProperty = true;
			}

			if (!wroteAnyProperty && emitted.find(className) == emitted.end())
			{
				emitted[className] = true;
				out << className << "\n";
				++written;
			}
		}

		out.close();
		ModLoaderLogger::LogInfo(L"[ObjectProperties] ExportKnownProperties: wrote %d entries to '%S'", written, outputPath);
		return written;
	}
}
