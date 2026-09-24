#pragma once

#ifndef MODLOADER_CLIENT_BUILD

// ---------------------------------------------------------------------------
// Game log category filter (server / generic builds)
//
// A stock dedicated server writes hundreds of LogTemp and LogSkinnedMeshComp
// warnings per second -- FPCGDataCollection::GetSettingsInterface() and
// USkinnedMeshComponent::GetSocketInfoByName() on a weapon actor with no
// skeletal mesh. None of it is actionable by a server operator, and it buries
// everything that is.
//
// Why this is not [Core.Log] or -LogCmds. Neither works on this build:
//
//   - FLogSuppressionImplementation::ProcessConfigAndCommandLine reads only
//     GConfig's [Core.Log] from GEngineIni, and this build stages its whole
//     ini hierarchy into the binary -- the generated Saved\Config Engine.ini
//     is never combined into GEngineIni. Same reason log_verbosity.cpp exists
//     on the client.
//   - That function never touches FCommandLine at all, so -LogCmds is a no-op
//     despite the exe carrying the help text that advertises it.
//     FLogSuppressionImplementation::ProcessCmdString has exactly three
//     callers -- Exec_Runtime, ProcessConfigAndCommandLine and ProcessLogOnly
//     -- and ProcessLogOnly is itself only reachable from Exec_Runtime.
//
// What does work is the engine's own "log" exec command, which survives into
// shipping as FExec::Exec -> FLogSuppressionImplementation::Exec_Runtime (it
// is Exec_Runtime, not the Exec_Dev that shipping compiles out). The dispatch
// chain UGameEngine::Exec -> UEngine::Exec -> StaticExec ->
// FSelfRegisteringExec::StaticExec -> FExec::Exec is intact, and
// Hooks::EngineExec already drives exactly that. So this needs no new AOB and
// no new detour: it is the existing exec route with a config-driven list.
//
// Note the engine models verbosity as one threshold per category, not a mask.
// There is no "errors and info but no warnings" -- turning a category down to
// Error drops its Display/Log lines too. That is why this suppresses the two
// specific broken categories rather than lowering anything globally: every
// other category keeps its warnings.
//
// Config, written to modloader.ini on first run so it is visible and editable:
//
//   [Logging]
//   GameLogCategories=LogTemp Error,LogSkinnedMeshComp Error
//
// Comma-separated "<category> <verbosity>" pairs, exactly the syntax the
// engine's own "log" command takes. An empty value disables the feature.
// Client builds use log_verbosity.cpp's global-verbosity control instead.
// ---------------------------------------------------------------------------

namespace GameLogFilter
{
	// Write GameLogCategories to modloader.ini if the key is absent. Called from
	// InitSubsystems, i.e. during startup rather than from the apply path.
	//
	// Seeding is deliberately separate from applying. Applying needs GEngine, so
	// it cannot happen until the first engine tick -- about seven seconds into a
	// server boot. A key that only appeared then would be missing for the whole
	// of startup and missing entirely from a server that died before ticking,
	// which is exactly when someone goes looking for it. The raw profile API is
	// used throughout, so this is safe this early (InitializeConfigManager is
	// about plugin config, not modloader.ini).
	void EnsureConfigDefaults();

	// Queue the configured rules onto the game thread. Called once from
	// engine-init; the rules land on the first engine tick.
	void Schedule();
}

#endif // !MODLOADER_CLIENT_BUILD
