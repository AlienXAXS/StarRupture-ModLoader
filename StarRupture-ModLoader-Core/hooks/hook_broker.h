#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Hooks::Broker -- one detour per address, however many owners want it.
//
// Before this existed, two Hook objects installed on the same address silently
// corrupted each other. The second Install read the first one's JMP stub as if
// it were the function prologue: it decoded the 6-byte `jmp qword [rip+0]`,
// then kept decoding straight into the 8-byte ADDRESS LITERAL behind it,
// treating a pointer as instructions. Those bytes were copied into the second
// trampoline and run through the rel32 relocation pass, and the copied
// rip-relative jump then read the 8 bytes after itself in the trampoline
// rather than the first detour's address. Calling "the original" jumped into
// whatever that happened to be.
//
// Removal was worse. Whoever removed first restored the true prologue over the
// other's JMP, silently unhooking it; whoever removed second wrote the FIRST
// one's stub back, resurrecting a jump to a detour whose module may already be
// gone.
//
// So the loader owns the address now, and callers own links on it.
//
// ---------------------------------------------------------------------------
// Chain, not broadcast
// ---------------------------------------------------------------------------
//
//   target JMP -> headThunk -> L1.detour -> L1.original (thunk1)
//                                        -> L2.detour -> L2.original (thunk2)
//                                                     -> trampoline -> real code
//
// The loader cannot fan one call out to N subscribers here, and it is worth
// being precise about why: for the typed hooks in hooks/game/<event>/ the
// loader WROTE the detour, so it knows the signature and can call each
// registered callback with the arguments. For an arbitrary AOB a plugin found,
// it has no idea what the signature is. What it can own is the plumbing --
// the prologue patch, the order, and the teardown -- while each link calls the
// next one through its own `original`, which is the ordinary detour idiom and
// needs no type knowledge.
//
// Consequence worth knowing: a link that declines to call its original cuts off
// every link behind it. That is inherent to chaining, and it is the same deal
// as an existing Before* callback returning false.
//
// ---------------------------------------------------------------------------
// Identity is the ADDRESS, not the pattern
// ---------------------------------------------------------------------------
//
// Two plugins hooking the same function will almost never share an AOB -- each
// author writes their own. Keying a chain on the pattern (or its hash) would
// file those as two unrelated hooks and put us straight back to two detours on
// one address. The pattern survives as a label on the link, which is what makes
// the diagnostics readable; it is not the identity.
//
// ---------------------------------------------------------------------------
// Why every `original` is a thunk
// ---------------------------------------------------------------------------
//
// A link's `original` is a 16-byte loader-owned stub (`jmp qword [rip+2]` plus
// an 8-byte slot), never the next detour's address directly. By the time a link
// needs rewiring -- because the link behind it was removed -- the owner has
// long since copied its `original` into a static of its own, and we cannot
// reach into a plugin to rewrite that. The indirection is the only thing that
// makes removal in any order possible.
//
// New links splice at the TAIL, so first-installed runs first (which is what
// PreloadInfo::priority is documented to mean). It also means game code is
// written exactly TWICE in a target's whole life: once when the first link
// arrives, once when the last one leaves. Every splice in between is an 8-byte
// write to an 8-byte-aligned slot in a loader thunk, which is atomic on x64 --
// a thread mid-call reads either the old next-link or the new one, and both are
// valid. That is why the thunk is laid out with two bytes of padding: at a
// 16-byte-aligned base, `jmp qword [rip+2]` puts the slot at +8.
//
// Thunks and trampolines are never freed. A thread can be executing inside one
// at the moment its link is removed, and 16 bytes is a much better price than a
// race nobody can reproduce.
// ---------------------------------------------------------------------------

namespace Hooks::Broker
{
	// Opaque handle to one link. Zero is "not attached".
	struct LinkId
	{
		uint64_t value = 0;
		bool Valid() const { return value != 0; }
	};

	// Splice `detour` onto target's chain, at the tail.
	//
	// The first link for a target installs the real prologue patch and builds
	// the trampoline; later links only rewrite a thunk slot. outOriginal always
	// receives a stable thunk -- call it exactly as you would have called the
	// original function.
	//
	// owner/name are recorded for diagnostics only ("ModA" / "FEngineLoop::PreInit").
	// Both may be null.
	LinkId Attach(uintptr_t target, void* detour, void** outOriginal,
	              const char* owner, const char* name);

	// Remove one link, wherever it sits in the chain. The prologue is restored
	// only when the last link goes -- which is the whole point: a plugin
	// unhooking does not unhook everyone else.
	bool Detach(LinkId id);

	bool IsHooked(uintptr_t target);
	int  GetLinkCount(uintptr_t target);

	// --- Diagnostics ---------------------------------------------------------

	struct LinkInfo
	{
		std::string owner;
		std::string name;
		uintptr_t   detour = 0;
	};

	struct ChainInfo
	{
		uintptr_t             target = 0;
		uintptr_t             trampoline = 0;
		std::vector<LinkInfo> links;   // call order: links[0] runs first
	};

	// Every live chain, for the console. Values, not pointers: chains change
	// under the lock and every caller is on another thread.
	std::vector<ChainInfo> Snapshot();

	// --- What the image looked like before we touched it ---------------------
	//
	// One entry per hooked address: where we patched, and the bytes that used to
	// be there.
	//
	// This exists for the pattern scanner. A plugin's AOB comes from the binary
	// as SHIPPED -- that is what the author's disassembler showed them -- but by
	// the time the plugin scans, the loader has stamped a 14-byte JMP over the
	// entry of ProcessEvent, BeginPlay, Tick and two dozen others, which are
	// exactly the functions a mod wants. Without this the scanner answers a
	// different question than the one the author asked, and it fails in both
	// directions: a pattern anchored on a hooked entry stops matching, and a
	// pattern containing FF 25 00 00 00 00 starts matching our own stubs.
	//
	// Snapshot semantics, taken under the lock. A hook installed while a scan is
	// running is not reflected, which is fine -- the loader installs on one
	// thread with the game held.
	struct PatchedRange
	{
		uintptr_t            address = 0;
		std::vector<uint8_t> originalBytes;
	};

	std::vector<PatchedRange> GetPatchedRanges();
}
