// http_get.cpp
// The loader's WinHTTP GET -- see http_get.h.

#include "http_get.h"
#include "logging/log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// Converts a WinHTTP (or Win32) error code to a human-readable string.
// Checks the WinHTTP message table first, then falls back to the system table.
std::string FormatWinHttpError(unsigned long err)
{
	char buf[512]{};
	HMODULE hWinHttp = GetModuleHandleW(L"winhttp.dll");

	if (hWinHttp)
	{
		DWORD n = FormatMessageA(
			FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
			hWinHttp, err, 0, buf, sizeof(buf) - 1, nullptr);
		if (n > 0)
		{
			// Strip trailing CR/LF/spaces inserted by FormatMessage
			while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' '))
				buf[--n] = '\0';
			return std::string(buf);
		}
	}

	// Fall back to the system error message table
	DWORD n = FormatMessageA(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, err, 0, buf, sizeof(buf) - 1, nullptr);
	if (n > 0)
	{
		while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' '))
			buf[--n] = '\0';
		return std::string(buf);
	}

	return "unknown error " + std::to_string(err);
}

namespace
{
	// Closes a WinHTTP handle when it goes out of scope.
	struct Handle
	{
		HINTERNET h = nullptr;
		~Handle() { if (h) WinHttpCloseHandle(h); }
	};

	// The longest URL, path or query taken. A signed download address runs to
	// well over a thousand characters.
	constexpr DWORD kUrlChars = 4096;

	// A header's text, kept only when it is printable ASCII of at most maxLen.
	std::string PlainHeader(HINTERNET request, DWORD query, const wchar_t* name, size_t maxLen)
	{
		std::vector<wchar_t> value(maxLen + 1, L'\0');
		DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
		std::string text;
		if (!WinHttpQueryHeaders(request, query, name, value.data(), &size, WINHTTP_NO_HEADER_INDEX))
			return text;
		for (const wchar_t* p = value.data(); *p; ++p)
		{
			if (*p < 0x20 || *p > 0x7E || text.size() >= maxLen)
				return {};
			text += static_cast<char>(*p);
		}
		return text;
	}
}

// Returns the full HTTP response body on success, or an empty string on any
// error.  Handles HTTPS and follows redirects (required for GitHub CDN) unless
// the caller asks for none, but never from HTTPS to HTTP.  `label` is used only
// in log messages to identify what's being fetched.
std::string HttpGet(const char* url, const char* label, const HttpGetOptions& options)
{
	const std::string shown = std::string(url).substr(0, std::string(url).find('?'));
	LogToFile::Debug("[AutoUpdate] HttpGet [%s]: %s", label, shown.c_str());
	if (options.status)
		*options.status = 0;
	if (options.overLimit)
		*options.overLimit = false;
	if (options.location)
		options.location->clear();
	const auto cancelled = [&] { return options.cancel && options.cancel->load(); };
	if (cancelled())
		return {};
	const ULONGLONG started = GetTickCount64();
	const auto expired = [&] { return options.deadlineMs != 0 && GetTickCount64() - started >= options.deadlineMs; };
	const auto fail = [&](const char* what)
	{
		DWORD err = GetLastError();
		LogToFile::Debug("[AutoUpdate] HttpGet [%s]: %s failed (%lu: %s)", label, what, err,
		                 FormatWinHttpError(err).c_str());
		return std::string();
	};

	// Convert URL to wide string
	std::vector<wchar_t> wUrl(kUrlChars, L'\0');
	if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wUrl.data(), kUrlChars) == 0)
		return fail("URL encoding");

	// Crack the URL into host, path and query components
	URL_COMPONENTS uc{};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256]{};
	std::vector<wchar_t> path(kUrlChars, L'\0'), extra(kUrlChars, L'\0');
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path.data();
	uc.dwUrlPathLength = kUrlChars;
	uc.lpszExtraInfo = extra.data();
	uc.dwExtraInfoLength = kUrlChars;
	if (!WinHttpCrackUrl(wUrl.data(), 0, 0, &uc))
		return fail("WinHttpCrackUrl");

	LogToFile::Debug("[AutoUpdate] HttpGet [%s]: host=%ls path=%ls port=%d", label, host, path.data(),
	                 static_cast<int>(uc.nPort));

	// The query goes with the path; a fragment is never sent.
	std::wstring target = std::wstring(path.data()) + extra.data();
	const size_t fragment = target.find(L'#');
	if (fragment != std::wstring::npos)
		target.resize(fragment);

	const bool plain = options.allowPlainHttp && uc.nScheme == INTERNET_SCHEME_HTTP;
	if (uc.nScheme != INTERNET_SCHEME_HTTPS && !plain)
	{
		LogToFile::Warn("[AutoUpdate] HttpGet [%s]: refused %s: the URL is not https", label, shown.c_str());
		return {};
	}

	// WINHTTP_ACCESS_TYPE_NO_PROXY: connect directly without reading IE/user
	// proxy registry settings.  Dedicated game servers run as SYSTEM which has
	// no IE proxy hive, causing WinHttpSendRequest to fail with error 5023
	// (ERROR_WINHTTP_CANNOT_CONNECT) when the default proxy mode is used.
	Handle session;
	session.h = WinHttpOpen(
		L"StarRupture-ModLoader-AutoUpdate/1.0",
		WINHTTP_ACCESS_TYPE_NO_PROXY,
		nullptr, nullptr, 0);
	if (!session.h)
		return fail("WinHttpOpen");

	// Cap individual operation timeouts so we never block startup indefinitely.
	WinHttpSetTimeouts(session.h, options.resolveMs, options.connectMs, options.sendMs, options.receiveMs);

	INTERNET_PORT port = (uc.nPort != 0) ? uc.nPort : (plain ? INTERNET_DEFAULT_HTTP_PORT : INTERNET_DEFAULT_HTTPS_PORT);
	Handle connect;
	connect.h = WinHttpConnect(session.h, host, port, 0);
	if (!connect.h)
		return fail("WinHttpConnect");

	Handle request;
	request.h = WinHttpOpenRequest(
		connect.h, L"GET", target.c_str(),
		nullptr, nullptr, nullptr, plain ? 0 : WINHTTP_FLAG_SECURE);
	if (!request.h)
		return fail("WinHttpOpenRequest");

	// Follow GitHub CDN redirects automatically, but never from HTTPS to HTTP.
	DWORD redirectPolicy = options.noRedirect ? WINHTTP_OPTION_REDIRECT_POLICY_NEVER
	                                          : WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	if (!WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy))
		&& options.noRedirect)
		return fail("WinHttpSetOption(redirect policy)");

	if (!options.ifNoneMatch.empty())
	{
		const std::wstring header = L"If-None-Match: " + std::wstring(options.ifNoneMatch.begin(), options.ifNoneMatch.end());
		WinHttpAddRequestHeaders(request.h, header.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
	}

	LogToFile::Debug("[AutoUpdate] HttpGet [%s]: sending request...", label);
	if (cancelled())
		return {};

	if (!WinHttpSendRequest(request.h, nullptr, 0, nullptr, 0, 0, 0))
		return fail("WinHttpSendRequest");
	if (expired())
	{
		LogToFile::Warn("[AutoUpdate] HttpGet [%s]: deadline of %u ms passed before the response", label, options.deadlineMs);
		return {};
	}

	if (cancelled())
		return {};
	if (!WinHttpReceiveResponse(request.h, nullptr))
		return fail("WinHttpReceiveResponse");

	// Check HTTP status code
	DWORD statusCode = 0;
	DWORD statusSize = sizeof(statusCode);
	WinHttpQueryHeaders(request.h,
	                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                    nullptr,
	                    &statusCode, &statusSize, nullptr);

	LogToFile::Debug("[AutoUpdate] HttpGet [%s]: HTTP %lu", label, statusCode);
	if (options.status)
		*options.status = statusCode;

	// Not followed: the caller reads the address and decides.
	if (options.noRedirect && statusCode >= 300 && statusCode < 400 && statusCode != 304)
	{
		if (options.location)
			*options.location = PlainHeader(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, kUrlChars - 1);
		return {};
	}

	if (statusCode == 304 && !options.ifNoneMatch.empty() && options.notModified)
	{
		*options.notModified = true;
		return {};
	}

	if (statusCode != 200)
	{
		// Promote to Warn so HTTP errors are always visible without debug logging.
		// Common causes:
		//   404 -- release does not exist or is a private/draft release
		//   403 -- asset requires authentication (private repo)
		//   5xx -- GitHub CDN or release server error
		LogToFile::Warn("[AutoUpdate] HttpGet [%s]: server returned HTTP %lu (URL: %s)", label, statusCode, shown.c_str());
		return {};
	}

	// Only printable ASCII, since it goes back out in a request header.
	if (options.etag)
		*options.etag = PlainHeader(request.h, WINHTTP_QUERY_CUSTOM, L"ETag", 128);

	// Read response body in chunks
	std::string body;
	body.reserve(options.maxBytes && options.maxBytes < 65536 ? options.maxBytes : 65536);

	for (;;)
	{
		if (cancelled())
			return {};
		if (expired())
		{
			LogToFile::Warn("[AutoUpdate] HttpGet [%s]: deadline of %u ms passed -- %zu bytes received so far; discarding", label, options.deadlineMs, body.size());
			return {};
		}

		DWORD available = 0;
		if (!WinHttpQueryDataAvailable(request.h, &available))
		{
			DWORD err = GetLastError();
			LogToFile::Warn(
				"[AutoUpdate] HttpGet [%s]: WinHttpQueryDataAvailable failed (%lu: %s) -- %zu bytes received so far; discarding partial data to avoid corrupt file on disk",
				label, err, FormatWinHttpError(err).c_str(), body.size());
			return {};
		}
		if (available == 0)
			break; // normal end of response

		if (options.maxBytes != 0 && body.size() + available > options.maxBytes)
		{
			LogToFile::Warn("[AutoUpdate] HttpGet [%s]: the body is over the %zu byte limit -- cut off", label, options.maxBytes);
			if (options.overLimit)
				*options.overLimit = true;
			return {};
		}

		std::string chunk(available, '\0');
		DWORD bytesRead = 0;
		if (!WinHttpReadData(request.h, chunk.data(), available, &bytesRead))
		{
			DWORD err = GetLastError();
			LogToFile::Warn(
				"[AutoUpdate] HttpGet [%s]: WinHttpReadData failed mid-stream (%lu: %s) -- %zu bytes received so far; discarding partial data to avoid corrupt file on disk",
				label, err, FormatWinHttpError(err).c_str(), body.size());
			return {};
		}
		chunk.resize(bytesRead);
		body += chunk;
	}

	LogToFile::Debug("[AutoUpdate] HttpGet [%s]: received %zu bytes", label, body.size());
	return body;
}

std::string HttpGetUpdate(const std::string& url, const std::string& source, const char* label)
{
	std::string refused;
	std::string body = UpdateHosts::Follow(url, source,
		[label](const std::string& at, unsigned& status, std::string& location)
		{
			HttpGetOptions options;
			options.noRedirect = true;
			options.status = &status;
			options.location = &location;
			return HttpGet(at.c_str(), label, options);
		}, refused);
	if (!refused.empty())
		LogToFile::Warn("[AutoUpdate] [%s]: update refused: it points to %s, not an expected update host",
		                label, refused.c_str());
	return body;
}
