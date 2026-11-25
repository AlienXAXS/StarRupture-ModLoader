#pragma once

#include <atomic>
#include <cstddef>
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

// A WinHTTP (or Win32) error code as text.
std::string FormatWinHttpError(unsigned long err);

// The response body on success, or an empty string on any error (a non-200
// status except a 304 to a conditional request, a timeout or deadline, a body
// over the cap). `label` names what is being fetched in the log. A URL's query
// is sent and never logged: a signed download address carries its key there.
std::string HttpGet(const char* url, const char* label = "resource", const HttpGetOptions& options = {});
