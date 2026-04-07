#include "pch.h"
#include "network_channel.h"
#include "logging/logger.h"

#if defined(MODLOADER_SERVER_BUILD) || defined(MODLOADER_CLIENT_BUILD)

// SDK headers -- paths resolved via $(StarRuptureSDKConfigDir) in Shared.props.
#include "CoreUObject_classes.hpp"  // UObject, UClass, UFunction, GObjects
#include "Engine_classes.hpp"       // UWorld, UGameplayStatics, AActor
#include "Chimera_classes.hpp"      // ACrPlayerControllerBase

#ifdef MODLOADER_CLIENT_BUILD
#include "hooks/game/client_message/client_message.h"
#endif

#ifdef MODLOADER_SERVER_BUILD
#include "hooks/game/server_chat_commit/server_chat_commit.h"
#endif

#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cstdint>

// ============================================================
// Base64 helpers (no external dependency)
// ============================================================

namespace
{
    static constexpr const char* kB64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string Base64Encode(const uint8_t* data, size_t size)
    {
        std::string out;
        out.reserve(((size + 2) / 3) * 4);
        uint32_t val = 0;
        int valBits = -6;
        for (size_t i = 0; i < size; ++i)
        {
            val = (val << 8) | data[i];
            valBits += 8;
            while (valBits >= 0)
            {
                out.push_back(kB64Chars[(val >> valBits) & 0x3F]);
                valBits -= 6;
            }
        }
        if (valBits > -6)
            out.push_back(kB64Chars[((val << 8) >> (valBits + 8)) & 0x3F]);
        while (out.size() % 4)
            out.push_back('=');
        return out;
    }

    std::vector<uint8_t> Base64Decode(const char* data, size_t size)
    {
        static const int8_t kLut[256] = {
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
            52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
            -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
            15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
            -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
            41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        };
        std::vector<uint8_t> out;
        out.reserve((size / 4) * 3);
        uint32_t val = 0;
        int valBits = -8;
        for (size_t i = 0; i < size; ++i)
        {
            if (data[i] == '=') break;
            int8_t c = kLut[static_cast<uint8_t>(data[i])];
            if (c < 0) continue;
            val = (val << 6) | static_cast<uint8_t>(c);
            valBits += 6;
            if (valBits >= 0)
            {
                out.push_back(static_cast<uint8_t>((val >> valBits) & 0xFF));
                valBits -= 8;
            }
        }
        return out;
    }

    // ============================================================
    // Envelope helpers
    // Wire format (ASCII inside FString): [MOD:pluginName:typeTag:size:]base64payload
    //   pluginName : plugin's self-reported name (must not contain ':')
    //   typeTag    : packet type string (must not contain ':')
    //   size       : decimal byte count of the decoded payload (validation guard)
    //   base64payload : standard base64, no line breaks
    // ============================================================

    static constexpr const char* kEnvPrefix = "[MOD:";
    static constexpr size_t kEnvPrefixLen   = 5; // strlen("[MOD:")

    // Build the ASCII envelope string.
    std::string BuildEnvelope(const char* pluginName, const char* typeTag,
                              const uint8_t* data, size_t size)
    {
        std::string b64 = Base64Encode(data, size);
        std::string env;
        env.reserve(kEnvPrefixLen + strlen(pluginName) + 1 + strlen(typeTag) + 1 + 10 + 2 + b64.size());
        env  = kEnvPrefix;
        env += pluginName;
        env += ':';
        env += typeTag;
        env += ':';
        char szSize[24];
        _snprintf_s(szSize, sizeof(szSize), _TRUNCATE, "%zu", size);
        env += szSize;
        env += ":]";
        env += b64;
        return env;
    }

    struct ParsedEnvelope
    {
        std::string pluginName;
        std::string typeTag;
        std::vector<uint8_t> payload;
        bool valid = false;
    };

    // Parse from a narrow-string view (converted from FString.Data on the receive path).
    ParsedEnvelope ParseEnvelope(const char* str, size_t len)
    {
        ParsedEnvelope result;
        if (len < kEnvPrefixLen + 4) return result;
        if (memcmp(str, kEnvPrefix, kEnvPrefixLen) != 0) return result;

        const char* p = str + kEnvPrefixLen;
        const char* end = str + len;

        // pluginName
        const char* colon1 = static_cast<const char*>(memchr(p, ':', end - p));
        if (!colon1) return result;
        result.pluginName.assign(p, colon1);

        // typeTag
        p = colon1 + 1;
        const char* colon2 = static_cast<const char*>(memchr(p, ':', end - p));
        if (!colon2) return result;
        result.typeTag.assign(p, colon2);

        // size
        p = colon2 + 1;
        const char* colonBracket = static_cast<const char*>(memchr(p, ':', end - p));
        if (!colonBracket) return result;
        size_t declaredSize = static_cast<size_t>(atoi(p));

        // check for ":]"
        p = colonBracket + 1;
        if (p >= end || *p != ']') return result;
        p++; // skip ']'

        // base64 payload
        result.payload = Base64Decode(p, static_cast<size_t>(end - p));
        if (result.payload.size() != declaredSize) return result;

        result.valid = true;
        return result;
    }

    // Shared handler key format: "pluginName\x01typeTag"
    static std::string MakeHandlerKey(const char* pluginName, const char* typeTag)
    {
        std::string key(pluginName);
        key += '\x01';
        key += typeTag;
        return key;
    }

    // SEH wrapper for ProcessEvent.
    // Must contain NO C++ objects with destructors (MSVC C2712 restriction).
    // The caller is responsible for adjusting UFunction flags before calling and
    // restoring them afterwards.
    static void CallProcessEventSEH(SDK::UObject* obj, SDK::UFunction* func, void* parms)
    {
        __try
        {
            obj->ProcessEvent(func, parms);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] SEH exception in ProcessEvent (code 0x%08lX)",
                GetExceptionCode());
        }
    }

} // anonymous namespace

// ============================================================
// Internal state
// ============================================================

namespace
{

#ifdef MODLOADER_SERVER_BUILD

    // Cached ClientSaveStringToTxt UFunction pointer.
    // Resolved lazily on first send via UObject::FindObjectFast<UFunction>.
    static SDK::UFunction* g_clientSaveTxtFunc = nullptr;

    // Resolve the ClientSaveStringToTxt UFunction from GObjects if not already cached.
    static void EnsureClientSaveTxtFunc()
    {
        if (g_clientSaveTxtFunc) return;

        g_clientSaveTxtFunc = SDK::UObject::FindObjectFast<SDK::UFunction>(
            "ClientSaveStringToTxt", SDK::EClassCastFlags::Function);

        if (g_clientSaveTxtFunc)
            ModLoaderLogger::LogInfo(L"[NetworkChannel] ClientSaveStringToTxt UFunction resolved at %p",
                                     static_cast<void*>(g_clientSaveTxtFunc));
        else
            ModLoaderLogger::LogWarn(L"[NetworkChannel] ClientSaveStringToTxt UFunction not found in GObjects yet");
    }

    // Params layout for ACrPlayerControllerBase::ClientSaveStringToTxt (0x20 bytes).
    // Matches CrPlayerControllerBase_ClientSaveStringToTxt in Chimera_parameters.hpp.
    // We use InString for the mod envelope and Path as a sentinel ("MOD_NET").
    struct alignas(8) ClientSaveTxtParms
    {
        // FString InString (0x0000, 0x0010)
        wchar_t* inStrData; // +0x00
        int32_t  inStrNum;  // +0x08  (includes null terminator)
        int32_t  inStrMax;  // +0x0C
        // FString Path (0x0010, 0x0010)
        wchar_t* pathData;  // +0x10
        int32_t  pathNum;   // +0x18  (includes null terminator)
        int32_t  pathMax;   // +0x1C
    };
    static_assert(sizeof(ClientSaveTxtParms) == 0x20, "ClientSaveTxtParms size mismatch");

    // Sentinel written into Path to tag mod traffic.  Must match client_message.cpp.
    static const wchar_t* kPathSentinel = L"MOD_NET";
    static const int      kPathSentinelLen = 8; // 7 chars + null terminator

    // Send the narrow-string envelope to one player controller via ProcessEvent.
    // FUNC_Native is left as-is: for a FUNC_NetClient function on a dedicated server
    // UE routes it to the client's NetConnection regardless of the Native flag.
    static void SendEnvelopeToPlayer(void* playerController, const std::string& envelope)
    {
        EnsureClientSaveTxtFunc();
        if (!g_clientSaveTxtFunc)
        {
            ModLoaderLogger::LogWarn(L"[NetworkChannel] SendEnvelope: ClientSaveStringToTxt UFunction not yet in GObjects");
            return;
        }

        ModLoaderLogger::LogDebug(L"[NetworkChannel] SendEnvelope: PC=%p func=%p flags=0x%08X envelope=%zu bytes",
                                  playerController, static_cast<void*>(g_clientSaveTxtFunc),
                                  g_clientSaveTxtFunc->FunctionFlags, envelope.size());

        // Convert narrow envelope to wide for FString InString
        int wlen = MultiByteToWideChar(CP_UTF8, 0, envelope.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return;
        std::wstring wideEnv(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, envelope.c_str(), -1, wideEnv.data(), wlen);

        ClientSaveTxtParms parms{};
        parms.inStrData = wideEnv.data();
        parms.inStrNum  = wlen;                  // includes null terminator
        parms.inStrMax  = wlen;
        parms.pathData  = const_cast<wchar_t*>(kPathSentinel);
        parms.pathNum   = kPathSentinelLen;
        parms.pathMax   = kPathSentinelLen;

        auto* obj = reinterpret_cast<SDK::UObject*>(playerController);
        CallProcessEventSEH(obj, g_clientSaveTxtFunc, &parms);

        ModLoaderLogger::LogDebug(L"[NetworkChannel] SendEnvelope: ProcessEvent returned");
    }

    // Server-side handler registry for Client->Server messages
    static std::mutex g_serverMutex;
    static std::unordered_map<std::string, std::vector<PluginNetworkServerMessageCallback>> g_serverHandlers;

    // IPluginNetworkChannel function implementations -- server build

    static bool NC_IsServer() { return true; }

    static void NC_SendPacketToClient(void* playerController, const char* pluginName,
                                      const char* typeTag, const uint8_t* data, size_t size)
    {
        if (!playerController || !pluginName || !typeTag || !data || size == 0) return;

        if (size > 1400)
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Payload %zu bytes exceeds 1400-byte recommended limit for plugin '%S'",
                size, pluginName);
        }

        std::string env = BuildEnvelope(pluginName, typeTag, data, size);
        SendEnvelopeToPlayer(playerController, env);
    }

    static void NC_SendPacketToAllPlayers(const char* pluginName, const char* typeTag,
                                          const uint8_t* data, size_t size)
    {
        if (!pluginName || !typeTag || !data || size == 0) return;

        if (size > 1400)
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] Payload %zu bytes exceeds 1400-byte recommended limit for plugin '%S'",
                size, pluginName);
        }

        std::string env = BuildEnvelope(pluginName, typeTag, data, size);

        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world)
        {
            ModLoaderLogger::LogWarn(L"[NetworkChannel] SendPacketToAllPlayers: UWorld not available");
            return;
        }

        SDK::TArray<SDK::AActor*> actors;
        SDK::UGameplayStatics::GetAllActorsOfClass(
            world,
            SDK::ACrPlayerControllerBase::StaticClass(),
            &actors);

        for (int32_t i = 0; i < actors.Num(); ++i)
            SendEnvelopeToPlayer(actors[i], env);
    }

    static void NC_RegisterMessageHandler(const char*, const char*, PluginNetworkMessageCallback)
    {
        // No-op on server -- server only sends to clients, never receives ClientMessage
    }

    static void NC_UnregisterMessageHandler(const char*, const char*, PluginNetworkMessageCallback)
    {
        // No-op on server
    }

    // Client->Server: no-op on server side (server only receives)
    static void NC_SendPacketToServer(const char*, const char*, const uint8_t*, size_t)
    {
        // No-op on server
    }

    static void NC_RegisterServerMessageHandler(const char* pluginName, const char* typeTag,
                                                PluginNetworkServerMessageCallback callback)
    {
        if (!pluginName || !typeTag || !callback) return;
        std::string key = MakeHandlerKey(pluginName, typeTag);
        {
            std::lock_guard<std::mutex> lk(g_serverMutex);
            g_serverHandlers[key].push_back(callback);
        }
        ModLoaderLogger::LogDebug(
            L"[NetworkChannel] Server handler registered for plugin='%S' tag='%S'",
            pluginName, typeTag);
    }

    static void NC_UnregisterServerMessageHandler(const char* pluginName, const char* typeTag,
                                                  PluginNetworkServerMessageCallback callback)
    {
        if (!pluginName || !typeTag || !callback) return;
        std::string key = MakeHandlerKey(pluginName, typeTag);
        std::lock_guard<std::mutex> lk(g_serverMutex);
        auto it = g_serverHandlers.find(key);
        if (it != g_serverHandlers.end())
        {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), callback), vec.end());
            if (vec.empty())
                g_serverHandlers.erase(it);
        }
    }

#endif // MODLOADER_SERVER_BUILD

#ifdef MODLOADER_CLIENT_BUILD

    static std::mutex g_mutex;

    // Handler registry for Server->Client messages: key = "pluginName\x01typeTag"
    static std::unordered_map<std::string, std::vector<PluginNetworkMessageCallback>> g_handlers;

    // Cached ServerExecuteConsoleCommand UFunction pointer -- resolved lazily for client->server sends.
    static SDK::UFunction* g_serverExecCmdFunc = nullptr;

    static void EnsureServerExecCmdFunc()
    {
        if (g_serverExecCmdFunc) return;

        g_serverExecCmdFunc = SDK::UObject::FindObjectFast<SDK::UFunction>(
            "ServerExecuteConsoleCommand", SDK::EClassCastFlags::Function);

        if (g_serverExecCmdFunc)
            ModLoaderLogger::LogInfo(L"[NetworkChannel] ServerExecuteConsoleCommand UFunction resolved at %p",
                                     static_cast<void*>(g_serverExecCmdFunc));
        else
            ModLoaderLogger::LogWarn(L"[NetworkChannel] ServerExecuteConsoleCommand UFunction not found in GObjects yet");
    }

    // Params layout for ACrPlayerControllerBase::ServerExecuteConsoleCommand (16 bytes).
    // Matches CrPlayerControllerBase_ServerExecuteConsoleCommand in Chimera_parameters.hpp.
    struct alignas(8) ServerExecCmdParms
    {
        // FString Command (0x0000, 0x0010): wchar_t* Data, int32 Num, int32 Max
        wchar_t* strData;  // +0x00
        int32_t  strNum;   // +0x08  (includes null terminator)
        int32_t  strMax;   // +0x0C
    };
    static_assert(sizeof(ServerExecCmdParms) == 0x10, "ServerExecCmdParms size mismatch");

    static bool NC_IsServer() { return false; }

    static void NC_SendPacketToClient(void*, const char*, const char*, const uint8_t*, size_t)
    {
        // No-op on client -- client does not send to players
    }

    static void NC_SendPacketToAllPlayers(const char*, const char*, const uint8_t*, size_t)
    {
        // No-op on client
    }

    static void NC_RegisterMessageHandler(const char* pluginName, const char* typeTag,
                                          PluginNetworkMessageCallback callback)
    {
        if (!pluginName || !typeTag || !callback) return;
        std::string key = MakeHandlerKey(pluginName, typeTag);
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_handlers[key].push_back(callback);
        }
        ModLoaderLogger::LogDebug(L"[NetworkChannel] Handler registered for plugin='%S' tag='%S'",
                                  pluginName, typeTag);
        // Install the ProcessEvent hook lazily on first registration
        if (!Hooks::ClientMessage::IsInstalled())
            Hooks::ClientMessage::Install();
    }

    static void NC_UnregisterMessageHandler(const char* pluginName, const char* typeTag,
                                            PluginNetworkMessageCallback callback)
    {
        if (!pluginName || !typeTag || !callback) return;
        std::string key = MakeHandlerKey(pluginName, typeTag);
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_handlers.find(key);
        if (it != g_handlers.end())
        {
            auto& vec = it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), callback), vec.end());
            if (vec.empty())
                g_handlers.erase(it);
        }
    }

    // Client->Server send: encode envelope and call ServerExecuteConsoleCommand via ProcessEvent.
    // FUNC_Native is left as-is: for a FUNC_NetServer function on the client UE routes it
    // to the server's NetConnection regardless of the Native flag.
    static void NC_SendPacketToServer(const char* pluginName, const char* typeTag,
                                      const uint8_t* data, size_t size)
    {
        if (!pluginName || !typeTag || !data || size == 0) return;

        if (size > 1400)
        {
            ModLoaderLogger::LogWarn(
                L"[NetworkChannel] SendPacketToServer: payload %zu bytes exceeds 1400-byte limit for plugin '%S'",
                size, pluginName);
        }

        EnsureServerExecCmdFunc();
        if (!g_serverExecCmdFunc)
        {
            ModLoaderLogger::LogWarn(L"[NetworkChannel] SendPacketToServer: ServerExecuteConsoleCommand UFunction not yet in GObjects");
            return;
        }

        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world)
        {
            ModLoaderLogger::LogWarn(L"[NetworkChannel] SendPacketToServer: UWorld not available");
            return;
        }

        void* localPC = SDK::UGameplayStatics::GetPlayerController(world, 0);
        if (!localPC)
        {
            ModLoaderLogger::LogWarn(L"[NetworkChannel] SendPacketToServer: local PlayerController not found");
            return;
        }

        std::string env = BuildEnvelope(pluginName, typeTag, data, size);

        // Convert to wide for FString
        int wlen = MultiByteToWideChar(CP_UTF8, 0, env.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return;
        std::wstring wide(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, env.c_str(), -1, wide.data(), wlen);

        ServerExecCmdParms parms{};
        parms.strData = wide.data();
        parms.strNum  = wlen; // includes null terminator
        parms.strMax  = wlen;

        auto* obj = reinterpret_cast<SDK::UObject*>(localPC);
        CallProcessEventSEH(obj, g_serverExecCmdFunc, &parms);
    }

    // Client->Server receive handlers are server-only; these are no-ops on client
    static void NC_RegisterServerMessageHandler(const char*, const char*,
                                                PluginNetworkServerMessageCallback)
    {
        // No-op on client
    }

    static void NC_UnregisterServerMessageHandler(const char*, const char*,
                                                  PluginNetworkServerMessageCallback)
    {
        // No-op on client
    }

#endif // MODLOADER_CLIENT_BUILD

    // Static IPluginNetworkChannel instance (shared between server and client)
    static IPluginNetworkChannel g_networkIface =
    {
        NC_IsServer,
        NC_SendPacketToClient,
        NC_SendPacketToAllPlayers,
        NC_RegisterMessageHandler,
        NC_UnregisterMessageHandler,
        NC_SendPacketToServer,
        NC_RegisterServerMessageHandler,
        NC_UnregisterServerMessageHandler,
    };

} // anonymous namespace

// ============================================================
// DispatchClientMessage  (client build only)
// Called from client_message.cpp ProcessEvent hook.
// ============================================================

#ifdef MODLOADER_CLIENT_BUILD
void NetworkChannel::DispatchClientMessage(const wchar_t* str, int numCharsWithNull)
{
    if (!str || numCharsWithNull <= static_cast<int>(kEnvPrefixLen)) return;

    // Convert wide string to narrow for envelope parsing
    int nbytes = WideCharToMultiByte(CP_UTF8, 0, str, numCharsWithNull - 1, nullptr, 0, nullptr, nullptr);
    if (nbytes <= 0) return;

    std::string narrow(static_cast<size_t>(nbytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, str, numCharsWithNull - 1, narrow.data(), nbytes, nullptr, nullptr);

    ParsedEnvelope env = ParseEnvelope(narrow.c_str(), static_cast<size_t>(nbytes));
    if (!env.valid) return;

    // Dispatch to registered handlers
    std::string key = MakeHandlerKey(env.pluginName.c_str(), env.typeTag.c_str());

    std::vector<PluginNetworkMessageCallback> callbacks;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_handlers.find(key);
        if (it == g_handlers.end()) return;
        callbacks = it->second; // copy snapshot
    }

    for (auto* cb : callbacks)
    {
        if (!cb) continue;
        try
        {
            cb(env.pluginName.c_str(), env.typeTag.c_str(),
               env.payload.data(), env.payload.size());
        }
        catch (const std::exception& ex)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] Exception in handler for plugin='%S' tag='%S': %S",
                env.pluginName.c_str(), env.typeTag.c_str(), ex.what());
        }
        catch (...)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] Unknown exception in handler for plugin='%S' tag='%S'",
                env.pluginName.c_str(), env.typeTag.c_str());
        }
    }
}
#endif // MODLOADER_CLIENT_BUILD

// ============================================================
// DispatchServerMessage  (server build only)
// Called from server_chat_commit.cpp ProcessEvent hook.
// Returns true if the message was a mod envelope (consumed); false for normal chat.
// ============================================================

#ifdef MODLOADER_SERVER_BUILD
bool NetworkChannel::DispatchServerMessage(void* senderUObject, const wchar_t* str, int numCharsWithNull)
{
    if (!str || numCharsWithNull <= static_cast<int>(kEnvPrefixLen)) return false;

    // Convert wide string to narrow for envelope parsing
    int nbytes = WideCharToMultiByte(CP_UTF8, 0, str, numCharsWithNull - 1, nullptr, 0, nullptr, nullptr);
    if (nbytes <= 0) return false;

    std::string narrow(static_cast<size_t>(nbytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, str, numCharsWithNull - 1, narrow.data(), nbytes, nullptr, nullptr);

    ParsedEnvelope env = ParseEnvelope(narrow.c_str(), static_cast<size_t>(nbytes));
    if (!env.valid) return false;

    // Dispatch to registered server-side handlers
    std::string key = MakeHandlerKey(env.pluginName.c_str(), env.typeTag.c_str());

    std::vector<PluginNetworkServerMessageCallback> callbacks;
    {
        std::lock_guard<std::mutex> lk(g_serverMutex);
        auto it = g_serverHandlers.find(key);
        if (it == g_serverHandlers.end()) return true; // valid envelope but no handler -- still consumed
        callbacks = it->second; // copy snapshot
    }

    for (auto* cb : callbacks)
    {
        if (!cb) continue;
        try
        {
            cb(senderUObject,
               env.pluginName.c_str(), env.typeTag.c_str(),
               env.payload.data(), env.payload.size());
        }
        catch (const std::exception& ex)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] Exception in server handler for plugin='%S' tag='%S': %S",
                env.pluginName.c_str(), env.typeTag.c_str(), ex.what());
        }
        catch (...)
        {
            ModLoaderLogger::LogError(
                L"[NetworkChannel] Unknown exception in server handler for plugin='%S' tag='%S'",
                env.pluginName.c_str(), env.typeTag.c_str());
        }
    }

    return true; // mod envelope consumed -- caller should suppress original chat
}
#endif // MODLOADER_SERVER_BUILD

// ============================================================
// Public API
// ============================================================

IPluginNetworkChannel* NetworkChannel::GetInterface()
{
    return &g_networkIface;
}

void NetworkChannel::Initialize()
{
#ifdef MODLOADER_SERVER_BUILD
    ModLoaderLogger::LogDebug(L"[NetworkChannel] Initialize: installing ProcessEvent hook...");
    if (!Hooks::ServerChatCommit::IsInstalled())
        Hooks::ServerChatCommit::Install();
    ModLoaderLogger::LogInfo(L"[NetworkChannel] Server network channel initialized");
#endif

#ifdef MODLOADER_CLIENT_BUILD
    ModLoaderLogger::LogInfo(L"[NetworkChannel] Client network channel initialized");
    // The ProcessEvent hook is installed lazily by client_message.cpp when the
    // first handler is registered.  No action needed here.
#endif
}

void NetworkChannel::Shutdown()
{
#ifdef MODLOADER_SERVER_BUILD
    Hooks::ServerChatCommit::Remove();
    {
        std::lock_guard<std::mutex> lk(g_serverMutex);
        g_serverHandlers.clear();
    }
    g_clientSaveTxtFunc = nullptr;
    ModLoaderLogger::LogInfo(L"[NetworkChannel] Server network channel shut down");
#endif

#ifdef MODLOADER_CLIENT_BUILD
    Hooks::ClientMessage::Remove();
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_handlers.clear();
    }
    g_serverExecCmdFunc = nullptr;
    ModLoaderLogger::LogInfo(L"[NetworkChannel] Client network channel shut down");
#endif
}

#else // Generic (plain Debug/Release) build

IPluginNetworkChannel* NetworkChannel::GetInterface() { return nullptr; }
void NetworkChannel::Initialize() {}
void NetworkChannel::Shutdown() {}

#endif // MODLOADER_SERVER_BUILD || MODLOADER_CLIENT_BUILD
