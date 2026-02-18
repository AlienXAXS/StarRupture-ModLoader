#pragma once

#include "plugin_interface.h"
#include "plugin_hooks.h"

// ---------------------------------------------------------------------------
// Hook infrastructure for plugins
// This provides a simple interface that wraps the mod loader's hooking system
// ---------------------------------------------------------------------------

namespace Hooks
{
    // Simple hook wrapper that uses handles instead of direct Hook objects
    class Hook
    {
    private:
    HookHandle m_handle = nullptr;

    public:
    Hook() = default;
        ~Hook() { Remove(); }

        // Prevent copying
        Hook(const Hook&) = delete;
      Hook& operator=(const Hook&) = delete;

        // Install a hook at `target`. Writes a 14-byte absolute JMP (x64).
   // `originalFunc` receives a pointer to a trampoline that calls the
      // original code — cast it to the right function pointer type.
        bool Install(uintptr_t target, void* detour, void** originalFunc)
        {
    if (m_handle)
       {
      return false; // Already installed
            }

          m_handle = PluginHooks::InstallHook(target, detour, originalFunc);
            return m_handle != nullptr;
        }

        // Remove the hook, restoring original bytes.
        void Remove()
        {
  if (m_handle)
  {
         PluginHooks::RemoveHook(m_handle);
      m_handle = nullptr;
            }
    }

   // Check if hook is installed
     bool IsInstalled() const
  {
            return m_handle && PluginHooks::IsHookInstalled(m_handle);
 }
    };
}
