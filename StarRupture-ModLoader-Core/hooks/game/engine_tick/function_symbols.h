#pragma once

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// FunctionSymbols
//
// Best-effort address -> "ClassName::FunctionName" resolver for the stack
// sampler (stack_sampler.h). StarRupture ships without PDBs, so arbitrary
// native code has no name -- but every UFUNCTION with a native
// implementation carries its own C++ entry point (UFunction::ExecFunction)
// as engine reflection data, available at runtime regardless of symbols.
// This walks GObjects once (lazily, cached) to build a reverse map from
// that pointer back to its owning UClass::UFunction name.
//
// Coverage is partial by nature: only functions Unreal's reflection system
// knows about (UFUNCTIONs) resolve to a name. Plain C++ internals (physics,
// scene management, most of the tick dispatch machinery itself) have no
// UFunction and will still show as Module+Offset -- Resolve() just returns
// an empty string for those, the caller falls back.
// ---------------------------------------------------------------------------
namespace Hooks::EngineTick::FunctionSymbols
{
	// Returns "ClassName::FunctionName" if functionStartAddress is exactly
	// some native UFunction's ExecFunction pointer, otherwise an empty
	// string. Builds/caches the reverse map on first call (a full GObjects
	// walk, same cost class as GObjectWalk::ExportKnownClassesAndFunctions)
	// -- game-thread only, not safe to call while any thread is suspended.
	std::string Resolve(uintptr_t functionStartAddress);
}
