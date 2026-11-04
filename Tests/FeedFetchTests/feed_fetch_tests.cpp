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

	// Sets the cache file's last-write time to `secondsAgo` before now.
	void AgeFile(const std::wstring& p, uint64_t secondsAgo)
	{
		HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return;
		FILETIME now;
		GetSystemTimeAsFileTime(&now);
		ULARGE_INTEGER t;
		t.LowPart = now.dwLowDateTime;
		t.HighPart = now.dwHighDateTime;
		t.QuadPart -= secondsAgo * 10000000ull;
		FILETIME old{ t.LowPart, t.HighPart };
		SetFileTime(h, nullptr, nullptr, &old);
		CloseHandle(h);
	}

	// ---- the local server ---------------------------------------------------
	//
	// Routes:
	//   /ok        200 with the current feed
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
		std::string feed;

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
			++hits;
			const size_t a = req.find(' ') + 1;
			const std::string path = req.substr(a, req.find(' ', a) - a);

			if (path == "/ok")
			{
				const std::string h = Head(200, "OK", feed.size());
				SendAll(c, h.data(), h.size());
				SendAll(c, feed.data(), feed.size());
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

	FeedFetch::Config ConfigFor(const Server& srv, const char* route, const std::vector<PublicKey>& keys, const std::wstring& cache)
	{
		FeedFetch::Config cfg;
		cfg.url = srv.Url(route);
		cfg.keys = keys;
		cfg.cachePath = cache;
		cfg.client = true;
		cfg.autoUpdate = true;
		cfg.allowPlainHttp = true;
		return cfg;
	}

	// ---- Stage 1's trigger ---------------------------------------------------

	const Exe kKnown{ 127004, 0x11111111, 0x01000000 };

	MapData Baseline(bool withAbsent, const char* absentKey = "Test::A", uint32_t absentFp = 0)
	{
		MapData m;
		m.builds.push_back({ kKnown.cl, kKnown.imageStamp, kKnown.imageSize, "public 1" });
		if (withAbsent)
		{
			VersionMap::MapFunctionRow row;
			row.key = absentKey;
			row.gameCL = kKnown.cl;
			row.imageStamp = kKnown.imageStamp;
			row.imageSize = kKnown.imageSize;
			row.kind = 1;
			row.fp = absentFp;
			row.absent = true;
			m.functions.push_back(row);
		}
		return m;
	}

	SignedFeed CacheWithLocator(const char* key, uint32_t fp)
	{
		SignedFeed f;
		f.seq = 5;
		f.map = Baseline(false);
		VersionMap::MapFunctionRow row;
		row.key = key;
		row.gameCL = kKnown.cl;
		row.imageStamp = kKnown.imageStamp;
		row.imageSize = kKnown.imageSize;
		row.kind = 1;
		row.fp = fp;
		row.pattern = "AA 01";
		row.rva = 0x1000;
		f.map.functions.push_back(row);
		return f;
	}
}

int main()
{
	const Signer sk = MakeSigner("k1", 1);
	const std::vector<PublicKey> keys{ sk.pub };
	const uint64_t kDay = FeedFetch::kDaySeconds;

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

	// ---- Stage 1's trigger: the running exe and the cache's age ------------
	{
		const VersionFeed::Coverage none;
		const SignedFeed filled = CacheWithLocator("Test::A", 0);
		const Exe unknown{ 999999, 0x33333333, 0x03000000 };
		const uint64_t never = UINT64_MAX;

		Check(FeedFetch::Stage1Wanted(Baseline(false), nullptr, unknown, none, 0), "Stage1Wanted: an exe in neither the map nor the cache");
		SignedFeed holdsUnknown;
		holdsUnknown.map.builds.push_back({ unknown.cl, unknown.imageStamp, unknown.imageSize, "x" });
		Check(!FeedFetch::Stage1Wanted(Baseline(false), &holdsUnknown, unknown, none, 0), "Stage1Wanted: an exe only the cache holds");
		Check(!FeedFetch::Stage1Wanted(Baseline(false), nullptr, kKnown, none, never), "Stage1Wanted: a known exe with no absent row never asks");
		Check(FeedFetch::Stage1Wanted(Baseline(true), nullptr, kKnown, none, never), "Stage1Wanted: an absent row and no cache");
		Check(FeedFetch::Stage1Wanted(Baseline(true), nullptr, kKnown, none, kDay + 1), "Stage1Wanted: an absent row and a cache over a day old");
		Check(!FeedFetch::Stage1Wanted(Baseline(true), nullptr, kKnown, none, kDay - 1), "Stage1Wanted: an absent row and a cache under a day old");
		Check(!FeedFetch::Stage1Wanted(Baseline(true), &filled, kKnown, none, never), "Stage1Wanted: the cache fills the absent row");
		const SignedFeed otherKey = CacheWithLocator("Test::B", 0);
		Check(FeedFetch::Stage1Wanted(Baseline(true), &otherKey, kKnown, none, never), "Stage1Wanted: the cache fills a different key");

		VersionFeed::Coverage cov;
		cov.functions.push_back({ "ModLoader/Hook", 0x1234 });
		Check(FeedFetch::Stage1Wanted(Baseline(true, "ModLoader/Hook", 0x1234), nullptr, kKnown, cov, never),
			"Stage1Wanted: a ModLoader/ absent row under this loader's own fp");
		Check(!FeedFetch::Stage1Wanted(Baseline(true, "ModLoader/Hook", 0x9999), nullptr, kKnown, cov, never),
			"Stage1Wanted: a ModLoader/ absent row under another fp cannot bind");
	}

	// ---- WhyNot -------------------------------------------------------------
	{
		FeedFetch::Config cfg;
		cfg.url = "https://example.invalid/version-map-client.v1.txt";
		cfg.keys = keys;
		cfg.client = true;
		Check(FeedFetch::WhyNot(cfg) == nullptr, "WhyNot: a full client config may ask");
		FeedFetch::Config server = cfg;
		server.client = false;
		Check(FeedFetch::WhyNot(server) != nullptr, "WhyNot: the server never asks");
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
	const std::wstring cache = TempPath(L"a.cache");
	DeleteFileW(cache.c_str());

	// ---- the happy path ----------------------------------------------------
	{
		srv.hits = 0;
		Check(FeedFetch::Refresh(ConfigFor(srv, "/ok", keys, cache), "test") == FeedFetch::Result::Stored && srv.hits == 1
			&& Slurp(cache) == srv.feed, "a verified feed is stored in the cache, verbatim");
		Check(FeedFetch::Refresh(ConfigFor(srv, "/ok", keys, cache), "test") == FeedFetch::Result::Unchanged,
			"the same seq again is unchanged");
		Check(FeedFetch::Refresh(ConfigFor(srv, "/redirect", keys, cache), "test") == FeedFetch::Result::Unchanged,
			"an HTTP-to-HTTP redirect is followed");
		Check(HttpGet(srv.Url("/redirect").c_str(), "test", [] { HttpGetOptions o; o.allowPlainHttp = true; return o; }()) == srv.feed,
			"HttpGet returns the body after a redirect");
		HttpGetOptions strict;
		strict.allowPlainHttp = false;
		srv.hits = 0;
		Check(HttpGet(srv.Url("/ok").c_str(), "test", strict).empty() && srv.hits == 0, "an http:// URL is refused without the test switch, with no request");
	}

	// ---- 404 ---------------------------------------------------------------
	{
		const std::string before = Slurp(cache);
		srv.hits = 0;
		Check(FeedFetch::Refresh(ConfigFor(srv, "/missing", keys, cache), "test") == FeedFetch::Result::Failed && srv.hits == 1
			&& Slurp(cache) == before, "a 404 fails and leaves the cache as it was");
		const std::wstring none = TempPath(L"none.cache");
		DeleteFileW(none.c_str());
		Check(FeedFetch::Refresh(ConfigFor(srv, "/missing", keys, none), "test") == FeedFetch::Result::Failed && Exists(none)
			&& Slurp(none).empty() && FeedFetch::CacheAgeSeconds(none) < 60,
			"a 404 with no cache leaves an empty cache, so the next request waits a day");
		DeleteFileW(none.c_str());
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
		srv.feed[40] ^= 1;
		Check(FeedFetch::Refresh(ConfigFor(srv, "/ok", keys, cache), "test") == FeedFetch::Result::Failed && Slurp(cache) == before,
			"a fetched file that fails its signature is not stored");
		srv.feed = SignedFile(sk, 200);
		Check(FeedFetch::Refresh(ConfigFor(srv, "/ok", keys, cache), "test") == FeedFetch::Result::Stored && Slurp(cache) == srv.feed,
			"a higher seq replaces the cache");
	}

	// ---- no request ------------------------------------------------------------
	{
		srv.hits = 0;
		FeedFetch::Config server = ConfigFor(srv, "/ok", keys, cache);
		server.client = false;
		FeedFetch::Config off = ConfigFor(srv, "/ok", keys, cache);
		off.autoUpdate = false;
		FeedFetch::Config noUrl = ConfigFor(srv, "/ok", keys, cache);
		noUrl.url.clear();
		FeedFetch::Config noKeys = ConfigFor(srv, "/ok", keys, cache);
		noKeys.keys.clear();
		Check(FeedFetch::Refresh(server, "test") == FeedFetch::Result::NotAllowed, "the server build makes no request");
		Check(FeedFetch::Refresh(off, "test") == FeedFetch::Result::NotAllowed, "[AutoUpdate] Enabled off makes no request");
		Check(FeedFetch::Refresh(noUrl, "test") == FeedFetch::Result::NotAllowed, "no URL makes no request");
		Check(FeedFetch::Refresh(noKeys, "test") == FeedFetch::Result::NotAllowed, "no key makes no request");
		FeedFetch::Stage3(off);
		FeedFetch::AfterRefusal(server, false);
		Check(!FeedFetch::Stage1(off, true), "Stage 1 with updates off makes no request");
		Check(srv.hits == 0, "none of those reached the server");
	}

	// ---- the stages ----------------------------------------------------------
	{
		const FeedFetch::Config cfg = ConfigFor(srv, "/ok", keys, cache);

		srv.hits = 0;
		AgeFile(cache, 3600);
		FeedFetch::Stage3(cfg);
		Check(srv.hits == 0, "Stage 3 asks nothing of a cache under a day old");
		AgeFile(cache, kDay + 3600);
		FeedFetch::Stage3(cfg);
		Check(srv.hits == 1 && FeedFetch::CacheAgeSeconds(cache) < 60, "Stage 3 asks once a cache is over a day old, and the check restarts the clock");

		srv.hits = 0;
		FeedFetch::AfterRefusal(cfg, true);
		Check(srv.hits == 0, "a refusal after Stage 1 asked does not ask again");
		FeedFetch::AfterRefusal(cfg, false);
		Check(srv.hits == 1, "a refusal that did not fetch asks once");

		// The test exe is in neither the fixture map nor this cache, so Stage 1 wants a feed.
		const std::wstring none = TempPath(L"stage1.cache");
		DeleteFileW(none.c_str());
		const FeedFetch::Config s1 = ConfigFor(srv, "/ok", keys, none);
		srv.hits = 0;
		Check(!FeedFetch::Stage1(s1, false) && srv.hits == 0, "Stage 1 with the main thread not parked makes no request");
		static uint32_t announced = 0;
		Check(FeedFetch::Stage1(s1, true, [](uint32_t cl) { announced = cl + 1; }) && srv.hits == 1 && announced != 0 && !Slurp(none).empty(),
			"Stage 1 on an unknown exe, main parked: announces, asks once, stores");
		DeleteFileW(none.c_str());
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
