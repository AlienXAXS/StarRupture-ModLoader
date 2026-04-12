#include "http_connection.h"
#include "plugin_helpers.h"
#include "plugin_config.h"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// FHttpConnection::ProcessRequest — Remote-Object-Call vulnerability filter
//
// This hook is now owned by the modloader (Hooks::HttpServer).
// ServerUtility registers a raw-request filter through IPluginHttpServer so
// it can deny unauthorised /remote/object/call requests.  No pattern scanning,
// no memory helpers, and no response construction live here any more — those
// responsibilities belong entirely to the modloader subsystem.
//
// The only logic retained is:
//   1. Check the configured enable flag.
//   2. Inspect the URL and, when it matches /remote/object/call, inspect the
//      JSON body for an objectPath that is not in the allow-list.
//   3. Return HttpRequestAction::Deny to make the modloader send a 403.
// ---------------------------------------------------------------------------

static constexpr auto REMOTE_CALL_URL  = "/remote/object/call";
static constexpr auto ALLOWED_OBJECT_PATH =
	"/Game/Chimera/Maps/DedicatedServerStart.DedicatedServerStart:PersistentLevel.BP_DedicatedServerSettingsActor_C_1.DedicatedServerSettingsComp";

// ---------------------------------------------------------------------------
// Minimal JSON string-value extractor — unchanged from original
// Handles: "key": "value"  with basic escape sequences.
// ---------------------------------------------------------------------------
static std::string ExtractJsonString(const std::string& json, const char* key)
{
	std::string needle = std::string("\"") + key + "\"";
	auto pos = json.find(needle);
	if (pos == std::string::npos) return {};
	pos += needle.size();

	while (pos < json.size() &&
	       (json[pos] == ' ' || json[pos] == '\t' ||
	        json[pos] == ':' || json[pos] == '\n' || json[pos] == '\r'))
		++pos;

	if (pos >= json.size() || json[pos] != '"') return {};
	++pos; // skip opening quote

	std::string value;
	value.reserve(256);
	while (pos < json.size() && json[pos] != '"')
	{
		if (json[pos] == '\\' && pos + 1 < json.size())
		{
			++pos;
			switch (json[pos])
			{
			case '"':  value += '"';  break;
			case '\\': value += '\\'; break;
			case '/':  value += '/';  break;
			case 'n':  value += '\n'; break;
			case 'r':  value += '\r'; break;
			case 't':  value += '\t'; break;
			default:   value += json[pos]; break;
			}
		}
		else
			value += json[pos];
		++pos;
	}
	return value;
}

// ---------------------------------------------------------------------------
// Raw-request filter callback
// Registered with hooks->HttpServer->RegisterOnRawRequest during Install().
// ---------------------------------------------------------------------------
static HttpRequestAction RemoteCallFilter(const PluginHttpRequest* req)
{
	if (!ServerUtilityConfig::Config::GetRemoteVulnerabilityPatch())
		return HttpRequestAction::Approve;

	if (strcmp(req->url, REMOTE_CALL_URL) != 0)
		return HttpRequestAction::Approve;

	std::string body(req->body ? req->body : "", req->bodyLen);
	std::string objectPath  = ExtractJsonString(body, "objectPath");
	std::string functionName = ExtractJsonString(body, "functionName");

	if (objectPath.find(ALLOWED_OBJECT_PATH) != std::string::npos)
		return HttpRequestAction::Approve;

	LOG_WARN("[RemoteVulnerabilityPatcher] Blocked unauthorized /remote/object/call");
	LOG_WARN("[RemoteVulnerabilityPatcher]   objectPath:   '%s'", objectPath.c_str());
	LOG_WARN("[RemoteVulnerabilityPatcher]   functionName: '%s'", functionName.c_str());
	return HttpRequestAction::Deny;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void HttpConnectionHook::Install()
{
	auto* hooks = GetHooks();
	if (!hooks || !hooks->HttpServer)
	{
		LOG_ERROR("[RemoteVulnerabilityPatcher] HttpServer interface not available — filter not registered");
		return;
	}

	hooks->HttpServer->RegisterOnRawRequest(&RemoteCallFilter);
	LOG_INFO("[RemoteVulnerabilityPatcher] Raw-request filter registered");
}

void HttpConnectionHook::Remove()
{
	auto* hooks = GetHooks();
	if (!hooks || !hooks->HttpServer)
	{
		LOG_DEBUG("[RemoteVulnerabilityPatcher] HttpServer interface not available — nothing to remove");
		return;
	}

	hooks->HttpServer->UnregisterOnRawRequest(&RemoteCallFilter);
	LOG_INFO("[RemoteVulnerabilityPatcher] Raw-request filter unregistered");
}
