// ModIndexTests: the mod index parser (auto_update/mod_index.h), its URL
// allowlist, IsInstalled, and the Fetcher's states (auto_update/mod_index_fetcher.cpp)
// through an injected request. No network, no game.

#include "auto_update/http_get.h"
#include "auto_update/mod_index.h"
#include "auto_update/mod_index_fetcher.h"
#include "auto_update/mod_index_signed.h"
#include "auto_update/mod_install.h"
#include "auto_update/thumbnail_check.h"
#include "utils/sha256.h"
#include "auto_update/feed_fetch.h"
#include "memory_scanner/version_feed.h"
#include "third_party/monocypher/monocypher-ed25519.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
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
	unsigned g_httpStatus = 0;           // the status the stand-in server reports with no body
	bool g_httpOverLimit = false;        // report a body over the byte limit
	int g_httpCalls = 0;
}

std::string HttpGet(const char* url, const char* /*label*/, const HttpGetOptions& options)
{
	std::lock_guard<std::mutex> lock(g_httpMutex);
	++g_httpCalls;
	g_httpUrl = url;
	g_httpOptions = options;
	g_httpIfNoneMatch = options.ifNoneMatch;
	if (options.status)
		*options.status = g_httpAnswer.empty() ? g_httpStatus : 200;
	if (options.overLimit)
		*options.overLimit = g_httpOverLimit;
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
			Check((e.testedLoader.empty() || e.testedLoader == "1.27.0") && IsAllowedReadmeUrl(e.readmeUrl), ("sample: tested_loader and readme_url on " + e.id).c_str());
		// every entry but the two server-only ones installs, as the loader reads it
		for (const Entry& e : r.entries)
			Check(e.install.dllFile.empty() == (e.id == "serverutility" || e.id == "keepticking"), ("sample: install fields on " + e.id).c_str());
		// and only those two are marked server_only
		for (const Entry& e : r.entries)
			Check(e.serverOnly == (e.id == "serverutility" || e.id == "keepticking"), ("sample: server_only on " + e.id).c_str());
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
		g_httpStatus = 0;
		g_httpOverLimit = false;
	}

	// An answer with no list: the HTTP status it carried (0 = none arrived) or a body over the limit.
	void ServeNothing(unsigned status, bool overLimit = false)
	{
		Serve("");
		std::lock_guard<std::mutex> l(g_httpMutex);
		g_httpStatus = status;
		g_httpOverLimit = overLimit;
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

		// no list: the reason is said in plain words, whatever the cause
		ServeNothing(404);
		Check(fetch(url, error).empty() && error == "The mod list isn't published yet.", "fetch: an HTTP 404 says the list isn't published");
		error.clear();
		ServeNothing(503);
		Check(fetch(url, error).empty() && error == "The mod list server answered with an error (HTTP 503).", "fetch: another HTTP status is named");
		error.clear();
		ServeNothing(200, true);
		Check(fetch(url, error).empty() && error == "The mod list is larger than allowed.", "fetch: a body over the limit says so");
		error.clear();
		ServeNothing(0);
		Check(fetch(url, error).empty() && error.empty(), "fetch: no answer leaves the reason empty, for the connection text");
		ServeNothing(200);
		Check(fetch(url, error).empty() && error.empty(), "fetch: a 200 with no body leaves the reason empty");
		{
			Fetcher f(url, MakeFetch(src));
			ServeNothing(404);
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Failed) && f.Get()->error == "The mod list isn't published yet.", "fetcher: a 404 shows the not-published text");
			f.Shutdown(1000);
		}
		{
			Fetcher f(url, MakeFetch(src));
			ServeNothing(0);
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Failed) && f.Get()->error.find("Check your connection") != std::string::npos, "fetcher: no answer keeps the connection text");
			f.Shutdown(1000);
		}
		// a saved list is still shown when the server answers with an error
		Serve(SignedFile(sk2, 3, idxA));
		Check(fetch(url, error) == idxA, "fetch: a first file is taken");
		ServeNothing(404);
		error.clear();
		Check(fetch(url, error) == idxA && error.empty(), "fetch: a 404 shows the saved list, with no error");
		RemoveCache(src.cachePath);

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
		ServeNothing(404);
		Check(fetch(url, error).empty() && error == "The mod list isn't published yet.", "own url: an HTTP 404 says the list isn't published");
		error.clear();
		ServeNothing(500);
		Check(fetch(url, error).empty() && error == "The mod list server answered with an error (HTTP 500).", "own url: another HTTP status is named");
		error.clear();
		ServeNothing(200, true);
		Check(fetch(url, error).empty() && error == "The mod list is larger than allowed.", "own url: a body over the limit says so");
	}

	std::string OneEntryIndex() { return "[" + Obj("a", "A", kGood) + "]"; }

	// ---- Install: the parser's fields, the downloader and the writer ----------

	const char* kRepoPage  = "https://github.com/Owner/Repo";
	const char* kAssetUrl  = "https://github.com/Owner/Repo/releases/download/v1.0.0/Thing.dll";
	const char* kSignedHop = "https://release-assets.githubusercontent.com/github-production-release-asset/1/abc?sp=r&sig=a%2Bb%3D&rscd=attachment%3B+filename%3DThing.dll";
	const char* kOwnManifest = "https://github.com/Owner/Repo/releases/latest/download/Thing-client-manifest.json";

	std::string InstallJson(const std::string& url, const std::string& sha, const std::string& size,
		const std::string& files, const std::string& manifest)
	{
		std::string s;
		if (!url.empty())      s += ", \"download_url\": \"" + url + "\"";
		if (!sha.empty())      s += ", \"download_sha256\": \"" + sha + "\"";
		if (!size.empty())     s += ", \"download_size\": " + size;
		if (!files.empty())    s += ", \"install_files\": " + files;
		if (!manifest.empty()) s += ", \"update_manifest_url\": \"" + manifest + "\"";
		return s;
	}

	void InstallParserTests()
	{
		const std::string sha(64, 'a');
		const std::string both = "[\"Thing.dll\", \"Thing.json\"]";
		const std::string one = "[\"Thing.dll\"]";
		const auto parse = [](const std::string& extra, const char* page)
		{
			const ParseResult r = ParseModIndex("[" + Obj("thing", "Thing", page, extra) + "]");
			return r.entries.size() == 1 ? r.entries[0] : Entry();
		};

		const Entry full = parse(InstallJson(kAssetUrl, sha, "1234", both, kOwnManifest), kRepoPage);
		Check(full.install.url == kAssetUrl && full.install.sha256 == sha && full.install.size == 1234
			&& full.install.dllFile == "Thing.dll" && full.install.manifestUrl == kOwnManifest, "install fields: all read");
		// a server_only entry is never installable, whatever install fields it carries
		const Entry server = parse(InstallJson(kAssetUrl, sha, "1234", both, kOwnManifest) + ", \"server_only\": true", kRepoPage);
		Check(server.id == "thing" && server.serverOnly && server.install.url.empty() && server.install.dllFile.empty()
			&& server.install.sha256.empty() && server.install.size == 0 && server.install.manifestUrl.empty(),
			"install fields: a server_only entry keeps none of them");
		Check(parse(InstallJson(kAssetUrl, sha, "1234", both, kOwnManifest) + ", \"server_only\": false", kRepoPage).install.dllFile == "Thing.dll",
			"install fields: server_only false still installs");
		const Entry reversed = parse(InstallJson(kAssetUrl, sha, "1234", "[\"Thing.json\", \"Thing.dll\"]", kOwnManifest), kRepoPage);
		Check(reversed.install.dllFile == "Thing.dll", "install fields: the file order does not matter");
		const Entry alone = parse(InstallJson(kAssetUrl, sha, "33554432", one, ""), kRepoPage);
		Check(alone.install.dllFile == "Thing.dll" && alone.install.manifestUrl.empty() && alone.install.size == 32u * 1024 * 1024,
			"install fields: a DLL alone, at the size limit");

		struct Bad { const char* what; std::string extra; const char* page; };
		const std::string good = InstallJson(kAssetUrl, sha, "10", both, kOwnManifest);
		const auto url = [&](const std::string& u) { return InstallJson(u, sha, "10", one, ""); };
		const auto files = [&](const std::string& f) { return InstallJson(kAssetUrl, sha, "10", f, ""); };
		const auto manifest = [&](const std::string& m) { return InstallJson(kAssetUrl, sha, "10", both, m); };
		const std::string base = "https://github.com/Owner/Repo/releases/download/v1.0.0/";
		const std::string latest = "https://github.com/Owner/Repo/releases/latest/download/";
		const std::vector<Bad> bad = {
			{ "no url", InstallJson("", sha, "10", one, ""), kRepoPage },
			{ "an http url", url("http://github.com/Owner/Repo/releases/download/v1.0.0/Thing.dll"), kRepoPage },
			{ "another host", url("https://example.test/Owner/Repo/releases/download/v1.0.0/Thing.dll"), kRepoPage },
			{ "a lookalike host", url("https://github.com.example.test/Owner/Repo/releases/download/v1/Thing.dll"), kRepoPage },
			{ "user info", url("https://x@github.com/Owner/Repo/releases/download/v1/Thing.dll"), kRepoPage },
			{ "a query", url(base + "Thing.dll?x=1"), kRepoPage },
			{ "dot dot", url(base + "../Thing.dll"), kRepoPage },
			{ "an escape", url(base + "Thing%2e.dll"), kRepoPage },
			{ "not a release asset", url("https://github.com/Owner/Repo/raw/main/x/Thing.dll"), kRepoPage },
			{ "not a dll", url(base + "Thing.zip"), kRepoPage },
			{ "another repository", url("https://github.com/Owner/Other/releases/download/v1/Thing.dll"), kRepoPage },
			{ "another owner", url("https://github.com/Else/Repo/releases/download/v1/Thing.dll"), kRepoPage },
			{ "a page that is not GitHub", good, "https://www.nexusmods.com/starrupture/mods/1" },
			{ "no sha", InstallJson(kAssetUrl, "", "10", one, ""), kRepoPage },
			{ "a short sha", InstallJson(kAssetUrl, std::string(63, 'a'), "10", one, ""), kRepoPage },
			{ "an uppercase sha", InstallJson(kAssetUrl, std::string(64, 'A'), "10", one, ""), kRepoPage },
			{ "a sha that is not hex", InstallJson(kAssetUrl, std::string(64, 'g'), "10", one, ""), kRepoPage },
			{ "no size", InstallJson(kAssetUrl, sha, "", one, ""), kRepoPage },
			{ "size zero", InstallJson(kAssetUrl, sha, "0", one, ""), kRepoPage },
			{ "a negative size", InstallJson(kAssetUrl, sha, "-5", one, ""), kRepoPage },
			{ "a size over the limit", InstallJson(kAssetUrl, sha, "33554433", one, ""), kRepoPage },
			{ "a huge size", InstallJson(kAssetUrl, sha, "99999999999999999999", one, ""), kRepoPage },
			{ "no files", InstallJson(kAssetUrl, sha, "10", "", ""), kRepoPage },
			{ "an empty file list", files("[]"), kRepoPage },
			{ "two dlls", files("[\"Thing.dll\", \"Other.dll\"]"), kRepoPage },
			{ "three files", InstallJson(kAssetUrl, sha, "10", "[\"Thing.dll\", \"Thing.json\", \"x.ini\"]", kOwnManifest), kRepoPage },
			{ "an exe", files("[\"Thing.exe\"]"), kRepoPage },
			{ "a parent path", files("[\"../Thing.dll\"]"), kRepoPage },
			{ "a folder", files("[\"sub/Thing.dll\"]"), kRepoPage },
			{ "a backslash path", files("[\"..\\\\Thing.dll\"]"), kRepoPage },
			{ "a stream", files("[\"a:Thing.dll\"]"), kRepoPage },
			{ "a server dll", files("[\"Thing-Server.dll\"]"), kRepoPage },
			{ "a device name", files("[\"CON.dll\"]"), kRepoPage },
			{ "a leading dot", files("[\".Thing.dll\"]"), kRepoPage },
			{ "a json alone", InstallJson(kAssetUrl, sha, "10", "[\"Thing.json\"]", kOwnManifest), kRepoPage },
			{ "a json with another stem", InstallJson(kAssetUrl, sha, "10", "[\"Thing.dll\", \"Other.json\"]", kOwnManifest), kRepoPage },
			{ "a json with no manifest url", manifest(""), kRepoPage },
			{ "a manifest url with no json", InstallJson(kAssetUrl, sha, "10", one, kOwnManifest), kRepoPage },
			{ "a manifest of another repository", manifest("https://github.com/Owner/Other/releases/latest/download/Thing-client-manifest.json"), kRepoPage },
			{ "a manifest on another host", manifest("https://example.test/Owner/Repo/releases/latest/download/Thing-client-manifest.json"), kRepoPage },
			{ "a manifest of one tag", manifest(base + "Thing-client-manifest.json"), kRepoPage },
			{ "a manifest by another name", manifest(latest + "Thing.json"), kRepoPage },
			{ "a manifest name with a quote", manifest(latest + "a\\\"-client-manifest.json"), kRepoPage },
		};
		for (const Bad& b : bad)
		{
			const Entry e = parse(b.extra, b.page);
			Check(e.id == "thing" && e.name == "Thing" && e.install.dllFile.empty() && e.install.url.empty(),
				(std::string("install fields: listed with nothing to install for ") + b.what).c_str());
		}
	}

	// ---- Thumbnail: the parser's fields and the PNG header check --------------

	const std::string kCommit = "0123456789abcdef0123456789abcdef01234567";
	const std::string kThumbUrl = "https://raw.githubusercontent.com/Owner/Repo/" + kCommit + "/images/thumbnail.png";

	std::string ThumbJson(const std::string& url, const std::string& sha, const std::string& size)
	{
		std::string s;
		if (!url.empty())  s += ", \"thumbnail_url\": \"" + url + "\"";
		if (!sha.empty())  s += ", \"thumbnail_sha256\": \"" + sha + "\"";
		if (!size.empty()) s += ", \"thumbnail_size\": " + size;
		return s;
	}

	void ThumbnailParserTests()
	{
		const std::string sha(64, 'b');
		const auto parse = [](const std::string& extra, const char* page)
		{
			const ParseResult r = ParseModIndex("[" + Obj("thing", "Thing", page, extra) + "]");
			return r.entries.size() == 1 ? r.entries[0] : Entry();
		};
		const auto has = [&](const Entry& e) { return e.thumbnail.url == kThumbUrl && e.thumbnail.sha256 == sha && e.thumbnail.size == 183296; };

		const Entry full = parse(ThumbJson(kThumbUrl, sha, "183296"), kRepoPage);
		Check(has(full), "thumbnail fields: all read");
		Check(parse(ThumbJson(kThumbUrl, sha, "1"), kRepoPage).thumbnail.size == 1, "thumbnail fields: the smallest size");
		Check(parse(ThumbJson(kThumbUrl, sha, "524288"), kRepoPage).thumbnail.size == 512u * 1024
			&& kMaxThumbnailBytes == 512u * 1024, "thumbnail fields: the size limit");
		Check(has(parse(ThumbJson(kThumbUrl, sha, "183296"), "https://github.com/owner/REPO")), "thumbnail fields: owner and repository match whatever the case");
		Check(has(parse(ThumbJson(kThumbUrl, sha, "183296"), "https://github.com/Owner/Repo/")), "thumbnail fields: a page with a trailing slash");
		Check(has(parse(ThumbJson(kThumbUrl, sha, "183296"), "https://github.com/Owner/Repo/releases")), "thumbnail fields: a page with a path");
		Check(has(parse(ThumbJson(kThumbUrl, sha, "183296") + ", \"server_only\": true", kRepoPage)), "thumbnail fields: a server_only entry keeps them");

		// no keys: no picture, and every other field as before
		const Entry none = parse("", kRepoPage);
		Check(none.id == "thing" && none.thumbnail.url.empty() && none.thumbnail.sha256.empty() && none.thumbnail.size == 0,
			"thumbnail fields: none given, none set");
		// with the keys: the other fields read the same, install included
		const std::string install = InstallJson(kAssetUrl, sha, "1234", "[\"Thing.dll\", \"Thing.json\"]", kOwnManifest)
			+ ", \"author\": \"Al\", \"version\": \"1.2.3\", \"summary\": \"S\", \"tested_loader\": \"1.27.0\", \"interface_version_min\": 79, \"requires\": [\"a\"]";
		const Entry before = parse(install, kRepoPage);
		const Entry after = parse(install + ThumbJson(kThumbUrl, sha, "183296"), kRepoPage);
		Check(has(after) && before.thumbnail.url.empty(), "thumbnail fields: read beside the install fields");
		Check(after.id == before.id && after.name == before.name && after.author == before.author && after.version == before.version
			&& after.summary == before.summary && after.testedLoader == before.testedLoader && after.pageUrl == before.pageUrl
			&& after.interfaceVersionMin == before.interfaceVersionMin && after.requiredIds == before.requiredIds
			&& after.install.url == before.install.url && after.install.sha256 == before.install.sha256
			&& after.install.size == before.install.size && after.install.dllFile == before.install.dllFile
			&& after.install.manifestUrl == before.install.manifestUrl && before.install.dllFile == "Thing.dll",
			"thumbnail fields: the other fields read the same with or without them");
		// bad install keys and good thumbnail keys do not touch each other
		Check(parse(ThumbJson(kThumbUrl, sha, "183296") + ", \"download_url\": \"x\"", kRepoPage).install.url.empty()
			&& has(parse(ThumbJson(kThumbUrl, sha, "183296") + ", \"download_url\": \"x\"", kRepoPage)), "thumbnail fields: independent of the install fields");

		struct Bad { const char* what; std::string extra; const char* page; };
		const std::string raw = "https://raw.githubusercontent.com/Owner/Repo/";
		const auto url = [&](const std::string& u) { return ThumbJson(u, sha, "10"); };
		const std::vector<Bad> bad = {
			{ "no url", ThumbJson("", sha, "10"), kRepoPage },
			{ "no sha", ThumbJson(kThumbUrl, "", "10"), kRepoPage },
			{ "no size", ThumbJson(kThumbUrl, sha, ""), kRepoPage },
			{ "only a size", ThumbJson("", "", "10"), kRepoPage },
			{ "an http url", url("http://raw.githubusercontent.com/Owner/Repo/" + kCommit + "/images/thumbnail.png"), kRepoPage },
			{ "another host", url("https://example.test/Owner/Repo/" + kCommit + "/images/thumbnail.png"), kRepoPage },
			{ "a github.com raw path", url("https://github.com/Owner/Repo/raw/" + kCommit + "/images/thumbnail.png"), kRepoPage },
			{ "a lookalike host", url("https://raw.githubusercontent.com.example.test/Owner/Repo/" + kCommit + "/images/thumbnail.png"), kRepoPage },
			{ "an upper case host", url("https://RAW.githubusercontent.com/Owner/Repo/" + kCommit + "/images/thumbnail.png"), kRepoPage },
			{ "user info", url("https://x@raw.githubusercontent.com/Owner/Repo/" + kCommit + "/images/thumbnail.png"), kRepoPage },
			{ "a branch", url(raw + "main/images/thumbnail.png"), kRepoPage },
			{ "a tag", url(raw + "v1.2.0/images/thumbnail.png"), kRepoPage },
			{ "a short commit", url(raw + kCommit.substr(1) + "/images/thumbnail.png"), kRepoPage },
			{ "a long commit", url(raw + kCommit + "0/images/thumbnail.png"), kRepoPage },
			{ "an upper case commit", url(raw + std::string(40, 'A') + "/images/thumbnail.png"), kRepoPage },
			{ "a commit that is not hex", url(raw + std::string(40, 'g') + "/images/thumbnail.png"), kRepoPage },
			{ "a query", url(raw + kCommit + "/images/thumbnail.png?x=1"), kRepoPage },
			{ "a fragment", url(raw + kCommit + "/images/thumbnail.png#x"), kRepoPage },
			{ "a trailing slash", url(raw + kCommit + "/images/thumbnail.png/"), kRepoPage },
			{ "another file name", url(raw + kCommit + "/images/other.png"), kRepoPage },
			{ "another folder", url(raw + kCommit + "/Images/thumbnail.png"), kRepoPage },
			{ "a second path", url(raw + kCommit + "/extra/images/thumbnail.png"), kRepoPage },
			{ "dot dot", url("https://raw.githubusercontent.com/../" + kCommit + "/images/thumbnail.png"), kRepoPage },
			{ "an escape", url(raw + kCommit + "/images/thumbnail%2epng"), kRepoPage },
			{ "another repository", url("https://raw.githubusercontent.com/Owner/Other/" + kCommit + "/images/thumbnail.png"), kRepoPage },
			{ "another owner", url("https://raw.githubusercontent.com/Else/Repo/" + kCommit + "/images/thumbnail.png"), kRepoPage },
			{ "a url over the length limit", url(raw + kCommit + "/images/thumbnail.png" + std::string(200, 'x')), kRepoPage },
			{ "a short sha", ThumbJson(kThumbUrl, std::string(63, 'b'), "10"), kRepoPage },
			{ "an uppercase sha", ThumbJson(kThumbUrl, std::string(64, 'B'), "10"), kRepoPage },
			{ "a sha that is not hex", ThumbJson(kThumbUrl, std::string(64, 'g'), "10"), kRepoPage },
			{ "size zero", ThumbJson(kThumbUrl, sha, "0"), kRepoPage },
			{ "a negative size", ThumbJson(kThumbUrl, sha, "-5"), kRepoPage },
			{ "a size over the limit", ThumbJson(kThumbUrl, sha, "524289"), kRepoPage },
			{ "a huge size", ThumbJson(kThumbUrl, sha, "99999999999999999999"), kRepoPage },
			{ "a size as text", ThumbJson(kThumbUrl, sha, "\"10\""), kRepoPage },
			{ "a nexusmods.com page", ThumbJson(kThumbUrl, sha, "10"), "https://www.nexusmods.com/starrupture/mods/1" },
			{ "a bare nexusmods.com page", ThumbJson(kThumbUrl, sha, "10"), "https://nexusmods.com/starrupture/mods/1" },
			{ "a www.github.com page", ThumbJson(kThumbUrl, sha, "10"), "https://www.github.com/Owner/Repo" },
			{ "a page that is another repository", ThumbJson(kThumbUrl, sha, "10"), "https://github.com/Owner/Other" },
			{ "a page with a query after the repository", ThumbJson(kThumbUrl, sha, "10"), "https://github.com/Owner/Repo/issues?x=1" },
			{ "a page with an empty segment", ThumbJson(kThumbUrl, sha, "10"), "https://github.com/Owner/Repo//issues" },
		};
		for (const Bad& b : bad)
		{
			const Entry e = parse(b.extra, b.page);
			Check(e.id == "thing" && e.name == "Thing" && e.thumbnail.url.empty() && e.thumbnail.sha256.empty() && e.thumbnail.size == 0,
				(std::string("thumbnail fields: listed with no picture for ") + b.what).c_str());
		}

		// the address check on its own
		Check(IsThumbnailUrl(kThumbUrl, kRepoPage) && !IsThumbnailUrl(kThumbUrl, "https://www.nexusmods.com/starrupture/mods/1")
			&& !IsThumbnailUrl("", kRepoPage) && !IsThumbnailUrl(kThumbUrl, ""), "thumbnail url: only for the page's own GitHub repository");
	}

	// The header of a PNG: signature, IHDR chunk length 13, "IHDR", width and height (big endian).
	std::string PngHeader(unsigned w, unsigned h, unsigned length = 13, const char* type = "IHDR")
	{
		std::string s("\x89PNG\r\n\x1A\n", 8);
		const auto be = [&](unsigned v) { for (int shift = 24; shift >= 0; shift -= 8) s += static_cast<char>((v >> shift) & 0xFF); };
		be(length);
		s += type;
		be(w);
		be(h);
		s += std::string(5, '\0');   // bit depth, colour type, compression, filter, interlace
		return s;
	}

	void ThumbnailHeaderTests()
	{
		unsigned w = 0, h = 0;
		Check(CheckThumbnailHeader(PngHeader(640, 360), &w, &h) == ThumbnailCheck::Ok && w == 640 && h == 360, "thumbnail header: a 640 x 360 PNG");
		Check(CheckThumbnailHeader(PngHeader(640, 360)) == ThumbnailCheck::Ok, "thumbnail header: no out parameters");
		Check(CheckThumbnailHeader(PngHeader(1280, 720)) == ThumbnailCheck::Ok, "thumbnail header: the largest allowed");
		Check(CheckThumbnailHeader(PngHeader(1, 1)) == ThumbnailCheck::Ok, "thumbnail header: the smallest allowed");
		Check(CheckThumbnailHeader(PngHeader(640, 360) + std::string(5000, 'x')) == ThumbnailCheck::Ok, "thumbnail header: only the header is read");
		Check(kMaxThumbnailWidth == 1280 && kMaxThumbnailHeight == 720, "thumbnail header: the limits");

		w = h = 0;
		Check(CheckThumbnailHeader(PngHeader(1281, 720), &w, &h) == ThumbnailCheck::BadSize && w == 1281 && h == 720, "thumbnail header: 1281 wide is too big, and read");
		Check(CheckThumbnailHeader(PngHeader(1280, 721)) == ThumbnailCheck::BadSize, "thumbnail header: 721 high is too big");
		Check(CheckThumbnailHeader(PngHeader(0, 360)) == ThumbnailCheck::BadSize, "thumbnail header: width 0");
		Check(CheckThumbnailHeader(PngHeader(640, 0)) == ThumbnailCheck::BadSize, "thumbnail header: height 0");
		Check(CheckThumbnailHeader(PngHeader(0xFFFFFFFFu, 0xFFFFFFFFu)) == ThumbnailCheck::BadSize, "thumbnail header: 4 billion square");

		Check(CheckThumbnailHeader("") == ThumbnailCheck::NotPng, "thumbnail header: empty");
		Check(CheckThumbnailHeader(PngHeader(640, 360).substr(0, 23)) == ThumbnailCheck::NotPng, "thumbnail header: cut off one byte short");
		Check(CheckThumbnailHeader(PngHeader(640, 360).substr(0, 8)) == ThumbnailCheck::NotPng, "thumbnail header: the signature alone");
		Check(CheckThumbnailHeader(std::string("\xFF\xD8\xFF\xE0", 4) + std::string(60, '\0')) == ThumbnailCheck::NotPng, "thumbnail header: a JPEG");
		Check(CheckThumbnailHeader("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"640\" height=\"360\"></svg>") == ThumbnailCheck::NotPng, "thumbnail header: SVG text");
		Check(CheckThumbnailHeader("GIF89a" + std::string(60, '\0')) == ThumbnailCheck::NotPng, "thumbnail header: a GIF");
		std::string wrong = PngHeader(640, 360);
		wrong[1] = 'p';
		Check(CheckThumbnailHeader(wrong) == ThumbnailCheck::NotPng, "thumbnail header: a wrong signature byte");
		std::string noCrLf = PngHeader(640, 360);
		noCrLf[4] = '\n';
		Check(CheckThumbnailHeader(noCrLf) == ThumbnailCheck::NotPng, "thumbnail header: a signature with its line ends changed");
		Check(CheckThumbnailHeader(PngHeader(640, 360, 12)) == ThumbnailCheck::NotPng, "thumbnail header: an IHDR of the wrong length");
		Check(CheckThumbnailHeader(PngHeader(640, 360, 13, "IDAT")) == ThumbnailCheck::NotPng, "thumbnail header: another chunk first");
	}

	// A game folder of its own under %TEMP%, removed again.
	struct GameDir
	{
		std::wstring game, plugins;
		GameDir()
		{
			static int n = 0;
			wchar_t temp[MAX_PATH]{};
			GetTempPathW(MAX_PATH, temp);
			game = std::wstring(temp) + L"mod_install_tests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++n);
			plugins = game + L"\\ModLoader\\Plugins";
			std::filesystem::create_directories(plugins);
		}
		~GameDir()
		{
			std::error_code ignored;
			std::filesystem::remove_all(game, ignored);
		}
		// The names in Plugins, sorted, as one string: "" when it is empty.
		std::string Listing() const
		{
			std::vector<std::string> names;
			std::error_code ignored;
			for (const auto& item : std::filesystem::directory_iterator(plugins, ignored))
				names.push_back(item.path().filename().string());
			std::sort(names.begin(), names.end());
			std::string all;
			for (const std::string& name : names)
				all += (all.empty() ? "" : ",") + name;
			return all;
		}
		std::wstring At(const wchar_t* name) const { return plugins + L"\\" + name; }
	};

	const std::string kModBytes = std::string("MZ\0\x01 not a real plugin", 21) + std::string(2000, 'x');

	ModInstall::Request RequestFor(const GameDir& dir)
	{
		ModInstall::Request r;
		r.name = "Thing";
		r.version = "1.0.0";
		r.info.url = kAssetUrl;
		r.info.sha256 = Sha256::OfBytes(kModBytes);
		r.info.size = kModBytes.size();
		r.info.dllFile = "Thing.dll";
		r.info.manifestUrl = kOwnManifest;
		r.pluginsDir = dir.plugins;
		r.gameDir = dir.game;
		r.listSigned = true;
		return r;
	}

	ModInstall::Hop To(const std::string& location, unsigned status = 302) { return { status, location, "" }; }
	ModInstall::Hop Body(const std::string& body) { return { 200, "", body }; }

	struct Fetched
	{
		bool ok = false;
		std::string why;
		std::vector<std::string> asked;
		size_t maxBytes = 0;
		bool Says(const char* part) const { return !ok && why.find(part) != std::string::npos; }
	};

	// Runs Download against `answers`, one per request.
	Fetched Fetch(const std::vector<ModInstall::Hop>& answers, const ModIndex::InstallInfo* info = nullptr)
	{
		const GameDir dir;
		Fetched out;
		const ModInstall::HopFn hop = [&](const std::string& url, size_t maxBytes, unsigned, const std::atomic<bool>&)
		{
			out.asked.push_back(url);
			out.maxBytes = maxBytes;
			return out.asked.size() <= answers.size() ? answers[out.asked.size() - 1] : ModInstall::Hop();
		};
		std::atomic<bool> cancel{ false };
		std::string bytes = "stale";
		out.ok = ModInstall::Download(info ? *info : RequestFor(dir).info, "Thing", hop, cancel, bytes, out.why);
		Check(out.ok ? bytes == kModBytes && out.why.empty() : bytes.empty() && !out.why.empty(),
			"download: bytes only on success, words only on failure");
		return out;
	}

	void DownloadTests()
	{
		const char* const mismatch = "does not match";
		const char* const refused = "not allowed";
		const char* const failed = "download failed";

		Check(Sha256::OfBytes("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256: a known digest");

		for (unsigned status : { 301u, 302u, 303u, 307u, 308u })
		{
			const Fetched f = Fetch({ To(kSignedHop, status), Body(kModBytes) });
			Check(f.ok && f.asked.size() == 2, "download: one redirect, then the file");
		}
		{
			const Fetched f = Fetch({ To(kSignedHop), Body(kModBytes) });
			Check(f.asked.size() == 2 && f.asked[0] == kAssetUrl && f.asked[1] == kSignedHop, "download: the signed query is asked for as it came");
			Check(f.maxBytes == kModBytes.size(), "download: the answer is capped at the listed size");
		}

		std::string other = kModBytes;
		other.back() = 'y';
		Check(Fetch({ Body(other) }).Says(mismatch), "download: a hash mismatch is refused");
		Check(Fetch({ Body(kModBytes.substr(0, kModBytes.size() - 1)) }).Says(mismatch), "download: a short file is refused");
		Check(Fetch({ Body(kModBytes + "x") }).Says(mismatch), "download: a long file is refused");
		Check(Fetch({ Body("") }).Says(failed), "download: an interrupted answer is a failed download");
		Check(Fetch({ ModInstall::Hop() }).Says(failed), "download: no answer is a failed download");
		Check(Fetch({ To(kSignedHop), ModInstall::Hop() }).Says(failed), "download: the file's host not answering is a failed download");
		for (unsigned status : { 204u, 206u, 300u, 304u, 403u, 404u, 500u })
		{
			const Fetched f = Fetch({ { status, kSignedHop, kModBytes } });
			Check(f.Says(failed) && f.asked.size() == 1, "download: any other status fails");
		}

		const std::vector<std::string> badTargets = {
			"", "/owner/file", "//release-assets.githubusercontent.com/x", "release-assets.githubusercontent.com/x",
			"http://release-assets.githubusercontent.com/x", "file:///c:/x.dll",
			"https://example.test/x", "https://github.com/Owner/Repo/releases/download/v1/Thing.dll",
			"https://release-assets.githubusercontent.com.example.test/x", "https://xrelease-assets.githubusercontent.com/x",
			"https://raw.githubusercontent.com/x", "https://release-assets.githubusercontent.com./x",
			"https://release-assets.githubusercontent.com:443/x", "https://user@release-assets.githubusercontent.com/x",
			"https://release-assets.githubusercontent.com@example.test/x", "https://example.test\\@release-assets.githubusercontent.com/x",
			"https://example.test%2f.release-assets.githubusercontent.com/x", "https://release-assets.githubusercontent.com/x y",
			"https://release-assets.githubusercontent.com/x\r\nHost: example.test",
		};
		for (const std::string& target : badTargets)
		{
			const Fetched f = Fetch({ To(target), Body(kModBytes) });
			Check(f.Says(refused) && f.asked.size() == 1, ("download: redirect refused: " + target).c_str());
		}
		{
			const Fetched f = Fetch({ To(kSignedHop), To("https://example.test/x"), Body(kModBytes) });
			Check(f.Says(refused) && f.asked.size() == 2, "download: a later hop is checked too");
		}

		const std::string next = "https://objects.githubusercontent.com/next";
		{
			const Fetched three = Fetch({ To(kSignedHop), To(next), To(kSignedHop), Body(kModBytes) });
			Check(three.ok && three.asked.size() == 4, "download: three redirects are followed");
			const Fetched four = Fetch({ To(kSignedHop), To(next), To(kSignedHop), To(next), Body(kModBytes) });
			Check(four.Says(refused) && four.asked.size() == 4, "download: a fourth redirect is refused");
		}

		for (const char* start : { "http://github.com/Owner/Repo/releases/download/v1.0.0/Thing.dll",
			"https://example.test/Owner/Repo/releases/download/v1.0.0/Thing.dll",
			"https://release-assets.githubusercontent.com/x" })
		{
			const GameDir dir;
			ModIndex::InstallInfo info = RequestFor(dir).info;
			info.url = start;
			const Fetched f = Fetch({ Body(kModBytes) }, &info);
			Check(!f.ok && f.asked.empty(), "download: a wrong first address is never asked for");
		}
		{
			const GameDir dir;
			ModIndex::InstallInfo info = RequestFor(dir).info;
			info.size = static_cast<size_t>(ModIndex::kMaxDownloadBytes) + 1;
			const Fetched f = Fetch({ Body(kModBytes) }, &info);
			Check(!f.ok && f.asked.empty(), "download: a size over the limit is never asked for");
		}

		// cancel: before the first request, and while one is answering
		{
			const GameDir dir;
			std::atomic<bool> cancel{ true };
			int calls = 0;
			std::string bytes, why;
			const bool before = ModInstall::Download(RequestFor(dir).info, "Thing",
				[&](const std::string&, size_t, unsigned, const std::atomic<bool>&) { ++calls; return Body(kModBytes); },
				cancel, bytes, why);
			Check(!before && calls == 0 && bytes.empty(), "download: cancelled before it starts, nothing is asked for");
			cancel = false;
			const bool during = ModInstall::Download(RequestFor(dir).info, "Thing",
				[&](const std::string&, size_t, unsigned, const std::atomic<bool>&) { cancel = true; return Body(kModBytes); },
				cancel, bytes, why);
			Check(!during && bytes.empty() && why.find("stopped") != std::string::npos,
				"download: cancelled while answering, the bytes are dropped");
		}

		// the loader's own hop: one request that follows nothing
		{
			Serve("the body");
			std::atomic<bool> cancel{ false };
			ModInstall::HttpHop()(kAssetUrl, 77, 5000, cancel);
			std::lock_guard<std::mutex> lock(g_httpMutex);
			Check(g_httpUrl == kAssetUrl && g_httpOptions.noRedirect && !g_httpOptions.allowPlainHttp && g_httpOptions.location
				&& g_httpOptions.status && g_httpOptions.maxBytes == 77 && g_httpOptions.deadlineMs == 5000 && g_httpOptions.cancel == &cancel,
				"hop: HTTPS only, no redirect followed, capped, with a deadline and cancel");
		}
	}

	void PlaceTests()
	{
		const std::string sidecar = std::string("{\n  \"manifest_url\": \"") + kOwnManifest + "\"\n}\n";
		std::string why;
		{
			const GameDir dir;
			Check(ModInstall::Place(RequestFor(dir), kModBytes, why) && why.empty(), "place: written");
			Check(dir.Listing() == "Thing.dll,Thing.json", "place: the DLL and its .json, no temp files");
			Check(Slurp(dir.At(L"Thing.dll")) == kModBytes && Slurp(dir.At(L"Thing.json")) == sidecar, "place: both contents");
		}
		{
			const GameDir dir;
			ModInstall::Request r = RequestFor(dir);
			r.info.manifestUrl.clear();
			Spit(dir.At(L"Thing.json"), "mine");
			Check(ModInstall::Place(r, kModBytes, why) && dir.Listing() == "Thing.dll,Thing.json" && Slurp(dir.At(L"Thing.json")) == "mine",
				"place: with no manifest only the DLL is written");
		}
		for (const wchar_t* there : { L"Thing.dll", L"thing.DLL", L"Thing.json" })
		{
			const GameDir dir;
			Spit(dir.At(there), "mine");
			const std::string before = dir.Listing();
			Check(!ModInstall::Place(RequestFor(dir), kModBytes, why) && why.find("already in") != std::string::npos,
				"place: an existing file is refused");
			Check(dir.Listing() == before && Slurp(dir.At(there)) == "mine", "place: and nothing changes");
		}
		// a file that appears after the check, just before its rename
		for (const char* late : { "Thing.dll", "Thing.json" })
		{
			const GameDir dir;
			const std::wstring target = dir.plugins + L"\\" + std::wstring(late, late + strlen(late));
			const bool ok = ModInstall::Place(RequestFor(dir), kModBytes, why,
				[&](const std::wstring& from, const std::wstring& to)
				{
					if (to == target)
						Spit(target, "theirs");
					return PluginManage::MoveOnSameVolume(from, to);
				});
			Check(!ok && !why.empty(), "place: a file that appears before the rename fails the install");
			Check(dir.Listing() == late && Slurp(target) == "theirs", "place: it is left alone, and nothing of ours stays");
		}
		{
			const GameDir dir;
			Spit(dir.At(L"Thing.dll.install.tmp"), "left over");
			Check(ModInstall::Place(RequestFor(dir), kModBytes, why) && dir.Listing() == "Thing.dll,Thing.json"
				&& Slurp(dir.At(L"Thing.dll")) == kModBytes, "place: a temp file of an earlier attempt is replaced");
		}
		// an X.json left by an install that was stopped between its two renames:
		// no DLL beside it and the text this install writes, so it is ours and is
		// replaced. The same name with other text is the player's.
		{
			const GameDir dir;
			Spit(dir.At(L"Thing.json"), sidecar);
			Check(ModInstall::CheckTarget(RequestFor(dir)).empty(), "check: a stranded sidecar of ours does not refuse");
			Check(ModInstall::Place(RequestFor(dir), kModBytes, why) && dir.Listing() == "Thing.dll,Thing.json"
				&& Slurp(dir.At(L"Thing.dll")) == kModBytes && Slurp(dir.At(L"Thing.json")) == sidecar,
				"place: a stranded sidecar of ours is replaced");
		}
		for (const std::string& text : { sidecar + " ", std::string("{}"), std::string() })
		{
			const GameDir dir;
			Spit(dir.At(L"Thing.json"), text);
			Check(!ModInstall::Place(RequestFor(dir), kModBytes, why) && why.find("Thing.json is already in") != std::string::npos
				&& dir.Listing() == "Thing.json" && Slurp(dir.At(L"Thing.json")) == text,
				"place: a sidecar with other text is the player's and is kept");
		}
		// the delete re-checks the text: an X.json that someone else wrote after the
		// check is not ours to remove
		{
			const GameDir dir;
			Spit(dir.At(L"Thing.json"), "theirs");
			ModInstall::ClearStranded(RequestFor(dir));
			Check(dir.Listing() == "Thing.json" && Slurp(dir.At(L"Thing.json")) == "theirs",
				"clear: an X.json that is not our text is not deleted");
			Spit(dir.At(L"Thing.json"), sidecar);
			ModInstall::ClearStranded(RequestFor(dir));
			Check(dir.Listing().empty(), "clear: an X.json that holds our text is deleted");
		}
		// a rename held up by a scanner is tried again; any other error is final
		for (const DWORD error : { ERROR_SHARING_VIOLATION, ERROR_ACCESS_DENIED })
		{
			const GameDir dir;
			int dllMoves = 0;
			const bool ok = ModInstall::Place(RequestFor(dir), kModBytes, why,
				[&](const std::wstring& from, const std::wstring& to)
				{
					if (to.compare(to.size() - 4, 4, L".dll") == 0 && ++dllMoves <= 2)
						return error;
					return PluginManage::MoveOnSameVolume(from, to);
				});
			Check(ok && dllMoves == 3 && dir.Listing() == "Thing.dll,Thing.json" && Slurp(dir.At(L"Thing.dll")) == kModBytes,
				"place: a rename that fails twice with a lock error is retried and succeeds");
		}
		{
			const GameDir dir;
			int moves = 0;
			const bool ok = ModInstall::Place(RequestFor(dir), kModBytes, why,
				[&](const std::wstring& from, const std::wstring& to)
				{
					if (to.compare(to.size() - 4, 4, L".dll") != 0)
						return PluginManage::MoveOnSameVolume(from, to);
					++moves;
					return static_cast<DWORD>(ERROR_SHARING_VIOLATION);
				});
			Check(!ok && moves == 4 && !why.empty() && dir.Listing().empty(),
				"place: a lock that never lets go gives up after three retries and leaves nothing");
		}
		{
			const GameDir dir;
			int moves = 0;
			const bool ok = ModInstall::Place(RequestFor(dir), kModBytes, why,
				[&](const std::wstring&, const std::wstring&) { ++moves; return static_cast<DWORD>(ERROR_ALREADY_EXISTS); });
			Check(!ok && moves == 1 && dir.Listing().empty(), "place: a name that is taken is not retried");
		}
		{
			const GameDir dir;
			ModInstall::Request r = RequestFor(dir);
			r.info.dllFile = "..\\Thing.dll";
			Check(!ModInstall::Place(r, kModBytes, why) && dir.Listing().empty()
				&& !Exists(dir.game + L"\\ModLoader\\Thing.dll"), "place: a name with a path is refused");
		}
		{
			const GameDir dir;
			Spit(dir.game + L"\\vortex.deployment.json", "{\"files\": [{\"relPath\": \"ModLoader\\\\Plugins\\\\Thing.pdb\"}]}");
			Check(!ModInstall::Place(RequestFor(dir), kModBytes, why) && why.find("Vortex") != std::string::npos && dir.Listing().empty(),
				"place: a mod Vortex manages is refused");
			Check(ModInstall::CheckTarget(RequestFor(dir)) == why, "check: the same words before the download");
		}

		using ModInstall::Need;
		Check(ModInstall::RequirementMessage("MainMap", {}).empty()
			&& ModInstall::RequirementMessage("MainMap", { { "MiniMap", Need::Met } }).empty(), "requires: met");
		Check(ModInstall::RequirementMessage("MainMap", { { "A", Need::Met }, { "MiniMap", Need::Missing } })
			== "MainMap needs MiniMap. Install MiniMap first.", "requires: missing");
		Check(ModInstall::RequirementMessage("MainMap", { { "MiniMap", Need::Disabled }, { "B", Need::Missing } })
			== "MainMap needs MiniMap. Enable MiniMap first.", "requires: disabled, the first unmet one is named");
	}

	template <class F>
	bool Until(F done)
	{
		const auto end = Clock::now() + std::chrono::seconds(5);
		while (!done() && Clock::now() < end)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		return done();
	}

	// An install's `done`, which can be waited for.
	struct Run
	{
		std::atomic<int> calls{ 0 };
		ModInstall::Outcome outcome;
		ModInstall::DoneFn Done()
		{
			return [this](const ModInstall::Outcome& o) { outcome = o; ++calls; };
		}
		bool Wait() { return Until([this] { return calls.load() > 0; }); }
	};

	void InstallerTests()
	{
		{
			const GameDir dir;
			std::atomic<bool> release{ false };
			std::atomic<int> hops{ 0 };
			Run first, second, again;
			ModInstall::Installer installer([&](const std::string&, size_t, unsigned, const std::atomic<bool>&)
			{
				++hops;
				Until([&] { return release.load(); });
				return Body(kModBytes);
			});
			Check(installer.Start(RequestFor(dir), first.Done()) && installer.Busy(), "installer: starts, and is busy");
			Check(!installer.Start(RequestFor(dir), second.Done()), "installer: one install at a time");
			release = true;
			Check(first.Wait() && first.outcome.placed && first.outcome.dllFile == "Thing.dll" && first.outcome.version == "1.0.0",
				"installer: done says what was placed");
			Check(Until([&] { return !installer.Busy(); }) && second.calls == 0 && hops == 1 && dir.Listing() == "Thing.dll,Thing.json",
				"installer: free again, and the refused start did nothing");

			Check(installer.Start(RequestFor(dir), again.Done()) && again.Wait() && !again.outcome.placed
				&& again.outcome.message.find("already in") != std::string::npos && hops == 1,
				"installer: a file already there stops it before the download");
			Check(!installer.Halted(), "installer: not halted before Shutdown");
			installer.Shutdown(1000);
			Check(installer.Halted() && !installer.Start(RequestFor(dir), again.Done()),
				"installer: halted, and nothing starts, after Shutdown");
		}

		// a failure ends in done, with the folder as it was: wrong bytes, a request
		// that throws, and a DLL that appears during the download
		for (int kind = 0; kind < 3; ++kind)
		{
			const GameDir dir;
			Run run;
			ModInstall::Installer installer([&](const std::string&, size_t, unsigned, const std::atomic<bool>&) -> ModInstall::Hop
			{
				if (kind == 0)
					return Body(std::string(kModBytes.size(), 'z'));
				if (kind == 1)
					throw std::runtime_error("boom");
				Spit(dir.At(L"Thing.dll"), "theirs");
				return Body(kModBytes);
			});
			Check(installer.Start(RequestFor(dir), run.Done()) && run.Wait() && !run.outcome.placed && !run.outcome.message.empty(),
				"installer: a failed install says why");
			Check(dir.Listing() == (kind == 2 ? "Thing.dll" : "") && (kind != 2 || Slurp(dir.At(L"Thing.dll")) == "theirs"),
				"installer: and leaves the folder as it was");
			installer.Shutdown(1000);
		}

		// Shutdown during the download: with a request that notices, and with one
		// that answers with the whole file anyway
		for (const bool noticesCancel : { true, false })
		{
			const GameDir dir;
			std::atomic<bool> started{ false }, release{ false };
			Run run;
			ModInstall::Installer installer([&](const std::string&, size_t, unsigned, const std::atomic<bool>& cancel)
			{
				started = true;
				Until([&] { return release.load() || (noticesCancel && cancel.load()); });
				return Body(kModBytes);
			});
			installer.Start(RequestFor(dir), run.Done());
			Until([&] { return started.load(); });
			const bool ended = installer.Shutdown(noticesCancel ? 4000 : 0);
			Check(ended == noticesCancel, "shutdown: says whether the worker has ended");
			release = true;
			Check(Until([&] { return !installer.Busy(); }), "shutdown: the worker ends");
			Check(run.calls == 0 && dir.Listing().empty(), "shutdown: nothing is written and done is not called");
		}
	}

	void SignatureStateTests()
	{
		const Signer sk2 = MakeSigner("k2", 2);
		const std::string url = "https://example.test/mod-index.v1.txt";
		const std::string idx = IndexOf("aaa");
		const auto verified = [&](bool signedOnly, const std::string& served)
		{
			Source src;
			src.keys = { sk2.pub };
			src.cachePath = TempCache();
			src.signedOnly = signedOnly;
			src.bodySigned = std::make_shared<bool>(!signedOnly);
			std::shared_ptr<const bool> flag = src.bodySigned;
			const std::wstring cache = src.cachePath;
			Serve(served);
			Fetcher f(url, MakeFetch(std::move(src)), std::move(flag));
			f.RequestFirstLoad();
			Check(WaitFor(f, State::Ready), "signature state: the list loads");
			const bool result = f.Get()->signatureVerified;
			f.Shutdown(1000);
			RemoveCache(cache);
			return result;
		};
		Check(verified(true, SignedFile(sk2, 2, idx)), "signature state: the loader's own list is verified");
		Check(!verified(false, idx), "signature state: a bare list from ModIndexUrl is not");
		Check(verified(false, SignedFile(sk2, 2, idx)), "signature state: a signed file from ModIndexUrl is");
	}

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

// The Browse tab's search box and the server-only label.
void SearchTests()
{
	const std::string mini = "A minimap that follows the player.";
	const auto hit = [&](const char* query, const char* name, const char* author = "Kian", const std::string& text = std::string())
	{
		return MatchesSearch(SearchTokens(query), name, author, text.empty() ? mini : text);
	};

	// the loose match
	Check(hit("minmap", "MiniMap"), "search: minmap finds MiniMap");
	Check(!hit("mm", "MiniMap"), "search: a word under 3 letters does not match by letters alone");
	Check(hit("mi", "MiniMap", "Someone", "Shows a view.") && hit("ki", "Kian", "Someone", "Shows a view."),
		"search: a word of 1 or 2 letters matches as a piece of the name");
	Check(!hit("mm", "MiniMap", "Someone", "Shows a view."), "search: and not as letters spread over the name");
	Check(!hit("minmapx", "MiniMap"), "search: letters out of the name do not match");
	Check(!hit("mapmini", "MiniMap", "Kian", "Shows a view."), "search: letters in the wrong order do not match");

	// case
	Check(hit("MINIMAP", "MiniMap") && hit("miniMAP", "minimap"), "search: case is ignored");
	Check(hit("kian", "MiniMap") && hit("KIAN", "MiniMap"), "search: the author is searched");
	Check(hit("player", "MiniMap") && hit("FOLLOWS", "MiniMap"), "search: the description is searched");
	Check(!hit("zzz", "MiniMap"), "search: no match");

	// every word, any order
	Check(hit("map mini", "MiniMap"), "search: words match in any order");
	Check(hit("kian minimap player", "MiniMap"), "search: words may come from name, author and description");
	Check(!hit("minimap zzz", "MiniMap"), "search: one word that matches nothing fails the listing");
	Check(hit("  mini 	 map  ", "MiniMap"), "search: extra spaces and tabs are ignored");

	// the description is never matched by letters alone
	Check(!hit("mnmp", "Codex", "AlienX", "A very long description of many things, maybe not mapped."), "search: letters across a description do not match");
	Check(hit("mnmp", "MiniMap"), "search: the letters of a longer word may be spread over the name");

	// nothing typed
	Check(SearchTokens("").empty() && SearchTokens("  	 ").empty(), "search: an empty or blank search has no words");
	Check(MatchesSearch(SearchTokens(""), "Anything", "", ""), "search: an empty search shows every listing");

	// the entry overload
	{
		const ParseResult r = ParseModIndex("[" + Obj("a", "WAILA", kGood, ", \"author\": \"AlienXAXS\", \"summary\": \"Shows what you look at\"") + "]");
		Check(r.entries.size() == 1 && MatchesSearch(SearchTokens("wla alien"), r.entries[0]) && !MatchesSearch(SearchTokens("minimap"), r.entries[0]),
			"search: an entry matches on its name, author and summary");
	}

	// server_only
	{
		const ParseResult r = ParseModIndex("[" + Obj("a", "A", kGood, ", \"server_only\": true") + "," + Obj("b", "B", kGood, ", \"server_only\": false") + "," +
			Obj("c", "C", kGood) + "," + Obj("d", "D", kGood, ", \"server_only\": \"true\"") + "]");
		Check(r.entries.size() == 4 && r.entries[0].serverOnly && !r.entries[1].serverOnly && !r.entries[2].serverOnly && !r.entries[3].serverOnly,
			"server_only: only a literal true marks an entry");
	}
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
	InstallParserTests();
	ThumbnailParserTests();
	ThumbnailHeaderTests();
	DownloadTests();
	PlaceTests();
	InstallerTests();
	SignatureStateTests();
	SearchTests();

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed ? 1 : 0;
}
