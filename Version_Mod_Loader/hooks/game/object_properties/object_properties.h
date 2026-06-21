#pragma once

#include "CoreUObject_classes.hpp"
#include <cstdint>

// ---------------------------------------------------------------------------
// Reflection-based property access (FProperty / UClass).
//
// Purpose: lets plugins read and write arbitrary UPROPERTY fields by name,
// resolved at runtime against the live UClass property list (ChildProperties
// + SuperStruct chain), instead of baking in a compile-time struct offset
// that moves every time Dumper-7 re-dumps the SDK after a game update. The
// only thing that has to stay stable across an update is the property's
// *name* -- the engine's own FProperty::Offset is read fresh every call.
//
// Does NOT cover fields with no UPROPERTY/FProperty entry (plain native C++
// members) -- those have no reflection metadata to look up by name and still
// require a hand-derived raw offset (separate PDB-backed offset tooling,
// not part of this module).
//
// String/Name properties are read-only here: FString/FName own their own
// backing storage (FString is a heap-owning TArray<wchar_t>), and writing one
// in place safely requires the engine's own string allocator, which is out
// of scope for a generic accessor.
// ---------------------------------------------------------------------------

namespace Hooks::ObjectProperties
{
	// Simplified type classification for a resolved FProperty, so callers can
	// branch without needing to know raw EClassCastFlags bit values.
	enum class PropertyKind : int32_t
	{
		Unknown = 0,
		Bool,
		Int,    // any signed/unsigned integer width 1-8 bytes (Byte/Int8/Int16/UInt16/Int/UInt32/Int64/UInt64)
		Float,  // FloatProperty or DoubleProperty
		Name,
		Str,
		Object, // raw-pointer-backed UObject* (ObjectProperty). NOT WeakObjectProperty/SoftObjectProperty/LazyObjectProperty.
		Struct,
		Array,
		Enum,
		Unsupported, // recognized FProperty subclass with no typed accessor here (Map/Set/Delegate/Interface/...)
	};

	// True once GObjects looks populated (mirrors Hooks::EngineInit::IsEngineInitialized()).
	bool IsReady();

	// Walks ChildProperties (FField list) + SuperStruct chain looking for an
	// exact property name match. Returns null if not found.
	SDK::FProperty* FindPropertyByName(SDK::UStruct* cls, const char* propertyName);

	// Convenience: resolves object->Class then calls FindPropertyByName.
	SDK::FProperty* FindPropertyOnObject(SDK::UObject* object, const char* propertyName);

	// Resolve a property directly by class name + property name string (no
	// live object required) -- mirrors GObjectWalk::ResolveUFunction.
	SDK::FProperty* ResolveProperty(const char* className, const char* propertyName);

	PropertyKind GetPropertyKind(SDK::FProperty* property);
	size_t GetPropertySize(SDK::FProperty* property);      // ElementSize * ArrayDim, 0 if null
	int32_t GetPropertyArrayDim(SDK::FProperty* property);  // 0 if null

	// Raw escape hatch: container + property->Offset, no type checking. For
	// property kinds without a typed accessor below (Array, Map, Set, ...).
	void* GetPropertyRawPtr(void* container, SDK::FProperty* property);

	// Typed getters/setters. Each validates the property's CastFlags before
	// touching memory and returns false on type mismatch or null arguments.
	bool GetBoolProperty(void* container, SDK::FProperty* property, bool* outValue);
	bool SetBoolProperty(void* container, SDK::FProperty* property, bool value);

	bool GetIntProperty(void* container, SDK::FProperty* property, int64_t* outValue);
	bool SetIntProperty(void* container, SDK::FProperty* property, int64_t value);

	bool GetFloatProperty(void* container, SDK::FProperty* property, double* outValue);
	bool SetFloatProperty(void* container, SDK::FProperty* property, double value);

	bool GetObjectProperty(void* container, SDK::FProperty* property, SDK::UObject** outValue);
	bool SetObjectProperty(void* container, SDK::FProperty* property, SDK::UObject* value);

	// Read-only -- see file header comment on why String/Name have no setter.
	bool GetStringProperty(void* container, SDK::FProperty* property, char* outBuffer, int bufferSize);
	bool GetNameProperty(void* container, SDK::FProperty* property, char* outBuffer, int bufferSize);

	// For FStructProperty: the nested UStruct, so callers can recurse
	// FindPropertyByName on inner fields. Null for any other property kind.
	SDK::UStruct* GetPropertyStructType(SDK::FProperty* property);

	// One-shot dev/export pass (NOT exposed to plugins): walks GObjects once
	// and, for every distinct UClass, writes every property it directly
	// declares (ChildProperties only -- NOT the SuperStruct chain, same
	// dedup rationale as ExportKnownClassesAndFunctions: every ancestor class
	// is itself a separate UClass entry in GObjects and contributes its own
	// properties when visited on its own iteration). Lines are formatted
	// "ClassName.PropertyName (Kind)"; classes with no properties get a bare
	// "ClassName" line. This raw dump is input to a manual curation pass, not
	// itself shipped to plugins. Returns entries written, -1 on file-open failure.
	int ExportKnownProperties(const char* outputPath);
}
