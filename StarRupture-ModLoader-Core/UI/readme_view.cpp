#include "pch.h"
#include "readme_view.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include "imgui_backend.h"
#include "theme.h"
#include "auto_update/readme_fetcher.h"
#include "auto_update/readme_textures.h"
#include "utils/game_thread_dispatch.h"
#include "utils/open_page.h"

#include <algorithm>
#include <string>

namespace UI::ReadmeView
{
	namespace
	{
		// Render thread only.
		bool               s_open = false;
		ModIndex::Entry    s_entry;           // a copy: the list may be refreshed while the page is open
		int                s_lastDrawnFrame = 0;

		// The decode and the texture upload run on the game thread (the same hop the
		// RELOAD button uses); freeing is safe from any thread.
		ModIndex::TextureOps MakeOps()
		{
			ModIndex::TextureOps ops;
			ops.load = [](const void* data, size_t size, const char* name) -> void*
			{
				IPluginImGuiTextures* const api = UI::ImGuiBackend::GetTextureAPI();
				return (api && api->LoadFromMemory) ? api->LoadFromMemory(data, size, name) : nullptr;
			};
			ops.free = [](void* handle)
			{
				IPluginImGuiTextures* const api = UI::ImGuiBackend::GetTextureAPI();
				if (api && api->FreeTexture)
					api->FreeTexture(handle);
			};
			ops.size = [](void* handle, int* width, int* height)
			{
				IPluginImGuiTextures* const api = UI::ImGuiBackend::GetTextureAPI();
				if (api && api->GetSize)
					api->GetSize(handle, width, height);
			};
			ops.post = [](std::function<void()> job) { GameThreadDispatch::PostVoid(std::move(job)); };
			ops.take = [](size_t slot) { return ModIndex::ReadmeInstance().TakeImageBytes(slot); };
			return ops;
		}

		// Made on first use and kept to the end of the process; Shutdown ends it.
		ModIndex::ReadmeTextures& Textures()
		{
			static ModIndex::ReadmeTextures* const t = new ModIndex::ReadmeTextures(MakeOps());
			return *t;
		}

		void StartRequest()
		{
			Textures().BeginPage();
			ModIndex::ReadmeInstance().Open(s_entry.id, s_entry.readmeUrl);
		}

		// -------------------------------------------------------------------
		// Text
		// -------------------------------------------------------------------

		// Widgets that take a label (TextLink, SeparatorText) read "##" as the start
		// of an ID and hide what follows. A space between the two marks shows all of it.
		std::string SafeLabel(std::string text)
		{
			for (size_t i = 1; i < text.size(); ++i)
				if (text[i] == '#' && text[i - 1] == '#')
					text.insert(i, " ");
			return text;
		}

		// A run of words in one style, flowed across the line end. Wrapping a
		// paragraph that mixes styles cannot be left to TextWrapped (a wrapped item
		// restarts at its own x), so this places one word at a time: it asks the
		// framework for the word's width and the previous item's right edge, and
		// keeps the word on the line if it fits.
		class Flow
		{
		public:
			Flow()
				: m_width(ImGui::GetContentRegionAvail().x)
				, m_lineRight(ImGui::GetCursorScreenPos().x + m_width)
			{
			}

			void Spans(const std::vector<ModIndex::Span>& spans)
			{
				for (const ModIndex::Span& sp : spans)
				{
					const std::string& t = sp.text;
					size_t i = 0;
					while (i < t.size())
					{
						if (t[i] == '\n')
						{
							LineBreak();
							++i;
							continue;
						}
						size_t end = i;
						while (end < t.size() && t[end] != ' ' && t[end] != '\n')
							++end;
						const size_t wordEnd = end;
						while (end < t.size() && t[end] == ' ')
							++end;
						Word(sp, t.c_str() + i, wordEnd - i, end - wordEnd);
						i = end;
					}
				}
			}

		private:
			void LineBreak()
			{
				if (!m_lineStarted)
					ImGui::NewLine();
				m_lineStarted = false;
			}

			// `word` is the word's text, `spaces` the blanks after it.
			void Word(const ModIndex::Span& sp, const char* word, size_t len, size_t spaces)
			{
				const char* const wordEnd = word + len;
				const float width = ImGui::CalcTextSize(word, wordEnd).x;
				// A word wider than the whole line (a long name in a narrow table
				// column) starts its own line and breaks inside the word.
				const bool tooWide = width > m_width;
				if (m_lineStarted)
				{
					// A broken word is one item several lines tall, and SameLine
					// would return to its top line: what follows starts below it.
					if (!m_broke && !tooWide && ImGui::GetItemRectMax().x + width <= m_lineRight)
						ImGui::SameLine(0.0f, 0.0f);
					// else: the cursor is already on the next line
				}
				m_lineStarted = true;
				m_broke = tooWide;

				ImGui::PushID(m_id++);
				const bool link = sp.kind == ModIndex::SpanKind::Link && sp.clickable;
				if (link)
				{
					// TextLink takes a C string: a word is cut out of the span's text.
					const std::string label = SafeLabel(std::string(word, len));
					if (ImGui::TextLink(label.c_str()))
						OpenPage::Open(sp.url);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", sp.url.c_str());
					if (spaces > 0)
					{
						ImGui::SameLine(0.0f, 0.0f);
						ImGui::TextUnformatted(" ");
					}
				}
				else
				{
					const char* const end = wordEnd + spaces;
					if (tooWide)
						ImGui::PushTextWrapPos(0.0f);
					if (sp.kind == ModIndex::SpanKind::Code)
						CodeBack(word, end, tooWide ? m_width : -1.0f);
					else if (sp.kind == ModIndex::SpanKind::Bold)
						ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::AccentColorVec4());
					ImGui::TextUnformatted(word, end);
					if (sp.kind == ModIndex::SpanKind::Bold)
						ImGui::PopStyleColor();
					if (tooWide)
						ImGui::PopTextWrapPos();
				}
				ImGui::PopID();
			}

			// Inline code is plain text on the input-box colour, so it cannot be
			// taken for a link (coloured and underlined). Call before the text.
			static void CodeBack(const char* text, const char* end, float wrapWidth)
			{
				const ImVec2 min = ImGui::GetCursorScreenPos();
				const ImVec2 size = ImGui::CalcTextSize(text, end, false, wrapWidth);
				ImGui::GetWindowDrawList()->AddRectFilled(
					min, ImVec2(min.x + size.x, min.y + size.y), ImGui::GetColorU32(ImGuiCol_FrameBg));
			}

			float m_width;
			float m_lineRight;
			bool  m_lineStarted = false;
			bool  m_broke = false;       // the last word was broken across lines
			int   m_id = 0;
		};

		// A paragraph or list item's spans. One plain span is the framework's own
		// wrapped text; mixed styles flow word by word, with the line gap taken out
		// so the lines sit as close as wrapped text does.
		void DrawSpans(const std::vector<ModIndex::Span>& spans, bool paragraphGap = true)
		{
			if (spans.empty())
				return;
			if (spans.size() == 1 && spans[0].kind == ModIndex::SpanKind::Plain)
			{
				ImGui::TextWrapped("%s", spans[0].text.c_str());
				return;
			}
			ImGuiStyle& style = ImGui::GetStyle();
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
			Flow().Spans(spans);
			ImGui::PopStyleVar();
			if (paragraphGap)
				ImGui::Spacing();
		}

		std::string Joined(const std::vector<ModIndex::Span>& spans)
		{
			std::string s;
			for (const ModIndex::Span& sp : spans)
				s += sp.text;
			return s;
		}

		// Levels 1 and 2 are section headers as the loader's own pages draw them;
		// deeper levels are accent text with a gap above.
		void DrawHeading(const ModIndex::Block& b)
		{
			const std::string text = Joined(b.spans);
			ImGui::Spacing();
			if (b.level >= 3)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, UI::Theme::AccentColorVec4());
				ImGui::TextWrapped("%s", text.c_str());
				ImGui::PopStyleColor();
				return;
			}
			// SeparatorText cuts a label short, so one that does not fit wraps
			// above a plain rule.
			const std::string label = SafeLabel(text);
			if (ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().SeparatorTextPadding.x * 2.0f
			    <= ImGui::GetContentRegionAvail().x)
			{
				ImGui::SeparatorText(label.c_str());
			}
			else
			{
				ImGui::TextWrapped("%s", text.c_str());
				ImGui::Separator();
			}
			ImGui::Spacing();
		}

		void DrawTableRow(const ModIndex::TableRow& row, ImGuiTableRowFlags flags)
		{
			ImGui::TableNextRow(flags);
			for (size_t c = 0; c < row.size(); ++c)
			{
				ImGui::TableSetColumnIndex(static_cast<int>(c));
				ImGui::PushID(static_cast<int>(c));
				DrawSpans(row[c], false);
				ImGui::PopID();
			}
		}

		// The width of the column's widest cell on one line. It is the column's
		// stretch weight: ImGui's own proportional weights are measured from the
		// cells as drawn, and these wrap to whatever width the column has. It stops
		// at `cap`, so one long column cannot squeeze the short ones to nothing.
		float ColumnWeight(const ModIndex::Block& b, size_t column, float cap)
		{
			// Runs every frame: a cell of one span is measured without a copy.
			const auto width = [](const ModIndex::TableCell& cell)
			{
				return ImGui::CalcTextSize(cell.size() == 1 ? cell[0].text.c_str() : Joined(cell).c_str()).x;
			};
			float widest = width(b.header[column]);
			for (const ModIndex::TableRow& row : b.rows)
				widest = (std::max)(widest, width(row[column]));
			return std::clamp(widest, 1.0f, cap);
		}

		// The loader's own table look. The header row is marked as one instead of
		// using TableHeadersRow, which takes plain labels that do not wrap.
		void DrawTable(const ModIndex::Block& b)
		{
			const ImGuiTableFlags flags =
				ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
			const int columns = static_cast<int>(b.header.size());
			const float equalShare = (std::max)(ImGui::GetContentRegionAvail().x / columns, 1.0f);
			if (!ImGui::BeginTable("##table", columns, flags))
				return;
			for (int c = 0; c < columns; ++c)
				ImGui::TableSetupColumn("##column", ImGuiTableColumnFlags_WidthStretch, ColumnWeight(b, c, equalShare));
			DrawTableRow(b.header, ImGuiTableRowFlags_Headers);
			for (size_t i = 0; i < b.rows.size(); ++i)
			{
				ImGui::PushID(static_cast<int>(i) + 1);
				DrawTableRow(b.rows[i], ImGuiTableRowFlags_None);
				ImGui::PopID();
			}
			ImGui::EndTable();
			ImGui::Spacing();
		}

		void DrawListItem(const ModIndex::Block& b)
		{
			if (b.depth > 0)
				ImGui::Indent();
			if (b.ordered)
			{
				ImGui::Text("%d.", b.number);
				ImGui::SameLine();
			}
			else
			{
				ImGui::Bullet();
			}
			ImGui::BeginGroup();
			DrawSpans(b.spans);
			ImGui::EndGroup();
			if (b.depth > 0)
				ImGui::Unindent();
		}

		// -------------------------------------------------------------------
		// Pictures
		// -------------------------------------------------------------------

		void DrawAlt(const ModIndex::Block& b)
		{
			ImGui::TextDisabled("[%s]", b.alt.empty() ? "picture" : b.alt.c_str());
		}

		void DrawImage(const ModIndex::Block& b, const ModIndex::ReadmeSnapshot& snap)
		{
			if (b.imageSlot < 0 || static_cast<size_t>(b.imageSlot) >= snap.images.size())
			{
				DrawAlt(b);
				return;
			}
			const size_t slot = static_cast<size_t>(b.imageSlot);
			const ModIndex::Picture pic = Textures().Get(slot);
			if (pic.state == ModIndex::PictureState::Ready)
			{
				IPluginImGuiTextures* const api = UI::ImGuiBackend::GetTextureAPI();
				float w = 0.0f;
				float h = 0.0f;
				ModIndex::FitPicture(pic.width, pic.height, ImGui::GetContentRegionAvail().x,
				                     ImGui::GetTextLineHeight() * 30.0f, &w, &h);
				if (api && api->Image && w > 0.0f)
					api->Image(pic.handle, w, h);
				return;
			}
			if (pic.state == ModIndex::PictureState::Failed || snap.images[slot].state == ModIndex::ImageState::Failed)
				DrawAlt(b);
			else
				ImGui::TextDisabled("Loading picture...");
		}

		// Asks for the next picture whose bytes have arrived. The texture is made on
		// the game thread, one at a time; this only asks.
		void RequestPictures(const ModIndex::ReadmeSnapshot& snap)
		{
			for (size_t i = 0; i < snap.images.size() && i < ModIndex::kMaxPictures; ++i)
				if (snap.images[i].state == ModIndex::ImageState::Bytes && !snap.images[i].taken)
					Textures().Request(i);
		}

		void DrawDocument(const ModIndex::ReadmeSnapshot& snap)
		{
			RequestPictures(snap);
			ImGui::BeginChild("##readme_doc", ImVec2(0, 0), ImGuiChildFlags_Borders);
			const std::vector<ModIndex::Block>& blocks = snap.document->blocks;
			for (size_t i = 0; i < blocks.size(); ++i)
			{
				const ModIndex::Block& b = blocks[i];
				ImGui::PushID(static_cast<int>(i));
				switch (b.kind)
				{
				case ModIndex::BlockKind::Heading:   DrawHeading(b);          break;
				case ModIndex::BlockKind::Paragraph: DrawSpans(b.spans);      break;
				case ModIndex::BlockKind::ListItem:  DrawListItem(b);         break;
				case ModIndex::BlockKind::Image:     DrawImage(b, snap);      break;
				case ModIndex::BlockKind::Rule:      ImGui::Separator();      break;
				case ModIndex::BlockKind::Table:     DrawTable(b);            break;
				case ModIndex::BlockKind::CodeBlock:
					ImGui::BeginChild("##code", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
					ImGui::TextWrapped("%s", b.code.c_str());
					ImGui::EndChild();
					break;
				}
				ImGui::PopID();
			}
			if (snap.document->shortened)
				ImGui::TextDisabled("This README is long, so only the start is shown.");
			ImGui::EndChild();
		}
	}

	void Open(const ModIndex::Entry& entry)
	{
		if (entry.readmeUrl.empty())
			return;
		s_entry = entry;
		s_open = true;
		s_lastDrawnFrame = ImGui::GetFrameCount();
		StartRequest();
	}

	void Close()
	{
		if (!s_open)
			return;
		s_open = false;
		ModIndex::ReadmeInstance().Close();
		Textures().EndPage();
	}

	bool IsOpen()
	{
		return s_open;
	}

	void BeginFrame()
	{
		if (s_open && ImGui::GetFrameCount() - s_lastDrawnFrame > 1)
			Close();
	}

	void Shutdown()
	{
		Textures().Shutdown(2000);
	}

	void Draw()
	{
		s_lastDrawnFrame = ImGui::GetFrameCount();

		ImGui::Spacing();
		if (ImGui::Button("Back"))
		{
			Close();
			return;
		}
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(s_entry.name.c_str());
		if (!s_entry.version.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("%s", s_entry.version.c_str());
		}
		if (!s_entry.author.empty())
		{
			ImGui::SameLine();
			ImGui::TextDisabled("by %s", s_entry.author.c_str());
		}
		ImGui::Spacing();

		const std::shared_ptr<const ModIndex::ReadmeSnapshot> snap = ModIndex::ReadmeInstance().Get();
		switch (snap->state)
		{
		case ModIndex::ReadmeState::Idle:
		case ModIndex::ReadmeState::Loading:
			ImGui::TextDisabled("Loading...");
			return;
		case ModIndex::ReadmeState::Failed:
			ImGui::TextUnformatted(snap->error.c_str());
			if (ImGui::Button("Retry"))
				StartRequest();
			return;
		case ModIndex::ReadmeState::Ready:
			break;
		}
		if (snap->document)
			DrawDocument(*snap);
	}
}

#endif // MODLOADER_CLIENT_BUILD
