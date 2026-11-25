#pragma once

#include <cstddef>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// readme_markdown.h -- a small, bounded reader for the markdown (and the bit of
// HTML) a GitHub README uses. Pure: text in, Document out, no I/O, no threads.
//
// The text comes from a third-party repository, so it is read as hostile: one
// forward scan with no recursion and no backtracking, and hard limits (below).
// Whatever is not understood shows as plain text; nothing is an error.
//
// Understood: # / ## / ### headings (#### and deeper count as level 3), setext
// headings, paragraphs, bulleted and numbered list items (two levels), fenced
// and indented code, horizontal rules, pipe tables, `code`, **bold**,
// [text](url), images ![alt](url), <https://...> and bare https:// links; block
// quote marks are dropped. HTML: <img src alt> is an image, <br> a line break, every other tag
// is dropped and its text kept, comments are dropped.
//
// A table is a header line, a delimiter line (| --- | :-: |) with as many cells,
// and the lines after it up to a blank line or another kind of block. Outer
// pipes are optional; "\|" and a pipe inside `code` do not split a cell. Lines
// with pipes that make no table stay one line each.
//
// Shown text (never an address) is fitted to the built-in font, which draws
// ASCII, Latin-1 and the euro sign: dashes, quotes, bullets, arrows, box
// drawing and the like become ASCII; emoji and other symbols are dropped.
// Letters of other scripts are kept for the fonts that have them.
// ---------------------------------------------------------------------------

namespace ModIndex
{
	constexpr size_t kMaxReadmeBytes = 128 * 1024;   // longer text is cut here, Document::shortened set
	constexpr size_t kMaxBlocks      = 1500;         // more blocks: the rest is dropped, shortened set
	constexpr size_t kMaxSpans       = 64;           // per block; the rest of that block is dropped
	constexpr size_t kMaxSpanBytes   = 2000;         // per span; longer plain text continues in the next span
	constexpr size_t kMaxCodeBytes   = 4000;         // per code block; the rest is dropped
	constexpr size_t kMaxTableColumns = 8;           // further columns are dropped, shortened set
	constexpr size_t kMaxTableRows    = 100;         // body rows per table; the rest is dropped, shortened set
	constexpr size_t kMaxTableCells   = 2000;        // all tables together, headers included; same
	// URLs longer than kMaxUrlLen (mod_index.h, 200 bytes): a link becomes plain
	// text, an image is skipped.

	enum class SpanKind { Plain, Bold, Code, Link };

	struct Span
	{
		SpanKind kind = SpanKind::Plain;
		std::string text;           // may hold '\n' for a line break (hard break or <br>)
		std::string url;            // Link only: as written; may be relative or not allowed
		bool clickable = false;     // Link only: url passes IsAllowedPageUrl (https, nexusmods.com or github.com)
	};

	enum class BlockKind { Heading, Paragraph, ListItem, CodeBlock, Image, Rule, Table };

	using TableCell = std::vector<Span>;   // may be empty; at most kMaxSpans spans
	using TableRow  = std::vector<TableCell>;

	struct Block
	{
		BlockKind kind = BlockKind::Paragraph;
		int  level   = 0;           // Heading: 1 to 3
		bool ordered = false;       // ListItem
		int  depth   = 0;           // ListItem: 0, or 1 for a nested item (deeper items count as 1)
		int  number  = 0;           // ListItem, ordered: the number as written
		std::vector<Span> spans;    // Heading, Paragraph, ListItem
		std::string code;           // CodeBlock: lines joined with '\n'
		std::string url;            // Image: the accepted https address; empty when refused (draw alt as text)
		std::string alt;            // Image
		int imageSlot = -1;         // Image: index into the fetcher's image slots; -1 = none (draw alt)
		TableRow header;            // Table: its size is the column count, 1 to kMaxTableColumns
		std::vector<TableRow> rows; // Table: body rows, each exactly header.size() cells; may be none
	};

	struct Document
	{
		std::vector<Block> blocks;
		bool shortened = false;     // the text or the block count hit its limit
	};

	// `readmeUrl` is the address the text came from; relative image paths resolve
	// against its folder (readme_images.h). Empty: relative images are refused.
	Document ParseReadme(const std::string& text, const std::string& readmeUrl);
}
