// ModIndexTests: the mod index parser (auto_update/mod_index.h), its URL
// allowlist, IsInstalled, and the Fetcher's states (auto_update/mod_index_fetcher.cpp)
// through an injected request. No network, no game.

#include "auto_update/http_get.h"
#include "auto_update/mod_index.h"
#include "auto_update/mod_index_fetcher.h"
#include "auto_update/feed_fetch.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <Windows.h>

// ---- HttpGet stands in for the real one: the Fetcher's default request lands
// here, so the options it passes can be read back. ---------------------------
namespace
{
	std::mutex g_httpMutex;
	std::string g_httpUrl;
	HttpGetOptions g_httpOptions;
	std::string g_httpAnswer;
	int g_httpCalls = 0;
}

std::string HttpGet(const char* url, const char* /*label*/, const HttpGetOptions& options)
{
	std::lock_guard<std::mutex> lock(g_httpMutex);
	++g_httpCalls;
	g_httpUrl = url;
	g_httpOptions = options;
	return g_httpAnswer;
}

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

	using namespace ModIndex;
	using Clock = std::chrono::steady_clock;

	std::string Obj(const char* id, const char* name, const char* url, const char* extra = "")
	{
		std::string s = "{\"id\": \"";
		s += id;
		s += "\", \"name\": \"";
		s += name;
		s += "\", \"page_url\": \"";
		s += url;
		s += "\"";
		s += extra;
		s += "}";
		return s;
	}

	const char* kGood = "https://github.com/AlienXAXS/StarRupture-Plugin-Codex";

	// Waits for a state other than Loading (or the wanted one), up to 5 s.
	bool WaitFor(Fetcher& f, State want)
	{
		const auto end = Clock::now() + std::chrono::seconds(5);
		while (Clock::now() < end)
		{
			if (f.Get()->state == want)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return false;
	}

	std::string ReadFile(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	void ParserTests()
	{
		// good parse, every field
		{
			const std::string text = "[" + Obj("codex", "Codex", kGood,
				", \"author\": \"AlienXAXS\", \"version\": \"1.2.0\", \"summary\": \"Recipes.\\nSearch.\", \"interface_version_min\": 77") + "]";
			const ParseResult r = ParseModIndex(text);
			Check(r.ok && r.entries.size() == 1 && r.skipped == 0 && !r.truncated, "good: one entry, none skipped");
			if (r.entries.size() == 1)
			{
				const Entry& e = r.entries[0];
				Check(e.id == "codex" && e.name == "Codex" && e.author == "AlienXAXS" && e.version == "1.2.0", "good: text fields");
				Check(e.pageUrl == kGood && e.interfaceVersionMin == 77, "good: url and interface number");
				Check(e.summary == "Recipes. Search.", "good: an escaped newline in the summary becomes a space");
			}
		}

		// missing optional fields
		{
			const ParseResult r = ParseModIndex("[" + Obj("a", "A", kGood) + "]");
			Check(r.ok && r.entries.size() == 1, "optional: entry kept");
			if (!r.entries.empty())
			{
				const Entry& e = r.entries[0];
				Check(e.author.empty() && e.version.empty() && e.summary.empty() && e.interfaceVersionMin == -1, "optional: absent fields are empty, interface -1");
			}
		}

		// bad objects skipped and counted, good ones kept in order
		{
			const std::string text = "[" + Obj("a", "A", kGood) + ", {\"name\": \"no id\", \"page_url\": \"" + kGood + "\"}, " +
				"{\"id\": \"x\", \"page_url\": \"" + kGood + "\"}, {\"id\": \"y\", \"name\": \"Y\"}, " +
				Obj("bad", "Bad", "http://github.com/x") + ", 42, \"text\", " + Obj("b", "B", kGood) + "]";
			const ParseResult r = ParseModIndex(text);
			Check(r.ok && r.entries.size() == 2 && r.entries[0].id == "a" && r.entries[1].id == "b", "skip: the good entries stay, in order");
			Check(r.skipped == 4, "skip: missing id, name, page_url and a bad url are counted (non-objects are not entries)");
		}

		// empty array, non-array, malformed
		{
			const ParseResult e = ParseModIndex("[]");
			Check(e.ok && e.entries.empty() && e.skipped == 0, "empty array: ok, no entries");
			const ParseResult e2 = ParseModIndex("  \r\n[ \n ]\n ");
			Check(e2.ok && e2.entries.empty(), "empty array with whitespace");
			const ParseResult bom = ParseModIndex("\xEF\xBB\xBF[" + Obj("a", "A", kGood) + "]");
			Check(bom.ok && bom.entries.size() == 1, "a UTF-8 byte order mark is skipped");

			const ParseResult o = ParseModIndex("{\"id\": \"a\"}");
			Check(!o.ok && !o.error.empty() && o.entries.empty(), "non-array: an object fails with a message");
			Check(!ParseModIndex("\"text\"").ok && !ParseModIndex("42").ok && !ParseModIndex("").ok && !ParseModIndex("   ").ok, "non-array: string, number, empty text fail");
			Check(!ParseModIndex("[" + Obj("a", "A", kGood)).ok, "malformed: missing closing bracket fails");
			Check(!ParseModIndex("not json at all").ok, "malformed: text fails");
			Check(!ParseModIndex("<html><body>404</body></html>").ok, "malformed: an HTML error page fails");
			const ParseResult cut = ParseModIndex("[{\"id\": \"a\", \"name\": \"A\", \"page_url\": \"" + std::string(kGood));
			Check(!cut.ok, "malformed: cut off inside a string fails");
			const ParseResult unterminated = ParseModIndex("[{\"id\": \"a\", \"name\": \"A\", \"summary\": \"never ends]");
			Check(unterminated.ok ? unterminated.entries.empty() : true, "malformed: an unterminated string yields no entry");
		}

		// oversize
		{
			const std::string big(kMaxIndexBytes + 1, ' ');
			const ParseResult r = ParseModIndex("[" + big + "]");
			Check(!r.ok && !r.error.empty(), "oversize: more than the byte cap fails");
			std::string atCap = "[";
			atCap += std::string(kMaxIndexBytes - 2, ' ');
			atCap += "]";
			Check(atCap.size() == kMaxIndexBytes && ParseModIndex(atCap).ok, "oversize: exactly the cap is read");
		}

		// entry cap
		{
			std::string text = "[";
			for (size_t i = 0; i < kMaxEntries + 5; ++i)
			{
				if (i)
					text += ",";
				text += Obj(("p" + std::to_string(i)).c_str(), "P", kGood);
			}
			text += "]";
			const ParseResult r = ParseModIndex(text);
			Check(r.ok && r.entries.size() == kMaxEntries && r.truncated, "entry cap: the list is cut at kMaxEntries and flagged");

			std::string exact = "[";
			for (size_t i = 0; i < kMaxEntries; ++i)
			{
				if (i)
					exact += ",";
				exact += Obj(("p" + std::to_string(i)).c_str(), "P", kGood);
			}
			exact += "]";
			const ParseResult r2 = ParseModIndex(exact);
			Check(r2.ok && r2.entries.size() == kMaxEntries && !r2.truncated, "entry cap: exactly kMaxEntries is not truncated");
		}

		// field caps and cleaning
		{
			const std::string longName(kMaxNameLen + 20, 'n');
			const std::string longSummary(kMaxSummaryLen + 50, 's');
			const std::string text = "[" + Obj("i", longName.c_str(), kGood, (", \"summary\": \"" + longSummary + "\", \"author\": \"" + std::string(100, 'a') + "\", \"version\": \"" + std::string(100, 'v') + "\"").c_str()) + "]";
			const ParseResult r = ParseModIndex(text);
			Check(r.ok && r.entries.size() == 1, "caps: entry kept");
			if (!r.entries.empty())
			{
				const Entry& e = r.entries[0];
				Check(e.name.size() == kMaxNameLen && e.summary.size() == kMaxSummaryLen, "caps: name and summary are cut");
				Check(e.author.size() == kMaxAuthorLen && e.version.size() == kMaxVersionLen, "caps: author and version are cut");
			}

			const std::string longId(kMaxIdLen + 1, 'x');
			const ParseResult r2 = ParseModIndex("[" + Obj(longId.c_str(), "N", kGood) + "]");
			Check(r2.ok && r2.entries.size() == 1 && r2.entries[0].id.size() == kMaxIdLen, "caps: id is cut");

			// a cut never ends inside a UTF-8 sequence
			std::string utf8name(kMaxNameLen - 1, 'a');
			utf8name += "\xC3\xA9\xC3\xA9";   // two 2-byte characters across the limit
			const ParseResult r3 = ParseModIndex("[" + Obj("u", utf8name.c_str(), kGood) + "]");
			Check(r3.ok && r3.entries.size() == 1 && r3.entries[0].name.size() == kMaxNameLen - 1, "caps: a cut does not split a UTF-8 character");

			// URL: too long is dropped, never cut
			const std::string longUrl = "https://github.com/" + std::string(kMaxUrlLen, 'a');
			const ParseResult r4 = ParseModIndex("[" + Obj("u", "U", longUrl.c_str()) + "]");
			Check(r4.ok && r4.entries.empty() && r4.skipped == 1, "caps: a too-long page_url drops the entry");

			// control characters become spaces
			const ParseResult r5 = ParseModIndex("[" + Obj("c", "Co\\u0001de\\tx", kGood, ", \"summary\": \"a\\u0007b\"") + "]");
			Check(r5.ok && r5.entries.size() == 1, "control: entry parsed");
			if (!r5.entries.empty())
			{
				bool clean = true;
				for (char c : r5.entries[0].name + r5.entries[0].summary)
					if (static_cast<unsigned char>(c) < 0x20)
						clean = false;
				Check(clean, "control: no control character reaches the text");
			}

			// a number too large for an int reads as unknown
			const ParseResult r6 = ParseModIndex("[" + Obj("n", "N", kGood, ", \"interface_version_min\": 99999999999999999999") + "]");
			Check(r6.ok && r6.entries.size() == 1 && r6.entries[0].interfaceVersionMin == -1, "interface: an out-of-range number reads as unknown");
			const ParseResult r7 = ParseModIndex("[" + Obj("n", "N", kGood, ", \"interface_version_min\": -5") + "]");
			Check(r7.ok && r7.entries.size() == 1 && r7.entries[0].interfaceVersionMin == -1, "interface: a negative number reads as unknown");
		}

		// hostile input
		{
			// deeply nested braces
			std::string deep = "[";
			deep += std::string(200000, '{');
			deep += "]";
			Check(ParseModIndex(deep).entries.empty(), "hostile: 200000 open braces give no entry and no crash");
			std::string deepBoth = "[" + std::string(50000, '{') + std::string(50000, '}') + "]";
			const ParseResult balanced = ParseModIndex(deepBoth);
			Check(balanced.entries.empty(), "hostile: balanced deep nesting gives no entry and no crash");

			// brackets and braces inside strings
			const ParseResult r = ParseModIndex("[{\"id\": \"a\", \"name\": \"A}]{\", \"page_url\": \"" + std::string(kGood) + "\", \"summary\": \"[{\\\"}\"}]");
			Check(r.ok && r.entries.size() == 1 && r.entries[0].name == "A}]{", "hostile: braces inside strings do not split the object");

			// NUL inside a string value
			std::string nul = "[{\"id\": \"a\", \"name\": \"A";
			nul += '\0';
			nul += "B\", \"page_url\": \"" + std::string(kGood) + "\"}]";
			const ParseResult rn = ParseModIndex(nul);
			Check(rn.ok && rn.entries.size() == 1 && rn.entries[0].name.find('\0') == std::string::npos, "hostile: an embedded NUL never reaches the entry");

			// NUL inside the URL is refused
			std::string nulUrl = std::string(kGood) + "/x";
			nulUrl[nulUrl.size() - 1] = '\0';
			Check(!IsAllowedPageUrl(nulUrl), "hostile: a NUL in the URL is refused");

			// unterminated string at the end of a big text
			std::string open = "[{\"id\": \"";
			open += std::string(100000, 'a');
			open += "]";
			Check(ParseModIndex(open).entries.empty(), "hostile: a long unterminated string gives no entry");

			// a key text hidden in another value must not stand in for the field
			const ParseResult k = ParseModIndex("[{\"summary\": \"x \\\"id\\\": \\\"fake\\\"\", \"id\": \"real\", \"name\": \"R\", \"page_url\": \"" + std::string(kGood) + "\"}]");
			Check(k.ok && k.entries.size() == 1 && k.entries[0].id == "real", "hostile: an escaped key inside a value does not stand in for the field");

			// only the first of repeated top-level arrays is read; trailing text fails
			Check(!ParseModIndex("[] []").ok || ParseModIndex("[] []").entries.empty(), "hostile: two arrays give no entry");
		}
	}

	void UrlTests()
	{
		const char* good[] = {
			"https://github.com/AlienXAXS/StarRupture-ModLoader",
			"https://www.github.com/a/b",
			"https://nexusmods.com/starrupture/mods/1",
			"https://www.nexusmods.com/starrupture/mods/1",
			"https://www.nexusmods.com",
			"https://github.com?x=1",
			"https://github.com#top",
			"https://GitHub.com/a",
		};
		for (const char* u : good)
			Check(IsAllowedPageUrl(u), (std::string("url allowed: ") + u).c_str());

		const char* bad[] = {
			"", "https://", "https:///", "github.com", "//github.com/a",
			"http://github.com/a", "http://www.nexusmods.com/a", "HTTPS://github.com/a",
			"javascript:alert(1)", "file:///C:/Windows/System32/calc.exe", "ftp://github.com/a", "data:text/html,x",
			"https://nexusmods.com.evil.com/a", "https://evilgithub.com/a", "https://github.com.evil.com/a",
			"https://evil.com/github.com", "https://notnexusmods.com/a", "https://sub.github.com/a",
			"https://github.com@evil.com/a", "https://evil.com@github.com/a", "https://user:pass@github.com/a",
			"https://github.com:8443/a", "https://github.com:443/a", "https://github.com:/a",
			"https://github.com\\@evil.com/a", "https://github.com\\evil.com/a", "https://github.com/a\\b",
			"https://github.com/a b", "https://github.com/a\tb", "https://github.com/a\nb", "https://github.com/a\rb",
			" https://github.com/a", "https://github.com/a ",
			"https://github.com/a\x01", "https://github.com/\x7F", "https://github.com/\xC3\xA9",
			"https://git\xC3\xA9hub.com/a", "https://github.com./a", "https://.github.com/a",
			"https://github%2Ecom/a", "https://github.com%00.evil.com/a", "https://[::1]/a", "https://127.0.0.1/a",
			"https:/github.com/a", "https:github.com/a", "https://github.com:80@evil.com/a",
		};
		for (const char* u : bad)
			Check(!IsAllowedPageUrl(u), (std::string("url refused: ") + u).c_str());
	}

	void InstalledTests()
	{
		Entry e;
		e.id = "codex";
		e.name = "Codex Recipes";
		const std::vector<std::string> none;
		Check(!IsInstalled(e, none), "installed: empty list");
		Check(IsInstalled(e, { "Other", "CODEX" }), "installed: id, any case");
		Check(IsInstalled(e, { "codex recipes" }), "installed: name, any case");
		Check(!IsInstalled(e, { "Codexx", "Code", "" }), "installed: no partial match");
		Entry empty;
		Check(!IsInstalled(empty, { "" }), "installed: an empty entry matches nothing");
	}

	std::string SamplePath()
	{
		wchar_t exe[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		std::wstring w = exe;
		for (int i = 0; i < 3; ++i)               // exe, tests, build -> solution folder
		{
			const size_t cut = w.find_last_of(L"\\/");
			if (cut == std::wstring::npos)
				break;
			w.resize(cut);
		}
		std::string path;
		for (wchar_t c : w)
			path += static_cast<char>(c);
		return path + "\\mod-index\\index.json";
	}

	void SampleTests()
	{
		const std::string text = ReadFile(SamplePath());
		Check(!text.empty(), "sample: mod-index/index.json is readable");
		const ParseResult r = ParseModIndex(text);
		Check(r.ok && r.skipped == 0 && !r.truncated, "sample: parses with 0 skipped");
		Check(r.entries.size() >= 5, "sample: has entries");
		for (const Entry& e : r.entries)
			Check(IsAllowedPageUrl(e.pageUrl) && !e.id.empty() && !e.name.empty() && e.interfaceVersionMin > 0, ("sample: complete entry " + e.id).c_str());
	}

	std::string OneEntryIndex() { return "[" + Obj("a", "A", kGood) + "]"; }

	void FetcherTests()
	{
		// Idle until asked; Get never starts anything
		{
			std::atomic<int> calls{ 0 };
			Fetcher f("https://example.test/index.json", [&](const std::string&) { ++calls; return OneEntryIndex(); });
			Check(f.Get()->state == State::Idle, "fetcher: starts Idle");
			f.Get(); f.Get();
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			Check(calls == 0 && f.Get()->state == State::Idle, "fetcher: Get alone starts no request");
			Check(f.Get()->entries.empty() && f.Get()->error.empty(), "fetcher: the Idle snapshot is empty");
		}

		// success: Idle -> Loading -> Ready, and the request got the URL
		{
			std::atomic<bool> release{ false };
			std::string seenUrl;
			std::mutex m;
			Fetcher f("https://example.test/index.json", [&](const std::string& url)
			{
				{ std::lock_guard<std::mutex> l(m); seenUrl = url; }
				while (!release)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				return OneEntryIndex();
			});
			f.RequestFirstLoad();
			Check(f.Get()->state == State::Loading, "fetcher: Loading right after RequestFirstLoad");
			const auto held = f.Get();              // a snapshot held by the tab
			release = true;
			Check(WaitFor(f, State::Ready), "fetcher: reaches Ready");
			const auto snap = f.Get();
			Check(snap->entries.size() == 1 && snap->entries[0].id == "a" && snap->error.empty(), "fetcher: Ready holds the entries");
			Check(held->state == State::Loading && held->entries.empty(), "fetcher: a snapshot already taken does not change");
			{ std::lock_guard<std::mutex> l(m); Check(seenUrl == "https://example.test/index.json", "fetcher: the request gets the URL"); }
			Check(f.Get() == snap, "fetcher: Ready: Get returns the same snapshot each frame (no copy)");
		}

		// idempotent first load: many calls, one request
		{
			std::atomic<int> calls{ 0 };
			std::atomic<bool> release{ false };
			Fetcher f("u", [&](const std::string&) { ++calls; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); return OneEntryIndex(); });
			std::vector<std::thread> threads;
			for (int i = 0; i < 8; ++i)
				threads.emplace_back([&] { for (int k = 0; k < 50; ++k) f.RequestFirstLoad(); });
			for (auto& t : threads)
				t.join();
			release = true;
			Check(WaitFor(f, State::Ready), "first load: reaches Ready");
			f.RequestFirstLoad(); f.RequestFirstLoad();
			f.Retry();                                  // Ready with entries: nothing
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			Check(calls == 1, "first load: 8 threads x 50 calls and later calls make one request");
			Check(f.Get()->state == State::Ready, "retry: Ready with entries stays Ready");
		}

		// failure: empty body -> Failed, Retry -> Loading -> Ready
		{
			std::atomic<int> calls{ 0 };
			std::atomic<bool> fail{ true };
			Fetcher f("u", [&](const std::string&) { ++calls; return fail ? std::string() : OneEntryIndex(); });
			f.Retry();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			Check(calls == 0 && f.Get()->state == State::Idle, "retry: from Idle does nothing");
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Failed), "failure: an empty answer reaches Failed");
			Check(!f.Get()->error.empty() && f.Get()->entries.empty(), "failure: Failed carries a message");
			f.RequestFirstLoad();
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			Check(calls == 1, "failure: RequestFirstLoad after Failed does not retry");
			fail = false;
			f.Retry();
			Check(WaitFor(f, State::Ready), "retry: Failed -> Loading -> Ready");
			Check(calls == 2 && f.Get()->entries.size() == 1 && f.Get()->error.empty(), "retry: one more request, entries, no error");
		}

		// bad body -> Failed with the parser's message; thrown request -> Failed
		{
			Fetcher f("u", [](const std::string&) { return std::string("<html>404</html>"); });
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Failed) && f.Get()->error == "The mod list is not a JSON array.", "failure: a body that is not an array reaches Failed with the parser's message");
			Fetcher g("u", [](const std::string&) -> std::string { throw std::runtime_error("boom"); });
			g.RequestFirstLoad();
			Check(WaitFor(g, State::Failed) && !g.Get()->error.empty(), "failure: a request that throws reaches Failed");
		}

		// Ready with no entries can retry; skipped count is carried
		{
			std::atomic<int> calls{ 0 };
			Fetcher f("u", [&](const std::string&) { return ++calls == 1 ? std::string("[]") : "[{\"id\":\"x\"}," + Obj("a", "A", kGood) + "]"; });
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Ready) && f.Get()->entries.empty(), "empty list: Ready with no entries");
			f.Retry();
			Check(WaitFor(f, State::Loading) || f.Get()->state == State::Ready, "empty list: Retry starts a request");
			const auto end = Clock::now() + std::chrono::seconds(5);
			while (Clock::now() < end && f.Get()->entries.empty())
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			Check(f.Get()->state == State::Ready && f.Get()->entries.size() == 1 && f.Get()->skipped == 1, "empty list: the second answer is Ready with the skip counted");
		}

		// default request: HttpGet with the feed's timeouts and the size cap
		{
			{ std::lock_guard<std::mutex> l(g_httpMutex); g_httpAnswer = OneEntryIndex(); g_httpCalls = 0; }
			Fetcher f("https://example.test/default.json");
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Ready), "default request: reaches Ready through HttpGet");
			std::lock_guard<std::mutex> l(g_httpMutex);
			Check(g_httpCalls == 1 && g_httpUrl == "https://example.test/default.json", "default request: one HttpGet with the URL");
			Check(g_httpOptions.maxBytes == kMaxIndexBytes, "default request: the response size cap is the index cap");
			Check(g_httpOptions.deadlineMs == FeedFetch::kDeadlineMs && g_httpOptions.receiveMs == FeedFetch::kOperationMs && g_httpOptions.connectMs == FeedFetch::kOperationMs, "default request: the feed's deadline and timeouts");
		}

		// shutdown: waits for a running request, blocks new ones, safe with a worker still running
		{
			std::atomic<bool> release{ false };
			std::atomic<bool> started{ false };
			Fetcher f("u", [&](const std::string&) { started = true; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); return OneEntryIndex(); });
			f.RequestFirstLoad();
			while (!started)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			const auto t0 = Clock::now();
			Check(!f.Shutdown(50), "shutdown: a running request that outlasts the wait returns false");
			Check(Clock::now() - t0 < std::chrono::seconds(2), "shutdown: the wait is bounded");
			release = true;
			Check(f.Shutdown(5000), "shutdown: returns true once the request has ended");
			f.Retry();
			f.RequestFirstLoad();
			Check(f.Get()->state != State::Loading, "shutdown: no new request after it");
		}
		{
			// the Fetcher is destroyed while the worker still runs: the worker must not touch freed memory
			std::atomic<bool> release{ false };
			std::atomic<bool> started{ false };
			std::atomic<bool> finished{ false };
			{
				Fetcher f("u", [&](const std::string&) { started = true; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); finished = true; return OneEntryIndex(); });
				f.RequestFirstLoad();
				while (!started)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			release = true;
			const auto end = Clock::now() + std::chrono::seconds(5);
			while (Clock::now() < end && !finished)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			Check(finished, "shutdown: a worker outlives its Fetcher and finishes cleanly");
		}
		{
			Fetcher f("u", [](const std::string&) { return std::string(); });
			Check(f.Shutdown(100), "shutdown: with nothing running returns true at once");
		}
	}
}

int main()
{
	ParserTests();
	UrlTests();
	InstalledTests();
	SampleTests();
	FetcherTests();

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
