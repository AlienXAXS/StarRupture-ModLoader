#include "LogisticsFragmentFixer.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "Engine_classes.hpp"
#include "CoreUObject_classes.hpp"
#include "MassEntity_structs.hpp"
#include "MassSignals_classes.hpp"
#include <Windows.h>
#include <string>

namespace RailJunctionFixer
{
	uintptr_t* LogisticsFragmentFixer::s_newChain      = nullptr;
	uintptr_t  LogisticsFragmentFixer::s_socketsStruct  = 0;
	uintptr_t* LogisticsFragmentFixer::s_origChain      = nullptr;
	int32_t    LogisticsFragmentFixer::s_origDepth      = 0;
	uintptr_t  LogisticsFragmentFixer::s_origSuperStruct = 0;

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
		s_socketsStruct   = socketsStruct;
		s_origChain       = sockChain;
		s_origDepth       = sockDepth;
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
			// Check if we can safely access GObjects without crashing
			SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
			if (!objArray)
			{
				LOG_ERROR("GObjects pointer is null - SDK may be incompatible with this game build");
				LOG_ERROR("This plugin was built with an SDK from the dedicated server");
				LOG_ERROR("Please disable this plugin when running on client builds");
				return false;
			}

			// Try to validate that GObjects has reasonable values
			UC::int32 numObjects = objArray->NumElements;
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
				return true;
			}

			// Patch the hierarchy chain to fix IsChildOf checks
			bool success = PatchHierarchyChain(
				reinterpret_cast<uintptr_t>(socketsStruct),
				reinterpret_cast<uintptr_t>(savableStruct));

			if (success)
			{
				LOG_INFO("SUCCESS - CrLogisticsSocketsFragment now inherits from CrMassSavableFragment");
				LOG_INFO("         IsChildOf checks will now work correctly for save system");
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
				WriteAt<int32_t>   (s_socketsStruct, UStructOff::HierarchyDepth,   s_origDepth);
				WriteAt<uintptr_t> (s_socketsStruct, UStructOff::SuperStruct,      s_origSuperStruct);
				VirtualProtect((void*)patchStart, 0x18, oldProtect, &oldProtect);
				LOG_INFO("Original UScriptStruct hierarchy restored");
			}
			else
			{
				LOG_ERROR("VirtualProtect failed during restore - original hierarchy NOT restored");
			}

			s_socketsStruct   = 0;
			s_origChain       = nullptr;
			s_origDepth       = 0;
			s_origSuperStruct = 0;
		}

		if (s_newChain)
		{
			VirtualFree(s_newChain, 0, MEM_RELEASE);
			s_newChain = nullptr;
		}
	}

	// =================================================================
	// SignalSocketEntities — post-save-load socket re-initialization
	//
	// After a save is loaded, FCrLogisticsSocketRuntimeData.ConnectionEntity
	// contains stale FMassEntityHandle values that were serialized.
	// We signal all entities via UMassSignalSubsystem::SignalEntity to
	// trigger UCrLogisticsSocketsSignalProcessor::Execute, which rebuilds
	// socket data from persistent FCrCustomConnectionData.SocketConnections.
	// =================================================================

	// SignalEntity function signature:
	//   void UMassSignalSubsystem::SignalEntity(FName signalName, FMassEntityHandle handle)
	using SignalEntityFn = void (*)(void* signalSubsystem,
	                                SDK::FName signalName,
	                                SDK::FMassEntityHandle handle);

	// Offset within UCrLogisticsSocketsSignalProcessor where the subscribed
	// signal FName is stored (discovered from InitializeInternal disassembly)
	static constexpr size_t SIGNAL_PROCESSOR_SIGNAL_OFFSET = 0x288;

	void LogisticsFragmentFixer::SignalSocketEntities()
	{
		LOG_INFO("SignalSocketEntities: Re-initializing logistics sockets after save load...");

		// ---- 1. Resolve SignalEntity function pointer ----
		HMODULE mainModule = GetModuleHandleW(nullptr);
		auto moduleBase = reinterpret_cast<uintptr_t>(mainModule);

		// Read RVA from config, default to PDB-verified address
		uintptr_t signalEntityRVA = 0x65F1BB0;
		auto* config = GetConfig();
		if (config)
		{
			auto configRVA = static_cast<uintptr_t>(
				config->ReadInt("RailJunctionFixer", "Advanced", "SignalEntity_RVA", 0));
			if (configRVA != 0)
				signalEntityRVA = configRVA;
		}

		auto fnSignalEntity = reinterpret_cast<SignalEntityFn>(moduleBase + signalEntityRVA);
		LOG_DEBUG("  SignalEntity at 0x%llX (base + 0x%llX)",
			static_cast<unsigned long long>(moduleBase + signalEntityRVA),
			static_cast<unsigned long long>(signalEntityRVA));

		// ---- 2. Find UMassSignalSubsystem instance ----
		SDK::UObject* signalSubsystem = nullptr;
		try
		{
			// Walk GObjects to find the live instance (not CDO)
			SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
			if (!objArray)
			{
				LOG_ERROR("  GObjects is null");
				return;
			}

			for (int32_t i = 0; i < objArray->NumElements; ++i)
			{
				SDK::UObject* obj = objArray->GetByIndex(i);
				if (!obj || !obj->Class) continue;

				std::string className = obj->Class->GetName();
				if (className == "MassSignalSubsystem")
				{
					// Skip the CDO — we need the live world instance
					if (!obj->IsDefaultObject())
					{
						signalSubsystem = obj;
						break;
					}
				}
			}
		}
		catch (...)
		{
			LOG_ERROR("  Exception while searching for MassSignalSubsystem");
			return;
		}

		if (!signalSubsystem)
		{
			LOG_ERROR("  UMassSignalSubsystem instance not found");
			return;
		}
		LOG_INFO("  UMassSignalSubsystem at %p", static_cast<void*>(signalSubsystem));

		// ---- 3. Discover signal name ----
		SDK::FName socketSignalName = {};

		// Try reading from CrLogisticsSocketsSignalProcessor CDO
		try
		{
			SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
			for (int32_t i = 0; i < objArray->NumElements; ++i)
			{
				SDK::UObject* obj = objArray->GetByIndex(i);
				if (!obj || !obj->Class) continue;

				std::string className = obj->Class->GetName();
				if (className == "CrLogisticsSocketsSignalProcessor")
				{
					// Read FName at the signal offset
					auto processorAddr = reinterpret_cast<uintptr_t>(obj);
					socketSignalName = ReadAt<SDK::FName>(processorAddr, SIGNAL_PROCESSOR_SIGNAL_OFFSET);

					if (socketSignalName.ComparisonIndex != 0)
					{
						LOG_INFO("  Signal name from CDO+0x%zX: CompIdx=0x%X",
							SIGNAL_PROCESSOR_SIGNAL_OFFSET,
							socketSignalName.ComparisonIndex);
						break;
					}
				}
			}
		}
		catch (...)
		{
			LOG_WARN("  Exception while discovering signal name from CDO");
		}

		// Fallback: find the FName by looking for an object named after the signal
		if (socketSignalName.ComparisonIndex == 0)
		{
			LOG_WARN("  CDO signal discovery failed, searching for FName by string...");

			// Read fallback name from config
			std::string fallbackName = "CrLogisticsSocketsSignal";
			if (config)
			{
				char buf[256] = {};
				config->ReadString("RailJunctionFixer", "Advanced", "SocketSignalName",
					fallbackName.c_str(), buf, sizeof(buf));
				fallbackName = buf;
			}

			// Search GObjects for any FName matching the signal name
			try
			{
				SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
				for (int32_t i = 0; i < objArray->NumElements; ++i)
				{
					SDK::UObject* obj = objArray->GetByIndex(i);
					if (!obj) continue;

					if (obj->GetName() == fallbackName)
					{
						socketSignalName = obj->Name;
						LOG_INFO("  Resolved signal FName '%s': CompIdx=0x%X",
							fallbackName.c_str(), socketSignalName.ComparisonIndex);
						break;
					}
				}
			}
			catch (...)
			{
				LOG_ERROR("  Exception during FName search");
			}
		}

		if (socketSignalName.ComparisonIndex == 0)
		{
			LOG_ERROR("  Could not resolve socket signal FName - aborting");
			return;
		}

		// ---- 4. Find UMassEntitySubsystem and iterate entity handles ----
		SDK::UObject* entitySubsystem = nullptr;
		try
		{
			SDK::TUObjectArray* objArray = SDK::UObject::GObjects.GetTypedPtr();
			for (int32_t i = 0; i < objArray->NumElements; ++i)
			{
				SDK::UObject* obj = objArray->GetByIndex(i);
				if (!obj || !obj->Class) continue;

				std::string className = obj->Class->GetName();
				if (className == "MassEntitySubsystem" && !obj->IsDefaultObject())
				{
					entitySubsystem = obj;
					break;
				}
			}
		}
		catch (...)
		{
			LOG_ERROR("  Exception while searching for MassEntitySubsystem");
			return;
		}

		if (!entitySubsystem)
		{
			LOG_ERROR("  UMassEntitySubsystem not found");
			return;
		}
		LOG_INFO("  UMassEntitySubsystem at %p", static_cast<void*>(entitySubsystem));

		// Scan the entity subsystem's memory for the entity manager's sparse array.
		// We look for a TSparseArray pattern: pointer + reasonable Num + Max >= Num.
		// Each element contains a serial number (int32) at offset 0 and an archetype
		// pointer at offset 8.
		auto subsysAddr = reinterpret_cast<uintptr_t>(entitySubsystem);
		constexpr int MAX_HANDLES = 100000;

		auto* handles = static_cast<SDK::FMassEntityHandle*>(
			VirtualAlloc(nullptr, MAX_HANDLES * sizeof(SDK::FMassEntityHandle),
				MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (!handles)
		{
			LOG_ERROR("  Failed to allocate handle buffer");
			return;
		}

		int handleCount = 0;

		for (size_t off = 0x30; off < 0x400 && handleCount == 0; off += 8)
		{
			uintptr_t arrayPtr = ReadAt<uintptr_t>(subsysAddr, off);
			if (arrayPtr == 0 || arrayPtr < 0x10000) continue;

			int32_t num = ReadAt<int32_t>(subsysAddr, off + 0x08);
			int32_t max = ReadAt<int32_t>(subsysAddr, off + 0x0C);

			if (num < 100 || num > 200000 || max < num || max > 400000)
				continue;

			// Validate: try element sizes 16, 24, 32
			for (int elemSize = 16; elemSize <= 32; elemSize += 8)
			{
				int validCount = 0;
				int sampleSize = (num < 20) ? num : 20;

				for (int i = 0; i < sampleSize; ++i)
				{
					uintptr_t elemAddr = arrayPtr + static_cast<uintptr_t>(i) * elemSize;
					int32_t serial = ReadAt<int32_t>(elemAddr, 0);
					uintptr_t archetype = ReadAt<uintptr_t>(elemAddr, 8);

					if (serial > 0 && serial < 10000 &&
						archetype > 0x10000 && archetype < 0x7FFFFFFFFFFF)
					{
						validCount++;
					}
				}

				if (validCount >= sampleSize / 2)
				{
					LOG_INFO("  Found entity array at subsys+0x%zX: num=%d, elemSize=%d",
						off, num, elemSize);

					for (int i = 0; i < num && handleCount < MAX_HANDLES; ++i)
					{
						uintptr_t elemAddr = arrayPtr + static_cast<uintptr_t>(i) * elemSize;
						int32_t serial = ReadAt<int32_t>(elemAddr, 0);

						if (serial > 0)
						{
							handles[handleCount].Index = i;
							handles[handleCount].SerialNumber = serial;
							handleCount++;
						}
					}

					LOG_INFO("  Extracted %d valid entity handles from %d slots",
						handleCount, num);
					break;
				}
			}
		}

		// ---- 5. Signal all entities ----
		if (handleCount > 0)
		{
			LOG_INFO("  Signaling %d entities with socket signal...", handleCount);

			for (int i = 0; i < handleCount; ++i)
			{
				fnSignalEntity(static_cast<void*>(signalSubsystem),
					socketSignalName, handles[i]);
			}

			LOG_INFO("  Socket signal sent to %d entities", handleCount);
		}
		else
		{
			LOG_WARN("  No entity handles found - could not signal for socket re-init");
			LOG_WARN("  This may indicate the entity manager layout has changed");
		}

		VirtualFree(handles, 0, MEM_RELEASE);
		LOG_INFO("SignalSocketEntities: complete");
	}
}
