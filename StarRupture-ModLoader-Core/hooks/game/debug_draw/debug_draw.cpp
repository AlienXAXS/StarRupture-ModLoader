#include "pch.h"
#include "debug_draw.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "Engine_classes.hpp"   // UWorld, ULineBatchComponent, AHUD, UGameplayStatics
#include "../scan_patterns.h"
#include <vector>
#include <cmath>
#include <algorithm>

namespace Hooks::DebugDraw
{
    // -----------------------------------------------------------------------
    // Native binding
    // -----------------------------------------------------------------------

    // TArrayView<FBatchedLine, int32>. MSVC x64 passes an aggregate this size
    // indirectly, so DrawLines receives a pointer to it in RDX -- confirmed in
    // the disassembly, which reads the element count from [rdx+8].
    struct TArrayViewPod
    {
        SDK::FBatchedLine* data;
        int32_t            num;
        int32_t            pad;
    };
    static_assert(sizeof(SDK::FBatchedLine) == 0x50, "FBatchedLine size mismatch");

    using DrawLines_t = void(__fastcall*)(void* lineBatchComponent, const TArrayViewPod* lines);
    using Flush_t     = void(__fastcall*)(void* lineBatchComponent);

    static DrawLines_t g_drawLines     = nullptr;
    static bool        g_scanAttempted = false;

    bool IsAvailable() { return g_drawLines != nullptr; }

    bool Resolve()
    {
        if (g_drawLines)     return true;
        if (g_scanAttempted) return false;
        g_scanAttempted = true;

        const uintptr_t addr = Scanner::FindPatternInMainModule(
            "ULineBatchComponent::DrawLines",
            ScanPatterns::ULineBatchComponent_DrawLines);

        if (!addr)
        {
            ModLoaderLogger::LogWarn(
                L"[DebugDraw] [FAIL] ULineBatchComponent::DrawLines pattern not found "
                L"-- in-world debug drawing is unavailable");
            return false;
        }

        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        g_drawLines = reinterpret_cast<DrawLines_t>(addr);
        ModLoaderLogger::LogInfo(
            L"[DebugDraw] [OK] ULineBatchComponent::DrawLines at 0x%llX (base+0x%llX)",
            static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(addr - base));
        return true;
    }

    // ELineBatcherType, read out of the binary rather than assumed -- see the
    // header comment. UGameViewportClient::Draw flushes {0,2} per frame and
    // FLUSHPERSISTENTDEBUGLINES flushes {1,3}.
    enum : int32_t
    {
        BatcherWorld                = 0,
        BatcherWorldPersistent      = 1,
        BatcherForeground           = 2,
        BatcherForegroundPersistent = 3,
    };

    // Native reads in their own SEH-guarded helpers, with no C++ objects that
    // need unwinding in scope (C2712).
    static void* GetBatcherSEH(int32_t index)
    {
        __try
        {
            SDK::UWorld* world = SDK::UWorld::GetWorld();
            if (!world)
                return nullptr;
            return world->LineBatchers[index];
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static bool CallDrawLinesSEH(void* component, const TArrayViewPod* view)
    {
        __try
        {
            g_drawLines(component, view);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // -----------------------------------------------------------------------
    // Style helpers
    // -----------------------------------------------------------------------

    // Mirrors the engine's GetDebugLineBatcher: anything with a positive
    // duration has to live past the per-frame flush, so it goes into the
    // persistent batcher.
    static bool UsePersistentBatcher(const DStyle& style)
    {
        return style.bPersistent || style.duration > 0.0f;
    }

    // Mirrors GetDebugLineLifeTime. -1 means "never expires"; the one-frame
    // batchers are flushed wholesale each frame so their value is irrelevant.
    static float LifeTimeFor(const DStyle& style)
    {
        if (style.bPersistent)
            return -1.0f;
        return (style.duration > 0.0f) ? style.duration : 0.0f;
    }

    static int32_t BatcherIndexFor(const DStyle& style)
    {
        return (style.bForeground ? BatcherForeground : BatcherWorld)
             + (UsePersistentBatcher(style) ? 1 : 0);
    }

    // -----------------------------------------------------------------------
    // Small vector math -- doubles throughout to match UE5's FVector
    // -----------------------------------------------------------------------

    static constexpr double kPi = 3.14159265358979323846;

    static DVec VAdd(const DVec& a, const DVec& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    static DVec VSub(const DVec& a, const DVec& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    static DVec VMul(const DVec& v, double s)      { return { v.x * s, v.y * s, v.z * s }; }
    static DVec VNeg(const DVec& v)                { return { -v.x, -v.y, -v.z }; }

    static double VDot(const DVec& a, const DVec& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    static DVec VCross(const DVec& a, const DVec& b)
    {
        return { a.y * b.z - a.z * b.y,
                 a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x };
    }

    static double VLen(const DVec& v) { return std::sqrt(VDot(v, v)); }

    static DVec VNorm(const DVec& v, const DVec& fallback)
    {
        const double len = VLen(v);
        if (len < 1e-8)
            return fallback;
        return VMul(v, 1.0 / len);
    }

    // FRotationTranslationMatrix: pitch/yaw/roll in degrees to an orthonormal
    // basis, matching the engine exactly so rotated boxes/capsules line up
    // with anything the game itself places.
    static void RotatorAxes(const DRot& r, DVec& outX, DVec& outY, DVec& outZ)
    {
        const double toRad = kPi / 180.0;
        const double sp = std::sin(r.pitch * toRad), cp = std::cos(r.pitch * toRad);
        const double sy = std::sin(r.yaw   * toRad), cy = std::cos(r.yaw   * toRad);
        const double sr = std::sin(r.roll  * toRad), cr = std::cos(r.roll  * toRad);

        outX = { cp * cy, cp * sy, sp };
        outY = { sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp };
        outZ = { -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp };
    }

    // FVector::FindBestAxisVectors -- picks a stable perpendicular basis for a
    // direction vector.
    static void FindBestAxisVectors(const DVec& dir, DVec& outAxis1, DVec& outAxis2)
    {
        const double nx = std::fabs(dir.x);
        const double ny = std::fabs(dir.y);
        const double nz = std::fabs(dir.z);

        DVec axis1 = (nz > nx && nz > ny) ? DVec{ 1.0, 0.0, 0.0 } : DVec{ 0.0, 0.0, 1.0 };
        axis1 = VNorm(VSub(axis1, VMul(dir, VDot(axis1, dir))), DVec{ 1.0, 0.0, 0.0 });

        outAxis1 = axis1;
        outAxis2 = VCross(axis1, dir);
    }

    // -----------------------------------------------------------------------
    // Line accumulation + submission
    // -----------------------------------------------------------------------

    using LineBuf = std::vector<SDK::FBatchedLine>;

    static void Emit(LineBuf& out, const DVec& a, const DVec& b,
                     const DColor& color, const DStyle& style)
    {
        SDK::FBatchedLine line{};
        line.Start             = SDK::FVector{ a.x, a.y, a.z };
        line.End               = SDK::FVector{ b.x, b.y, b.z };
        line.Color             = SDK::FLinearColor{ color.r, color.g, color.b, color.a };
        line.Thickness         = style.thickness;
        line.RemainingLifeTime = LifeTimeFor(style);
        // SDPG_World = 0, SDPG_Foreground = 3.
        line.DepthPriority     = style.bForeground ? 3 : 0;
        line.BatchID           = 0;
        out.push_back(line);
    }

    static void Emit(LineBuf& out, const DVec& a, const DVec& b, const DStyle& style)
    {
        Emit(out, a, b, style.color, style);
    }

    static void Submit(LineBuf& lines, const DStyle& style)
    {
        if (lines.empty())
            return;

        if (!Resolve())
            return;

        void* component = GetBatcherSEH(BatcherIndexFor(style));
        if (!component)
            return;

        TArrayViewPod view{};
        view.data = lines.data();
        view.num  = static_cast<int32_t>(lines.size());

        if (!CallDrawLinesSEH(component, &view))
            ModLoaderLogger::LogError(
                L"[DebugDraw] Exception in ULineBatchComponent::DrawLines (%d lines)",
                view.num);
    }

    // Shared circle/half-circle emitters. Both match the engine's DrawCircle /
    // DrawHalfCircle helpers, which sweep the plane spanned by X and Y.
    static void EmitCircle(LineBuf& out, const DVec& base, const DVec& x, const DVec& y,
                           double radius, int32_t numSides, const DStyle& style)
    {
        numSides = std::max(numSides, 4);
        const double angleDelta = 2.0 * kPi / static_cast<double>(numSides);

        DVec last = VAdd(base, VMul(x, radius));
        for (int32_t i = 0; i < numSides; ++i)
        {
            const double angle = angleDelta * (i + 1);
            const DVec vertex = VAdd(base,
                VMul(VAdd(VMul(x, std::cos(angle)), VMul(y, std::sin(angle))), radius));
            Emit(out, last, vertex, style);
            last = vertex;
        }
    }

    static void EmitHalfCircle(LineBuf& out, const DVec& base, const DVec& x, const DVec& y,
                               double radius, int32_t numSides, const DStyle& style)
    {
        numSides = std::max(numSides, 4);
        const double angleDelta = 2.0 * kPi / static_cast<double>(numSides);

        DVec last = VAdd(base, VMul(x, radius));
        for (int32_t i = 0; i < numSides / 2; ++i)
        {
            const double angle = angleDelta * (i + 1);
            const DVec vertex = VAdd(base,
                VMul(VAdd(VMul(x, std::cos(angle)), VMul(y, std::sin(angle))), radius));
            Emit(out, last, vertex, style);
            last = vertex;
        }
    }

    static void EmitArrow(LineBuf& out, const DVec& start, const DVec& end,
                          double arrowSize, const DColor& color, const DStyle& style)
    {
        Emit(out, start, end, color, style);

        if (arrowSize <= 0.0)
            arrowSize = 10.0;

        const DVec dir = VNorm(VSub(end, start), DVec{ 1.0, 0.0, 0.0 });

        DVec up{ 0.0, 0.0, 1.0 };
        DVec right = VCross(dir, up);
        if (VLen(right) < 1e-4)
            FindBestAxisVectors(dir, up, right);
        else
            right = VNorm(right, DVec{ 0.0, 1.0, 0.0 });

        // Engine builds a basis with dir as X, right as Y, up as Z and offsets
        // the head by (-ArrowSize, +/-ArrowSize, 0).
        const DVec back = VMul(dir, -arrowSize);
        Emit(out, end, VAdd(end, VAdd(back, VMul(right,  arrowSize))), color, style);
        Emit(out, end, VAdd(end, VAdd(back, VMul(right, -arrowSize))), color, style);
    }

    // -----------------------------------------------------------------------
    // Primitives
    // -----------------------------------------------------------------------

    void DrawLine(const DVec& start, const DVec& end, const DStyle& style)
    {
        LineBuf lines;
        lines.reserve(1);
        Emit(lines, start, end, style);
        Submit(lines, style);
    }

    void DrawPoint(const DVec& position, float size, const DStyle& style)
    {
        const double h = (size > 0.0f) ? (size * 0.5) : 1.0;

        LineBuf lines;
        lines.reserve(3);
        Emit(lines, { position.x - h, position.y, position.z }, { position.x + h, position.y, position.z }, style);
        Emit(lines, { position.x, position.y - h, position.z }, { position.x, position.y + h, position.z }, style);
        Emit(lines, { position.x, position.y, position.z - h }, { position.x, position.y, position.z + h }, style);
        Submit(lines, style);
    }

    void DrawCircle(const DVec& center, float radius, int32_t numSegments,
                    const DVec& yAxis, const DVec& zAxis, bool bDrawAxis,
                    const DStyle& style)
    {
        const DVec y = VNorm(yAxis, DVec{ 0.0, 1.0, 0.0 });
        const DVec z = VNorm(zAxis, DVec{ 0.0, 0.0, 1.0 });

        numSegments = std::max(numSegments, 4);

        LineBuf lines;
        lines.reserve(static_cast<size_t>(numSegments) + 2);
        EmitCircle(lines, center, y, z, radius, numSegments, style);

        if (bDrawAxis)
        {
            Emit(lines, VSub(center, VMul(y, radius)), VAdd(center, VMul(y, radius)), style);
            Emit(lines, VSub(center, VMul(z, radius)), VAdd(center, VMul(z, radius)), style);
        }

        Submit(lines, style);
    }

    void DrawSphere(const DVec& center, float radius, int32_t segments, const DStyle& style)
    {
        segments = std::max(segments, 4);

        LineBuf lines;
        lines.reserve(static_cast<size_t>(segments) * segments * 2);

        const double angleInc = 2.0 * kPi / static_cast<double>(segments);
        double latitude = angleInc;
        double sinY1 = 0.0, cosY1 = 1.0;

        for (int32_t y = 0; y < segments; ++y)
        {
            const double sinY2 = std::sin(latitude);
            const double cosY2 = std::cos(latitude);

            DVec vertex1 = VAdd(VMul(DVec{ sinY1, 0.0, cosY1 }, radius), center);
            DVec vertex3 = VAdd(VMul(DVec{ sinY2, 0.0, cosY2 }, radius), center);

            double longitude = angleInc;
            for (int32_t x = 0; x < segments; ++x)
            {
                const double sinX = std::sin(longitude);
                const double cosX = std::cos(longitude);

                const DVec vertex2 = VAdd(VMul(DVec{ cosX * sinY1, sinX * sinY1, cosY1 }, radius), center);
                const DVec vertex4 = VAdd(VMul(DVec{ cosX * sinY2, sinX * sinY2, cosY2 }, radius), center);

                Emit(lines, vertex1, vertex2, style);
                Emit(lines, vertex1, vertex3, style);

                vertex1 = vertex2;
                vertex3 = vertex4;
                longitude += angleInc;
            }

            sinY1 = sinY2;
            cosY1 = cosY2;
            latitude += angleInc;
        }

        Submit(lines, style);
    }

    void DrawBox(const DVec& center, const DVec& extent, const DRot& rotation, const DStyle& style)
    {
        DVec ax, ay, az;
        RotatorAxes(rotation, ax, ay, az);

        const DVec ex = VMul(ax, extent.x);
        const DVec ey = VMul(ay, extent.y);
        const DVec ez = VMul(az, extent.z);

        // Corner index bits: 1 = +X, 2 = +Y, 4 = +Z.
        DVec corners[8];
        for (int i = 0; i < 8; ++i)
        {
            DVec c = center;
            c = VAdd(c, (i & 1) ? ex : VNeg(ex));
            c = VAdd(c, (i & 2) ? ey : VNeg(ey));
            c = VAdd(c, (i & 4) ? ez : VNeg(ez));
            corners[i] = c;
        }

        static const int kEdges[12][2] = {
            { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },   // bottom face
            { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },   // top face
            { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },   // verticals
        };

        LineBuf lines;
        lines.reserve(12);
        for (const auto& e : kEdges)
            Emit(lines, corners[e[0]], corners[e[1]], style);

        Submit(lines, style);
    }

    void DrawCapsule(const DVec& center, float halfHeight, float radius,
                     const DRot& rotation, const DStyle& style)
    {
        const int32_t kSides = 16;

        DVec ax, ay, az;
        RotatorAxes(rotation, ax, ay, az);

        const double halfAxis = std::max<double>(static_cast<double>(halfHeight) - radius, 1.0);
        const DVec topEnd    = VAdd(center, VMul(az, halfAxis));
        const DVec bottomEnd = VSub(center, VMul(az, halfAxis));

        LineBuf lines;
        lines.reserve(static_cast<size_t>(kSides) * 6 + 4);

        EmitCircle(lines, topEnd,    ax, ay, radius, kSides, style);
        EmitCircle(lines, bottomEnd, ax, ay, radius, kSides, style);

        EmitHalfCircle(lines, topEnd, ay, az, radius, kSides, style);
        EmitHalfCircle(lines, topEnd, ax, az, radius, kSides, style);

        const DVec negZ = VNeg(az);
        EmitHalfCircle(lines, bottomEnd, ay, negZ, radius, kSides, style);
        EmitHalfCircle(lines, bottomEnd, ax, negZ, radius, kSides, style);

        Emit(lines, VAdd(topEnd, VMul(ax,  radius)), VAdd(bottomEnd, VMul(ax,  radius)), style);
        Emit(lines, VAdd(topEnd, VMul(ax, -radius)), VAdd(bottomEnd, VMul(ax, -radius)), style);
        Emit(lines, VAdd(topEnd, VMul(ay,  radius)), VAdd(bottomEnd, VMul(ay,  radius)), style);
        Emit(lines, VAdd(topEnd, VMul(ay, -radius)), VAdd(bottomEnd, VMul(ay, -radius)), style);

        Submit(lines, style);
    }

    void DrawCylinder(const DVec& start, const DVec& end, float radius,
                      int32_t segments, const DStyle& style)
    {
        segments = std::max(segments, 4);

        const DVec axis = VNorm(VSub(end, start), DVec{ 0.0, 0.0, 1.0 });

        DVec perp, dummy;
        FindBestAxisVectors(axis, perp, dummy);
        const DVec perp2 = VCross(axis, perp);

        LineBuf lines;
        lines.reserve(static_cast<size_t>(segments) * 3);

        const double angleInc = 2.0 * kPi / static_cast<double>(segments);
        auto ring = [&](double angle) -> DVec
        {
            return VMul(VAdd(VMul(perp, std::cos(angle)), VMul(perp2, std::sin(angle))), radius);
        };

        DVec offsetPrev = ring(0.0);
        for (int32_t i = 0; i < segments; ++i)
        {
            const DVec offset = ring(angleInc * (i + 1));

            const DVec b1 = VAdd(start, offsetPrev);
            const DVec b2 = VAdd(start, offset);
            const DVec t1 = VAdd(end,   offsetPrev);
            const DVec t2 = VAdd(end,   offset);

            Emit(lines, b1, b2, style);   // bottom rim
            Emit(lines, t1, t2, style);   // top rim
            Emit(lines, b1, t1, style);   // wall

            offsetPrev = offset;
        }

        Submit(lines, style);
    }

    void DrawConeInDegrees(const DVec& origin, const DVec& direction, float length,
                           float angleWidthDeg, float angleHeightDeg, int32_t numSides,
                           const DStyle& style)
    {
        numSides = std::max(numSides, 4);

        const double toRad = kPi / 180.0;
        const double kSmall = 1e-4;
        const double angle1 = std::clamp(static_cast<double>(angleHeightDeg) * toRad, kSmall, kPi - kSmall);
        const double angle2 = std::clamp(static_cast<double>(angleWidthDeg)  * toRad, kSmall, kPi - kSmall);

        const double sinX2 = std::sin(0.5 * angle1);
        const double sinY2 = std::sin(0.5 * angle2);
        const double sinSqX2 = sinX2 * sinX2;
        const double sinSqY2 = sinY2 * sinY2;

        // Cone-space vertices, exactly as the engine derives them.
        std::vector<DVec> coneVerts(static_cast<size_t>(numSides));
        for (int32_t i = 0; i < numSides; ++i)
        {
            const double fraction = static_cast<double>(i) / static_cast<double>(numSides);
            const double thi = 2.0 * kPi * fraction;
            const double phi = std::atan2(std::sin(thi) * sinY2, std::cos(thi) * sinX2);
            const double sinPhi = std::sin(phi);
            const double cosPhi = std::cos(phi);
            const double sinSqPhi = sinPhi * sinPhi;
            const double cosSqPhi = cosPhi * cosPhi;

            const double denom = sinSqX2 * sinSqPhi + sinSqY2 * cosSqPhi;
            const double rSq   = (denom > 0.0) ? (sinSqX2 * sinSqY2 / denom) : 0.0;
            const double r     = std::sqrt(rSq);
            const double sqr   = std::sqrt(std::max(0.0, 1.0 - rSq));
            const double alpha = r * cosPhi;
            const double beta  = r * sinPhi;

            coneVerts[static_cast<size_t>(i)] = { 1.0 - 2.0 * rSq, 2.0 * sqr * alpha, 2.0 * sqr * beta };
        }

        const DVec dirNorm = VNorm(direction, DVec{ 1.0, 0.0, 0.0 });
        DVec yAxis, zAxis;
        FindBestAxisVectors(dirNorm, yAxis, zAxis);

        auto coneToWorld = [&](const DVec& v) -> DVec
        {
            DVec p = origin;
            p = VAdd(p, VMul(dirNorm, v.x * length));
            p = VAdd(p, VMul(yAxis,   v.y * length));
            p = VAdd(p, VMul(zAxis,   v.z * length));
            return p;
        };

        LineBuf lines;
        lines.reserve(static_cast<size_t>(numSides) * 2 + 1);

        DVec firstPoint{}, prevPoint{}, currentPoint{};
        for (int32_t i = 0; i < numSides; ++i)
        {
            currentPoint = coneToWorld(coneVerts[static_cast<size_t>(i)]);
            Emit(lines, origin, currentPoint, style);

            if (i > 0)
                Emit(lines, prevPoint, currentPoint, style);
            else
                firstPoint = currentPoint;

            prevPoint = currentPoint;
        }
        Emit(lines, currentPoint, firstPoint, style);

        Submit(lines, style);
    }

    void DrawArrow(const DVec& start, const DVec& end, float arrowSize, const DStyle& style)
    {
        LineBuf lines;
        lines.reserve(3);
        EmitArrow(lines, start, end, arrowSize, style.color, style);
        Submit(lines, style);
    }

    void DrawCoordinateSystem(const DVec& location, const DRot& rotation, float scale,
                              const DStyle& style)
    {
        DVec ax, ay, az;
        RotatorAxes(rotation, ax, ay, az);

        // Fixed axis colours, matching the engine -- style.color is ignored.
        const DColor red  { 1.0f, 0.0f, 0.0f, 1.0f };
        const DColor green{ 0.0f, 1.0f, 0.0f, 1.0f };
        const DColor blue { 0.0f, 0.0f, 1.0f, 1.0f };

        LineBuf lines;
        lines.reserve(3);
        Emit(lines, location, VAdd(location, VMul(ax, scale)), red,   style);
        Emit(lines, location, VAdd(location, VMul(ay, scale)), green, style);
        Emit(lines, location, VAdd(location, VMul(az, scale)), blue,  style);
        Submit(lines, style);
    }

    void DrawPlane(const DPlane& plane, const DVec& location, float size, const DStyle& style)
    {
        // FPlane::PlaneDot works on the raw plane values, and the engine's
        // ClosestPtOnPlane scales the raw XYZ by it -- keep both unnormalised
        // so a caller passing an unnormalised plane gets the same answer the
        // engine would give. The normalised copy is only for the basis/arrow.
        const DVec raw{ plane.x, plane.y, plane.z };
        const double planeDot = VDot(raw, location) - plane.w;
        const DVec closest = VSub(location, VMul(raw, planeDot));

        const DVec normal = VNorm(raw, DVec{ 0.0, 0.0, 1.0 });

        DVec u, v;
        FindBestAxisVectors(normal, u, v);
        u = VMul(u, size);
        v = VMul(v, size);

        const DVec c0 = VAdd(closest, VAdd(u, v));
        const DVec c1 = VAdd(closest, VAdd(VNeg(u), v));
        const DVec c2 = VAdd(closest, VAdd(VNeg(u), VNeg(v)));
        const DVec c3 = VAdd(closest, VAdd(u, VNeg(v)));

        LineBuf lines;
        lines.reserve(9);

        // Border plus both diagonals, standing in for the engine's filled quad.
        Emit(lines, c0, c1, style);
        Emit(lines, c1, c2, style);
        Emit(lines, c2, c3, style);
        Emit(lines, c3, c0, style);
        Emit(lines, c0, c2, style);
        Emit(lines, c1, c3, style);

        const DColor yellow{ 1.0f, 1.0f, 0.0f, 1.0f };
        EmitArrow(lines, closest, VAdd(closest, VMul(normal, 16.0)), 8.0, yellow, style);

        Submit(lines, style);
    }

    void DrawFrustum(const DTransform& frustumTransform, const DStyle& style)
    {
        DVec ax, ay, az;
        RotatorAxes(frustumTransform.rotation, ax, ay, az);

        // FTransform::ToMatrixWithScale, applied to the unit frustum box the
        // engine unprojects. The transform is affine so W stays 1 and the
        // engine's perspective divide is a no-op.
        auto transform = [&](double x, double y, double z) -> DVec
        {
            DVec p = frustumTransform.location;
            p = VAdd(p, VMul(ax, x * frustumTransform.scale.x));
            p = VAdd(p, VMul(ay, y * frustumTransform.scale.y));
            p = VAdd(p, VMul(az, z * frustumTransform.scale.z));
            return p;
        };

        // Indexed [X][Y][Z] exactly as the engine builds them.
        DVec verts[2][2][2];
        for (int z = 0; z < 2; ++z)
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x)
                    verts[x][y][z] = transform(x ? -1.0 : 1.0, y ? -1.0 : 1.0, z ? 0.0 : 1.0);

        LineBuf lines;
        lines.reserve(12);

        Emit(lines, verts[0][0][0], verts[0][0][1], style);
        Emit(lines, verts[1][0][0], verts[1][0][1], style);
        Emit(lines, verts[0][1][0], verts[0][1][1], style);
        Emit(lines, verts[1][1][0], verts[1][1][1], style);

        Emit(lines, verts[0][0][0], verts[0][1][0], style);
        Emit(lines, verts[1][0][0], verts[1][1][0], style);
        Emit(lines, verts[0][0][1], verts[0][1][1], style);
        Emit(lines, verts[1][0][1], verts[1][1][1], style);

        Emit(lines, verts[0][0][0], verts[1][0][0], style);
        Emit(lines, verts[0][1][0], verts[1][1][0], style);
        Emit(lines, verts[0][0][1], verts[1][0][1], style);
        Emit(lines, verts[0][1][1], verts[1][1][1], style);

        Submit(lines, style);
    }

    void DrawCameraAt(const DVec& location, const DRot& rotation, float fovDegrees,
                      float scale, const DStyle& style)
    {
        // Proportions lifted from the engine's DrawDebugCamera.
        const double baseScale = 4.0;
        const DVec   baseProportions{ 2.0, 1.0, 1.5 };

        DrawCoordinateSystem(location, rotation, static_cast<float>(baseScale * scale), style);

        const DVec extents = VMul(baseProportions, baseScale * scale);
        DrawBox(location, extents, rotation, style);

        DVec ax, ay, az;
        RotatorAxes(rotation, ax, ay, az);

        const DVec  lensPoint    = VAdd(location, VMul(ax, extents.x));
        const double lensSize    = baseProportions.z * scale * baseScale;
        const double halfLens    = lensSize * std::tan(static_cast<double>(fovDegrees) * 0.5 * kPi / 180.0);
        const DVec  lensCenter   = VAdd(lensPoint, VMul(ax, lensSize));

        const DVec corners[4] = {
            VAdd(lensCenter, VAdd(VMul(ay,  halfLens), VMul(az,  halfLens))),
            VAdd(lensCenter, VAdd(VMul(ay,  halfLens), VMul(az, -halfLens))),
            VAdd(lensCenter, VAdd(VMul(ay, -halfLens), VMul(az, -halfLens))),
            VAdd(lensCenter, VAdd(VMul(ay, -halfLens), VMul(az,  halfLens))),
        };

        LineBuf lines;
        lines.reserve(8);
        for (int i = 0; i < 4; ++i)
        {
            Emit(lines, lensPoint, corners[i], style);
            Emit(lines, corners[i], corners[(i + 1) % 4], style);
        }
        Submit(lines, style);
    }

    // Actor reads live in their own SEH helper -- FVector/FRotator are PODs so
    // nothing here needs unwinding.
    static bool ReadCameraActorSEH(void* cameraActor, DVec& outLocation, DRot& outRotation,
                                   float& outFov)
    {
        __try
        {
            SDK::ACameraActor* actor = static_cast<SDK::ACameraActor*>(cameraActor);
            SDK::UCameraComponent* camera = actor->CameraComponent;
            if (!camera)
                return false;

            const SDK::FVector  loc = actor->K2_GetActorLocation();
            const SDK::FRotator rot = actor->K2_GetActorRotation();

            outLocation = { loc.X, loc.Y, loc.Z };
            outRotation = { rot.Pitch, rot.Yaw, rot.Roll };
            outFov      = camera->FieldOfView;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void DrawCamera(void* cameraActor, float scale, const DStyle& style)
    {
        if (!cameraActor)
            return;

        DVec  location{};
        DRot  rotation{};
        float fov = 90.0f;

        if (!ReadCameraActorSEH(cameraActor, location, rotation, fov))
        {
            ModLoaderLogger::LogWarn(L"[DebugDraw] DrawCamera: could not read the camera actor");
            return;
        }

        DrawCameraAt(location, rotation, fov, scale, style);
    }

    // -----------------------------------------------------------------------
    // World-anchored text (AHUD::AddDebugText -> AHUD::DrawDebugTextList)
    // -----------------------------------------------------------------------

    // FLinearColor::ToFColor(true) -- linear to sRGB-encoded 8-bit.
    static uint8_t LinearToSRGB(float value)
    {
        float f = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
        f = (f <= 0.0031308f) ? (f * 12.92f)
                              : (1.055f * std::pow(f, 1.0f / 2.4f) - 0.055f);
        const int v = static_cast<int>(f * 255.0f + 0.5f);
        return static_cast<uint8_t>((v < 0) ? 0 : ((v > 255) ? 255 : v));
    }

    static bool AddDebugTextSEH(const wchar_t* text, void* srcActor, float duration,
                                double lx, double ly, double lz,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        __try
        {
            SDK::UWorld* world = SDK::UWorld::GetWorld();
            if (!world)
                return false;

            SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
            if (!pc || !pc->MyHUD)
                return false;

            // Non-owning FString over the caller's buffer. AHUD::AddDebugText
            // deep-copies into its FDebugTextInfo list, and ProcessEvent uses
            // the params block in place for native UFUNCTIONs rather than
            // destroying it, so nothing tries to free this.
            const SDK::FString debugText(text);
            const SDK::FVector location{ lx, ly, lz };
            SDK::FColor        color{};
            color.B = b;
            color.G = g;
            color.R = r;
            color.A = a;

            pc->MyHUD->AddDebugText(debugText,
                                    static_cast<SDK::AActor*>(srcActor),
                                    duration,
                                    location,
                                    location,
                                    color,
                                    /*bSkipOverwriteCheck*/ true,
                                    /*bAbsoluteLocation*/   srcActor == nullptr,
                                    /*bKeepAttachedToActor*/false,
                                    /*InFont*/              nullptr,
                                    /*FontScale*/           1.0f,
                                    /*bDrawShadow*/         false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void DrawString(const DVec& location, const wchar_t* text, void* testBaseActor,
                    const DColor& color, float duration)
    {
        if (!text || !text[0])
            return;

        if (!AddDebugTextSEH(text, testBaseActor, duration,
                             location.x, location.y, location.z,
                             LinearToSRGB(color.r), LinearToSRGB(color.g),
                             LinearToSRGB(color.b), LinearToSRGB(color.a)))
            ModLoaderLogger::LogWarn(L"[DebugDraw] DrawString: no HUD available (or the call faulted)");
    }

    static void ClearAllStringsSEH()
    {
        __try
        {
            SDK::UWorld* world = SDK::UWorld::GetWorld();
            if (!world)
                return;

            SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
            if (pc && pc->MyHUD)
                pc->MyHUD->RemoveAllDebugStrings();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void ClearAllStrings()
    {
        ClearAllStringsSEH();
    }

    // -----------------------------------------------------------------------
    // Float history graphs
    // -----------------------------------------------------------------------

    // The engine fills the graph body with a BatchedMeshes quad strip, which is
    // unreachable here (only DrawLines survived), so this draws the bounding
    // frame plus the sample polyline instead.
    static void DrawFloatHistoryInternal(const DFloatHistory& history, const DVec& location,
                                         const DVec& axisY, const DVec& axisZ,
                                         const DVec& drawSize, const DStyle& style)
    {
        if (!history.samples || history.count < 2)
            return;

        float minValue = history.minValue;
        float maxValue = history.maxValue;

        if (history.bAutoAdjustMinMax)
        {
            minValue = history.samples[0];
            maxValue = history.samples[0];
            for (int32_t i = 1; i < history.count; ++i)
            {
                minValue = std::min(minValue, history.samples[i]);
                maxValue = std::max(maxValue, history.samples[i]);
            }
        }

        const double range = std::max(static_cast<double>(maxValue) - minValue, 1e-4);

        const DVec stepX = VMul(axisY, drawSize.x / static_cast<double>(history.count - 1));
        const DVec stepY = VMul(axisZ, drawSize.y / range);

        LineBuf lines;
        lines.reserve(static_cast<size_t>(history.count) + 4);

        // Frame.
        const DVec bl = location;
        const DVec br = VAdd(bl, VMul(axisY, drawSize.x));
        const DVec tl = VAdd(bl, VMul(axisZ, drawSize.y));
        const DVec tr = VAdd(br, VMul(axisZ, drawSize.y));
        Emit(lines, bl, br, style);
        Emit(lines, br, tr, style);
        Emit(lines, tr, tl, style);
        Emit(lines, tl, bl, style);

        auto samplePoint = [&](int32_t index) -> DVec
        {
            const double normalized = static_cast<double>(history.samples[index]) - minValue;
            return VAdd(VAdd(location, VMul(stepX, static_cast<double>(index))),
                        VMul(stepY, normalized));
        };

        DVec prev = samplePoint(0);
        for (int32_t i = 1; i < history.count; ++i)
        {
            const DVec current = samplePoint(i);
            Emit(lines, prev, current, style);
            prev = current;
        }

        Submit(lines, style);
    }

    void DrawFloatHistoryTransform(const DFloatHistory& history, const DTransform& drawTransform,
                                   const DVec& drawSize, const DStyle& style)
    {
        DVec ax, ay, az;
        RotatorAxes(drawTransform.rotation, ax, ay, az);

        // The engine graphs along the transform's Y (width) and Z (height).
        DrawFloatHistoryInternal(history, drawTransform.location,
                                 VMul(ay, drawTransform.scale.y),
                                 VMul(az, drawTransform.scale.z),
                                 drawSize, style);
    }

    void DrawFloatHistoryLocation(const DFloatHistory& history, const DVec& drawLocation,
                                  const DVec& drawSize, const DStyle& style)
    {
        // World-axis aligned, matching the engine's location-only variant.
        DrawFloatHistoryInternal(history, drawLocation,
                                 DVec{ 0.0, 1.0, 0.0 },
                                 DVec{ 0.0, 0.0, 1.0 },
                                 drawSize, style);
    }

    // -----------------------------------------------------------------------
    // Flush
    // -----------------------------------------------------------------------

    static Flush_t g_flush          = nullptr;
    static bool    g_flushAttempted = false;

    static bool ResolveFlush()
    {
        if (g_flush)          return true;
        if (g_flushAttempted) return false;
        g_flushAttempted = true;

        const uintptr_t addr = Scanner::FindPatternInMainModule(
            "ULineBatchComponent::Flush",
            ScanPatterns::ULineBatchComponent_Flush);

        if (!addr)
        {
            ModLoaderLogger::LogWarn(
                L"[DebugDraw] [FAIL] ULineBatchComponent::Flush pattern not found "
                L"-- persistent lines cannot be cleared");
            return false;
        }

        g_flush = reinterpret_cast<Flush_t>(addr);
        return true;
    }

    static void FlushBatcherSEH(void* component)
    {
        __try
        {
            g_flush(component);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void FlushPersistentLines()
    {
        if (!ResolveFlush())
            return;

        // Same pair the engine's FLUSHPERSISTENTDEBUGLINES exec handler clears.
        if (void* world = GetBatcherSEH(BatcherWorldPersistent))
            FlushBatcherSEH(world);
        if (void* foreground = GetBatcherSEH(BatcherForegroundPersistent))
            FlushBatcherSEH(foreground);
    }

    // -----------------------------------------------------------------------
    // Test scene (debug builds only)
    // -----------------------------------------------------------------------

#ifdef _DEBUG

    static bool GetPawnLocationSEH(DVec& outLocation)
    {
        __try
        {
            SDK::UWorld* world = SDK::UWorld::GetWorld();
            if (!world)
                return false;

            SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
            if (!pc)
                return false;

            SDK::APawn* pawn = pc->K2_GetPawn();
            if (!pawn)
                return false;

            const SDK::FVector loc = pawn->K2_GetActorLocation();
            outLocation = DVec{ loc.X, loc.Y, loc.Z };
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void DrawTestScene()
    {
        DVec pawnLocation{};
        if (!GetPawnLocationSEH(pawnLocation))
        {
            ModLoaderLogger::LogWarn(
                L"[DebugDraw] Test scene: no local pawn to anchor to (are you in a world?)");
            return;
        }

        if (!Resolve())
        {
            ModLoaderLogger::LogWarn(
                L"[DebugDraw] Test scene: DrawLines is unresolved, nothing to draw");
            return;
        }

        // A row laid out along world Y, in front of the pawn along world X.
        const double kSpacing  = 260.0;
        const double kForward  = 700.0;
        const double kHeight   = 120.0;
        const int    kSlots    = 15;

        int slot = 0;
        auto nextSlot = [&]() -> DVec
        {
            const double offset = (static_cast<double>(slot) - (kSlots - 1) * 0.5) * kSpacing;
            ++slot;
            return DVec{ pawnLocation.x + kForward, pawnLocation.y + offset, pawnLocation.z + kHeight };
        };

        // Persistent rather than timed -- the Debug section has its own
        // "Clear Debug Draw" button, so there is no reason for this to vanish
        // while you are still walking around looking at it.
        DStyle style{};
        style.bPersistent = true;
        style.thickness   = 2.0f;
        style.color       = DColor{ 1.0f, 1.0f, 1.0f, 1.0f };

        const DColor labelColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        auto label = [&](const DVec& at, const wchar_t* text)
        {
            // -1 exactly: AHUD::DrawDebugTextList guards its countdown with
            // "if (-1.0 != TimeRemaining)", so unlike the line batcher (which
            // treats anything <= 0 as permanent) a 0 here would expire at once.
            DrawString(DVec{ at.x, at.y, at.z + 140.0 }, text, nullptr, labelColor, -1.0f);
        };

        // Line
        {
            const DVec p = nextSlot();
            style.color = DColor{ 1.0f, 0.2f, 0.2f, 1.0f };
            DrawLine({ p.x, p.y, p.z - 80.0 }, { p.x, p.y, p.z + 80.0 }, style);
            label(p, L"Line");
        }

        // Point
        {
            const DVec p = nextSlot();
            style.color = DColor{ 1.0f, 0.6f, 0.0f, 1.0f };
            DrawPoint(p, 80.0f, style);
            label(p, L"Point");
        }

        // Circle
        {
            const DVec p = nextSlot();
            style.color = DColor{ 1.0f, 1.0f, 0.2f, 1.0f };
            DrawCircle(p, 70.0f, 32, DVec{ 0.0, 1.0, 0.0 }, DVec{ 0.0, 0.0, 1.0 }, true, style);
            label(p, L"Circle");
        }

        // Sphere
        {
            const DVec p = nextSlot();
            style.color = DColor{ 0.2f, 1.0f, 0.2f, 1.0f };
            DrawSphere(p, 70.0f, 12, style);
            label(p, L"Sphere");
        }

        // Box (rotated, so the rotator maths is visible)
        {
            const DVec p = nextSlot();
            style.color = DColor{ 0.2f, 1.0f, 1.0f, 1.0f };
            DrawBox(p, DVec{ 60.0, 60.0, 60.0 }, DRot{ 20.0, 35.0, 10.0 }, style);
            label(p, L"Box (rotated)");
        }

        // Capsule
        {
            const DVec p = nextSlot();
            style.color = DColor{ 0.4f, 0.6f, 1.0f, 1.0f };
            DrawCapsule(p, 90.0f, 45.0f, DRot{ 0.0, 0.0, 0.0 }, style);
            label(p, L"Capsule");
        }

        // Cylinder
        {
            const DVec p = nextSlot();
            style.color = DColor{ 0.7f, 0.4f, 1.0f, 1.0f };
            DrawCylinder({ p.x, p.y, p.z - 80.0 }, { p.x, p.y, p.z + 80.0 }, 50.0f, 16, style);
            label(p, L"Cylinder");
        }

        // Cone
        {
            const DVec p = nextSlot();
            style.color = DColor{ 1.0f, 0.4f, 0.8f, 1.0f };
            DrawConeInDegrees(p, DVec{ 0.0, 0.0, 1.0 }, 160.0f, 40.0f, 25.0f, 16, style);
            label(p, L"Cone");
        }

        // Arrow
        {
            const DVec p = nextSlot();
            style.color = DColor{ 1.0f, 1.0f, 1.0f, 1.0f };
            DrawArrow({ p.x, p.y, p.z - 80.0 }, { p.x, p.y, p.z + 80.0 }, 25.0f, style);
            label(p, L"Arrow");
        }

        // Coordinate system (fixed RGB axes -- style.color is ignored)
        {
            const DVec p = nextSlot();
            DrawCoordinateSystem(p, DRot{ 0.0, 0.0, 0.0 }, 120.0f, style);
            label(p, L"CoordinateSystem");
        }

        // Plane
        {
            const DVec p = nextSlot();
            style.color = DColor{ 0.6f, 0.6f, 0.6f, 1.0f };
            DrawPlane(DPlane{ 0.0, 0.0, 1.0, p.z }, p, 90.0f, style);
            label(p, L"Plane");
        }

        // Frustum
        {
            const DVec p = nextSlot();
            style.color = DColor{ 0.2f, 0.8f, 1.0f, 1.0f };
            DTransform t{};
            t.location = p;
            t.rotation = DRot{ 0.0, 0.0, 0.0 };
            t.scale    = DVec{ 100.0, 100.0, 100.0 };
            DrawFrustum(t, style);
            label(p, L"Frustum");
        }

        // Camera
        {
            const DVec p = nextSlot();
            style.color = DColor{ 1.0f, 0.8f, 0.2f, 1.0f };
            DrawCameraAt(p, DRot{ 0.0, 180.0, 0.0 }, 90.0f, 4.0f, style);
            label(p, L"CameraAt");
        }

        // Float history -- one period of a sine wave.
        float samples[48];
        for (int i = 0; i < 48; ++i)
            samples[i] = static_cast<float>(std::sin(static_cast<double>(i) / 47.0 * 2.0 * kPi));

        DFloatHistory history{};
        history.samples           = samples;
        history.count             = 48;
        history.minValue          = -1.0f;
        history.maxValue          =  1.0f;
        history.bAutoAdjustMinMax = false;

        {
            const DVec p = nextSlot();
            style.color = DColor{ 0.2f, 1.0f, 0.6f, 1.0f };
            DrawFloatHistoryLocation(history, p, DVec{ 200.0, 120.0, 0.0 }, style);
            label(p, L"FloatHistoryLocation");
        }

        // Float history (transform form), plus a foreground line through it so
        // the depth-priority path gets exercised too.
        {
            const DVec p = nextSlot();
            style.color = DColor{ 1.0f, 0.5f, 0.5f, 1.0f };
            DTransform t{};
            t.location = p;
            t.rotation = DRot{ 0.0, 45.0, 0.0 };
            t.scale    = DVec{ 1.0, 1.0, 1.0 };
            DrawFloatHistoryTransform(history, t, DVec{ 200.0, 120.0, 0.0 }, style);
            label(p, L"FloatHistoryTransform");

            DStyle fg = style;
            fg.bForeground = true;
            fg.color       = DColor{ 1.0f, 0.0f, 1.0f, 1.0f };
            DrawLine({ p.x, p.y, p.z - 60.0 }, { p.x, p.y, p.z + 200.0 }, fg);
        }

        ModLoaderLogger::LogInfo(
            L"[DebugDraw] Test scene drawn at %.0f, %.0f, %.0f (%d primitives, persistent "
            L"-- use Clear Debug Draw to remove)",
            pawnLocation.x, pawnLocation.y, pawnLocation.z, slot);
    }

#endif // _DEBUG
}

#endif // MODLOADER_CLIENT_BUILD
