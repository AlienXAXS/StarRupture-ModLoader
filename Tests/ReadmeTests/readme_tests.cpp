// ReadmeTests: the README reader (auto_update/readme_markdown.cpp), the picture
// and link address rules (readme_images.h) and the README fetcher's states
// (readme_fetcher.cpp) through an injected download, the picture header size
// rule, the texture handoff (readme_textures.cpp) and the open-page helper
// through injected calls. No network, no game.

#include "auto_update/http_get.h"
#include "auto_update/readme_fetcher.h"
#include "auto_update/readme_images.h"
#include "auto_update/readme_markdown.h"
#include "auto_update/readme_textures.h"
#include "utils/open_page.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
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
	bool SaneSpans(const std::vector<Span>& spans)
	{
		if (spans.size() > kMaxSpans)
			return false;
		for (const Span& s : spans)
		{
			if (s.text.size() > kMaxSpanBytes || !ValidUtf8(s.text) || !ValidUtf8(s.url))
				return false;
			if (s.text.find('\0') != std::string::npos)
				return false;
		}
		return true;
	}

	size_t TableCellCount(const Document& d)
	{
		size_t n = 0;
		for (const Block& b : d.blocks)
			n += b.header.size() * (1 + b.rows.size());
		return n;
	}

	bool SaneTable(const Block& b)
	{
		if (b.kind != BlockKind::Table)
			return b.header.empty() && b.rows.empty();
		if (b.header.empty() || b.header.size() > kMaxTableColumns || b.rows.size() > kMaxTableRows)
			return false;
		for (const TableCell& cell : b.header)
			if (!SaneSpans(cell))
				return false;
		for (const TableRow& row : b.rows)
		{
			if (row.size() != b.header.size())
				return false;
			for (const TableCell& cell : row)
				if (!SaneSpans(cell))
					return false;
		}
		return true;
	}

	bool Sane(const Document& d)
	{
		if (d.blocks.size() > kMaxBlocks || TableCellCount(d) > kMaxTableCells)
			return false;
		for (const Block& b : d.blocks)
		{
			if (b.code.size() > kMaxCodeBytes || !SaneSpans(b.spans) || !SaneTable(b))
				return false;
			if (!ValidUtf8(b.code) || !ValidUtf8(b.alt) || !ValidUtf8(b.url))
				return false;
			if (b.level < 0 || b.level > 3 || b.depth < 0 || b.depth > 1)
				return false;
		}
		return true;
	}

	std::string CellText(const TableCell& cell)
	{
		std::string t;
		for (const Span& s : cell)
			t += s.text;
		return t;
	}

	// The cells' texts, a '/' between cells.
	std::string RowText(const TableRow& row)
	{
		std::string t;
		for (size_t i = 0; i < row.size(); ++i)
			t += (i ? "/" : "") + CellText(row[i]);
		return t;
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
		// Short paragraphs, so that no block hits its own limits: 1310 x 100 bytes, then 72 more.
		Document d = P(Repeat(std::string(98, 'a') + "\n\n", 1310) + std::string(72, 'a'));
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
		Check(!d.shortened, "exactly kMaxSpans spans loses nothing");
		d = P(Repeat("[a](https://github.com/o/r) ", 40) + "x");
		Check(d.blocks.size() == 1 && d.blocks[0].spans.size() == kMaxSpans, "spans over the limit are cut");
		Check(d.shortened, "links lost past kMaxSpans set shortened");
		d = P(Repeat("**b** and `c` ", 35));
		Check(d.shortened, "70 bold and code spans in one block set shortened");
		d = P(Repeat("**b** and `c` ", 10));
		Check(!d.shortened, "20 bold and code spans do not set shortened");
		d = P(Repeat("`a` ", 32) + "   ");
		Check(!d.shortened, "trailing blanks past the span limit lose nothing");
		d = P(Repeat("a", 130000));
		Check(d.shortened, "one paragraph longer than all its spans can hold sets shortened");
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
		Check(!d.shortened, "code at the limit is not shortened");
		d = P("```\n" + Repeat("a", kMaxCodeBytes + 500) + "\n```\n");
		code = First(d, BlockKind::CodeBlock);
		Check(code && code->code.size() == kMaxCodeBytes, "code over the limit is cut");
		Check(d.shortened, "a code line cut at the limit sets shortened");
		d = P("```\n" + Repeat("line\n", 2000) + "```\n");
		code = First(d, BlockKind::CodeBlock);
		Check(code && code->code.size() <= kMaxCodeBytes, "many code lines are capped");
		Check(d.shortened, "code lines dropped past the limit set shortened");
		d = P("    " + Repeat("a", kMaxCodeBytes + 500) + "\n");
		Check(d.shortened, "an indented code line cut at the limit sets shortened");
		d = P("```\n" + Repeat("line\n", 100) + "```\n");
		Check(!d.shortened, "a normal code block is not shortened");
		d = P("para\n" + Repeat("&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;&amp;\n", 2840));
		Check(d.shortened, "lines dropped from a full pending paragraph set shortened");
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

	// The text of a document that is one block.
	std::string OnlyText(const std::string& markdown)
	{
		const Document d = P(markdown);
		return Sane(d) && d.blocks.size() == 1 ? AllText(d.blocks[0]) : "<not one sane block>";
	}

	// ---- tables ------------------------------------------------------------

	void TestTables()
	{
		// ProductionViewer's Configuration table.
		Document d = P(
			"| Section | Key               | Default | Description                               |\n"
			"|---------|-------------------|---------|-------------------------------------------|\n"
			"| General | Enabled           | true    | Enable or disable the plugin              |\n"
			"| Menu    | ToggleKey         | P       | Key to open / close the Production Viewer |\n"
			"| Network | BroadcastInterval | 1.0     | Seconds between production updates        |\n");
		const Block* t = First(d, BlockKind::Table);
		Check(Sane(d) && d.blocks.size() == 1 && t && !d.shortened, "config table is one table block");
		Check(t && RowText(t->header) == "Section/Key/Default/Description", "config table header");
		Check(t && t->rows.size() == 3 && RowText(t->rows[0]) == "General/Enabled/true/Enable or disable the plugin", "config table first row");
		Check(t && t->rows.size() == 3 && RowText(t->rows[1]) == "Menu/ToggleKey/P/Key to open / close the Production Viewer", "a slash in a cell stays");

		d = P("a | b\n--- | :-:\n1 | 2\n");
		t = First(d, BlockKind::Table);
		Check(Sane(d) && t && RowText(t->header) == "a/b" && t->rows.size() == 1 && RowText(t->rows[0]) == "1/2", "table without outer pipes");

		d = P("> | a | b |\n> |---|---|\n> | 1 | 2 |\n");
		t = First(d, BlockKind::Table);
		Check(Sane(d) && t && t->rows.size() == 1 && RowText(t->rows[0]) == "1/2", "quoted table");

		d = P("| **bold** | `a|b` | [link](https://github.com/o/r) | x \\| y |\n|---|---|---|---|\n| `c \\| d` | e\\*f | | z |\n");
		t = First(d, BlockKind::Table);
		const bool spansTable = Sane(d) && t && t->header.size() == 4 && t->rows.size() == 1 && !d.shortened;
		Check(spansTable, "table with spans");
		if (spansTable)
		{
			Check(t->header[0].size() == 1 && t->header[0][0].kind == SpanKind::Bold && t->header[0][0].text == "bold", "bold in a cell");
			Check(t->header[1].size() == 1 && t->header[1][0].kind == SpanKind::Code && t->header[1][0].text == "a|b", "a pipe in code does not split");
			Check(t->header[2].size() == 1 && t->header[2][0].kind == SpanKind::Link && t->header[2][0].clickable, "link in a cell");
			Check(CellText(t->header[3]) == "x | y", "an escaped pipe is text");
			Check(CellText(t->rows[0][0]) == "c | d", "an escaped pipe in code loses its backslash");
			Check(CellText(t->rows[0][1]) == "e*f", "other escapes still work in a cell");
			Check(t->rows[0][2].empty(), "empty cell");
		}

		// Whatever follows ends the table.
		const auto endsAt = [](const std::string& after, BlockKind kind)
		{
			const Document doc = P("| a |\n|---|\n| 1 |\n" + after);
			return Sane(doc) && doc.blocks.size() == 2 && doc.blocks[0].kind == BlockKind::Table &&
			       doc.blocks[0].rows.size() == 1 && doc.blocks[1].kind == kind;
		};
		Check(endsAt("\ntext\n", BlockKind::Paragraph), "a blank line ends a table");
		Check(endsAt("# H\n", BlockKind::Heading), "a heading ends a table");
		Check(endsAt("- item\n", BlockKind::ListItem), "a list item ends a table");
		Check(endsAt("```\ncode\n```\n", BlockKind::CodeBlock), "a fence ends a table");
		Check(endsAt("    code\n", BlockKind::CodeBlock), "indented code ends a table");
		Check(endsAt("---\n", BlockKind::Rule), "a rule ends a table");
		Check(endsAt("<p>html</p>\n", BlockKind::Paragraph), "HTML ends a table");

		d = P("| a | b |\n|---|---|\n");
		t = First(d, BlockKind::Table);
		Check(Sane(d) && t && t->rows.empty(), "header-only table");

		// Pipes that make no table: one line each.
		Check(OnlyText("| a | b |\n|---|\n| 1 | 2 |\n") == "| a | b |\n|---|\n| 1 | 2 |", "wrong delimiter count keeps one line per row");
		Check(OnlyText("| a | b |\n| c | d |\n") == "| a | b |\n| c | d |", "pipe rows without a delimiter keep their lines");
		Check(OnlyText("run a | b here\nnext line\n") == "run a | b here next line", "prose with a pipe still flows");

		// Ragged rows.
		d = P("|a|b|c|\n|-|-|-|\n|1|\n");
		t = First(d, BlockKind::Table);
		Check(Sane(d) && t && t->rows.size() == 1 && RowText(t->rows[0]) == "1//" && !d.shortened, "a short row is filled");
		d = P("|a|b|c|\n|-|-|-|\n|1|2|3|4|5|\n");
		t = First(d, BlockKind::Table);
		Check(Sane(d) && t && t->rows.size() == 1 && RowText(t->rows[0]) == "1/2/3" && d.shortened, "extra cells are dropped, shortened");

		// Each cap: at the limit nothing is lost, one over sets shortened.
		const auto wide = [](size_t columns)
		{
			return P(Repeat("|h", columns) + "|\n" + Repeat("|-", columns) + "|\n" + Repeat("|c", columns) + "|\n");
		};
		d = wide(kMaxTableColumns);
		Check(Sane(d) && TableCellCount(d) == 2 * kMaxTableColumns && !d.shortened, "columns at the cap");
		d = wide(kMaxTableColumns + 1);
		Check(Sane(d) && TableCellCount(d) == 2 * kMaxTableColumns && d.shortened, "one column over is dropped, shortened");
		d = wide(10000);
		Check(Sane(d) && TableCellCount(d) == 2 * kMaxTableColumns && d.shortened, "10,000 columns");

		const auto tall = [](size_t rows) { return P("|h|\n|-|\n" + Repeat("|c|\n", rows)); };
		d = tall(kMaxTableRows);
		t = First(d, BlockKind::Table);
		Check(Sane(d) && t && t->rows.size() == kMaxTableRows && !d.shortened, "rows at the cap");
		d = tall(kMaxTableRows + 1);
		t = First(d, BlockKind::Table);
		Check(Sane(d) && t && t->rows.size() == kMaxTableRows && d.shortened && d.blocks.size() == 1, "one row over is dropped, shortened");
		d = tall(20000);
		Check(Sane(d) && d.blocks.size() == 1 && d.shortened, "huge table");

		// Five tables of 8 columns and 50 lines are exactly kMaxTableCells.
		const std::string line = Repeat("|c", 8) + "|\n";
		const std::string head = line + Repeat("|-", 8) + "|\n";
		const std::string full = head + Repeat(line, 49) + "\n";
		d = P(Repeat(full, 5));
		Check(Sane(d) && TableCellCount(d) == kMaxTableCells && !d.shortened, "cells at the cap");
		d = P(Repeat(full, 5) + "|h|\n|-|\n|c|\n\ntail\n");
		Check(Sane(d) && TableCellCount(d) == kMaxTableCells && d.shortened, "a table past the cell cap is dropped, shortened");
		Check(d.blocks.size() == 6 && AllText(d.blocks.back()) == "tail", "text after a dropped table is kept");
		d = P(Repeat(full, 4) + head + Repeat(line, 60));
		Check(Sane(d) && TableCellCount(d) == kMaxTableCells && d.shortened, "rows past the cell cap are dropped, shortened");

		d = P("|h|\n|-|\n|" + Repeat("`a` ", 100) + "|\n");
		t = First(d, BlockKind::Table);
		Check(Sane(d) && t && t->rows.size() == 1 && t->rows[0][0].size() == kMaxSpans - 1 && d.shortened, "spans past the cap in a cell, shortened");   // the 64th was a space: trimmed
		const std::string links = Repeat("[a](https://github.com/o/r)", 40);
		d = P("|h|\n|-|\n|" + links + "![p](a.png)" + links + "|\n");
		t = First(d, BlockKind::Table);
		Check(Sane(d) && t && t->rows.size() == 1 && t->rows[0][0].size() == kMaxSpans && d.shortened, "a cell split by a picture keeps the cap, shortened");

		// Hostile cells and lines.
		d = P("| `open | b |\n|---|---|\n| `x | y |\n");
		t = First(d, BlockKind::Table);
		Check(Sane(d) && t && RowText(t->header) == "`open/b" && t->rows.size() == 1 && RowText(t->rows[0]) == "`x/y", "unterminated code in a cell");
		d = P(Repeat("|", 10000) + "\n");
		Check(Sane(d) && Count(d, BlockKind::Table) == 0, "10,000 pipes on a line");
		d = P(Repeat("|", 10000) + "\n" + Repeat("|-", 10000) + "\n");
		Check(Sane(d), "10,000 pipes over a delimiter line");
		d = P("|a|\n|-|\n" + Repeat("|", 10000) + "\n");
		Check(Sane(d) && d.shortened, "10,000 pipes as a body row");

		d = P("| a |\n|---|\n| text ![pic](https://raw.githubusercontent.com/o/r/main/a.png) |\n");
		Check(Sane(d) && d.blocks.size() == 2 && d.blocks[0].kind == BlockKind::Table && d.blocks[1].kind == BlockKind::Image &&
		      d.blocks[0].rows.size() == 1 && CellText(d.blocks[0].rows[0][0]) == "text", "a picture in a cell follows the table");
		d = P("|h|\n|-|\n" + Repeat("|" + Repeat("![p](a.png)", 40) + "|\n", 100));
		Check(Sane(d) && d.blocks.size() == kMaxBlocks && d.shortened, "pictures in cells stay within the block cap");
		d = P(Repeat("|x|\n|-|\n\n", 5000));
		Check(Sane(d) && d.shortened, "thousands of tiny tables");

		const auto t0 = std::chrono::steady_clock::now();
		d = P(Repeat("a|b\n-|-\n" + Repeat("`x|", 40) + "\n\n", 2000));
		Check(Sane(d), "many tables with open code marks");
		d = P("|h|\n|-|\n|" + Repeat("`x", 200) + std::string(100000, '`') + "|\n");
		Check(Sane(d), "backtick runs in a cell");
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
		Check(ms < 3000, "hostile tables under 3 s");
	}

	// ---- text fitted to the built-in font -----------------------------------

	void TestFontText()
	{
		// ProductionViewer's folder tree, in a code block.
		Document d = P(
			"```\n"
			"<parent folder>/\n"
			"\xE2\x94\x9C\xE2\x94\x80\xE2\x94\x80 StarRupture-Plugin-SDK/\n"
			"\xE2\x94\x94\xE2\x94\x80\xE2\x94\x80 StarRupture-Plugin-ProductionViewer/\n"
			"```\n");
		const Block* code = First(d, BlockKind::CodeBlock);
		Check(Sane(d) && code && code->code == "<parent folder>/\n+-- StarRupture-Plugin-SDK/\n`-- StarRupture-Plugin-ProductionViewer/", "folder tree reads as a tree");
		d = P("```\n\xE2\x94\x82   file\n```\n");
		code = First(d, BlockKind::CodeBlock);
		Check(code && code->code == "|   file", "vertical bar keeps its indent");

		// Codex: U+2726 marks a credit in the bullets and in the key.
		Check(OnlyText("- **Pins, your way** <sup>[\xE2\x9C\xA6](#msantoro12)</sup> -- pins are saved\n") == "Pins, your way * -- pins are saved", "credit mark in a bullet becomes *");
		Check(OnlyText("- <a id=\"msantoro12\"></a><sup>\xE2\x9C\xA6</sup> [@x](https://github.com/x)\n") == "* @x", "credit mark in the key becomes *");

		Check(OnlyText("not interchangeable \xE2\x80\x94 and both\n") == "not interchangeable -- and both", "em dash");
		Check(OnlyText("1\xE2\x80\x93" "2 \xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\x98s\xE2\x80\x99 etc\xE2\x80\xA6\n") == "1-2 \"q\" 's' etc...", "dashes, quotes, ellipsis");
		Check(OnlyText("a \xE2\x86\x92 b \xE2\x86\x90 c \xE2\x96\xB8 d \xE2\x80\xA2 e\xC2\xA0" "f\n") == "a -> b <- c > d * e f", "arrows, bullets, non-breaking space");
		Check(OnlyText("x &#x2014; y\n") == "x -- y", "an entity is fitted too");
		Check(OnlyText("run `a \xE2\x86\x92 b` now\n") == "run a -> b now", "inline code is fitted");

		// What the font cannot draw is dropped, without a double or hanging space.
		Check(OnlyText("a \xF0\x9F\x9A\x80 b\n") == "a b", "emoji between words");
		Check(OnlyText("\xF0\x9F\x9A\x80 Start\n") == "Start", "emoji at the start");
		Check(OnlyText("done \xE2\xAD\x90\n") == "done", "symbol at the end");
		Check(OnlyText("x \xE2\x9A\xA0\xEF\xB8\x8F y\n") == "x y", "variation selector goes with its symbol");
		Check(OnlyText("a \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB b\n") == "a b", "joined emoji");
		Check(OnlyText("wo\xE2\x80\x8Brd\n") == "word", "zero-width space inside a word");
		Check(OnlyText("## \xF0\x9F\x9A\x80 Features\n") == "Features", "emoji heading");
		Check(OnlyText("**\xF0\x9F\x9A\x80 Bold** text\n") == "Bold text", "emoji at the start of a bold span");
		Check(OnlyText("**a \xF0\x9F\x9A\x80** b\n") == "a b", "emoji at the end of a bold span");
		d = P("## \xF0\x9F\x9A\x80\n\ntext\n");
		Check(Sane(d) && d.blocks.size() == 1 && d.blocks[0].kind == BlockKind::Paragraph, "a heading of only emoji goes");
		d = P("|h|\n|-|\n| \xE2\xAD\x90 yes \xE2\x80\x94 ok |\n");
		const Block* t = First(d, BlockKind::Table);
		Check(t && t->rows.size() == 1 && CellText(t->rows[0][0]) == "yes -- ok", "table cells are fitted");
		// Check and cross marks answer a feature table: they must not vanish.
		const auto markCell = [](const char* mark)
		{
			const Document doc = P("|h|\n|-|\n| " + std::string(mark) + " |\n");
			const Block* table = First(doc, BlockKind::Table);
			return table && table->rows.size() == 1 ? CellText(table->rows[0][0]) : std::string("no table");
		};
		for (const char* const mark : { "\xE2\x9C\x85", "\xE2\x9C\x93", "\xE2\x9C\x94\xEF\xB8\x8F", "\xE2\x98\x91" })
			Check(markCell(mark) == "yes", "a check mark reads yes");
		for (const char* const mark : { "\xE2\x9D\x8C", "\xE2\x9C\x95", "\xE2\x9C\x96", "\xE2\x9C\x97", "\xE2\x9C\x98" })
			Check(markCell(mark) == "no", "a cross mark reads no");
		Check(OnlyText("a\xE2\x80\xA0 b\xE2\x80\xA1\n") == "a+ b+", "daggers become +");

		d = P("![a \xE2\x80\x94 b](https://example.com/x.png)\n");
		const Block* image = First(d, BlockKind::Image);
		Check(image && image->alt == "a -- b", "picture alt text is fitted");

		// Letters the built-in or a chosen font may draw are left alone.
		const std::string letters = "caf\xC3\xA9 \xC3\xBC \xE2\x82\xAC \xD0\x9F\xD1\x80\xD0\xB8 \xE4\xB8\xAD";
		Check(OnlyText(letters + "\n") == letters, "letters are kept");

		// Addresses are never changed.
		const std::string address = "https://github.com/o/r/\xE2\x80\x93x";
		d = P("[a \xE2\x80\x93 b](" + address + ")\n");
		Check(Sane(d) && d.blocks.size() == 1 && d.blocks[0].spans.size() == 1 && d.blocks[0].spans[0].text == "a - b" &&
		      d.blocks[0].spans[0].url == address, "link text is fitted, its address is not");
		d = P("<" + address + ">\n");
		Check(Sane(d) && d.blocks.size() == 1 && d.blocks[0].spans.size() == 1 && d.blocks[0].spans[0].text == address &&
		      d.blocks[0].spans[0].url == address, "an address shown as text is not fitted");
		Check(OnlyText("[\xF0\x9F\x9A\x80](https://github.com/o/r)\n") == "https://github.com/o/r", "a link of only emoji shows its address");

		// Malformed input stays valid UTF-8 everywhere text can go.
		const std::string bad = "a\xC3 \xE2\x94 \xED\xA0\x80 \xF4\x90\x80\x80 \xC0\xAF \x80\xA0\xAD \xF0\x9F z";
		d = P(bad + "\n\n```\n" + bad + "\n```\n\n|" + bad + "|`" + bad + "`|\n|-|-|\n|[" + bad + "](https://github.com/o/r)|![" + bad + "](a.png)|\n\n## " + bad + "\xE2\x94");
		Check(Sane(d) && Count(d, BlockKind::Table) == 1, "malformed UTF-8 stays valid");

		const char* const pieces[] = { "|", "-", ":", "`", "\\", "\n", " ", "a", "*", "[", "]", "(", ")", "!", "<", ">",
			"\xE2\x94\x80", "\xE2\x9C\xA6", "\xF0\x9F\x9A\x80", "\xE2\x80\x8D", "\xC2\xA0", "\xE2", "\x80", "\xF0\x9F", "\xC3\xA9" };
		uint32_t seed = 12345;
		bool allSane = true;
		for (int run = 0; run < 400; ++run)
		{
			std::string in = run % 2 ? "|a|b|\n|-|-|\n" : "";
			for (int i = 0; i < 400; ++i)
			{
				seed = seed * 1664525u + 1013904223u;
				in += pieces[(seed >> 16) % (sizeof(pieces) / sizeof(pieces[0]))];
			}
			if (!Sane(P(in)))
				allSane = false;
		}
		Check(allSane, "seeded random table and symbol pieces: always sane");
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

	// ---- picture header sizes (before any decoder runs) --------------------

	void Put16(std::string& s, unsigned v, bool bigEndian)
	{
		const char lo = static_cast<char>(v & 0xFF);
		const char hi = static_cast<char>((v >> 8) & 0xFF);
		if (bigEndian) { s.push_back(hi); s.push_back(lo); }
		else           { s.push_back(lo); s.push_back(hi); }
	}

	void Put32(std::string& s, uint32_t v, bool bigEndian)
	{
		if (bigEndian) { Put16(s, v >> 16, true);  Put16(s, v & 0xFFFF, true); }
		else           { Put16(s, v & 0xFFFF, false); Put16(s, v >> 16, false); }
	}

	std::string PngSized(uint32_t w, uint32_t h)
	{
		std::string s("\x89PNG\r\n\x1a\n", 8);
		Put32(s, 13, true);
		s += "IHDR";
		Put32(s, w, true);
		Put32(s, h, true);
		s += std::string("\x08\x06\x00\x00\x00", 5);
		return s;
	}

	// Logical screen w x h, then (optionally) a global colour table and a graphic
	// control extension, then the first image descriptor of size fw x fh.
	std::string GifFrame(unsigned w, unsigned h, unsigned fw, unsigned fh, bool table = false, bool extension = false)
	{
		std::string s("GIF89a");
		Put16(s, w, false);
		Put16(s, h, false);
		s += static_cast<char>(table ? 0x80 : 0x00);   // table of 2 colours
		s += std::string("\x00\x00", 2);
		if (table)
			s += std::string(6, '\x11');
		if (extension)
			s += std::string("\x21\xF9\x04\x00\x00\x00\x00\x00", 8);   // introducer, label, one sub-block of 4, end
		s += '\x2C';
		Put16(s, 0, false);
		Put16(s, 0, false);
		Put16(s, fw, false);
		Put16(s, fh, false);
		s += '\x00';
		return s;
	}

	std::string GifSized(unsigned w, unsigned h)
	{
		return GifFrame(w, h, w, h);
	}

	std::string BmpSized(int32_t w, int32_t h, bool core = false)
	{
		std::string s("BM");
		Put32(s, 100, false);   // file size
		Put32(s, 0, false);
		Put32(s, 54, false);    // pixel data offset
		if (core)
		{
			Put32(s, 12, false);
			Put16(s, static_cast<unsigned>(w), false);
			Put16(s, static_cast<unsigned>(h), false);
			Put16(s, 1, false);
			Put16(s, 24, false);
			s.append(8, '\0');
		}
		else
		{
			Put32(s, 40, false);
			Put32(s, static_cast<uint32_t>(w), false);
			Put32(s, static_cast<uint32_t>(h), false);
			s.append(32, '\0');
		}
		return s;
	}

	// SOI, an APP0 segment, padding, then a frame header of the given marker.
	std::string JpegSized(unsigned w, unsigned h, unsigned sof = 0xC0)
	{
		std::string s("\xFF\xD8", 2);
		s += std::string("\xFF\xE0", 2);
		Put16(s, 16, true);
		s.append(14, '\0');
		s.push_back('\xFF');   // padding byte before the next marker
		s.push_back('\xFF');
		s.push_back(static_cast<char>(sof));
		Put16(s, 17, true);
		s.push_back('\x08');
		Put16(s, h, true);
		Put16(s, w, true);
		s += std::string("\x03\x01\x22\x00\x02\x11\x01\x03\x11\x01", 10);
		return s;
	}

	std::string WebpHeader(const char* fourcc, const std::string& data)
	{
		std::string s("RIFF");
		Put32(s, static_cast<uint32_t>(4 + 8 + data.size()), false);
		s += "WEBP";
		s += fourcc;
		Put32(s, static_cast<uint32_t>(data.size()), false);
		return s + data;
	}

	std::string WebpLossy(unsigned w, unsigned h)
	{
		std::string d("\x10\x02\x00\x9D\x01\x2A", 6);
		Put16(d, w, false);
		Put16(d, h, false);
		return WebpHeader("VP8 ", d);
	}

	std::string WebpLossless(unsigned w, unsigned h)
	{
		std::string d("\x2F", 1);
		Put32(d, ((w - 1) & 0x3FFF) | (((h - 1) & 0x3FFF) << 14), false);
		d.append(4, '\0');
		return WebpHeader("VP8L", d);
	}

	std::string WebpExtended(uint32_t w, uint32_t h)
	{
		std::string d("\x00\x00\x00\x00", 4);
		const uint32_t cw = w - 1;
		const uint32_t ch = h - 1;
		d.push_back(static_cast<char>(cw & 0xFF));
		d.push_back(static_cast<char>((cw >> 8) & 0xFF));
		d.push_back(static_cast<char>((cw >> 16) & 0xFF));
		d.push_back(static_cast<char>(ch & 0xFF));
		d.push_back(static_cast<char>((ch >> 8) & 0xFF));
		d.push_back(static_cast<char>((ch >> 16) & 0xFF));
		return WebpHeader("VP8X", d);
	}

	bool SizeIs(const std::string& bytes, int64_t w, int64_t h)
	{
		int64_t gw = -1;
		int64_t gh = -1;
		return ReadImageSize(bytes, &gw, &gh) && gw == w && gh == h;
	}

	void TestImageHeaderSize()
	{
		Check(SizeIs(PngSized(640, 480), 640, 480), "PNG size");
		Check(SizeIs(GifSized(320, 200), 320, 200), "GIF size");
		Check(SizeIs(BmpSized(200, 100), 200, 100), "BMP size");
		Check(SizeIs(BmpSized(200, -100), 200, 100), "BMP top-down height is its absolute value");
		Check(SizeIs(BmpSized(30, 20, true), 30, 20), "BMP with the old core header");
		Check(SizeIs(JpegSized(800, 600), 800, 600), "JPEG size past a segment and padding");
		Check(SizeIs(JpegSized(800, 600, 0xC2), 800, 600), "progressive JPEG size");
		Check(SizeIs(WebpLossy(300, 150), 300, 150), "WebP VP8 size");
		Check(SizeIs(WebpLossless(300, 150), 300, 150), "WebP VP8L size");
		Check(SizeIs(WebpExtended(70000, 5), 70000, 5), "WebP VP8X size (24 bit sides)");

		// Hostile headers read as written; the allow rule decides.
		Check(SizeIs(PngSized(0x7FFFFFFFu, 0x7FFFFFFFu), 0x7FFFFFFF, 0x7FFFFFFF), "PNG with a huge header reads as written");
		Check(SizeIs(PngSized(0xFFFFFFFFu, 1), 0xFFFFFFFFLL, 1), "PNG width above 32 bit signed");

		// Zero size, truncated, garbage.
		int64_t w = 0;
		int64_t h = 0;
		Check(!ReadImageSize(PngSized(0, 10), &w, &h), "PNG zero width refused");
		Check(!ReadImageSize(GifSized(0, 0), &w, &h), "GIF zero size refused");
		Check(!ReadImageSize(BmpSized(0, 5), &w, &h), "BMP zero width refused");
		Check(!ReadImageSize(JpegSized(0, 5), &w, &h), "JPEG zero width refused");
		Check(!ReadImageSize(WebpLossy(0, 5), &w, &h), "WebP VP8 zero width refused");

		const std::string formats[] = { PngSized(10, 10), GifSized(10, 10), BmpSized(10, 10), BmpSized(10, 10, true),
			JpegSized(10, 10), WebpLossy(10, 10), WebpLossless(10, 10), WebpExtended(10, 10) };
		bool anyCutReads = false;
		for (const std::string& full : formats)
		{
			// The full header reads; every shorter prefix must not crash, and a prefix
			// that cuts into the size fields must not read.
			Check(ReadImageSize(full, &w, &h), "a complete header reads");
			for (size_t cut = 0; cut + 1 < full.size(); ++cut)
			{
				const std::string part = full.substr(0, cut);
				int64_t pw = 0;
				int64_t ph = 0;
				if (ReadImageSize(part, &pw, &ph) && (pw != 10 || ph != 10))
					anyCutReads = true;
			}
		}
		Check(!anyCutReads, "a truncated header never reads a wrong size");
		Check(!ReadImageSize(PngSized(10, 10).substr(0, 23), &w, &h), "PNG cut inside the height");
		Check(!ReadImageSize(GifSized(10, 10).substr(0, 9), &w, &h), "GIF cut inside the height");
		Check(!ReadImageSize(JpegSized(10, 10).substr(0, 29), &w, &h), "JPEG cut inside the frame header");
		Check(!ReadImageSize(WebpLossy(10, 10).substr(0, 29), &w, &h), "WebP cut inside the height");

		// A PNG whose first chunk is not IHDR, a JPEG with no frame header, a WebP with an unknown chunk.
		std::string badPng = PngSized(10, 10);
		badPng.replace(12, 4, "IDAT");
		Check(!ReadImageSize(badPng, &w, &h), "PNG without IHDR first");
		std::string noFrame("\xFF\xD8\xFF\xDA", 4);
		noFrame.append(40, '\0');
		Check(!ReadImageSize(noFrame, &w, &h), "JPEG scan before any frame header");
		std::string loopJpeg("\xFF\xD8\xFF\xE0\x00\x02", 6);   // a segment of length 2, then nothing
		Check(!ReadImageSize(loopJpeg, &w, &h), "JPEG that ends in segments");
		std::string zeroLen("\xFF\xD8\xFF\xE0\x00\x00", 6);
		zeroLen.append(20, '\xFF');
		Check(!ReadImageSize(zeroLen, &w, &h), "JPEG segment of length 0 does not loop");
		Check(!ReadImageSize(WebpHeader("ABCD", std::string(20, '\0')), &w, &h), "WebP with an unknown first chunk");
		Check(!ReadImageSize("", &w, &h), "empty bytes");
		Check(!ReadImageSize(std::string(200, 'x'), &w, &h), "garbage");
		Check(!ReadImageSize("<svg width=\"10\" height=\"10\"></svg>", &w, &h), "markup");
		std::string bigJpeg("\xFF\xD8", 2);
		for (int i = 0; i < 5000; ++i)
			bigJpeg += std::string("\xFF\xE1\x00\x04xx", 6);
		Check(!ReadImageSize(bigJpeg, &w, &h), "JPEG with thousands of segments and no frame ends cleanly");

		// The allow rule.
		Check(IsImageSizeAllowed(PngSized(4096, 3900)), "4096 wide is allowed up to 16 million pixels");
		Check(!IsImageSizeAllowed(PngSized(4096, 4096)), "16.7 million pixels refused");
		Check(IsImageSizeAllowed(PngSized(4000, 4000)), "exactly 16 million pixels allowed");
		Check(!IsImageSizeAllowed(PngSized(4097, 10)), "width over 4096 refused");
		Check(!IsImageSizeAllowed(PngSized(10, 4097)), "height over 4096 refused");
		Check(!IsImageSizeAllowed(PngSized(0x7FFFFFFFu, 0x7FFFFFFFu)), "huge PNG refused");
		Check(!IsImageSizeAllowed(PngSized(0xFFFFFFFFu, 0xFFFFFFFFu)), "maximum PNG sides refused without overflow");
		Check(!IsImageSizeAllowed(WebpExtended(16777216u, 16777216u)), "maximum WebP canvas refused without overflow");
		Check(!IsImageSizeAllowed(BmpSized(INT32_MIN, 5)), "BMP width of the smallest int refused");
		Check(!IsImageSizeAllowed(PngSized(0, 0)), "zero size refused");
		Check(!IsImageSizeAllowed(PngBytes(10)), "a signature with no header refused");
		Check(!IsImageSizeAllowed(""), "no bytes refused");
		Check(IsImageSizeAllowed(JpegSized(4096, 100)), "JPEG at the side limit allowed");
		Check(!IsImageSizeAllowed(JpegSized(65535, 65535, 0xC1)), "huge JPEG refused");
		Check(!IsImageSizeAllowed(WebpLossless(16384, 16)), "WebP side over 4096 refused");
		Check(IsImageSizeAllowed(GifSized(1, 1)), "1 x 1 GIF allowed");

		// A GIF is decoded at the size of its first frame, not its logical screen.
		Check(!IsImageSizeAllowed(GifFrame(1, 1, 65535, 65535)), "small screen with a huge first frame refused");
		Check(!IsImageSizeAllowed(GifFrame(1, 1, 65535, 65535, true, true)), "huge first frame behind a colour table and extension refused");
		Check(!IsImageSizeAllowed(GifFrame(100, 100, 5000, 10)), "first frame over a side limit refused");
		Check(IsImageSizeAllowed(GifFrame(200, 100, 100, 50, true, true)), "normal GIF with table and extension allowed");
		{
			int64_t gw = 0;
			int64_t gh = 0;
			Check(ReadImageSize(GifFrame(200, 100, 100, 300), &gw, &gh) && gw == 200 && gh == 300, "GIF size is the larger of screen and frame per side");
			// Truncated at every length, and a block chain that runs off the end: refused, no read past the end.
			const std::string full = GifFrame(10, 10, 10, 10, true, true);
			bool anyRead = false;
			for (size_t cut = 0; cut < full.size(); ++cut)
				if (ReadImageSize(full.substr(0, cut), &gw, &gh))
					anyRead = true;
			Check(!anyRead, "a truncated GIF is never read");
			std::string runaway = GifFrame(10, 10, 10, 10).substr(0, 13);
			runaway += std::string("\x21\xF9\xFF", 3);
			runaway += std::string(20, '\x01');
			Check(!ReadImageSize(runaway, &gw, &gh), "a GIF extension that runs past the end is refused");
			std::string noFrame = GifFrame(10, 10, 10, 10).substr(0, 13);
			noFrame += '\x3B';
			Check(!ReadImageSize(noFrame, &gw, &gh), "a GIF with no frame is refused");
		}
	}

	// ---- FitPicture --------------------------------------------------------

	void TestFitPicture()
	{
		float w = -1.0f;
		float h = -1.0f;
		FitPicture(100, 50, 200.0f, 200.0f, &w, &h);
		Check(w == 100.0f && h == 50.0f, "a small picture is never enlarged");
		FitPicture(400, 200, 200.0f, 200.0f, &w, &h);
		Check(w == 200.0f && h == 100.0f, "a wide picture shrinks to the width");
		FitPicture(100, 400, 200.0f, 200.0f, &w, &h);
		Check(w == 50.0f && h == 200.0f, "a tall picture shrinks to the height, aspect kept");
		FitPicture(400, 400, 200.0f, 100.0f, &w, &h);
		Check(w == 100.0f && h == 100.0f, "both limits apply");
		FitPicture(0, 50, 200.0f, 200.0f, &w, &h);
		Check(w == 0.0f && h == 0.0f, "zero width gives nothing");
		FitPicture(50, -5, 200.0f, 200.0f, &w, &h);
		Check(w == 0.0f && h == 0.0f, "negative height gives nothing");
		FitPicture(50, 50, 0.0f, 200.0f, &w, &h);
		Check(w == 0.0f && h == 0.0f, "no room gives nothing");
	}

	// ---- the texture handoff -----------------------------------------------

	// Stands in for the texture calls and the game thread: `post` only queues; the
	// test runs the queue itself.
	struct FakeTex
	{
		std::mutex mutex;
		std::vector<std::function<void()>> queue;
		std::map<void*, std::pair<int, int>> live;   // loaded and not yet freed
		std::map<size_t, std::string> bytes;         // by slot, until taken
		std::map<size_t, std::pair<int, int>> sizeOf;   // reported size by slot, default 64 x 32
		int loads = 0;
		int frees = 0;
		int takes = 0;
		int nextHandle = 1;
		bool throwOnLoad = false;
		bool nullOnLoad = false;
		std::function<void()> inLoad;                // runs inside load, before it returns

		static std::string Valid() { return PngSized(64, 32); }

		size_t Queued()
		{
			std::lock_guard<std::mutex> lock(mutex);
			return queue.size();
		}

		size_t LiveCount()
		{
			std::lock_guard<std::mutex> lock(mutex);
			return live.size();
		}

		// Runs one queued job on the calling thread, the way the game thread would.
		bool RunOne()
		{
			std::function<void()> job;
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (queue.empty())
					return false;
				job = queue.front();
				queue.erase(queue.begin());
			}
			job();
			return true;
		}
	};

	TextureOps MakeOps(const std::shared_ptr<FakeTex>& t)
	{
		TextureOps ops;
		ops.load = [t](const void*, size_t, const char*) -> void*
		{
			std::function<void()> hook;
			{
				std::lock_guard<std::mutex> lock(t->mutex);
				++t->loads;
				hook = t->inLoad;
			}
			if (hook)
				hook();
			std::lock_guard<std::mutex> lock(t->mutex);
			if (t->throwOnLoad)
				throw std::runtime_error("no texture slot");
			if (t->nullOnLoad)
				return nullptr;
			void* h = reinterpret_cast<void*>(static_cast<uintptr_t>(t->nextHandle++));
			t->live[h] = { 64, 32 };
			return h;
		};
		ops.free = [t](void* h)
		{
			if (!h)
				return;
			std::lock_guard<std::mutex> lock(t->mutex);
			++t->frees;
			t->live.erase(h);
		};
		ops.size = [t](void* h, int* w, int* hh)
		{
			std::lock_guard<std::mutex> lock(t->mutex);
			auto it = t->live.find(h);
			*w = it == t->live.end() ? 0 : it->second.first;
			*hh = it == t->live.end() ? 0 : it->second.second;
		};
		ops.post = [t](std::function<void()> job)
		{
			std::lock_guard<std::mutex> lock(t->mutex);
			t->queue.push_back(std::move(job));
		};
		ops.take = [t](size_t slot)
		{
			std::lock_guard<std::mutex> lock(t->mutex);
			++t->takes;
			auto it = t->bytes.find(slot);
			if (it == t->bytes.end())
				return std::string();
			std::string out = std::move(it->second);
			t->bytes.erase(it);
			return out;
		};
		return ops;
	}

	void TestTexturesBasics()
	{
		auto t = std::make_shared<FakeTex>();
		t->bytes[0] = FakeTex::Valid();
		t->bytes[1] = FakeTex::Valid();
		ReadmeTextures tex(MakeOps(t));
		tex.BeginPage();

		Check(tex.Get(0).state == PictureState::None, "a new page has no pictures");
		tex.Request(0);
		Check(tex.Get(0).state == PictureState::Decoding && t->Queued() == 1, "Request posts one job and marks Decoding");
		Check(t->loads == 0 && t->takes == 0, "Request itself never decodes or takes");
		tex.Request(0);
		tex.Request(1);
		Check(t->Queued() == 1, "one job at a time: further requests while one is in flight do nothing");
		Check(tex.Get(1).state == PictureState::None, "the second picture waits");

		Check(t->RunOne(), "the job runs");
		Picture p = tex.Get(0);
		Check(p.state == PictureState::Ready && p.handle != nullptr && p.width == 64 && p.height == 32, "Ready with the texture and its size");
		Check(tex.Held() == 1 && t->LiveCount() == 1, "one texture held");

		tex.Request(0);
		Check(t->Queued() == 0, "a Ready picture is not requested again");
		tex.Request(1);
		Check(t->Queued() == 1, "the next request goes through once the first job ended");
		t->RunOne();
		Check(tex.Held() == 2 && t->LiveCount() == 2, "two textures held");

		tex.Request(kMaxPictures);
		tex.Request(9999);
		Check(t->Queued() == 0, "a slot out of range is ignored");
		Check(tex.Get(kMaxPictures).state == PictureState::None, "Get out of range is empty");

		tex.EndPage();
		Check(tex.Held() == 0 && t->LiveCount() == 0 && t->frees == 2, "EndPage frees every texture");
		Check(tex.Get(0).state == PictureState::None && tex.Get(1).state == PictureState::None, "EndPage clears the slots");

		t->bytes[0] = FakeTex::Valid();
		tex.BeginPage();
		tex.Request(0);
		t->RunOne();
		Check(tex.Get(0).state == PictureState::Ready, "a new page can load again");
		tex.BeginPage();
		Check(tex.Held() == 0 && t->LiveCount() == 0, "BeginPage frees the old page's textures");
	}

	void TestTexturesFailures()
	{
		{
			// Bytes already taken or none: Failed, and it is not retried.
			auto t = std::make_shared<FakeTex>();
			ReadmeTextures tex(MakeOps(t));
			tex.BeginPage();
			tex.Request(0);
			t->RunOne();
			Check(tex.Get(0).state == PictureState::Failed && t->loads == 0, "no bytes: Failed without a load");
			tex.Request(0);
			Check(t->Queued() == 0, "a Failed picture is not retried");
		}
		{
			auto t = std::make_shared<FakeTex>();
			t->bytes[0] = FakeTex::Valid();
			t->throwOnLoad = true;
			ReadmeTextures tex(MakeOps(t));
			tex.BeginPage();
			tex.Request(0);
			t->RunOne();
			Check(tex.Get(0).state == PictureState::Failed && tex.Held() == 0, "a load that throws gives Failed");
			t->throwOnLoad = false;
			t->nullOnLoad = true;
			t->bytes[1] = FakeTex::Valid();
			tex.Request(1);
			t->RunOne();
			Check(tex.Get(1).state == PictureState::Failed, "a load that returns null gives Failed");
			Check(tex.Held() == 0 && t->LiveCount() == 0, "nothing leaks on a failed load");
		}
		{
			// A hostile header never reaches the decoder.
			auto t = std::make_shared<FakeTex>();
			t->bytes[0] = PngSized(60000, 60000);
			t->bytes[1] = PngSized(4097, 10);
			t->bytes[2] = PngBytes(40);   // a signature and nothing else
			t->bytes[3] = std::string(100, 'x');
			ReadmeTextures tex(MakeOps(t));
			tex.BeginPage();
			for (size_t slot = 0; slot < 4; ++slot)
			{
				tex.Request(slot);
				t->RunOne();
				Check(tex.Get(slot).state == PictureState::Failed, "a refused header gives Failed");
			}
			Check(t->loads == 0, "a refused header never reaches LoadFromMemory");
			Check(t->takes == 4 && t->bytes.empty(), "the bytes were taken (and dropped)");
		}
		{
			// The decoded size is a second guard.
			auto t = std::make_shared<FakeTex>();
			t->bytes[0] = FakeTex::Valid();
			auto ops = MakeOps(t);
			auto realLoad = ops.load;
			ops.load = [t, realLoad](const void* d, size_t n, const char* name) -> void*
			{
				void* h = realLoad(d, n, name);
				std::lock_guard<std::mutex> lock(t->mutex);
				t->live[h] = { 5000, 10 };   // the decoder says otherwise
				return h;
			};
			ReadmeTextures tex(ops);
			tex.BeginPage();
			tex.Request(0);
			t->RunOne();
			Check(tex.Get(0).state == PictureState::Failed && tex.Held() == 0, "a decoded size over the limit gives Failed");
			Check(t->LiveCount() == 0 && t->frees == 1, "and the texture is freed");
		}
		{
			// No way to post: Failed at once, nothing in flight.
			auto t = std::make_shared<FakeTex>();
			auto ops = MakeOps(t);
			ops.post = nullptr;
			ReadmeTextures tex(ops);
			tex.BeginPage();
			tex.Request(0);
			Check(tex.Get(0).state == PictureState::Failed, "without a game thread hop the picture fails");
		}
	}

	void TestTexturesClose()
	{
		{
			// Closed after the request, before the job runs: nothing is taken or made.
			auto t = std::make_shared<FakeTex>();
			t->bytes[0] = FakeTex::Valid();
			ReadmeTextures tex(MakeOps(t));
			tex.BeginPage();
			tex.Request(0);
			tex.EndPage();
			Check(t->RunOne(), "the stale job still runs");
			Check(t->takes == 0 && t->loads == 0, "a stale job takes nothing and loads nothing");
			Check(tex.Get(0).state == PictureState::None && tex.Held() == 0, "nothing is shown after Close");
			tex.BeginPage();
			t->bytes[0] = FakeTex::Valid();
			tex.Request(0);
			Check(t->Queued() == 1, "a new page can request once the stale job has ended");
			t->RunOne();
			Check(tex.Get(0).state == PictureState::Ready, "the new page loads");
			tex.EndPage();
		}
		{
			// Closed while the texture is being made: it is freed, never shown.
			auto t = std::make_shared<FakeTex>();
			t->bytes[0] = FakeTex::Valid();
			ReadmeTextures tex(MakeOps(t));
			tex.BeginPage();
			t->inLoad = [&tex] { tex.EndPage(); };
			tex.Request(0);
			t->RunOne();
			Check(t->loads == 1 && t->frees == 1 && t->LiveCount() == 0, "a texture made during Close is freed");
			Check(tex.Get(0).state == PictureState::None && tex.Held() == 0, "and never shown");
		}
		{
			// A new page begins while the old page's job is making a texture.
			auto t = std::make_shared<FakeTex>();
			t->bytes[0] = FakeTex::Valid();
			ReadmeTextures tex(MakeOps(t));
			tex.BeginPage();
			t->inLoad = [&tex] { tex.BeginPage(); };
			tex.Request(0);
			t->RunOne();
			Check(t->LiveCount() == 0 && tex.Get(0).state == PictureState::None, "the old page's texture does not appear on the new page");
		}
		{
			// A job posted for a texture owner that is already gone is harmless.
			auto t = std::make_shared<FakeTex>();
			t->bytes[0] = FakeTex::Valid();
			{
				ReadmeTextures tex(MakeOps(t));
				tex.BeginPage();
				tex.Request(0);
			}
			Check(t->RunOne(), "the job outlives its owner");
			Check(t->takes == 0 && t->loads == 0 && t->LiveCount() == 0, "and does nothing");
		}
	}

	void TestTexturesShutdown()
	{
		{
			auto t = std::make_shared<FakeTex>();
			t->bytes[0] = FakeTex::Valid();
			ReadmeTextures tex(MakeOps(t));
			tex.BeginPage();
			tex.Request(0);
			t->RunOne();
			Check(tex.Held() == 1, "one texture before Shutdown");
			Check(tex.Shutdown(1000), "Shutdown with no load running returns true");
			Check(tex.Held() == 0 && t->LiveCount() == 0, "Shutdown frees what is held");
			t->bytes[1] = FakeTex::Valid();
			tex.Request(1);
			Check(t->Queued() == 0, "no request after Shutdown");
		}
		{
			// Shutdown waits for a load that is running; its texture is freed, not kept.
			auto t = std::make_shared<FakeTex>();
			t->bytes[0] = FakeTex::Valid();
			std::mutex gm;
			std::condition_variable gcv;
			bool entered = false;
			bool release = false;
			t->inLoad = [&]
			{
				std::unique_lock<std::mutex> lock(gm);
				entered = true;
				gcv.notify_all();
				gcv.wait(lock, [&] { return release; });
			};
			ReadmeTextures tex(MakeOps(t));
			tex.BeginPage();
			tex.Request(0);
			std::thread game([&] { t->RunOne(); });
			{
				std::unique_lock<std::mutex> lock(gm);
				gcv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
			}
			Check(entered, "the load is running");
			Check(!tex.Shutdown(50), "Shutdown reports a load that is still running");
			{
				std::lock_guard<std::mutex> lock(gm);
				release = true;
			}
			gcv.notify_all();
			Check(tex.Shutdown(3000), "Shutdown returns true once the load ends");
			game.join();
			Check(tex.Held() == 0 && t->LiveCount() == 0 && t->frees == 1, "the texture made during Shutdown is freed");
			Check(tex.Get(0).state == PictureState::None, "and never shown");
		}
		{
			// The destructor frees too.
			auto t = std::make_shared<FakeTex>();
			t->bytes[0] = FakeTex::Valid();
			{
				ReadmeTextures tex(MakeOps(t));
				tex.BeginPage();
				tex.Request(0);
				t->RunOne();
				Check(t->LiveCount() == 1, "a texture is live");
			}
			Check(t->LiveCount() == 0, "destroying the owner frees its textures");
		}
	}

	// ---- the open-page helper ----------------------------------------------

	void TestOpenPage()
	{
		std::vector<std::wstring> opened;
		auto opener = [&opened](const std::wstring& u) { opened.push_back(u); };

		Check(OpenPage::Open("https://www.nexusmods.com/starrupture/mods/1", opener) && opened.size() == 1, "a Nexus page opens");
		Check(opened.back() == L"https://www.nexusmods.com/starrupture/mods/1", "the address is handed over as it is");
		Check(OpenPage::Open("https://github.com/o/r", opener) && opened.size() == 2, "a GitHub page opens");
		Check(OpenPage::Open("https://GitHub.com/o/r#readme", opener) && opened.size() == 3, "the host is compared without case");

		const size_t before = opened.size();
		const char* const refused[] = {
			"http://github.com/o/r",
			"https://evil.example.com/o/r",
			"https://github.com.evil.example.com/o/r",
			"https://user@github.com/o/r",
			"https://github.com:8080/o/r",
			"https://github.com/o/r with space",
			"https://github.com/o\\r",
			"https://raw.githubusercontent.com/o/r/main/README.md",
			"javascript:alert(1)",
			"file:///C:/Windows/System32/calc.exe",
			"ftp://github.com/o/r",
			"github.com/o/r",
			"https://",
			"",
		};
		for (const char* url : refused)
			Check(!OpenPage::Open(url, opener), "an address outside the allowlist is refused");
		Check(opened.size() == before, "a refused address never reaches the shell");
		Check(!OpenPage::Open(std::string("https://github.com/o/r\x01"), opener) && opened.size() == before, "a control character is refused");
		Check(!OpenPage::Open(std::string("https://github.com/") + std::string(300, 'a'), opener) && opened.size() == before, "an address over the length limit is refused");
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
	TestTables();
	TestFontText();
	TestAddressRules();
	TestFetcherOk();
	TestFetcherFailures();
	TestFetcherImageRules();
	TestFetcherAddressAndClose();
	TestFetcherShutdown();
	TestImageHeaderSize();
	TestFitPicture();
	TestTexturesBasics();
	TestTexturesFailures();
	TestTexturesClose();
	TestTexturesShutdown();
	TestOpenPage();

	printf("%d checks, %d failed\n", g_checks, g_failed);
	return g_failed == 0 ? 0 : 1;
}
