#include "readme_markdown.h"
#include "readme_images.h"
#include "mod_index.h"

#include <algorithm>
#include <string_view>

// The reader is one forward scan per level: lines first (block structure), then
// the characters of one block (spans). Every search for a closing mark is
// bounded (kLook bytes), nothing recurses, and every output has a limit, so the
// work is linear in the text for any input.

namespace ModIndex
{
	namespace
	{
		constexpr size_t kLook     = 512;    // how far a closing mark is searched
		constexpr size_t kAttrLook = 1024;   // how far the end of an HTML tag is searched
		constexpr size_t kDestLook = 600;    // how far a link address and title are read
		constexpr size_t kPendingMax = kMaxSpans * kMaxSpanBytes + 1024;   // text of one block kept

		inline unsigned char U(char c) { return static_cast<unsigned char>(c); }
		inline bool IsAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
		inline bool IsSpace(char c) { return c == ' ' || c == '\n'; }

		// The largest end in [from, from + maxLen] that does not split a UTF-8 sequence.
		size_t CutUtf8(const std::string& s, size_t from, size_t maxLen)
		{
			size_t end = from + maxLen;
			if (end >= s.size())
				return s.size();
			while (end > from && (U(s[end]) & 0xC0) == 0x80)
				--end;
			return end;
		}

		void AppendUtf8(std::string& out, unsigned cp)
		{
			if (cp < 0x80)
				out += static_cast<char>(cp);
			else if (cp < 0x800)
			{
				out += static_cast<char>(0xC0 | (cp >> 6));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
			else if (cp < 0x10000)
			{
				out += static_cast<char>(0xE0 | (cp >> 12));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xF0 | (cp >> 18));
				out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
		}

		// Characters that never reach the screen: C1 controls, the byte order
		// mark, and the bidirectional overrides that can make text read backwards.
		bool IsDroppedCodePoint(unsigned cp)
		{
			return (cp >= 0x80 && cp <= 0x9F) || cp == 0xFEFF || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
		}

		// What the built-in font cannot draw: nullptr keeps the code point, ""
		// drops it, anything else replaces it. No replacement is longer in bytes
		// than what it replaces, so the size limits still hold.
		const char* FontText(unsigned cp)
		{
			if (cp == 0xA0)
				return " ";
			if (cp == 0xAD)
				return "";                       // soft hyphen
			if (cp < 0x300)
				return nullptr;

			switch (cp)
			{
			case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2212: return "-";
			case 0x2014: case 0x2015: return "--";
			case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: return "'";
			case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return "\"";
			case 0x2020: case 0x2021: return "+";     // daggers are footnote marks too
			// Check and cross marks carry a feature table's answers.
			case 0x2611: case 0x2705: case 0x2713: case 0x2714: return "yes";
			case 0x2715: case 0x2716: case 0x2717: case 0x2718: case 0x274C: return "no";
			case 0x2039: return "<";
			case 0x203A: return ">";
			case 0x2022: case 0x2023: case 0x2043: case 0x2217: case 0x2219: case 0x25A0: case 0x25A1:
			case 0x25AA: case 0x25AB: case 0x25CB: case 0x25CF: case 0x25E6:
			case 0x2726: return "*";                // a footnote mark in credit keys
			case 0x2026: return "...";
			case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: return " ";
			case 0x2044: case 0x2215: return "/";
			case 0x2122: return "TM";
			case 0x2190: case 0x27F5: return "<-";
			case 0x2192: case 0x21A6: case 0x2794: case 0x279C: case 0x27A1: case 0x27F6: return "->";
			case 0x2194: case 0x27F7: return "<->";
			case 0x21D0: case 0x2264: return "<=";
			case 0x21D2: return "=>";
			case 0x21D4: return "<=>";
			case 0x2265: return ">=";
			case 0x2260: return "!=";
			case 0x2248: return "~";
			case 0x2191: case 0x25B2: case 0x25B4: return "^";
			case 0x2193: case 0x25BC: case 0x25BE: return "v";
			case 0x25B6: case 0x25B8: case 0x25BA: return ">";
			case 0x25C0: case 0x25C2: case 0x25C4: return "<";
			// Box drawing, so that a folder tree still reads as one.
			case 0x2500: case 0x2501: case 0x2504: case 0x2505: case 0x2508: case 0x2509:
			case 0x254C: case 0x254D: case 0x2550: case 0x2574: case 0x2576: case 0x2578:
			case 0x257A: case 0x257C: case 0x257E: return "-";
			case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250A: case 0x250B:
			case 0x254E: case 0x254F: case 0x2551: case 0x2575: case 0x2577: case 0x2579:
			case 0x257B: case 0x257D: case 0x257F: return "|";
			case 0x2514: case 0x2515: case 0x2516: case 0x2517: case 0x2558: case 0x2559:
			case 0x255A: case 0x2570: return "`";
			default: break;
			}

			if (cp >= 0x2500 && cp <= 0x257F)
				return "+";                          // the other corners and joints
			if (cp >= 0x2580 && cp <= 0x259F)
				return "#";                          // block elements (bars)
			if (cp >= 0x2000 && cp <= 0x200A)
				return " ";
			if ((cp >= 0x0300 && cp <= 0x036F) ||    // combining accents: the letter stays
			    (cp >= 0x200B && cp <= 0x206F) ||    // joiners, marks, other punctuation
			    (cp >= 0x20D0 && cp <= 0x20FF) ||    // keycap and other combining signs
			    (cp >= 0x2100 && cp <= 0x2BFF) ||    // arrows, technical signs, shapes, dingbats
			    (cp >= 0xE000 && cp <= 0xF8FF) ||    // private use
			    (cp >= 0xFE00 && cp <= 0xFE0F) ||    // variation selectors
			    cp == 0xFFFC ||
			    (cp >= 0x1F000 && cp <= 0x1FBFF) ||  // emoji
			    cp >= 0xE0000)                       // tags, selectors, private use
				return "";
			return nullptr;
		}

		// Fits one block's (or cell's) shown text to the font, piece by piece, in
		// order. Where something was dropped, the space after it goes too when
		// there is already one, or a line start, before it.
		struct FontFit
		{
			bool afterSpace = true;     // nothing kept yet, or the last was a space or '\n'
			bool dropped = false;       // something was dropped since then

			void Apply(std::string& text)
			{
				std::string out;
				out.reserve(text.size());
				for (size_t i = 0; i < text.size();)
				{
					const unsigned char c = U(text[i]);
					const size_t len = std::min<size_t>(c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4, text.size() - i);
					unsigned cp = len == 1 ? c : c & (0x7Fu >> len);
					for (size_t k = 1; k < len; ++k)
						cp = (cp << 6) | (U(text[i + k]) & 0x3F);

					const char* const other = FontText(cp);
					const std::string_view piece = other ? std::string_view(other) : std::string_view(text).substr(i, len);
					i += len;
					if (piece.empty())
						dropped = true;
					else if (piece == " " && dropped && afterSpace)
						dropped = false;
					else
					{
						out.append(piece);
						afterSpace = IsSpace(piece.back());
						dropped = false;
					}
				}
				text = std::move(out);
			}

			// The next text is left as it is.
			void Skip() { afterSpace = dropped = false; }
		};

		void FitSpans(std::vector<Span>& spans)
		{
			FontFit fit;
			for (Span& sp : spans)
			{
				if (sp.kind == SpanKind::Link && sp.text == sp.url)
				{
					fit.Skip();                     // an address is shown as written
					continue;
				}
				fit.Apply(sp.text);
				// A usable link whose whole text was dropped shows its address.
				if (sp.text.empty() && sp.kind == SpanKind::Link && sp.clickable)
				{
					sp.text = sp.url;
					fit.Skip();
				}
			}
			spans.erase(std::remove_if(spans.begin(), spans.end(), [](const Span& sp) { return sp.text.empty(); }), spans.end());
			// Dropped at the very end: no space is left hanging.
			while (fit.dropped && !spans.empty() && spans.back().kind != SpanKind::Link)
			{
				std::string& t = spans.back().text;
				t.erase(t.find_last_not_of(' ') + 1);
				if (!t.empty())
					break;
				spans.pop_back();
			}
		}

		// Fits every shown text and removes the blocks that have none left.
		void FitToFont(Document& doc)
		{
			for (Block& b : doc.blocks)
			{
				FitSpans(b.spans);
				FontFit().Apply(b.code);
				FontFit().Apply(b.alt);
				for (TableCell& cell : b.header)
					FitSpans(cell);
				for (TableRow& row : b.rows)
					for (TableCell& cell : row)
						FitSpans(cell);
			}
			const auto nothingLeft = [](const Block& b)
			{
				switch (b.kind)
				{
				case BlockKind::Heading:
				case BlockKind::Paragraph:
				case BlockKind::ListItem:  return b.spans.empty();
				case BlockKind::CodeBlock: return b.code.find_first_not_of(" \n") == std::string::npos;
				case BlockKind::Image:     return b.url.empty() && b.alt.empty();
				default:                   return false;
				}
			};
			doc.blocks.erase(std::remove_if(doc.blocks.begin(), doc.blocks.end(), nothingLeft), doc.blocks.end());
		}

		// Cut at kMaxReadmeBytes, then: valid UTF-8 only (a bad byte becomes
		// U+FFFD), control characters and NUL out, tabs to four spaces, CR and CRLF
		// to LF.
		std::string Sanitize(const std::string& in, bool& shortened)
		{
			size_t n = in.size();
			shortened = n > kMaxReadmeBytes;
			if (shortened)
			{
				n = kMaxReadmeBytes;
				while (n > 0 && (U(in[n]) & 0xC0) == 0x80)
					--n;
			}

			std::string out;
			out.reserve(n);
			for (size_t i = 0; i < n;)
			{
				const unsigned char c = U(in[i]);
				if (c < 0x80)
				{
					if (c == '\n')
						out += '\n';
					else if (c == '\r')
					{
						if (i + 1 >= n || in[i + 1] != '\n')
							out += '\n';
					}
					else if (c == '\t')
						out += "    ";
					else if (c >= 0x20 && c != 0x7F)
						out += static_cast<char>(c);
					++i;
					continue;
				}

				size_t len = 0;
				unsigned cp = 0;
				if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
				else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
				else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }

				bool ok = len != 0 && i + len <= n;
				for (size_t k = 1; ok && k < len; ++k)
				{
					const unsigned char cc = U(in[i + k]);
					ok = (cc & 0xC0) == 0x80;
					cp = (cp << 6) | (cc & 0x3F);
				}
				if (ok)
				{
					const unsigned lowest = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
					ok = cp >= lowest && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
				}

				if (!ok)
				{
					out += "\xEF\xBF\xBD";
					++i;
					continue;
				}
				if (!IsDroppedCodePoint(cp))
					out.append(in, i, len);
				i += len;
			}
			return out;
		}

		// "&amp;" and the like at s[i]: appends the character to `out` and returns
		// how many bytes of `s` it used, or 0 when s[i] does not start an entity.
		size_t DecodeEntityAt(const std::string& s, size_t i, std::string& out)
		{
			if (s[i] != '&')
				return 0;
			const size_t last = std::min(s.size(), i + 11);
			size_t semi = i + 1;
			while (semi < last && s[semi] != ';')
				++semi;
			if (semi >= last || semi - i > 10)
				return 0;
			const std::string name = s.substr(i + 1, semi - i - 1);
			if (name == "amp") out += '&';
			else if (name == "lt") out += '<';
			else if (name == "gt") out += '>';
			else if (name == "quot") out += '"';
			else if (name == "apos") out += '\'';
			else if (name == "nbsp") out += ' ';
			else if (name.size() >= 2 && name[0] == '#')
			{
				const bool hex = name[1] == 'x' || name[1] == 'X';
				const size_t first = hex ? 2 : 1;
				if (first >= name.size())
					return 0;
				unsigned cp = 0;
				for (size_t k = first; k < name.size(); ++k)
				{
					const char c = name[k];
					unsigned d;
					if (c >= '0' && c <= '9') d = static_cast<unsigned>(c - '0');
					else if (hex && c >= 'a' && c <= 'f') d = static_cast<unsigned>(c - 'a' + 10);
					else if (hex && c >= 'A' && c <= 'F') d = static_cast<unsigned>(c - 'A' + 10);
					else return 0;
					cp = cp * (hex ? 16u : 10u) + d;
				}
				if (cp < 0x20 || cp == 0x7F || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || IsDroppedCodePoint(cp))
					return 0;
				AppendUtf8(out, cp);
			}
			else
				return 0;
			return semi - i + 1;
		}

		std::string DecodeEntities(const std::string& s)
		{
			std::string out;
			out.reserve(s.size());
			for (size_t i = 0; i < s.size();)
			{
				const size_t used = DecodeEntityAt(s, i, out);
				if (used)
					i += used;
				else
					out += s[i++];
			}
			return out;
		}

		std::string Trim(const std::string& s)
		{
			size_t a = 0, b = s.size();
			while (a < b && IsSpace(s[a])) ++a;
			while (b > a && IsSpace(s[b - 1])) --b;
			return s.substr(a, b - a);
		}

		// ---- table lines ------------------------------------------------

		// The cells of one table line, trimmed, "\|" turned into "|". A pipe
		// inside `code` does not split; a backtick run with no partner is text.
		// Only the first `maxCells` are returned; `total` counts them all.
		std::vector<std::string> SplitRow(std::string_view line, size_t maxCells, size_t& total)
		{
			while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
			while (!line.empty() && line.back() == ' ') line.remove_suffix(1);
			if (!line.empty() && line.front() == '|') line.remove_prefix(1);

			std::vector<std::string> cells;
			std::string cell;
			total = 0;
			const auto closeCell = [&]
			{
				if (++total <= maxCells)
					cells.push_back(Trim(cell));
				cell.clear();
			};

			const size_t n = line.size();
			bool endsWithPipe = false;
			for (size_t i = 0; i < n;)
			{
				const char c = line[i];
				endsWithPipe = false;
				if (c == '\\' && i + 1 < n)
				{
					if (line[i + 1] != '|')
						cell += c;
					cell += line[i + 1];
					i += 2;
				}
				else if (c == '`')
				{
					size_t run = 1;
					while (i + run < n && line[i + run] == '`') ++run;
					size_t end = i + run;       // only the opening marks, unless closed
					for (size_t k = i + run, lim = std::min(n, i + run + kLook); k < lim;)
					{
						if (line[k] != '`') { ++k; continue; }
						size_t r = 1;
						while (k + r < n && line[k + r] == '`') ++r;
						if (r == run) { end = k + r; break; }
						k += r;
					}
					for (; i < end; ++i)
						if (!(line[i] == '\\' && i + 1 < end && line[i + 1] == '|'))
							cell += line[i];
				}
				else if (c == '|')
				{
					closeCell();
					endsWithPipe = true;
					++i;
				}
				else
				{
					cell += c;
					++i;
				}
			}
			if (!endsWithPipe)
				closeCell();
			return cells;
		}

		// Only pipes, dashes, colons and spaces, with a pipe and a dash.
		bool IsDelimiterShape(std::string_view v)
		{
			return v.find_first_not_of("|-: ") == std::string_view::npos &&
			       v.find('|') != std::string_view::npos && v.find('-') != std::string_view::npos;
		}

		// The column count of a delimiter line, or 0 when it is not one.
		size_t DelimiterColumns(std::string_view v)
		{
			if (!IsDelimiterShape(v))
				return 0;
			size_t total = 0;
			for (const std::string& c : SplitRow(v, std::string::npos, total))
			{
				const size_t a = !c.empty() && c.front() == ':' ? 1 : 0;
				const size_t b = c.size() > a && c.back() == ':' ? c.size() - 1 : c.size();
				if (a >= b || c.find_first_not_of('-', a) < b)
					return 0;
			}
			return total;
		}

		// Block quote marks are dropped; the text reads as ordinary lines.
		std::string_view StripQuote(std::string_view line)
		{
			for (;;)
			{
				size_t ind = 0;
				while (ind < line.size() && ind < 3 && line[ind] == ' ') ++ind;
				if (ind >= line.size() || line[ind] != '>')
					return line;
				line.remove_prefix(ind + 1);
				if (!line.empty() && line[0] == ' ')
					line.remove_prefix(1);
			}
		}

		// At s[i] == '[': the matching ']' within kLook bytes, or npos.
		size_t MatchBracket(const std::string& s, size_t i)
		{
			int depth = 0;
			const size_t limit = std::min(s.size(), i + kLook);
			for (size_t k = i; k < limit; ++k)
			{
				const char c = s[k];
				if (c == '\\') { ++k; continue; }
				if (c == '[') ++depth;
				else if (c == ']' && --depth == 0) return k;
			}
			return std::string::npos;
		}

		// At s[open] == '(': reads "(address", an optional "title", and the ')'.
		bool ParseDest(const std::string& s, size_t open, std::string& dest, size_t& end)
		{
			const size_t limit = std::min(s.size(), open + 1 + kDestLook);
			size_t k = open + 1;
			while (k < limit && s[k] == ' ') ++k;
			if (k < limit && s[k] == '<')
			{
				size_t e = k + 1;
				while (e < limit && s[e] != '>') ++e;
				if (e >= limit)
					return false;
				dest = s.substr(k + 1, e - k - 1);
				k = e + 1;
			}
			else
			{
				int depth = 0;
				const size_t start = k;
				while (k < limit)
				{
					const char c = s[k];
					if (c == ' ') break;
					if (c == '(') ++depth;
					else if (c == ')') { if (depth == 0) break; --depth; }
					else if (c == '\\' && k + 1 < limit) ++k;
					++k;
				}
				dest = s.substr(start, k - start);
			}
			while (k < limit && s[k] == ' ') ++k;
			if (k < limit && (s[k] == '"' || s[k] == '\'' || s[k] == '('))
			{
				const char close = s[k] == '(' ? ')' : s[k];
				++k;
				while (k < limit && s[k] != close) ++k;
				if (k >= limit)
					return false;
				++k;
				while (k < limit && s[k] == ' ') ++k;
			}
			if (k >= limit || s[k] != ')')
				return false;
			end = k + 1;
			dest = DecodeEntities(dest);
			return true;
		}

		// The value of attribute `want` in the text between a tag's name and its '>'.
		std::string AttrValue(const std::string& tag, const char* want)
		{
			size_t i = 0;
			const size_t n = tag.size();
			while (i < n)
			{
				while (i < n && (tag[i] == ' ' || tag[i] == '\n' || tag[i] == '/')) ++i;
				std::string name;
				while (i < n && tag[i] != ' ' && tag[i] != '\n' && tag[i] != '=' && tag[i] != '/')
					name += detail::Lower(tag[i++]);
				while (i < n && (tag[i] == ' ' || tag[i] == '\n')) ++i;
				std::string value;
				if (i < n && tag[i] == '=')
				{
					++i;
					while (i < n && (tag[i] == ' ' || tag[i] == '\n')) ++i;
					if (i < n && (tag[i] == '"' || tag[i] == '\''))
					{
						const char q = tag[i++];
						while (i < n && tag[i] != q) value += tag[i++];
						if (i < n) ++i;
					}
					else
						while (i < n && tag[i] != ' ' && tag[i] != '\n') value += tag[i++];
				}
				if (name == want)
					return DecodeEntities(value);
			}
			return {};
		}

		bool IsInlineTag(const std::string& name)
		{
			static const char* const kInline[] = { "a", "b", "i", "em", "strong", "code", "span", "u", "s", "sub", "sup", "kbd", "small", "mark" };
			for (const char* t : kInline)
				if (name == t)
					return true;
			return false;
		}

		// The first `mark` at or after `from`, within kLook bytes, that follows a
		// non-space character. For a single '*' the next character must not be '*'.
		size_t FindClose(const std::string& s, size_t from, const char* mark)
		{
			const size_t len = mark[1] ? 2 : 1;
			const size_t limit = std::min(s.size(), from + kLook);
			for (size_t k = from; k + len <= limit; ++k)
			{
				if (s[k] != mark[0] || (len == 2 && s[k + 1] != mark[1]))
					continue;
				if (k > from && s[k - 1] == ' ')
					continue;
				if (len == 1 && k + 1 < s.size() && s[k + 1] == '*')
				{
					++k;
					continue;
				}
				return k;
			}
			return std::string::npos;
		}

		// One block's characters become spans (and, for an image in the middle of
		// the text, image blocks between two pieces of the block).
		struct InlineRun
		{
			Document& doc;
			const std::string& s;
			const std::string& base;
			Block cur;
			bool& stop;

			std::string plain;
			bool bold = false, ital = false, noCommentEnd = false;
			size_t boldEnd = 0, italEnd = 0;

			bool SpansFull() const { return cur.spans.size() >= kMaxSpans; }

			bool PushBlock(Block&& b)
			{
				if (doc.blocks.size() >= kMaxBlocks)
				{
					doc.shortened = true;
					stop = true;
					return false;
				}
				doc.blocks.push_back(std::move(b));
				return true;
			}

			void AddSpan(SpanKind kind, const std::string& text, const std::string& url = {}, bool clickable = false)
			{
				if (text.empty())
					return;
				if (kind == SpanKind::Link)
				{
					if (SpansFull())
					{
						doc.shortened = true;   // the page says text is missing
						return;
					}
					Span sp;
					sp.kind = kind;
					sp.text = text.substr(0, CutUtf8(text, 0, kMaxSpanBytes));
					sp.url = url;
					sp.clickable = clickable;
					cur.spans.push_back(std::move(sp));
					return;
				}
				size_t pos = 0;
				while (pos < text.size())
				{
					if (!cur.spans.empty() && cur.spans.back().kind == kind && cur.spans.back().text.size() < kMaxSpanBytes)
					{
						Span& last = cur.spans.back();
						const size_t end = CutUtf8(text, pos, kMaxSpanBytes - last.text.size());
						if (end > pos)
						{
							last.text.append(text, pos, end - pos);
							pos = end;
							continue;
						}
					}
					if (SpansFull())
					{
						if (!Trim(text.substr(pos)).empty())
							doc.shortened = true;   // the page says text is missing
						return;
					}
					size_t end = CutUtf8(text, pos, kMaxSpanBytes);
					if (end == pos)
						end = text.size();
					Span sp;
					sp.kind = kind;
					sp.text = text.substr(pos, end - pos);
					cur.spans.push_back(std::move(sp));
					pos = end;
				}
			}

			void FlushPlain()
			{
				if (!plain.empty())
				{
					AddSpan(bold ? SpanKind::Bold : SpanKind::Plain, plain);
					plain.clear();
				}
			}

			// Trims the block's ends and pushes it when anything is left.
			void FlushBlock()
			{
				FlushPlain();
				while (!cur.spans.empty() && cur.spans.front().kind != SpanKind::Link && Trim(cur.spans.front().text).empty())
					cur.spans.erase(cur.spans.begin());
				while (!cur.spans.empty() && cur.spans.back().kind != SpanKind::Link && Trim(cur.spans.back().text).empty())
					cur.spans.pop_back();
				if (!cur.spans.empty())
				{
					Span& first = cur.spans.front();
					if (first.kind != SpanKind::Link)
						first.text = first.text.substr(std::min(first.text.find_first_not_of(" \n"), first.text.size()));
					Span& last = cur.spans.back();
					if (last.kind != SpanKind::Link)
						last.text = last.text.substr(0, last.text.find_last_not_of(" \n") + 1);
					Block b = cur;
					PushBlock(std::move(b));
				}
				cur.spans.clear();
				if (cur.kind == BlockKind::ListItem)
				{
					cur.kind = BlockKind::Paragraph;
					cur.depth = 0;
				}
			}

			void EmitImage(const std::string& src, const std::string& altRaw)
			{
				FlushBlock();
				if (stop || src.size() > kMaxUrlLen)
					return;
				std::string alt = altRaw;
				for (char& c : alt)
					if (c == '\n')
						c = ' ';
				alt = Trim(alt);
				alt = alt.substr(0, CutUtf8(alt, 0, kMaxSpanBytes));

				const std::string url = ResolveImageUrl(src, base);
				const bool ok = !url.empty() && IsAllowedImageUrl(url);
				if (!ok && alt.empty())
					return;
				Block b;
				b.kind = BlockKind::Image;
				if (ok)
					b.url = url;
				b.alt = alt;
				PushBlock(std::move(b));
			}

			void AddLink(const std::string& text, const std::string& dest)
			{
				FlushPlain();
				const std::string shown = text.empty() ? dest : text;
				if (dest.empty() || dest.size() > kMaxUrlLen)
					AddSpan(bold ? SpanKind::Bold : SpanKind::Plain, shown);
				else
					AddSpan(SpanKind::Link, shown, dest, IsAllowedPageUrl(dest));
			}

			// Link text shows as plain text: code marks and ** are dropped.
			static std::string LinkText(const std::string& inner)
			{
				std::string t;
				for (size_t i = 0; i < inner.size(); ++i)
				{
					if (inner[i] == '`') continue;
					if (inner[i] == '*' && i + 1 < inner.size() && inner[i + 1] == '*') { ++i; continue; }
					t += inner[i] == '\n' ? ' ' : inner[i];
				}
				return Trim(t);
			}

			// s[i] == '['. Consumes a link (or the image in a linked image).
			bool TryLink(size_t& i)
			{
				const size_t close = MatchBracket(s, i);
				if (close == std::string::npos || close + 1 >= s.size() || s[close + 1] != '(')
					return false;
				std::string dest;
				size_t end = 0;
				if (!ParseDest(s, close + 1, dest, end))
					return false;
				const std::string inner = s.substr(i + 1, close - i - 1);

				if (inner.size() >= 2 && inner[0] == '!' && inner[1] == '[')
				{
					const size_t ic = MatchBracket(inner, 1);
					std::string isrc;
					size_t iend = 0;
					if (ic != std::string::npos && ic + 1 < inner.size() && inner[ic + 1] == '(' && ParseDest(inner, ic + 1, isrc, iend) && iend == inner.size())
					{
						EmitImage(isrc, inner.substr(2, ic - 2));
						i = end;
						return true;
					}
				}
				AddLink(LinkText(inner), dest);
				i = end;
				return true;
			}

			// s[i] == '!' and s[i + 1] == '['.
			bool TryImage(size_t& i)
			{
				const size_t close = MatchBracket(s, i + 1);
				if (close == std::string::npos || close + 1 >= s.size() || s[close + 1] != '(')
					return false;
				std::string dest;
				size_t end = 0;
				if (!ParseDest(s, close + 1, dest, end))
					return false;
				EmitImage(dest, s.substr(i + 2, close - i - 2));
				i = end;
				return true;
			}

			// s[i] == '<'.
			bool TryTag(size_t& i)
			{
				const size_t n = s.size();
				if (s.compare(i, 4, "<!--") == 0)
				{
					if (noCommentEnd)
						return false;
					const size_t e = s.find("-->", i + 4);
					if (e == std::string::npos)
					{
						noCommentEnd = true;
						return false;
					}
					i = e + 3;
					return true;
				}

				if (s.compare(i + 1, 4, "http") == 0)
				{
					const size_t lim = std::min(n, i + 1 + kMaxUrlLen + 1);
					size_t e = i + 1;
					while (e < lim && s[e] != '>' && s[e] != ' ' && s[e] != '\n' && s[e] != '<')
						++e;
					if (e < lim && s[e] == '>')
					{
						const std::string url = s.substr(i + 1, e - i - 1);
						if (url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0)
						{
							AddLink(url, url);
							i = e + 1;
							return true;
						}
					}
				}

				size_t j = i + 1;
				bool closing = false;
				if (j < n && s[j] == '/')
				{
					closing = true;
					++j;
				}
				std::string name;
				while (j < n && IsAlnum(s[j]) && name.size() < 16)
					name += detail::Lower(s[j++]);
				if (name.empty() || (j < n && IsAlnum(s[j])))
					return false;

				size_t k = j;
				char quote = 0;
				const size_t lim = std::min(n, j + kAttrLook);
				for (; k < lim; ++k)
				{
					const char c = s[k];
					if (quote)
					{
						if (c == quote)
							quote = 0;
					}
					else if (c == '"' || c == '\'')
						quote = c;
					else if (c == '>')
						break;
				}
				if (k >= lim)
					return false;

				if (name == "img" && !closing)
				{
					const std::string attrs = s.substr(j, k - j);
					EmitImage(AttrValue(attrs, "src"), AttrValue(attrs, "alt"));
				}
				else if (name == "br")
				{
					if (!plain.empty() || !cur.spans.empty())
						plain += '\n';
				}
				else if (!IsInlineTag(name) && !plain.empty() && !IsSpace(plain.back()))
					plain += ' ';
				i = k + 1;
				return true;
			}

			// s[i] is 'h' of a bare https:// address.
			bool TryBareUrl(size_t& i)
			{
				if (s.compare(i, 8, "https://") != 0 || (i > 0 && IsAlnum(s[i - 1])))
					return false;
				const size_t lim = std::min(s.size(), i + kMaxUrlLen + 1);
				size_t e = i;
				while (e < lim && s[e] != ' ' && s[e] != '\n' && s[e] != '<' && s[e] != '>' && s[e] != '"')
					++e;
				if (e >= lim && e - i > kMaxUrlLen)
					return false;
				while (e > i + 8 && std::string_view(".,;:!?)*_'").find(s[e - 1]) != std::string_view::npos)
					--e;
				if (e <= i + 8)
					return false;
				const std::string url = s.substr(i, e - i);
				AddLink(url, url);
				i = e;
				return true;
			}

			void Run()
			{
				const size_t n = s.size();
				size_t i = 0;
				while (i < n && !stop)
				{
					if (bold && i >= boldEnd)
					{
						FlushPlain();
						bold = false;
						if (i == boldEnd)
						{
							i += 2;
							continue;
						}
					}
					if (ital && i >= italEnd)
					{
						ital = false;
						if (i == italEnd)
						{
							++i;
							continue;
						}
					}

					const char c = s[i];
					if (c == '\\' && i + 1 < n && s[i + 1] > ' ' && s[i + 1] < 0x7F && !IsAlnum(s[i + 1]))
					{
						plain += s[i + 1];
						i += 2;
					}
					else if (c == '`')
					{
						size_t run = 1;
						while (i + run < n && s[i + run] == '`') ++run;
						size_t close = std::string::npos;
						for (size_t k = i + run, lim = std::min(n, i + run + kLook); k < lim;)
						{
							if (s[k] != '`') { ++k; continue; }
							size_t r = 1;
							while (k + r < n && s[k + r] == '`') ++r;
							if (r == run) { close = k; break; }
							k += r;
						}
						if (close == std::string::npos)
						{
							plain.append(run, '`');
							i += run;
						}
						else
						{
							FlushPlain();
							std::string code = s.substr(i + run, close - i - run);
							for (char& ch : code)
								if (ch == '\n')
									ch = ' ';
							if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ')
								code = code.substr(1, code.size() - 2);
							AddSpan(SpanKind::Code, code);
							i = close + run;
						}
					}
					else if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c)
					{
						const char mark[3] = { c, c, 0 };
						size_t close = std::string::npos;
						if (!bold && i + 2 < n && s[i + 2] != ' ' && s[i + 2] != c)
							close = FindClose(s, i + 3, mark);
						if (close != std::string::npos)
						{
							FlushPlain();
							bold = true;
							boldEnd = close;
						}
						else
							plain.append(2, c);
						i += 2;
					}
					else if (c == '*')
					{
						size_t close = std::string::npos;
						if (!ital && i + 1 < n && s[i + 1] != ' ')
							close = FindClose(s, i + 2, "*");
						if (close != std::string::npos)
						{
							ital = true;
							italEnd = close;
						}
						else
							plain += '*';
						++i;
					}
					else if (c == '[')
					{
						if (!TryLink(i))
						{
							plain += '[';
							++i;
						}
					}
					else if (c == '!' && i + 1 < n && s[i + 1] == '[')
					{
						if (!TryImage(i))
						{
							plain += '!';
							++i;
						}
					}
					else if (c == '<')
					{
						if (!TryTag(i))
						{
							plain += '<';
							++i;
						}
					}
					else if (c == 'h' && TryBareUrl(i))
					{
					}
					else if (c == '&')
					{
						const size_t used = DecodeEntityAt(s, i, plain);
						if (used)
							i += used;
						else
						{
							plain += '&';
							++i;
						}
					}
					else
					{
						plain += c;
						++i;
					}
				}
				FlushBlock();
			}
		};

		// The block structure: one line at a time.
		struct Parser
		{
			Document& doc;
			const std::string& base;
			bool stop = false;

			enum class Pending { None, Para, Item } pending = Pending::None;
			std::string text;               // the pending block's text
			bool hardBreak = false;         // the previous line ended with two spaces
			bool ordered = false;
			int depth = 0, number = 0;

			bool inFence = false;
			char fenceChar = 0;
			size_t fenceLen = 0;
			bool inIndented = false;
			std::string code;

			bool inTable = false;           // body lines add to doc.blocks.back()
			bool droppingTable = false;     // ...or are dropped: over the cell limit
			bool skipLine = false;          // the delimiter line, read with its header
			bool pipeRows = false;          // the pending text has a delimiter line that made no table
			bool pipeLine = false;          // the pending text's last line is a pipe line
			size_t tableCells = 0;          // in all tables so far
			std::vector<Block> pictures;    // from the open table's cells

			void Inline(Block meta, const std::string& t)
			{
				if (stop)
					return;
				InlineRun run{ doc, t, base, std::move(meta), stop };
				run.Run();
			}

			void Flush()
			{
				if (pending == Pending::Para)
				{
					Block b;
					b.kind = BlockKind::Paragraph;
					Inline(std::move(b), text);
				}
				else if (pending == Pending::Item)
				{
					Block b;
					b.kind = BlockKind::ListItem;
					b.ordered = ordered;
					b.depth = depth;
					b.number = number;
					Inline(std::move(b), text);
				}
				pending = Pending::None;
				text.clear();
				hardBreak = false;
				pipeRows = pipeLine = false;
				EndTable();
			}

			// The pictures found in the table's cells follow it.
			void EndTable()
			{
				inTable = droppingTable = false;
				for (Block& b : pictures)
				{
					if (stop)
						break;
					if (doc.blocks.size() >= kMaxBlocks)
					{
						doc.shortened = true;
						stop = true;
						break;
					}
					doc.blocks.push_back(std::move(b));
				}
				pictures.clear();
			}

			// One cell's text becomes spans. A picture cannot be drawn in a cell.
			TableCell Cell(const std::string& t)
			{
				Document part;
				bool partStop = false;
				InlineRun run{ part, t, base, Block{}, partStop };
				run.Run();
				if (part.shortened)
					doc.shortened = true;

				TableCell cell;
				for (Block& b : part.blocks)
				{
					if (b.kind == BlockKind::Image)
					{
						if (pictures.size() < kMaxBlocks)
							pictures.push_back(std::move(b));
						continue;
					}
					for (Span& sp : b.spans)
					{
						if (cell.size() >= kMaxSpans)
						{
							doc.shortened = true;
							break;
						}
						cell.push_back(std::move(sp));
					}
				}
				return cell;
			}

			// A line's cells as a row of exactly `columns` cells.
			TableRow Row(std::string_view line, size_t columns)
			{
				size_t total = 0;
				const std::vector<std::string> parts = SplitRow(line, columns, total);
				if (total > columns)
					doc.shortened = true;       // cells past the last column are dropped
				TableRow row(columns);
				for (size_t i = 0; i < parts.size(); ++i)
					row[i] = Cell(parts[i]);
				tableCells += columns;
				return row;
			}

			void TableLine(std::string_view line)
			{
				if (droppingTable)
					return;
				Block& table = doc.blocks.back();
				const size_t columns = table.header.size();
				if (table.rows.size() >= kMaxTableRows || tableCells + columns > kMaxTableCells)
					doc.shortened = true;
				else
					table.rows.push_back(Row(line, columns));
			}

			// `line` is a table's header when `next` is a delimiter line with as
			// many cells. A table over the cell limit is read and dropped.
			bool TryTable(std::string_view line, std::string_view next)
			{
				if (line.find('|') == std::string_view::npos)
					return false;
				next = StripQuote(next);
				const size_t first = next.find_first_not_of(' ');
				if (first == std::string_view::npos || first >= 4)
					return false;
				next.remove_prefix(first);
				while (!next.empty() && next.back() == ' ')
					next.remove_suffix(1);
				const size_t columns = DelimiterColumns(next);
				size_t headerCells = 0;
				if (columns == 0)
					return false;
				SplitRow(line, 0, headerCells);
				if (headerCells != columns)
					return false;

				Flush();
				if (stop)
					return true;
				skipLine = true;
				const size_t kept = std::min(columns, kMaxTableColumns);
				Block b;
				b.kind = BlockKind::Table;
				if (columns > kept)
					doc.shortened = true;
				if (doc.blocks.size() >= kMaxBlocks)
				{
					doc.shortened = true;
					stop = true;
				}
				else if (tableCells + kept > kMaxTableCells)
				{
					doc.shortened = true;
					tableCells = kMaxTableCells;
					droppingTable = inTable = true;
				}
				else
				{
					b.header = Row(line, kept);
					doc.blocks.push_back(std::move(b));
					inTable = true;
				}
				return true;
			}

			void EmitCode()
			{
				while (!code.empty() && code.back() == '\n')
					code.pop_back();
				if (!code.empty() && !stop)
				{
					if (doc.blocks.size() >= kMaxBlocks)
					{
						doc.shortened = true;
						stop = true;
					}
					else
					{
						Block b;
						b.kind = BlockKind::CodeBlock;
						b.code = code;
						doc.blocks.push_back(std::move(b));
					}
				}
				code.clear();
				inFence = inIndented = false;
			}

			void AddCodeLine(std::string_view line)
			{
				if (code.size() < kMaxCodeBytes)
				{
					const size_t room = kMaxCodeBytes - code.size();
					size_t take = std::min(line.size(), room);
					while (take > 0 && take < line.size() && (U(line[take]) & 0xC0) == 0x80)
						--take;
					code.append(line.substr(0, take));
					code += '\n';
					if (take < line.size())
						doc.shortened = true;   // the rest of the line is cut
				}
				else if (line.find_first_not_of(' ') != std::string_view::npos)
					doc.shortened = true;       // the rest of the code block is cut
			}

			void Heading(int level, std::string t)
			{
				Flush();
				t = Trim(t);
				size_t e = t.size();
				while (e > 0 && t[e - 1] == '#') --e;
				if (e == 0)
					t.clear();
				else if (t[e - 1] == ' ')
					t = Trim(t.substr(0, e));
				Block b;
				b.kind = BlockKind::Heading;
				b.level = std::min(level, 3);
				Inline(std::move(b), t);
			}

			void Rule()
			{
				Flush();
				if (stop)
					return;
				if (doc.blocks.size() >= kMaxBlocks)
				{
					doc.shortened = true;
					stop = true;
					return;
				}
				Block b;
				b.kind = BlockKind::Rule;
				doc.blocks.push_back(std::move(b));
			}

			static bool AllOf(std::string_view v, char c)
			{
				return !v.empty() && v.find_first_not_of(c) == std::string_view::npos;
			}

			// A line of three or more of one of - * _, spaces allowed between.
			static bool IsRule(std::string_view v)
			{
				if (v.empty() || (v[0] != '-' && v[0] != '*' && v[0] != '_'))
					return false;
				size_t count = 0;
				for (char c : v)
				{
					if (c == v[0]) ++count;
					else if (c != ' ') return false;
				}
				return count >= 3;
			}

			void Line(std::string_view line, std::string_view next)
			{
				if (inFence)
				{
					size_t ind = 0;
					while (ind < line.size() && ind < 3 && line[ind] == ' ') ++ind;
					std::string_view t = line.substr(ind);
					size_t run = 0;
					while (run < t.size() && t[run] == fenceChar) ++run;
					if (run >= fenceLen && t.find_first_not_of(' ', run) == std::string_view::npos)
						EmitCode();
					else
						AddCodeLine(line);
					return;
				}

				if (skipLine)
				{
					skipLine = false;
					return;
				}
				line = StripQuote(line);

				size_t indent = 0;
				while (indent < line.size() && line[indent] == ' ') ++indent;
				std::string_view rest = line.substr(indent);
				while (!rest.empty() && rest.back() == ' ')
					rest.remove_suffix(1);
				const bool hard = line.size() >= 2 && line.substr(line.size() - 2) == "  " && !rest.empty();

				if (rest.empty())
				{
					if (inIndented)
						code += code.size() < kMaxCodeBytes ? "\n" : "";
					else
						Flush();
					return;
				}

				if (inIndented)
				{
					if (indent >= 4)
					{
						AddCodeLine(line.substr(4));
						return;
					}
					EmitCode();
				}
				if (indent >= 4 && pending == Pending::None)
				{
					EndTable();
					inIndented = true;
					AddCodeLine(line.substr(4));
					return;
				}

				if (rest.size() >= 3 && (rest[0] == '`' || rest[0] == '~'))
				{
					size_t run = 0;
					while (run < rest.size() && rest[run] == rest[0]) ++run;
					if (run >= 3)
					{
						Flush();
						inFence = true;
						fenceChar = rest[0];
						fenceLen = run;
						return;
					}
				}

				if (rest[0] == '#')
				{
					size_t level = 0;
					while (level < rest.size() && rest[level] == '#') ++level;
					if (level <= 6 && (level == rest.size() || rest[level] == ' '))
					{
						Heading(static_cast<int>(level), std::string(rest.substr(level)));
						return;
					}
				}

				if (pending == Pending::Para && (AllOf(rest, '=') || AllOf(rest, '-')))
				{
					const int level = rest[0] == '=' ? 1 : 2;
					std::string t = text;
					pending = Pending::None;
					text.clear();
					Heading(level, t);
					return;
				}

				if (IsRule(rest))
				{
					Rule();
					return;
				}

				if (!inTable && TryTable(rest, next))
					return;

				// A list item: "- ", "* ", "+ ", "1. " or "1) ".
				size_t marker = 0;
				bool isOrdered = false;
				int num = 0;
				if ((rest[0] == '-' || rest[0] == '*' || rest[0] == '+') && rest.size() >= 2 && rest[1] == ' ')
					marker = 2;
				else if (rest[0] >= '0' && rest[0] <= '9')
				{
					size_t d = 0;
					while (d < rest.size() && d < 9 && rest[d] >= '0' && rest[d] <= '9')
						num = num * 10 + (rest[d++] - '0');
					if (d < rest.size() && (rest[d] == '.' || rest[d] == ')') && d + 1 < rest.size() && rest[d + 1] == ' ')
					{
						marker = d + 2;
						isOrdered = true;
					}
				}
				if (marker)
				{
					Flush();
					pending = Pending::Item;
					ordered = isOrdered;
					depth = indent >= 2 ? 1 : 0;
					number = num;
					text = std::string(rest.substr(marker));
					hardBreak = hard;
					return;
				}

				if (inTable)
				{
					if (rest[0] != '<')
					{
						TableLine(rest);
						return;
					}
					EndTable();                 // an HTML block ends the table
				}

				// Pipes that made no table keep their lines apart.
				const bool delimiter = IsDelimiterShape(rest);
				const bool isPipeLine = rest[0] == '|' || delimiter || (pipeRows && rest.find('|') != std::string_view::npos);
				const bool ownLine = isPipeLine || pipeLine;
				if (pending == Pending::None)
				{
					pending = Pending::Para;
					text = std::string(rest);
				}
				else if (text.size() < kPendingMax)
				{
					text += hardBreak || ownLine ? '\n' : ' ';
					text.append(rest);
				}
				else
					doc.shortened = true;   // the block's text is full: this line is cut
				pipeRows = pipeRows || delimiter;
				pipeLine = isPipeLine;
				hardBreak = hard;
			}

			void Run(const std::string& in)
			{
				size_t pos = 0;
				const std::string_view all(in);
				const auto lineAt = [&all](size_t from)
				{
					if (from >= all.size())
						return std::string_view();
					return all.substr(from, std::min(all.find('\n', from), all.size()) - from);
				};
				while (pos <= in.size() && !stop)
				{
					const std::string_view line = lineAt(pos);
					const size_t after = pos + line.size() + 1;
					Line(line, lineAt(after));
					pos = after;
				}
				if (inFence || inIndented)
					EmitCode();
				Flush();
			}
		};
	}

	Document ParseReadme(const std::string& textIn, const std::string& readmeUrl)
	{
		Document doc;
		const std::string text = Sanitize(textIn, doc.shortened);
		Parser parser{ doc, readmeUrl };
		parser.Run(text);
		FitToFont(doc);
		return doc;
	}
}
