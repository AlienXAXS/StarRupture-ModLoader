#include "pch.h"
#include "engine_exec.h"

#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "utils/game_thread_dispatch.h"
#include "Engine_classes.hpp"   // UWorld, UEngine
#include "../scan_patterns.h"
#include <cstdint>

namespace Hooks::EngineExec
{
    // 'self' is NOT the UEngine pointer. UEngine derives from UObject *and*
    // FExec, and Exec is FExec's virtual, so the compiler emits the override
    // expecting the FExec sub-object -- which sits after the 0x28-byte UObject
    // header. The function body reaches back with this[-1] / (char*)this - 40
    // to get its UEngine, and reads GameViewport at this+0xBE8 (UEngine 0xC10).
    // Passing the UObject pointer instead shifts every member read by 0x28 and
    // faults somewhere inside (first seen as a bogus CreatePackage of the
    // command text followed by check(Package)). Same rule as any other
    // multiply-inherited virtual called by hand.
    using Exec_t = bool(__fastcall*)(void* self, void* world, const wchar_t* cmd, void* outputDevice);

    // sizeof(UObject): vtable, ObjectFlags, InternalIndex, ClassPrivate,
    // NamePrivate, OuterPrivate. Where FExec begins inside UEngine.
    static constexpr size_t kFExecOffsetInUEngine = 0x28;

    static Exec_t g_exec          = nullptr;
    static bool   g_scanAttempted = false;

    bool IsAvailable() { return g_exec != nullptr; }

    bool Resolve()
    {
        if (g_exec)          return true;
        if (g_scanAttempted) return false;
        g_scanAttempted = true;

        const uintptr_t addr = Scanner::FindPatternInMainModule(
            "UGameEngine::Exec", ScanPatterns::UGameEngine_Exec);

        if (!addr)
        {
            ModLoaderLogger::LogWarn(
                L"[EngineExec] [FAIL] UGameEngine::Exec pattern not found "
                L"-- the -console window cannot forward commands to the engine");
            return false;
        }

        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        g_exec = reinterpret_cast<Exec_t>(addr);
        ModLoaderLogger::LogInfo(
            L"[EngineExec] [OK] UGameEngine::Exec at 0x%llX (base+0x%llX)",
            static_cast<unsigned long long>(addr),
            static_cast<unsigned long long>(addr - base));
        return true;
    }

    // -----------------------------------------------------------------------
    // A hand-built FOutputDevice.
    //
    // The engine writes command output through the virtual Serialize on the
    // FOutputDevice& it is given, so capturing it means being one. Deriving a
    // real C++ class is the obvious way and the wrong one: MSVC lays the two
    // Serialize overloads out in *reverse* declaration order, and nothing in
    // the compiler promises to reproduce the engine's vtable for a class we
    // re-declare ourselves. The vtable is therefore spelled out by hand, in
    // the order read from FOutputDeviceDebug's vtable in the server binary:
    //
    //   0  ~FOutputDevice
    //   1  Serialize(V, Verbosity, Category, double Time)
    //   2  Serialize(V, Verbosity, Category)            <- Logf/Log land here
    //   3  SerializeRecord(const UE::FLogRecord&)
    //   4  Flush
    //   5  TearDown
    //   6  Dump(FArchive&)
    //   7  IsMemoryOnly
    //   8  CanBeUsedOnAnyThread
    //   9  CanBeUsedOnMultipleThreads
    //  10  CanBeUsedOnPanicThread
    //
    // Layout after the vtable pointer is FOutputDevice's two bools
    // (bSuppressEventTag, bAutoEmitLineTerminator); nothing in the Exec path
    // reads past them, and our own state goes after.
    // -----------------------------------------------------------------------
    struct CaptureDevice
    {
        const void* const* vtable;
        bool               bSuppressEventTag;
        bool               bAutoEmitLineTerminator;
        std::wstring*      out;
    };

    static void Append(CaptureDevice* self, const wchar_t* text)
    {
        if (!self || !self->out || !text || !*text)
            return;
        if (!self->out->empty() && self->out->back() != L'\n')
            self->out->push_back(L'\n');
        self->out->append(text);
    }

    static void __fastcall Dev_Destructor(CaptureDevice*, unsigned int) {}
    static void __fastcall Dev_SerializeTimed(CaptureDevice* self, const wchar_t* v, int /*verbosity*/,
                                              const void* /*category*/, double /*time*/) { Append(self, v); }
    static void __fastcall Dev_Serialize(CaptureDevice* self, const wchar_t* v, int /*verbosity*/,
                                         const void* /*category*/) { Append(self, v); }
    // FLogRecord carries a format string plus packed arguments; expanding it
    // needs the engine's own formatter. Commands write through Logf, which
    // never comes this way, so a record is dropped rather than mis-rendered.
    static void __fastcall Dev_SerializeRecord(CaptureDevice*, const void*) {}
    static void __fastcall Dev_Flush(CaptureDevice*) {}
    static void __fastcall Dev_TearDown(CaptureDevice*) {}
    static void __fastcall Dev_Dump(CaptureDevice*, void*) {}
    static bool __fastcall Dev_IsMemoryOnly(CaptureDevice*)               { return false; }
    static bool __fastcall Dev_CanBeUsedOnAnyThread(CaptureDevice*)       { return true; }
    static bool __fastcall Dev_CanBeUsedOnMultipleThreads(CaptureDevice*) { return false; }
    static bool __fastcall Dev_CanBeUsedOnPanicThread(CaptureDevice*)     { return false; }

    static const void* const g_captureVTable[] =
    {
        reinterpret_cast<const void*>(&Dev_Destructor),
        reinterpret_cast<const void*>(&Dev_SerializeTimed),
        reinterpret_cast<const void*>(&Dev_Serialize),
        reinterpret_cast<const void*>(&Dev_SerializeRecord),
        reinterpret_cast<const void*>(&Dev_Flush),
        reinterpret_cast<const void*>(&Dev_TearDown),
        reinterpret_cast<const void*>(&Dev_Dump),
        reinterpret_cast<const void*>(&Dev_IsMemoryOnly),
        reinterpret_cast<const void*>(&Dev_CanBeUsedOnAnyThread),
        reinterpret_cast<const void*>(&Dev_CanBeUsedOnMultipleThreads),
        reinterpret_cast<const void*>(&Dev_CanBeUsedOnPanicThread),
    };

    // Native call kept in its own SEH-guarded helper with no C++ objects in
    // scope (C2712).
    static bool CallExecSEH(void* self, void* world, const wchar_t* cmd, CaptureDevice* dev,
                            bool* handled, unsigned long* code, void** faultAddr)
    {
        __try
        {
            *handled = g_exec(self, world, cmd, dev);
            return true;
        }
        __except (*code = GetExceptionCode(),
                  *faultAddr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                  EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // "UGameEngine 'GameEngine_2147482624'" -- enough to see whether GetEngine()
    // handed back the live engine or something else entirely.
    static std::wstring DescribeObject(SDK::UObject* obj)
    {
        if (!obj)
            return L"null";
        const std::string cls  = obj->Class ? obj->Class->GetName() : std::string("?");
        const std::string name = obj->GetName();
        std::wstring out(cls.begin(), cls.end());
        out += L" '";
        out.append(name.begin(), name.end());
        out += L"'";
        return out;
    }

    Result Execute(const wchar_t* command, std::wstring& outResult)
    {
        outResult.clear();

        if (!command || !command[0])
            return Result::Unavailable;

        if (!Resolve())
            return Result::Unavailable;

        SDK::UEngine* engine = SDK::UEngine::GetEngine();
        if (!engine)
        {
            ModLoaderLogger::LogWarn(L"[EngineExec] No GEngine -- cannot execute");
            return Result::Unavailable;
        }

        // The world may legitimately be null before a map is up; most console
        // objects do not need one and the engine tolerates it.
        SDK::UWorld* world = SDK::UWorld::GetWorld();

        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        ModLoaderLogger::LogTrace(L"[EngineExec] GEngine=0x%llX (%s), world=0x%llX (%s), "
                                  L"UGameEngine::Exec=base+0x%llX, on game thread=%d",
                                  reinterpret_cast<unsigned long long>(engine), DescribeObject(engine).c_str(),
                                  reinterpret_cast<unsigned long long>(world),  DescribeObject(world).c_str(),
                                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_exec) - base),
                                  GameThreadDispatch::IsGameThread() ? 1 : 0);

        void* self = reinterpret_cast<char*>(engine) + kFExecOffsetInUEngine;

        CaptureDevice dev{};
        dev.vtable                  = g_captureVTable;
        dev.bSuppressEventTag       = true;
        dev.bAutoEmitLineTerminator = true;
        dev.out                     = &outResult;

        ModLoaderLogger::LogDebug(L"[EngineExec] Executing: %s", command);

        bool          handled   = false;
        unsigned long code      = 0;
        void*         faultAddr = nullptr;
        if (!CallExecSEH(self, world, command, &dev, &handled, &code, &faultAddr))
        {
            const auto fa = reinterpret_cast<uintptr_t>(faultAddr);
            ModLoaderLogger::LogError(L"[EngineExec] Exception 0x%08lX at 0x%llX (%s) while executing: %s",
                                      code,
                                      static_cast<unsigned long long>(fa),
                                      (fa >= base) ? L"game exe" : L"other module",
                                      command);
            if (fa >= base)
                ModLoaderLogger::LogError(L"[EngineExec]   fault at base+0x%llX",
                                          static_cast<unsigned long long>(fa - base));
            if (!outResult.empty())
                ModLoaderLogger::LogError(L"[EngineExec]   output captured before the fault: %s", outResult.c_str());
            outResult.clear();
            return Result::Unavailable;
        }

        ModLoaderLogger::LogTrace(L"[EngineExec] Returned handled=%d, %llu chars of output",
                                  handled ? 1 : 0, static_cast<unsigned long long>(outResult.size()));

        return handled ? Result::Handled : Result::Unhandled;
    }
}
