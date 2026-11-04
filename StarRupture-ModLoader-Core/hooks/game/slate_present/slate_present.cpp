#include "pch.h"
#include "slate_present.h"
#include "../../hooks_common.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"
#include "../../../UI/imgui_backend.h"
#include <atomic>
#include <cstdint>

namespace Hooks::SlatePresent
{
	// UE 5.6 layout, as PresentWindow_RenderThread and the inline
	// EnqueueLambda in it use it in the game's own code.
	//   FSlateDrawWindowPassOutputs { ViewportRHI; ViewportTextureRHI; OutputTextureRHI; }
	constexpr size_t kOutputs_OutputTextureRHI = 0x10;
	//   FRHICommandListBase: CommandLink (FRHICommandBase**), NumCommands, bExecuting
	constexpr size_t kCmdList_CommandLink = 0x30;
	constexpr size_t kCmdList_NumCommands = 0x60;
	constexpr size_t kCmdList_Executing   = 0x64;

	// ERHIAccess (RHIAccess.h)
	constexpr uint32_t kAccessUnknown = 0;
	constexpr uint32_t kAccessRTV = 1u << 12;

	// FRHITransitionInfo (RHITransition.h), 0x30 bytes.
	struct TransitionInfo
	{
		uint16_t mipIndex = 0xFFFF;     // FRHISubresourceRange: all subresources
		uint16_t arraySlice = 0xFFFF;
		uint16_t planeSlice = 0xFFFF;
		void*    resource = nullptr;
		uint8_t  type = 1;              // EType::Texture
		uint32_t accessBefore = kAccessUnknown;
		uint32_t accessAfter = kAccessUnknown;
		uint8_t  flags = 0;             // EResourceTransitionFlags::None
		uint64_t commitSize = 0;        // TOptional<FRHICommitResourceInfo>, unset
		bool     commitIsSet = false;
	};
	static_assert(sizeof(TransitionInfo) == 0x30, "FRHITransitionInfo layout");

	struct TransitionInfoView { const TransitionInfo* data; int32_t num; };

	// FRHICommandBase: one virtual (ExecuteAndDestruct), then Next. The
	// executor reads Next before it runs a command, so a command may free
	// itself in ExecuteAndDestruct.
	struct RhiCommandBase
	{
		virtual void ExecuteAndDestruct(void* rhiCmdList) = 0;
		RhiCommandBase* next = nullptr;
	};

	using PresentWindow_t = void(__fastcall*)(void* renderer, void* rhiCmdList, const void* inputs, const void* outputs);
	using Transition_t    = void(__fastcall*)(void* rhiCmdList, const TransitionInfoView* infos, uint32_t createFlags);
	// FD3D12DynamicRHI members; neither reads `this`.
	using ManualFence_t   = void(__fastcall*)(void* dynamicRhi, void* rhiCmdList, ID3D12Fence* fence, uint64_t value);

	static Hook            g_hook;
	static PresentWindow_t g_original = nullptr;
	static Transition_t    g_transition = nullptr;
	static ManualFence_t   g_signalManualFence = nullptr;
	static ManualFence_t   g_waitManualFence = nullptr;
	static std::atomic<bool> g_loggedFirst{ false };

	// RHI thread, in order with the commands around it: UE's queue signals
	// that Slate's draw is done, the host's queue waits for that, draws and
	// signals, and UE's queue waits for the overlay before anything after it
	// (frame generation's UI pass, Present) runs.
	struct SubmitOverlayCommand final : RhiCommandBase
	{
		int slot = -1;

		void ExecuteAndDestruct(void* rhiCmdList) override
		{
			ImGuiUiOverlaySync sync{};
			if (UI::ImGuiBackend::SubmitUiOverlay(slot, &sync))
			{
				g_signalManualFence(nullptr, rhiCmdList, sync.readyFence, sync.readyValue);
				g_waitManualFence(nullptr, rhiCmdList, sync.doneFence, sync.doneValue);
			}
			delete this;
		}
	};

	static void Enqueue(void* rhiCmdList, RhiCommandBase* cmd)
	{
		auto* base = static_cast<uint8_t*>(rhiCmdList);
		if (*reinterpret_cast<bool*>(base + kCmdList_Executing))
		{
			cmd->ExecuteAndDestruct(rhiCmdList);
			return;
		}
		RhiCommandBase**& link = *reinterpret_cast<RhiCommandBase***>(base + kCmdList_CommandLink);
		++*reinterpret_cast<uint32_t*>(base + kCmdList_NumCommands);
		*link = cmd;
		link = &cmd->next;
	}

	// Render thread. Every widget of this window is in its back buffer;
	// nothing has read it yet.
	static void __fastcall Detour(void* renderer, void* rhiCmdList, const void* inputs, const void* outputs)
	{
		void* backBuffer = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(outputs) + kOutputs_OutputTextureRHI);
		const int slot = backBuffer ? UI::ImGuiBackend::RecordUiOverlay(backBuffer) : -1;
		if (slot >= 0)
		{
			TransitionInfo info;
			info.resource = backBuffer;
			info.accessAfter = kAccessRTV;
			const TransitionInfoView view{ &info, 1 };
			g_transition(rhiCmdList, &view, 0);

			auto* cmd = new SubmitOverlayCommand();
			cmd->slot = slot;
			Enqueue(rhiCmdList, cmd);

			if (!g_loggedFirst.exchange(true))
				ModLoaderLogger::LogInfo(L"[SlatePresent] First overlay drawn at Slate's present step");
		}
		g_original(renderer, rhiCmdList, inputs, outputs);
	}

	static uintptr_t Find(const char* name, const char* pattern)
	{
		const uintptr_t addr = Scanner::FindPatternInMainModule(name, pattern);
		if (!addr)
			ModLoaderLogger::LogWarn(L"[SlatePresent] %S not found", name);
		return addr;
	}

	bool Install()
	{
		if (g_hook.installed)
			return true;

		const uintptr_t present    = Find("FSlateRHIRenderer::PresentWindow_RenderThread", ScanPatterns::FSlateRHIRenderer_PresentWindow_RenderThread);
		const uintptr_t transition = Find("FRHIComputeCommandList::Transition", ScanPatterns::FRHIComputeCommandList_Transition);
		const uintptr_t signal     = Find("FD3D12DynamicRHI::RHISignalManualFence", ScanPatterns::FD3D12DynamicRHI_RHISignalManualFence);
		const uintptr_t wait       = Find("FD3D12DynamicRHI::RHIWaitManualFence", ScanPatterns::FD3D12DynamicRHI_RHIWaitManualFence);
		if (!present || !transition || !signal || !wait)
		{
			ModLoaderLogger::LogWarn(L"[SlatePresent] The overlay stays on the Present hook; frame generation shows it on real frames only");
			return false;
		}

		g_transition        = reinterpret_cast<Transition_t>(transition);
		g_signalManualFence = reinterpret_cast<ManualFence_t>(signal);
		g_waitManualFence   = reinterpret_cast<ManualFence_t>(wait);

		const bool ok = g_hook.Install(present, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original));
		if (ok)
			ModLoaderLogger::LogInfo(L"[SlatePresent] PresentWindow_RenderThread hooked at 0x%llX", static_cast<unsigned long long>(present));
		else
			ModLoaderLogger::LogError(L"[SlatePresent] PresentWindow_RenderThread hook failed");
		return ok;
	}

	void Remove()
	{
		g_hook.Remove();
	}
}

#endif // MODLOADER_CLIENT_BUILD
