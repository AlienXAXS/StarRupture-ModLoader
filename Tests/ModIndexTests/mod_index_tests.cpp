// ModIndexTests: the mod index parser (auto_update/mod_index.h), its URL
// allowlist, IsInstalled, and the Fetcher's states (auto_update/mod_index_fetcher.cpp)
// through an injected request. No network, no game.

#include "auto_update/http_get.h"
#include "auto_update/mod_index.h"
#include "auto_update/mod_index_fetcher.h"
#include "auto_update/mod_index_signed.h"
#include "auto_update/feed_fetch.h"
#include "memory_scanner/version_feed.h"
#include "third_party/monocypher/monocypher-ed25519.h"

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
	std::string g_httpIfNoneMatch;
	std::string g_httpAnswer;
	std::string g_httpEtagOut;           // the ETag header the stand-in server sends
	bool g_httpNotModified = false;      // answer a conditional request with 304
	int g_httpCalls = 0;
}

std::string HttpGet(const char* url, const char* /*label*/, const HttpGetOptions& options)
{
	std::lock_guard<std::mutex> lock(g_httpMutex);
	++g_httpCalls;
	g_httpUrl = url;
	g_httpOptions = options;
	g_httpIfNoneMatch = options.ifNoneMatch;
	if (g_httpNotModified && !options.ifNoneMatch.empty() && options.notModified)
	{
		*options.notModified = true;
		return {};
	}
	if (options.etag)
		*options.etag = g_httpEtagOut;
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

	std::string Obj(const char* id, const char* name, const char* url, const std::string& extra = std::string())
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
		for (const Entry& e : r.entries)
			Check(e.testedLoader == "1.27.0" && IsAllowedReadmeUrl(e.readmeUrl), ("sample: tested_loader and readme_url on " + e.id).c_str());
	}

	void RequiresTests()
	{
		const auto parse = [](const std::string& extra)
		{
			return ParseModIndex("[" + Obj("a", "A", kGood, extra) + "]");
		};
		const auto idsAre = [&parse](const std::string& extra, const std::vector<std::string>& ids)
		{
			const ParseResult r = parse(extra);
			return r.ok && r.entries.size() == 1 && r.entries[0].id == "a" && r.entries[0].requiredIds == ids;
		};

		Check(idsAre("", {}), "requires: absent");
		Check(idsAre(", \"requires\": []", {}), "requires: empty");
		Check(idsAre(", \"requires\": [ \"minimap\" , \"codex\" ]", { "minimap", "codex" }), "requires: ids read in order");
		Check(idsAre(", \"requires\": [\"a\\\"b\", \"c]d\", \"id\"]", { "a\"b", "c]d", "id" }), "requires: quotes, brackets and key names inside an id");
		Check(idsAre(", \"requires\": [\"\", \"  \", \"x\"]", { "x" }), "requires: blank ids are left out");
		Check(idsAre(", \"requires\": \"minimap\"", {}), "requires: a string is not a list");
		Check(idsAre(", \"requires\": [12]", {}), "requires: a number refuses the list");
		Check(idsAre(", \"requires\": [\"a\", {\"id\": \"z\"}]", {}), "requires: an object refuses the list, and the entry keeps its own id");
		Check(idsAre(", \"requires\": [[\"a\"]]", {}), "requires: a nested array refuses the list");

		std::string many = ", \"requires\": [";
		std::vector<std::string> kept;
		for (size_t i = 0; i < kMaxRequires + 3; ++i)
		{
			many += (i ? ",\"p" : "\"p") + std::to_string(i) + "\"";
			if (i < kMaxRequires)
				kept.push_back("p" + std::to_string(i));
		}
		Check(idsAre(many + "]", kept), "requires: at most kMaxRequires ids");
		Check(idsAre(", \"requires\": [\"" + std::string(kMaxIdLen + 5, 'r') + "\"]", { std::string(kMaxIdLen, 'r') }), "requires: a long id is cut as an id is");

		Check(JsonExtractStringArray("{\"requires\": [\"a\", \"b", "requires").empty(), "requires: cut inside a string");
		Check(JsonExtractStringArray("{\"requires\": [\"a\",", "requires").empty(), "requires: cut after a comma");
		Check(JsonExtractStringArray("{\"requires\": [\"a\\", "requires").empty(), "requires: cut after a backslash");
		Check(JsonExtractStringArray("{\"requires\": [\"a\u00", "requires").empty(), "requires: cut inside an escape");
		Check(JsonExtractStringArray("{\"requires\": [", "requires").empty(), "requires: cut after the bracket");

		const ParseResult r = parse(", \"requires\": [\"b\"], \"version\": \"2.0\", \"interface_version_min\": 79");
		Check(r.ok && r.entries.size() == 1 && r.entries[0].version == "2.0" && r.entries[0].interfaceVersionMin == 79 &&
		      r.entries[0].name == "A" && r.entries[0].pageUrl == kGood, "requires: the other fields read as before");
	}

	void EntryFieldTests()
	{
		const std::string raw = "https://raw.githubusercontent.com/AlienXAXS/StarRupture-Plugin-Codex/main/README.md";

		// both new fields read
		{
			const ParseResult r = ParseModIndex("[" + Obj("a", "A", kGood, ", \"tested_loader\": \"1.27.0\", \"readme_url\": \"" + raw + "\"") + "]");
			Check(r.ok && r.entries.size() == 1 && r.entries[0].testedLoader == "1.27.0" && r.entries[0].readmeUrl == raw, "fields: tested_loader and readme_url are read");
		}

		// both are optional
		{
			const ParseResult r = ParseModIndex("[" + Obj("a", "A", kGood) + "]");
			Check(r.ok && r.entries.size() == 1 && r.skipped == 0 && r.entries[0].testedLoader.empty() && r.entries[0].readmeUrl.empty(),
				"fields: an entry without them stays valid");
		}

		// tested_loader is cleaned and capped
		{
			const ParseResult r = ParseModIndex("[" + Obj("a", "A", kGood, ", \"tested_loader\": \"" + std::string(60, '9') + "\"") + "]");
			Check(r.entries.size() == 1 && r.entries[0].testedLoader.size() == kMaxTestedLoaderLen, "fields: tested_loader is cut at the cap");
			const ParseResult c = ParseModIndex("[" + Obj("a", "A", kGood, ", \"tested_loader\": \"1.2\\n7\\u0000.0\"") + "]");
			Check(c.entries.size() == 1 && c.entries[0].testedLoader.find('\n') == std::string::npos && c.entries[0].testedLoader.find('\0') == std::string::npos,
				"fields: control characters in tested_loader do not survive");
			const ParseResult n = ParseModIndex("[" + Obj("a", "A", kGood, ", \"tested_loader\": 127") + "]");
			Check(n.entries.size() == 1 && n.entries[0].testedLoader.empty(), "fields: a number is not a tested_loader");
		}

		// the README allowlist
		Check(IsAllowedReadmeUrl(raw), "readme url allowed: a raw README");
		Check(!IsAllowedReadmeUrl("https://raw.githubusercontent.com.example.com/a/b/c"), "readme url refused: another host that only starts alike");
		Check(IsAllowedReadmeUrl("https://Raw.GithubUsercontent.com/a/b/c/README.md"), "readme url allowed: host in any case");
		Check(IsAllowedReadmeUrl("https://raw.githubusercontent.com/" + std::string(200 - 34, 'a')) && ("https://raw.githubusercontent.com/" + std::string(200 - 34, 'a')).size() == kMaxUrlLen,
			"readme url allowed: exactly the length cap");
		Check(!IsAllowedReadmeUrl("https://raw.githubusercontent.com/" + std::string(200 - 33, 'a')), "readme url refused: one byte over the cap");
		Check(!IsAllowedPageUrl(raw), "page url refused: the README host is not a page host");
		Check(!IsAllowedReadmeUrl(kGood) && !IsAllowedReadmeUrl("https://www.nexusmods.com/starrupture/mods/1"), "readme url refused: the page hosts");

		const char* bad[] = {
			"", "https://", "raw.githubusercontent.com/a", "//raw.githubusercontent.com/a",
			"http://raw.githubusercontent.com/a/b", "HTTPS://raw.githubusercontent.com/a/b",
			"javascript:alert(1)", "file:///C:/Windows/System32/calc.exe", "ftp://raw.githubusercontent.com/a", "data:text/html,x",
			"https://raw.githubusercontent.com.evil.com/a", "https://evilraw.githubusercontent.com/a", "https://sub.raw.githubusercontent.com/a",
			"https://evil.com/raw.githubusercontent.com", "https://github.com/a/b", "https://githubusercontent.com/a/b", "https://gist.githubusercontent.com/a",
			"https://raw.githubusercontent.com@evil.com/a", "https://evil.com@raw.githubusercontent.com/a", "https://user:pass@raw.githubusercontent.com/a",
			"https://raw.githubusercontent.com:8443/a", "https://raw.githubusercontent.com:/a",
			"https://raw.githubusercontent.com\\@evil.com/a", "https://raw.githubusercontent.com/a\\b",
			"https://raw.githubusercontent.com/a b", "https://raw.githubusercontent.com/a\tb", "https://raw.githubusercontent.com/a\nb",
			" https://raw.githubusercontent.com/a", "https://raw.githubusercontent.com/a ",
			"https://raw.githubusercontent.com/a\x01", "https://raw.githubusercontent.com/\x7F", "https://raw.githubusercontent.com/\xC3\xA9",
			"https://raw.githubusercontent.com./a", "https://raw%2Egithubusercontent.com/a", "https://[::1]/a", "https://127.0.0.1/a",
		};
		for (const char* u : bad)
			Check(!IsAllowedReadmeUrl(u), (std::string("readme url refused: ") + u).c_str());

		// a bad readme_url drops the field, never the entry
		const char* hostile[] = {
			"http://raw.githubusercontent.com/a/b", "https://evil.com/a", "https://raw.githubusercontent.com.evil.com/a",
			"javascript:alert(1)", "https://user@raw.githubusercontent.com/a", "https://github.com/a/b/blob/main/README.md",
		};
		for (const char* u : hostile)
		{
			const ParseResult r = ParseModIndex("[" + Obj("a", "A", kGood, std::string(", \"readme_url\": \"") + u + "\"") + "]");
			Check(r.ok && r.entries.size() == 1 && r.skipped == 0 && r.entries[0].readmeUrl.empty() && r.entries[0].pageUrl == kGood,
				(std::string("fields: the entry stays, the readme_url goes: ") + u).c_str());
		}
		{
			const std::string longUrl = "https://raw.githubusercontent.com/" + std::string(200, 'a');
			const ParseResult r = ParseModIndex("[" + Obj("a", "A", kGood, ", \"readme_url\": \"" + longUrl + "\"") + "]");
			Check(r.entries.size() == 1 && r.entries[0].readmeUrl.empty(), "fields: an overlong readme_url is dropped, not cut");
			const ParseResult o = ParseModIndex("[" + Obj("a", "A", kGood, ", \"readme_url\": {\"x\": 1}") + "]");
			Check(o.entries.size() == 1 && o.entries[0].readmeUrl.empty(), "fields: an object is not a readme_url");
			const ParseResult nul = ParseModIndex("[" + Obj("a", "A", kGood, ", \"readme_url\": \"https://raw.githubusercontent.com/a\\u0000.evil\"") + "]");
			Check(nul.entries.size() == 1 && nul.entries[0].readmeUrl.find('\0') == std::string::npos && (nul.entries[0].readmeUrl.empty() || IsAllowedReadmeUrl(nul.entries[0].readmeUrl)), "fields: a NUL escape in the readme_url leaves no NUL and an allowed address or none");
		}
	}

	// ---- the signed file ---------------------------------------------------

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
		uint8_t                secret[64]{};
		VersionFeed::PublicKey pub;
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

	// `region` is everything the signature covers.
	std::string Sign(const Signer& s, const std::string& region, const char* keyId = nullptr)
	{
		uint8_t sig[64];
		crypto_ed25519_sign(sig, s.secret, reinterpret_cast<const uint8_t*>(region.data()), region.size());
		return region + "sig\t" + (keyId ? keyId : s.pub.id.c_str()) + "\t" + Base64(sig, 64) + "\n";
	}

	// The file as it is published: format, seq, kind, the index, the signature.
	std::string SignedFile(const Signer& s, uint64_t seq, const std::string& json, const char* kind = "mod-index")
	{
		return Sign(s, "format\t1\nseq\t" + std::to_string(seq) + "\nkind\t" + kind + "\n" + json);
	}

	std::string IndexOf(const char* id) { return "[" + Obj(id, "A", kGood, ", \"tested_loader\": \"1.27.0\"") + "]\n"; }

	std::wstring TempCache()
	{
		static int n = 0;
		wchar_t dir[MAX_PATH]{};
		GetTempPathW(MAX_PATH, dir);
		return std::wstring(dir) + L"mod_index_tests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++n) + L".cache";
	}

	void RemoveCache(const std::wstring& cache)
	{
		DeleteFileW(cache.c_str());
		DeleteFileW((cache + L".etag").c_str());
	}

	bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

	std::string Slurp(const std::wstring& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	void Spit(const std::wstring& path, const std::string& text)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << text;
	}

	// What the stand-in server answers next.
	void Serve(const std::string& file, const std::string& etag = "", bool notModified = false)
	{
		std::lock_guard<std::mutex> l(g_httpMutex);
		g_httpAnswer = file;
		g_httpEtagOut = etag;
		g_httpNotModified = notModified;
	}

	void EnvelopeTests()
	{
		const Signer sk1 = MakeSigner("k1", 1);
		const Signer sk2 = MakeSigner("k2", 2);
		const Signer other = MakeSigner("k2", 9);
		const std::vector<VersionFeed::PublicKey> keys{ sk1.pub, sk2.pub };
		const std::string json = IndexOf("a");

		const auto read = [&](const std::string& file, Envelope& out, std::string& error) { return ReadEnvelope(file, keys, out, error); };
		Envelope e;
		std::string err;

		Check(read(SignedFile(sk2, 7, json), e, err) && e.seq == 7 && e.body == json && err.empty(), "envelope: a good file gives its seq and the body byte for byte");
		Check(read(SignedFile(sk1, 1, json), e, err) && e.seq == 1, "envelope: the first key slot verifies too");
		Check(IsEnvelope(SignedFile(sk2, 7, json)) && !IsEnvelope(json) && !IsEnvelope("") && !IsEnvelope("form"), "envelope: IsEnvelope tells a signed file from a bare index");

		std::string tampered = SignedFile(sk2, 7, json);
		tampered.replace(tampered.find("\"A\""), 3, "\"B\"");
		Check(!read(tampered, e, err) && e.body.empty() && !err.empty(), "envelope: a changed byte in the body is refused and nothing is taken");
		std::string reSeq = SignedFile(sk2, 7, json);
		reSeq.replace(reSeq.find("seq\t7"), 5, "seq\t8");
		Check(!read(reSeq, e, err) && e.body.empty(), "envelope: a changed seq is refused");
		Check(!read(SignedFile(other, 7, json), e, err) && e.body.empty(), "envelope: another key's signature is refused");
		Check(!read(Sign(sk2, "format\t1\nseq\t7\nkind\tmod-index\n" + json, "k9"), e, err), "envelope: an unknown key id is refused");
		Check(!read(Sign(sk2, "format\t1\nseq\t7\nkind\tmod-index\n" + json, "k1"), e, err), "envelope: a key id that does not match is refused");

		Check(!read(SignedFile(sk2, 7, json, "plugin-catalog"), e, err) && e.body.empty() && err.find("form") != std::string::npos, "envelope: another kind is refused, in plain words");
		Check(!read(Sign(sk2, "format\t2\nseq\t7\nkind\tmod-index\n" + json), e, err) && e.body.empty(), "envelope: another format is refused");
		Check(!read(Sign(sk2, "format\t1\nseq\t7\n" + json), e, err) && e.body.empty(), "envelope: a missing kind line is refused");
		Check(!read(Sign(sk2, "format\t1\nseq\t0\nkind\tmod-index\n" + json), e, err), "envelope: seq 0 is refused");

		const std::string good = SignedFile(sk2, 7, json);
		Check(!read(good.substr(0, good.size() - 12), e, err), "envelope: a truncated file is refused");
		Check(!read(good.substr(0, good.find("sig\t")), e, err), "envelope: a file with no sig line is refused");
		Check(!read(json, e, err) && e.body.empty(), "envelope: a bare index is refused here");
		Check(!read("", e, err), "envelope: an empty file is refused");
		Check(!read(SignedFile(sk2, 7, "[" + std::string(1024 * 1024, ' ') + "]\n"), e, err), "envelope: a signed file over 1 MiB is refused");
	}

	void SignedFetchTests()
	{
		const Signer sk1 = MakeSigner("k1", 1);
		const Signer sk2 = MakeSigner("k2", 2);
		const std::string url = "https://example.test/mod-index.v1.txt";
		const std::string idxA = IndexOf("aaa");
		const std::string idxB = IndexOf("bbb");

		Source src;
		src.keys = { sk1.pub, sk2.pub };
		src.cachePath = TempCache();
		const FetchFn fetch = MakeFetch(src);
		std::string error;

		// first download: taken, cached with its ETag, asked for the feed's way
		{
			std::lock_guard<std::mutex> l(g_httpMutex); g_httpCalls = 0;
		}
		const std::string file5 = SignedFile(sk2, 5, idxA);
		Serve(file5, "\"e5\"");
		Check(fetch(url, error) == idxA && error.empty(), "fetch: a verified file gives its body");
		Check(Slurp(src.cachePath) == file5 && Slurp(src.cachePath + L".etag") == "5 \"e5\"", "fetch: the verified file and its ETag are kept");
		{
			std::lock_guard<std::mutex> l(g_httpMutex);
			Check(g_httpCalls == 1 && g_httpUrl == url && g_httpIfNoneMatch.empty(), "fetch: one request for the URL, unconditional the first time");
			Check(g_httpOptions.maxBytes == VersionFeed::kMaxFileBytes, "fetch: the response is capped at the feed's file size");
			Check(g_httpOptions.deadlineMs == FeedFetch::kDeadlineMs && g_httpOptions.receiveMs == FeedFetch::kOperationMs && g_httpOptions.connectMs == FeedFetch::kOperationMs,
				"fetch: the feed's deadline and timeouts");
		}

		// unchanged: a conditional request, answered 304, the cached list
		Serve("", "", true);
		Check(fetch(url, error) == idxA, "fetch: 304 gives the cached list");
		{
			std::lock_guard<std::mutex> l(g_httpMutex);
			Check(g_httpIfNoneMatch == "\"e5\"", "fetch: the stored ETag is sent");
		}

		// no answer: the verified cached list
		Serve("");
		Check(fetch(url, error) == idxA && error.empty(), "fetch: a failed download shows the cached list");

		// a lower seq is refused and the cache stays
		Serve(SignedFile(sk2, 4, idxB), "\"e4\"");
		Check(fetch(url, error).empty() && error.find("older") != std::string::npos, "fetch: a lower seq is a failure, in plain words");
		Check(Slurp(src.cachePath) == file5 && Slurp(src.cachePath + L".etag") == "5 \"e5\"", "fetch: a refused file leaves the cache alone");

		// the same seq: the cached body
		error.clear();
		Serve(SignedFile(sk2, 5, idxB), "\"e5b\"");
		Check(fetch(url, error) == idxA && error.empty(), "fetch: an equal seq serves the cached body");

		// a file that does not verify fails, whatever the cache holds
		std::string forged = SignedFile(sk2, 9, idxB);
		forged.replace(forged.find("\"A\""), 3, "\"B\"");
		Serve(forged);
		Check(fetch(url, error).empty() && !error.empty(), "fetch: an unverified file fails and the cache is not used");
		Check(Slurp(src.cachePath) == file5, "fetch: an unverified file is not cached");
		error.clear();
		Serve(idxB);
		Check(fetch(url, error).empty() && !error.empty(), "fetch: a bare index fails on the loader's own address");
		error.clear();
		Serve(SignedFile(sk2, 9, idxB, "plugin-catalog"));
		Check(fetch(url, error).empty() && !error.empty(), "fetch: another kind fails");

		// a higher seq replaces the cache
		error.clear();
		const std::string file6 = SignedFile(sk2, 6, idxB);
		Serve(file6, "\"e6\"");
		Check(fetch(url, error) == idxB && error.empty(), "fetch: a higher seq is taken");
		Check(Slurp(src.cachePath) == file6 && Slurp(src.cachePath + L".etag") == "6 \"e6\"", "fetch: the cache moves to the new file");

		// a cache that no longer verifies is ignored
		Spit(src.cachePath, file6 + "x");
		Serve("");
		Check(fetch(url, error).empty() && error.empty(), "fetch: a damaged cache is not shown");
		RemoveCache(src.cachePath);

		// nothing cached, nothing downloaded
		Check(fetch(url, error).empty() && error.empty() && !Exists(src.cachePath), "fetch: no cache and no answer is a plain failure");

		// no answer at all, with a stale cache older than a later download
		Serve(SignedFile(sk2, 3, idxA));
		Check(fetch(url, error) == idxA && Exists(src.cachePath), "fetch: a first file is taken when nothing is cached");
		RemoveCache(src.cachePath);

		// through the Fetcher
		{
			Serve(SignedFile(sk2, 2, idxA));
			Fetcher f(url, MakeFetch(src));
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Ready) && f.Get()->entries.size() == 1 && f.Get()->entries[0].id == "aaa" && f.Get()->entries[0].testedLoader == "1.27.0",
				"fetcher: a verified file reaches Ready with its entries");
			f.Shutdown(1000);
		}
		{
			std::string bad = SignedFile(sk2, 3, idxB);
			bad.replace(bad.find("\"A\""), 3, "\"B\"");
			Serve(bad);
			Fetcher f(url, MakeFetch(src));
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Failed) && f.Get()->entries.empty() && f.Get()->error.find("verified") != std::string::npos,
				"fetcher: an unverified file is Failed with a plain error and no entries");
			f.Shutdown(1000);
		}
		{
			Serve("");
			Fetcher f(url, MakeFetch(src));
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Ready) && f.Get()->entries.size() == 1 && f.Get()->entries[0].id == "aaa", "fetcher: a failed download shows the cached list");
			f.Shutdown(1000);
		}
		RemoveCache(src.cachePath);
	}

	void PlayersOwnFetchTests()
	{
		const Signer sk2 = MakeSigner("k2", 2);
		const std::string url = "https://example.test/my-index.json";
		const std::string idxA = IndexOf("aaa");

		Source src;
		src.keys = { sk2.pub };
		src.cachePath = TempCache();
		src.signedOnly = false;
		const FetchFn fetch = MakeFetch(src);
		std::string error;

		Serve(idxA, "\"x\"");
		Check(fetch(url, error) == idxA && error.empty(), "own url: a bare index is read as it comes");
		Check(!Exists(src.cachePath) && !Exists(src.cachePath + L".etag"), "own url: nothing is cached");
		{
			std::lock_guard<std::mutex> l(g_httpMutex);
			Check(g_httpIfNoneMatch.empty() && g_httpUrl == url, "own url: the request is unconditional");
		}

		Serve(SignedFile(sk2, 4, idxA), "\"x\"");
		Check(fetch(url, error) == idxA && error.empty() && !Exists(src.cachePath), "own url: a verified envelope gives its body");

		std::string forged = SignedFile(sk2, 4, idxA);
		forged.replace(forged.find("\"A\""), 3, "\"B\"");
		Serve(forged);
		Check(fetch(url, error).empty() && !error.empty(), "own url: an envelope that does not verify fails");
		error.clear();
		Serve(SignedFile(sk2, 4, idxA, "plugin-catalog"));
		Check(fetch(url, error).empty() && !error.empty(), "own url: an envelope of another kind fails");
		error.clear();
		Serve("");
		Check(fetch(url, error).empty() && error.empty(), "own url: no answer is a plain failure");
	}

	std::string OneEntryIndex() { return "[" + Obj("a", "A", kGood) + "]"; }

	void FetcherTests()
	{
		// Idle until asked; Get never starts anything
		{
			std::atomic<int> calls{ 0 };
			Fetcher f("https://example.test/index.json", [&](const std::string&, std::string&) { ++calls; return OneEntryIndex(); });
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
			Fetcher f("https://example.test/index.json", [&](const std::string& url, std::string&)
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
			Fetcher f("u", [&](const std::string&, std::string&) { ++calls; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); return OneEntryIndex(); });
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
			Fetcher f("u", [&](const std::string&, std::string&) { ++calls; return fail ? std::string() : OneEntryIndex(); });
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
			Fetcher f("u", [](const std::string&, std::string&) { return std::string("<html>404</html>"); });
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Failed) && f.Get()->error == "The mod list is not a JSON array.", "failure: a body that is not an array reaches Failed with the parser's message");
			Fetcher g("u", [](const std::string&, std::string&) -> std::string { throw std::runtime_error("boom"); });
			g.RequestFirstLoad();
			Check(WaitFor(g, State::Failed) && !g.Get()->error.empty(), "failure: a request that throws reaches Failed");
		}

		// Ready with no entries can retry; skipped count is carried
		{
			std::atomic<int> calls{ 0 };
			Fetcher f("u", [&](const std::string&, std::string&) { return ++calls == 1 ? std::string("[]") : "[{\"id\":\"x\"}," + Obj("a", "A", kGood) + "]"; });
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Ready) && f.Get()->entries.empty(), "empty list: Ready with no entries");
			f.Retry();
			Check(WaitFor(f, State::Loading) || f.Get()->state == State::Ready, "empty list: Retry starts a request");
			const auto end = Clock::now() + std::chrono::seconds(5);
			while (Clock::now() < end && f.Get()->entries.empty())
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			Check(f.Get()->state == State::Ready && f.Get()->entries.size() == 1 && f.Get()->skipped == 1, "empty list: the second answer is Ready with the skip counted");
		}

		// the request's own words reach the Failed state; a plain empty answer gets the generic ones
		{
			Fetcher f("u", [](const std::string&, std::string& error) { error = "Plain words from the request."; return std::string(); });
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Failed) && f.Get()->error == "Plain words from the request.", "failed: the request's error text is shown");
			Fetcher g("u", [](const std::string&, std::string&) { return std::string(); });
			g.RequestFirstLoad();
			Check(WaitFor(g, State::Failed) && !g.Get()->error.empty(), "failed: an empty answer gets the generic text");
		}

		// shutdown: waits for a running request, blocks new ones, safe with a worker still running
		{
			std::atomic<bool> release{ false };
			std::atomic<bool> started{ false };
			Fetcher f("u", [&](const std::string&, std::string&) { started = true; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); return OneEntryIndex(); });
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
				Fetcher f("u", [&](const std::string&, std::string&) { started = true; while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); finished = true; return OneEntryIndex(); });
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
			Fetcher f("u", [](const std::string&, std::string&) { return std::string(); });
			Check(f.Shutdown(100), "shutdown: with nothing running returns true at once");
		}
	}
}

namespace
{
// The shared key lookup in auto_update/json_extract.h: a key matches only as a
// real key, and the value read is the one that belongs to it.
void JsonExtractTests()
{
	// Non-string values read as absent, never as the next string in the text
	Check(JsonExtractString("{\"version\": 1.2, \"name\": \"X\"}", "version").empty(), "json: a number value is not a string");
	Check(JsonExtractString("{\"version\": true, \"name\": \"X\"}", "version").empty(), "json: true is not a string");
	Check(JsonExtractString("{\"version\": false, \"name\": \"X\"}", "version").empty(), "json: false is not a string");
	Check(JsonExtractString("{\"version\": null, \"name\": \"X\"}", "version").empty(), "json: null is not a string");
	Check(JsonExtractString("{\"version\": {\"a\": \"b\"}, \"name\": \"X\"}", "version").empty(), "json: an object value is not a string");
	Check(JsonExtractString("{\"version\": [\"a\"], \"name\": \"X\"}", "version").empty(), "json: an array value is not a string");
	Check(JsonExtractDecodedString("{\"version\": 1.2, \"name\": \"X\"}", "version").empty(), "json: decoded: a number value is not a string");
	Check(JsonExtractDecodedString("{\"version\": null, \"name\": \"X\"}", "version").empty(), "json: decoded: null is not a string");
	Check(JsonExtractDecodedString("{\"version\": [\"a\"], \"name\": \"X\"}", "version").empty(), "json: decoded: an array value is not a string");
	Check(JsonExtractString("{\"version\": 1.2, \"name\": \"X\"}", "name") == "X", "json: the next field still reads after a number");

	// A value that spells a key is not that key
	Check(JsonExtractString("{\"name\":\"id\",\"id\":\"real\"}", "id") == "real", "json: a value equal to the key does not stand in for it");
	Check(JsonExtractDecodedString("{\"name\":\"id\",\"id\":\"real\"}", "id") == "real", "json: decoded: a value equal to the key does not stand in for it");
	Check(JsonExtractString("{\"name\":\"id\"}", "id").empty(), "json: a value equal to the key is not found as a key");
	Check(JsonExtractInt("{\"name\":\"n\",\"n\":5}", "n", -1) == 5, "json: an int lookup skips a value equal to the key");
	Check(JsonExtractInt("{\"name\":\"n\"}", "n", -1) == -1, "json: an int lookup finds no key in a value");

	// Escapes and spacing
	Check(JsonExtractString("{\"a\": \"x \\\"id\\\": y\", \"id\": \"real\"}", "id") == "real", "json: an escaped key inside a value is skipped");
	Check(JsonExtractString("{\"id\"\t:\r\n \"v\"}", "id") == "v", "json: whitespace around the colon is allowed");
	Check(JsonExtractString("{\"a\": \"c:\\\\\", \"id\": \"v\"}", "id") == "v", "json: a value ending in an escaped backslash closes");
	Check(JsonExtractString("{\"id\": \"v", "id") == "v", "json: an unterminated value reads to the end, as before");
	Check(JsonExtractString("{\"id\": ", "id").empty() && JsonExtractInt("{\"id\":", "id", -1) == -1, "json: a key with no value is absent");
	Check(JsonExtractString("{\"a\": \"unterminated", "id").empty(), "json: an unterminated string does not loop or match");
	Check(JsonExtractString("{\"identity\": \"x\"}", "id").empty(), "json: a longer key does not match its prefix");

	Check(JsonExtractString("{\"a\": \"5\\\" pipe\", \"id\": \"real\"}", "id") == "real", "json: a single escaped quote in a value does not end the string");
	Check(JsonExtractString("{\"a\": \"x\\", "id").empty() && JsonExtractInt("{\"a\": \"x\\", "id", -1) == -1, "json: a trailing backslash ends the walk");

	// Nested objects: the first real key in the text still wins
	Check(JsonExtractString("{\"a\": {\"id\": \"inner\"}, \"id\": \"outer\"}", "id") == "inner", "json: the first match in text order wins");
	Check(JsonExtractString("{\"id\": \"outer\", \"a\": {\"id\": \"inner\"}}", "id") == "outer", "json: the outer key first wins");

	// The object-array lookup needs a real array value
	Check(JsonExtractObjectArray("{\"plugins\": [{\"a\":\"1\"},{\"a\":\"2\"}]}", "plugins").size() == 2, "json: an object array splits");
	Check(JsonExtractObjectArray("{\"plugins\": 3, \"other\": [{\"a\":\"1\"}]}", "plugins").empty(), "json: a non-array value gives no objects");
	Check(JsonExtractObjectArray("{\"x\":\"plugins\", \"other\": [{\"a\":\"1\"}]}", "plugins").empty(), "json: a value equal to the array key gives no objects");

	// Release manifests as the release workflow writes them: unchanged
	const std::string manifest =
		"{\n"
		"  \"build_tag\": \"v1.28.0\",\n"
		"  \"download_url\": \"https://example.test/releases/download/v1.28.0/ModLoader.zip\",\n"
		"  \"plugin_name\": \"ModLoader\",\n"
		"  \"version\": \"1.28.0\",\n"
		"  \"interface_version_min\": 19,\n"
		"  \"interface_version_max\": 81,\n"
		"  \"notes\": \"Line one.\\nSays \\\"hi\\\" and the key \\\"version\\\": 7.\",\n"
		"  \"release_notes\": \"## v1.28.0\\n- A change\"\n"
		"}";
	Check(JsonExtractString(manifest, "build_tag") == "v1.28.0", "manifest: build_tag");
	Check(JsonExtractString(manifest, "download_url") == "https://example.test/releases/download/v1.28.0/ModLoader.zip", "manifest: download_url");
	Check(JsonExtractString(manifest, "plugin_name") == "ModLoader", "manifest: plugin_name");
	Check(JsonExtractString(manifest, "version") == "1.28.0", "manifest: version");
	Check(JsonExtractInt(manifest, "interface_version_min", -1) == 19 && JsonExtractInt(manifest, "interface_version_max", -1) == 81, "manifest: the interface range");
	Check(JsonExtractDecodedString(manifest, "notes") == "Line one.\nSays \"hi\" and the key \"version\": 7.", "manifest: notes decode with an escaped key in the text");
	Check(JsonExtractDecodedString(manifest, "release_notes") == "## v1.28.0\n- A change", "manifest: release_notes");
	Check(JsonExtractString(manifest, "manifest_url").empty(), "manifest: an absent key is empty");

	// A plugin's own update manifest (flat)
	const std::string pluginManifest = "{\"version\":\"0.26.0\",\"download_url\":\"https://example.test/p.zip\",\"plugin_name\":\"BetterCheats\",\"interface_version_min\":77}";
	Check(JsonExtractString(pluginManifest, "version") == "0.26.0" && JsonExtractString(pluginManifest, "plugin_name") == "BetterCheats", "manifest: a flat plugin manifest reads");
	Check(JsonExtractInt(pluginManifest, "interface_version_min", -1) == 77 && JsonExtractInt(pluginManifest, "interface_version_max", -1) == -1, "manifest: a plugin manifest's min is read and a missing max falls back");
}
}

int main()
{
	JsonExtractTests();
	ParserTests();
	UrlTests();
	InstalledTests();
	SampleTests();
	RequiresTests();
	EntryFieldTests();
	EnvelopeTests();
	SignedFetchTests();
	PlayersOwnFetchTests();
	FetcherTests();

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
