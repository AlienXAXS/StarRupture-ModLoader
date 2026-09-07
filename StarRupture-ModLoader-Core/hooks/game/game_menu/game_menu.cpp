#include "pch.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "game_menu.h"
#include "game_menu_registry.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "hooks/memory/engine_allocator.h"
#include "../scan_patterns.h"
#include "../ufunction_resolve.h"
#include "../text_localization/text_key.h"
#include "../text_localization/text_localization.h"

#include "ChimeraUI_classes.hpp"
#include "ChimeraUI_parameters.hpp"

#include <vector>
#include <cstring>

namespace Hooks::GameMenu
{
	namespace
	{
		// The registry lives at global scope; inside Hooks::GameMenu an unqualified
		// GameMenu:: would resolve to this namespace instead.
		namespace Reg = ::GameMenu::Registry;

		// -------------------------------------------------------------------
		// Sentinels
		//
		// ECrMenuType is a uint8 and its real values stop at 19
		// (ECrMenuType_MAX = 20), so 0xC0..0xFF is a range the game cannot
		// produce.  A registry slot N is spliced in as kSentinelBase + N, which
		// is what lets the click path recover the slot from the widget's own
		// LeftButtons array instead of remembering button pointers.
		// -------------------------------------------------------------------
		constexpr uint8_t kSentinelBase    = 0xC0;
		constexpr uint8_t kMenuTypeOptions = static_cast<uint8_t>(SDK::ECrMenuType::Options);

		// Sanity bound on LeftButtons before we trust it.  The stock menus have
		// six rows; anything wildly outside that means we are not looking at
		// what we think we are, and doing nothing is the right answer.
		constexpr int32_t kMaxSaneRows = 256;

		// -------------------------------------------------------------------
		// Button index bases
		//
		// The two menus number their buttons differently, and getting this
		// wrong fails silently rather than loudly: the click falls through to
		// the original, which finds the sentinel in ButtonsIndexes and
		// broadcasts it into OnActionTriggered's default case, so the row is
		// built, labelled, clickable -- and does nothing at all.
		//
		//   UCrUW_MainMenuWidget sets Index = 1 and ContinueButton->SetIndex(0)
		//   BEFORE the loop, so LeftButtons[i] is index i + 1 and ExitButton
		//   takes Num + 1.
		//
		//   UCrUW_PauseMenu starts Index at 0, so LeftButtons[i] is index i,
		//   and ContinueButton / ExitButton / ExitToMainMenuButton take Num,
		//   Num + 1 and Num + 2 AFTER the loop.
		//
		// Either way the fixed buttons land outside [0, Num), so they are
		// rejected by the range check rather than by a special case.
		// -------------------------------------------------------------------
		constexpr int32_t kMainIndexBase  = 1;
		constexpr int32_t kPauseIndexBase = 0;

		uint8_t SlotToSentinel(int slot)
		{
			return static_cast<uint8_t>(kSentinelBase + slot);
		}

		// TArray<ECrMenuType>'s three fields.  The SDK's TArray keeps them
		// protected and offers no insert, and we need to replace the buffer
		// wholesale, so mirror the layout rather than fight the wrapper.
		struct FRawByteArray
		{
			uint8_t* Data;
			int32_t  Num;
			int32_t  Max;
		};

		using NativeConstruct_t = void(__fastcall*)(void* self);
		using ButtonClicked_t   = void(__fastcall*)(void* self, int32_t index);

		Hook              g_mainConstructHook;
		Hook              g_mainClickHook;
		Hook              g_pauseConstructHook;
		Hook              g_pauseClickHook;
		NativeConstruct_t g_mainConstructOrig  = nullptr;
		NativeConstruct_t g_pauseConstructOrig = nullptr;
		ButtonClicked_t   g_mainClickOrig      = nullptr;
		ButtonClicked_t   g_pauseClickOrig     = nullptr;
		bool              g_mainInstalled      = false;
		bool              g_pauseInstalled     = false;

		// Renders a row list as hex for the log.  Sentinels are the values at
		// or above kSentinelBase, so a glance at the line says which positions
		// are ours and which are the game's.
		void FormatRows(const uint8_t* data, int32_t num, wchar_t* out, size_t outCount)
		{
			size_t written = 0;
			if (!out || outCount == 0)
				return;
			out[0] = L'\0';
			if (!data)
				return;

			for (int32_t i = 0; i < num && written + 4 < outCount; ++i)
			{
				const int n = swprintf_s(out + written, outCount - written,
				                         i == 0 ? L"%02X" : L" %02X", data[i]);
				if (n <= 0)
					break;
				written += static_cast<size_t>(n);
			}
		}

		// -------------------------------------------------------------------
		// Row labels
		//
		// The engine's own ButtonsTexts lookup misses on a sentinel (that is
		// the point -- a miss sets no text rather than crashing), so the label
		// is applied afterwards through UTabButton::SetButtonText.
		//
		// Each FText is built once and kept for the life of the process.
		// Releasing one means a virtual call through its refcounted ITextData,
		// and hand-rolling that to reclaim a handful of strings would be a much
		// better way to crash than to save memory.  A label change replaces the
		// cache entry and abandons the previous FText, which is bounded by how
		// often a plugin renames a row.
		// -------------------------------------------------------------------
		struct TextCache
		{
			char        label[96]{};
			SDK::FText  text{};
			bool        valid = false;
		};
		TextCache g_text[Reg::kMaxEntries];

		bool EnsureText(int slot, const char* label, SDK::FText& out)
		{
			if (slot < 0 || slot >= Reg::kMaxEntries || !label)
				return false;

			TextCache& cache = g_text[slot];
			if (cache.valid && strcmp(cache.label, label) == 0)
			{
				out = cache.text;
				return true;
			}

			const uintptr_t keyCtor  = Hooks::TextKey::GetOriginalPtr();
			const uintptr_t textCtor = Hooks::TextLocalization::GetOriginalPtr();
			if (!keyCtor || !textCtor)
			{
				ModLoaderLogger::LogWarn(
					L"[GameMenu] FText helpers are unavailable -- menu rows would be blank");
				return false;
			}

			auto MakeKey  = reinterpret_cast<void(__fastcall*)(void*, const wchar_t*)>(keyCtor);
			auto MakeText = reinterpret_cast<SDK::FText*(__fastcall*)(
				SDK::FText*, const void*, const void*, const wchar_t*)>(textCtor);

			// Labels are ASCII (loader convention), so a widening copy is enough.
			wchar_t wide[96]{};
			for (int i = 0; i < 95 && label[i]; ++i)
				wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(label[i]));

			// FTextKey is 8 bytes; the buffers are deliberately oversized so a
			// wider one in some future engine build writes into slack instead
			// of over the stack.
			uint8_t namespaceKey[32]{};
			uint8_t textKey[32]{};
			MakeKey(namespaceKey, L"ModLoader");
			MakeKey(textKey, wide);

			SDK::FText built{};
			MakeText(&built, namespaceKey, textKey, wide);

			// Nothing registers "ModLoader"/<label> in a localization table, so
			// this resolves to the source string -- which is what we want, and
			// leaves the door open for someone to translate it later.
			cache.text  = built;
			cache.valid = true;
			strncpy_s(cache.label, sizeof(cache.label), label, _TRUNCATE);

			out = built;
			return true;
		}

		void SetButtonText(SDK::UTabButton* button, const SDK::FText& text)
		{
			static SDK::UFunction* s_fn = nullptr;
			if (!s_fn)
			{
				if (!button->Class)
					return;
				s_fn = button->Class->GetFunction("TabButton", "SetButtonText");
				if (!s_fn)
				{
					ModLoaderLogger::LogWarn(L"[GameMenu] TabButton::SetButtonText not found -- rows will be blank");
					return;
				}
			}

			// Mirrors the SDK's generated body: the temporary FUNC_Native flag
			// is what makes ProcessEvent dispatch straight to the native
			// implementation instead of looking for bytecode.
			SDK::Params::TabButton_SetButtonText parms{};
			parms.Text = text;

			const auto flags = s_fn->FunctionFlags;
			s_fn->FunctionFlags |= 0x400;
			button->ProcessEvent(s_fn, &parms);
			s_fn->FunctionFlags = flags;
		}

		// -------------------------------------------------------------------
		// Splice -- runs before the engine's button loop
		// -------------------------------------------------------------------
		void SpliceInto(FRawByteArray* rows, int target, const wchar_t* menuName)
		{
			if (!rows || rows->Num < 0 || rows->Num > kMaxSaneRows)
				return;
			if (rows->Num > 0 && !rows->Data)
				return;   // a non-empty array with no buffer is not something to reason about
			if (!Reg::HasAnyFor(target))
			{
				ModLoaderLogger::LogDebug(
					L"[GameMenu] %s built with no entries registered for it -- nothing to splice",
					menuName);
				return;
			}

			// The replacement buffer is handed to UE, which will free it with
			// FMemory::Free when the widget goes away.  Without the engine
			// allocator that would be a CRT pointer passed to the binned
			// allocator, so there is no fallback here -- we simply do nothing.
			if (!EngineAllocator::IsAvailable())
			{
				static bool s_warned = false;
				if (!s_warned)
				{
					s_warned = true;
					ModLoaderLogger::LogWarn(
						L"[GameMenu] Engine allocator unavailable -- menu entries disabled");
				}
				return;
			}

			// NativeConstruct can run more than once on the same widget
			// instance; a second splice would double every row.
			for (int32_t i = 0; i < rows->Num; ++i)
			{
				if (rows->Data[i] >= kSentinelBase)
				{
					ModLoaderLogger::LogDebug(
						L"[GameMenu] %s already carries spliced rows -- NativeConstruct ran again, "
						L"leaving the list alone", menuName);
					return;
				}
			}

			Reg::EntryView views[Reg::kMaxEntries];
			const int count = Reg::Snapshot(target, views, Reg::kMaxEntries);
			if (count <= 0)
				return;

			std::vector<uint8_t> merged;
			merged.reserve(static_cast<size_t>(rows->Num) + static_cast<size_t>(count));

			for (int e = 0; e < count; ++e)
			{
				if (views[e].anchor == PLUGIN_GAME_MENU_ANCHOR_TOP)
					merged.push_back(SlotToSentinel(views[e].slot));
			}

			bool afterOptionsPlaced = false;
			for (int32_t i = 0; i < rows->Num; ++i)
			{
				const uint8_t value = rows->Data[i];
				merged.push_back(value);

				if (value == kMenuTypeOptions && !afterOptionsPlaced)
				{
					afterOptionsPlaced = true;
					for (int e = 0; e < count; ++e)
					{
						if (views[e].anchor == PLUGIN_GAME_MENU_ANCHOR_AFTER_OPTIONS)
							merged.push_back(SlotToSentinel(views[e].slot));
					}
				}
			}

			for (int e = 0; e < count; ++e)
			{
				// An AFTER_OPTIONS row falls back to the bottom when the menu
				// has no Options row at all -- better a row in the wrong place
				// than a row that silently never appears.
				const bool orphaned = views[e].anchor == PLUGIN_GAME_MENU_ANCHOR_AFTER_OPTIONS
				                      && !afterOptionsPlaced;
				if (views[e].anchor == PLUGIN_GAME_MENU_ANCHOR_BOTTOM || orphaned)
					merged.push_back(SlotToSentinel(views[e].slot));
			}

			if (merged.size() == static_cast<size_t>(rows->Num))
				return;

			auto* fresh = static_cast<uint8_t*>(EngineAllocator::Alloc(merged.size(), 0));
			if (!fresh)
			{
				ModLoaderLogger::LogError(L"[GameMenu] Failed to allocate %zu bytes for the %s row list",
				                          merged.size(), menuName);
				return;
			}
			memcpy(fresh, merged.data(), merged.size());

			uint8_t* previous = rows->Data;
			rows->Data = fresh;
			rows->Num  = static_cast<int32_t>(merged.size());
			rows->Max  = static_cast<int32_t>(merged.size());

			// UE allocated the old buffer through FMemory, so this is the
			// matching free.
			if (previous)
				EngineAllocator::Free(previous);

			wchar_t layout[kMaxSaneRows * 3 + 8];
			FormatRows(rows->Data, rows->Num, layout, _countof(layout));
			ModLoaderLogger::LogDebug(
				L"[GameMenu] Spliced %d entr%s into the %s -- %d rows now: %s",
				count, count == 1 ? L"y" : L"ies", menuName, rows->Num, layout);
		}

		// -------------------------------------------------------------------
		// Label -- runs after the engine has built the buttons
		//
		// Children of ButtonsBox are exactly the LeftButtons rows, in order
		// (the loop clears the box first and only adds those), so slot index
		// and row index are the same number.  If they ever stop matching, the
		// menu is not the one this code was written against and labelling by
		// position would rename somebody else's button -- so bail instead.
		// -------------------------------------------------------------------
		void LabelRows(FRawByteArray* rows, SDK::UPanelWidget* box, int target, const wchar_t* menuName)
		{
			if (!rows || !rows->Data || !box)
				return;
			if (rows->Num <= 0 || rows->Num > kMaxSaneRows)
				return;

			if (box->Slots.Num() != rows->Num)
			{
				ModLoaderLogger::LogWarn(
					L"[GameMenu] %s has %d buttons for %d rows -- not labelling, the menu layout changed",
					menuName, box->Slots.Num(), rows->Num);
				return;
			}

			Reg::EntryView views[Reg::kMaxEntries];
			const int count = Reg::Snapshot(target, views, Reg::kMaxEntries);
			if (count <= 0)
				return;

			const char* labelBySlot[Reg::kMaxEntries]{};
			for (int e = 0; e < count; ++e)
			{
				if (views[e].slot >= 0 && views[e].slot < Reg::kMaxEntries)
					labelBySlot[views[e].slot] = views[e].label;
			}

			SDK::UClass* tabButtonClass = SDK::UTabButton::StaticClass();
			int labelled = 0;

			for (int32_t i = 0; i < rows->Num; ++i)
			{
				const uint8_t value = rows->Data[i];
				if (value < kSentinelBase)
					continue;

				const int slot = value - kSentinelBase;
				const char* label = (slot < Reg::kMaxEntries) ? labelBySlot[slot] : nullptr;
				if (!label)
					continue;   // registered and then removed between splice and here

				SDK::UPanelSlot* panelSlot = box->Slots[i];
				if (!panelSlot || !panelSlot->Content)
					continue;
				if (tabButtonClass && !panelSlot->Content->IsA(tabButtonClass))
					continue;

				SDK::FText text{};
				if (!EnsureText(slot, label, text))
					continue;

				SetButtonText(static_cast<SDK::UTabButton*>(panelSlot->Content), text);
				++labelled;
			}

			ModLoaderLogger::LogDebug(L"[GameMenu] Labelled %d of %d %s row%s",
			                          labelled, count, menuName, count == 1 ? L"" : L"s");
		}

		// -------------------------------------------------------------------
		// Click -- returns true when the row belongs to us and must not reach
		// the game.  ButtonClicked's index counts from indexBase over
		// LeftButtons, so row N is LeftButtons[N - indexBase]; see the
		// kMainIndexBase / kPauseIndexBase comment above.
		// -------------------------------------------------------------------
		bool ClaimClick(FRawByteArray* rows, int32_t index, int32_t indexBase, const wchar_t* menuName)
		{
			if (!rows || !rows->Data || rows->Num <= 0 || rows->Num > kMaxSaneRows)
			{
				ModLoaderLogger::LogDebug(
					L"[GameMenu] %s click on index %d -- the row list is unusable, passing through",
					menuName, index);
				return false;
			}

			const int32_t row = index - indexBase;

			// Every click into a hooked menu logs one line, because the failure
			// this guards against is silent: with the wrong index base the row
			// resolves to a stock ECrMenuType, the click reaches the original,
			// and the button does nothing at all -- no crash, no warning, and
			// nothing in the log to say the click was even seen.  Menu clicks
			// are user-driven and rare, so a Debug line each is not spam.
			if (row < 0 || row >= rows->Num)
			{
				// Continue / Exit / ExitToMainMenu are separate widgets and take
				// indices outside the row range; they land here every time.
				ModLoaderLogger::LogDebug(
					L"[GameMenu] %s click on index %d is not a row (%d rows, base %d) -- passing through",
					menuName, index, rows->Num, indexBase);
				return false;
			}

			const uint8_t value = rows->Data[row];
			if (value < kSentinelBase)
			{
				ModLoaderLogger::LogDebug(
					L"[GameMenu] %s click on index %d -> row %d = ECrMenuType %u -- the game's, passing through",
					menuName, index, row, static_cast<unsigned>(value));
				return false;
			}

			const int slot = value - kSentinelBase;
			ModLoaderLogger::LogDebug(
				L"[GameMenu] %s click on index %d -> row %d = sentinel 0x%02X -- claimed for entry slot %d",
				menuName, index, row, static_cast<unsigned>(value), slot);

			Reg::Invoke(slot);
			return true;
		}

		// -------------------------------------------------------------------
		// Detours
		// -------------------------------------------------------------------
		void __fastcall MainConstructDetour(SDK::UCrUW_MainMenuWidget* self)
		{
			if (self)
			{
				try
				{
					SpliceInto(reinterpret_cast<FRawByteArray*>(&self->LeftButtons),
					           PLUGIN_GAME_MENU_MAIN, L"main menu");
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[GameMenu] Exception while splicing main menu rows");
				}
			}

			if (g_mainConstructOrig)
				g_mainConstructOrig(self);

			if (self)
			{
				try
				{
					LabelRows(reinterpret_cast<FRawByteArray*>(&self->LeftButtons),
					          self->ButtonsBox, PLUGIN_GAME_MENU_MAIN, L"main menu");
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[GameMenu] Exception while labelling main menu rows");
				}
			}
		}

		void __fastcall PauseConstructDetour(SDK::UCrUW_PauseMenu* self)
		{
			if (self)
			{
				try
				{
					SpliceInto(reinterpret_cast<FRawByteArray*>(&self->LeftButtons),
					           PLUGIN_GAME_MENU_PAUSE, L"pause menu");
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[GameMenu] Exception while splicing pause menu rows");
				}
			}

			if (g_pauseConstructOrig)
				g_pauseConstructOrig(self);

			if (self)
			{
				try
				{
					LabelRows(reinterpret_cast<FRawByteArray*>(&self->LeftButtons),
					          self->ButtonsBox, PLUGIN_GAME_MENU_PAUSE, L"pause menu");
				}
				catch (...)
				{
					ModLoaderLogger::LogError(L"[GameMenu] Exception while labelling pause menu rows");
				}
			}
		}

		void __fastcall MainClickDetour(SDK::UCrUW_MainMenuWidget* self, int32_t index)
		{
			if (self && ClaimClick(reinterpret_cast<FRawByteArray*>(&self->LeftButtons),
			                       index, kMainIndexBase, L"main menu"))
				return;

			if (g_mainClickOrig)
				g_mainClickOrig(self, index);
		}

		void __fastcall PauseClickDetour(SDK::UCrUW_PauseMenu* self, int32_t index)
		{
			if (self && ClaimClick(reinterpret_cast<FRawByteArray*>(&self->LeftButtons),
			                       index, kPauseIndexBase, L"pause menu"))
				return;

			if (g_pauseClickOrig)
				g_pauseClickOrig(self, index);
		}

		// -------------------------------------------------------------------
		// Address resolution
		// -------------------------------------------------------------------
		bool InMainModule(uintptr_t addr)
		{
			HMODULE module = GetModuleHandleW(nullptr);
			if (!module || !addr)
				return false;

			const auto base = reinterpret_cast<uintptr_t>(module);
			auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				return false;
			auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE)
				return false;

			return addr >= base && addr < base + nt->OptionalHeader.SizeOfImage;
		}

		// ButtonClicked is a UFUNCTION, so no AOB is needed for it -- but the
		// address we want is the member function, not the exec stub, because
		// the menu binds the raw member into a delegate and the stub is never
		// on that path.  UHT's generated stub unpacks the parameters and then
		// makes exactly one call into the member immediately before its
		// epilogue, so the last call rel32 before the first ret is the target.
		//
		// Walked with the length decoder rather than scanned for 0xE8: a rel32
		// or a displacement can contain that byte and a naive scan would take
		// it, giving a detour written over the middle of an instruction.
		uintptr_t ResolveButtonClicked(const char* className)
		{
			const uintptr_t execAddr = Hooks::ResolveUFunctionNativeAddr(className, "ButtonClicked");
			if (!execAddr)
				return 0;

			const auto* code = reinterpret_cast<const uint8_t*>(execAddr);
			uintptr_t target = 0;

			for (size_t offset = 0; offset < 256; )
			{
				const uint8_t* insn = code + offset;
				if (*insn == 0xC3 || *insn == 0xC2)   // ret / ret imm16
					break;

				const size_t length = Hooks::GetInstructionLength(insn);
				if (length == 0 || length > 15)
				{
					ModLoaderLogger::LogWarn(
						L"[GameMenu] Could not decode %S::execButtonClicked at +0x%zX", className, offset);
					return 0;
				}

				if (*insn == 0xE8 && length == 5)
				{
					const int32_t rel = *reinterpret_cast<const int32_t*>(insn + 1);
					target = reinterpret_cast<uintptr_t>(insn + 5) + static_cast<intptr_t>(rel);
				}

				offset += length;
			}

			if (!InMainModule(target))
			{
				ModLoaderLogger::LogWarn(
					L"[GameMenu] %S::ButtonClicked resolved to 0x%llX, which is outside the game module",
					className, static_cast<unsigned long long>(target));
				return 0;
			}

			ModLoaderLogger::LogDebug(L"[GameMenu] Resolved %S::ButtonClicked = 0x%llX (from its exec stub)",
			                          className, static_cast<unsigned long long>(target));
			return target;
		}

		// -----------------------------------------------------------------------
		// Install one menu.
		//
		// The click hook goes on FIRST, and the construct hook is only installed
		// if it took.  The other order has a failure mode with teeth: a menu
		// carrying a sentinel row whose click is NOT intercepted broadcasts an
		// ECrMenuType the game has never seen into
		// UCrUW_PauseMenuMainScreen::OnActionTriggered's default case.  Better to
		// have no button than a button that does something nobody has reasoned
		// about.
		// -----------------------------------------------------------------------
		bool InstallMenu(const char* className,
		                 const char*  constructPatternName,
		                 const char*  constructPattern,
		                 Hook&        clickHook,
		                 void*        clickDetour,
		                 void**       clickOrig,
		                 Hook&        constructHook,
		                 void*        constructDetour,
		                 void**       constructOrig)
		{
			const uintptr_t clickAddr = ResolveButtonClicked(className);
			if (!clickAddr)
			{
				ModLoaderLogger::LogWarn(
					L"[GameMenu] %S::ButtonClicked could not be resolved -- no entries in this menu",
					className);
				return false;
			}

			if (!clickHook.Install(clickAddr, clickDetour, clickOrig))
			{
				ModLoaderLogger::LogError(L"[GameMenu] Failed to hook %S::ButtonClicked", className);
				return false;
			}

			const uintptr_t constructAddr =
				Scanner::FindPatternInMainModule(constructPatternName, constructPattern);
			if (!constructAddr)
			{
				ModLoaderLogger::LogWarn(
					L"[GameMenu] Pattern for %S failed -- no entries in this menu", constructPatternName);
				clickHook.Remove();
				*clickOrig = nullptr;
				return false;
			}

			ModLoaderLogger::LogDebug(L"[GameMenu] Found %S at 0x%llX",
			                          constructPatternName,
			                          static_cast<unsigned long long>(constructAddr));

			if (!constructHook.Install(constructAddr, constructDetour, constructOrig))
			{
				ModLoaderLogger::LogError(L"[GameMenu] Failed to hook %S", constructPatternName);
				clickHook.Remove();
				*clickOrig = nullptr;
				return false;
			}

			ModLoaderLogger::LogInfo(
				L"[GameMenu] Hooked %S -- ButtonClicked at 0x%llX, NativeConstruct at 0x%llX",
				className,
				static_cast<unsigned long long>(clickAddr),
				static_cast<unsigned long long>(constructAddr));
			return true;
		}
	}

	bool Install()
	{
		if (g_mainInstalled || g_pauseInstalled)
			return true;

		ModLoaderLogger::LogInfo(L"[GameMenu] Installing menu entry hooks...");

		g_mainInstalled = InstallMenu(
			"CrUW_MainMenuWidget",
			"UCrUW_MainMenuWidget::NativeConstruct",
			ScanPatterns::UCrUW_MainMenuWidget_NativeConstruct,
			g_mainClickHook,     reinterpret_cast<void*>(&MainClickDetour),
			reinterpret_cast<void**>(&g_mainClickOrig),
			g_mainConstructHook, reinterpret_cast<void*>(&MainConstructDetour),
			reinterpret_cast<void**>(&g_mainConstructOrig));

		g_pauseInstalled = InstallMenu(
			"CrUW_PauseMenu",
			"UCrUW_PauseMenu::NativeConstruct",
			ScanPatterns::UCrUW_PauseMenu_NativeConstruct,
			g_pauseClickHook,     reinterpret_cast<void*>(&PauseClickDetour),
			reinterpret_cast<void**>(&g_pauseClickOrig),
			g_pauseConstructHook, reinterpret_cast<void*>(&PauseConstructDetour),
			reinterpret_cast<void**>(&g_pauseConstructOrig));

		if (g_mainInstalled || g_pauseInstalled)
		{
			ModLoaderLogger::LogInfo(L"[GameMenu] Menu entries enabled (main menu: %s, pause menu: %s)",
			                         g_mainInstalled ? L"yes" : L"no",
			                         g_pauseInstalled ? L"yes" : L"no");
			return true;
		}

		ModLoaderLogger::LogWarn(
			L"[GameMenu] Neither menu could be hooked -- the mod loader row will not appear. "
			L"The overlay is still reachable with its open key.");
		return false;
	}

	void Remove()
	{
		if (g_mainInstalled || g_pauseInstalled)
			ModLoaderLogger::LogInfo(L"[GameMenu] Removing menu entry hooks");

		if (g_mainInstalled)
		{
			g_mainConstructHook.Remove();
			g_mainClickHook.Remove();
			g_mainConstructOrig = nullptr;
			g_mainClickOrig     = nullptr;
			g_mainInstalled     = false;
		}

		if (g_pauseInstalled)
		{
			g_pauseConstructHook.Remove();
			g_pauseClickHook.Remove();
			g_pauseConstructOrig = nullptr;
			g_pauseClickOrig     = nullptr;
			g_pauseInstalled     = false;
		}
	}

	bool IsInstalled()
	{
		return g_mainInstalled || g_pauseInstalled;
	}
}

#endif // MODLOADER_CLIENT_BUILD
