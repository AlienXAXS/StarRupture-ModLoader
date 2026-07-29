#pragma once

#ifdef MODLOADER_CLIENT_BUILD

#include <cstdint>

// ---------------------------------------------------------------------------
// DebugDraw -- in-world 3D debug rendering (client builds only)
//
// Every UKismetSystemLibrary::DrawDebug* UFUNCTION is a no-op on this shipping
// build. ENABLE_DRAW_DEBUG is 0, so all sixteen implementations compiled down
// to empty bodies and the linker dropped them; what survives is only the
// execDrawDebugX thunk, which FFrame::Step's each parameter off the stack and
// then returns without calling anything. There is no standalone
// UKismetSystemLibrary::DrawDebugLine and no global DrawDebugHelpers.cpp free
// function anywhere in the binary. The DrawDebugType pin on the trace nodes is
// dead for the same reason -- execLineTraceSingle / execSphereTraceSingle call
// only UWorld::LineTraceSingleByChannel / SweepSingleByChannel.
//
// The renderer underneath is completely intact, though, so this module
// reimplements the same geometry on top of it:
//
//   UWorld::LineBatchers[4]                    (offset 0xF0, exposed by the SDK)
//   ULineBatchComponent::DrawLines(TArrayView<FBatchedLine>)   (AOB-scanned)
//   UWorld::FlushLineBatchers                  (driven by UGameViewportClient::Draw)
//
// UWorld::UpdateWorldComponents NewObject's all four batchers into the
// transient package and registers them with the world, so they are live
// objects at runtime rather than null slots -- nothing needs to be created.
//
// ELineBatcherType ordering was read out of the binary rather than assumed:
// UGameViewportClient::Draw flushes {0, 2} each frame and the
// FLUSHPERSISTENTDEBUGLINES exec handler flushes {1, 3}, which gives
//   0 = World, 1 = WorldPersistent, 2 = Foreground, 3 = ForegroundPersistent
// (note this is NOT World/Foreground/WorldPersistent/ForegroundPersistent).
//
// Lifetime rules mirror the engine's GetDebugLineBatcher/GetDebugLineLifeTime:
// a duration greater than zero (or an explicit bPersistent) routes the lines
// into the persistent batcher so they survive the per-frame flush; anything
// else goes to the one-frame batcher.
//
// Threading: everything in this namespace is GAME THREAD ONLY. Each Draw*
// mutates a UObject's TArray; everything is SEH-guarded, but a cross-thread
// call can corrupt the batcher's array long before anything faults.
//
// This is the raw layer -- callers here are responsible for being on the game
// thread. The plugin-facing wrappers in hooks_interface.cpp are not: they copy
// their arguments and defer through GameThreadDispatch when needed, because
// plugins draw from ImGui callbacks that run on the render thread. Any new
// modloader-internal caller (like the Debug-section test button, which is
// clicked on the render thread) has to do the same by hand.
//
// Two primitives deviate from the engine, because only DrawLines survived as
// an out-of-line function -- DrawPoint (BatchedPoints) and the filled quad in
// DrawPlane (BatchedMeshes) have no reachable entry point. Both are drawn from
// line segments instead; see the notes on those functions below.
// ---------------------------------------------------------------------------

namespace Hooks::DebugDraw
{
    // Geometry PODs. Deliberately plain so the plugin-facing structs in
    // plugin_interface.h can be layout-identical and cast straight across;
    // hooks_interface.cpp static_asserts that they still match.
    struct DVec   { double x, y, z; };
    struct DRot   { double pitch, yaw, roll; };   // degrees, UE convention
    struct DColor { float  r, g, b, a; };         // linear, 0..1
    struct DPlane { double x, y, z, w; };         // normal + distance along it

    struct DTransform
    {
        DVec location;
        DRot rotation;
        DVec scale;
    };

    // Caller-owned sample array; read during the call and never retained.
    struct DFloatHistory
    {
        const float* samples;
        int32_t      count;
        float        minValue;
        float        maxValue;
        bool         bAutoAdjustMinMax;
    };

    struct DStyle
    {
        DColor color;
        float  duration;      // <= 0: this frame only. > 0: seconds, persistent batcher.
        float  thickness;     // 0 = thin (single pixel) lines
        bool   bPersistent;   // never expires until FlushPersistentLines
        bool   bForeground;   // draw on top of world geometry
    };

    // Resolve ULineBatchComponent::DrawLines by AOB. Safe to call repeatedly;
    // only scans on the first call.
    bool Resolve();

    // True once Resolve() has succeeded.
    bool IsAvailable();

    // --- Primitives (mirroring the Kismet DrawDebug* set) ------------------

    void DrawLine(const DVec& start, const DVec& end, const DStyle& style);

    // NOTE: the engine draws this into BatchedPoints via ULineBatchComponent::
    // DrawPoint, which did not survive as an out-of-line function. Drawn here
    // as a three-axis cross of half-length `size` instead.
    void DrawPoint(const DVec& position, float size, const DStyle& style);

    void DrawCircle(const DVec& center, float radius, int32_t numSegments,
                    const DVec& yAxis, const DVec& zAxis, bool bDrawAxis,
                    const DStyle& style);

    void DrawSphere(const DVec& center, float radius, int32_t segments, const DStyle& style);

    void DrawBox(const DVec& center, const DVec& extent, const DRot& rotation, const DStyle& style);

    void DrawCapsule(const DVec& center, float halfHeight, float radius,
                     const DRot& rotation, const DStyle& style);

    void DrawCylinder(const DVec& start, const DVec& end, float radius,
                      int32_t segments, const DStyle& style);

    void DrawConeInDegrees(const DVec& origin, const DVec& direction, float length,
                           float angleWidthDeg, float angleHeightDeg, int32_t numSides,
                           const DStyle& style);

    void DrawArrow(const DVec& start, const DVec& end, float arrowSize, const DStyle& style);

    // Axes are drawn red/green/blue like the engine's version, so style.color
    // is ignored here (everything else in DStyle still applies).
    void DrawCoordinateSystem(const DVec& location, const DRot& rotation, float scale,
                              const DStyle& style);

    // NOTE: the engine fills this quad through ULineBatchComponent::DrawMesh
    // (BatchedMeshes), which is unreachable here. Drawn as the four border
    // edges plus both diagonals, with the same yellow normal arrow.
    void DrawPlane(const DPlane& plane, const DVec& location, float size, const DStyle& style);

    void DrawFrustum(const DTransform& frustumTransform, const DStyle& style);

    // Location/rotation/FOV form -- the geometry half of the engine's
    // DrawDebugCamera, with no actor lookup.
    void DrawCameraAt(const DVec& location, const DRot& rotation, float fovDegrees,
                      float scale, const DStyle& style);

    // ACameraActor* form: pulls location/rotation off the actor and FOV off its
    // UCameraComponent, then defers to DrawCameraAt. No-op if the actor or its
    // camera component is null.
    void DrawCamera(void* cameraActor, float scale, const DStyle& style);

    // Canvas-space text pinned to a world location. Routed through
    // AHUD::AddDebugText on player controller 0, which is what the engine's
    // DrawDebugString does -- AHUD::DrawDebugTextList is still live and renders
    // it. Independent of the line batchers; FlushPersistentLines does not clear
    // these, ClearAllStrings does.
    // testBaseActor may be null, in which case the WorldSettings actor is
    // substituted as the anchor and the text is placed at an absolute world
    // location -- exactly what the engine's own DrawDebugString does. The
    // substitution is not cosmetic: AHUD::DrawDebugTextList skips and then
    // deletes any list entry whose SrcActor is null, so passing one straight
    // through means the text is silently dropped on the next HUD render.
    //
    // duration uses the HUD's rule, NOT the line batcher's: DrawDebugTextList
    // guards its countdown with "if (-1.0 != TimeRemaining)", so only exactly
    // -1.0f means "never expire". Anything else counts down, including 0, which
    // disappears on the next HUD render. (The line batcher, by contrast, leaves
    // anything <= 0 alone forever.)
    //
    // fontScale multiplies the text size directly -- DrawDebugTextList assigns
    // it straight to FCanvasTextItem::Scale with no distance falloff and no
    // clamping, so 2.0f is exactly twice the size at any range. 1.0f is the
    // engine's small font at native size; anything <= 0 is treated as 1.0f so a
    // zeroed style can't produce invisible text. The font itself is always
    // UEngine::GetSmallFont() (AHUD's InFont parameter is left null).
    void DrawString(const DVec& location, const wchar_t* text, void* testBaseActor,
                    const DColor& color, float duration, float fontScale);

    void ClearAllStrings();

    void DrawFloatHistoryTransform(const DFloatHistory& history, const DTransform& drawTransform,
                                   const DVec& drawSize, const DStyle& style);

    void DrawFloatHistoryLocation(const DFloatHistory& history, const DVec& drawLocation,
                                  const DVec& drawSize, const DStyle& style);

    // Clear both persistent batchers (equivalent to the engine's
    // FLUSHPERSISTENTDEBUGLINES exec command).
    void FlushPersistentLines();

#ifdef _DEBUG
    // Draws one of every primitive in a labelled row in front of the local
    // pawn, persistent for 30 seconds, so the whole surface can be eyeballed
    // in one go. Wired to the "Test Debug Draw" button in the modloader
    // window's Debug section. Game thread only, like everything else here.
    // Debug builds only -- this is a test harness, not shipped behaviour.
    void DrawTestScene();
#endif
}

#endif // MODLOADER_CLIENT_BUILD
