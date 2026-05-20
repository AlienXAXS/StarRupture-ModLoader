#pragma once

namespace ScanPatterns
{
	// CoreUObject::StaticLoadObject — synchronously loads (or returns the already-loaded instance
	// of) any UObject asset by full path.  Available on all build types.
	// Signature: UObject* StaticLoadObject(UClass*, UObject* Outer, const wchar_t* Name,
	//   const wchar_t* Filename, uint32 LoadFlags, UPackageMap*,
	//   bool bAllowObjectReconciliation, const FLinkerInstancingContext*)
	inline constexpr auto StaticLoadObject =
		"40 55 53 56 41 54 41 55 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 0F B6 85";

#if defined(MODLOADER_CLIENT_BUILD)
	// AHUD::PostRender — called every frame on the game thread; hud->Canvas is valid inside.
	// ACrHUD does not override this, so AHUD::PostRender is the hook target.
	// Signature: void AHUD::PostRender()
	inline constexpr auto AHUD_PostRender =
		"40 55 53 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0";

	// UGameViewportClient::InputKey -- intercepts all keyboard/mouse input before UE5 processes it.
	// Returning false (0) from our detour consumes the event (game will not react to it).
	// Signature: __int64 __fastcall UGameViewportClient::InputKey(
	//               UGameViewportClient* this, const FInputKeyEventArgs* InEventArgs)
	inline constexpr auto UGameViewportClient_InputKey =
		"48 8B C4 55 53 57 41 55 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 48 8B 5A";
#endif

	// UObject::ProcessEvent -- called for every UFUNCTION dispatch in the game.
	// Hooking this once and immediately unlatching after the first call is the
	// most reliable way to detect that GObjects is fully populated (the engine
	// must have processed at least one event before reaching here).
	// Signature: void UObject::ProcessEvent(UObject* this, UFunction* fn, void* params)
	inline constexpr auto UObject_ProcessEvent =
		"40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8D 6C 24 ?? 48 89 9D ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 ?? ?? ?? ?? 8B 59";

	// UGameInstance::Init -- fires after UGameEngine::Init, kept for reference.
	// Signature: void UGameInstance::Init(UGameInstance* this)
	inline constexpr auto UGameInstance_Init =
		"40 55 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 ?? 48 8B 15";

	inline constexpr auto FEngineLoop_Init =
		"4C 8B DC 55 57 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 5B ?? 48 8D 15";

	inline constexpr auto UGameEngine_Init =
		"48 89 5C 24 ?? 48 89 74 24 ?? 55 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 45 33 E4 48 89 4D";

	inline constexpr auto FEngineLoop_Exit =
		"40 53 55 56 57 48 83 EC ?? ?? ?? ?? 48 8B F9 FF 50";

	inline constexpr auto UEngine_PreExit =
		"E8 ?? ?? ?? ?? 48 83 3D ?? ?? ?? ?? ?? 75 ?? 4C 8D 0D ?? ?? ?? ?? 41 B8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 15";

	// AGameModeBase::PostLogin(AGameModeBase *this, struct APlayerController *a2)
	// Native C++ override -- not a UFUNCTION, not in GObjects Children list.
	inline constexpr auto AGameModeBase_PostLogin =
		"40 56 41 56 48 83 EC ?? ?? ?? ?? 48 8B F2 4C 8B F1";

	// AGameModeBase::Logout(AGameModeBase *this, struct AController *a2)
	// Native C++ override -- not a UFUNCTION, not in GObjects Children list.
	inline constexpr auto AGameModeBase_Logout =
		"48 85 D2 0F 84 ?? ?? ?? ?? 55 41 57 48 83 EC ?? 48 8B EA 4C 8B F9 E8 ?? ?? ?? ?? 4C 8B 45 ?? 4C 8D 48 ?? 48 63 40 ?? 41 3B 40 ?? 0F 8F ?? ?? ?? ?? 48 8B D0 49 8B 40 ?? ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 48 89 5C 24";

	// UCrMassSaveSubsystem::OnSaveLoaded(UCrMassSaveSubsystem *this)
	// Native C++ callback -- not a UFUNCTION, not in GObjects Children list.
	inline constexpr auto UCrMassSaveSubsystem_OnSaveLoaded =
		"4C 8B DC 55 57 49 8D AB ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 49 89 5B ?? B8";

	// UCrExperienceManagerComponent::OnExperienceLoadComplete(UCrExperienceManagerComponent *this)
	// Native C++ callback -- not a UFUNCTION, not in GObjects Children list.
	inline constexpr auto UCrExperienceManagerComponent_OnExperienceLoadComplete =
		"48 89 4C 24 ?? 55 53 56 41 54 41 55 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 83 B9";

	// UWorld::BeginPlay() -- engine-level native call, not a UFUNCTION.
	inline constexpr auto UWorld_BeginPlay =
		"48 83 EC ?? 48 89 5C 24 ?? 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B CB";

	// UWorld::EndPlay() -- engine-level native call, not a UFUNCTION.
	inline constexpr auto UWorld_EndPlay =
		"40 55 41 56 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? F6 81";

	// FMassEntityManager::GetArchetypeForEntity(FMassEntityHandle)
	inline constexpr auto FMassEntityManager_GetArchetypeForEntity =
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B FA 49 8B D8 49 8B D0 48 8B F1 E8 ?? ?? ?? ?? 84 C0";

	// UMassSignalSubsystem::SignalEntity(FName SignalName, FMassEntityHandle Entity)
	inline constexpr auto UMassSignalSubsystem_SignalEntity =
		"48 89 5C 24 ?? 4C 89 44 24 ?? 57 48 83 EC ?? 48 8B DA 48 8B F9 45 85 C0";

	// FHttpConnection::ProcessRequest(FHttpConnection* this, TSharedPtr<FHttpServerRequest> request,
	//   TFunction<void(TUniquePtr<FHttpServerResponse>&&)> onComplete)
	// Used only on server builds; defined unconditionally so it compiles on all configs.
	// Confirmed offsets inside FHttpServerRequest (verified experimentally):
	// obj+0   : HTTP verb  FString { wchar_t* Data @+0,  int32 Num @+8  }
	//   obj+16  : RelativePath FString { wchar_t* Data @+16, int32 Num @+24 }
	//   obj+280 : Body TArray<uint8> { uint8* Data @+280, int32 Num @+288 }
	inline constexpr auto FHttpConnection_ProcessRequest =
		"48 89 5C 24 ?? 55 56 41 54 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 83 79";

#if defined(MODLOADER_CLIENT_BUILD)
	inline constexpr const char* UGameEngine_Tick =
		"40 55 53 56 41 54 41 56 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 0F 29 BC 24 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 ?? 0F 29 B4 24";
#elif defined(MODLOADER_SERVER_BUILD)
	// UGameEngine::Tick(float DeltaSeconds, bool bIdleMode)
	inline constexpr auto UGameEngine_Tick =
		"4C 8B DC 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 49 89 5B ?? 49 89 6B ?? 49 89 73 ?? 49 89 7B ?? 4D 89 63 ?? 45 0F B6 E0";
#endif

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

	// FHttpServerResponse::Create(__int64 *retStorage, const TArray<uint8>& body, const FString& contentType)
	// Used to construct a 200 OK response for mod-owned HTTP routes.
	// Confirmed via IDA: FPerfCounters::ProcessStatsRequest calls this with body in RDX, FString in R8.
	inline constexpr auto FHttpServerResponse_Create =
		"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 ?? 48 8B F9 4D 8B E0";
}
