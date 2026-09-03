#pragma once

namespace ScanPatterns
{
	// FText::AsLocalizable_Advanced -- builds a localizable FText from a namespace/key/source string.
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan.
	// Signature: FText* __fastcall FText::AsLocalizable_Advanced(FText* result,
	//               const FTextKey* Namespace, const FTextKey* Key, const wchar_t* String)
	inline constexpr auto FText_AsLocalizable_Advanced =
		"48 89 5C 24 ?? 56 57 41 56 48 83 EC ?? 45 33 D2";

	// FTextKey::FTextKey -- builds an interned FTextKey from a wide string.
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan.
	// Required to construct the Namespace/Key arguments for AsLocalizable_Advanced.
	// Signature: void __fastcall FTextKey::FTextKey(FTextKey* this, const wchar_t* InStr)
	inline constexpr auto FTextKey_FTextKey =
		"40 53 48 83 EC ?? 33 C0 48 89 54 24 ?? 48 8B D9 48 85 D2 74 ?? ?? ?? ?? 74 ?? 66 0F 1F 44 00 ?? 48 8D 52 ?? FF C0 ?? ?? ?? ?? 75 ?? 89 44 24 ?? 48 8D 54 24 ?? 0F 28 44 24 ?? 66 0F 7F 44 24 ?? E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 ?? 5B C3 ?? ?? 48 89 5C 24";

	// CoreUObject::StaticLoadObject — synchronously loads (or returns the already-loaded instance
	// of) any UObject asset by full path.  Available on all build types.
	// Signature: UObject* StaticLoadObject(UClass*, UObject* Outer, const wchar_t* Name,
	//   const wchar_t* Filename, uint32 LoadFlags, UPackageMap*,
	//   bool bAllowObjectReconciliation, const FLinkerInstancingContext*)
	inline constexpr auto StaticLoadObject =
		"40 55 53 56 41 54 41 55 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 0F B6 85";

	// AActor::InternalGetNetMode — returns the net mode (Standalone/DedicatedServer/
	// ListenServer/Client) for the actor's owning world. Native C++ function --
	// not a UFUNCTION, must be found via AOB scan and called directly (no hook).
	// Available on all build types.
	// Signature: ENetMode __fastcall AActor::InternalGetNetMode(AActor* this)
	inline constexpr auto AActor_InternalGetNetMode =
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 8B D9 E8 ?? ?? ?? ?? 48 8B 9B";

	// UClass::FindFunctionByName -- resolves a function by FName on a class
	// (and its supers if IncludeSuper). This is the path every multicast
	// delegate Broadcast() uses to turn an FScriptDelegate's FunctionName back
	// into a callable UFunction*. Native C++ function -- not a UFUNCTION,
	// must be found via AOB scan. Available on all build types.
	// Signature: UFunction* __fastcall UClass::FindFunctionByName(UClass* this,
	//               FName InName, EIncludeSuperFlag::Type IncludeSuper)
	inline constexpr auto UClass_FindFunctionByName =
		"48 89 54 24 ?? 55 53 57 41 54 41 55 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 4C 8B 79";

#if defined(MODLOADER_CLIENT_BUILD)
	// AHUD::PostRender — called every frame on the game thread; hud->Canvas is valid inside.
	// ACrHUD does not override this, so AHUD::PostRender is the hook target.
	// Signature: void AHUD::PostRender()
	inline constexpr auto AHUD_PostRender =
		"40 55 53 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0";

	// UCrMapManuSubsystem::GatherPlayersData(UCrMapManuSubsystem* this)
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan.
	//
	// The prologue alone (up to the first call) is NOT unique: as of Update 2,
	// UBiomesRuntimeSpawningSubsystem::Initialize compiles to the identical
	// frame setup + call to UAnimationSharingManager::GetWorld, and sits at a
	// LOWER address, so a first-match scan resolved to it instead. The extra
	// bytes below cover what follows that call:
	//   GatherPlayersData:  mov rsi, [rax+1B0h] / test rsi, rsi   (World->GameState)
	//   Initialize:         mov rsi, rax        / test rax, rax
	// The +1B0h displacement is wildcarded so a UWorld layout change does not
	// break the scan.
	inline constexpr auto UCrMapManuSubsystem_GatherPlayersData =
		"40 55 53 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B D9 E8 ?? ?? ?? ?? 48 8B B0";

	// UGameViewportClient::InputKey -- intercepts all keyboard/mouse input before UE5 processes it.
	// Returning false (0) from our detour consumes the event (game will not react to it).
	// Signature: __int64 __fastcall UGameViewportClient::InputKey(
	//               UGameViewportClient* this, const FInputKeyEventArgs* InEventArgs)
	inline constexpr auto UGameViewportClient_InputKey =
		"48 8B C4 55 53 57 41 55 48 8D 68 ?? 48 81 EC ?? ?? ?? ?? 48 8B 5A";

	// anonymous_namespace_::ReportCrashUsingCrashReportClient(FWindowsPlatformCrashContext* InContext,
	//   _EXCEPTION_POINTERS* ExceptionInfo, EErrorReportUI ReportUI)
	// Spawns CrashReportClient.exe and uploads the crash report to the developers. Also called
	// (from different call sites) for non-fatal ensure() failures and hangs -- see
	// HandleCrashInternal_FatalReportCallSite below for how the crash_reporter hook tells those
	// apart from a real fatal crash.
	inline constexpr auto ReportCrashUsingCrashReportClient =
		"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 45 33 E4 48 89 55 ?? 66 44 39 25";

	// The specific call site inside FCrashReportingThread::HandleCrashInternal that invokes
	// ReportCrashUsingCrashReportClient for a real fatal engine crash:
	//   cmp cs:GUseCrashReportClient, r13b
	//   lea rcx, [rsp+...]
	//   jz  short loc_...
	//   mov rdx, [r14+28h]   ; ExceptionInfo
	//   xor r8d, r8d         ; ReportUI
	//   call ReportCrashUsingCrashReportClient
	// ReportContinuableEventUsingCrashReportClient (ensure() failures) and ReportHang (hang
	// detection) call the same function from different call sites elsewhere in the module, so
	// matching this exact sequence -- rather than hooking ReportCrashUsingCrashReportClient
	// unconditionally -- lets the hook only intervene on real fatal crashes.
	// Match length is 26 bytes; the return address for this call site is (matchAddress + 26).
	inline constexpr auto HandleCrashInternal_FatalReportCallSite =
		"44 38 2D ?? ?? ?? ?? 48 8D 4C 24 30 74 0E 49 8B 56 28 45 33 C0 E8 ?? ?? ?? ??";
	inline constexpr int HandleCrashInternal_FatalReportCallSite_Length = 26;

	// APlayerController::ConsoleCommand -- runs a console command and returns the engine's
	// output. Used by the ModLoader's own developer console; the game's built-in UConsole
	// cannot be revived on this build because its input and render dispatch are compiled
	// out (UGameViewportClient::InputKey has no ViewportConsole access, PostRender_Console
	// has zero callers, and nothing reads UInputSettings::ConsoleKeys).
	// Returns FString by value, so MSVC x64 puts a hidden return pointer in RDX:
	// Signature: FString* __fastcall APlayerController::ConsoleCommand(APlayerController* this,
	//               FString* outResult, const FString* Command, bool bWriteToLog)
	inline constexpr auto APlayerController_ConsoleCommand =
		"40 53 48 83 EC 20 48 8B 89 ?? ?? ?? ?? 48 8B DA 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3 48 8D 15";
	// ULineBatchComponent::DrawLines -- appends a batch of FBatchedLine into the
	// component's BatchedLines array (offset 0x530) and marks the render state dirty.
	// This is the only in-world line-drawing entry point that survived Shipping:
	// ENABLE_DRAW_DEBUG is 0, so every UKismetSystemLibrary::DrawDebug* body compiled
	// away to nothing and DrawDebugHelpers.cpp's free functions are absent entirely,
	// but the batchers themselves are alive -- UWorld::UpdateWorldComponents NewObject's
	// all four of UWorld::LineBatchers[4] and UGameViewportClient::Draw flushes the
	// per-frame pair every frame.
	// MSVC x64 passes the 16-byte TArrayView indirectly, so RDX is a pointer to it
	// (the function reads the element count from [rdx+8]).
	// Signature: void __fastcall ULineBatchComponent::DrawLines(ULineBatchComponent* this,
	//               const TArrayView<FBatchedLine, int32>* InLines)
	inline constexpr auto ULineBatchComponent_DrawLines =
		"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 63 5A 08 48 8B F2 48 8B F9 85 DB 79 ?? 4C 8D 0D ?? ?? ?? ?? 48 89 5C 24 20";

	// ULineBatchComponent::Flush -- empties BatchedLines/Points/Meshes and marks the
	// render state dirty. Used to clear the two persistent batchers, which is what the
	// engine's own FLUSHPERSISTENTDEBUGLINES exec handler does (it flushes
	// LineBatchers[1] and LineBatchers[3]).
	// Signature: void __fastcall ULineBatchComponent::Flush(ULineBatchComponent* this)
	inline constexpr auto ULineBatchComponent_Flush =
		"40 53 48 83 EC 30 83 B9 ?? ?? ?? ?? 00 48 8B D9 7F ?? 83 B9 ?? ?? ?? ?? 00 7F ?? 83 B9 ?? ?? ?? ?? 00 0F 8E";

	// FLogSuppressionImplementation::ApplyGlobalChanges -- pushes the static "Global"
	// FLogCategoryBase's verbosity out to every registered log category, clamping each to
	// its own compile-time verbosity. This is the machinery behind the engine's
	// "Log global <verbosity>" console command; calling it directly avoids having to build
	// an FString or reach the console.
	// Also the source of the Global verbosity byte: the function's first
	//   44 0F B6 15 <rel32>   movzx r10d, byte ptr [rip+rel32]
	// resolves to it (Verbosity is at offset 0 of FLogCategoryBase in this build).
	// Signature: void __fastcall FLogSuppressionImplementation::ApplyGlobalChanges(void* this)
	inline constexpr auto FLogSuppression_ApplyGlobalChanges =
		"48 89 4C 24 ?? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC F8 00 00 00 65 48 8B 04 25";

	// FLogSuppressionInterface::Get -- returns the FLogSuppressionImplementation singleton,
	// lazily constructing it. Usable directly as the `this` for ApplyGlobalChanges.
	// The signature is unusually long because the TLS-guarded lazy-singleton prologue is
	// shared verbatim by many other getters in the binary -- a shorter prefix is ambiguous
	// and would resolve to the wrong function.
	// Signature: void* __fastcall FLogSuppressionInterface::Get()
	inline constexpr auto FLogSuppression_Get =
		"48 83 EC 28 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 ?? ?? ?? ?? BA B0 17 00 00 48 8B 04 C8 8B 04 02 39 05 ?? ?? ?? ?? 7F ?? 48 8B 05 ?? ?? ?? ?? 48 85 C0 75 ?? 4C 8D 0D ?? ?? ?? ?? 44 8D 40 ?? 48 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 90 CC 48 8B 05 ?? ?? ?? ?? 48 83 C4 28 C3";

	// FLogSuppressionImplementation::ProcessConfigAndCommandLine -- applies [Core.Log] and
	// -LogCmds during FEngineLoop::PreInit. Anything set before this runs is overwritten,
	// so the verbosity override is (re)applied from a detour on return.
	// Signature: void __fastcall FLogSuppressionImplementation::ProcessConfigAndCommandLine(void* this)
	inline constexpr auto FLogSuppression_ProcessConfigAndCommandLine =
		"48 89 5C 24 ?? 55 57 41 54 41 55 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC C0 01 00 00";
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
	// This one IS a UFUNCTION, so the hook normally resolves ExecFunction via
	// GObjects and never scans; this pattern is the fallback for the native
	// member, and matches the one-argument member, not the exec thunk.
	inline constexpr auto UCrMassSaveSubsystem_OnSaveLoaded =
		"48 89 5C 24 ?? 48 89 74 24 ?? 55 57 41 54 41 55 41 56 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 41 BC";

	// UCrExperienceManagerComponent::OnExperienceLoadComplete(UCrExperienceManagerComponent *this)
	// Native C++ callback -- not a UFUNCTION, not in GObjects Children list.
	inline constexpr auto UCrExperienceManagerComponent_OnExperienceLoadComplete =
		"48 89 4C 24 ?? 55 53 56 41 54 41 55 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 83 B9";

	// AActor::BeginPlay() -- engine-level native call, not resolvable by UFUNCTION name.
	inline constexpr auto AActor_BeginPlay =
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 0F B6 41";

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
		"40 53 48 83 EC ?? 88 54 24 ?? 48 8B D9 48 8B 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? ?? ?? ?? 4C 8D 44 24 ?? 48 8B D0 48 8B CB 41 FF 91 ?? ?? ?? ?? 48 83 C4 ?? 5B C3 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 89 5C 24 ?? 57 48 83 EC ?? 48 8B F9 48 85 C9";

	// AMassSpawner::DoSpawning(AMassSpawner* this)
	// TODO: fill pattern via IDA/x64dbg — leave empty until found (hook will no-op gracefully)
	inline constexpr auto AMassSpawner_DoSpawning =
		"40 55 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 83 B9 ?? ?? ?? ?? ?? 48 8B F9 75";

	// UObject* __fastcall StaticFindObject(UClass* Class, FTopLevelAssetPath* ObjectPath, bool ExactClass)
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan and called directly (no hook).
	inline constexpr auto StaticFindObject_ByPath =
		"48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? ?? ?? ?? ?? 41 0F B6 F0 48 8B FA";

	// UObject* __fastcall StaticFindObject(UClass* ObjectClass, UObject* InObjectPackage, const wchar_t* OrigInName, bool bExactClass)
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan and called directly (no hook).
	inline constexpr auto StaticFindObject_ByName =
		"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 48 83 FA";

	// UObject* __fastcall StaticFindObjectSafe(UClass* Class, FTopLevelAssetPath* ObjectPath, bool ExactClass)
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan and called directly (no hook).
	inline constexpr auto StaticFindObjectSafe_ByPath =
		"40 53 55 56 48 83 EC ?? 48 8B E9 41 0F B6 F0";

	// UObject* __fastcall StaticFindObjectSafe(UClass* ObjectClass, UObject* ObjectParent, const wchar_t* InName, bool bExactClass)
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan and called directly (no hook).
	inline constexpr auto StaticFindObjectSafe_ByName =
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B E9 41 0F B6 D9 33 C9 49 8B F8";

	// UObject* __fastcall StaticFindObjectFast(UClass* ObjectClass, UObject* ObjectPackage, FName ObjectName,
	//   bool bExactClass, EObjectFlags ExclusiveFlags, EInternalObjectFlags ExclusiveInternalFlags)
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan and called directly (no hook).
	inline constexpr auto StaticFindObjectFast =
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B E9 41 0F B6 F9 33 C9 49 8B D8 48 8B F2 E8 ?? ?? ?? ?? 84 C0 74";

	// UPackage* __fastcall FindPackage(UObject* InOuter, const wchar_t* PackageName)
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan and called directly (no hook).
	inline constexpr auto FindPackage =
		"48 89 5C 24 ?? 55 56 57 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 ?? ?? ?? ?? 48 8D 44 24 ?? 48 89 4C 24";

	// void __fastcall UPackage::FullyLoad(UPackage* this)
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan and called directly (no hook).
	inline constexpr auto UPackage_FullyLoad =
		"48 89 5C 24 ?? 57 48 83 EC ?? 48 83 79 ?? ?? 48 8D 3D ?? ?? ?? ?? 48 8B D9 0F 84";

	// UPackage* __fastcall LoadPackage(UPackage* InOuter, FScriptContainerElement* InLongPackageNameOrFilename,
	//   unsigned int LoadFlags, FArchive* InReaderOverride, const FLinkerInstancingContext* InstancingContext)
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan and called directly (no hook).
	inline constexpr auto LoadPackage =
		"48 89 5C 24 ?? 48 89 74 24 ?? 48 89 7C 24 ?? 55 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 45 33 ED 4D 8B E1";

	// UObject* __fastcall FAssetData::FastGetAsset(FAssetData* this, bool bLoad,
	//   TMap<FName,FName,FDefaultSetAllocator,TDefaultMapHashableKeyFuncs<FName,FName,0>>* LoadTags)
	// Native C++ function -- not a UFUNCTION, must be found via AOB scan and called directly (no hook).
	inline constexpr auto FAssetData_FastGetAsset =
		"48 89 5C 24 ?? 55 56 57 41 56 41 57 48 81 EC ?? ?? ?? ?? ?? ?? ?? 49 8B F8";

	// ACrCrafter::NativeOnItemCraftingComplete(ACrCrafter* this, FMassEntityHandle EntityHandle, FName SignalName)
	// Native C++ signal handler -- not the BlueprintEvent. Plays crafting-complete sounds
	// and then calls ACrCrafter::OnItemCraftingComplete(this) (the BlueprintEvent, which has
	// no usable native ExecFunction). This is the real per-craft completion entry point.
#if defined(MODLOADER_CLIENT_BUILD)
	inline constexpr auto ACrCrafter_NativeOnItemCraftingComplete =
		"40 56 48 81 EC ?? ?? ?? ?? 48 8B F1 E8 ?? ?? ?? ?? 83 F8";
#elif defined(MODLOADER_SERVER_BUILD)
	inline constexpr auto ACrCrafter_NativeOnItemCraftingComplete =
		"40 53 56 41 57 48 81 EC ?? ?? ?? ?? 48 8B 99";
#endif
	     
	// FHttpServerResponse::Create(__int64 *retStorage, const TArray<uint8>& body, const FString& contentType)
	// Used to construct a 200 OK response for mod-owned HTTP routes.
	// Confirmed via IDA: FPerfCounters::ProcessStatsRequest calls this with body in RDX, FString in R8.
	inline constexpr auto FHttpServerResponse_Create =
		"48 89 5C 24 ?? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 ?? 48 8B F9 4D 8B E0";

	// -----------------------------------------------------------------------
	// Preflight registry
	//
	// Every pattern above that the modloader itself resolves on the current
	// build target MUST have an entry here. The preflight verifier
	// (memory_scanner/pattern_preflight.*) scans all of them at startup,
	// before any hook is installed; if any *required* pattern is missing the
	// modloader logs every failure and disables itself entirely (no hooks,
	// no plugins) so the game never runs partially hooked.
	//
	// required = false marks patterns whose absence is not fatal: either
	// reference-only patterns that no modloader code currently resolves, or
	// optional capabilities that degrade on their own (the debug-draw entry
	// points below just make in-world drawing unavailable to plugins). Both
	// are still scanned and logged as warnings, but neither disables the
	// modloader when missing.
	//
	// When adding a new pattern, add it to this table with the same
	// MODLOADER_CLIENT_BUILD / MODLOADER_SERVER_BUILD gating as its
	// definition and its usage site.
	// -----------------------------------------------------------------------
	// -----------------------------------------------------------------------
	// Control-channel wire  (client + server)
	//
	// Native primitives for sending/receiving modloader payloads directly on the
	// Unreal per-connection control channel, replacing the UFUNCTION-envelope
	// transport. See network_channel/CONTROL_CHANNEL_WIRE.md for the reverse
	// engineering and hooks/game/control_channel/ for the consumer.
	//
	// All six are required together, and all six are fatal at preflight: this is
	// the only transport plugin networking has. control_channel.cpp still reports
	// IsAvailable() false if one is missing, but that is now a belt-and-braces
	// runtime guard rather than a supported degraded mode -- preflight stops the
	// loader before it gets that far.
	//
	// These six are confirmed IDENTICAL in the client
	// (StarRuptureGameSteam-Win64-Shipping) and server
	// (StarRuptureServerEOS-Win64-Shipping) binaries, so there is deliberately no
	// per-build variant. Stack-frame sizes are wildcarded (48 83 EC ??) because
	// that is the byte most likely to shift between builds.
	// -----------------------------------------------------------------------

	// void __fastcall UControlChannel::ReceivedBunch(UControlChannel* this, FInBunch& Bunch)
	// Hook target: the virtual that dispatches control messages by leading uint8.
	inline constexpr auto UControlChannel_ReceivedBunch =
		"40 55 56 57 41 54 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 ?? ?? ?? ?? 33 F6";

	// FPacketIdRange* __fastcall UControlChannel::SendBunch(UControlChannel* this,
	//     FPacketIdRange* result, FOutBunch* Bunch, bool bMerge)
	// NB: FPacketIdRange (8 bytes) is returned via a HIDDEN POINTER in RDX, so
	// Bunch is the THIRD argument (R8), not the second. Confirmed both by IDA's
	// argument analysis and by this signature's own bytes: 49 8B F0 (mov rsi, r8)
	// takes the bunch from R8, 48 8B FA (mov rdi, rdx) takes the return slot.
	inline constexpr auto UControlChannel_SendBunch =
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 8B 41 ?? 49 8B F0 48 8B FA 48 8B D9 85 C0";

	// void __fastcall FControlChannelOutBunch::FControlChannelOutBunch(
	//     FControlChannelOutBunch* this, UChannel* InChannel, bool bClose)
	// Internally constructs the FOutBunch base and marks it reliable.
	inline constexpr auto FControlChannelOutBunch_ctor =
		"40 53 48 83 EC ?? 48 8B D9 E8 ?? ?? ?? ?? 80 8B ?? ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? ?? ?? ?? 48 8B C3";

	// void __fastcall FOutBunch::~FOutBunch(FOutBunch* this) -- frees buffer allocations.
	// Matched directly at its entry point rather than through a call site.
	// The prefix it shares with other destructors is not unique, so this pattern
	// necessarily runs past the function's own 0x53 bytes, through the inter-function
	// padding, and into the first bytes of the next function (48 89 5C 24). That tail
	// is the fragile part: if this is the one that stops resolving after a game
	// patch, shorten it from the right before suspecting anything else.
	inline constexpr auto FOutBunch_dtor =
		"40 53 48 83 EC ?? 48 8D 05 ?? ?? ?? ?? 48 8B D9 ?? ?? ?? 48 8B 89 ?? ?? ?? ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B 8B ?? ?? ?? ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B 8B ?? ?? ?? ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 8B CB 48 83 C4 ?? 5B E9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 48 89 5C 24";

	// void __fastcall FBitWriter::Serialize(FBitWriter* this, void* src, int64 numBYTES)
	inline constexpr auto FBitWriter_Serialize =
		"48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B D9 4A 8D 3C C5";

	// void __fastcall FBitReader::SerializeBits(FBitReader* this, void* dest, int64 numBITS)
	// Deliberately SerializeBits, not FBitReader::Serialize -- both exist with nearly
	// identical mangled names and both take an int64, but they differ in UNIT.
	// The wire code passes bits.
	inline constexpr auto FBitReader_SerializeBits =
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? F6 41 ?? ?? 49 8B F8 48 8B F2 48 8B D9 0F 85";

	struct PreflightEntry
	{
		const char* name;
		const char* pattern;
		bool        required;
	};

	inline constexpr PreflightEntry PreflightRegistry[] =
	{
		{ "FText::AsLocalizable_Advanced",             FText_AsLocalizable_Advanced,             true },
		{ "FTextKey::FTextKey",                        FTextKey_FTextKey,                        true },
		{ "StaticLoadObject",                          StaticLoadObject,                         true },
		{ "UClass::FindFunctionByName",                UClass_FindFunctionByName,                true },
		{ "UObject::ProcessEvent",                     UObject_ProcessEvent,                     true },
		{ "FEngineLoop::Init",                         FEngineLoop_Init,                         true },
		{ "UGameEngine::Init",                         UGameEngine_Init,                         true },
		{ "FEngineLoop::Exit",                         FEngineLoop_Exit,                         true },
		{ "UEngine::PreExit",                          UEngine_PreExit,                          true },
		{ "AGameModeBase::PostLogin",                  AGameModeBase_PostLogin,                  true },
		{ "AGameModeBase::Logout",                     AGameModeBase_Logout,                     true },
		{ "UCrMassSaveSubsystem::OnSaveLoaded",        UCrMassSaveSubsystem_OnSaveLoaded,        true },
		{ "UCrExperienceManagerComponent::OnExperienceLoadComplete",
		                                               UCrExperienceManagerComponent_OnExperienceLoadComplete, true },
		{ "AActor::BeginPlay",                         AActor_BeginPlay,                         true },
		{ "UWorld::BeginPlay",                         UWorld_BeginPlay,                         true },
		{ "UWorld::EndPlay",                           UWorld_EndPlay,                           true },
		{ "AAbstractMassEnemySpawner::ActivateSpawner",   AAbstractMassEnemySpawner_ActivateSpawner,   true },
		{ "AAbstractMassEnemySpawner::DeactivateSpawner", AAbstractMassEnemySpawner_DeactivateSpawner, true },
		{ "AMassSpawner::DoSpawning",                  AMassSpawner_DoSpawning,                  true },
		{ "ACrCrafter::NativeOnItemCraftingComplete",  ACrCrafter_NativeOnItemCraftingComplete,  true },
		{ "StaticFindObject (ByPath)",                 StaticFindObject_ByPath,                  true },
		{ "StaticFindObject (ByName)",                 StaticFindObject_ByName,                  true },
		{ "StaticFindObjectSafe (ByPath)",             StaticFindObjectSafe_ByPath,              true },
		{ "StaticFindObjectSafe (ByName)",             StaticFindObjectSafe_ByName,              true },
		{ "StaticFindObjectFast",                      StaticFindObjectFast,                     true },
		{ "FindPackage",                               FindPackage,                              true },
		{ "UPackage::FullyLoad",                       UPackage_FullyLoad,                       true },
		{ "LoadPackage",                               LoadPackage,                              true },
		{ "FAssetData::FastGetAsset",                  FAssetData_FastGetAsset,                  true },

#if defined(MODLOADER_CLIENT_BUILD) || defined(MODLOADER_SERVER_BUILD)
		{ "AActor::InternalGetNetMode",                AActor_InternalGetNetMode,                true },
		{ "UGameEngine::Tick",                         UGameEngine_Tick,                         true },
		// Control-channel wire -- all required. These carry every plugin packet
		// and there is no second transport to fall back to, so losing one does
		// not degrade plugin networking, it ends it. Failing preflight loudly is
		// better than booting into a session where networked plugins are quietly
		// inert; and if these moved, the game updated and every other pattern
		// here is suspect too.
		{ "UControlChannel::ReceivedBunch",            UControlChannel_ReceivedBunch,            true },
		{ "UControlChannel::SendBunch",                UControlChannel_SendBunch,                true },
		{ "FControlChannelOutBunch::ctor",             FControlChannelOutBunch_ctor,             true },
		{ "FOutBunch::~FOutBunch",                     FOutBunch_dtor,                           true },
		{ "FBitWriter::Serialize",                     FBitWriter_Serialize,                     true },
		{ "FBitReader::SerializeBits",                 FBitReader_SerializeBits,                 true },
#endif

#if defined(MODLOADER_CLIENT_BUILD)
		{ "AHUD::PostRender",                          AHUD_PostRender,                          true },
		{ "UCrMapManuSubsystem::GatherPlayersData",    UCrMapManuSubsystem_GatherPlayersData,    true },
		{ "UGameViewportClient::InputKey",             UGameViewportClient_InputKey,             true },
		{ "ReportCrashUsingCrashReportClient",         ReportCrashUsingCrashReportClient,        true },
		{ "HandleCrashInternal_FatalReportCallSite",   HandleCrashInternal_FatalReportCallSite,  true },
		{ "APlayerController::ConsoleCommand",         APlayerController_ConsoleCommand,         true },
		{ "FLogSuppressionImplementation::ApplyGlobalChanges",           FLogSuppression_ApplyGlobalChanges,           true },
		{ "FLogSuppressionInterface::Get",                               FLogSuppression_Get,                          true },
		{ "FLogSuppressionImplementation::ProcessConfigAndCommandLine",  FLogSuppression_ProcessConfigAndCommandLine,  true },
		// Optional -- only in-world debug drawing (hooks->HUD->DebugDraw) is lost
		// if these ever stop resolving, so they must not disable the modloader.
		{ "ULineBatchComponent::DrawLines",            ULineBatchComponent_DrawLines,            false },
		{ "ULineBatchComponent::Flush",                ULineBatchComponent_Flush,                false },
#endif

#if defined(MODLOADER_SERVER_BUILD)
		{ "FHttpConnection::ProcessRequest",           FHttpConnection_ProcessRequest,           true },
		{ "FHttpServerResponse::Create",               FHttpServerResponse_Create,               true },
#endif

		// Reference-only patterns -- kept in the header but not currently
		// resolved by any modloader code. Scanned and logged, never fatal.
		{ "UGameInstance::Init",                       UGameInstance_Init,                       false },
		{ "FMassEntityManager::GetArchetypeForEntity", FMassEntityManager_GetArchetypeForEntity, false },
		{ "UMassSignalSubsystem::SignalEntity",        UMassSignalSubsystem_SignalEntity,        false },
	};
}
