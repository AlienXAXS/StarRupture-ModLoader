#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>

// ---------------------------------------------------------------------------
// http_get.h -- the loader's one HTTPS GET (WinHTTP), shared by the manifest
// check, the plugin downloads and the version-map feed.
//
// Every request refuses a redirect from HTTPS to HTTP. The defaults keep the
// limits the updater always had; the feed asks for tighter ones.
// ---------------------------------------------------------------------------

struct HttpGetOptions
{
	// Per-operation timeouts, in milliseconds.
	unsigned resolveMs = 3000, connectMs = 5000, sendMs = 15000, receiveMs = 30000;

	// A wall-clock limit for the whole request, checked after each step and
	// between reads, so a server that trickles bytes cannot hold the caller
	// past the deadline plus one timeout. 0 = none.
	unsigned deadlineMs = 0;

	// The most body bytes read; a longer body is cut off while reading and the
	// request fails. 0 = no cap.
	size_t maxBytes = 0;

	// Lets an http:// URL through, for tests that run a local server. Off
	// everywhere else: every request is HTTPS.
	bool allowPlainHttp = false;

	// A conditional request. With ifNoneMatch set, the request carries it as an
	// If-None-Match header (plain printable ASCII, as an ETag is), and a 304 answer
	// is not an error: the body is empty and *notModified is set. With etag set, a 200 answer's ETag header is
	// copied into it ("" when it has none, or one that is not plain text).
	std::string  ifNoneMatch;
	std::string* etag        = nullptr;
	bool*        notModified = nullptr;

	// Follow no redirect, for a caller that checks every hop itself. A 3xx
	// answer fails like any non-200 one, and with location set its Location
	// header is copied into it ("" when it has none, or one that is not plain
	// text or is too long).
	bool         noRedirect = false;
	std::string* location   = nullptr;

	// With status set, the answer's HTTP status; 0 when no answer arrived.
	unsigned* status = nullptr;

	// With overLimit set, true when the request failed because the body went
	// over maxBytes.
	bool* overLimit = nullptr;

	// Once true, the request fails at its next step. A call that is already
	// waiting ends on its own timeout.
	const std::atomic<bool>* cancel = nullptr;
};

// Where an update may be fetched from. The configured address (a plugin's
// manifest_url, or the loader's manifest URL) names its own host; besides it,
// only GitHub's release hosts are expected. Shared with the updater exe, so
// everything here is inline.
namespace UpdateHosts
{
	// The two hosts GitHub serves release assets from.
	inline constexpr const char* kDownloadHosts[] = { "release-assets.githubusercontent.com", "objects.githubusercontent.com" };

	inline constexpr int kMaxRedirects = 10;

	inline bool IsDownloadHost(const std::string& host)
	{
		for (const char* allowed : kDownloadHosts)
			if (_stricmp(host.c_str(), allowed) == 0)
				return true;
		return false;
	}

	// "host", or "host:port" for a port other than 443, in lower case, of an
	// https URL; else empty. The host is letters, digits, '.', '-' and '_'
	// only, so user info, percent escapes, backslashes, spaces and anything
	// else WinHTTP might read differently are all refused.
	inline std::string Authority(const std::string& url)
	{
		if (_strnicmp(url.c_str(), "https://", 8) != 0)
			return {};
		const size_t end = url.find_first_of("/?#", 8);
		std::string host = url.substr(8, end == std::string::npos ? std::string::npos : end - 8);
		unsigned long port = 443;
		const size_t colon = host.find(':');
		if (colon != std::string::npos)
		{
			const std::string digits = host.substr(colon + 1);
			host.resize(colon);
			if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos)
				return {};
			port = std::stoul(digits);
			if (port == 0 || port > 65535)
				return {};
		}
		if (host.empty())
			return {};
		for (char& c : host)
		{
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
			else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
				return {};
		}
		return port == 443 ? host : host + ":" + std::to_string(port);
	}

	// Whether `url` may be fetched for an update whose configured address is
	// `source`. Hosts match whole: no suffixes.
	inline bool IsExpected(const std::string& url, const std::string& source)
	{
		const std::string to = Authority(url);
		return !to.empty() && (to == "github.com" || IsDownloadHost(to) || to == Authority(source));
	}

	// A Location header as a full address, against the one that answered.
	inline std::string ResolveLocation(const std::string& from, const std::string& location)
	{
		if (location.empty())
			return {};
		const size_t scheme = location.find("://");
		if (scheme != std::string::npos && location.find_first_of("/?#") > scheme)
			return location;
		if (location.compare(0, 2, "//") == 0)
			return "https:" + location;
		const std::string origin = from.substr(0, from.find_first_of("/?#", 8));
		if (location[0] == '/')
			return origin + location;
		const std::string path = from.substr(0, from.find_first_of("?#"));
		const size_t slash = path.rfind('/');
		return (slash == std::string::npos || slash < origin.size() ? origin + "/" : path.substr(0, slash + 1)) + location;
	}

	// One request that follows no redirect: the body of a 200, else empty.
	using HopFn = std::function<std::string(const std::string& url, unsigned& status, std::string& location)>;

	// GETs `url`, following redirects by hand so that it and every address it
	// is sent on to pass IsExpected. `refused` gets what was turned down.
	inline std::string Follow(std::string url, const std::string& source, const HopFn& hop, std::string& refused)
	{
		refused.clear();
		for (int redirects = 0;; ++redirects)
		{
			if (!IsExpected(url, source))
			{
				// The host only: the rest of a download address can carry a key.
				const std::string host = Authority(url);
				refused = host.empty() ? "an address that is not a plain https one" : host;
				return {};
			}
			unsigned status = 0;
			std::string location;
			std::string body = hop(url, status, location);
			if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308)
				return body;
			if (redirects >= kMaxRedirects)
			{
				refused = "a chain of too many redirects";
				return {};
			}
			url = ResolveLocation(url, location);
		}
	}
}

// A WinHTTP (or Win32) error code as text.
std::string FormatWinHttpError(unsigned long err);

// The response body on success, or an empty string on any error (a non-200
// status except a 304 to a conditional request, a timeout or deadline, a body
// over the cap). `label` names what is being fetched in the log. A URL's query
// is sent and never logged: a signed download address carries its key there.
std::string HttpGet(const char* url, const char* label = "resource", const HttpGetOptions& options = {});

// HttpGet for an update's manifest or file, held to UpdateHosts::Follow.
// `source` is the configured address the update started from.
std::string HttpGetUpdate(const std::string& url, const std::string& source, const char* label);
