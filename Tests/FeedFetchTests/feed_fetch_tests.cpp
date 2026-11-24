// FeedFetchTests: the loader's feed fetch (auto_update/feed_fetch.cpp) and its
// HttpGet (auto_update/http_get.cpp) against a server on 127.0.0.1 that this
// test starts. Feeds are signed at run time with a throwaway key made here.
//
// The cases that need something outside this machine are not here: a request
// to a real release, and an HTTPS-to-HTTP redirect (that needs a TLS server).
#include "test_doubles.h"

#include "auto_update/feed_fetch.h"
#include "auto_update/http_get.h"
#include "auto_update/plugin_dll_version.h"
#include "auto_update/version_compare.h"
#include "memory_scanner/version_feed.h"
#include "memory_scanner/version_map.h"
#include "plugins/plugin_catalog.h"
#include "third_party/monocypher/monocypher-ed25519.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

// This program stands in for a plugin: it exports GetPluginInfo the way a
// plugin DLL does, so PluginDllVersion::Read can be pointed at it.
extern "C" __declspec(dllexport) PluginInfo* GetPluginInfo()
{
	static PluginInfo info = { "Test", "9.8.7-test", "test", "test", PLUGIN_INTERFACE_VERSION_MAX, 0 };
	return &info;
}

using VersionFeed::PublicKey;
using VersionFeed::SignedFeed;
using VersionMap::Exe;
using VersionMap::MapData;

namespace
{
	int g_checks = 0;
	int g_failed = 0;

	void Check(bool ok, const char* what)
	{
		++g_checks;
		if (!ok)
		{
			++g_failed;
			printf("FAIL: %s\n", what);
		}
	}

	using Clock = std::chrono::steady_clock;
	long long MsSince(Clock::time_point t) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count(); }

	// ---- signing (as VersionFeedTests) --------------------------------------

	std::string Base64(const uint8_t* p, size_t n)
	{
		static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string s;
		for (size_t i = 0; i < n; i += 3)
		{
			const uint32_t v = (uint32_t(p[i]) << 16) | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
			s += a[v >> 18];
			s += a[(v >> 12) & 63];
			s += i + 1 < n ? a[(v >> 6) & 63] : '=';
			s += i + 2 < n ? a[v & 63] : '=';
		}
		return s;
	}

	struct Signer
	{
		uint8_t   secret[64]{};
		PublicKey pub;
	};

	Signer MakeSigner(const char* id, uint8_t salt)
	{
		Signer s;
		uint8_t seed[32];
		for (int i = 0; i < 32; ++i)
			seed[i] = static_cast<uint8_t>(salt * 7 + i);
		crypto_ed25519_key_pair(s.secret, s.pub.key.data(), seed);
		s.pub.id = id;
		return s;
	}

	std::string SignedFile(const Signer& s, uint64_t seq)
	{
		const std::string region = "format\t1\nseq\t" + std::to_string(seq) + "\n"
			"build\t127004\t0x11111111\t0x01000000\tpublic 1\n"
			"build\t128786\t0x22222222\t0x02000000\tpublic_test_branch 2\n"
			"function\tTest::MapOnly\t0x00000000\t128786\t0x22222222\t0x02000000\t1\tAA 01\t0\t-1\t-1\t0x00001000\n";
		uint8_t sig[64];
		crypto_ed25519_sign(sig, s.secret, reinterpret_cast<const uint8_t*>(region.data()), region.size());
		return region + "sig\t" + s.pub.id + "\t" + Base64(sig, 64) + "\n";
	}

	std::string SignedCatalog(const Signer& s, uint64_t seq, const char* version)
	{
		const std::string region = "format\t1\nseq\t" + std::to_string(seq) + "\nkind\tplugin-catalog\n"
			"plugin\tAlpha\t" + version + "\n"
			"feature\tAlpha\tfast-mode\t-\tFast mode\n";
		uint8_t sig[64];
		crypto_ed25519_sign(sig, s.secret, reinterpret_cast<const uint8_t*>(region.data()), region.size());
		return region + "sig\t" + s.pub.id + "\t" + Base64(sig, 64) + "\n";
	}

	// The dedicated server's file: its build lines are labelled "server ...".
	std::string ServerSignedFile(const Signer& s, uint64_t seq)
	{
		const std::string region = "format\t1\nseq\t" + std::to_string(seq) + "\n"
			"build\t127004\t0x2E3595FF\t0x0E2A8000\tserver steam_build 1\n"
			"build\t128786\t0x33333333\t0x03000000\tserver steam_build 2\n"
			"function\tTest::ServerOnly\t0x00000000\t128786\t0x33333333\t0x03000000\t1\t-\t0\t-1\t-1\t0x00001000\n";
		uint8_t sig[64];
		crypto_ed25519_sign(sig, s.secret, reinterpret_cast<const uint8_t*>(region.data()), region.size());
		return region + "sig\t" + s.pub.id + "\t" + Base64(sig, 64) + "\n";
	}

	// ---- files --------------------------------------------------------------

	std::wstring TempPath(const wchar_t* name)
	{
		wchar_t dir[MAX_PATH]{};
		GetTempPathW(MAX_PATH, dir);
		return std::wstring(dir) + L"vmap_fetch_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + name;
	}

	bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

	std::string Slurp(const std::wstring& p)
	{
		std::string s;
		HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return s;
		LARGE_INTEGER sz{};
		GetFileSizeEx(h, &sz);
		s.resize(static_cast<size_t>(sz.QuadPart));
		DWORD n = 0;
		if (!s.empty())
			ReadFile(h, s.data(), static_cast<DWORD>(s.size()), &n, nullptr);
		CloseHandle(h);
		return s;
	}

	// ---- the local server ---------------------------------------------------
	//
	// Routes:
	//   /ok        200 with the current feed (and its ETag, when one is set); 304 when
	//              the request's If-None-Match is that ETag
	//   /catalog   200 with the current plugin catalog, no ETag
	//   /redirect  302 to /ok
	//   /missing   404
	//   /hang      reads the request and says nothing
	//   /stall     200 with a length, then no body
	//   /trickle   200 with a length, then one byte every 400 ms
	//   /big       200 with a body one byte over the 1 MiB cap
	class Server
	{
	public:
		std::atomic<int> hits{ 0 };
		std::atomic<int> served304{ 0 };
		std::string feed;
		std::string etag;                // set between requests, never during one
		std::string catalog;             // served at /catalog, with no ETag
		std::string lastIfNoneMatch;     // of the latest request; read after its answer arrived

		bool Start()
		{
			WSADATA wsa;
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
				return false;
			m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			addr.sin_port = 0;
			if (bind(m_listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(m_listener, 16) != 0)
				return false;
			int len = sizeof(addr);
			getsockname(m_listener, reinterpret_cast<sockaddr*>(&addr), &len);
			m_port = ntohs(addr.sin_port);
			m_acceptor = std::thread([this] { Accept(); });
			return true;
		}

		void Stop()
		{
			m_stop = true;
			closesocket(m_listener);
			if (m_acceptor.joinable())
				m_acceptor.join();
			for (auto& t : m_handlers)
				t.join();
			WSACleanup();
		}

		std::string Url(const char* route) const { return "http://127.0.0.1:" + std::to_string(m_port) + route; }

	private:
		SOCKET m_listener = INVALID_SOCKET;
		unsigned short m_port = 0;
		std::atomic<bool> m_stop{ false };
		std::thread m_acceptor;
		std::vector<std::thread> m_handlers;

		void Accept()
		{
			for (;;)
			{
				SOCKET c = accept(m_listener, nullptr, nullptr);
				if (c == INVALID_SOCKET)
					return;
				m_handlers.emplace_back([this, c] { Handle(c); });
			}
		}

		void Nap(int ms)
		{
			for (int slept = 0; slept < ms && !m_stop; slept += 50)
				Sleep(50);
		}

		static bool SendAll(SOCKET s, const char* p, size_t n)
		{
			while (n)
			{
				const int sent = send(s, p, static_cast<int>(n > 65536 ? 65536 : n), 0);
				if (sent <= 0)
					return false;
				p += sent;
				n -= static_cast<size_t>(sent);
			}
			return true;
		}

		static std::string Head(int status, const char* text, size_t length, const char* extra = "")
		{
			return "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Length: " + std::to_string(length)
				+ "\r\nConnection: close\r\n" + extra + "\r\n";
		}

		void Handle(SOCKET c)
		{
			std::string req;
			char buf[1024];
			while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192)
			{
				const int n = recv(c, buf, sizeof(buf), 0);
				if (n <= 0)
				{
					closesocket(c);
					return;
				}
				req.append(buf, static_cast<size_t>(n));
			}
			const size_t a = req.find(' ') + 1;
			const std::string path = req.substr(a, req.find(' ', a) - a);
			std::string lower = req;
			for (char& ch : lower)
				ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
			const std::string headerName = "if-none-match: ";
			std::string inm;
			const size_t at = lower.find(headerName);
			if (at != std::string::npos)
				inm = req.substr(at + headerName.size(), req.find("\r\n", at) - at - headerName.size());
			lastIfNoneMatch = inm;
			++hits;

			if (path == "/ok")
			{
				if (!etag.empty() && inm == etag)
				{
					++served304;
					const std::string h = Head(304, "Not Modified", 0);
					SendAll(c, h.data(), h.size());
				}
				else
				{
					const std::string extra = etag.empty() ? std::string() : "ETag: " + etag + "\r\n";
					const std::string h = Head(200, "OK", feed.size(), extra.c_str());
					SendAll(c, h.data(), h.size());
					SendAll(c, feed.data(), feed.size());
				}
			}
			else if (path == "/catalog")
			{
				const std::string h = Head(200, "OK", catalog.size());
				SendAll(c, h.data(), h.size());
				SendAll(c, catalog.data(), catalog.size());
			}
			else if (path == "/redirect")
			{
				const std::string h = Head(302, "Found", 0, "Location: /ok\r\n");
				SendAll(c, h.data(), h.size());
			}
			else if (path == "/missing")
			{
				const std::string h = Head(404, "Not Found", 0);
				SendAll(c, h.data(), h.size());
			}
			else if (path == "/hang")
			{
				Nap(6000);
			}
			else if (path == "/stall")
			{
				const std::string h = Head(200, "OK", 100000);
				SendAll(c, h.data(), h.size());
				Nap(6000);
			}
			else if (path == "/trickle")
			{
				const std::string h = Head(200, "OK", 100000);
				SendAll(c, h.data(), h.size());
				for (int i = 0; i < 40 && !m_stop; ++i)
				{
					if (!SendAll(c, "x", 1))
						break;
					Nap(400);
				}
			}
			else if (path == "/big")
			{
				const size_t n = VersionFeed::kMaxFileBytes + 1;
				const std::string h = Head(200, "OK", n);
				SendAll(c, h.data(), h.size());
				const std::string body(n, 'x');
				SendAll(c, body.data(), body.size());
			}
			closesocket(c);
		}
	};

	FeedFetch::Config ConfigFor(const Server& srv, const char* route, const std::vector<PublicKey>& keys, const std::wstring& cache,
		VersionFeed::Flavor flavor = VersionFeed::Flavor::Client)
	{
		FeedFetch::Config cfg;
		cfg.url = srv.Url(route);
		cfg.keys = keys;
		cfg.cachePath = cache;
		cfg.flavor = flavor;
		cfg.autoUpdate = true;
		cfg.allowPlainHttp = true;
		return cfg;
	}

	bool Logged(const wchar_t* text)
	{
		for (const std::wstring& line : TestDoubles::g_log)
			if (line.find(text) != std::wstring::npos)
				return true;
		return false;
	}

	std::wstring EtagFile(const std::wstring& cache) { return cache + L".etag"; }
}

int main()
{
	const Signer sk = MakeSigner("k1", 1);
	const std::vector<PublicKey> keys{ sk.pub };

	// ---- Update checks: newer by number, never an older release ------------
	{
		using namespace AutoUpdateVersion;
		const auto cmp = [](const char* a, const char* b) { return CompareVersions(ParseVersion(a), ParseVersion(b)); };
		Check(cmp("v1.22.1", "v1.23.0-rc1") < 0, "version: a release older than the pre-release installed is older");
		Check(cmp("v1.23.0", "v1.23.0-rc1") == 0, "version: a pre-release equals its release (not offered)");
		Check(cmp("v0.10.0", "0.11.0-rc1") < 0, "version: an older plugin release is older than a newer local build");
		Check(cmp("v0.11.0", "0.10.0-gss.32") > 0, "version: a newer release is newer than a local build");
		Check(cmp("1.23.0.0", "v1.23.0") == 0, "version: a file version equals its tag");
		Check(cmp("v1.10.0", "v1.9.9") > 0, "version: compared by number, not as text");
		Check(!ParseVersion("dev").valid && !ParseVersion("").valid && !ParseVersion(nullptr).valid, "version: no number, not valid");
	}

	// ---- The plugin pass on the first launch after a hand install -----------
	{
		using namespace AutoUpdateVersion;

		// The hole: update_state.ini still holds the last release the updater
		// wrote down (v0.20.0), a newer release has come out (v0.22.0), and a
		// build newer than both was put in by hand (0.23.0-rc1) before the
		// plugin ever loaded. Judged on the stored version alone, the pass
		// would download v0.22.0 over the hand-installed build.
		Check(ReleaseIsNewer("v0.22.0", "v0.20.0"), "hole: the old rule offers v0.22.0 over the stored v0.20.0");
		const PluginUpdateDecision hand = DecidePluginUpdate("v0.20.0", "0.23.0-rc1", "v0.22.0");
		Check(!hand.download, "hand install: a release older than the installed build is not downloaded");
		Check(hand.installed == "0.23.0-rc1", "hand install: the installed build becomes the stored version");

		// Nothing stored yet: the DLL is the baseline, so only a newer release is taken.
		Check(!DecidePluginUpdate("", "0.23.0-rc1", "v0.22.0").download, "no stored version: an older release is not downloaded");
		Check(DecidePluginUpdate("", "0.21.0", "v0.22.0").download, "no stored version: a newer release is downloaded");
		Check(DecidePluginUpdate("", "0.21.0", "v0.22.0").installed == "0.21.0", "no stored version: the DLL's version is recorded");

		// A pre-release is never offered its own release.
		Check(!DecidePluginUpdate("v0.20.0", "0.23.0-rc1", "v0.23.0").download, "a pre-release is not offered its own release");

		// The ordinary update still happens, and a stored version newer than the
		// DLL (a downgrade by hand) is kept as the bar.
		Check(DecidePluginUpdate("v0.20.0", "0.20.0", "v0.22.0").download, "an installed release is updated to a newer one");
		Check(!DecidePluginUpdate("v0.23.0", "0.20.0", "v0.22.0").download, "a stored version newer than the DLL stays the bar");

		// The DLL cannot be read: the stored version decides, as before; with
		// neither, nothing is downloaded.
		Check(DecidePluginUpdate("v0.20.0", "", "v0.22.0").download, "unreadable DLL: the stored version decides");
		const PluginUpdateDecision unknown = DecidePluginUpdate("", "", "v0.22.0");
		Check(!unknown.download && unknown.installed.empty(), "no version known: nothing is downloaded");
	}

	// ---- Release notes per version --------------------------------------------
	{
		using namespace AutoUpdateVersion;

		const char* hist =
			"## v1.23.1\n- c1\n- c2\n\n"
			"## v1.23.0\n- b1\n- b2\n\n"
			"## v1.22.1\n- a1";

		// Selection: the sections newer than the previous version.
		VersionedNotes v = SelectVersionedNotes(hist, "1.22.1", "v1.23.1");
		Check(v.versioned && v.text == "## v1.23.1\n- c1\n- c2\n## v1.23.0\n- b1\n- b2", "notes: from 1.22.1 two versions, grouped");
		v = SelectVersionedNotes(hist, "1.23.0", "v1.23.1");
		Check(v.text == "- c1\n- c2", "notes: from 1.23.0 one version, no header");
		v = SelectVersionedNotes(hist, "1.21.0", "v1.23.1");
		Check(v.text.find("## v1.22.1\n- a1") != std::string::npos && v.text.find("## v1.23.1") == 0, "notes: from older, three versions in file order");
		Check(SelectVersionedNotes(hist, "1.23.1", "v1.23.1").text.empty(), "notes: previous equal to current selects nothing");
		Check(SelectVersionedNotes(hist, "1.24.0", "v1.23.1").text.empty(), "notes: previous newer than all selects nothing");
		Check(SelectVersionedNotes(hist, "1.23.1-rc1", "v1.23.1").text.empty(), "notes: an rc previous counts by number");
		Check(SelectVersionedNotes(hist, "1.22.1-rc2", "v1.23.1").text.find("## v1.23.0") != std::string::npos, "notes: an rc previous of an older number still lists newer");

		// Unknown previous: the current version only.
		Check(SelectVersionedNotes(hist, "", "v1.23.1").text == "- c1\n- c2", "notes: unknown previous lists the current version only");
		Check(SelectVersionedNotes(hist, nullptr, "1.23.0").text == "- b1\n- b2", "notes: unknown previous, current in the middle");
		Check(SelectVersionedNotes(hist, "garbage", "v1.23.1").text == "- c1\n- c2", "notes: unparseable previous is unknown");
		Check(SelectVersionedNotes(hist, "", "1.20.0").text.empty(), "notes: unknown previous and no section for the current");

		// Sections newer than the current version are not listed.
		Check(SelectVersionedNotes(hist, "1.22.1", "1.23.0").text == "- b1\n- b2", "notes: nothing above the current version");

		// Headers: odd, missing, CRLF, empty sections, text before the first header.
		Check(!SelectVersionedNotes("- a\n- b", "", "1.0.0").versioned, "notes: no headers is not versioned");
		Check(!SelectVersionedNotes("", "", "1.0.0").versioned && !SelectVersionedNotes(nullptr, "", "1.0.0").versioned, "notes: empty or null is not versioned");
		Check(!SelectVersionedNotes("## notes\n- a\n##v1.2.3\n- b\n#  v1.2.3", "", "1.2.3").versioned, "notes: headers that are not '## <version>' do not count");
		v = SelectVersionedNotes("## v1.2.0\n- a\n## 2.0 is great\n- b\n## v1.2.0 and more\n- c", "1.1.0", "1.2.0");
		Check(v.versioned && v.text == "- a\n## 2.0 is great\n- b\n## v1.2.0 and more\n- c", "notes: a line with more than the version token stays a note");
		v = SelectVersionedNotes("## v1.2.0\n- a\n## notes for people\n- b", "1.1.0", "1.2.0");
		Check(v.versioned && v.text == "- a\n## notes for people\n- b", "notes: a bad header line stays a note line");
		v = SelectVersionedNotes("## v1.2.0\r\n- a\r\n\r\n## v1.1.0\r\n- b\r\n", "1.0.0", "1.2.0");
		Check(v.text == "## v1.2.0\n- a\n## v1.1.0\n- b", "notes: CRLF lines");
		v = SelectVersionedNotes("## v1.3.0\n\n## v1.2.0\n- a", "1.1.0", "1.3.0");
		Check(v.text == "- a", "notes: an empty section is dropped");
		v = SelectVersionedNotes("stray line\n## v1.2.0\n- a", "", "1.2.0");
		Check(v.text == "- a", "notes: text before the first header is ignored");
		v = SelectVersionedNotes("  ## v1.2.0  \n- a", "", "1.2.0");
		Check(v.versioned && v.text == "- a", "notes: header whitespace is trimmed");
		v = SelectVersionedNotes("## v1.2.0-rc1\n- a", "", "1.2.0");
		Check(v.text == "- a", "notes: an rc header matches by number");
		v = SelectVersionedNotes("## v1.0.0\n- old\n## v1.1.0\n- new", "0.9.0", "1.1.0");
		Check(v.text == "## v1.0.0\n- old\n## v1.1.0\n- new", "notes: file order is kept");
		Check(SelectVersionedNotes(hist, "", "").text == "- c1\n- c2", "notes: unknown previous and current gives the first section");

		// A row for the window: versioned text, else notes.
		Check(NotesForUpdate(hist, "- short", "1.23.0", "1.23.1") == "- c1\n- c2", "row: the versioned text wins");
		Check(NotesForUpdate("", "- short", "1.23.0", "1.23.1") == "- short", "row: no release_notes falls back to notes");
		Check(NotesForUpdate(nullptr, "- short", "1.23.0", "1.23.1") == "- short", "row: a null release_notes falls back to notes");
		Check(NotesForUpdate("- plain\n- list", "- short", "1.23.0", "1.23.1") == "- short", "row: release_notes without headers falls back to notes");
		Check(NotesForUpdate(hist, "- short", "1.23.1", "1.23.1") == "- short", "row: previous equal to every entry falls back to notes");
		Check(NotesForUpdate(hist, "- short", "2.0.0", "2.0.1") == "- short", "row: previous newer than every entry falls back to notes");
		Check(NotesForUpdate(hist, "- short", "", "1.23.1") == "- c1\n- c2", "row: unknown previous lists the current version");
		Check(NotesForUpdate("", "", "1.0.0", "1.1.0").empty() && NotesForUpdate(nullptr, nullptr, "", "").empty(), "row: nothing at all is empty");
		Check(NotesForUpdate(hist, nullptr, "1.23.1", "1.23.1").empty(), "row: no notes and nothing selected is empty");

		// The previous version from the updater's logs.
		const char* updLog =
			"[2026-11-06 10:00:00.000] [INFO ] Installed version (Core DLL file version): 1.22.1\r\n"
			"[2026-11-06 10:00:01.000] [INFO ] Remote build_tag: v1.23.1\r\n"
			"[2026-11-06 10:00:01.100] [INFO ] Update available: 1.22.1 -> v1.23.1\r\n"
			"[2026-11-06 10:00:09.000] [INFO ] Install: update v1.23.1 installed successfully (14 files)\r\n";
		Check(PreviousFromUpdateLine(updLog, "1.23.1") == "1.22.1", "prev: update line, from 1.22.1");
		Check(PreviousFromUpdateLine(updLog, "1.23.1-rc1") == "1.22.1", "prev: update line, an rc matches by number");
		Check(PreviousFromUpdateLine(updLog, "1.23.2").empty(), "prev: an update line to another version is not used");
		Check(PreviousFromUpdateLine("Update available: 1.23.0 -> v1.23.1\n", "1.23.1") == "1.23.0", "prev: update line without a prefix, from 1.23.0");
		Check(PreviousFromUpdateLine("Update available: 1.24.0 -> v1.23.1\n", "1.23.1").empty(), "prev: an update line that goes backwards is not used");
		Check(PreviousFromUpdateLine("Update available: x -> y\nUpdate available:\nUpdate available: 1.2 ->", "1.2.0").empty(), "prev: odd update lines are ignored");
		Check(PreviousFromUpdateLine(nullptr, "1.23.1").empty() && PreviousFromUpdateLine("", "1.23.1").empty(), "prev: no log, no previous");
		Check(PreviousFromUpdateLine("Update available: 1.21.0 -> v1.23.1\nUpdate available: 1.22.1 -> v1.23.1\n", "1.23.1") == "1.22.1", "prev: the last matching update line wins");

		const char* instLog =
			"[x] [INFO ] Installed version (Core DLL file version): 1.22.1\r\n"
			"[x] [INFO ] Installed version (update_state.ini): v1.23.0\r\n";
		Check(PreviousFromInstalledLine(instLog) == "v1.23.0", "prev: installed line, the last one");
		Check(PreviousFromInstalledLine("Installed version (Core DLL file version): 1.22.1") == "1.22.1", "prev: installed line without a line end");
		Check(PreviousFromInstalledLine("Installed version could not be determined").empty(), "prev: the failure line is not a version");
		Check(PreviousFromInstalledLine("Installed version (x): nope").empty() && PreviousFromInstalledLine(nullptr).empty(), "prev: odd installed lines are ignored");

		// ResolvePreviousVersion: pending, then recorded, then this boot's update line, then last boot's installed line.
		Check(ResolvePreviousVersion("", "v1.23.1", updLog, instLog, "1.23.1-rc1") == "v1.23.1", "resolve: a record wins");
		Check(ResolvePreviousVersion("", "", updLog, instLog, "1.23.1") == "1.22.1", "resolve: this boot's update line wins over the old log");
		Check(ResolvePreviousVersion("", "garbage", updLog, "", "1.23.1") == "1.22.1", "resolve: an unparseable record is none");
		Check(ResolvePreviousVersion("", "", "", instLog, "1.23.1") == "v1.23.0", "resolve: a manual update falls back to last boot's version");
		Check(ResolvePreviousVersion("", "", "", "Installed version (Core DLL file version): 1.23.1\n", "1.23.1").empty(), "resolve: last boot already ran this version, unknown");
		Check(ResolvePreviousVersion("", "", "", "Installed version (Core DLL file version): 1.24.0\n", "1.23.1").empty(), "resolve: last boot newer, unknown");
		Check(ResolvePreviousVersion("", "", "", "", "1.23.1").empty() && ResolvePreviousVersion(nullptr, nullptr, nullptr, nullptr, "1.23.1").empty(), "resolve: fresh install, unknown");
		Check(ResolvePreviousVersion("1.22.1", "", updLog, instLog, "1.23.1") == "1.22.1" && ResolvePreviousVersion("1.21.0", "", updLog, instLog, "1.23.1") == "1.21.0", "resolve: a pending previous wins over the logs");
		Check(ResolvePreviousVersion("1.22.1", "v1.23.0", "", "", "1.23.1") == "1.22.1", "resolve: a pending previous wins over an older record");
		Check(ResolvePreviousVersion("1.23.1", "", updLog, instLog, "1.23.1") == "1.22.1" && ResolvePreviousVersion("garbage", "", "", instLog, "1.23.1") == "v1.23.0", "resolve: a pending value that is not older is ignored");
		Check(ResolvePreviousVersion("1.22.1", "v1.23.1", "", "", "1.23.1") == "v1.23.1", "resolve: a record of the running version beats a stale pending one");

		// DecideLoaderNotes: gate and text.
		const char* brief = "- short";
		LoaderNotesDecision d = DecideLoaderNotes("1.23.1", "v1.23.1", "1.22.1", hist, brief);
		Check(d.show && d.text == "## v1.23.1\n- c1\n- c2\n## v1.23.0\n- b1\n- b2", "loader: from 1.22.1 lists 1.23.1 and 1.23.0");
		d = DecideLoaderNotes("1.23.1", "v1.23.1", "v1.23.0", hist, brief);
		Check(d.show && d.text == "- c1\n- c2", "loader: from 1.23.0 lists 1.23.1 only");
		d = DecideLoaderNotes("1.23.1", "v1.23.1", "", hist, brief);
		Check(d.show && d.text == "- c1\n- c2", "loader: unknown previous lists the running version only");
		Check(DecideLoaderNotes("1.23.1", "v1.23.1", nullptr, hist, brief).show, "loader: a null previous is unknown");
		Check(DecideLoaderNotes("1.23.1", "v1.23.1", "garbage", hist, brief).show, "loader: an unparseable previous is unknown");
		Check(!DecideLoaderNotes("1.23.1", "v1.23.1", "v1.23.1", hist, brief).show, "loader: previous equal to running shows nothing");
		Check(!DecideLoaderNotes("1.23.1", "v1.23.1", "1.23.1-rc1", hist, brief).show, "loader: an rc previous of this number shows nothing");
		Check(!DecideLoaderNotes("1.23.1", "v1.23.1", "v1.24.0", hist, brief).show, "loader: previous newer than running shows nothing");
		Check(!DecideLoaderNotes("1.23.1", "v1.23.2", "1.23.0", hist, brief).show, "loader: a newer manifest shows nothing");
		Check(!DecideLoaderNotes("1.23.1", "v1.23.0", "1.22.1", hist, brief).show, "loader: an older manifest shows nothing");
		Check(!DecideLoaderNotes("1.23.1", "", "", hist, brief).show && !DecideLoaderNotes("1.23.1", nullptr, "", hist, brief).show, "loader: a failed fetch shows nothing");
		Check(!DecideLoaderNotes("", "v1.23.1", "", hist, brief).show, "loader: a dev build shows nothing");
		d = DecideLoaderNotes("1.23.1", "v1.23.1", "1.22.1", "", brief);
		Check(d.show && d.text == brief, "loader: no release_notes falls back to notes");
		d = DecideLoaderNotes("1.23.1", "v1.23.1", "1.22.1", " \n\r\t", brief);
		Check(d.show && d.text == brief, "loader: blank release_notes falls back to notes");
		Check(!DecideLoaderNotes("1.23.1", "v1.23.1", "", "", "").show && !DecideLoaderNotes("1.23.1", "v1.23.1", "", nullptr, nullptr).show, "loader: no text at all shows nothing");
		Check(DecideLoaderNotes("1.23.1-rc1", "v1.23.1", "1.22.1", hist, brief).show, "loader: an rc label matches the release");
		Check(DecideLoaderNotes("1.23.1-rc2", "v1.23.1", "1.23.0", hist, brief).show, "loader: another label on the release number matches");
		Check(!DecideLoaderNotes("1.23.0-rc1", "v1.23.1", "", hist, brief).show, "loader: an rc of the older release is not offered the newer notes");

		// Launches in sequence, as the loader runs them: at list time the previous version is
		// resolved and kept in LoaderNotesPrevious; LoaderNotesShown is written (and the
		// pending value cleared) only when the window opens, which needs the main menu.
		struct Ini { std::string shown, pending; };
		auto launch = [&](Ini& ini, const char* running, const char* tag, const char* releaseNotes,
		                  const char* curLog, const char* prevLog, bool reachesMenu) -> std::string
		{
			const std::string prev = ResolvePreviousVersion(ini.pending.c_str(), ini.shown.c_str(), curLog, prevLog, running);
			const LoaderNotesDecision dec = DecideLoaderNotes(running, tag, prev.c_str(), releaseNotes, brief);
			if (!dec.show)
				return {};
			ini.pending = prev;
			if (reachesMenu)
			{
				ini.shown = tag;
				ini.pending.clear();
			}
			return dec.text;
		};
		const std::string twoSections = "## v1.23.1\n- c1\n- c2\n## v1.23.0\n- b1\n- b2";
		const char* nextLogs = "Installed version (Core DLL file version): 1.23.1\n";

		// Quit before the main menu, then relaunch: the notes and the history are still there.
		{
			Ini ini;
			Check(launch(ini, "1.23.1", "v1.23.1", hist, updLog, "", false) == twoSections && ini.shown.empty() && ini.pending == "1.22.1",
				"lifecycle: a quit before the menu records nothing as shown and keeps the previous version");
			Check(launch(ini, "1.23.1", "v1.23.1", hist, "", nextLogs, true) == twoSections && ini.shown == "v1.23.1" && ini.pending.empty(),
				"lifecycle: the relaunch lists the same notes and records them when the window opens");
			Check(launch(ini, "1.23.1", "v1.23.1", hist, "", nextLogs, true).empty(), "lifecycle: shown, then relaunch shows nothing");
		}
		// Two quits before the menu.
		{
			Ini ini;
			launch(ini, "1.23.1", "v1.23.1", hist, updLog, "", false);
			Check(launch(ini, "1.23.1", "v1.23.1", hist, "", nextLogs, false) == twoSections && ini.pending == "1.22.1",
				"lifecycle: a second quit keeps the history");
			Check(launch(ini, "1.23.1", "v1.23.1", hist, "", nextLogs, true) == twoSections, "lifecycle: the third launch still lists it");
		}
		// A manual update (no update line, only last boot's installed line), then a quit.
		{
			Ini ini;
			const char* oldInst = "Installed version (Core DLL file version): 1.22.1\n";
			launch(ini, "1.23.1", "v1.23.1", hist, "", oldInst, false);
			Check(launch(ini, "1.23.1", "v1.23.1", hist, "", nextLogs, true) == twoSections, "lifecycle: a manual update keeps its history after a quit");
		}
		// An unknown previous version stays unknown and lists the running release only.
		{
			Ini ini;
			launch(ini, "1.23.1", "v1.23.1", hist, "", "", false);
			Check(ini.pending.empty() && launch(ini, "1.23.1", "v1.23.1", hist, "", "", true) == "- c1\n- c2", "lifecycle: an unknown previous version lists the running release only");
		}
		// A newer release arrives while one is still unshown: both are listed.
		{
			const char* histNext =
				"## v1.24.0\n- d1\n\n"
				"## v1.23.1\n- c1\n- c2\n\n"
				"## v1.23.0\n- b1\n- b2";
			Ini ini;
			launch(ini, "1.23.1", "v1.23.1", hist, updLog, "", false);
			Check(launch(ini, "1.24.0", "v1.24.0", histNext, "Update available: 1.23.1 -> v1.24.0\n", "", true) ==
				"## v1.24.0\n- d1\n## v1.23.1\n- c1\n- c2\n## v1.23.0\n- b1\n- b2", "lifecycle: a newer release lists the unshown one too");
			Check(ini.shown == "v1.24.0" && ini.pending.empty(), "lifecycle: and records the newer one");
			// The same release arriving after the first was shown lists only itself.
			Ini shown; shown.shown = "v1.23.1";
			Check(launch(shown, "1.24.0", "v1.24.0", histNext, "", "", true) == "- d1", "lifecycle: after a shown release only the newer one is listed");
		}
		// A crash between the two writes at open leaves a stale pending value: nothing shows again.
		{
			Ini ini; ini.shown = "v1.23.1"; ini.pending = "1.22.1";
			Check(launch(ini, "1.23.1", "v1.23.1", hist, "", nextLogs, true).empty(), "lifecycle: a stale pending value after a shown record shows nothing");
		}
	}

	// ---- Reading the version out of a DLL image ----------------------------
	{
		wchar_t self[MAX_PATH]{};
		GetModuleFileNameW(nullptr, self, MAX_PATH);
		Check(PluginDllVersion::Read(self) == "9.8.7-test", "the version a plugin image reports is read");
		Check(PluginDllVersion::Read(L"C:\\no\\such\\plugin.dll").empty(), "a missing file reads as no version");
		wchar_t kernel[MAX_PATH]{};
		GetSystemDirectoryW(kernel, MAX_PATH);
		wcscat_s(kernel, L"\\kernel32.dll");
		Check(PluginDllVersion::Read(kernel).empty(), "a DLL that is not a plugin reads as no version");
	}

	// ---- WhyNot -------------------------------------------------------------
	{
		FeedFetch::Config cfg;
		cfg.url = "https://example.invalid/version-map-client.v1.txt";
		cfg.keys = keys;
		cfg.flavor = VersionFeed::Flavor::Client;
		Check(FeedFetch::WhyNot(cfg) == nullptr, "WhyNot: a full client config may ask");
		FeedFetch::Config server = cfg;
		server.flavor = VersionFeed::Flavor::Server;
		server.url = "https://example.invalid/version-map-server.v1.txt";
		Check(FeedFetch::WhyNot(server) == nullptr, "WhyNot: a full server config may ask");
		FeedFetch::Config off = cfg;
		off.autoUpdate = false;
		Check(FeedFetch::WhyNot(off) != nullptr, "WhyNot: [AutoUpdate] Enabled off");
		FeedFetch::Config noUrl = cfg;
		noUrl.url.clear();
		Check(FeedFetch::WhyNot(noUrl) != nullptr, "WhyNot: no URL compiled in");
		FeedFetch::Config noKeys = cfg;
		noKeys.keys.clear();
		Check(FeedFetch::WhyNot(noKeys) != nullptr, "WhyNot: no public key");
	}

	Server srv;
	if (!srv.Start())
	{
		printf("FAIL: the local server did not start\n");
		return 1;
	}
	srv.feed = SignedFile(sk, 100);
	srv.etag = "\"v100\"";
	const std::wstring cache = TempPath(L"a.cache");
	DeleteFileW(cache.c_str());
	DeleteFileW(EtagFile(cache).c_str());

	// ---- Stage 3 asks only when no request went out yet this launch --------
	// (first, because the first request of the process is what Stage 3 looks for)
	{
		const std::wstring late = TempPath(L"stage3.cache");
		DeleteFileW(late.c_str());
		DeleteFileW(EtagFile(late).c_str());
		const FeedFetch::Config s3 = ConfigFor(srv, "/ok", keys, late);
		srv.hits = 0;
		FeedFetch::Stage3(s3);
		Check(srv.hits == 1 && Slurp(late) == srv.feed, "Stage 3 asks once when nothing asked yet this launch, and stores the feed");
		FeedFetch::Stage3(s3);
		Check(srv.hits == 1, "...and asks nothing once a request went out this launch");
		DeleteFileW(late.c_str());
		DeleteFileW(EtagFile(late).c_str());
	}

	// ---- the happy path ----------------------------------------------------
	{
		srv.hits = 0;
		Check(FeedFetch::Refresh(ConfigFor(srv, "/ok", keys, cache), "test") == FeedFetch::Result::Stored && srv.hits == 1
			&& Slurp(cache) == srv.feed, "a verified feed is stored in the cache, verbatim");
		Check(srv.lastIfNoneMatch.empty() && Slurp(EtagFile(cache)) == "100 \"v100\"",
			"with no cache nothing conditional is sent, and the answer's ETag is kept beside the cache with its seq");
		srv.served304 = 0;
		Check(FeedFetch::Refresh(ConfigFor(srv, "/ok", keys, cache), "test") == FeedFetch::Result::Unchanged
			&& srv.lastIfNoneMatch == "\"v100\"" && srv.served304 == 1,
			"the same seq again goes out with that ETag, comes back 304 and is unchanged");
		srv.served304 = 0;
		Check(FeedFetch::Refresh(ConfigFor(srv, "/redirect", keys, cache), "test") == FeedFetch::Result::Unchanged
			&& srv.lastIfNoneMatch == "\"v100\"" && srv.served304 == 1,
			"an HTTP-to-HTTP redirect is followed, and the If-None-Match goes through it to the 304");
		Check(HttpGet(srv.Url("/redirect").c_str(), "test", [] { HttpGetOptions o; o.allowPlainHttp = true; return o; }()) == srv.feed,
			"HttpGet returns the body after a redirect");
		HttpGetOptions strict;
		strict.allowPlainHttp = false;
		srv.hits = 0;
		Check(HttpGet(srv.Url("/ok").c_str(), "test", strict).empty() && srv.hits == 0, "an http:// URL is refused without the test switch, with no request");
	}

	// ---- a newer feed ------------------------------------------------------
	{
		srv.feed = SignedFile(sk, 150);
		srv.etag = "\"v150\"";
		srv.served304 = 0;
		TestDoubles::g_log.clear();
		Check(FeedFetch::Refresh(ConfigFor(srv, "/ok", keys, cache), "test") == FeedFetch::Result::Stored && srv.served304 == 0
			&& srv.lastIfNoneMatch == "\"v100\"" && Slurp(cache) == srv.feed,
			"a feed newer than the cache is fetched in full and stored, though the request carried the old ETag");
		Check(Slurp(EtagFile(cache)) == "150 \"v150\"", "...and its ETag replaces the old one");
		Check(Logged(L"[VersionFeed] Version object map seq 150 stored"), "...and is logged");
	}

	// ---- 404 ---------------------------------------------------------------
	{
		const std::string before = Slurp(cache);
		const std::string sideBefore = Slurp(EtagFile(cache));
		srv.hits = 0;
		Check(FeedFetch::Refresh(ConfigFor(srv, "/missing", keys, cache), "test") == FeedFetch::Result::Failed && srv.hits == 1
			&& Slurp(cache) == before && Slurp(EtagFile(cache)) == sideBefore, "a 404 fails and leaves the cache and its ETag as they were");
		const std::wstring none = TempPath(L"none.cache");
		DeleteFileW(none.c_str());
		Check(FeedFetch::Refresh(ConfigFor(srv, "/missing", keys, none), "test") == FeedFetch::Result::Failed && !Exists(none),
			"a 404 with no cache creates none");
	}

	// ---- stall ---------------------------------------------------------------
	{
		const std::string before = Slurp(cache);
		auto t = Clock::now();
		Check(FeedFetch::Refresh(ConfigFor(srv, "/hang", keys, cache), "test") == FeedFetch::Result::Failed, "a server that never answers fails");
		const long long hang = MsSince(t);
		Check(hang >= 1500 && hang < 5000, "...within about one operation timeout");
		t = Clock::now();
		Check(FeedFetch::Refresh(ConfigFor(srv, "/stall", keys, cache), "test") == FeedFetch::Result::Failed, "a body that never arrives fails");
		const long long stall = MsSince(t);
		Check(stall >= 1500 && stall < 5000, "...within about one operation timeout");
		Check(Slurp(cache) == before, "neither touched the cache");
		printf("  hang %lld ms, stall %lld ms\n", hang, stall);

		// A trickle keeps every read under the 2 s timeout, so only the wall-clock deadline ends it.
		HttpGetOptions o;
		o.allowPlainHttp = true;
		o.resolveMs = o.connectMs = o.sendMs = o.receiveMs = FeedFetch::kOperationMs;
		o.deadlineMs = 1500;
		t = Clock::now();
		const std::string got = HttpGet(srv.Url("/trickle").c_str(), "test", o);
		const long long trickle = MsSince(t);
		Check(got.empty() && trickle >= 1400 && trickle < 3500, "a trickling body is cut off at the deadline");
		printf("  trickle %lld ms (deadline 1500)\n", trickle);
	}

	// ---- oversize ------------------------------------------------------------
	{
		const std::string before = Slurp(cache);
		Check(FeedFetch::Refresh(ConfigFor(srv, "/big", keys, cache), "test") == FeedFetch::Result::Failed && Slurp(cache) == before,
			"a body over 1 MiB fails and leaves the cache as it was");
		HttpGetOptions o;
		o.allowPlainHttp = true;
		o.maxBytes = srv.feed.size() - 1;
		Check(HttpGet(srv.Url("/ok").c_str(), "test", o).empty(), "HttpGet stops at its own byte limit");
		o.maxBytes = srv.feed.size();
		Check(HttpGet(srv.Url("/ok").c_str(), "test", o) == srv.feed, "...and passes a body exactly at the limit");
	}

	// ---- a bad signature -----------------------------------------------------
	{
		const std::string before = Slurp(cache);
		srv.etag.clear();
		srv.feed = SignedFile(sk, 200);
		srv.feed[40] ^= 1;
		Check(FeedFetch::Refresh(ConfigFor(srv, "/ok", keys, cache), "test") == FeedFetch::Result::Failed && Slurp(cache) == before
			&& Slurp(EtagFile(cache)) == "150 \"v150\"", "a fetched file that fails its signature is not stored, and its ETag is not kept");
		srv.feed = SignedFile(sk, 200);
		srv.etag = "\"v200\"";
		Check(FeedFetch::Refresh(ConfigFor(srv, "/ok", keys, cache), "test") == FeedFetch::Result::Stored && Slurp(cache) == srv.feed,
			"a higher seq replaces the cache");
	}

	// ---- the plugin catalog: one GET, verified, cached ---------------------------
	{
		const std::wstring catCache = TempPath(L"catalog.cache");
		const auto Served = [] { const auto t = PluginCatalog::AccessorText(); return t ? *t : std::string(); };
		DeleteFileW(catCache.c_str());
		FeedFetch::Config cfg = ConfigFor(srv, "/ok", keys, cache);
		cfg.catalogUrl = srv.Url("/catalog");
		cfg.catalogCachePath = catCache;

		srv.hits = 0;
		srv.catalog = SignedCatalog(sk, 5, "1.0.0");
		Check(FeedFetch::RefreshCatalog(cfg) == FeedFetch::Result::Stored && srv.hits == 1 && Slurp(catCache) == srv.catalog,
			"catalog: a verified catalog is stored in its cache, verbatim");
		Check(Served().find("seq\t5\n") != std::string::npos
			&& Served().find("plugin\tAlpha\t1.0.0\n") != std::string::npos, "catalog: the accessor serves it at once");
		Check(srv.lastIfNoneMatch.empty(), "catalog: no ETag is sent");

		Check(FeedFetch::RefreshCatalog(cfg) == FeedFetch::Result::Unchanged && Slurp(catCache) == srv.catalog,
			"catalog: the same file again is unchanged");

		const std::string at5 = srv.catalog;
		srv.catalog = SignedCatalog(sk, 4, "0.9.0");
		Check(FeedFetch::RefreshCatalog(cfg) == FeedFetch::Result::Unchanged && Slurp(catCache) == at5,
			"catalog: a lower seq leaves the cache as it was");
		srv.catalog = SignedCatalog(sk, 6, "1.1.0");
		srv.catalog[40] ^= 1;
		Check(FeedFetch::RefreshCatalog(cfg) == FeedFetch::Result::Failed && Slurp(catCache) == at5,
			"catalog: a file that fails its signature leaves the cache as it was");

		cfg.catalogUrl = srv.Url("/missing");
		Check(FeedFetch::RefreshCatalog(cfg) == FeedFetch::Result::Failed && Slurp(catCache) == at5,
			"catalog: a 404 leaves the cache as it was");
		cfg.catalogUrl = srv.Url("/catalog");

		srv.catalog = SignedCatalog(sk, 7, "1.2.0");
		Check(FeedFetch::RefreshCatalog(cfg) == FeedFetch::Result::Stored && Slurp(catCache) == srv.catalog
			&& Served().find("plugin\tAlpha\t1.2.0\n") != std::string::npos, "catalog: a higher seq replaces the cache");

		srv.hits = 0;
		FeedFetch::Config off = cfg;
		off.autoUpdate = false;
		FeedFetch::Config noUrl = cfg;
		noUrl.catalogUrl.clear();
		FeedFetch::Config noKeys = cfg;
		noKeys.keys.clear();
		Check(FeedFetch::RefreshCatalog(off) == FeedFetch::Result::NotAllowed && FeedFetch::RefreshCatalog(noUrl) == FeedFetch::Result::NotAllowed
			&& FeedFetch::RefreshCatalog(noKeys) == FeedFetch::Result::NotAllowed && srv.hits == 0,
			"catalog: updates off, no URL or no key makes no request");
		DeleteFileW(catCache.c_str());
	}

	// ---- no request ------------------------------------------------------------
	{
		srv.hits = 0;
		FeedFetch::Config server = ConfigFor(srv, "/ok", keys, cache, VersionFeed::Flavor::Server);
		server.url.clear();   // a server build with no VMAP_SERVER_FEED_URL compiled in
		FeedFetch::Config off = ConfigFor(srv, "/ok", keys, cache);
		off.autoUpdate = false;
		FeedFetch::Config noUrl = ConfigFor(srv, "/ok", keys, cache);
		noUrl.url.clear();
		FeedFetch::Config noKeys = ConfigFor(srv, "/ok", keys, cache);
		noKeys.keys.clear();
		Check(FeedFetch::Refresh(server, "test") == FeedFetch::Result::NotAllowed && FeedFetch::WhyNot(server) != nullptr,
			"a server build with no feed URL makes no request");
		Check(FeedFetch::Refresh(off, "test") == FeedFetch::Result::NotAllowed, "[AutoUpdate] Enabled off makes no request");
		Check(FeedFetch::Refresh(noUrl, "test") == FeedFetch::Result::NotAllowed, "no URL makes no request");
		Check(FeedFetch::Refresh(noKeys, "test") == FeedFetch::Result::NotAllowed, "no key makes no request");
		FeedFetch::AfterRefusal(server, false);
		Check(!FeedFetch::Stage1(off, true), "Stage 1 with updates off makes no request");
		Check(srv.hits == 0, "none of those reached the server");
	}

	// ---- the ETag is sent only when it belongs to the cache ---------------------
	{
		const std::wstring side = EtagFile(cache);
		const auto writeSide = [&](const std::string& text)
		{
			HANDLE h = CreateFileW(side.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			DWORD n = 0;
			WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
			CloseHandle(h);
		};
		const FeedFetch::Config cfg = ConfigFor(srv, "/ok", keys, cache);

		// The cache is seq 200 and the server still has seq 200: whatever the sidecar says, nothing changes.
		writeSide("100 \"v200\"");
		srv.served304 = 0;
		Check(FeedFetch::Refresh(cfg, "test") == FeedFetch::Result::Unchanged && srv.lastIfNoneMatch.empty() && srv.served304 == 0,
			"an ETag kept for another seq than the cache's is not sent: the whole feed comes back, no newer");
		Check(Slurp(side) == "200 \"v200\"", "...and the ETag is kept again, for the cache's seq");

		writeSide("200 a\x01");
		Check(FeedFetch::Refresh(cfg, "test") == FeedFetch::Result::Unchanged && srv.lastIfNoneMatch.empty(),
			"an ETag with a control character is not sent");
		DeleteFileW(side.c_str());
		Check(FeedFetch::Refresh(cfg, "test") == FeedFetch::Result::Unchanged && srv.lastIfNoneMatch.empty() && Slurp(side) == "200 \"v200\"",
			"with no ETag on file the whole feed is asked for, and its ETag is kept");
		Check(FeedFetch::Refresh(cfg, "test") == FeedFetch::Result::Unchanged && srv.lastIfNoneMatch == "\"v200\"" && srv.served304 == 1,
			"...so that the next start gets its 304");

		// A server that sends no ETag: the whole feed each start, nothing kept, the cache works as before.
		srv.etag.clear();
		DeleteFileW(side.c_str());
		Check(FeedFetch::Refresh(cfg, "test") == FeedFetch::Result::Unchanged && !Exists(side), "a server with no ETag leaves none on file");
		srv.etag = "\"v200\"";
		FeedFetch::Refresh(cfg, "test");   // keeps the ETag again for what follows
	}

	// ---- the stages ----------------------------------------------------------
	{
		const FeedFetch::Config cfg = ConfigFor(srv, "/ok", keys, cache);   // the cache holds seq 200 and its ETag

		// Stage 1 asks at every start, with the cache of any age; a feed no newer than the cache is a 304.
		srv.hits = 0;
		srv.served304 = 0;
		TestDoubles::g_log.clear();
		Check(FeedFetch::Stage1(cfg, true) && srv.hits == 1 && srv.served304 == 1 && Slurp(cache) == srv.feed,
			"Stage 1 asks, gets a 304 for a feed no newer than the cache, and changes nothing");
		Check(!Logged(L"Newer rows"), "...and says nothing of new rows");
		FeedFetch::Stage1(cfg, true);
		Check(srv.hits == 2, "...and asks again at the next start, however soon");

		srv.feed = SignedFile(sk, 300);
		srv.etag = "\"v300\"";
		TestDoubles::g_log.clear();
		Check(FeedFetch::Stage1(cfg, true) && Slurp(cache) == srv.feed && Slurp(EtagFile(cache)) == "300 \"v300\"",
			"Stage 1 stores a newer feed, so this very start applies it");
		Check(Logged(L"[VersionFeed] Newer rows for the version object map were fetched and are read on this start"),
			"...and logs one plain line for it");

		// Offline, 404, a bad signature: the game starts and the cache is as it was.
		const std::string before = Slurp(cache);
		const std::string sideBefore = Slurp(EtagFile(cache));
		TestDoubles::g_log.clear();
		Check(FeedFetch::Stage1(ConfigFor(srv, "/missing", keys, cache), true) && Slurp(cache) == before && Slurp(EtagFile(cache)) == sideBefore
			&& !Logged(L"Newer rows"), "Stage 1 on a 404 starts normally and leaves the cache and its ETag as they were");

		srv.hits = 0;
		FeedFetch::AfterRefusal(cfg, true);
		Check(srv.hits == 0, "a refusal after Stage 1 asked does not ask again");
		FeedFetch::AfterRefusal(cfg, false);
		Check(srv.hits == 1, "a refusal that did not fetch asks once");

		// The test exe is in neither the fixture map nor this cache, so the splash is told.
		const std::wstring none = TempPath(L"stage1.cache");
		DeleteFileW(none.c_str());
		DeleteFileW(EtagFile(none).c_str());
		const FeedFetch::Config s1 = ConfigFor(srv, "/ok", keys, none);
		srv.hits = 0;
		Check(!FeedFetch::Stage1(s1, false) && srv.hits == 0, "Stage 1 with the main thread not parked makes no request");
		static uint32_t announced = 0;
		Check(FeedFetch::Stage1(s1, true, [](uint32_t cl) { announced = cl + 1; }) && srv.hits == 1 && announced != 0 && !Slurp(none).empty(),
			"Stage 1 on an unknown exe, main parked: announces, asks once, stores");
		DeleteFileW(none.c_str());
		DeleteFileW(EtagFile(none).c_str());
	}

	// ---- the dedicated server's file ------------------------------------------
	// The same fetch, each build with its own file: a server loader stores a
	// server file in its own cache, and neither build takes the other's.
	{
		const std::wstring serverCache = TempPath(L"server.cache");
		DeleteFileW(serverCache.c_str());
		DeleteFileW(EtagFile(serverCache).c_str());
		const std::string serverFile = ServerSignedFile(sk, 500);
		const FeedFetch::Config server = ConfigFor(srv, "/ok", keys, serverCache, VersionFeed::Flavor::Server);

		// A server file reaches a server loader's cache, with its ETag, and is read back as a server file.
		srv.feed = serverFile;
		srv.etag = "\"s500\"";
		srv.hits = 0;
		TestDoubles::g_log.clear();
		Check(FeedFetch::Refresh(server, "test") == FeedFetch::Result::Stored && srv.hits == 1 && Slurp(serverCache) == serverFile
			&& Slurp(EtagFile(serverCache)) == "500 \"s500\"", "server: a signed server file is fetched, stored verbatim in the server cache, with its ETag");
		Check(Logged(L"[VersionFeed] Version object map seq 500 stored"), "server: ...and logged like the client's");
		SignedFeed read;
		Check(VersionFeed::ReadCache(serverCache, keys, read, VersionFeed::Flavor::Server) && read.seq == 500 && read.map.builds.size() == 2,
			"server: the cache reads back as a server file");
		Check(!VersionFeed::ReadCache(serverCache, keys, read, VersionFeed::Flavor::Client),
			"server: ...and a client loader does not read it");
		srv.served304 = 0;
		Check(FeedFetch::Refresh(server, "test") == FeedFetch::Result::Unchanged && srv.lastIfNoneMatch == "\"s500\"" && srv.served304 == 1,
			"server: the next start asks with the ETag and gets a 304");

		// A client file on the server's URL is refused whole, and the cache and its ETag stay as they were.
		srv.feed = SignedFile(sk, 600);
		srv.etag = "\"c600\"";
		const std::string before = Slurp(serverCache);
		TestDoubles::g_log.clear();
		Check(FeedFetch::Refresh(server, "test") == FeedFetch::Result::Failed && Slurp(serverCache) == before && Slurp(EtagFile(serverCache)) == "500 \"s500\"",
			"server: a client file (a higher seq) is refused and the server cache is as it was");
		Check(Logged(L"not a dedicated server file"), "server: ...and the log says it is not a server file");

		// A server file on the client's URL, the other way round.
		const std::wstring clientCache = TempPath(L"clientside.cache");
		DeleteFileW(clientCache.c_str());
		DeleteFileW(EtagFile(clientCache).c_str());
		srv.feed = serverFile;
		srv.etag = "\"s500\"";
		TestDoubles::g_log.clear();
		Check(FeedFetch::Refresh(ConfigFor(srv, "/ok", keys, clientCache), "test") == FeedFetch::Result::Failed && !Exists(clientCache) && !Exists(EtagFile(clientCache)),
			"client: a server file is refused and nothing is stored");
		Check(Logged(L"which a client loader does not read"), "client: ...and the log says so");

		// Stage 1 on a server: asks once with the main thread parked, never otherwise, and has no splash to tell.
		srv.hits = 0;
		Check(!FeedFetch::Stage1(server, false) && srv.hits == 0, "server: Stage 1 with the main thread not parked makes no request");
		srv.feed = ServerSignedFile(sk, 700);
		srv.etag = "\"s700\"";
		Check(FeedFetch::Stage1(server, true) && srv.hits == 1 && Slurp(serverCache) == srv.feed, "server: Stage 1 with the main thread parked asks once and stores a newer server file");
		DeleteFileW(serverCache.c_str());
		DeleteFileW(EtagFile(serverCache).c_str());
		DeleteFileW(clientCache.c_str());
		srv.feed = SignedFile(sk, 300);
		srv.etag = "\"v300\"";
	}

	// ---- an unroutable host ----------------------------------------------------
	{
		const std::wstring none = TempPath(L"unroutable.cache");
		DeleteFileW(none.c_str());
		FeedFetch::Config cfg = ConfigFor(srv, "/ok", keys, none);
		cfg.url = "http://192.0.2.1:81/version-map-client.v1.txt";   // TEST-NET-1: nothing answers
		const auto t = Clock::now();
		const auto r = FeedFetch::Refresh(cfg, "test");
		const long long ms = MsSince(t);
		Check(r == FeedFetch::Result::Failed && Slurp(none).empty() && ms < FeedFetch::kDeadlineMs + FeedFetch::kOperationMs, "an unroutable host fails within the deadline plus one timeout");
		printf("  unroutable %lld ms\n", ms);
	}

	srv.Stop();
	DeleteFileW(cache.c_str());
	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
