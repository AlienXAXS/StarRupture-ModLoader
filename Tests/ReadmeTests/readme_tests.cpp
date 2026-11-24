// ReadmeTests: the README reader (auto_update/readme_markdown.cpp), the picture
// and link address rules (readme_images.h) and the README fetcher's states
// (readme_fetcher.cpp) through an injected download. No network, no game.

#include "auto_update/http_get.h"
#include "auto_update/readme_fetcher.h"
#include "auto_update/readme_images.h"
#include "auto_update/readme_markdown.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The loader's default request lands here; the tests never use it for real.
std::string HttpGet(const char* /*url*/, const char* /*label*/, const HttpGetOptions& /*options*/)
{
	return {};
}

using namespace ModIndex;

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

	const std::string kReadmeUrl = "https://raw.githubusercontent.com/o/r/main/docs/README.md";
	const std::string kRaw = "https://raw.githubusercontent.com/o/r/main/";

	Document P(const std::string& text)
	{
		return ParseReadme(text, kReadmeUrl);
	}

	const Block* First(const Document& d, BlockKind kind)
	{
		for (const Block& b : d.blocks)
			if (b.kind == kind)
				return &b;
		return nullptr;
	}

	size_t Count(const Document& d, BlockKind kind)
	{
		size_t n = 0;
		for (const Block& b : d.blocks)
			if (b.kind == kind)
				++n;
		return n;
	}

	std::string AllText(const Block& b)
	{
		std::string t;
		for (const Span& s : b.spans)
			t += s.text;
		return t;
	}

	bool ValidUtf8(const std::string& s)
	{
		size_t i = 0;
		while (i < s.size())
		{
			const unsigned char c = static_cast<unsigned char>(s[i]);
			size_t len = 0;
			unsigned cp = 0;
			if (c < 0x80) { ++i; continue; }
			if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
			else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
			else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
			else return false;
			if (i + len > s.size())
				return false;
			for (size_t k = 1; k < len; ++k)
			{
				const unsigned char cc = static_cast<unsigned char>(s[i + k]);
				if ((cc & 0xC0) != 0x80)
					return false;
				cp = (cp << 6) | (cc & 0x3F);
			}
			if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000))
				return false;
			i += len;
		}
		return true;
	}

	// The limits and the text rules every Document must keep, whatever went in.
	bool Sane(const Document& d)
	{
		if (d.blocks.size() > kMaxBlocks)
			return false;
		for (const Block& b : d.blocks)
		{
			if (b.spans.size() > kMaxSpans || b.code.size() > kMaxCodeBytes)
				return false;
			if (!ValidUtf8(b.code) || !ValidUtf8(b.alt) || !ValidUtf8(b.url))
				return false;
			if (b.level < 0 || b.level > 3 || b.depth < 0 || b.depth > 1)
				return false;
			for (const Span& s : b.spans)
			{
				if (s.text.size() > kMaxSpanBytes || !ValidUtf8(s.text))
					return false;
				if (s.text.find('\0') != std::string::npos)
					return false;
			}
		}
		return true;
	}

	std::string Repeat(const std::string& unit, size_t times)
	{
		std::string s;
		s.reserve(unit.size() * times);
		for (size_t i = 0; i < times; ++i)
			s += unit;
		return s;
	}

	// ---- markdown --------------------------------------------------------

	void TestBlocks()
	{
		Document d = P("# One\n\n## Two\n\n### Three\n\n#### Four\n\nBody text\n");
		Check(d.blocks.size() == 5, "five blocks");
		Check(d.blocks[0].kind == BlockKind::Heading && d.blocks[0].level == 1 && AllText(d.blocks[0]) == "One", "h1");
		Check(d.blocks[1].level == 2, "h2");
		Check(d.blocks[2].level == 3, "h3");
		Check(d.blocks[3].kind == BlockKind::Heading && d.blocks[3].level == 3, "h4 counts as level 3");
		Check(d.blocks[4].kind == BlockKind::Paragraph && AllText(d.blocks[4]) == "Body text", "paragraph");
		Check(!d.shortened, "not shortened");

		d = P("Title\n=====\n\nSub\n---\n\ntext\n");
		Check(d.blocks.size() == 3 && d.blocks[0].kind == BlockKind::Heading && d.blocks[0].level == 1, "setext h1");
		Check(d.blocks.size() == 3 && d.blocks[1].kind == BlockKind::Heading && d.blocks[1].level == 2, "setext h2");

		d = P("- a\n- b\n  - nested\n\n1. one\n2. two\n");
		Check(Count(d, BlockKind::ListItem) == 5, "five list items");
		Check(d.blocks.size() == 5 && !d.blocks[0].ordered && d.blocks[0].depth == 0, "bullet item");
		Check(d.blocks.size() == 5 && d.blocks[2].depth == 1, "nested item depth 1");
		Check(d.blocks.size() == 5 && d.blocks[3].ordered && d.blocks[3].number == 1 && d.blocks[4].number == 2, "numbered items");

		d = P("```\nint x = 1;\n  indented\n```\n\nafter\n");
		Check(d.blocks.size() == 2 && d.blocks[0].kind == BlockKind::CodeBlock, "fenced code");
		Check(d.blocks[0].code == "int x = 1;\n  indented", "fenced code text");

		d = P("para\n\n    code line\n    second\n\nafter\n");
		const Block* code = First(d, BlockKind::CodeBlock);
		Check(code && code->code == "code line\nsecond", "indented code");

		d = P("above\n\n---\n\nbelow\n");
		Check(Count(d, BlockKind::Rule) == 1, "horizontal rule");

		d = P("> quoted text\n");
		Check(d.blocks.size() == 1 && AllText(d.blocks[0]) == "quoted text", "quote mark dropped");
	}

	void TestSpans()
	{
		Document d = P("Some **bold** and `code` and [link](https://github.com/o/r) and [other](https://example.com/x) end\n");
		Check(d.blocks.size() == 1, "one paragraph");
		const Block& b = d.blocks[0];
		bool bold = false, code = false, link = false, other = false;
		for (const Span& s : b.spans)
		{
			if (s.kind == SpanKind::Bold && s.text == "bold") bold = true;
			if (s.kind == SpanKind::Code && s.text == "code") code = true;
			if (s.kind == SpanKind::Link && s.text == "link" && s.url == "https://github.com/o/r" && s.clickable) link = true;
			if (s.kind == SpanKind::Link && s.text == "other" && !s.clickable) other = true;
		}
		Check(bold, "bold span");
		Check(code, "code span");
		Check(link, "github link is clickable");
		Check(other, "other host link is shown but not clickable");

		d = P("See <https://github.com/o/r> and https://www.nexusmods.com/starrupture/mods/1 now\n");
		size_t links = 0;
		bool clickable = true;
		for (const Span& s : d.blocks[0].spans)
			if (s.kind == SpanKind::Link)
			{
				++links;
				clickable = clickable && s.clickable;
			}
		Check(links == 2 && clickable, "autolink and bare link");

		d = P("one  \ntwo\n");
		Check(d.blocks.size() == 1 && AllText(d.blocks[0]).find('\n') != std::string::npos, "hard line break");
	}

	void TestHtml()
	{
		Document d = P("<img src=\"" + kRaw + "pic.png\" alt=\"A pic\">\n");
		const Block* img = First(d, BlockKind::Image);
		Check(img && img->url == kRaw + "pic.png" && img->alt == "A pic", "html img");

		d = P("line one<br>line two\n");
		Check(d.blocks.size() == 1 && AllText(d.blocks[0]) == "line one\nline two", "br is a line break");

		d = P("<div align=\"center\"><b>kept</b> text</div>\n");
		Check(d.blocks.size() == 1 && AllText(d.blocks[0]).find("kept") != std::string::npos && AllText(d.blocks[0]).find('<') == std::string::npos, "other tags dropped, text kept");

		d = P("before <!-- hidden --> after\n");
		Check(d.blocks.size() == 1 && AllText(d.blocks[0]).find("hidden") == std::string::npos && AllText(d.blocks[0]).find("after") != std::string::npos, "comment dropped");

		d = P("Tom &amp; Jerry &lt;3\n");
		Check(d.blocks.size() == 1 && AllText(d.blocks[0]) == "Tom & Jerry <3", "entities decoded");

		d = P("a <!-- never ends\nstill text\n");
		Check(Sane(d), "unterminated comment is sane");
		d = P("<img src=\"" + kRaw + "pic.svg\" alt=\"logo\">\n");
		img = First(d, BlockKind::Image);
		Check(img && img->url.empty() && img->alt == "logo", "svg img refused, alt kept");
	}

	void TestImages()
	{
		Document d = P("![shot](" + kRaw + "a.png)\n");
		const Block* img = First(d, BlockKind::Image);
		Check(img && img->url == kRaw + "a.png" && img->alt == "shot" && img->imageSlot == -1, "markdown image");

		d = P("![rel](img/b.png)\n");
		img = First(d, BlockKind::Image);
		Check(img && img->url == kRaw + "docs/img/b.png", "relative image resolves against the readme folder");

		d = P("![up](../c.png)\n");
		img = First(d, BlockKind::Image);
		Check(img && img->url == kRaw + "c.png", "one level up");

		d = P("![esc](../../../../../d.png)\n");
		img = First(d, BlockKind::Image);
		Check(img && img->url == kRaw + "d.png", ".. never climbs above the root");

		d = P("![root](/e.png)\n");
		img = First(d, BlockKind::Image);
		Check(img && img->url == kRaw + "e.png", "leading slash is the root");

		d = P("![bad](https://evil.example.com/x.png)\n");
		img = First(d, BlockKind::Image);
		Check(img && img->url.empty() && img->alt == "bad", "foreign host refused, alt kept");

		d = P("![](https://evil.example.com/x.png)\n");
		Check(Count(d, BlockKind::Image) == 0, "refused image without alt is skipped");

		d = P("[![badge](" + kRaw + "badge.png)](https://github.com/o/r)\n");
		img = First(d, BlockKind::Image);
		Check(img && img->url == kRaw + "badge.png", "linked image");

		std::string longUrl = "https://raw.githubusercontent.com/o/r/main/";
		longUrl += std::string(kMaxUrlLen + 1 - longUrl.size(), 'a');
		d = P("![long](" + longUrl + ")\n");
		Check(Count(d, BlockKind::Image) == 0, "image address over the limit is skipped");
	}

	void TestLimits()
	{
		// Text length: exactly the limit is kept whole, one byte over is cut.
		Document d = P(Repeat("a", kMaxReadmeBytes));
		Check(!d.shortened, "text at the limit is not shortened");
		d = P(Repeat("a", kMaxReadmeBytes + 1));
		Check(d.shortened, "text over the limit is shortened");
		Check(Sane(d), "over-limit text is sane");

		// Blocks.
		d = P(Repeat("x\n\n", kMaxBlocks));
		Check(d.blocks.size() == kMaxBlocks && !d.shortened, "exactly kMaxBlocks blocks");
		d = P(Repeat("x\n\n", kMaxBlocks + 1));
		Check(d.blocks.size() == kMaxBlocks && d.shortened, "one block over: dropped, shortened");

		// Spans: 32 links with spaces between and a trailing word make exactly 64.
		std::string exact;
		for (int i = 0; i < 32; ++i)
			exact += "[a](https://github.com/o/r) ";
		exact += "x";
		d = P(exact);
		Check(d.blocks.size() == 1 && d.blocks[0].spans.size() == kMaxSpans, "exactly kMaxSpans spans");
		d = P(Repeat("[a](https://github.com/o/r) ", 40) + "x");
		Check(d.blocks.size() == 1 && d.blocks[0].spans.size() == kMaxSpans, "spans over the limit are cut");
		d = P(Repeat("`a` b ", 200));
		Check(d.blocks.size() == 1 && d.blocks[0].spans.size() <= kMaxSpans, "code and plain alternating stays within the limit");

		// Span bytes.
		d = P(Repeat("a", kMaxSpanBytes));
		Check(d.blocks.size() == 1 && d.blocks[0].spans.size() == 1 && d.blocks[0].spans[0].text.size() == kMaxSpanBytes, "span at the byte limit is one span");
		d = P(Repeat("a", kMaxSpanBytes + 1));
		Check(d.blocks.size() == 1 && d.blocks[0].spans.size() == 2 && AllText(d.blocks[0]).size() == kMaxSpanBytes + 1, "span one byte over continues in the next");

		// Code bytes.
		d = P("```\n" + Repeat("a", kMaxCodeBytes) + "\n```\n");
		const Block* code = First(d, BlockKind::CodeBlock);
		Check(code && code->code.size() == kMaxCodeBytes, "code at the limit is kept whole");
		d = P("```\n" + Repeat("a", kMaxCodeBytes + 500) + "\n```\n");
		code = First(d, BlockKind::CodeBlock);
		Check(code && code->code.size() == kMaxCodeBytes, "code over the limit is cut");
		d = P("```\n" + Repeat("line\n", 2000) + "```\n");
		code = First(d, BlockKind::CodeBlock);
		Check(code && code->code.size() <= kMaxCodeBytes, "many code lines are capped");
		d = P("para\n\n    start\n" + Repeat("\n", 20000) + "    end\n");
		Check(Sane(d), "blank lines in indented code are capped");

		// URL length: 200 bytes is a link, 201 is plain text.
		std::string at200 = "https://github.com/";
		at200 += std::string(kMaxUrlLen - at200.size(), 'a');
		d = P("[t](" + at200 + ")\n");
		bool linkAt200 = false;
		for (const Span& s : d.blocks[0].spans)
			linkAt200 = linkAt200 || (s.kind == SpanKind::Link && s.clickable);
		Check(linkAt200, "link address at the limit is a clickable link");
		d = P("[t](" + at200 + "a)\n");
		bool anyLink = false;
		for (const Span& s : d.blocks[0].spans)
			anyLink = anyLink || s.kind == SpanKind::Link;
		Check(!anyLink && AllText(d.blocks[0]).find('t') != std::string::npos, "link address over the limit is plain text");
	}

	void TestHostile()
	{
		uint32_t state = 0x1234ABCDu;
		auto next = [&state]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };

		bool allSane = true;
		for (int round = 0; round < 200; ++round)
		{
			std::string in;
			const size_t len = next() % 4096;
			for (size_t i = 0; i < len; ++i)
				in += static_cast<char>(next() & 0xFF);
			if (!Sane(P(in)))
				allSane = false;
		}
		Check(allSane, "seeded random bytes: always sane");

		// Random text built from markdown's own characters.
		const char alphabet[] = "[]()*_`<>!#-+. \n\t\\&;:/\"'|0123456789abcdefghij";
		bool allSane2 = true;
		for (int round = 0; round < 200; ++round)
		{
			std::string in;
			const size_t len = next() % 6000;
			for (size_t i = 0; i < len; ++i)
				in += alphabet[next() % (sizeof(alphabet) - 1)];
			if (!Sane(P(in)))
				allSane2 = false;
		}
		Check(allSane2, "seeded random markdown characters: always sane");

		const char* const nasty[] = { "[", "*", "`", "<", "(", "![", "](", "&", "<img src=\"", "**a" };
		for (const char* unit : nasty)
		{
			const auto t0 = std::chrono::steady_clock::now();
			Document d = P(Repeat(unit, 100 * 1024 / std::string(unit).size()));
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			char what[96];
			snprintf(what, sizeof(what), "100 KB of '%s': sane", unit);
			Check(Sane(d), what);
			snprintf(what, sizeof(what), "100 KB of '%s': under 3 s", unit);
			Check(ms < 3000, what);
		}

		Check(Sane(P(Repeat("> ", 20000))), "deeply nested quote");
		Check(Sane(P(Repeat("    ", 3000) + "- x\n")), "deep indent");
		std::string nest;
		for (int i = 0; i < 2000; ++i)
			nest += std::string(static_cast<size_t>(i % 40), ' ') + "- item\n";
		Check(Sane(P(nest)), "deep list nesting");
		Document d = P(nest);
		Check(Count(d, BlockKind::ListItem) > 0, "deep nesting still yields items");
		Check(Sane(P(Repeat("[", 5000) + "x" + Repeat("](u)", 5000))), "nested brackets");
		Check(Sane(P(Repeat("**", 20000) + "x")), "many bold marks");

		d = P("```\nnever closed\nmore text\n");
		const Block* code = First(d, BlockKind::CodeBlock);
		Check(code && code->code.find("more text") != std::string::npos, "unterminated fence keeps the code");
		Check(Sane(P("~~~\n```\n    x\n")), "mixed unterminated fences");

		std::string nul("a\0b\0c text\n", 11);
		d = P(nul);
		Check(Sane(d) && d.blocks.size() == 1, "NUL bytes");
		d = P(std::string("bad \xC0\xAF \xED\xA0\x80 \xFF \xE2\x82 end\n"));
		Check(Sane(d) && d.blocks.size() == 1 && AllText(d.blocks[0]).find("end") != std::string::npos, "invalid UTF-8");
		d = P(std::string("\xEF\xBB\xBF# Title\n"));
		Check(Sane(d), "byte order mark");

		const auto t0 = std::chrono::steady_clock::now();
		d = P(Repeat("Some **text** with `code` and [a](https://github.com/o/r) here.\n\n", 20000));
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		Check(Sane(d) && d.shortened, "1 MB input is cut and sane");
		Check(ms < 3000, "1 MB input under 3 s");
	}

	// ---- address and picture rules -----------------------------------------

	void TestAddressRules()
	{
		Check(IsAllowedImageUrl("https://raw.githubusercontent.com/o/r/main/a.png"), "raw image");
		Check(IsAllowedImageUrl("https://user-images.githubusercontent.com/1/abc.png"), "user-images");
		Check(IsAllowedImageUrl("https://github.com/o/r/raw/main/a.png"), "github raw path");
		Check(IsAllowedImageUrl("https://github.com/user-attachments/assets/abc"), "user-attachments");
		Check(!IsAllowedImageUrl("https://github.com/o/r/blob/main/a.png"), "github blob page refused");
		Check(!IsAllowedImageUrl("https://github.com/o/r"), "github repo page refused");
		Check(!IsAllowedImageUrl("http://raw.githubusercontent.com/o/r/main/a.png"), "plain http refused");
		Check(!IsAllowedImageUrl("https://example.com/a.png"), "other host refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com.evil.com/a.png"), "look-alike host refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com"), "no path refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com/"), "empty path refused");

		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com/o/r/main/a.svg"), "svg refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com/o/r/main/a.SVG"), "upper-case svg refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com/o/r/main/a.svgz"), "svgz refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com/o/r/main/a.svg?sanitize=true"), "svg with a query string refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com/o/r/main/a.svg#x"), "svg with a fragment refused");
		Check(!IsAllowedImageUrl("https://github.com/o/r/raw/main/a.svg?x=1"), "github svg with a query string refused");
		Check(IsAllowedImageUrl("https://raw.githubusercontent.com/o/r/main/a.png?x=a.svg"), "svg only in the query string is fine");

		Check(!IsAllowedImageUrl("https://user@raw.githubusercontent.com/o/r/main/a.png"), "user info refused");
		Check(!IsAllowedImageUrl("https://user:pw@raw.githubusercontent.com/o/r/main/a.png"), "user and password refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com:443/o/r/main/a.png"), "port refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com/o/r/main/a\\b.png"), "backslash refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com/o/r/main/a\x01.png"), "control character refused");
		Check(!IsAllowedImageUrl("https://raw.githubusercontent.com/o/r/main/\xC3\xA9.png"), "non-ASCII refused");
		std::string at200 = "https://raw.githubusercontent.com/o/r/main/";
		at200 += std::string(kMaxUrlLen - at200.size(), 'a');
		Check(IsAllowedImageUrl(at200), "address at 200 bytes");
		Check(!IsAllowedImageUrl(at200 + "a"), "address at 201 bytes");

		const std::string base = "https://raw.githubusercontent.com/o/r/main/docs/README.md";
		Check(ResolveImageUrl("a.png", base) == kRaw + "docs/a.png", "resolve plain");
		Check(ResolveImageUrl("./a.png", base) == kRaw + "docs/a.png", "resolve ./");
		Check(ResolveImageUrl("../a.png", base) == kRaw + "a.png", "resolve ../");
		Check(ResolveImageUrl("../../../../../a.png", base) == kRaw + "a.png", "resolve .. past the root");
		Check(ResolveImageUrl("sub/../a.png", base) == kRaw + "docs/a.png", "resolve inner ..");
		Check(ResolveImageUrl("/a.png", base) == kRaw + "a.png", "resolve leading slash");
		Check(ResolveImageUrl("a.png?x=1#frag", base) == kRaw + "docs/a.png?x=1", "query kept, fragment dropped");
		Check(ResolveImageUrl("//evil.com/a.png", base).empty(), "scheme-relative refused");
		Check(ResolveImageUrl("a.png", "https://github.com/o/r/blob/main/README.md").empty(), "base that is not raw refused");
		Check(ResolveImageUrl("a.png", "").empty(), "empty base");
		Check(ResolveImageUrl("https://x.example/a.png", base) == "https://x.example/a.png", "absolute address returned as written");
		Check(ResolveImageUrl("..", base).empty() || IsAllowedImageUrl(ResolveImageUrl("..", base)) == false, "bare .. names no file");

		Check(DetectImageFormat(std::string("\x89PNG\r\n\x1a\n") + "xxxx") == ImageFormat::Png, "png signature");
		Check(DetectImageFormat("\xFF\xD8\xFF\xE0xxxx") == ImageFormat::Jpeg, "jpeg signature");
		Check(DetectImageFormat("GIF89a....") == ImageFormat::Gif, "gif signature");
		Check(DetectImageFormat(std::string("RIFF\x10\x00\x00\x00WEBPVP8 ", 16)) == ImageFormat::WebP, "webp signature");
		std::string bmp(40, '\0');
		bmp[0] = 'B'; bmp[1] = 'M'; bmp[14] = 40;
		Check(DetectImageFormat(bmp) == ImageFormat::Bmp, "bmp signature");
		Check(DetectImageFormat("<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>") == ImageFormat::None, "svg is not a picture");
		Check(DetectImageFormat("<html></html>") == ImageFormat::None, "html is not a picture");
		Check(DetectImageFormat("") == ImageFormat::None, "empty is not a picture");
		Check(LooksLikeMarkup("<svg></svg>"), "svg is markup");
		Check(LooksLikeMarkup("\xEF\xBB\xBF  \n<?xml version=\"1.0\"?>"), "xml after BOM is markup");
		Check(LooksLikeMarkup("<!DOCTYPE html>"), "doctype is markup");
		Check(LooksLikeMarkup("<HTML>"), "upper-case html is markup");
		Check(!LooksLikeMarkup(std::string("\x89PNG\r\n\x1a\n")), "png is not markup");
	}

	// ---- the fetcher -----------------------------------------------------

	struct Fake
	{
		std::mutex m;
		std::condition_variable cv;
		std::map<std::string, std::string> bodies;
		std::map<std::string, std::string> errors;      // url -> error text for a failed download
		std::vector<std::string> calls;
		std::string gateUrl;                            // a fetch of this url waits until Release()
		bool gateOpen = true;
		bool gateHit = false;
		int finished = 0;

		int CallsTo(const std::string& url)
		{
			std::lock_guard<std::mutex> lock(m);
			int n = 0;
			for (const std::string& c : calls)
				if (c == url)
					++n;
			return n;
		}
		void Gate(const std::string& url)
		{
			std::lock_guard<std::mutex> lock(m);
			gateUrl = url;
			gateOpen = false;
			gateHit = false;
		}
		bool WaitGateHit()
		{
			std::unique_lock<std::mutex> lock(m);
			return cv.wait_for(lock, std::chrono::seconds(5), [this] { return gateHit; });
		}
		void Release()
		{
			std::lock_guard<std::mutex> lock(m);
			gateOpen = true;
			cv.notify_all();
		}
		bool WaitFinished(int n)
		{
			std::unique_lock<std::mutex> lock(m);
			return cv.wait_for(lock, std::chrono::seconds(5), [this, n] { return finished >= n; });
		}
	};

	ReadmeFetchFn MakeFetch(std::shared_ptr<Fake> fake)
	{
		return [fake](const std::string& url, size_t /*maxBytes*/, std::string& error) -> std::string {
			std::unique_lock<std::mutex> lock(fake->m);
			fake->calls.push_back(url);
			if (url == fake->gateUrl)
			{
				fake->gateHit = true;
				fake->cv.notify_all();
				fake->cv.wait(lock, [&] { return fake->gateOpen; });
			}
			std::string out;
			const auto body = fake->bodies.find(url);
			if (body != fake->bodies.end())
				out = body->second;
			else
			{
				const auto err = fake->errors.find(url);
				error = err != fake->errors.end() ? err->second : "";
			}
			++fake->finished;
			fake->cv.notify_all();
			return out;
		};
	}

	std::string PngBytes(size_t total)
	{
		std::string s = std::string("\x89PNG\r\n\x1a\n");
		if (total > s.size())
			s.append(total - s.size(), 'x');
		return s;
	}

	template <class Pred>
	bool WaitFor(ReadmeFetcher& f, Pred pred)
	{
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < until)
		{
			if (pred(*f.Get()))
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return false;
	}

	bool ImagesSettled(const ReadmeSnapshot& s)
	{
		if (s.state != ReadmeState::Ready)
			return false;
		for (const ImageSlot& i : s.images)
			if (i.state == ImageState::Pending || i.state == ImageState::Loading)
				return false;
		return true;
	}

	std::string ImageUrl(int n)
	{
		return kRaw + "img" + std::to_string(n) + ".png";
	}

	std::string ReadmeWith(int firstImage, int lastImage)
	{
		std::string t = "# Mod\n\ntext\n\n";
		for (int i = firstImage; i <= lastImage; ++i)
			t += "![pic" + std::to_string(i) + "](" + ImageUrl(i) + ")\n\n";
		return t;
	}

	const std::string kUrl = kRaw + "README.md";

	void TestFetcherOk()
	{
		auto fake = std::make_shared<Fake>();
		fake->bodies[kUrl] = ReadmeWith(1, 1);
		fake->bodies[ImageUrl(1)] = PngBytes(100);
		ReadmeFetcher f(MakeFetch(fake));

		Check(f.Get()->state == ReadmeState::Idle, "starts Idle");
		Check(f.Open("mod-a", kUrl), "Open accepted");
		Check(f.Get()->state == ReadmeState::Loading || f.Get()->state == ReadmeState::Ready, "Loading right after Open");
		Check(WaitFor(f, ImagesSettled), "README and picture settle");
		auto snap = f.Get();
		Check(snap->entryId == "mod-a" && snap->document && snap->images.size() == 1, "Ready with one slot");
		Check(snap->images.size() == 1 && snap->images[0].state == ImageState::Bytes && snap->images[0].size == 100 && !snap->images[0].taken, "picture bytes held");
		const Block* img = snap->document ? First(*snap->document, BlockKind::Image) : nullptr;
		Check(img && img->imageSlot == 0, "image block points at slot 0");
		Check(f.BytesHeld() == 100, "BytesHeld counts the bytes");

		const std::string got = f.TakeImageBytes(0);
		Check(got == PngBytes(100), "TakeImageBytes hands the bytes out");
		Check(f.TakeImageBytes(0).empty(), "bytes only once");
		Check(f.BytesHeld() == 0, "nothing held after the take");
		Check(f.Get()->images[0].taken && f.Get()->images[0].state == ImageState::Bytes, "slot marked taken");
		Check(f.TakeImageBytes(7).empty(), "slot out of range");
		Check(snap->images[0].taken == false, "an earlier snapshot never changes");

		// Reopen refetches.
		Check(fake->CallsTo(kUrl) == 1, "one README fetch so far");
		Check(f.Open("mod-a", kUrl), "reopen accepted");
		Check(WaitFor(f, ImagesSettled), "reopened README settles");
		Check(fake->CallsTo(kUrl) == 2 && fake->CallsTo(ImageUrl(1)) == 2, "reopen fetches again");

		// Close drops everything.
		Check(f.BytesHeld() == 100, "bytes held again after reopen");
		f.Close();
		Check(f.BytesHeld() == 0, "Close frees the bytes");
		snap = f.Get();
		Check(snap->state == ReadmeState::Idle && !snap->document && snap->images.empty() && snap->entryId.empty(), "Close returns to Idle");
		Check(f.TakeImageBytes(0).empty(), "nothing to take after Close");
	}

	void TestFetcherFailures()
	{
		{
			auto fake = std::make_shared<Fake>();
			fake->errors[kUrl] = "Not found.";
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("x", kUrl);
			Check(WaitFor(f, [](const ReadmeSnapshot& s) { return s.state == ReadmeState::Failed; }), "README download fails");
			auto snap = f.Get();
			Check(snap->error == "Not found." && !snap->document && snap->images.empty(), "failure keeps the reason, no document");
			Check(f.BytesHeld() == 0, "no bytes after a failed README");
		}
		{
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = std::string(kMaxFetchBytes + 1, 'a');
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("x", kUrl);
			Check(WaitFor(f, [](const ReadmeSnapshot& s) { return s.state == ReadmeState::Failed; }), "README over the fetch cap fails");
			Check(!f.Get()->error.empty(), "over-cap README has words");
		}
		{
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = std::string(kMaxFetchBytes, 'a');
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("x", kUrl);
			Check(WaitFor(f, [](const ReadmeSnapshot& s) { return s.state == ReadmeState::Ready; }), "README exactly at the fetch cap is read");
		}
		{
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = ReadmeWith(1, 2);
			fake->bodies[ImageUrl(2)] = PngBytes(50);
			fake->errors[ImageUrl(1)] = "Picture missing.";
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("x", kUrl);
			Check(WaitFor(f, ImagesSettled), "image failure: settles");
			auto snap = f.Get();
			Check(snap->state == ReadmeState::Ready && snap->images.size() == 2, "image failure: README still ready");
			Check(snap->images.size() == 2 && snap->images[0].state == ImageState::Failed && snap->images[0].error == "Picture missing.", "failed picture has its reason");
			Check(snap->images.size() == 2 && snap->images[1].state == ImageState::Bytes, "the next picture still loads");
			Check(f.BytesHeld() == 50, "only the good picture is held");
		}
		{
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = ReadmeWith(1, 3);
			fake->bodies[ImageUrl(1)] = PngBytes(kMaxImageBytes);
			fake->bodies[ImageUrl(2)] = PngBytes(kMaxImageBytes + 1);
			fake->bodies[ImageUrl(3)] = std::string();   // missing
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("x", kUrl);
			Check(WaitFor(f, ImagesSettled), "size caps: settles");
			auto snap = f.Get();
			Check(snap->images.size() == 3, "size caps: three slots");
			Check(snap->images.size() == 3 && snap->images[0].state == ImageState::Bytes && snap->images[0].size == kMaxImageBytes, "picture exactly at the cap is accepted");
			Check(snap->images.size() == 3 && snap->images[1].state == ImageState::Failed, "picture one byte over the cap is refused");
			Check(f.BytesHeld() == kMaxImageBytes, "over-cap bytes are not kept");
		}
	}

	void TestFetcherImageRules()
	{
		{
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = ReadmeWith(1, 9) + "![again](" + ImageUrl(1) + ")\n";
			for (int i = 1; i <= 9; ++i)
				fake->bodies[ImageUrl(i)] = PngBytes(10);
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("x", kUrl);
			Check(WaitFor(f, ImagesSettled), "nine pictures: settles");
			auto snap = f.Get();
			Check(snap->images.size() == kMaxImages, "eight slots at most");
			Check(fake->CallsTo(ImageUrl(9)) == 0, "the ninth picture is never fetched");
			Check(fake->CallsTo(ImageUrl(1)) == 1, "a repeated address is fetched once");
			int ninth = 0, repeated = 0;
			if (snap->document)
				for (const Block& b : snap->document->blocks)
				{
					if (b.kind == BlockKind::Image && b.alt == "pic9") ninth = b.imageSlot;
					if (b.kind == BlockKind::Image && b.alt == "again") repeated = b.imageSlot;
				}
			Check(ninth == -1, "the ninth image block has no slot");
			Check(repeated == 0, "a repeated address shares the first slot");
		}
		{
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = ReadmeWith(1, 4);
			fake->bodies[ImageUrl(1)] = "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>";
			fake->bodies[ImageUrl(2)] = "<?xml version=\"1.0\"?><svg/>";
			fake->bodies[ImageUrl(3)] = "<!DOCTYPE html><html></html>";
			fake->bodies[ImageUrl(4)] = "just some text";
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("x", kUrl);
			Check(WaitFor(f, ImagesSettled), "markup bytes: settles");
			auto snap = f.Get();
			bool allFailed = snap->images.size() == 4;
			for (const ImageSlot& i : snap->images)
				allFailed = allFailed && i.state == ImageState::Failed && !i.error.empty();
			Check(allFailed, "SVG, XML, HTML and unknown bytes are refused");
			Check(f.BytesHeld() == 0, "refused bytes are not kept");
		}
		{
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = ReadmeWith(1, 5);
			std::string jpeg = "\xFF\xD8\xFF\xE0", gif = "GIF89a", webp("RIFF\x10\x00\x00\x00WEBPVP8 ", 16), bmp(40, '\0');
			bmp[0] = 'B'; bmp[1] = 'M'; bmp[14] = 40;
			fake->bodies[ImageUrl(1)] = PngBytes(20);
			fake->bodies[ImageUrl(2)] = jpeg + "xxxx";
			fake->bodies[ImageUrl(3)] = gif + "xxxx";
			fake->bodies[ImageUrl(4)] = webp;
			fake->bodies[ImageUrl(5)] = bmp;
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("x", kUrl);
			Check(WaitFor(f, ImagesSettled), "all formats: settles");
			bool allBytes = f.Get()->images.size() == 5;
			for (const ImageSlot& i : f.Get()->images)
				allBytes = allBytes && i.state == ImageState::Bytes;
			Check(allBytes, "PNG, JPEG, GIF, WebP and BMP are accepted");
		}
		{
			// Three pictures of exactly 2 MB make exactly 6 MB; the fourth is over.
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = ReadmeWith(1, 4);
			for (int i = 1; i <= 4; ++i)
				fake->bodies[ImageUrl(i)] = PngBytes(kMaxImageBytes);
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("x", kUrl);
			Check(WaitFor(f, ImagesSettled), "total cap: settles");
			auto snap = f.Get();
			int good = 0, bad = 0;
			for (const ImageSlot& i : snap->images)
			{
				if (i.state == ImageState::Bytes) ++good;
				if (i.state == ImageState::Failed) ++bad;
			}
			Check(good == 3 && bad == 1, "6 MB total: three accepted, the fourth refused");
			Check(f.BytesHeld() == kMaxImageTotalBytes, "held bytes stay at the total cap");
			f.Close();
			Check(f.BytesHeld() == 0, "Close frees every picture");
		}
	}

	void TestFetcherAddressAndClose()
	{
		{
			auto fake = std::make_shared<Fake>();
			ReadmeFetcher f(MakeFetch(fake));
			Check(!f.Open("x", "https://evil.example.com/README.md"), "foreign host refused");
			Check(f.Get()->state == ReadmeState::Failed && !f.Get()->error.empty(), "refused address is Failed with words");
			Check(!f.Open("x", "http://raw.githubusercontent.com/o/r/main/README.md"), "plain http refused");
			Check(!f.Open("x", ""), "empty address refused");
			Check(!f.Open("x", "https://github.com/o/r/blob/main/README.md"), "github page refused");
			Check(fake->calls.empty(), "refused addresses are never fetched");
		}
		{
			// Close while the README is still downloading.
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = ReadmeWith(1, 1);
			fake->bodies[ImageUrl(1)] = PngBytes(10);
			fake->Gate(kUrl);
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("a", kUrl);
			Check(fake->WaitGateHit(), "README download is in flight");
			f.Close();
			Check(f.Get()->state == ReadmeState::Idle, "Close during load: Idle");
			fake->Release();
			Check(fake->WaitFinished(1), "gated download ended");
			// A second open runs after the discarded job; its answer is the only one that shows.
			fake->bodies[kRaw + "B.md"] = "# B\n";
			Check(f.Open("b", kRaw + "B.md"), "open after Close");
			Check(WaitFor(f, [](const ReadmeSnapshot& s) { return s.state == ReadmeState::Ready; }), "second README ready");
			Check(f.Get()->entryId == "b" && f.Get()->images.empty() && f.BytesHeld() == 0, "the discarded README left nothing behind");
			Check(fake->CallsTo(ImageUrl(1)) == 0, "the discarded README's pictures were never fetched");
		}
		{
			// Close while a picture is downloading.
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = ReadmeWith(1, 2);
			fake->bodies[ImageUrl(1)] = PngBytes(10);
			fake->bodies[ImageUrl(2)] = PngBytes(10);
			fake->Gate(ImageUrl(1));
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("a", kUrl);
			Check(fake->WaitGateHit(), "picture download is in flight");
			Check(f.Get()->state == ReadmeState::Ready, "document shows while the picture loads");
			f.Close();
			fake->Release();
			fake->bodies[kRaw + "B.md"] = "# B\n";
			f.Open("b", kRaw + "B.md");
			Check(WaitFor(f, [](const ReadmeSnapshot& s) { return s.state == ReadmeState::Ready; }), "next README ready");
			Check(f.Get()->entryId == "b" && f.BytesHeld() == 0, "the picture that finished after Close was dropped");
			Check(fake->CallsTo(ImageUrl(2)) == 0, "no further pictures after Close");
		}
		{
			// Open while another is loading: the newer one wins.
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = "# A\n";
			fake->bodies[kRaw + "B.md"] = "# B\n";
			fake->Gate(kUrl);
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("a", kUrl);
			Check(fake->WaitGateHit(), "first README in flight");
			f.Open("b", kRaw + "B.md");
			fake->Release();
			Check(WaitFor(f, [](const ReadmeSnapshot& s) { return s.state == ReadmeState::Ready; }), "newer README ready");
			Check(f.Get()->entryId == "b" && First(*f.Get()->document, BlockKind::Heading) && AllText(*First(*f.Get()->document, BlockKind::Heading)) == "B", "the newer open replaces the older");
		}
	}

	void TestFetcherShutdown()
	{
		{
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = ReadmeWith(1, 1);
			fake->bodies[ImageUrl(1)] = PngBytes(10);
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("a", kUrl);
			Check(WaitFor(f, ImagesSettled), "before Shutdown");
			Check(f.Shutdown(3000), "Shutdown with an idle worker returns true");
			Check(f.Get()->state == ReadmeState::Idle && f.BytesHeld() == 0, "Shutdown drops the document and bytes");
			Check(!f.Open("a", kUrl), "no Open after Shutdown");
		}
		{
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = "# A\n";
			fake->Gate(kUrl);
			ReadmeFetcher f(MakeFetch(fake));
			f.Open("a", kUrl);
			Check(fake->WaitGateHit(), "request in flight before Shutdown");
			Check(!f.Shutdown(50), "Shutdown reports a worker that is still running");
			fake->Release();
			Check(f.Shutdown(3000), "Shutdown returns true once the request ends");
			Check(f.Get()->state == ReadmeState::Idle, "the late answer is discarded");
		}
		{
			// Destroying the fetcher with work in flight is safe.
			auto fake = std::make_shared<Fake>();
			fake->bodies[kUrl] = "# A\n";
			fake->Gate(kUrl);
			{
				ReadmeFetcher f(MakeFetch(fake));
				f.Open("a", kUrl);
				Check(fake->WaitGateHit(), "request in flight before destruction");
			}
			fake->Release();
			Check(fake->WaitFinished(1), "worker ends after the fetcher is gone");
		}
		Check(!ReadmeInstance().Open("x", "https://evil.example.com/README.md"), "the loader's instance refuses a bad address");
		Check(ShutdownReadmeInstance(1000), "the loader's instance shuts down");
	}
}

int main()
{
	TestBlocks();
	TestSpans();
	TestHtml();
	TestImages();
	TestLimits();
	TestHostile();
	TestAddressRules();
	TestFetcherOk();
	TestFetcherFailures();
	TestFetcherImageRules();
	TestFetcherAddressAndClose();
	TestFetcherShutdown();

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
