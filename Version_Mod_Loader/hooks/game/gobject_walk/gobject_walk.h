#pragma once

#include "CoreUObject_classes.hpp"
#include <cstdint>

// ---------------------------------------------------------------------------
// GObjects walking, object lookup, and generic UFunction invocation
//
// Purpose: lets plugins enumerate live UObjects (by class name, by object
// name, or unfiltered) and call a UFunction on any of them via ProcessEvent,
// without the maintainer hand-writing a new hooks/game/<event>/ module for
// every callback a plugin might want.
//
// This is deliberately on-demand only -- every call here walks SDK::UObject::
// GObjects synchronously (tens of thousands of entries). There is NO hook
// into UObject construction/AddObject, so nothing fires live as objects
// spawn; callers must trigger a walk themselves and cache the result.
//
// Known limitation: garbage/pending-kill filtering is best-effort. This SDK
// dump only exposes EObjectFlags on UObject (BeginDestroyed/FinishDestroyed
// are checked); EInternalObjectFlags is not separately available, so some
// objects mid-destruction may still pass the filter.
//
// UFunction invocation has no parameter marshaling: this SDK dump does not
// expose a UFunction params-size field, so callers are responsible for
// knowing/building the exact native Params struct layout for whatever
// UFunction they call.
// ---------------------------------------------------------------------------

namespace Hooks::GObjectWalk
{
	struct ObjectInfo
	{
		SDK::UObject* object;      // never null when passed to a visitor
		const char*   className;   // object->Class->GetName(); valid only during the call
		const char*   objectName;  // object->GetName(); valid only during the call
		uint32_t      nameNumber;  // object->Name.Number -- UE's own instance disambiguator
		uint32_t      objectFlags; // raw EObjectFlags bits
		int32_t       objectIndex; // index into GObjects
	};

	// Visitor return value is an early-exit signal for the *ByName helpers
	// (false stops the walk early); WalkAll ignores the return value.
	using ObjectVisitor = bool(*)(const ObjectInfo& info, void* userContext);

	// True once GObjects looks populated (mirrors Hooks::EngineInit::IsEngineInitialized()).
	bool IsReady();

	// Walks every GObjects entry, skipping null Object/Class and objects
	// flagged BeginDestroyed/FinishDestroyed, calling visitor for the rest.
	// Returns the number of objects visited. EXPENSIVE -- on-demand only.
	int WalkAll(ObjectVisitor visitor, void* userContext);

	// Convenience filters layered on WalkAll.
	int FindObjectsByClassName(const char* className, ObjectVisitor visitor, void* userContext);
	SDK::UObject* FindFirstObjectByName(const char* objectName); // exact FName string match, first hit
	int FindObjectsByName(const char* objectName, ObjectVisitor visitor, void* userContext); // handles Name_N collisions

	// Looks up className::funcName via UClass::GetFunction and calls
	// object->ProcessEvent(fn, paramsBuffer). paramsBuffer must already match
	// the UFunction's native Params layout -- no marshaling, no size
	// validation. Returns false if object/class/function could not be resolved.
	bool InvokeUFunctionByName(SDK::UObject* object, const char* className,
		const char* funcName, void* paramsBuffer);

	// Resolve once, call many times, to avoid repeated name lookups in a loop
	// that is still caller-triggered, not engine-tick-triggered.
	SDK::UFunction* ResolveUFunction(const char* className, const char* funcName);
	bool InvokeResolvedUFunction(SDK::UObject* object, SDK::UFunction* fn, void* paramsBuffer);

	// One-shot dev/export pass (NOT exposed to plugins): walks GObjects once,
	// collects every distinct UClass name plus, for each, every UFunction
	// name found in its Children chain, and writes a flat text dump
	// ("ClassName.FunctionName" lines, classes with no functions get a bare
	// "ClassName" line) to outputPath. This raw dump is input to a manual
	// curation pass that builds a generated header of known-name constants
	// for plugin IDE autocomplete -- it is not itself shipped to plugins.
	// Returns entries written, -1 on file-open failure.
	int ExportKnownClassesAndFunctions(const char* outputPath);
}
