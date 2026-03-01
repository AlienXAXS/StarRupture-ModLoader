#include "LogisticsFragmentFixer.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "Engine_classes.hpp"
#include "CoreUObject_classes.hpp"
#include "MassEntity_structs.hpp"
#include "MassEntity_classes.hpp"
#include "MassSignals_classes.hpp"
#include "ChimeraMassCommon_classes.hpp"
#include <Windows.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace RailJunctionFixer
{
	uintptr_t* LogisticsFragmentFixer::s_newChain = nullptr;
	uintptr_t  LogisticsFragmentFixer::s_socketsStruct = 0;
	uintptr_t* LogisticsFragmentFixer::s_origChain = nullptr;
	int32_t    LogisticsFragmentFixer::s_origDepth = 0;
	uintptr_t  LogisticsFragmentFixer::s_origSuperStruct = 0;
	void*      LogisticsFragmentFixer::s_socketsFragmentStruct = nullptr;

	bool LogisticsFragmentFixer::PatchHierarchyChain(uintptr_t socketsStruct, uintptr_t savableStruct)
	{
		// Read current chain info for sockets fragment
		int32_t sockDepth = ReadAt<int32_t>(socketsStruct, UStructOff::HierarchyDepth);
		uintptr_t* sockChain = ReadAt<uintptr_t*>(socketsStruct, UStructOff::InheritanceChain);

		// Read savable fragment's info
		int32_t savDepth = ReadAt<int32_t>(savableStruct, UStructOff::HierarchyDepth);
		uintptr_t savIdentity = savableStruct + UStructOff::InheritanceChain;
		uintptr_t sockIdentity = socketsStruct + UStructOff::InheritanceChain;

		LOG_DEBUG("PatchHierarchyChain:");
		LOG_DEBUG("  sockets depth=%d, chain=0x%llX, identity=0x%llX",
			sockDepth, (unsigned long long)(uintptr_t)sockChain,
			(unsigned long long)sockIdentity);
		LOG_DEBUG("  savable depth=%d, identity=0x%llX",
			savDepth, (unsigned long long)savIdentity);

		if (!sockChain || sockDepth < 0 || sockDepth > 30)
		{
			LOG_ERROR("Invalid sockets hierarchy data");
			return false;
		}

		// Verify the chain's self-entry is correct
		if (sockChain[sockDepth] != sockIdentity)
		{
			LOG_ERROR("chain[self_depth] (0x%llX) != self identity (0x%llX)",
				(unsigned long long)sockChain[sockDepth],
				(unsigned long long)sockIdentity);
			return false;
		}

		// Check if savable is already in the chain (patch already applied)
		if (savDepth <= sockDepth && sockChain[savDepth] == savIdentity)
		{
			LOG_INFO("Hierarchy chain already contains CrMassSavableFragment");
			return true;
		}

		// Build new chain: insert savable at index savDepth, shift rest up by 1
		// Old chain: [root, ..., FMassFragment, socketsFragment]  (sockDepth+1 entries)
		// New chain: [root, ..., FMassFragment, savableFragment, socketsFragment]  (sockDepth+2 entries)
		int newSize = sockDepth + 2;
		LOG_DEBUG("Building new chain: %d -> %d entries", sockDepth + 1, newSize);

		// Allocate permanent memory for the new chain
		s_newChain = (uintptr_t*)VirtualAlloc(
			nullptr, newSize * sizeof(uintptr_t),
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!s_newChain)
		{
			LOG_ERROR("VirtualAlloc failed for new chain");
			return false;
		}

		// Copy entries before the insertion point
		for (int i = 0; i < savDepth; ++i)
		{
			s_newChain[i] = sockChain[i];
		}

		// Insert savable fragment's identity at its depth
		s_newChain[savDepth] = savIdentity;

		// Copy remaining entries (shifted by 1), including self
		for (int i = savDepth; i <= sockDepth; ++i)
		{
			s_newChain[i + 1] = sockChain[i];
		}

		// Log the new chain
		for (int i = 0; i < newSize; ++i)
		{
			LOG_DEBUG("  newChain[%d] = 0x%llX%s%s",
				i, (unsigned long long)s_newChain[i],
				(s_newChain[i] == savIdentity) ? " [SAVABLE]" : "",
				(s_newChain[i] == sockIdentity) ? " [SELF]" : "");
		}

		// Save originals for restoration on shutdown
		s_socketsStruct = socketsStruct;
		s_origChain = sockChain;
		s_origDepth = sockDepth;
		s_origSuperStruct = ReadAt<uintptr_t>(socketsStruct, UStructOff::SuperStruct);

		// Apply the patch: update chain pointer, depth, and SuperStruct
		uintptr_t patchStart = socketsStruct + UStructOff::InheritanceChain;
		// Unprotect the entire region from +0x30 to +0x48 (InheritanceChain + HierarchyDepth + SuperStruct)
		DWORD oldProtect;
		if (!VirtualProtect((void*)patchStart, 0x18, PAGE_READWRITE, &oldProtect))
		{
			LOG_ERROR("VirtualProtect failed to unprotect memory");
			return false;
		}

		WriteAt<uintptr_t*>(socketsStruct, UStructOff::InheritanceChain, s_newChain);
		WriteAt<int32_t>(socketsStruct, UStructOff::HierarchyDepth, sockDepth + 1);
		WriteAt<uintptr_t>(socketsStruct, UStructOff::SuperStruct, savableStruct);

		VirtualProtect((void*)patchStart, 0x18, oldProtect, &oldProtect);

		// Verify
		uintptr_t* verifyChain = ReadAt<uintptr_t*>(socketsStruct, UStructOff::InheritanceChain);
		int32_t verifyDepth = ReadAt<int32_t>(socketsStruct, UStructOff::HierarchyDepth);
		uintptr_t verifySuper = ReadAt<uintptr_t>(socketsStruct, UStructOff::SuperStruct);

		bool ok = (verifyChain == s_newChain) &&
			(verifyDepth == sockDepth + 1) &&
			(verifySuper == savableStruct);

		if (ok)
		{
			// Test the IsChildOf logic ourselves
			uintptr_t testEntry = s_newChain[savDepth];
			bool isChildOf = (savDepth <= verifyDepth) && (testEntry == savIdentity);
			LOG_INFO("  IsChildOf(CrMassSavableFragment) = %s", isChildOf ? "TRUE" : "FALSE");
		}
		else
		{
			LOG_ERROR("Verification failed after patching hierarchy chain");
		}

		return ok;
	}

	bool LogisticsFragmentFixer::Initialize()
	{
		LOG_INFO("Initializing LogisticsFragmentFixer...");

		try
		{
			// Verify GObjects is valid before attempting to find objects
			SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
			if (!objArray)
			{
				LOG_ERROR("GObjects pointer is null - SDK may be incompatible with this game build");
				LOG_ERROR("This plugin was built with an SDK from the dedicated server");
				LOG_ERROR("Please disable this plugin when running on client builds");
				return false;
			}

			// Try to validate that GObjects has reasonable values
			int32_t numObjects = objArray->NumElements;
			if (numObjects <= 0 || numObjects > 10000000) // Sanity check
			{
				LOG_ERROR("GObjects has invalid NumElements (%d) - SDK offset mismatch", numObjects);
				LOG_ERROR("This indicates the SDK is incompatible with this game build");
				LOG_ERROR("Please disable this plugin when running on client builds");
				return false;
			}

			LOG_DEBUG("GObjects validation passed (NumElements: %d)", numObjects);

			// Find both UScriptStruct objects via Unreal reflection
			SDK::UScriptStruct* socketsStruct = nullptr;
			SDK::UScriptStruct* savableStruct = nullptr;

			try
			{
				socketsStruct = SDK::UObject::FindObjectFast<SDK::UScriptStruct>(
					"CrLogisticsSocketsFragment", SDK::EClassCastFlags::ScriptStruct);
			}
			catch (...)
			{
				LOG_ERROR("Exception while searching for CrLogisticsSocketsFragment - SDK mismatch");
				return false;
			}

			try
			{
				savableStruct = SDK::UObject::FindObjectFast<SDK::UScriptStruct>(
					"CrMassSavableFragment", SDK::EClassCastFlags::ScriptStruct);
			}
			catch (...)
			{
				LOG_ERROR("Exception while searching for CrMassSavableFragment - SDK mismatch");
				return false;
			}

			if (!socketsStruct)
			{
				LOG_ERROR("Could not find UScriptStruct for CrLogisticsSocketsFragment");
				LOG_ERROR("This may indicate an SDK version mismatch");
				return false;
			}

			if (!savableStruct)
			{
				LOG_ERROR("Could not find UScriptStruct for CrMassSavableFragment");
				LOG_ERROR("This may indicate an SDK version mismatch");
				return false;
			}

			// Log current state
			auto* currentSuper = socketsStruct->SuperStruct;
			LOG_INFO("CrLogisticsSocketsFragment at 0x%llX", reinterpret_cast<uintptr_t>(socketsStruct));
			LOG_INFO("Current SuperStruct at 0x%llX (%s)",
				reinterpret_cast<uintptr_t>(currentSuper),
				currentSuper ? currentSuper->GetName().c_str() : "nullptr");
			LOG_INFO("CrMassSavableFragment at 0x%llX", reinterpret_cast<uintptr_t>(savableStruct));

			// Verify we're patching what we expect (SuperStruct should be FMassFragment)
			if (currentSuper && currentSuper == savableStruct)
			{
				LOG_INFO("Already reparented - nothing to do");
				s_socketsFragmentStruct = static_cast<void*>(socketsStruct);
				return true;
			}

			// Patch the hierarchy chain to fix IsChildOf checks
			bool success = PatchHierarchyChain(
				reinterpret_cast<uintptr_t>(socketsStruct),
				reinterpret_cast<uintptr_t>(savableStruct));

			if (success)
			{
				LOG_INFO("SUCCESS - CrLogisticsSocketsFragment now inherits from CrMassSavableFragment");
				LOG_INFO("       IsChildOf checks will now work correctly for save system");
				s_socketsFragmentStruct = static_cast<void*>(socketsStruct);
				return true;
			}
			else
			{
				LOG_ERROR("Failed to patch hierarchy chain");
				return false;
			}
		}
		catch (const std::exception& e)
		{
			LOG_ERROR("Exception during initialization: %s", e.what());
			return false;
		}
		catch (...)
		{
			LOG_ERROR("Unknown exception during initialization");
			return false;
		}
	}

	void LogisticsFragmentFixer::Shutdown()
	{
		LOG_INFO("Shutting down LogisticsFragmentFixer...");

		// Restore the original UScriptStruct data so the engine doesn't encounter
		// our VirtualAlloc'd chain pointer during its own teardown, which would
		// cause MallocBinned2 corruption when it tries to free non-UE memory.
		if (s_socketsStruct != 0 && s_origChain != nullptr)
		{
			DWORD oldProtect;
			uintptr_t patchStart = s_socketsStruct + UStructOff::InheritanceChain;
			if (VirtualProtect((void*)patchStart, 0x18, PAGE_READWRITE, &oldProtect))
			{
				WriteAt<uintptr_t*>(s_socketsStruct, UStructOff::InheritanceChain, s_origChain);
				WriteAt<int32_t>(s_socketsStruct, UStructOff::HierarchyDepth, s_origDepth);
				WriteAt<uintptr_t>(s_socketsStruct, UStructOff::SuperStruct, s_origSuperStruct);
				VirtualProtect((void*)patchStart, 0x18, oldProtect, &oldProtect);
				LOG_INFO("Original UScriptStruct hierarchy restored");
			}
			else
			{
				LOG_ERROR("VirtualProtect failed during restore - original hierarchy NOT restored");
			}

			s_socketsStruct = 0;
			s_origChain = nullptr;
			s_origDepth = 0;
			s_origSuperStruct = 0;
		}

		if (s_newChain)
		{
			VirtualFree(s_newChain, 0, MEM_RELEASE);
			s_newChain = nullptr;
		}
	}

	// =================================================================
	// SignalSocketEntities -- post-save-load socket re-initialization
	//
	// After a save is loaded, FCrLogisticsSocketRuntimeData.ConnectionEntity
	// contains stale FMassEntityHandle values that were serialized.
	// We signal entities that have CrLogisticsSocketsFragment via
	// UMassSignalSubsystem::SignalEntity to trigger
	// UCrLogisticsSocketsSignalProcessor::Execute, which rebuilds
	// socket data from persistent FCrCustomConnectionData.SocketConnections.
	// =================================================================

	// SignalEntity function signature (native, not a UFunction):
	//   void UMassSignalSubsystem::SignalEntity(FName SignalName, FMassEntityHandle Entity)
	// MSVC x64: rcx=this, rdx=FName (8 bytes by value), r8=FMassEntityHandle (8 bytes by value)
	// Despite UE5 source declaring const FMassEntityHandle&, the compiler passes it
	// by value in register since it fits in 8 bytes (confirmed by IDA disassembly).
	// Found via pattern scan at runtime for future-proofing across game updates.
	using SignalEntityFn = void (__fastcall*)(void* signalSubsystem,
		SDK::FName signalName,
		SDK::FMassEntityHandle handle);

	// GetArchetypeForEntity function signature (native):
	//   FMassArchetypeHandle FMassEntityManager::GetArchetypeForEntity(FMassEntityHandle entity)
	//
	// Disassembly analysis of the pattern:
	//   48 8B FA   mov rdi, rdx     ; rdi = 2nd param (output ptr or entity)
	//   49 8B D8  mov rbx, r8      ; rbx = 3rd param
	//   49 8B D0       mov rdx, r8      ; setup rdx = r8 for IsEntityValid call
	//   48 8B F1       mov rsi, rcx  ; rsi = this (FMassEntityManager*)
	//   E8 ...     call IsEntityValid(this=rcx, entity=r8)
	//
	// In shipping builds, FMassArchetypeHandle may be optimized to a single pointer
	// (TSharedPtr reduced to raw ptr), fitting in rax with no hidden return.
	// However the 3-register usage (rcx, rdx, r8) with rcx=this, rdx=outPtr, r8=entity
	// suggests hidden return pointer convention:
	//   rcx = this (FMassEntityManager*)
	//   rdx = output pointer (FMassArchetypeHandle*)
	//   r8  = FMassEntityHandle (8 bytes by value)
	//
	// The function preserves rcx into rsi (this), rdx into rdi (output),
	// r8 into rbx (entity), then passes this+entity to IsEntityValid.
	// FMassArchetypeHandle in UE5 wraps a TSharedPtr<FMassArchetypeData> which is
	// 16 bytes (pointer + reference controller). The function writes all 16 bytes
	// into the output buffer via the hidden return pointer.
	struct FMassArchetypeHandle
	{
		void* DataPtr;// FMassArchetypeData*
		void* RefController; // TSharedReferencer reference controller
	};

	using GetArchetypeForEntityFn = void (__fastcall*)(void* thisEntityManager,
		FMassArchetypeHandle* outResult,
		SDK::FMassEntityHandle entity);

	// Pattern for FMassEntityManager::GetArchetypeForEntity
	static constexpr const char* PATTERN_GetArchetypeForEntity =
		"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B FA 49 8B D8 49 8B D0 48 8B F1 E8 ?? ?? ?? ?? 84 C0";

	// Pattern for UMassSignalSubsystem::SignalEntity
	static constexpr const char* PATTERN_SignalEntity =
		"48 89 5C 24 ?? 4C 89 44 24 ?? 57 48 83 EC ?? 48 8B DA 48 8B F9 45 85 C0";

	// Check whether an archetype's data contains a specific UScriptStruct* pointer.
	// FMassArchetypeData stores fragment types in TArray<FMassArchetypeFragmentConfig>
	// where each entry starts with a UScriptStruct*. We scan a reasonable region of
	// the archetype object's memory for the pointer value.
	static bool ArchetypeContainsFragment(void* archetypeData, void* fragmentStruct)
	{
		if (!archetypeData || !fragmentStruct)
			return false;

		// Scan the first 4KB of the archetype data for the UScriptStruct* pointer.
		// FMassArchetypeData is typically a few hundred bytes; 4KB is generous but safe.
		constexpr size_t SCAN_SIZE = 4096;
		auto base = reinterpret_cast<uintptr_t>(archetypeData);
		auto target = reinterpret_cast<uintptr_t>(fragmentStruct);

		__try
		{
			// Step through pointer-aligned slots looking for our UScriptStruct*
			for (size_t offset = 0; offset + sizeof(uintptr_t) <= SCAN_SIZE; offset += sizeof(uintptr_t))
			{
				uintptr_t value = *reinterpret_cast<uintptr_t*>(base + offset);
				if (value == target)
					return true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			// Hit unmapped memory -- stop scanning
		}

		return false;
	}

	void LogisticsFragmentFixer::SignalSocketEntities()
	{
		LOG_INFO("SignalSocketEntities: Re-initializing logistics sockets after save load...");

		// ---- 1. Get UWorld via SDK ----
		SDK::UWorld* world = SDK::UWorld::GetWorld();
		if (!world)
		{
			LOG_ERROR("  UWorld::GetWorld() returned null - cannot signal entities");
			return;
		}
		LOG_INFO("  UWorld at %p ('%s')", static_cast<void*>(world), world->GetName().c_str());

		// ---- 2. Resolve native function pointers ----
		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto moduleBase = reinterpret_cast<uintptr_t>(mainModule);

		// SignalEntity -- found via pattern scan for future-proofing
		SignalEntityFn fnSignalEntity = nullptr;
		auto* scanner = GetScanner();
		if (scanner)
		{
			uintptr_t addr = scanner->FindPatternInMainModule(PATTERN_SignalEntity);
			if (addr != 0)
			{
				fnSignalEntity = reinterpret_cast<SignalEntityFn>(addr);
				LOG_INFO("  SignalEntity at 0x%llX (base + 0x%llX) -- pattern scan OK",
					static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - moduleBase));
			}
			else
			{
				LOG_ERROR("  SignalEntity pattern not found -- cannot signal entities");
				return;
			}
		}
		else
		{
			LOG_ERROR("  Scanner not available -- cannot resolve SignalEntity");
			return;
		}

		auto* config = GetConfig();

		// GetArchetypeForEntity -- found via pattern scan for future-proofing
		GetArchetypeForEntityFn fnGetArchetype = nullptr;
		if (scanner)
		{
			uintptr_t addr = scanner->FindPatternInMainModule(PATTERN_GetArchetypeForEntity);
			if (addr != 0)
			{
				fnGetArchetype = reinterpret_cast<GetArchetypeForEntityFn>(addr);
				LOG_INFO("  GetArchetypeForEntity at 0x%llX (base + 0x%llX) -- pattern scan OK",
					static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - moduleBase));
			}
			else
			{
				LOG_WARN("  GetArchetypeForEntity pattern not found -- entity filtering DISABLED");
			}
		}

		// ---- 3. Discover signal name via SDK StringToName ----
		SDK::FName socketSignalName = {};

		std::string signalNameStr = "CrLogisticsSocketsSignal";
		if (config)
		{
			char buf[256] = {};
			config->ReadString("RailJunctionFixer", "Advanced", "SocketSignalName",
				buf, sizeof(buf), signalNameStr.c_str());
			signalNameStr = buf;
		}

		try
		{
			std::wstring wSignalName(signalNameStr.begin(), signalNameStr.end());
			SDK::FName constructed = SDK::BasicFilesImpleUtils::StringToName(wSignalName.c_str());
			if (constructed.ComparisonIndex != 0)
			{
				socketSignalName = constructed;
				LOG_INFO("  Signal FName '%s': CompIdx=0x%X",
					signalNameStr.c_str(), socketSignalName.ComparisonIndex);
			}
			else
			{
				LOG_ERROR("  StringToName('%s') returned ComparisonIndex=0 - name not registered",
					signalNameStr.c_str());
				return;
			}
		}
		catch (...)
		{
			LOG_ERROR("  Exception during StringToName('%s') - aborting", signalNameStr.c_str());
			return;
		}

		// ---- 4. Find UMassEntitySubsystem for entity filtering ----
		// UMassEntitySubsystem wraps FMassEntityManager. At offset 0x38 it has a
		// TSharedPtr<FMassEntityManager> -- the raw pointer to the entity manager.
		void* entityManager = nullptr;
		if (fnGetArchetype && s_socketsFragmentStruct)
		{
			try
			{
				SDK::UClass* entitySubClass = SDK::UMassEntitySubsystem::StaticClass();
				if (entitySubClass)
				{
					SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
					if (objArray)
					{
						for (int32_t i = 0; i < objArray->NumElements; ++i)
						{
							SDK::UObject* obj = objArray->GetByIndex(i);
							if (!obj || !obj->Class) continue;
							if (obj->Outer != world) continue;

							if (obj->IsA(entitySubClass))
							{
								// UMassEntitySubsystem at +0x38 has TSharedPtr<FMassEntityManager>
								// TSharedPtr layout: [T* Object (8 bytes), FReferenceControllerBase* RefController (8 bytes)]
								// The raw FMassEntityManager* is at +0x38
								auto subsystemAddr = reinterpret_cast<uintptr_t>(obj);

								// Read the raw pointer from TSharedPtr
								void* sharedPtrObj = ReadAt<void*>(subsystemAddr, 0x38);
								LOG_INFO("  UMassEntitySubsystem at %p", static_cast<void*>(obj));
								LOG_DEBUG("    +0x38 = %p (TSharedPtr.Object)", sharedPtrObj);
								LOG_DEBUG("    +0x40 = %p (TSharedPtr.RefController)", ReadAt<void*>(subsystemAddr, 0x40));

								if (sharedPtrObj)
								{
									entityManager = sharedPtrObj;
									LOG_INFO("  FMassEntityManager at %p", entityManager);
								}
								else
								{
									// TSharedPtr not yet initialized -- try reading through
									// UMassSubsystemBase which may store the manager differently.
									// Scan the subsystem's memory for a non-null pointer that
									// looks like a heap allocation (heuristic).
									LOG_WARN("  TSharedPtr<FMassEntityManager> at +0x38 is null");
									LOG_WARN("  Dumping UMassEntitySubsystem memory for diagnosis:");
									for (size_t off = 0x30; off < 0x48; off += 8)
									{
										LOG_WARN("    +0x%02zX = 0x%016llX", off,
											static_cast<unsigned long long>(ReadAt<uintptr_t>(subsystemAddr, off)));
									}
								}
								break;
							}
						}
					}
				}
			}
			catch (...)
			{
				LOG_ERROR("  Exception while searching for UMassEntitySubsystem");
				entityManager = nullptr;
			}

			if (!entityManager)
			{
				LOG_WARN("  FMassEntityManager not found -- entity filtering DISABLED, will signal all entities");
				fnGetArchetype = nullptr;
			}
		}
		else if (fnGetArchetype && !s_socketsFragmentStruct)
		{
			LOG_WARN("  CrLogisticsSocketsFragment UScriptStruct not saved -- entity filtering DISABLED");
			fnGetArchetype = nullptr;
		}

		// ---- 5. Find entity handles via CrMassPersistentIDSubsystem ----
		SDK::UMassSignalSubsystem* signalSubsystem = nullptr;
		std::vector<SDK::FMassEntityHandle> handles;

		SDK::UCrMassPersistentIDSubsystem* persistentIDSubsystem = nullptr;
		try
		{
			SDK::UClass* persistentIDClass = SDK::UCrMassPersistentIDSubsystem::StaticClass();
			if (persistentIDClass)
			{
				SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
				if (objArray)
				{
					for (int32_t i = 0; i < objArray->NumElements; ++i)
					{
						SDK::UObject* obj = objArray->GetByIndex(i);
						if (!obj || !obj->Class) continue;
						if (obj->Outer != world) continue;

						if (obj->IsA(persistentIDClass))
						{
							persistentIDSubsystem = static_cast<SDK::UCrMassPersistentIDSubsystem*>(obj);
							LOG_INFO("  UCrMassPersistentIDSubsystem at %p (Outer=World)",
								static_cast<void*>(persistentIDSubsystem));
							break;
						}
					}
				}
			}
		}
		catch (...)
		{
			LOG_ERROR("  Exception while searching for UCrMassPersistentIDSubsystem");
		}

		if (persistentIDSubsystem)
		{
			try
			{
				auto& idHandleMap = persistentIDSubsystem->IDHandleMap;
				int mapCount = idHandleMap.Num();
				LOG_INFO("  IDHandleMap contains %d entries", mapCount);

				for (auto it = begin(idHandleMap); it != end(idHandleMap); ++it)
				{
					SDK::FMassEntityHandle handle = it->Value();
					if (handle.SerialNumber > 0)
					{
						handles.push_back(handle);
					}
				}

				LOG_INFO("  Collected %zu valid entity handles from IDHandleMap", handles.size());
			}
			catch (...)
			{
				LOG_ERROR("  Exception while iterating IDHandleMap");
			}
		}
		else
		{
			LOG_WARN("  UCrMassPersistentIDSubsystem not found - trying CrMassActorComponent fallback...");

			SDK::UClass* massActorCompClass = SDK::UCrMassActorComponent::StaticClass();
			if (massActorCompClass)
			{
				try
				{
					SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
					if (objArray)
					{
						for (int32_t i = 0; i < objArray->NumElements; ++i)
						{
							SDK::UObject* obj = objArray->GetByIndex(i);
							if (!obj || !obj->Class) continue;
							if (!obj->IsA(massActorCompClass)) continue;

							auto* comp = static_cast<SDK::UCrMassActorComponent*>(obj);
							if (!comp->bInitialized) continue;

							if (!signalSubsystem && comp->SignalSubsystem)
							{
								signalSubsystem = comp->SignalSubsystem;
								LOG_INFO("  UMassSignalSubsystem at %p (from CrMassActorComponent)",
									static_cast<void*>(signalSubsystem));
							}

							SDK::FMassEntityHandle handle = comp->EntityHandle;
							if (handle.SerialNumber > 0)
							{
								handles.push_back(handle);
							}
						}
					}
				}
				catch (...)
				{
					LOG_ERROR("  Exception while iterating CrMassActorComponent instances");
				}
			}
		}

		// ---- 6. Find signal subsystem ----
		if (!signalSubsystem)
		{
			try
			{
				SDK::UClass* signalDelegateClass = SDK::UCrMassSignalDelegateSubsystem::StaticClass();
				if (signalDelegateClass)
				{
					SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
					if (objArray)
					{
						for (int32_t i = 0; i < objArray->NumElements; ++i)
						{
							SDK::UObject* obj = objArray->GetByIndex(i);
							if (!obj || !obj->Class) continue;
							if (obj->Outer != world) continue;

							if (obj->IsA(signalDelegateClass))
							{
								auto* delegateSub = static_cast<SDK::UCrMassSignalDelegateSubsystem*>(obj);
								if (delegateSub->SignalSubsystem)
								{
									signalSubsystem = delegateSub->SignalSubsystem;
									LOG_INFO("  UMassSignalSubsystem at %p (from CrMassSignalDelegateSubsystem)",
										static_cast<void*>(signalSubsystem));
								}
								break;
							}
						}
					}
				}
			}
			catch (...)
			{
				LOG_ERROR("  Exception while searching for CrMassSignalDelegateSubsystem");
			}
		}

		if (!signalSubsystem)
		{
			LOG_WARN("  No SignalSubsystem from delegate subsystem, searching via world...");
			try
			{
				SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
				for (int32_t i = 0; i < objArray->NumElements; ++i)
				{
					SDK::UObject* obj = objArray->GetByIndex(i);
					if (!obj || !obj->Class) continue;
					if (obj->Outer != world) continue;

					if (obj->IsA(SDK::UMassSignalSubsystem::StaticClass()))
					{
						signalSubsystem = static_cast<SDK::UMassSignalSubsystem*>(obj);
						LOG_INFO("  UMassSignalSubsystem at %p (Outer=World)",
							static_cast<void*>(signalSubsystem));
						break;
					}
				}
			}
			catch (...)
			{
				LOG_ERROR("  Exception while searching for UMassSignalSubsystem");
				return;
			}
		}

		if (!signalSubsystem)
		{
			LOG_ERROR("  UMassSignalSubsystem not found - cannot signal entities");
			return;
		}

		LOG_INFO("  Found %zu entity handles total from persistent ID map", handles.size());

		// ---- 7. Filter and signal entities ----
		if (handles.empty())
		{
			LOG_WARN("  No entity handles found - no persistent entities exist");
			LOG_WARN("  This may be normal if no buildings exist yet");
			LOG_INFO("SignalSocketEntities: complete");
			return;
		}

		if (fnGetArchetype && entityManager && s_socketsFragmentStruct)
		{
			// Archetype-based filtering: entities sharing the same archetype either
			// ALL have CrLogisticsSocketsFragment or NONE do. We call GetArchetypeForEntity
			// per entity, cache the result per archetype pointer, and check each unique
			// archetype once by scanning its memory for our UScriptStruct*.
			std::vector<SDK::FMassEntityHandle> socketHandles;
			socketHandles.reserve(handles.size() / 10);

			// Cache: archetype pointer -> has fragment
			std::unordered_map<uintptr_t, bool> archetypeCache;
			size_t checkedCount = 0;
			size_t errorCount = 0;

			for (const auto& handle : handles)
			{
				try
				{
					FMassArchetypeHandle result = {};
					fnGetArchetype(entityManager, &result, handle);
					void* archetype = result.DataPtr;
					if (!archetype)
					{
						++checkedCount;
						continue;
					}

					auto archetypeKey = reinterpret_cast<uintptr_t>(archetype);
					auto cacheIt = archetypeCache.find(archetypeKey);
					bool hasFragment;

					if (cacheIt != archetypeCache.end())
					{
						hasFragment = cacheIt->second;
					}
					else
					{
						hasFragment = ArchetypeContainsFragment(archetype, s_socketsFragmentStruct);
						archetypeCache[archetypeKey] = hasFragment;
					}

					if (hasFragment)
					{
						socketHandles.push_back(handle);
					}
					++checkedCount;
				}
				catch (...)
				{
					++errorCount;
					if (errorCount == 1)
					{
						LOG_ERROR("  Exception during GetArchetypeForEntity for entity [%d,%d] - "
							"pattern match may be incorrect, disabling filtering",
							handle.Index, handle.SerialNumber);
					}
					if (errorCount >= 3)
					{
						LOG_ERROR("  Multiple exceptions during archetype check -- "
							"falling back to signaling all %zu entities", handles.size());
						socketHandles = std::move(handles);
						break;
					}
				}
			}

			if (errorCount < 3)
			{
				LOG_INFO("  Archetype filter: %zu / %zu entities have CrLogisticsSocketsFragment "
					"(%zu unique archetypes, checked %zu, errors %zu)",
					socketHandles.size(), handles.size(), archetypeCache.size(),
					checkedCount, errorCount);
			}

			if (!socketHandles.empty())
			{
				LOG_INFO("  Signaling %zu socket entities with '%s'...",
					socketHandles.size(), signalNameStr.c_str());

				size_t signalCount = 0;
				for (const auto& handle : socketHandles)
				{
					LOG_DEBUG("  -> SignalEntity [%d,%d] (idx=%d, serial=%d)",
						handle.Index, handle.SerialNumber,
						handle.Index, handle.SerialNumber);

					fnSignalEntity(static_cast<void*>(signalSubsystem),
						socketSignalName, handle);

					++signalCount;

					if (signalCount % 100 == 0)
					{
						LOG_INFO("  ... signaled %zu / %zu entities so far",
							signalCount, socketHandles.size());
					}
				}

				LOG_INFO("  Socket signal sent to %zu entities", signalCount);
			}
			else
			{
				LOG_WARN("  No entities with CrLogisticsSocketsFragment found");
				LOG_WARN("  This may be normal if no buildings with logistics sockets exist yet");
			}
		}
		else
		{
			// No filtering -- signal all persistent entities.
			// The signal processor's archetype query will naturally skip entities
			// that don't have CrLogisticsSocketsFragment, so this is functionally
			// correct but sends unnecessary signals.
			if (!fnGetArchetype)
			{
				LOG_INFO("  Entity filtering not available (GetArchetypeForEntity pattern not found)");
			}
			LOG_INFO("  Signaling ALL %zu persistent entities with '%s' (signal processor will filter by archetype)...",
				handles.size(), signalNameStr.c_str());

			for (const auto& handle : handles)
			{
				fnSignalEntity(static_cast<void*>(signalSubsystem),
					socketSignalName, handle);
			}

			LOG_INFO("  Socket signal sent to %zu entities (unfiltered)", handles.size());
		}

		LOG_INFO("SignalSocketEntities: complete");
	}
}
