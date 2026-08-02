#include "pch.h"
#include "control_channel.h"
#include "logging/logger.h"
#include "memory_scanner/scanner.h"
#include "../scan_patterns.h"

// SDK -- only used to resolve UControlChannel's UClass* for the OpenChannels scan.
#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Hooks::ControlChannel
{
    // ---- Layout offsets (from CONTROL_CHANNEL_WIRE.md) ----------------------
    // FBitWriter / FBitReader (shared base of FOutBunch / FInBunch):
    static constexpr size_t kOff_ArchiveFlags = 0x29;  // byte; bit 0x2 = ArIsError
    static constexpr size_t kOff_BitNum       = 0xA0;  // int64: bits written / total
    static constexpr size_t kOff_ReaderPos    = 0xA8;  // int64: reader bit cursor
    static constexpr uint8_t kErrorBit        = 0x02;
    // UChannel:
    static constexpr size_t kOff_ChannelConnection = 0x28; // UNetConnection*
    // UNetConnection::OpenChannels TArray<UChannel*>:
    static constexpr size_t kOff_OpenChannelsData = 0x70;  // UChannel**
    static constexpr size_t kOff_OpenChannelsNum  = 0x78;  // int32
    // UObject::ClassPrivate:
    static constexpr size_t kOff_UObjectClass     = 0x10;  // UClass*

    // Bunch scratch size -- generously larger than sizeof(FOutBunch) (~0x130).
    static constexpr size_t kBunchScratchSize = 0x400;

    // ---- Native function typedefs ------------------------------------------
    using ReceivedBunch_t        = void  (__fastcall*)(void* thisCC, void* bunch);
    using CtrlSendBunch_t        = void* (__fastcall*)(void* thisCC, void* retIdRange, void* bunch, bool bMerge);
    using CtorControlBunch_t     = void  (__fastcall*)(void* thisBunch, void* channel, bool bClose);
    using OutBunchDtor_t         = void  (__fastcall*)(void* thisBunch);
    using BitWriterSerialize_t   = void  (__fastcall*)(void* thisWriter, void* src, int64_t numBytes);
    using BitReaderSerialize_t   = void  (__fastcall*)(void* thisReader, void* dest, int64_t numBits);

    // ---- Resolved state -----------------------------------------------------
    static Hooks::Hook          g_hook;
    static ReceivedBunch_t      g_original            = nullptr;
    static CtrlSendBunch_t      g_ctrlSendBunch       = nullptr;
    static CtorControlBunch_t   g_ctorBunch           = nullptr;
    static OutBunchDtor_t       g_outBunchDtor        = nullptr;
    static BitWriterSerialize_t g_bitWriterSerialize  = nullptr;
    static BitReaderSerialize_t g_bitReaderSerialize  = nullptr; // SerializeBits(numBits)
    static void*                g_controlChannelClass = nullptr; // SDK::UControlChannel UClass*

    static RawReceiveCallback   g_recvCb              = nullptr;

    // Captured UNetConnection* -> UControlChannel* (populated by the detour).
    static std::mutex                            g_mapMutex;
    static std::unordered_map<void*, void*>      g_ccByConn;

    static bool AllSendNativesResolved()
    {
        return g_ctrlSendBunch && g_ctorBunch && g_outBunchDtor &&
               g_bitWriterSerialize && g_bitReaderSerialize;
    }

    // ---- SEH helpers (no C++ objects -- MSVC C2712) -------------------------

    // Peek the leading message byte. If it is ours, leave the reader advanced past
    // the byte and report the remaining payload size; otherwise rewind and return
    // false so the original ReceivedBunch sees an untouched reader.
    static bool PeekWireTypeSEH(void* bunch, size_t* outPayloadBytes)
    {
        __try
        {
            const uint8_t flags = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(bunch) + kOff_ArchiveFlags);
            const int64_t num    = *reinterpret_cast<int64_t*>(reinterpret_cast<char*>(bunch) + kOff_BitNum);
            const int64_t pos0   = *reinterpret_cast<int64_t*>(reinterpret_cast<char*>(bunch) + kOff_ReaderPos);
            if ((flags & kErrorBit) != 0) return false;
            if (num - pos0 < 8) return false;

            uint8_t type = 0;
            g_bitReaderSerialize(bunch, &type, 8); // reads 8 bits, advances Pos
            if (type != kWireMsgType)
            {
                *reinterpret_cast<int64_t*>(reinterpret_cast<char*>(bunch) + kOff_ReaderPos) = pos0; // rewind
                return false;
            }
            const int64_t posNow = *reinterpret_cast<int64_t*>(reinterpret_cast<char*>(bunch) + kOff_ReaderPos);
            *outPayloadBytes = static_cast<size_t>((num - posNow) >> 3);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ReadBytesSEH(void* bunch, uint8_t* dst, size_t n)
    {
        __try
        {
            if (n) g_bitReaderSerialize(bunch, dst, static_cast<int64_t>(n) * 8);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Construct a control bunch on a caller-provided scratch buffer, write
    // (typeByte + payload), send it, then destruct the bunch. Returns false on
    // overflow or fault. No C++ objects live here.
    static bool ConstructWriteSendSEH(void* cc, const uint8_t* data, size_t len)
    {
        __try
        {
            alignas(16) uint8_t bunch[kBunchScratchSize];
            memset(bunch, 0, sizeof(bunch));

            g_ctorBunch(bunch, cc, /*bClose*/ false);

            uint8_t type = kWireMsgType;
            g_bitWriterSerialize(bunch, &type, 1);
            if (len) g_bitWriterSerialize(bunch, const_cast<uint8_t*>(data), static_cast<int64_t>(len));

            const bool overflow =
                (*reinterpret_cast<uint8_t*>(bunch + kOff_ArchiveFlags) & kErrorBit) != 0;

            if (!overflow)
            {
                uint64_t idRange = 0;
                g_ctrlSendBunch(cc, &idRange, bunch, /*bMerge*/ false);
            }

            g_outBunchDtor(bunch);
            return !overflow;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Scan a connection's OpenChannels for the (exact-class) UControlChannel.
    static void* ResolveControlChannelClassSEH()
    {
        __try { return SDK::UControlChannel::StaticClass(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    static void* ScanControlChannelSEH(void* netConnection)
    {
        __try
        {
            uint8_t* conn = reinterpret_cast<uint8_t*>(netConnection);
            void** data = *reinterpret_cast<void***>(conn + kOff_OpenChannelsData);
            int32_t num = *reinterpret_cast<int32_t*>(conn + kOff_OpenChannelsNum);
            if (!data || num <= 0 || num > 4096) return nullptr;
            for (int32_t i = 0; i < num; ++i)
            {
                void* chan = data[i];
                if (!chan) continue;
                void* cls = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(chan) + kOff_UObjectClass);
                if (cls == g_controlChannelClass)
                    return chan;
            }
            return nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    static void* ReadConnectionSEH(void* thisCC)
    {
        __try { return *reinterpret_cast<void**>(reinterpret_cast<char*>(thisCC) + kOff_ChannelConnection); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }

    // ---- Detour -------------------------------------------------------------
    static void __fastcall Detour(void* thisCC, void* bunch)
    {
        // Capture the connection -> control-channel mapping for later sends.
        void* conn = thisCC ? ReadConnectionSEH(thisCC) : nullptr;
        if (conn)
        {
            std::lock_guard<std::mutex> lk(g_mapMutex);
            g_ccByConn[conn] = thisCC;
        }

        size_t payloadBytes = 0;
        if (bunch && g_bitReaderSerialize && PeekWireTypeSEH(bunch, &payloadBytes))
        {
            // Ours -- consume it and never call the original.
            std::vector<uint8_t> buf(payloadBytes);
            const bool ok = (payloadBytes == 0) ? true : ReadBytesSEH(bunch, buf.data(), payloadBytes);
            if (ok && g_recvCb)
            {
                try { g_recvCb(conn, buf.data(), buf.size()); }
                catch (...) { ModLoaderLogger::LogError(L"[ControlChannel] Exception in receive callback"); }
            }
            return;
        }

        if (g_original) g_original(thisCC, bunch);
    }

    // ---- Public API ---------------------------------------------------------
    bool Install()
    {
        if (g_hook.installed) return true;

        ModLoaderLogger::LogInfo(L"[ControlChannel] Resolving native functions...");

        uintptr_t recvAddr = Scanner::FindPatternInMainModule(
            "UControlChannel::ReceivedBunch", ScanPatterns::UControlChannel_ReceivedBunch);

        g_ctrlSendBunch = reinterpret_cast<CtrlSendBunch_t>(Scanner::FindPatternInMainModule(
            "UControlChannel::SendBunch", ScanPatterns::UControlChannel_SendBunch));
        g_ctorBunch = reinterpret_cast<CtorControlBunch_t>(Scanner::FindPatternInMainModule(
            "FControlChannelOutBunch::ctor", ScanPatterns::FControlChannelOutBunch_ctor));
        g_bitWriterSerialize = reinterpret_cast<BitWriterSerialize_t>(Scanner::FindPatternInMainModule(
            "FBitWriter::Serialize", ScanPatterns::FBitWriter_Serialize));
        g_bitReaderSerialize = reinterpret_cast<BitReaderSerialize_t>(Scanner::FindPatternInMainModule(
            "FBitReader::SerializeBits", ScanPatterns::FBitReader_SerializeBits));

        // FOutBunch::~FOutBunch is matched directly at its entry point. (Earlier
        // notes resolved it through a call site because a unique entry signature
        // had not been found; one has, so there is no rel32 indirection here.)
        g_outBunchDtor = reinterpret_cast<OutBunchDtor_t>(Scanner::FindPatternInMainModule(
            "FOutBunch::~FOutBunch", ScanPatterns::FOutBunch_dtor));

        // Optional -- resolve the UControlChannel UClass* for the send-side scan.
        g_controlChannelClass = ResolveControlChannelClassSEH();

        if (!recvAddr)
        {
            ModLoaderLogger::LogError(L"[ControlChannel] ReceivedBunch pattern not found -- wire disabled");
            return false;
        }

        const bool hooked = g_hook.Install(
            recvAddr, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_original));
        if (!hooked)
        {
            ModLoaderLogger::LogError(L"[ControlChannel] Failed to install ReceivedBunch detour");
            return false;
        }

        ModLoaderLogger::LogInfo(
            L"[ControlChannel] Installed. send=%d ctor=%d dtor=%d bw=%d br=%d cls=%d",
            g_ctrlSendBunch != nullptr, g_ctorBunch != nullptr, g_outBunchDtor != nullptr,
            g_bitWriterSerialize != nullptr, g_bitReaderSerialize != nullptr,
            g_controlChannelClass != nullptr);

        if (!AllSendNativesResolved())
            ModLoaderLogger::LogError(
                L"[ControlChannel] One or more send-side natives missing -- receive only. "
                L"There is no fallback transport, so plugin sends will be dropped. "
                L"Preflight should have caught this; check the pattern warnings above.");

        return true;
    }

    void Remove()
    {
        g_hook.Remove();
        {
            std::lock_guard<std::mutex> lk(g_mapMutex);
            g_ccByConn.clear();
        }
        g_recvCb = nullptr;
    }

    bool IsInstalled() { return g_hook.installed; }

    bool IsAvailable() { return g_hook.installed && AllSendNativesResolved(); }

    void SetReceiveCallback(RawReceiveCallback cb) { g_recvCb = cb; }

    bool SendRaw(void* controlChannel, const uint8_t* data, size_t len)
    {
        if (!controlChannel || !AllSendNativesResolved()) return false;
        if (!data && len) return false;
        return ConstructWriteSendSEH(controlChannel, data, len);
    }

    void* GetControlChannel(void* netConnection)
    {
        if (!netConnection) return nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mapMutex);
            auto it = g_ccByConn.find(netConnection);
            if (it != g_ccByConn.end()) return it->second;
        }
        if (!g_controlChannelClass) return nullptr;
        void* cc = ScanControlChannelSEH(netConnection);
        if (cc)
        {
            std::lock_guard<std::mutex> lk(g_mapMutex);
            g_ccByConn[netConnection] = cc;
        }
        return cc;
    }

    void ForgetConnection(void* netConnection)
    {
        if (!netConnection) return;
        std::lock_guard<std::mutex> lk(g_mapMutex);
        g_ccByConn.erase(netConnection);
    }

    void ForgetAllConnections()
    {
        std::lock_guard<std::mutex> lk(g_mapMutex);
        if (g_ccByConn.empty()) return;
        ModLoaderLogger::LogDebug(
            L"[ControlChannel] Dropping %zu cached control channel(s)", g_ccByConn.size());
        g_ccByConn.clear();
    }
}
