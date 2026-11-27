#pragma once

#include <windows.h>
#include <functional>

// One gate around every call from the loader into plugin code, so that a plugin
// DLL is never unmapped while any such call can be running or about to run.
// A plugin can call another plugin's functions directly, which the loader cannot
// see, so the gate counts calls into all plugins, not into one.
//
// It is a readers-writer lock with a timed writer. The rule for a reader:
// take a Scope BEFORE reading a plugin's callback out of a registry, and keep it
// until the call returns. Taking it only around the call leaves a copied pointer
// uncounted, and the DLL can be freed between the copy and the call. Take it
// before the registry's mutex, never while holding one.
//
// Unloading removes the plugin's registrations first and only then calls
// FreeWhenIdle: a reader that is already in is waited for, and a later one no
// longer finds the pointer.
//
// Whoever waits here must hold no loader lock: a call being waited for may need
// it. In particular the plugin manager releases its lock for every wait.
namespace PluginCallGate
{
	// How long FreeWhenIdle waits for running calls. A call still running after
	// this may be waiting on the thread that is unloading.
	inline constexpr DWORD kWaitTimeoutMs = 1000;

	// A reader. Nests on one thread; only the outermost one can wait, which it
	// does while a DLL is being freed on another thread.
	class Scope
	{
	public:
		Scope();
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	};

	// True while this thread holds a Scope: a plugin may be on its stack.
	bool InsideCall();

	// Waits for the running calls to return and frees nothing. Asked before a
	// plugin is shut down, so that a busy gate refuses with nothing changed.
	bool WaitUntilIdle(DWORD timeoutMs);

	// Frees `module` once no thread holds a Scope. False, with the module left
	// mapped and the reason logged: this thread holds a Scope itself, another
	// free is in progress, or the calls did not return within timeoutMs.
	// whileIdle runs just before the free, with no call running or able to start.
	bool FreeWhenIdle(HMODULE module, const char* name, DWORD timeoutMs = kWaitTimeoutMs,
	                  const std::function<void()>& whileIdle = {});

	// Replaces what FreeWhenIdle runs while the gate is closed, for tests.
	// Null restores the real one (PluginGuard::Untrack, then FreeLibrary).
	using FreeStep = void (*)(HMODULE module);
	void SetFreeStep(FreeStep step);
}
