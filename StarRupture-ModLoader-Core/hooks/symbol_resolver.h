#pragma once

#include <cstdint>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// SymbolResolver
//
// Thin, process-wide-serialized wrapper around DbgHelp (SymInitialize /
// SymFromAddrW). DbgHelp is explicitly documented as NOT thread-safe -- every
// call into it from anywhere in the mod loader must be serialized through
// GetMutex(), or two callers on different threads (crash_reporter's
// crash-time stack walk and engine_tick's stack-sampler resolving a
// stutter's hotspots, say) can corrupt its internal state.
//
// Resolves whatever DbgHelp's default search path can find a matching PDB
// for. Out of the box that's just the mod loader's own modules (their .pdb
// sits next to the .dll after a normal local build), but DbgHelp also looks
// next to every other loaded module -- so dropping a
// StarRupture-Win64-Shipping.pdb into Binaries\Win64\ (same folder as the
// .exe) is enough for it to start resolving engine-internal frames too, no
// code change needed here. (Also honors the _NT_SYMBOL_PATH /
// _NT_ALT_SYMBOL_PATH environment variables if set, same as any debugger.)
// ---------------------------------------------------------------------------
namespace Hooks::SymbolResolver
{
	// Idempotent, thread-safe. Resolve() calls this itself; exposed so other
	// DbgHelp callers (crash_reporter) can share the same one-time init
	// instead of duplicating it.
	void EnsureInitialized();

	// Re-enumerates the process's loaded modules into DbgHelp. Idempotent,
	// thread-safe, takes GetMutex() itself -- do not call it while already
	// holding that lock.
	//
	// SymInitialize(fInvadeProcess=TRUE) snapshots the module list exactly
	// ONCE, whenever EnsureInitialized() first runs. Everything loaded after
	// that moment is invisible to DbgHelp: it has no symbols for it at all,
	// not even the export-table fallback, so its frames come back as bare
	// addresses with no module name attached. Which modules that covers
	// depends on what happened to run first -- the stack sampler's first
	// sample or a crash -- and it always covers a plugin hot-loaded from the
	// console, which by definition arrives long afterwards.
	//
	// Plugin frames are the ones a crash report most needs to name, so any
	// walk whose output a human has to read should refresh first. Deliberately
	// NOT called from the stack sampler's per-sample path: this re-walks the
	// loader's module list, which is the wrong cost to pay hundreds of times a
	// second for a list that changes maybe five times in a session.
	void RefreshModules();

	// The single lock every DbgHelp call in the process must hold. Resolve()
	// takes it internally; callers making their own DbgHelp calls (e.g.
	// crash_reporter's StackWalk64 loop, which calls SymFunctionTableAccess64/
	// SymGetModuleBase64 on top of SymFromAddrW) must wrap their whole
	// sequence of calls in this too.
	std::mutex& GetMutex();

	// Resolves `address` to its containing symbol's bare (demangled) name,
	// or an empty string if DbgHelp has no symbol covering it (no PDB
	// loaded for that module, or the address isn't in any loaded module).
	// outDisplacement, if non-null, receives address - symbolStart -- 0 when
	// address lands exactly on the symbol's start.
	std::string Resolve(uintptr_t address, uintptr_t* outDisplacement = nullptr);
}
