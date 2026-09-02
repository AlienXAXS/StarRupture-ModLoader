#pragma once

// Stage 1: the hooks the engine would run past before we could install them
// (engine init/shutdown, log verbosity, crash reporter, localization, ...).
void InstallAllHooks();

// Stage 3, between LoadPluginsPhase() and InitPluginsPhase(): every remaining
// plugin-facing event hook, so a plugin registering from PluginInit is only
// ever appending to a hook that is already live. See the comment on the
// definition for why the lazy install-on-first-registration path is not enough.
void InstallPluginEventHooks();

void RemoveAllHooks();
