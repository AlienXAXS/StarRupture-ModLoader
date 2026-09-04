#pragma once

#include <windows.h>
#include <cstdint>

// v19: Introduced IPluginSelf.  MIN bumped to 19.
// v20: Added OnBeforeWorldEndPlay / OnAfterWorldEndPlay.  MIN remains 19.
// v21: Added IPluginNativePointers.  MIN remains 19.
// v22: Added IPluginHttpServer (hooks->HttpServer).
//   Static file route registration (AddRoute/RemoveRoute), raw-request
//      filter hook (RegisterOnRawRequest/UnregisterOnRawRequest), and
//  raw-response route registration (AddRawRoute/RemoveRawRoute).
//      URL scheme: /<pluginName>/<routeName>/...  (case-insensitive).
//      Static files are served from <exe_dir>\Plugins\<pluginName>\<folderName>\.
//      Raw-response routes let plugins handle arbitrary URL prefixes and write
//      any response body + content-type directly (e.g. JSON API endpoints).
//   Server builds only; nullptr on client/generic builds.
//      MIN remains 19.
// v23 - SR Hotfix update 22/04/2026 118961
// v24: Added scaling/metrics functions to IModLoaderImGui (GetFontSize, GetTextLineHeight,
//      GetTextLineHeightWithSpacing, GetFrameHeight, GetFrameHeightWithSpacing, CalcTextSize,
//      SetWindowFontScale, GetContentRegionAvail, GetDisplaySize).
//      Added PluginWindowHints struct and windowHints field to PluginWidgetDesc.
//      MIN remains 23.
// v31: Added extra_window_flags to PluginWindowHints.
//      Allows plugins to pass additional ImGuiWindowFlags (e.g. NoTitleBar, NoResize).
//      0 = no extra flags (default behaviour unchanged). MIN remains 26.
// v32: Added IPluginClientSessionInfo (client only, null on server/generic).
//      Exposes GetSessionOnlineMode, IsMultiplayer, IsServer query functions.
// v33: Added pluginTarget field to PluginInfo. Every plugin must now declare
//      PLUGIN_TARGET_CLIENT or PLUGIN_TARGET_SERVER. The loader rejects plugins
//      that don't match the current build target.
// v34: Game had an update, needed interface bump
// v35: Added layout/sidebar functions to IModLoaderImGui:
//      BeginChild, EndChild, PushStyleColor, PopStyleColor,
//      PushStyleVarFloat, PushStyleVarVec2, PopStyleVar,
//      PushItemWidth, PopItemWidth, SetCursorPosX, GetCursorPosX,
//      BeginTable, TableNextColumn, EndTable, IsItemClicked,
//      GetWindowWidth, GetWindowHeight, Dummy.
//      Also wired SetWindowFontScale which was declared but not populated.
//      MIN remains 34.
//      -- Mass expansion of IModLoaderImGui — added ~100 additional ImGui
//      functions covering: window queries, scroll, groups, extended cursor
//      control, all drag/slider variants, listbox, tab bar, menus, popups,
//      tooltips, full table API, item state predicates, disabled regions,
//      clip rect, mouse queries, color utilities, and misc helpers.
//      MIN remains 34.
//      --- Added MakeTextKey to IPluginTextUtils (trampoline for FTextKey::FTextKey),
//      letting plugins build the Namespace/Key FTextKey arguments required by
//      AsLocalizable_Advanced.  MIN remains 34.
// v36: Replaced IPluginClientSessionInfo with IPluginNetModeInfo (server + client,
//      null on generic). The old offset-based UCrSessionSubsystem reads were
//      replaced with an AOB-resolved trampoline to AActor::InternalGetNetMode,
//      exposed via the new EPluginNetMode enum (mirrors engine ENetMode) so
//      plugins can compare against typed names instead of raw integers.
//      MIN should be bumped, but no one is using it yet.
//      --- Added object/package lookup address resolvers to IPluginEngineEvents:
//      GetStaticFindObjectByPathAddress, GetStaticFindObjectByNameAddress,
//      GetStaticFindObjectSafeByPathAddress, GetStaticFindObjectSafeByNameAddress,
//      GetStaticFindObjectFastAddress, GetFindPackageAddress,
//      GetPackageFullyLoadAddress, GetLoadPackageAddress,
//      GetAssetDataFastGetAssetAddress -- AOB-resolved during early modloader
//      startup (Hooks::ObjectLookup), exposed as raw trampoline addresses.
//      MIN remains 34.
// v37: Added IPluginImGuiTextures (client only, null on server/generic).
//      Lets plugins load images from file or memory (WIC: PNG/JPG/BMP/GIF/TIFF),
//      from raw RGBA pixels, or directly from a live SDK::UTexture2D*, and render
//      them via Image/ImageButton without any direct D3D12 access.
//      Up to 64 textures may be live at once.
//      hooks->ImGuiTextures->{LoadFromFile, LoadFromMemory, LoadFromRGBA,
//      LoadFromUTexture2D, FreeTexture, GetSize, Image, ImageButton}.
//      LoadFromUTexture2D copies the engine texture to a modloader-owned resource.
//      MIN remains 34.
// v38: IPluginImGuiTextures: raised texture slot cap 64 -> 2048.
//      Added GetFreeSlotCount(), GetCapacity().
//      Load functions now throw std::out_of_range when all slots are in use
//      (previously returned nullptr silently).
//      LoadFromUTexture2D now gates on FD3D12Resource::DefaultResourceState --
//      returns nullptr (without throwing) when streaming is still in flight.
//      Image/ImageButton silently skip rendering if the engine resource is not
//      yet in a shader-readable state (same check, re-evaluated each draw call).
//      MIN remains 34.
// v39: All Load* functions now take a mandatory name parameter (const char*).
//      The name is logged when the texture is rendered and on load errors to
//      make it easier to identify which texture is causing problems.
//      LoadFromUTexture2D now copies the engine texture to a modloader-owned
//      resource, so the handle survives engine GC/eviction.  Plugins must call
//      FreeTexture when done; handles are no longer auto-expired each frame.
//      MIN remains 34.
// v40: Added IPluginSplash (client only, null on server/generic).
//      Lets plugins push status messages and progress updates to the startup
//      splash window during PluginInit, giving users feedback when a plugin
//      takes several seconds to initialise.
//      hooks->Splash->{SetStatus, SetProgress, SetSubStatus, SetSubProgress, ClearSubBar}.
//      All text parameters are UTF-8 const char* (converted to wchar internally).
//      Safe to call from PluginInit; no-ops on server/generic builds.
//      MIN remains 34.
// v41: Added AcquireSplashHold / ReleaseSplashHold to IPluginSplash.
//      Lets plugins that PostToGameThread during PluginInit keep the splash
//      open until the async work completes.  Call AcquireSplashHold before
//      returning from PluginInit, then ReleaseSplashHold when the game-thread
//      callback finishes.  The init thread waits up to 30 s for all holds to
//      drain before closing the splash.  Safe to call from any thread.
//      MIN remains 34.
// v42: Needed version bump as game updated
// v43: Added IPluginUIEvents::RegisterOnPanelWindowClosed /
//      UnregisterOnPanelWindowClosed -- fires PluginPanelClosedCallback(handle)
//      when a panel window is closed, either via the ImGui titlebar X button
//      or via a plugin calling SetPanelClose.
//      Added IPluginUIEvents::AcquireInputCapture / ReleaseInputCapture --
//      lets any plugin request that the modloader suppress game mouse/
//      keyboard input (same as while a panel/modloader window is open),
//      independent of panel/widget state. Reference-counted via opaque
//      tokens; input stays suppressed until every acquired token is released.
//      MIN remains 42 (additive fields appended to IPluginUIEvents).
// v44: Added IPluginCraftingEvents (hooks->Crafting) -- RegisterOnCraftingFinished /
//      UnregisterOnCraftingFinished, fired by ACrCrafter::NativeOnItemCraftingComplete
//      (AOB-resolved native signal handler, not a UFUNCTION) whenever any crafting
//      building (Crafter, Forge, Refinery, Factory, Assembler, Exporter,
//      FoodProcessor, ItemPrinter, etc.) finishes crafting an item. Callback
//      receives the ACrCrafter* (as void*), its UCrCraftingComponent* (as void*,
//      may be null), and the crafter's FMassEntityHandle Index/SerialNumber.
//      Added CraftingFinished() to IPluginNativePointers (appended at end).
//      hooks->Crafting is appended at the end of IPluginHooks (not inserted
//      in the middle) to preserve struct layout for v42/v43 plugins.
//      MIN remains 42.
// v45: IPluginImGuiTextures texture slot cap raised 2048 -> 4096.
//      Textures are now shared and refcounted by name (case-insensitive):
//      if Load* is called with a name that matches an already-loaded,
//      in-use texture, the existing GPU resource is reused and its
//      refcount is incremented instead of creating a new copy. Each
//      successful Load* call must be paired with exactly one FreeTexture
//      call; the underlying resource is only released once the refcount
//      reaches zero. Plugins sharing a texture name must ensure the
//      underlying image data is identical -- the first registration wins.
//      MIN remains 42.
// v46: New game update, min/max bump
// v47: Added IPluginObjectWalker (hooks->ObjectWalker) -- walks SDK::UObject::
//      GObjects on demand and lets plugins find objects by class/object name
//      and invoke a UFunction on one by name via ProcessEvent. On-demand only
//      (every call walks tens of thousands of GObjects entries synchronously);
//      there is no hook into UObject construction, so nothing fires live as
//      objects spawn -- callers must trigger a walk and cache results.
//      InvokeUFunctionByName/InvokeResolvedUFunction perform no parameter
//      marshaling: paramsBuffer must already match the target UFunction's
//      native Params layout, since this SDK dump exposes no params-size field
//      to validate against. Appended at the end of IPluginHooks to preserve
//      layout for existing plugins. MIN remains 46.
//      --- Added IPluginDelegateHook (hooks->Delegate) -- lets a plugin
//      splice a synthetic UFunction into an existing UE5 multicast delegate
//      (e.g. SDK::UCrSaveSubsystem::OnAfterSave) and get a parameterless
//      callback when it broadcasts, without the maintainer hand-writing a
//      dedicated AOB-scanned hook module for it. Never touches a real
//      UClass's FuncMap/AllFunctionsCache, so other code resolving the same
//      function name by name is unaffected. Multiple concurrent hooks,
//      including several on the same delegate, are supported via opaque
//      DelegateHookHandle values. Appended at the end of IPluginHooks.
//      v47 is unreleased, so this and IPluginObjectWalker land in the same
//      version; MIN remains 46.
//      --- IPluginObjectWalker reworked while still unreleased (no plugins
//      built against the visitor-based shape yet): WalkAllObjects /
//      FindObjectsByClassName / FindObjectsByName replaced with *Into
//      variants that fill a plugin-owned PluginObjectInfo buffer instead of
//      invoking a callback -- no allocation crosses the DLL boundary in
//      either direction. Each takes a PluginObjectLookupMode to filter
//      CDOs/archetypes out of GObjects without the plugin needing to know
//      raw EObjectFlags bit values. Return value is the total match count
//      (may exceed capacity; caller can re-call with a bigger buffer if
//      truncated). FindFirstObjectByName/InvokeUFunctionByName/
//      ResolveUFunction/InvokeResolvedUFunction are unchanged.
//      --- Added IPluginObjectProperties (hooks->ObjectProperties) --
//      resolves a UPROPERTY/FProperty by class name + property name (walks
//      the live UClass's ChildProperties + SuperStruct chain), then reads/
//      writes it through typed accessors (Bool/Int/Float/Object) or a raw
//      escape hatch (GetPropertyRawPtr). Because FProperty::Offset is looked
//      up fresh against the running build's reflection metadata every call,
//      a game update that reshuffles struct layout does not require
//      touching any plugin that goes through this interface instead of
//      casting raw SDK structs and reading at a compiled-in offset -- only
//      the property *name* needs to stay the same.
//      String/Name properties are read-only (FString/FName own their own
//      backing storage; writing one in place safely needs the engine's own
//      allocator, out of scope here). Does not cover plain native C++
//      members with no UPROPERTY/FProperty entry -- those have no reflection
//      metadata to look up by name. v47 is unreleased, so this lands in the
//      same version as IPluginObjectWalker/IPluginDelegateHook above.
//      Appended at the end of IPluginHooks. MIN remains 46.
// v48: Added ImDrawList direct-drawing access to IModLoaderImGui (appended at
//      the end of the struct, so existing v34+ plugins keep working unchanged).
//      GetWindowDrawList/GetBackgroundDrawList/GetForegroundDrawList return an
//      opaque PluginDrawList (== ImDrawList*) handle valid for the current
//      frame only -- do not cache it across frames. Covers shape primitives
//      (Line, Rect(Filled), RectFilledMultiColor, Quad(Filled),
//      Triangle(Filled), Circle(Filled), Ngon(Filled), Ellipse(Filled),
//      Text/TextSized, Polyline, ConvexPolyFilled, BezierCubic/Quadratic),
//      images via existing PluginTextureHandle (Image, ImageQuad,
//      ImageRounded), the stateful Path* builder API (PathClear/LineTo/ArcTo/
//      ArcToFast/EllipticalArcTo/BezierCubicCurveTo/BezierQuadraticCurveTo/
//      Rect/FillConvex/Stroke), and draw-list-local clip rect stack
//      (PushClipRect/PushClipRectFullScreen/PopClipRect/GetClipRectMin/Max).
//      Colors are packed 0xAABBGGRR ImU32 -- build them with the existing
//      GetColorU32FromVec4/GetColorU32FromCol helpers. MIN remains 46.
// v49: IPluginUIEvents::RegisterOnConfigChanged/UnregisterOnConfigChanged now
//      take a leading `const IPluginSelf* self` parameter, matching the
//      self-first convention already used by IPluginLogger/IPluginConfig.
//      self is the same pointer received in PluginInit -- pass it straight
//      through, no plugin-name string needed. The modloader uses it to scope
//      FireConfigChanged so a plugin's callback only fires for edits to its
//      own config file, instead of broadcasting every plugin's config
//      changes to every registered listener. Breaking signature change:
//      MIN bumped to 49.

// v50 - 29/07/2026: Added IPluginDebugDraw, reachable as hooks->HUD->DebugDraw
//      (client only -- the whole HUD interface is null on server/generic).
//      Reimplements all sixteen UKismetSystemLibrary::DrawDebug* nodes, which
//      are dead on this shipping build: ENABLE_DRAW_DEBUG is 0, so every body
//      compiled away to nothing and only the exec thunks survive, parsing
//      their parameters off the FFrame and returning without drawing. The
//      DrawDebugType pin on the trace nodes is inert for the same reason. What
//      is still alive is the renderer -- UWorld::LineBatchers[4] (all four
//      NewObject'd and registered by UWorld::UpdateWorldComponents) and
//      ULineBatchComponent::DrawLines -- so this builds the same geometry and
//      hands it to the engine's own batchers via a new AOB-scanned hook module
//      (hooks/game/debug_draw/).
//      Covers Line, Point, Circle, Sphere, Box, Capsule, Cylinder,
//      ConeInDegrees, Arrow, CoordinateSystem, Plane, Frustum, Camera (both an
//      ACameraActor* form and a location/rotation/FOV form), String,
//      FloatHistoryTransform and FloatHistoryLocation, plus FlushPersistentLines
//      and ClearAllStrings.
//      DrawString takes a trailing fontScale (1.0f = the engine's small font at
//      native size; the engine applies it with no distance falloff). Added
//      while v50 was still unreleased, so the signature was amended in place
//      rather than appending a second function -- no plugin has been built
//      against the earlier shape.
//      Two primitives deviate from the engine because only DrawLines survived
//      as an out-of-line function: DrawPoint (engine uses BatchedPoints) is a
//      three-axis cross, and DrawPlane / the float-history graphs (engine uses
//      BatchedMeshes) draw outlines instead of filled quads.
//      Callable from any thread: the batchers are game-thread only, but the
//      wrappers copy every argument and defer to GameThreadDispatch when the
//      caller is elsewhere, so drawing from an ImGui panel callback (which
//      runs on the render thread) just works.
//      The new field is appended at the end of IPluginHUDEvents and all the
//      geometry structs are new, so nothing existing shifts: MIN remains 49.

// v51: Added IPluginUIEvents::AcquireInputPassthrough / ReleaseInputPassthrough
//      -- a cooperative counterpart to the v43 AcquireInputCapture pair.
//      AcquireInputCapture is all-or-nothing: while a token is held the game
//      receives no mouse or keyboard input at all. That is right for a modal
//      settings panel and wrong for an always-on widget UI the player is meant
//      to keep playing underneath (a timeline editor, a build planner, an
//      overlay with draggable handles) -- those currently freeze the player out
//      until the window is closed. A passthrough token instead puts the
//      modloader in cooperative mode: ImGui is fed every message and draws and
//      owns the cursor exactly as before, but the game is only cut out of the
//      input classes ImGui actually wants that frame -- mouse while the cursor
//      is over an ImGui window or dragging a widget (io.WantCaptureMouse), and
//      keyboard while a text field has focus (io.WantCaptureKeyboard /
//      WantTextInput). Everything else, including the WM_INPUT raw-mouse deltas
//      UE5 uses for camera look, reaches the game untouched.
//      Exclusive capture always wins: while any modloader window, plugin panel
//      or v43 capture token is active, passthrough tokens have no effect and
//      behaviour is exactly what it was before. Tokens are refcounted across
//      all plugins the same way, and must be released in PluginShutdown.
//      Appended at the end of IPluginUIEvents, so nothing existing shifts:
//      MIN remains 49.

// v52: Added IModLoaderImGui::GetMouseWheel / GetMouseWheelH. The v36 mouse
//      query block covers buttons, position, dragging and hovering but never
//      exposed the wheel, so a plugin drawing its own scrollable or zoomable
//      surface (a timeline, a map, a graph) had no way to read it at all --
//      ImGui consumes the WM_MOUSEWHEEL itself and the plugin never sees the
//      message. Both return the per-frame delta straight off ImGuiIO, in the
//      usual ImGui units (one notch = 1.0). Appended at the end of the
//      function table, which is append-only: MIN remains 49.

// v53: IPluginNetworkChannel now works on a listen host (a client build that is
//      hosting the session).  Previously SendPacketToClient, SendPacketToAllClients,
//      RegisterServerMessageHandler and ExcludeFromBroadcast were compiled as
//      unconditional no-ops on client builds, so a listen host could not talk to
//      its own clients at all -- only a dedicated server build could.  Authority is
//      a runtime property, not a build-time one, so all four are now real on client
//      builds and gated per call on the net mode (ListenServer or DedicatedServer).
//
//      Behaviour change, no layout change: IsServer() now reports net authority
//      rather than "was this the server build". It was hardcoded false on client
//      builds; it returns true on a listen host and stays false on a pure client
//      and in Standalone. This is the point of the change -- the natural guard
//      `if (Network->IsServer()) Broadcast(...)` starts working on a listen host
//      without touching plugin code. A plugin that used IsServer() to ask which
//      DLL it was loaded into should use PluginInfo::pluginTarget instead, or
//      hooks->NetMode->GetNetMode() for the full four-way answer.
//
//      On a listen host both send functions skip the host's own player controller:
//      an RPC aimed at it executes in-process rather than going over the wire, so
//      it would loop back into this same process's client handlers (and arrive
//      twice). "All clients" means all remote clients on both builds. Host-side
//      code already holds the authoritative data and can call its own handler
//      directly. No struct changed: MIN remains 49.

// v54: Plugin networking moved onto the Unreal control channel and the previous
//      transport was DELETED, not deprecated. Envelopes used to ride two game
//      RPCs -- ClientSaveStringToTxt (server->client) and ServerExecuteConsoleCommand
//      (client->server); both hooks are gone. IPluginNetworkChannel is unchanged
//      in shape and every function behaves the same from a plugin's point of view.
//
//      Two consequences plugins can observe:
//
//      - IPluginNativePointers::ClientMessageExec is now ALWAYS nullptr, on every
//        build, because the hook it exposed no longer exists. The field is kept so
//        the struct layout does not shift under already-built plugins. Nothing
//        else in that struct moved.
//
//      - There is no fallback transport and no capability negotiation, and one
//        cannot be added: asking a peer whether it speaks the wire would itself
//        have to travel over the wire. Sending to a peer whose loader lacks a
//        working control channel does not degrade -- the engine closes that
//        connection on an unrecognised control message. Every peer in a session
//        must therefore run a loader whose control-channel patterns resolved --
//        which is why those patterns are REQUIRED at preflight: a game update
//        that moves one disables the entire mod loader rather than starting a
//        session where networked plugins are silently inert. Plugins do not need
//        to handle a "networking unavailable" state, because there is no build
//        in which the loader runs without it.
//
//      No struct changed: MIN remains 49.

// v55: Server->client sends are now gated on a client plugin manifest. A joining
//      client reports every plugin it has loaded, with versions; the authority
//      records that per connection and delivers a packet from plugin P version V
//      only to clients that reported P at exactly V.
//
//      No signature changed -- SendPacketToClient and SendPacketToAllClients
//      already took `const IPluginSelf* self`, which carries both name and
//      version, so the loader could already attribute every packet to its sender.
//
//      What changed for plugin authors, and it is worth reading twice:
//
//      - SendPacketToAllClients no longer means "every client". It means every
//        client that has your plugin at your exact version. A version bump on one
//        side silently stops delivery between the two -- which is the point, since
//        two builds disagreeing about their own packet layout is the failure this
//        prevents, but it will look like the network dropped your packet.
//        Skipped clients are logged at Debug/Trace naming the plugin and version.
//
//      - A client that has not (yet) reported receives nothing. A manifest is sent
//        at world begin play and retried before the client's first outgoing
//        packet, so there is a window early in a join where a broadcast reaches
//        nobody. Send state on request, or after your own client-ready signal,
//        rather than assuming a broadcast at join lands.
//
//      - This is also a safety improvement: a client not running the loader can
//        never report, so it can never be sent a control bunch, so it can no
//        longer be disconnected by one (see v54).
//
//      Operators can inspect all of this with the `clients` console command.
//      No struct changed: MIN remains 49.

// v56 (2026-08-05): Handshake before the wire, and a client-ready signal.
//
//      THE BUG THIS FIXES: a client sent its manifest the moment it had a world,
//      unprompted. On a server not running the loader that control bunch is an
//      unrecognised message type, and the engine's response to one is to close
//      the connection (ControlChannelMessageUnknown). Joining a vanilla server
//      with the loader installed therefore disconnected you.
//
//      v54 claimed capability negotiation was impossible. That was true only
//      IN-BAND: a probe sent as a control bunch is the very thing that kills a
//      vanilla peer. Out of band it works fine. The authority now greets each
//      joining client through APlayerController::ClientMessage -- an ordinary
//      replicated engine RPC, which a client without the loader hands to a
//      viewport console that shipping builds never create, so it does nothing at
//      all there. Nothing is put on the control channel in either direction until
//      that greeting has been seen, so no peer can be disconnected by us again.
//
//      Plugins do not see the handshake. What they see is that a client is now
//      "ready" at a defined moment, and that moment is LATER than player-join:
//
//      - IsClientReady(pc, self) is exactly the predicate SendPacketToClient
//        gates on -- that client reported YOUR plugin at YOUR version. It is
//        false during the join window even for a client that will be ready a
//        moment later.
//
//      - RegisterClientReadyCallback fires once per client, per plugin, when that
//        becomes true. Send join-time state from there, NOT from a player-joined
//        hook: a packet sent at PostLogin is dropped, because the client has not
//        reported yet. Registering late is safe -- the callback fires immediately
//        for every client already ready, so a hot-reloaded plugin still learns
//        about the clients already in the session.
//
//      - IsServerReady / RegisterServerReadyCallback are the client-side mirror:
//        true once the authority has acknowledged our manifest, which is the
//        point from which SendPacketToServer can actually arrive. The callback
//        carries the authority's loader build tag.
//
//      The loader deliberately does NOT buffer and replay packets sent before a
//      peer is ready. It cannot: payloads are opaque bytes, so it cannot tell a
//      stale position update (worse than useless when replayed three seconds
//      late) from state that is still valid. Sends to a peer that is not ready
//      are dropped, and now warn once per plugin per connection instead of
//      logging at Debug -- if you see that warning, move the send into the ready
//      callback.
//

// v62 (2026-09-02): AOB scanning moved into its own load phase, and out of
//      IPluginSelf. BREAKING -- MIN and MAX both move to 62.
//
//      THE PROBLEM: a plugin resolved its patterns from PluginInit, which runs
//      long after the loader finished its own preflight and installed its own
//      hooks. A plugin whose AOB stopped matching after a game update therefore
//      loaded anyway, then either did nothing or wrote a detour over whatever
//      now lived at the address it guessed. The loader had no idea, and neither
//      did the user, until something behaved strangely mid-session.
//
//      WHAT CHANGED:
//
//      - IPluginSelf::scanner is GONE, and so is IPluginScanner. This is the
//        breaking part: a plugin built against v61 reads self->hooks out of
//        what is now a shorter struct. Every plugin must be rebuilt.
//
//      - New optional export OnPluginLoadHooks(IPluginSelf*, IPluginHookScanner*)
//        (PLUGIN_LOAD_HOOKS_FUNC_NAME). It runs after GetPluginInfo and before
//        PluginInit, and the IPluginHookScanner it is handed is the only way to
//        pattern scan. That table refuses calls made outside the event, so
//        stashing it does not buy a plugin its old behaviour back.
//
//      - Resolve* names each address. ANY MISS REFUSES THE PLUGIN -- one is
//        enough, and required vs optional does not change that. The loader
//        skips PluginInit, frees the DLL, and adds the miss to a report shown
//        in a popup (client) and by the hookfailures console command (both),
//        with a copy-to-clipboard button so a user can hand a plugin author
//        something actionable.
//
//        ResolveOptional / ReportWarning still exist and still mean something
//        to the plugin -- optional is the resolve whose null return the plugin
//        is expected to handle rather than assume -- but they are a LABEL on
//        the report, not a lighter verdict. A plugin that asked the loader for
//        an address and did not get it is a plugin running against a build it
//        was not made for, and which half of it still works is not a question
//        the loader (or the user staring at the game) can answer.
//
//      - self->hooks is null for the duration of the event, on purpose. A
//        plugin that misses a pattern is unloaded, so anything it registered or
//        detoured during the event would be left pointing into a freed module.
//        Resolve in the event, install in PluginInit.


// v63 (2026-09-04): Added IPluginConsole (hooks->Console) -- ModConsole command
//      registration and output sinks, on every build. Purely additive: the
//      field is appended at the end of IPluginHooks and nothing existing
//      shifts, so MIN remains 62.
//
//      ModConsole is the loader's own command registry -- one registry, two
//      front-ends (the ImGui developer console on client builds, the -console
//      window on any build). It was internal, so a plugin had no way to add a
//      command to either, and no way to get text back out of one: every engine
//      console function on a dedicated server returns void, and the only
//      text-returning path (APlayerController::ConsoleCommand) is client-only.
//
//      RegisterCommand adds a named command, with aliases, usage/help text for
//      `help`, an opaque userData, and a gameThread flag that makes the loader
//      run the handler on the next engine tick instead of on whichever console
//      thread typed it -- set it for anything touching engine state.
//
//      A handler is handed an opaque PluginConsoleSink and writes through
//      Write/Printf/Clear. Output is line-oriented and typed (Output / Notice /
//      Error) so the front-end can colour it, and the same command prints the
//      same lines wherever it was typed.
//
//      Execute runs any command line -- yours, another plugin's, or a built-in
//      -- and delivers its output to your own callback instead of to a console
//      window, which is what makes a command's result available to something
//      that is not a person looking at a screen (an RCON bridge, an HTTP
//      route). It returns false if the first token is not a registered command.
//
//      Three rules, each of which is a crash the loader had to make impossible:
//
//      - A sink is valid ONLY inside the call it was handed to. Do not stash it
//        and write from a thread later: the front-end owns it, and by then it
//        may be gone. Writes through a stale sink are dropped with a log line.
//
//      - Unregister in PluginShutdown. Handler addresses point into your
//        module; the loader drops every command a plugin owns before freeing
//        it and re-validates a queued gameThread handler before calling it,
//        but a command that outlives its DLL is not a state worth reaching.
//
//      - Output can arrive on the game thread. A gameThread command's handler,
//        and the Execute callbacks for one, run on a later tick, from a
//        different thread than the one that called Execute. Both must be
//        thread-safe, and userData must outlive the call.
//
//      Names are case-insensitive and global. RegisterCommand returns false
//      rather than shadowing a built-in or another plugin's command, so check
//      it -- and prefix anything generic with your plugin's name.

#define PLUGIN_INTERFACE_VERSION_MIN 63
#define PLUGIN_INTERFACE_VERSION_MAX 63
#define PLUGIN_INTERFACE_VERSION 63

enum class PluginLogLevel { Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4 };
enum class ConfigValueType { String, Integer, Float, Boolean, Keybind };

// Network mode — mirrors the engine's ENetMode enum, letting plugins compare
// against nicely-typed names instead of raw integers when querying GetNetMode().
//   Standalone      = solo/offline, no networking
//   DedicatedServer = headless server, no local players
//   ListenServer    = server with a local player
//   Client          = connected to a remote server
enum class EPluginNetMode : uint8_t
{
    Standalone      = 0,
    DedicatedServer = 1,
    ListenServer    = 2,
    Client          = 3,
    Unknown         = 255
};

struct ConfigEntry
{
	const char* section;
	const char* key;
	ConfigValueType type;
	const char* defaultValue;
	const char* description;
	float rangeMin;
	float rangeMax;
};

struct ConfigSchema { const ConfigEntry* entries; int entryCount; };

struct IPluginSelf; // forward — defined after all sub-interfaces

struct IPluginLogger
{
	void (*Log)(PluginLogLevel level, const IPluginSelf* self, const char* message);
	void (*Trace)(const IPluginSelf* self, const char* format, ...);
	void (*Debug)(const IPluginSelf* self, const char* format, ...);
	void (*Info) (const IPluginSelf* self, const char* format, ...);
	void (*Warn) (const IPluginSelf* self, const char* format, ...);
	void (*Error)(const IPluginSelf* self, const char* format, ...);
};

struct IPluginConfig
{
	bool  (*ReadString)(const IPluginSelf* self, const char* section, const char* key, char* outValue, int maxLen, const char* defaultValue);
	bool  (*WriteString)(const IPluginSelf* self, const char* section, const char* key, const char* value);
	int   (*ReadInt)(const IPluginSelf* self, const char* section, const char* key, int defaultValue);
	bool  (*WriteInt)(const IPluginSelf* self, const char* section, const char* key, int value);
	float (*ReadFloat)(const IPluginSelf* self, const char* section, const char* key, float defaultValue);
	bool  (*WriteFloat)(const IPluginSelf* self, const char* section, const char* key, float value);
	bool  (*ReadBool)(const IPluginSelf* self, const char* section, const char* key, bool defaultValue);
	bool  (*WriteBool)(const IPluginSelf* self, const char* section, const char* key, bool value);
	bool  (*InitializeFromSchema)(const IPluginSelf* self, const ConfigSchema* schema);
	void  (*ValidateConfig)(const IPluginSelf* self, const ConfigSchema* schema);
};

struct PluginXRef { uintptr_t address; bool isRelative; };

// ---------------------------------------------------------------------------
// IPluginHookScanner -- pattern scanning, and the ONLY place a plugin gets it.
//
// A pointer to this table is handed to the optional OnPluginLoadHooks export
// (see PLUGIN_LOAD_HOOKS_FUNC_NAME at the bottom of this header) and is valid
// only for the duration of that call. Every function takes the same self the
// event was called with, and the loader rejects the call -- returning 0 and
// logging an error -- if that plugin has no scan session open. Stashing the
// table pointer and scanning later therefore does not work, by design: the
// loader has to know that every AOB a plugin depends on resolved BEFORE it
// decides whether to run PluginInit at all.
//
// Resolve* records the outcome under hookName, which is what the failure report
// and the log show the user -- name it after the thing being hooked
// ("UCrCraftingComponent::FinishCrafting"), not after the pattern.
//
// A MISS REFUSES THE PLUGIN, required or optional, and one is enough:
// PluginInit is never called, the DLL is freed, and every miss is listed in the
// failure window. The two flavours differ only in the label the report shows:
//
//   Required -- this plugin cannot work without the address.
//   Optional -- the plugin handles a null return rather than assuming one, so
//               it will not fault on it. It still does not load: an address
//               that stopped resolving means this build of the plugin does not
//               match this build of the game, and neither the loader nor the
//               user can tell which parts of it are still doing what they think.
//
// So resolve everything you intend to use, and mark optional only what your own
// code genuinely null-checks -- it makes the report readable for whoever has to
// fix it, and it keeps the plugin from faulting inside the event itself.
//
// RESOLVE ONLY -- DO NOT INSTALL. OnPluginLoadHooks runs before the plugin is
// committed to, and the loader frees the DLL when any pattern misses. A detour
// written into game code during this event would then be a jump into a freed
// module. Store the addresses in your own statics here and install from
// PluginInit, which only runs once every pattern resolved. That is also why
// self->hooks is null for the duration of the event.
// ---------------------------------------------------------------------------
struct IPluginHookScanner
{
	// Main module (the game .exe). Returns 0 on a miss.
	uintptr_t (*ResolveRequired)(const IPluginSelf* self, const char* hookName, const char* pattern);
	uintptr_t (*ResolveOptional)(const IPluginSelf* self, const char* hookName, const char* pattern);

	// Same, scoped to one loaded module.
	uintptr_t (*ResolveRequiredInModule)(const IPluginSelf* self, const char* hookName, HMODULE module, const char* pattern);
	uintptr_t (*ResolveOptionalInModule)(const IPluginSelf* self, const char* hookName, HMODULE module, const char* pattern);

	// Several candidate patterns; succeeds only when exactly one of them matches
	// exactly once. outPatternIndex (may be null) receives which one did.
	uintptr_t (*ResolveRequiredUnique)(const IPluginSelf* self, const char* hookName, const char** patterns, int patternCount, int* outPatternIndex);
	uintptr_t (*ResolveOptionalUnique)(const IPluginSelf* self, const char* hookName, const char** patterns, int patternCount, int* outPatternIndex);

	// Raw scans, for resolving an address in steps (find a string reference,
	// walk back to the function that uses it, and so on). These record nothing
	// on their own: report the outcome yourself with ReportFailure /
	// ReportWarning, or feed the result into a Resolve* call that does.
	int (*FindAllPatternsInMainModule)(const IPluginSelf* self, const char* pattern, uintptr_t* outAddresses, int maxResults);
	int (*FindAllPatternsInModule)(const IPluginSelf* self, HMODULE module, const char* pattern, uintptr_t* outAddresses, int maxResults);
	int (*FindXrefsToAddress)(const IPluginSelf* self, uintptr_t targetAddress, uintptr_t start, size_t size, PluginXRef* outXRefs, int maxResults);
	int (*FindXrefsToAddressInModule)(const IPluginSelf* self, uintptr_t targetAddress, HMODULE module, PluginXRef* outXRefs, int maxResults);
	int (*FindXrefsToAddressInMainModule)(const IPluginSelf* self, uintptr_t targetAddress, PluginXRef* outXRefs, int maxResults);

	// Record something the plugin worked out for itself -- a vtable slot that
	// held an unexpected value, an offset that failed a sanity check, a
	// multi-step resolve that ended nowhere. Both refuse the plugin, exactly
	// like a missed pattern; the only difference is whether the report line
	// reads [required] or [optional].
	void (*ReportFailure)(const IPluginSelf* self, const char* hookName, const char* detail);
	void (*ReportWarning)(const IPluginSelf* self, const char* hookName, const char* detail);

	// True once anything at all has missed this session. Lets a plugin skip the
	// rest of a resolve chain it already knows cannot work -- it does not change
	// the outcome, which is decided when the event returns.
	bool (*HasFailures)(const IPluginSelf* self);
};

typedef void* HookHandle;
namespace SDK { class UWorld; class UTexture2D; }

// ---------------------------------------------------------------------------
// Callback typedefs (v14+)
// ---------------------------------------------------------------------------
typedef void (*PluginEngineInitCallback)();
typedef void (*PluginEngineShutdownCallback)();
typedef void (*PluginEngineTickCallback)(float deltaSeconds);
typedef void (*PluginWorldBeginPlayCallback)(SDK::UWorld* world);
typedef void (*PluginAnyWorldBeginPlayCallback)(SDK::UWorld* world, const char* worldName);
typedef void (*PluginSaveLoadedCallback)();
typedef void (*PluginExperienceLoadCompleteCallback)();
typedef void (*PluginActorBeginPlayCallback)(void* actor);
typedef void (*PluginPlayerJoinedCallback)(void* playerController);
typedef void (*PluginPlayerLeftCallback)(void* exitingController);
typedef void (*PluginWorldEndPlayCallback)(SDK::UWorld* world, const char* worldName);
typedef void (*PluginHUDPostRenderCallback)(void* hud);
typedef void (*PluginNetworkMessageCallback)(const char* pluginName, const char* typeTag, const uint8_t* data, size_t size);
typedef void (*PluginNetworkServerMessageCallback)(void* senderPlayerController, const char* pluginName, const char* typeTag, const uint8_t* data, size_t size);

// v56 -- authority side. Fired once per client, per registered plugin, at the
// moment that client becomes a valid target for THAT plugin: it has reported the
// plugin at the registering plugin's exact version, and its player controller
// exists. This is strictly later than a player-joined hook; a packet sent from
// player-joined is dropped, one sent from here is not.
typedef void (*PluginClientReadyCallback)(void* playerController);

// v56 -- client side. Fired once per connection when the authority acknowledges
// our manifest, which is the point from which SendPacketToServer can arrive.
// serverBuildTag is the authority's loader build tag ("dev" on local builds).
typedef void (*PluginServerReadyCallback)(const char* serverBuildTag);
typedef void (*PluginGameThreadCallback)(void* context);

// v44 -- fired whenever any crafting building finishes crafting an item.
// crafter           : the ACrCrafter* (or subclass) that finished crafting, as void*
// craftingComponent : the building's UCrCraftingComponent*, as void* (may be null)
// entityIndex       : FMassEntityHandle::Index for the crafter's Mass entity
// entitySerial      : FMassEntityHandle::SerialNumber for the crafter's Mass entity
typedef void (*PluginCraftingFinishedCallback)(void* crafter, void* craftingComponent,
                                                int32_t entityIndex, int32_t entitySerial);

// v47 -- Hooks::DelegateHook. Splices a synthetic UFunction into an existing
// UE5 TMulticastInlineDelegate<...> (any arity) without touching any real
// UClass's FuncMap/AllFunctionsCache, so other code resolving the same
// function name by name is never affected. Deliberately parameterless --
// see IPluginDelegateHook below.
typedef uint64_t DelegateHookHandle;
typedef void (*PluginDelegateCallback)(void* userContext);

typedef bool (*PluginBeforeActivateSpawnerCallback)(void* spawner, bool bDisableAggroLock);
typedef void (*PluginAfterActivateSpawnerCallback)(void* spawner, bool bDisableAggroLock);
typedef bool (*PluginBeforeDeactivateSpawnerCallback)(void* spawner, bool bPermanently);
typedef void (*PluginAfterDeactivateSpawnerCallback)(void* spawner, bool bPermanently);
typedef bool (*PluginBeforeDoSpawningCallback)(void* spawner);
typedef void (*PluginAfterDoSpawningCallback)(void* spawner);

// ---------------------------------------------------------------------------
// Sub-interface structs (v14)
// ---------------------------------------------------------------------------
struct IPluginHookUtils
{
	HookHandle (*Install)(uintptr_t targetAddress, void* detourFunction, void** originalFunction);
	void       (*Remove)(HookHandle handle);
	bool       (*IsInstalled)(HookHandle handle);
};

struct IPluginMemoryUtils
{
	bool  (*Patch)(uintptr_t address, const uint8_t* data, size_t size);
	bool  (*Nop)(uintptr_t address, size_t size);
	bool  (*Read)(uintptr_t address, void* buffer, size_t size);
	void* (*Alloc)(size_t count, uint32_t alignment);
	void  (*Free)(void* ptr);
	bool  (*IsAllocatorAvailable)();
};

struct IPluginEngineEvents
{
	void      (*RegisterOnInit)(PluginEngineInitCallback);
	void      (*UnregisterOnInit)(PluginEngineInitCallback);
	void      (*RegisterOnShutdown)(PluginEngineShutdownCallback);
	void      (*UnregisterOnShutdown)(PluginEngineShutdownCallback);
	void      (*RegisterOnTick)(PluginEngineTickCallback);
	void    (*UnregisterOnTick)(PluginEngineTickCallback);
	uintptr_t (*GetStaticLoadObjectAddress)();           // v16
	void      (*PostToGameThread)(PluginGameThreadCallback fn, void* context); // v18

	// v36 -- resolved addresses of CoreUObject object/package lookup and loading
	// functions, scanned during early modloader startup. Each returns 0 if the
	// AOB scan failed to resolve the function on the running build.
	//
	//   GetStaticFindObjectByPathAddress     -> UObject* __fastcall StaticFindObject(UClass*, FTopLevelAssetPath*, bool)
	//   GetStaticFindObjectByNameAddress     -> UObject* __fastcall StaticFindObject(UClass*, UObject*, const wchar_t*, bool)
	//   GetStaticFindObjectSafeByPathAddress -> UObject* __fastcall StaticFindObjectSafe(UClass*, FTopLevelAssetPath*, bool)
	//   GetStaticFindObjectSafeByNameAddress -> UObject* __fastcall StaticFindObjectSafe(UClass*, UObject*, const wchar_t*, bool)
	//   GetStaticFindObjectFastAddress       -> UObject* __fastcall StaticFindObjectFast(UClass*, UObject*, FName, bool, EObjectFlags, EInternalObjectFlags)
	//   GetFindPackageAddress                -> UPackage* __fastcall FindPackage(UObject*, const wchar_t*)
	//   GetPackageFullyLoadAddress           -> void __fastcall UPackage::FullyLoad(UPackage*)
	//   GetLoadPackageAddress                -> UPackage* __fastcall LoadPackage(UPackage*, FScriptContainerElement*, unsigned int, FArchive*, const FLinkerInstancingContext*)
	//   GetAssetDataFastGetAssetAddress      -> UObject* __fastcall FAssetData::FastGetAsset(FAssetData*, bool, TMap<FName,FName,FDefaultSetAllocator,TDefaultMapHashableKeyFuncs<FName,FName,0>>*)
	uintptr_t (*GetStaticFindObjectByPathAddress)();
	uintptr_t (*GetStaticFindObjectByNameAddress)();
	uintptr_t (*GetStaticFindObjectSafeByPathAddress)();
	uintptr_t (*GetStaticFindObjectSafeByNameAddress)();
	uintptr_t (*GetStaticFindObjectFastAddress)();
	uintptr_t (*GetFindPackageAddress)();
	uintptr_t (*GetPackageFullyLoadAddress)();
	uintptr_t (*GetLoadPackageAddress)();
	uintptr_t (*GetAssetDataFastGetAssetAddress)();
};

struct IPluginWorldEvents
{
	void (*RegisterOnWorldBeginPlay)(PluginWorldBeginPlayCallback);
	void (*UnregisterOnWorldBeginPlay)(PluginWorldBeginPlayCallback);
	void (*RegisterOnAnyWorldBeginPlay)(PluginAnyWorldBeginPlayCallback);
	void (*UnregisterOnAnyWorldBeginPlay)(PluginAnyWorldBeginPlayCallback);
	void (*RegisterOnSaveLoaded)(PluginSaveLoadedCallback);
	void (*UnregisterOnSaveLoaded)(PluginSaveLoadedCallback);
	void (*RegisterOnExperienceLoadComplete)(PluginExperienceLoadCompleteCallback);
	void (*UnregisterOnExperienceLoadComplete)(PluginExperienceLoadCompleteCallback);
	void (*RegisterOnBeforeWorldEndPlay)(PluginWorldEndPlayCallback);   // v20
	void (*UnregisterOnBeforeWorldEndPlay)(PluginWorldEndPlayCallback); // v20
	void (*RegisterOnAfterWorldEndPlay)(PluginWorldEndPlayCallback);    // v20
	void (*UnregisterOnAfterWorldEndPlay)(PluginWorldEndPlayCallback);  // v20
};

struct IPluginPlayerEvents
{
	void (*RegisterOnPlayerJoined)(PluginPlayerJoinedCallback);
	void (*UnregisterOnPlayerJoined)(PluginPlayerJoinedCallback);
	void (*RegisterOnPlayerLeft)(PluginPlayerLeftCallback);
	void (*UnregisterOnPlayerLeft)(PluginPlayerLeftCallback);
};

struct IPluginActorEvents
{
	void (*RegisterOnActorBeginPlay)(PluginActorBeginPlayCallback);
	void (*UnregisterOnActorBeginPlay)(PluginActorBeginPlayCallback);
};

// v44 -- crafting building events.
struct IPluginCraftingEvents
{
	// Fired by ACrCrafter::OnItemCraftingComplete whenever any crafting building
	// (Crafter, Forge, Refinery, Factory, Assembler, Exporter, FoodProcessor,
	// ItemPrinter, etc.) finishes crafting an item.
	void (*RegisterOnCraftingFinished)(PluginCraftingFinishedCallback);
	void (*UnregisterOnCraftingFinished)(PluginCraftingFinishedCallback);
};

struct IPluginSpawnerHooks
{
	void (*RegisterOnBeforeActivate)(PluginBeforeActivateSpawnerCallback callback);
	void (*UnregisterOnBeforeActivate)(PluginBeforeActivateSpawnerCallback callback);
	void (*RegisterOnAfterActivate)(PluginAfterActivateSpawnerCallback callback);
	void (*UnregisterOnAfterActivate)(PluginAfterActivateSpawnerCallback callback);
	void (*RegisterOnBeforeDeactivate)(PluginBeforeDeactivateSpawnerCallback callback);
	void (*UnregisterOnBeforeDeactivate)(PluginBeforeDeactivateSpawnerCallback callback);
	void (*RegisterOnAfterDeactivate)(PluginAfterDeactivateSpawnerCallback callback);
	void (*UnregisterOnAfterDeactivate)(PluginAfterDeactivateSpawnerCallback callback);
	void (*RegisterOnBeforeDoSpawning)(PluginBeforeDoSpawningCallback callback);
	void (*UnregisterOnBeforeDoSpawning)(PluginBeforeDoSpawningCallback callback);
	void (*RegisterOnAfterDoSpawning)(PluginAfterDoSpawningCallback callback);
	void (*UnregisterOnAfterDoSpawning)(PluginAfterDoSpawningCallback callback);
};

// ---------------------------------------------------------------------------
// Input (v15, client only)
// ---------------------------------------------------------------------------
enum class EModKey : uint32_t
{
	F1 = 0, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
	A, B, C, D, E, F, G, H, I, J, K, L, M,
	N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
	Zero, One, Two, Three, Four, Five, Six, Seven, Eight, Nine,
	Escape, Tab, CapsLock, SpaceBar, Enter, BackSpace, Delete, Insert,
	LeftShift, RightShift, LeftControl, RightControl, LeftAlt, RightAlt,
	Up, Down, Left, Right, Home, End, PageUp, PageDown,
	Tilde, Hyphen, Equals, LeftBracket, RightBracket, Backslash,
	Semicolon, Apostrophe, Comma, Period, Slash,
	NumPadZero, NumPadOne, NumPadTwo, NumPadThree, NumPadFour,
	NumPadFive, NumPadSix, NumPadSeven, NumPadEight, NumPadNine,
	Add, Subtract, Multiply, Divide, Decimal,
	LeftMouseButton, RightMouseButton, MiddleMouseButton,
	ThumbMouseButton, ThumbMouseButton2,
	Unknown
};

enum class EModKeyEvent : uint32_t { Pressed = 0, Released = 1 };

// Modifier bitmask used with combo keybinds (v28).
// Each flag covers both Left and Right variants of that modifier key.
enum EModKeyModifiers : uint32_t
{
	EModKeyMod_None  = 0,
	EModKeyMod_Ctrl  = 1 << 0,
	EModKeyMod_Shift = 1 << 1,
	EModKeyMod_Alt   = 1 << 2,
};

typedef void (*PluginKeybindCallback)(EModKey key, EModKeyEvent event);

// Combo callback — receives the base key, the modifier bitmask held at the
// moment of the transition, and the event type (v28).
typedef void (*PluginKeybindComboCallback)(EModKey key, EModKeyModifiers mods, EModKeyEvent event);

struct IPluginInputEvents
{
	// v15 — register by enum; fires on key transition regardless of modifier state.
	void (*RegisterKeybind)(EModKey key, EModKeyEvent event, PluginKeybindCallback callback);
	void (*UnregisterKeybind)(EModKey key, EModKeyEvent event, PluginKeybindCallback callback);

	// v15/v30 — register by name string.  Accepts plain key names ("F5") and
	// combo strings ("Ctrl+C", "Shift+F5", "Ctrl+Shift+Delete").  Modifier tokens
	// are case-insensitive.  The callback receives the base key and event; the
	// modifiers are implicit in the name you registered.
	// The modloader tracks these registrations and automatically re-registers
	// the keybind when the user rebinds it in the plugin config UI.
	void (*RegisterKeybindByName)(const char* combo, EModKeyEvent event, PluginKeybindCallback callback);
	void (*UnregisterKeybindByName)(const char* combo, EModKeyEvent event, PluginKeybindCallback callback);

	// v28 — advanced: register by enum + explicit modifier mask.  Fires only when
	// the key transitions with exactly those modifiers held.  The callback receives
	// the modifier mask at fire time.  Use this only when you need the mods passed
	// back; most plugins should use RegisterKeybindByName instead.
	void (*RegisterKeybindCombo)(EModKey key, EModKeyModifiers mods, EModKeyEvent event, PluginKeybindComboCallback callback);
	void (*UnregisterKeybindCombo)(EModKey key, EModKeyModifiers mods, EModKeyEvent event, PluginKeybindComboCallback callback);
};

// Opaque handle for a texture registered through IPluginImGuiTextures (v37).
// Forward-declared here (fully defined again, identically, near
// IPluginImGuiTextures below) so IModLoaderImGui's draw-list Image* functions
// can take one without reordering the whole header.
typedef void* PluginTextureHandle;

// Opaque handle for an ImDrawList*, returned by GetWindowDrawList /
// GetBackgroundDrawList / GetForegroundDrawList (v48). Only valid for the
// duration of the current frame's render callback -- do not cache across
// frames.
typedef void* PluginDrawList;

// Flags for the rounding-corner parameters below. Mirrors ImDrawFlags.
#define PluginDrawFlags_None                     0
#define PluginDrawFlags_Closed                    (1 << 0)
#define PluginDrawFlags_RoundCornersTopLeft        (1 << 4)
#define PluginDrawFlags_RoundCornersTopRight        (1 << 5)
#define PluginDrawFlags_RoundCornersBottomLeft      (1 << 6)
#define PluginDrawFlags_RoundCornersBottomRight     (1 << 7)
#define PluginDrawFlags_RoundCornersNone           (1 << 8)
#define PluginDrawFlags_RoundCornersTop            (PluginDrawFlags_RoundCornersTopLeft | PluginDrawFlags_RoundCornersTopRight)
#define PluginDrawFlags_RoundCornersBottom         (PluginDrawFlags_RoundCornersBottomLeft | PluginDrawFlags_RoundCornersBottomRight)
#define PluginDrawFlags_RoundCornersLeft           (PluginDrawFlags_RoundCornersBottomLeft | PluginDrawFlags_RoundCornersTopLeft)
#define PluginDrawFlags_RoundCornersRight          (PluginDrawFlags_RoundCornersBottomRight | PluginDrawFlags_RoundCornersTopRight)
#define PluginDrawFlags_RoundCornersAll            (PluginDrawFlags_RoundCornersTopLeft | PluginDrawFlags_RoundCornersTopRight | PluginDrawFlags_RoundCornersBottomLeft | PluginDrawFlags_RoundCornersBottomRight)

// ---------------------------------------------------------------------------
// UI (v15–v16, client only; extended v24)
// ---------------------------------------------------------------------------
struct IModLoaderImGui
{
	void (*Text)(const char* text);
	void (*TextColored)(float r, float g, float b, float a, const char* text);
	void (*TextDisabled)(const char* text);
	void (*TextWrapped)(const char* text);
	void (*LabelText)(const char* label, const char* text);
	void (*SeparatorText)(const char* label);
	bool (*InputText)(const char* label, char* buf, size_t buf_size);
	bool (*InputInt)(const char* label, int* v, int step, int step_fast);
	bool (*InputFloat)(const char* label, float* v, float step, float step_fast, const char* format);
	bool (*Checkbox)(const char* label, bool* v);
	bool (*SliderFloat)(const char* label, float* v, float v_min, float v_max, const char* format);
	bool (*SliderInt)(const char* label, int* v, int v_min, int v_max, const char* format);
	bool (*Button)(const char* label);
	bool (*SmallButton)(const char* label);
	void (*SameLine)(float offset_from_start_x, float spacing);
	void (*NewLine)();
	void (*Separator)();
	void (*Spacing)();
	void (*Indent)(float indent_w);
	void (*Unindent)(float indent_w);
	void (*PushIDStr)(const char* str_id);
	void (*PushIDInt)(int int_id);
	void (*PopID)();
	bool (*BeginCombo)(const char* label, const char* preview_value);
	bool (*Selectable)(const char* label, bool selected);
	void (*EndCombo)();
	bool (*CollapsingHeader)(const char* label);
	bool (*TreeNodeStr)(const char* label);
	void (*TreePop)();
	bool (*ColorEdit3)(const char* label, float col[3]);
	bool (*ColorEdit4)(const char* label, float col[4]);
	void (*SetTooltip)(const char* text);
	bool (*IsItemHovered)();
	void (*SetNextItemWidth)(float item_width);

	// Font / text metrics
	float (*GetFontSize)();
	float (*GetTextLineHeight)();
	float (*GetTextLineHeightWithSpacing)();
	float (*GetFrameHeight)();
	float (*GetFrameHeightWithSpacing)();
	void  (*CalcTextSize)(const char* text, float* out_x, float* out_y, bool hide_text_after_double_hash, float wrap_width);

	// Per-window font scale (call inside your render callback)
	void  (*SetWindowFontScale)(float scale);

	// Content / display size queries
	void  (*GetContentRegionAvail)(float* out_x, float* out_y);
	void  (*GetDisplaySize)(float* out_x, float* out_y);

	// v35: Child windows
	bool (*BeginChild)(const char* id, float size_x, float size_y, bool border);
	void (*EndChild)();

	// v35: Style color / var stack
	void (*PushStyleColor)(int idx, float r, float g, float b, float a);
	void (*PopStyleColor)(int count);
	void (*PushStyleVarFloat)(int idx, float val);
	void (*PushStyleVarVec2)(int idx, float x, float y);
	void (*PopStyleVar)(int count);

	// v35: Item width / cursor control
	void  (*PushItemWidth)(float item_width);
	void  (*PopItemWidth)();
	void  (*SetCursorPosX)(float x);
	float (*GetCursorPosX)();

	// v35: Table layout
	bool (*BeginTable)(const char* id, int columns, int flags);
	void (*TableNextColumn)();
	void (*EndTable)();

	// v35: Misc queries
	bool  (*IsItemClicked)(int mouse_button);
	float (*GetWindowWidth)();
	float (*GetWindowHeight)();
	void  (*Dummy)(float size_x, float size_y);

	// -------------------------------------------------------------------------
	// v36: Window queries
	// -------------------------------------------------------------------------
	bool  (*IsWindowAppearing)();
	bool  (*IsWindowCollapsed)();
	bool  (*IsWindowFocused)(int flags);
	bool  (*IsWindowHovered)(int flags);
	void  (*GetWindowPos)(float* out_x, float* out_y);
	void  (*GetWindowSize)(float* out_x, float* out_y);
	void  (*SetNextWindowBgAlpha)(float alpha);

	// v36: Scroll
	float (*GetScrollX)();
	float (*GetScrollY)();
	void  (*SetScrollX)(float scroll_x);
	void  (*SetScrollY)(float scroll_y);
	float (*GetScrollMaxX)();
	float (*GetScrollMaxY)();
	void  (*SetScrollHereX)(float center_x_ratio);
	void  (*SetScrollHereY)(float center_y_ratio);

	// v36: Grouping / alignment
	void  (*BeginGroup)();
	void  (*EndGroup)();
	void  (*AlignTextToFramePadding)();

	// v36: Extended cursor control
	float (*GetCursorPosY)();
	void  (*SetCursorPosY)(float y);
	void  (*SetCursorPos)(float x, float y);
	void  (*GetCursorScreenPos)(float* out_x, float* out_y);
	void  (*SetCursorScreenPos)(float x, float y);
	void  (*GetCursorStartPos)(float* out_x, float* out_y);
	float (*CalcItemWidth)();
	void  (*PushTextWrapPos)(float wrap_local_pos_x);
	void  (*PopTextWrapPos)();

	// v36: Style extras (PushStyleVarX/Y from ImGui 1.87+)
	void  (*PushStyleVarX)(int idx, float val_x);
	void  (*PushStyleVarY)(int idx, float val_y);
	void  (*PushItemFlag)(int option, bool enabled);
	void  (*PopItemFlag)();

	// v36: Text helpers
	void  (*BulletText)(const char* text);
	void  (*Bullet)();

	// v36: Buttons / widgets
	bool  (*ButtonSized)(const char* label, float w, float h);
	bool  (*InvisibleButton)(const char* str_id, float w, float h);
	bool  (*ArrowButton)(const char* str_id, int dir);
	bool  (*RadioButton)(const char* label, bool active);
	bool  (*RadioButtonInt)(const char* label, int* v, int v_button);
	void  (*ProgressBar)(float fraction, float w, float h, const char* overlay);
	bool  (*CheckboxFlagsInt)(const char* label, int* flags, int flags_value);
	bool  (*SelectableFull)(const char* label, bool selected, int flags, float w, float h);
	bool  (*TextLink)(const char* label);

	// v36: Input extras
	bool  (*InputTextMultiline)(const char* label, char* buf, size_t buf_size, float w, float h);
	bool  (*InputTextWithHint)(const char* label, const char* hint, char* buf, size_t buf_size);
	bool  (*InputFloat2)(const char* label, float v[2]);
	bool  (*InputFloat3)(const char* label, float v[3]);
	bool  (*InputFloat4)(const char* label, float v[4]);
	bool  (*InputInt2)(const char* label, int v[2]);
	bool  (*InputInt3)(const char* label, int v[3]);
	bool  (*InputInt4)(const char* label, int v[4]);

	// v36: Drag widgets
	bool  (*DragFloat)(const char* label, float* v, float v_speed, float v_min, float v_max, const char* format);
	bool  (*DragFloat2)(const char* label, float v[2], float v_speed, float v_min, float v_max, const char* format);
	bool  (*DragFloat3)(const char* label, float v[3], float v_speed, float v_min, float v_max, const char* format);
	bool  (*DragFloat4)(const char* label, float v[4], float v_speed, float v_min, float v_max, const char* format);
	bool  (*DragInt)(const char* label, int* v, float v_speed, int v_min, int v_max, const char* format);
	bool  (*DragInt2)(const char* label, int v[2], float v_speed, int v_min, int v_max, const char* format);
	bool  (*DragInt3)(const char* label, int v[3], float v_speed, int v_min, int v_max, const char* format);
	bool  (*DragInt4)(const char* label, int v[4], float v_speed, int v_min, int v_max, const char* format);
	bool  (*DragFloatRange2)(const char* label, float* v_min, float* v_max, float speed, float mn, float mx, const char* format);
	bool  (*DragIntRange2)(const char* label, int* v_min, int* v_max, float speed, int mn, int mx, const char* format);

	// v36: Vertical sliders + angle slider
	bool  (*VSliderFloat)(const char* label, float w, float h, float* v, float v_min, float v_max, const char* format);
	bool  (*VSliderInt)(const char* label, float w, float h, int* v, int v_min, int v_max, const char* format);
	bool  (*SliderAngle)(const char* label, float* v_rad, float deg_min, float deg_max);

	// v36: Color pickers / button
	bool  (*ColorPicker3)(const char* label, float col[3]);
	bool  (*ColorPicker4)(const char* label, float col[4]);
	bool  (*ColorButton)(const char* desc_id, float r, float g, float b, float a, float w, float h);

	// v36: Plot
	void  (*PlotLines)(const char* label, const float* values, int values_count, int values_offset, const char* overlay, float scale_min, float scale_max, float graph_w, float graph_h);
	void  (*PlotHistogram)(const char* label, const float* values, int values_count, int values_offset, const char* overlay, float scale_min, float scale_max, float graph_w, float graph_h);

	// v36: Tree extras
	bool  (*TreeNodeExStr)(const char* label, int flags);
	void  (*TreePushStr)(const char* str_id);
	float (*GetTreeNodeToLabelSpacing)();
	void  (*SetNextItemOpen)(bool is_open, int cond);

	// v36: Listbox
	bool  (*BeginListBox)(const char* label, float w, float h);
	void  (*EndListBox)();

	// v36: Tab bar
	bool  (*BeginTabBar)(const char* str_id, int flags);
	void  (*EndTabBar)();
	bool  (*BeginTabItem)(const char* label, bool* p_open, int flags);
	void  (*EndTabItem)();
	bool  (*TabItemButton)(const char* label, int flags);

	// v36: Menu bar / menus / menu items
	bool  (*BeginMenuBar)();
	void  (*EndMenuBar)();
	bool  (*BeginMenu)(const char* label, bool enabled);
	void  (*EndMenu)();
	bool  (*MenuItem)(const char* label, const char* shortcut, bool selected, bool enabled);

	// v36: Popups
	bool  (*BeginPopup)(const char* str_id, int flags);
	bool  (*BeginPopupModal)(const char* name, bool* p_open, int flags);
	void  (*EndPopup)();
	void  (*OpenPopup)(const char* str_id, int popup_flags);
	void  (*CloseCurrentPopup)();
	bool  (*BeginPopupContextItem)(const char* str_id, int popup_flags);
	bool  (*BeginPopupContextWindow)(const char* str_id, int popup_flags);
	bool  (*IsPopupOpen)(const char* str_id, int flags);

	// v36: Tooltips
	bool  (*BeginTooltip)();
	void  (*EndTooltip)();
	bool  (*BeginItemTooltip)();
	void  (*SetItemTooltip)(const char* text);

	// v36: Table extras
	void  (*TableNextRow)(int row_flags, float min_row_height);
	bool  (*TableSetColumnIndex)(int column_n);
	void  (*TableSetupColumn)(const char* label, int flags, float init_width);
	void  (*TableSetupScrollFreeze)(int cols, int rows);
	void  (*TableHeadersRow)();
	void  (*TableHeader)(const char* label);
	int   (*TableGetColumnCount)();
	int   (*TableGetColumnIndex)();
	int   (*TableGetRowIndex)();
	const char* (*TableGetColumnName)(int column_n);
	void  (*TableSetBgColor)(int target, unsigned int color, int column_n);

	// v36: Item state predicates
	bool  (*IsItemActive)();
	bool  (*IsItemFocused)();
	bool  (*IsItemVisible)();
	bool  (*IsItemEdited)();
	bool  (*IsItemActivated)();
	bool  (*IsItemDeactivated)();
	bool  (*IsItemDeactivatedAfterEdit)();
	bool  (*IsItemToggledOpen)();
	bool  (*IsAnyItemHovered)();
	bool  (*IsAnyItemActive)();
	bool  (*IsAnyItemFocused)();
	void  (*GetItemRectMin)(float* out_x, float* out_y);
	void  (*GetItemRectMax)(float* out_x, float* out_y);
	void  (*GetItemRectSize)(float* out_x, float* out_y);
	void  (*SetItemDefaultFocus)();
	void  (*SetKeyboardFocusHere)(int offset);
	void  (*SetNextItemAllowOverlap)();

	// v36: Disabled regions
	void  (*BeginDisabled)(bool disabled);
	void  (*EndDisabled)();

	// v36: Clip rect
	void  (*PushClipRect)(float min_x, float min_y, float max_x, float max_y, bool intersect_current);
	void  (*PopClipRect)();

	// v36: Mouse queries
	bool  (*IsMouseDown)(int button);
	bool  (*IsMouseClicked)(int button, bool repeat);
	bool  (*IsMouseReleased)(int button);
	bool  (*IsMouseDoubleClicked)(int button);
	void  (*GetMousePos)(float* out_x, float* out_y);
	bool  (*IsMouseDragging)(int button, float lock_threshold);
	void  (*GetMouseDragDelta)(int button, float lock_threshold, float* out_x, float* out_y);
	void  (*ResetMouseDragDelta)(int button);
	void  (*SetMouseCursor)(int cursor_type);
	bool  (*IsMouseHoveringRect)(float min_x, float min_y, float max_x, float max_y, bool clip);

	// v36: Color utilities
	unsigned int (*GetColorU32FromCol)(int idx, float alpha_mul);
	unsigned int (*GetColorU32FromVec4)(float r, float g, float b, float a);
	void         (*GetStyleColorVec4)(int idx, float* out_r, float* out_g, float* out_b, float* out_a);
	void         (*ColorConvertRGBtoHSV)(float r, float g, float b, float* out_h, float* out_s, float* out_v);
	void         (*ColorConvertHSVtoRGB)(float h, float s, float v, float* out_r, float* out_g, float* out_b);

	// v36: Misc
	double        (*GetTime)();
	int           (*GetFrameCount)();
	bool          (*IsRectVisible)(float w, float h);
	const char*   (*GetClipboardText)();
	void          (*SetClipboardText)(const char* text);
	const char*   (*GetStyleColorName)(int idx);

	// -------------------------------------------------------------------------
	// v48: Direct drawing (ImDrawList)
	// -------------------------------------------------------------------------
	// Acquire a draw list. Only valid for the current frame's render callback --
	// do not cache the returned handle across frames.
	PluginDrawList (*GetWindowDrawList)();
	PluginDrawList (*GetBackgroundDrawList)();
	PluginDrawList (*GetForegroundDrawList)();

	// Shape primitives. Colors are packed 0xAABBGGRR ImU32 (see
	// GetColorU32FromVec4 / GetColorU32FromCol above).
	void (*DL_AddLine)(PluginDrawList dl, float x1, float y1, float x2, float y2, unsigned int col, float thickness);
	void (*DL_AddRect)(PluginDrawList dl, float min_x, float min_y, float max_x, float max_y, unsigned int col, float rounding, int flags, float thickness);
	void (*DL_AddRectFilled)(PluginDrawList dl, float min_x, float min_y, float max_x, float max_y, unsigned int col, float rounding, int flags);
	void (*DL_AddRectFilledMultiColor)(PluginDrawList dl, float min_x, float min_y, float max_x, float max_y, unsigned int col_upr_left, unsigned int col_upr_right, unsigned int col_bot_right, unsigned int col_bot_left);
	void (*DL_AddQuad)(PluginDrawList dl, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, unsigned int col, float thickness);
	void (*DL_AddQuadFilled)(PluginDrawList dl, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, unsigned int col);
	void (*DL_AddTriangle)(PluginDrawList dl, float x1, float y1, float x2, float y2, float x3, float y3, unsigned int col, float thickness);
	void (*DL_AddTriangleFilled)(PluginDrawList dl, float x1, float y1, float x2, float y2, float x3, float y3, unsigned int col);
	void (*DL_AddCircle)(PluginDrawList dl, float center_x, float center_y, float radius, unsigned int col, int num_segments, float thickness);
	void (*DL_AddCircleFilled)(PluginDrawList dl, float center_x, float center_y, float radius, unsigned int col, int num_segments);
	void (*DL_AddNgon)(PluginDrawList dl, float center_x, float center_y, float radius, unsigned int col, int num_segments, float thickness);
	void (*DL_AddNgonFilled)(PluginDrawList dl, float center_x, float center_y, float radius, unsigned int col, int num_segments);
	void (*DL_AddEllipse)(PluginDrawList dl, float center_x, float center_y, float radius_x, float radius_y, unsigned int col, float rot, int num_segments, float thickness);
	void (*DL_AddEllipseFilled)(PluginDrawList dl, float center_x, float center_y, float radius_x, float radius_y, unsigned int col, float rot, int num_segments);

	// Text drawn directly onto the draw list (bypasses layout/cursor).
	void (*DL_AddText)(PluginDrawList dl, float x, float y, unsigned int col, const char* text);
	void (*DL_AddTextSized)(PluginDrawList dl, float font_size, float x, float y, unsigned int col, const char* text);

	// points_xy is a flat array of (x,y) pairs, length == point_count * 2.
	void (*DL_AddPolyline)(PluginDrawList dl, const float* points_xy, int point_count, unsigned int col, int flags, float thickness);
	void (*DL_AddConvexPolyFilled)(PluginDrawList dl, const float* points_xy, int point_count, unsigned int col);
	void (*DL_AddBezierCubic)(PluginDrawList dl, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, unsigned int col, float thickness, int num_segments);
	void (*DL_AddBezierQuadratic)(PluginDrawList dl, float x1, float y1, float x2, float y2, float x3, float y3, unsigned int col, float thickness, int num_segments);

	// Images -- tex must be a handle obtained from IPluginImGuiTextures (hooks->ImGuiTextures).
	void (*DL_AddImage)(PluginDrawList dl, PluginTextureHandle tex, float min_x, float min_y, float max_x, float max_y, float uv_min_x, float uv_min_y, float uv_max_x, float uv_max_y, unsigned int col);
	void (*DL_AddImageQuad)(PluginDrawList dl, PluginTextureHandle tex, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, float u1, float v1, float u2, float v2, float u3, float v3, float u4, float v4, unsigned int col);
	void (*DL_AddImageRounded)(PluginDrawList dl, PluginTextureHandle tex, float min_x, float min_y, float max_x, float max_y, float uv_min_x, float uv_min_y, float uv_max_x, float uv_max_y, unsigned int col, float rounding, int flags);

	// Stateful path builder: accumulate points with PathLineTo/PathArcTo/etc,
	// then emit a primitive with PathStroke or PathFillConvex. PathClear resets
	// the accumulated point list without emitting anything.
	void (*DL_PathClear)(PluginDrawList dl);
	void (*DL_PathLineTo)(PluginDrawList dl, float x, float y);
	void (*DL_PathArcTo)(PluginDrawList dl, float center_x, float center_y, float radius, float a_min, float a_max, int num_segments);
	void (*DL_PathArcToFast)(PluginDrawList dl, float center_x, float center_y, float radius, int a_min_of_12, int a_max_of_12);
	void (*DL_PathEllipticalArcTo)(PluginDrawList dl, float center_x, float center_y, float radius_x, float radius_y, float rot, float a_min, float a_max, int num_segments);
	void (*DL_PathBezierCubicCurveTo)(PluginDrawList dl, float x2, float y2, float x3, float y3, float x4, float y4, int num_segments);
	void (*DL_PathBezierQuadraticCurveTo)(PluginDrawList dl, float x2, float y2, float x3, float y3, int num_segments);
	void (*DL_PathRect)(PluginDrawList dl, float min_x, float min_y, float max_x, float max_y, float rounding, int flags);
	void (*DL_PathFillConvex)(PluginDrawList dl, unsigned int col);
	void (*DL_PathStroke)(PluginDrawList dl, unsigned int col, int flags, float thickness);

	// Draw-list-local clip rect stack (separate from the window-level clip
	// stack managed by PushClipRect/PopClipRect above).
	void (*DL_PushClipRect)(PluginDrawList dl, float min_x, float min_y, float max_x, float max_y, bool intersect_current);
	void (*DL_PushClipRectFullScreen)(PluginDrawList dl);
	void (*DL_PopClipRect)(PluginDrawList dl);
	void (*DL_GetClipRectMin)(PluginDrawList dl, float* out_x, float* out_y);
	void (*DL_GetClipRectMax)(PluginDrawList dl, float* out_x, float* out_y);

	// -------------------------------------------------------------------------
	// v52: Mouse wheel
	// -------------------------------------------------------------------------
	// Per-frame wheel delta in ImGui units (one notch = 1.0), read straight off
	// ImGuiIO. ImGui consumes WM_MOUSEWHEEL itself, so this is the only way a
	// plugin can see the wheel at all -- needed by anything that draws its own
	// zoomable or scrollable surface with the draw-list API.
	float (*GetMouseWheel)();
	float (*GetMouseWheelH)();
};

typedef void (*PluginImGuiRenderCallback)(IModLoaderImGui* imgui);

// ---------------------------------------------------------------------------
// Texture API (v37, client only)
// ---------------------------------------------------------------------------

// Opaque handle returned by the texture load functions. NULL = invalid/failed.
typedef void* PluginTextureHandle;

// Interface for loading and rendering images without direct D3D12 access.
// Retrieved via hooks->ImGuiTextures (client only; nullptr on server/generic).
//
// THREADING RULES
// ---------------
// Load* functions perform a blocking GPU copy and MUST be called from a game-
// thread callback (OnExperienceLoadComplete, OnWorldBeginPlay, PluginInit, etc.)
// -- NOT from inside an ImGui render callback.  Calling them from the render
// thread blocks the GPU pipeline and will cause crashes with Streamline/DLSS.
//
// Typical usage pattern:
//   OnExperienceLoadComplete  ->  handle = LoadFromUTexture2D(tex, "MyTex")
//   ImGui render callback     ->  Image(handle, w, h)
//   PluginShutdown            ->  FreeTexture(handle)
//
// Image/ImageButton must be called from inside a plugin render callback while
// an ImGui frame is in progress.  FreeTexture must not be called while the
// texture may still be in flight on the GPU (i.e. not from the render callback
// on the same frame you stop using it -- wait one frame or call from game thread).
//
// Up to GetCapacity() textures may be live at once.
// All Load* functions take a mandatory name parameter for log identification.
// They throw std::out_of_range if no slots are available.
// They return NULL (without throwing) only when D3D12 is not yet ready or
// when the specific resource cannot be loaded (bad file, null pointer, etc.).
// Call GetFreeSlotCount() before loading if you need to avoid the exception.
//
// Sharing/refcounting (v45): the name is also used as a sharing key
// (case-insensitive). If any Load* call is made with a name that matches an
// already-loaded, in-use texture, the existing GPU resource is reused and its
// refcount is incremented -- no new slot is consumed and no new GPU work is
// done. Every successful Load* call (whether it created a new texture or
// reused an existing one) must be paired with exactly one FreeTexture call;
// the underlying resource is only released once the refcount drops to zero.
// If multiple plugins use the same name, they must be referring to the same
// image -- the first registration's pixel data wins.
struct IPluginImGuiTextures
{
    // Load from a UTF-8 file path. Supports PNG, JPG, BMP, GIF, TIFF (via WIC).
    // Blocks the calling thread until the GPU upload completes.
    // Call from a game-thread callback, not from a render callback.
    // Returns NULL if D3D12 is not ready yet or if decoding fails.
    // Throws std::out_of_range if all slots are in use.
    PluginTextureHandle (*LoadFromFile)(const char* utf8_path, const char* name);

    // Load from encoded image bytes in memory (same formats as LoadFromFile).
    // data must remain valid only for the duration of this call.
    // Blocks the calling thread until the GPU upload completes.
    // Call from a game-thread callback, not from a render callback.
    // Throws std::out_of_range if all slots are in use.
    PluginTextureHandle (*LoadFromMemory)(const void* data, size_t size, const char* name);

    // Load from raw 32-bit RGBA pixels (4 bytes/pixel, top-left row-major).
    // data must remain valid only for the duration of this call.
    // Blocks the calling thread until the GPU upload completes.
    // Call from a game-thread callback, not from a render callback.
    // Throws std::out_of_range if all slots are in use.
    PluginTextureHandle (*LoadFromRGBA)(const unsigned char* rgba, int width, int height, const char* name);

    // Copy a UTexture2D into a modloader-owned GPU resource and return a handle.
    // Blocks the calling thread until the GPU copy completes (~1-50ms).
    // Call from a game-thread callback (OnExperienceLoadComplete etc.), NOT from
    // a render callback -- doing so will stall the render thread and crash with DLSS.
    // The copy is yours to keep; the engine may GC the source freely after this call.
    // Call FreeTexture when done -- handles persist until explicitly freed.
    // Returns NULL if D3D12 is not ready or the texture has no GPU resource yet.
    // Throws std::out_of_range if all slots are in use.
    PluginTextureHandle (*LoadFromUTexture2D)(SDK::UTexture2D* texture, const char* name);

    // Release the texture and free its slot. Safe to call with NULL.
    // Do not call while the texture may still be rendered on the GPU.
    // If the texture is shared (loaded by name more than once), this only
    // decrements its refcount -- the slot is freed once every owner has
    // called FreeTexture.
    void (*FreeTexture)(PluginTextureHandle handle);

    // Query the texture's natural dimensions. Either out pointer may be NULL.
    void (*GetSize)(PluginTextureHandle handle, int* out_width, int* out_height);

    // Render the texture at the current ImGui cursor position.
    // Pass width=0, height=0 to use the texture's natural size.
    void (*Image)(PluginTextureHandle handle, float width, float height);

    // Clickable image button. Returns true if clicked.
    // Pass width=0, height=0 to use the texture's natural size.
    bool (*ImageButton)(const char* str_id, PluginTextureHandle handle, float width, float height);

    // Returns the number of texture slots not currently in use. (v38)
    int (*GetFreeSlotCount)();

    // Returns the total texture slot capacity. (v38)
    int (*GetCapacity)();
};

// Flags for PluginWindowHints::extra_window_flags (v31).
// Values mirror ImGuiWindowFlags so plugins do not need imgui.h.
#define PluginWindowFlags_NoTitleBar        (1 << 0)
#define PluginWindowFlags_NoResize          (1 << 1)
#define PluginWindowFlags_NoMove            (1 << 2)
#define PluginWindowFlags_NoScrollbar       (1 << 3)
#define PluginWindowFlags_NoBackground      (1 << 7)
#define PluginWindowFlags_NoSavedSettings   (1 << 8)
#define PluginWindowFlags_NoMouseInputs     (1 << 9)

// Optional size/position hints for RegisterWidget windows.
// Set width/height to 0 for no size hint. Set pos_x/pos_y to -1 to skip positioning.
// size_cond / pos_cond: 0 = Always, 1 = FirstUseEver.
struct PluginWindowHints
{
	float width;
	float height;
	float pos_x;
	float pos_y;
	float pivot_x;
	float pivot_y;
	int   size_cond;
	int   pos_cond;
	int   extra_window_flags;  // v31: OR'd into ImGuiWindowFlags; 0 = default
};

struct PluginPanelDesc
{
	const char* buttonLabel;
	const char* windowTitle;
	PluginImGuiRenderCallback renderFn;
};

typedef void* PanelHandle;
typedef void* WidgetHandle;

struct PluginWidgetDesc
{
	const char*               name;
	PluginImGuiRenderCallback renderFn;
	const PluginWindowHints*  windowHints;  // optional, may be nullptr
};

typedef void (*PluginConfigChangedCallback)(const char* section, const char* key, const char* newValue);

// v43: Fired when a panel window is closed -- either via the ImGui titlebar
// X button or via a plugin calling SetPanelClose (e.g. driving its own
// visibility toggle). handle identifies which panel was closed.
typedef void (*PluginPanelClosedCallback)(PanelHandle handle);

struct IPluginUIEvents
{
	PanelHandle  (*RegisterPanel)(const PluginPanelDesc* desc);
	void         (*UnregisterPanel)(PanelHandle handle);
	// v49: self scopes the callback to this plugin's own config file -- pass
	// the same IPluginSelf* received in PluginInit.
	void         (*RegisterOnConfigChanged)(const IPluginSelf* self, PluginConfigChangedCallback callback);
	void         (*UnregisterOnConfigChanged)(const IPluginSelf* self, PluginConfigChangedCallback callback);
	void         (*SetPanelOpen)(PanelHandle handle);
	void       (*SetPanelClose)(PanelHandle handle);
	WidgetHandle (*RegisterWidget)(const PluginWidgetDesc* desc);      // v16
	void      (*UnregisterWidget)(WidgetHandle handle);   // v16
	void         (*SetWidgetVisible)(WidgetHandle handle, bool visible); // v16

	// v43: Notified whenever any panel is closed (X button or SetPanelClose).
	void (*RegisterOnPanelWindowClosed)(PluginPanelClosedCallback callback);
	void (*UnregisterOnPanelWindowClosed)(PluginPanelClosedCallback callback);

	// v43: Request that the modloader suppress game mouse/keyboard input,
	// the same way it does while a panel/modloader window is open. Useful
	// for plugins driving their UI via RegisterWidget (which does not
	// trigger capture on its own). Returns an opaque token; input stays
	// suppressed as long as at least one token (from any plugin) is held.
	// Always call ReleaseInputCapture for every acquired token, including
	// during PluginShutdown -- a leaked token permanently suppresses input
	// until the modloader restarts.
	void* (*AcquireInputCapture)();
	void  (*ReleaseInputCapture)(void* token);

	// v51: Cooperative alternative to AcquireInputCapture. While a passthrough
	// token is held, ImGui receives every message and draws/owns the cursor
	// just as it does under an exclusive capture, but the game is only cut out
	// of the input classes ImGui actually wants that frame: mouse while the
	// cursor is over an ImGui window or dragging a widget, keyboard while a
	// text field has focus. Everything else -- including the WM_INPUT raw
	// mouse deltas UE5 uses for camera look -- still reaches the game, so the
	// player can keep moving with your UI on screen.
	//
	// Use this for always-on widget UIs the player is meant to interact with
	// the world underneath; use AcquireInputCapture for modal windows that
	// should freeze the game.
	//
	// Exclusive capture wins: while any modloader window, plugin panel, or
	// AcquireInputCapture token is active, passthrough tokens have no effect.
	// Refcounted across all plugins; always release every acquired token,
	// including during PluginShutdown.
	void* (*AcquireInputPassthrough)();
	void  (*ReleaseInputPassthrough)(void* token);
};

// ---------------------------------------------------------------------------
// In-world debug drawing (v50, client only) -- hooks->HUD->DebugDraw
//
// Reimplements the UKismetSystemLibrary::DrawDebug* set, which is entirely
// dead on shipping builds: ENABLE_DRAW_DEBUG is 0, so all sixteen bodies
// compiled away to nothing and only the exec thunks remain (they parse their
// parameters off the stack and return). The DrawDebugType pin on the trace
// nodes is inert for the same reason. What survives is the renderer --
// UWorld::LineBatchers and ULineBatchComponent::DrawLines -- so these
// functions build the same geometry and feed it to the engine's own batchers.
//
// THREADING: safe to call from any thread. The line batchers are UObjects and
// can only be touched on the game thread, but you do not have to arrange that
// -- the modloader converts your arguments to owned values up front and then
// either draws inline (if you were already on the game thread) or defers by a
// single frame. This matters because ImGui panel and widget callbacks run on
// the render thread, and those are the most natural place to want to draw.
//
// Nothing you pass is retained past the call, including the float-history
// sample array (it is deep-copied). The exception is actor pointers --
// DrawCamera's camera actor and DrawString's base actor -- which must stay
// valid for the frame.
//
// LIFETIME: style->duration <= 0 draws for a single frame. A positive duration
// (or style->bPersistent) routes into the persistent batcher, which survives
// the per-frame flush -- FlushPersistentLines clears it. This mirrors the
// engine's own GetDebugLineBatcher/GetDebugLineLifeTime rules.
//
// The geometry structs below are compiled into the plugin by value, so they
// are frozen: never reorder or extend them -- add a new struct instead.
// ---------------------------------------------------------------------------

struct PluginDebugVector  { double x, y, z; };
struct PluginDebugRotator { double pitch, yaw, roll; };   // degrees, UE convention
struct PluginDebugColor   { float  r, g, b, a; };         // linear, 0..1
struct PluginDebugPlane   { double x, y, z, w; };         // normal xyz + distance w

struct PluginDebugTransform
{
	PluginDebugVector  location;
	PluginDebugRotator rotation;
	PluginDebugVector  scale;
};

// Plugin-owned sample array. Read during the call and never retained.
struct PluginDebugFloatHistory
{
	const float* samples;
	int32_t      count;
	float        minValue;
	float        maxValue;
	bool         bAutoAdjustMinMax;   // recompute min/max from the samples
};

struct PluginDebugDrawStyle
{
	PluginDebugColor color;
	float            duration;      // <= 0: this frame only.  > 0: seconds.
	float            thickness;     // 0 = thin (single pixel) lines
	bool             bPersistent;   // never expires until FlushPersistentLines
	bool             bForeground;   // draw on top of world geometry
};

struct IPluginDebugDraw
{
	// False if ULineBatchComponent::DrawLines could not be resolved on this
	// build -- every Draw* below is then a silent no-op. Check once at init.
	bool (*IsAvailable)();

	void (*DrawLine)(const PluginDebugVector* start, const PluginDebugVector* end,
	                 const PluginDebugDrawStyle* style);

	// The engine draws this through BatchedPoints, which has no reachable entry
	// point here, so it is a three-axis cross of half-length `size` instead.
	void (*DrawPoint)(const PluginDebugVector* position, float size,
	                  const PluginDebugDrawStyle* style);

	// Circle lies in the plane spanned by yAxis/zAxis (pass {0,1,0} and {0,0,1}
	// for a world XY circle). Both are normalised internally.
	void (*DrawCircle)(const PluginDebugVector* center, float radius, int numSegments,
	                   const PluginDebugVector* yAxis, const PluginDebugVector* zAxis,
	                   bool bDrawAxis, const PluginDebugDrawStyle* style);

	void (*DrawSphere)(const PluginDebugVector* center, float radius, int segments,
	                   const PluginDebugDrawStyle* style);

	void (*DrawBox)(const PluginDebugVector* center, const PluginDebugVector* extent,
	                const PluginDebugRotator* rotation, const PluginDebugDrawStyle* style);

	void (*DrawCapsule)(const PluginDebugVector* center, float halfHeight, float radius,
	                    const PluginDebugRotator* rotation, const PluginDebugDrawStyle* style);

	void (*DrawCylinder)(const PluginDebugVector* start, const PluginDebugVector* end,
	                     float radius, int segments, const PluginDebugDrawStyle* style);

	void (*DrawConeInDegrees)(const PluginDebugVector* origin, const PluginDebugVector* direction,
	                          float length, float angleWidthDeg, float angleHeightDeg,
	                          int numSides, const PluginDebugDrawStyle* style);

	void (*DrawArrow)(const PluginDebugVector* start, const PluginDebugVector* end,
	                  float arrowSize, const PluginDebugDrawStyle* style);

	// Axes are drawn red/green/blue like the engine's version, so style->color
	// is ignored here. Everything else in the style still applies.
	void (*DrawCoordinateSystem)(const PluginDebugVector* location, const PluginDebugRotator* rotation,
	                             float scale, const PluginDebugDrawStyle* style);

	// The engine fills this quad via BatchedMeshes, which is unreachable, so it
	// is drawn as four border edges plus both diagonals, with the same yellow
	// normal arrow.
	void (*DrawPlane)(const PluginDebugPlane* plane, const PluginDebugVector* location,
	                  float size, const PluginDebugDrawStyle* style);

	void (*DrawFrustum)(const PluginDebugTransform* frustumTransform,
	                    const PluginDebugDrawStyle* style);

	// cameraActor must be an SDK::ACameraActor*; location/rotation come off the
	// actor and the FOV off its UCameraComponent. No-op if either is null.
	void (*DrawCamera)(void* cameraActor, float scale, const PluginDebugDrawStyle* style);

	// Same geometry with no actor lookup, for cameras you already have a
	// transform for.
	void (*DrawCameraAt)(const PluginDebugVector* location, const PluginDebugRotator* rotation,
	                     float fovDegrees, float scale, const PluginDebugDrawStyle* style);

	// Canvas-space text pinned to a world location, routed through
	// AHUD::AddDebugText (which AHUD::DrawDebugTextList still renders). text is
	// UTF-8. testBaseActor may be null, in which case the text is placed at an
	// absolute world location. Independent of the line batchers:
	// FlushPersistentLines does not clear these, ClearAllStrings does.
	//
	// WARNING: duration does NOT follow the same rule as the style duration
	// used everywhere else here. The HUD only treats exactly -1.0f as "never
	// expire"; every other value counts down, including 0, which vanishes on
	// the next HUD render. (Line durations are the opposite -- anything <= 0
	// lives forever in a persistent batcher.)
	//
	// fontScale multiplies the text size directly -- the engine assigns it
	// straight to the canvas text item's scale, with no distance falloff and no
	// clamping, so 2.0f is exactly twice the size at any range. 1.0f is the
	// default (the engine's small font at native size); anything <= 0 is
	// treated as 1.0f so a zeroed struct can't produce invisible text.
	void (*DrawString)(const PluginDebugVector* location, const char* text, void* testBaseActor,
	                   const PluginDebugColor* color, float duration, float fontScale);

	// drawSize uses x as the graph width and y as its height (z is unused).
	// The engine fills the graph body with a mesh; these draw the bounding
	// frame plus the sample polyline.
	void (*DrawFloatHistoryTransform)(const PluginDebugFloatHistory* history,
	                                  const PluginDebugTransform* drawTransform,
	                                  const PluginDebugVector* drawSize,
	                                  const PluginDebugDrawStyle* style);

	void (*DrawFloatHistoryLocation)(const PluginDebugFloatHistory* history,
	                                 const PluginDebugVector* drawLocation,
	                                 const PluginDebugVector* drawSize,
	                                 const PluginDebugDrawStyle* style);

	// Clears both persistent batchers, like the engine's
	// FLUSHPERSISTENTDEBUGLINES console command. Affects every plugin's
	// persistent lines, not just yours.
	void (*FlushPersistentLines)();

	// Clears every world-anchored debug string on the local HUD.
	void (*ClearAllStrings)();
};

struct IPluginHUDEvents
{
	void      (*RegisterOnPostRender)(PluginHUDPostRenderCallback callback);
	void  (*UnregisterOnPostRender)(PluginHUDPostRenderCallback callback);
	uintptr_t (*GetGatherPlayersDataAddress)();

	// v50 -- appended at the end, do not relocate. Never null on client builds
	// (the whole HUD interface is null on server/generic); check
	// DebugDraw->IsAvailable() for whether the underlying native resolved.
	IPluginDebugDraw* DebugDraw;
};

// ---------------------------------------------------------------------------
// Network channel (v17–v18)
// ---------------------------------------------------------------------------
struct IPluginNetworkChannel
{
	// True when this process holds net authority for the session, i.e. it is a
	// dedicated server OR a listen host. False on a pure client and in Standalone.
	// This is a runtime answer, not a build-time one -- a client build hosting a
	// game returns true here (v53; it was hardcoded false before).
	// For "which DLL am I in", use PluginInfo::pluginTarget.
	bool (*IsServer)();

	// Authority-only: no-ops (with a warning logged) when IsServer() is false.
	// Both skip the listen host's own player controller, which has no connection.
	//
	// v55: both also skip any client that has not reported THIS plugin at THIS
	// exact version in its manifest. "All clients" therefore means "all remote
	// clients running the same build of me". See the v55 note at the top of this
	// file before assuming a broadcast reached anyone.
	void (*SendPacketToClient)(void* playerController, const IPluginSelf* self, const char* typeTag, const uint8_t* data, size_t size);
	void (*SendPacketToAllClients)(const IPluginSelf* self, const char* typeTag, const uint8_t* data, size_t size);
	void (*RegisterMessageHandler)(const IPluginSelf* self, const char* typeTag, PluginNetworkMessageCallback callback);
	void (*UnregisterMessageHandler)(const IPluginSelf* self, const char* typeTag, PluginNetworkMessageCallback callback);
	void (*SendPacketToServer)(const IPluginSelf* self, const char* typeTag, const uint8_t* data, size_t size);  // v18
	void (*RegisterServerMessageHandler)(const IPluginSelf* self, const char* typeTag, PluginNetworkServerMessageCallback callback); // v18
	void (*UnregisterServerMessageHandler)(const IPluginSelf* self, const char* typeTag, PluginNetworkServerMessageCallback callback); // v18
	void (*ExcludeFromBroadcast)(void* playerController);    // v18
	void (*UnexcludeFromBroadcast)(void* playerController);  // v18

	// ---- Readiness (v56) -- see the v56 note at the top of this file ----------
	//
	// Authority side. IsClientReady is the exact predicate SendPacketToClient
	// gates on: this client has reported YOUR plugin at YOUR version. False on a
	// pure client, for the listen host's own controller, and during the join
	// window -- a client is NOT ready when a player-joined hook fires.
	bool (*IsClientReady)(void* playerController, const IPluginSelf* self);

	// Fires once per client when IsClientReady(pc, self) becomes true. Registering
	// after clients are already ready fires immediately for each of them, so a
	// reloaded plugin does not miss the session it was loaded into. No-ops with a
	// warning when this process is not the authority.
	void (*RegisterClientReadyCallback)(const IPluginSelf* self, PluginClientReadyCallback callback);
	void (*UnregisterClientReadyCallback)(const IPluginSelf* self, PluginClientReadyCallback callback);

	// Client side. True once the authority has acknowledged our manifest. Always
	// false on the authority itself (there is no server to be ready) -- a listen
	// host's own code does not need the wire to reach itself.
	bool (*IsServerReady)();

	// Fires when IsServerReady() becomes true; fires immediately if it already is.
	void (*RegisterServerReadyCallback)(const IPluginSelf* self, PluginServerReadyCallback callback);
	void (*UnregisterServerReadyCallback)(const IPluginSelf* self, PluginServerReadyCallback callback);
};

// ---------------------------------------------------------------------------
// Native pointers (v21)
// ---------------------------------------------------------------------------
struct IPluginNativePointers
{
	uintptr_t (*EngineLoopInit)();
	uintptr_t (*GameEngineInit)();
	uintptr_t (*EngineLoopExit)();
	uintptr_t (*EnginePreExit)();
	uintptr_t (*EngineTick)();
	uintptr_t (*WorldBeginPlay)();
	uintptr_t (*WorldEndPlay)();
	uintptr_t (*SaveLoaded)();
	uintptr_t (*ExperienceLoadComplete)();
	uintptr_t (*ActorBeginPlay)();
	uintptr_t (*PlayerJoined)();
	uintptr_t (*PlayerLeft)();
	uintptr_t (*SpawnerActivate)();
	uintptr_t (*SpawnerDeactivate)();
	uintptr_t (*SpawnerDoSpawning)();
	uintptr_t (*HUDPostRender)();   // client only (nullptr on server/generic)
	// v54: ALWAYS nullptr. The ClientSaveStringToTxt transport this exposed was
	// removed when networking moved to the control channel. Slot retained so the
	// struct layout does not shift for already-built plugins.
	uintptr_t (*ClientMessageExec)();

	// v44 -- trampoline address of ACrCrafter::NativeOnItemCraftingComplete.
	// Cast to: void(__fastcall*)(void* thisPtr, uint64_t entityHandle, uint64_t signalName)
	uintptr_t (*CraftingFinished)();
};

// ---------------------------------------------------------------------------
// Text utilities
// ---------------------------------------------------------------------------
struct IPluginTextUtils
{
	// Trampoline address of the original FText::AsLocalizable_Advanced, or 0 if the hook
	// failed to install. Cast to:
	//   FText*(__fastcall*)(FText* result, const FTextKey* Namespace, const FTextKey* Key, const wchar_t* String)
	uintptr_t (*AsLocalizable_Advanced)();

	// Resolved native address of SDK::UKismetTextLibrary::Conv_TextToString, or 0 if not found.
	// Cast to: FString*(__fastcall*)(FString* result, const FText* InText)
	uintptr_t (*Conv_TextToString)();

	// Trampoline address of the original FTextKey::FTextKey(const wchar_t*), or 0 if the hook
	// failed to install. Use this to build the Namespace/Key arguments required by
	// AsLocalizable_Advanced -- FTextKey is just an interned-string-table index and cannot
	// be constructed any other way.
	// Cast to: void(__fastcall*)(FTextKey* this, const wchar_t* InStr)
	uintptr_t (*MakeTextKey)();
};

// ---------------------------------------------------------------------------
// HTTP server (v22, server only)
// ---------------------------------------------------------------------------

// HTTP verb reported to OnRawRequest filter callbacks.
enum class HttpMethod : uint8_t
{
	Get = 0, Post = 1, Put = 2, Delete = 3,
	Patch = 4, Options = 5, Head = 6, Other = 7,
};

// Return value for OnRawRequest filter callbacks.
enum class HttpRequestAction : uint8_t
{
	Approve = 0, // Continue processing (route handlers, then original engine handler).
	Deny    = 1, // Send 403 Forbidden; stop all further processing.
};

// Read-only snapshot of the incoming request. Pointers valid only for the callback duration.
// body is NOT null-terminated — use bodyLen.
struct PluginHttpRequest
{
	const char* url;     // UTF-8, null-terminated, e.g. "/remote/object/call"
	const char* body;    // Raw body bytes; nullptr when bodyLen == 0
	size_t      bodyLen;
	HttpMethod  method;
};

// Signature for raw-request filter callbacks.
typedef HttpRequestAction (*PluginHttpRequestFilterCallback)(const PluginHttpRequest* req);

// ---------------------------------------------------------------------------
// Raw-response routes (v22)
//
// A plugin-owned HTTP handler. The modloader calls the callback whenever an
// incoming URL starts with /<pluginName>/<urlPrefix>/ (case-insensitive).
// The plugin populates PluginHttpResponse to control what is sent back.
// If the callback leaves body/bodyLen at zero, a 200 with an empty body is sent.
// To send a non-200 status, set statusCode accordingly.
// The callback is always called from the HTTP connection thread — do NOT block
// indefinitely or access UObjects without proper synchronisation.
// ---------------------------------------------------------------------------

// Response descriptor filled by the plugin's route callback.
// All fields must remain valid until the callback returns.
// The modloader copies body bytes and the content-type string before returning.
struct PluginHttpResponse
{
	int      statusCode;   // HTTP status code, e.g. 200, 404, 500. Default: 200.
	const char* contentType;  // MIME type string, e.g. "application/json". Default: "text/plain".
	const char* body; // Response body bytes. May be nullptr when bodyLen == 0.
	size_t      bodyLen;      // Byte count of body. 0 produces an empty body.
};

// Callback signature for raw-response routes.
// req  : read-only incoming request snapshot — same semantics as filter callbacks.
// resp : output descriptor — plugin fills this before returning.
//        All pointer fields in resp must stay valid until the function returns;
// the modloader copies them immediately after.
typedef void (*PluginHttpRouteCallback)(const PluginHttpRequest* req, PluginHttpResponse* resp);

// ============================================================
// IPluginHttpServer — HTTP intercept interface (v22, server only)
//
// hooks->HttpServer is nullptr on client and generic builds.
//
// URL scheme:  /<pluginName>/<routeName>/...  (always case-insensitive)
//   Static files:   /<self->name>/<folderName>/path/to/file.html
//          → served from <exe_dir>\Plugins\<self->name>\<folderName>\
//   Raw routes:     /<self->name>/<urlPrefix>/any/sub/path
//          → callback receives the full original URL
//
// Processing order for every incoming request:
//   1. Raw-request filters  — RegisterOnRawRequest / UnregisterOnRawRequest
//        First Deny sends 403 and stops all further processing.
//   2. Raw-response routes  — AddRawRoute / RemoveRawRoute
//   Plugin-owned handler; plugin writes status, content-type, and body.
//   3. Static-file routes   — AddRoute / RemoveRoute
//        Files served from disk with a 200 and an inferred MIME type.
//   4. Pass-through     — original engine handler (produces 404 for unknown paths).
// ============================================================
struct IPluginHttpServer
{
	// -----------------------------------------------------------------------
	// Static-file routes
	// -----------------------------------------------------------------------

	// Register a static-file route.
	// URL matched (case-insensitive): /<self->name>/<folderName>/...
	// Files served from:     <exe_dir>\Plugins\<self->name>\<folderName>\
	// Returns false if already registered or folderName is empty/null.
	bool (*AddRoute)(const IPluginSelf* self, const char* folderName);

	// Unregister a static-file route. No-op if not registered.
	void (*RemoveRoute)(const IPluginSelf* self, const char* folderName);

	// -----------------------------------------------------------------------
	// Raw-request filters
	// -----------------------------------------------------------------------

	// Register a raw-request filter. Fires for every HTTP request before routes.
	// Return Deny to send 403; return Approve to continue. First Deny wins.
	void (*RegisterOnRawRequest)(PluginHttpRequestFilterCallback callback);

	// Unregister a raw-request filter. No-op if not registered.
	void (*UnregisterOnRawRequest)(PluginHttpRequestFilterCallback callback);

	// -----------------------------------------------------------------------
	// Raw-response routes (v22)
	// -----------------------------------------------------------------------

	// Register a plugin-owned HTTP handler.
	// urlPrefix  : route name, e.g. "api".
	// Matched URL (case-insensitive): /<self->name>/<urlPrefix>/...
	// callback   : called when a matching request arrives; plugin fills PluginHttpResponse.
	// Returns false if already registered or urlPrefix is empty/null.
	bool (*AddRawRoute)(const IPluginSelf* self, const char* urlPrefix, PluginHttpRouteCallback callback);

	// Unregister a previously added raw-response route. No-op if not registered.
	// Always call during PluginShutdown to avoid dangling callback pointers.
	void (*RemoveRawRoute)(const IPluginSelf* self, const char* urlPrefix);
};

// ---------------------------------------------------------------------------
// Net mode info (v36) — server + client, null on generic
// ---------------------------------------------------------------------------
struct IPluginNetModeInfo
{
    // Returns the current network mode via AActor::InternalGetNetMode, resolved
    // by AOB scan and invoked directly as a trampoline (no hook/detour).
    // Returns Unknown if the function could not be resolved or no actor is
    // available yet to query (e.g. called before world begin play).
    EPluginNetMode (*GetNetMode)();

    // Convenience: true if GetNetMode() is ListenServer or Client, i.e. an
    // active multiplayer session as opposed to a Standalone game.
    bool (*IsMultiplayer)();

    // Convenience: true if GetNetMode() is DedicatedServer or ListenServer.
    bool (*IsServer)();
};

// ---------------------------------------------------------------------------
// Splash feedback (v40, client only; null on server/generic)
// ---------------------------------------------------------------------------
// Lets plugins push messages and progress to the secondary sub-bar on the
// startup splash window during PluginInit, giving users feedback when a plugin
// takes several seconds to initialise.  The main status/progress bar is
// owned by the modloader.  All text is UTF-8.  Safe to call from any thread.
struct IPluginSplash
{
    // Returns true while the splash window is open and visible.
    // Guard all other calls with this -- returns false after startup completes
    // or when the plugin is hot-reloaded after the splash has already closed.
    bool (*IsVisible)();

    // Update the secondary status label shown below the main progress bar.
    // Pass an empty string or call ClearSubBar to hide the secondary section.
    void (*SetSubStatus)(const char* text);

    // Update the secondary progress bar fill (0.0 to 1.0).
    void (*SetSubProgress)(float fraction);

    // Hide the secondary bar and clear its label.
    void (*ClearSubBar)();

    // Acquire a hold to keep the splash open past the normal startup close.
    // Call during PluginInit before PostToGameThread if your async work needs
    // to update the splash.  You MUST call ReleaseSplashHold exactly once when
    // that work finishes (or fails).  Holds are ref-counted.  No-op if the
    // splash is already closed.  Safe to call from any thread.
    void (*AcquireSplashHold)();

    // Release a hold previously acquired with AcquireSplashHold.  When all
    // holds are released the splash is allowed to proceed to close.
    // Safe to call from any thread.
    void (*ReleaseSplashHold)();
};

// ---------------------------------------------------------------------------
// Top-level hooks interface (v14+)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// v47 -- GObjects walking, object lookup by class/name, raw UFunction
// invocation via ProcessEvent. On-demand only; see changelog comment above.
// ---------------------------------------------------------------------------
// className/objectName are fixed buffers, not pointers -- the *Into functions
// fill outArray and return, so nothing can point at a temporary that has
// already been destroyed by the time the caller reads the array. Truncated
// (rather than failing) if a name is longer than the buffer.
struct PluginObjectInfo
{
	void*   object;
	char    className[128];
	char    objectName[256];
	uint32_t nameNumber;
	uint32_t objectFlags;
	int32_t  objectIndex;
};

// Filters layered on top of the existing BeginDestroyed/FinishDestroyed skip,
// so plugins never need to know raw EObjectFlags bit values themselves.
enum PluginObjectLookupMode : int32_t
{
	PluginObjectLookup_Both         = 0, // CDOs, archetypes, and live instances
	PluginObjectLookup_InstanceOnly = 1, // skips ClassDefaultObject + ArchetypeObject
	PluginObjectLookup_CDOOnly      = 2, // only ClassDefaultObject
};

struct IPluginObjectWalker
{
	// True once GObjects is populated (i.e. after EngineInit). Plugins must
	// gate all other calls on this returning true.
	bool (*IsReady)();

	// Fills outArray (capacity entries max) with every UObject in GObjects
	// matching mode, in GObjects order. Returns the TOTAL number of matches
	// found, which may exceed capacity -- compare the return value against
	// capacity to detect truncation and re-call with a bigger buffer if
	// needed. outArray is plugin-owned; nothing is allocated by the modloader.
	// EXPENSIVE: tens of thousands of entries. Caller is responsible for
	// caching results -- do not call this every tick.
	int (*WalkAllObjectsInto)(PluginObjectLookupMode mode, PluginObjectInfo* outArray, int capacity);

	// Convenience: only matches objects whose class short-name matches exactly.
	int (*FindObjectsByClassNameInto)(const char* className, PluginObjectLookupMode mode,
		PluginObjectInfo* outArray, int capacity);

	// Exact FName string match (ignores instance number); returns first hit or null.
	void* (*FindFirstObjectByName)(const char* objectName);

	// Exact FName string match; matches all instances (handles Name_N collisions).
	int (*FindObjectsByNameInto)(const char* objectName, PluginObjectLookupMode mode,
		PluginObjectInfo* outArray, int capacity);

	// Invoke className::funcName on object via ProcessEvent. paramsBuffer must
	// match the UFunction's native Params struct layout exactly -- no
	// marshaling is performed. Returns false if object/class/function could
	// not be resolved.
	bool (*InvokeUFunctionByName)(void* object, const char* className, const char* funcName, void* paramsBuffer);

	// Resolve once, call many times (avoids repeated name lookups in a loop
	// that still must remain caller-triggered, not engine-tick-triggered).
	void* (*ResolveUFunction)(const char* className, const char* funcName);
	bool (*InvokeResolvedUFunction)(void* object, void* resolvedFunction, void* paramsBuffer);
};

// v47 -- Hooks::DelegateHook (engine-side: hooks/game/delegate_hook/).
// Lets a plugin react to an existing UE5 multicast delegate (e.g.
// SDK::UCrSaveSubsystem::OnAfterSave) without the maintainer hand-writing a
// dedicated AOB-scanned hooks/game/<event>/ module for it.
//
// Mechanism (see delegate_hook.h/.cpp for full detail): a brand-new,
// privately-named UFunction is cloned from an existing native UFunction
// (used purely as a safe field-layout template -- its actual behavior is
// irrelevant and never invoked, so hostClassName/hostFuncName are OPTIONAL;
// leave them null to use the modloader's own built-in template instead of
// hunting one down yourself) and spliced into the target delegate's
// InvocationList. UClass::FindFunctionByName is hooked to resolve this
// synthetic name privately; every other name -- i.e. everything else in the
// game -- takes the real, unmodified path. No real UClass's FuncMap/
// AllFunctionsCache is ever touched, so nothing else that resolves the same
// real function name is affected.
//
// Deliberately parameterless: PluginDelegateCallback receives only
// userContext, not the delegate's broadcast arguments. Multiple concurrent
// hooks are supported, including several hooks on the very same delegatePtr
// -- each Hook() call gets its own independent handle.
struct IPluginDelegateHook
{
	// delegatePtr must point at a live TMulticastInlineDelegate<...> member
	// (any arity -- InvocationList is always TArray<FScriptDelegate>).
	// hostObject is the object the delegate will report as broadcasting on
	// (becomes the FWeakObjectPtr in the spliced FScriptDelegate) -- usually
	// the same object delegatePtr lives on.
	//
	// hostClassName/hostFuncName are OPTIONAL -- pass nullptr for both (the
	// common case) to use the modloader's built-in template UFunction. Only
	// the template's native FunctionFlags/layout are copied; its actual
	// behavior is irrelevant and never invoked, so there's no reason to
	// supply your own unless you have a specific reason not to depend on the
	// modloader's built-in default.
	//
	// Returns 0 on failure (template not resolved, FindFunctionByName hook
	// unavailable, or InvocationList could not be grown); otherwise a handle
	// for Unhook/IsHooked.
	DelegateHookHandle (*Hook)(void* delegatePtr, void* hostObject,
		const char* hostClassName, const char* hostFuncName,
		PluginDelegateCallback callback, void* userContext);

	// Removes the spliced FScriptDelegate entry and frees the synthetic
	// clone. False if handle is not (or no longer) active.
	bool (*Unhook)(DelegateHookHandle handle);

	// True if handle currently has an active hook installed via this module.
	bool (*IsHooked)(DelegateHookHandle handle);
};

// v47 -- Hooks::ObjectProperties. Lets a plugin read/write a UPROPERTY by
// name instead of casting to a raw SDK struct and reading a compiled-in
// offset, so a game update that reshuffles struct layout (a Dumper-7
// re-export) does not require recompiling the plugin -- only the property
// *name* has to stay the same. PluginPropertyHandle is an opaque FProperty*.
//
// Typed accessors validate the property's reflection type before touching
// memory and return false on a mismatch. GetPropertyRawPtr is the escape
// hatch for kinds with no typed accessor here (Array/Map/Set/Delegate/...).
//
// Does NOT cover plain native C++ members with no UPROPERTY/FProperty entry
// -- those have no reflection metadata to look up by name and still need a
// hand-derived raw offset.
typedef void* PluginPropertyHandle;

enum class PluginPropertyKind : int32_t
{
	Unknown = 0,
	Bool,
	Int,    // any signed/unsigned integer width 1-8 bytes
	Float,  // FloatProperty or DoubleProperty
	Name,
	Str,
	Object, // raw-pointer-backed UObject* only (not Weak/Soft/LazyObjectProperty)
	Struct,
	Array,
	Enum,
	Unsupported, // recognized property type with no typed accessor here
};

struct IPluginObjectProperties
{
	bool (*IsReady)();

	// Resolve a property by class name + property name (walks the class's
	// ChildProperties + SuperStruct chain). Returns null if not found.
	PluginPropertyHandle (*FindPropertyByName)(const char* className, const char* propertyName);

	// Convenience: resolves object's class then calls FindPropertyByName.
	PluginPropertyHandle (*FindPropertyOnObject)(void* object, const char* propertyName);

	PluginPropertyKind (*GetPropertyKind)(PluginPropertyHandle property);
	size_t  (*GetPropertySize)(PluginPropertyHandle property);     // ElementSize * ArrayDim
	int32_t (*GetPropertyArrayDim)(PluginPropertyHandle property);

	// Raw escape hatch: container + property's offset, no type checking.
	void* (*GetPropertyRawPtr)(void* container, PluginPropertyHandle property);

	// Typed getters/setters. Each validates the property's reflection type
	// before touching memory; returns false on type mismatch or null args.
	bool (*GetBoolProperty)(void* container, PluginPropertyHandle property, bool* outValue);
	bool (*SetBoolProperty)(void* container, PluginPropertyHandle property, bool value);

	bool (*GetIntProperty)(void* container, PluginPropertyHandle property, int64_t* outValue);
	bool (*SetIntProperty)(void* container, PluginPropertyHandle property, int64_t value);

	bool (*GetFloatProperty)(void* container, PluginPropertyHandle property, double* outValue);
	bool (*SetFloatProperty)(void* container, PluginPropertyHandle property, double value);

	bool (*GetObjectProperty)(void* container, PluginPropertyHandle property, void** outValue);
	bool (*SetObjectProperty)(void* container, PluginPropertyHandle property, void* value);

	// Read-only -- see file header comment above on why String/Name have no setter.
	bool (*GetStringProperty)(void* container, PluginPropertyHandle property, char* outBuffer, int bufferSize);
	bool (*GetNameProperty)(void* container, PluginPropertyHandle property, char* outBuffer, int bufferSize);

	// For struct-typed properties: the nested type's class name, so callers
	// can recurse FindPropertyByName on inner fields. Empty string if the
	// property is not a struct property.
	bool (*GetPropertyStructTypeName)(PluginPropertyHandle property, char* outBuffer, int bufferSize);
};

// ---------------------------------------------------------------------------
// Console commands (v63) -- every build
// ---------------------------------------------------------------------------
// Registers into ModConsole, the mod loader's own command registry, which is
// shared by both front-ends: the ImGui developer console on client builds and
// the Win32 console window on any build launched with -console. A command
// registered once is available in whichever of those the user has, and is
// listed by `help` under the registering plugin's name.
//
// These are mod loader commands, not engine commands -- the client console
// tries this registry first and falls through to the engine console for
// anything it does not recognise, so engine commands are unaffected.
// ---------------------------------------------------------------------------

// Line kinds mirror ModConsole::LineKind; the front-end colours them.
enum class PluginConsoleLineKind : int
{
    Output = 0,   // normal command output
    Notice = 1,   // dimmed supporting text (headers, hints)
    Error  = 2,   // something went wrong
};

// Opaque handle to the console output sink for one command invocation.
//
// Valid ONLY for the duration of the handler call it was passed to. The
// front-end owns the sink; keeping the handle and writing through it later
// (from a worker thread, say) is a use-after-free, and the loader answers it
// by dropping the write and logging rather than by crashing -- but the write
// is still lost. Print everything before returning.
typedef void* PluginConsoleSink;

// argv[0] is the command name exactly as the user typed it (which may be one
// of its aliases); argv[1..argc-1] are the arguments, with "quoted runs"
// already collapsed into single arguments. argv is valid for the call only.
typedef void (*PluginConsoleHandler)(const char* const* argv, int argc,
                                     PluginConsoleSink sink, void* userData);

// Execute output. Called once per line, on whichever thread ran the command --
// the game thread for a gameThread command, otherwise the calling thread.
typedef void (*PluginConsoleOutputCallback)(PluginConsoleLineKind kind,
                                            const char* text, void* userData);

// Fires after the last line of an Execute, on the same thread that wrote it.
typedef void (*PluginConsoleCompleteCallback)(void* userData);

struct PluginConsoleCommandDesc
{
    const char* name;        // primary name, case-insensitive, no spaces
    const char* aliases;     // space-separated alternates, may be null
    const char* usage;       // e.g. "mystuff <player> [amount]", may be null
    const char* help;        // one line, shown by `help`, may be null
    PluginConsoleHandler handler;
    void*       userData;    // passed back to handler; may be null

    // Run the handler on the game thread (next engine tick) instead of on the
    // console thread that typed the command. Required for anything touching
    // engine state; the command is queued, so Execute/typing returns before
    // the handler has run.
    bool        gameThread;
};

struct IPluginConsole
{
    // Add a command. The loader copies every string in desc, so they need not
    // outlive the call.
    //
    // False when a required field is missing, or when the name or one of its
    // aliases is already taken -- names are global and case-insensitive across
    // built-ins and every plugin. Check the return value and prefix generic
    // names with your plugin's own.
    bool (*RegisterCommand)(const IPluginSelf* self, const PluginConsoleCommandDesc* desc);

    // Remove one of your commands by its registered primary name (not an
    // alias). False if you do not own a command by that name.
    bool (*UnregisterCommand)(const IPluginSelf* self, const char* name);

    // Remove all of your commands; returns how many went. The loader does this
    // for you before unloading your DLL, but call it in PluginShutdown anyway.
    int (*UnregisterAllCommands)(const IPluginSelf* self);

    // True if any command (built-in or plugin) answers to this name or alias.
    bool (*HasCommand)(const char* name);

    // Write one line to the sink your handler was given. Ignored (and logged)
    // if the sink is no longer the one being served.
    void (*Write)(PluginConsoleSink sink, PluginConsoleLineKind kind, const char* text);
    void (*Printf)(PluginConsoleSink sink, PluginConsoleLineKind kind, const char* format, ...);

    // Wipe the hosting console's scrollback, where it has one. No-op otherwise.
    void (*Clear)(PluginConsoleSink sink);

    // Run a command line and receive its output through your own callback
    // instead of a console window -- the way to get a textual result out of a
    // command from code.
    //
    // False, having run nothing, when the first token is not a registered
    // command. True once the command has *started*: a gameThread command is
    // still queued at that point, and both callbacks fire later, on the game
    // thread. userData must stay alive until onComplete has fired.
    bool (*Execute)(const IPluginSelf* self, const char* line,
                    PluginConsoleOutputCallback onLine,
                    PluginConsoleCompleteCallback onComplete,
                    void* userData);
};

struct IPluginHooks
{
	IPluginSpawnerHooks*   Spawner;        // v14
	IPluginHookUtils*  Hooks;    // v14
	IPluginMemoryUtils*    Memory;         // v14
	IPluginEngineEvents*   Engine;         // v14
	IPluginWorldEvents*    World;          // v14
	IPluginPlayerEvents* Players;        // v14
	IPluginActorEvents*    Actors;       // v14
	IPluginInputEvents*    Input;          // v15 — client only, null on server
	IPluginUIEvents*  UI;          // v15 — client only, null on server
	IPluginHUDEvents*      HUD;// v16 — client only, null on server
	IPluginNetworkChannel* Network;   // v17 — server+client; null on generic
	IPluginNativePointers* NativePointers; // v21
	IPluginHttpServer*     HttpServer;     // v22 — server only, null on client/generic
	IPluginNetModeInfo*    NetMode;          // v36 — server + client; null on generic
	IPluginTextUtils*      Text;             // FText localization helpers (AsLocalizable_Advanced, Conv_TextToString)
	IPluginImGuiTextures*  ImGuiTextures;    // v37 — client only; null on server/generic
	IPluginSplash*         Splash;           // v40 — client only; null on server/generic
	IPluginCraftingEvents* Crafting;       // v44 -- appended at end to preserve layout for v42/v43 plugins
	IPluginObjectWalker*   ObjectWalker;   // v47 -- appended at end, do not relocate
	IPluginDelegateHook*   Delegate;       // v47 -- appended at end, do not relocate
	IPluginObjectProperties* ObjectProperties; // v47 -- appended at end, do not relocate
	IPluginConsole*        Console;         // v63 -- appended at end, do not relocate
};

// ---------------------------------------------------------------------------
// Plugin build target
// ---------------------------------------------------------------------------
enum PluginTarget : int
{
    PLUGIN_TARGET_CLIENT = 0,
    PLUGIN_TARGET_SERVER = 1,
};

#define PLUGIN_TARGET_CLIENT_ONLY  PLUGIN_TARGET_CLIENT
#define PLUGIN_TARGET_SERVER_ONLY  PLUGIN_TARGET_SERVER

// ---------------------------------------------------------------------------
// Plugin metadata and identity
// ---------------------------------------------------------------------------
struct PluginInfo
{
	const char* name;
	const char* version;
	const char* author;
	const char* description;
	int interfaceVersion;
	int pluginTarget;  // PluginTarget value -- required
};

struct IPluginSelf
{
	const char*     name;
	const char*     version;
	IPluginLogger*  logger;
	IPluginConfig*  config;

	// Null for the duration of OnPluginLoadHooks -- that event resolves
	// addresses, it does not install anything (see IPluginHookScanner).
	// Populated before PluginInit and for the whole life of the plugin.
	IPluginHooks*   hooks;
};

typedef PluginInfo* (*GetPluginInfoFunc)();
typedef bool        (*PluginInitFunc)(IPluginSelf* self);
typedef void        (*PluginShutdownFunc)();

// OPTIONAL fourth export. Called once per load, after GetPluginInfo and before
// PluginInit, and the only context in which a plugin can pattern scan. Resolve
// every AOB you depend on here; if ANY of them misses -- required or optional --
// the loader refuses the plugin: PluginInit is never called, the DLL is freed,
// and the miss is listed in the failure window.
//
// Plugins that do not scan simply do not export it.
//
//   extern "C" __declspec(dllexport)
//   void OnPluginLoadHooks(IPluginSelf* self, IPluginHookScanner* scan)
//   {
//       g_finishCrafting = scan->ResolveRequired(self,
//           "UCrCraftingComponent::FinishCrafting",
//           "40 55 53 56 57 41 56 48 8D 6C 24 ??");
//   }
typedef void        (*PluginLoadHooksFunc)(IPluginSelf* self, IPluginHookScanner* scanner);

#define PLUGIN_GET_INFO_FUNC_NAME   "GetPluginInfo"
#define PLUGIN_INIT_FUNC_NAME       "PluginInit"
#define PLUGIN_SHUTDOWN_FUNC_NAME   "PluginShutdown"
#define PLUGIN_LOAD_HOOKS_FUNC_NAME "OnPluginLoadHooks"
