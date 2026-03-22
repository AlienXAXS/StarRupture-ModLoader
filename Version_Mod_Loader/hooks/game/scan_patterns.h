#pragma once

namespace ScanPatterns
{
#if defined(MODLOADER_CLIENT_BUILD)
#elif defined(MODLOADER_SERVER_BUILD)
#endif
	inline constexpr auto UWorld_BeginPlay =
		"48 83 EC ?? 48 89 5C 24 ?? 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B CB";

	inline constexpr auto FEngineLoop_Init =
		"4C 8B DC 55 57 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 5B ?? 48 8D 15";

	inline constexpr auto UGameEngine_Init =
		"48 89 5C 24 ?? 48 89 74 24 ?? 55 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 45 33 E4 48 89 4D";

	inline constexpr auto FEngineLoop_Exit =
		"40 53 55 56 57 48 83 EC ?? ?? ?? ?? 48 8B F9 FF 50";

	inline constexpr auto UEngine_PreExit =
		"E8 ?? ?? ?? ?? 48 83 3D ?? ?? ?? ?? ?? 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 15";

	// UCrMassSaveSubsystem::OnSaveLoaded(UCrMassSaveSubsystem *this)
	inline constexpr auto UCrMassSaveSubsystem_OnSaveLoaded =
		"4C 8B DC 55 57 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 5B ?? B8";

	// UCrExperienceManagerComponent::OnExperienceLoadComplete(UCrExperienceManagerComponent *this)
	inline constexpr auto UCrExperienceManagerComponent_OnExperienceLoadComplete =
		"48 89 4C 24 ?? 55 53 56 41 54 41 55 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 83 B9";

	// FMassEntityManager::GetArchetypeForEntity(FMassEntityHandle)
	inline constexpr auto FMassEntityManager_GetArchetypeForEntity =
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B FA 49 8B D8 49 8B D0 48 8B F1 E8 ?? ?? ?? ?? 84 C0";

	// UMassSignalSubsystem::SignalEntity(FName SignalName, FMassEntityHandle Entity)
	inline constexpr auto UMassSignalSubsystem_SignalEntity =
		"48 89 5C 24 ?? 4C 89 44 24 ?? 57 48 83 EC ?? 48 8B DA 48 8B F9 45 85 C0";

#if defined(MODLOADER_CLIENT_BUILD)
	inline constexpr const char* UGameEngine_Tick =
		"40 55 53 56 41 54 41 56 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 0F 29 BC 24 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 ?? 0F 29 B4 24";
#elif defined(MODLOADER_SERVER_BUILD)
	// UGameEngine::Tick(float DeltaSeconds, bool bIdleMode)
	inline constexpr auto UGameEngine_Tick =
		"4C 8B DC 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 49 89 5B ?? 49 89 6B ?? 49 89 73 ?? 49 89 7B ?? 4D 89 63 ?? 45 0F B6 E0";
#endif

	// AActor::BeginPlay (v1 pattern, function prologue - fires at the very start of the function before any instructions)
	inline constexpr auto AActor_BeginPlay =
		"48 8B 07 48 8B CF F3 0F 10 4F 64 FF 90 00 04 00 00 45 33 C0 B2 01 48 8B CF E8 ?? ?? ?? ?? 33 F6";

	// ACrGameModeBase::PostLogin(ACrGameModeBase* this, APlayerController* NewPlayer)
	inline constexpr auto ACrGameModeBase_PostLogin =
		"48 8B C4 53 56 57 48 83 EC ?? 48 89 68 ?? 48 8B F2";

	// ACrGameModeBase::Logout(ACrGameModeBase* this, AController* Exiting)
	inline constexpr auto ACrGameModeBase_Logout =
		"48 8B C4 55 56 57 48 81 EC ?? ?? ?? ?? 33 F6";

	// AAbstractMassEnemySpawner::ActivateSpawner(AAbstractMassEnemySpawner* this, bool bDisableAggroLock)
	// This is actually ?EnableSpawning@AMegaMachineMassEnemySpawner@@UEAAXXZ with a offset to find ActivateSpawner
	inline constexpr auto AAbstractMassEnemySpawner_ActivateSpawner =
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 8B 81 ?? ?? ?? ?? 48 8B D9 33 D2";
	inline constexpr int AAbstractMassEnemySpawner_ActivateSpawner_Offset =
		0x3E;

	// AAbstractMassEnemySpawner::DeactivateSpawner(AAbstractMassEnemySpawner* this, bool bPermanently)
	// TODO: fill pattern via IDA/x64dbg — leave empty until found (hook will no-op gracefully)
	inline constexpr auto AAbstractMassEnemySpawner_DeactivateSpawner =
		"40 53 48 83 EC ?? 88 54 24 ?? 48 8B D9 48 8B 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? ?? ?? ?? 4C 8D 44 24 ?? 48 8B D0 48 8B CB 41 FF 91 ?? ?? ?? ?? 48 83 C4 ?? 5B C3 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 8B C4";

	// AMassSpawner::DoSpawning(AMassSpawner* this)
	// TODO: fill pattern via IDA/x64dbg — leave empty until found (hook will no-op gracefully)
	inline constexpr auto AMassSpawner_DoSpawning =
		"40 55 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 83 B9 ?? ?? ?? ?? ?? 48 8B F9 75";
}
