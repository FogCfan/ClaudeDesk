#include "ui/Markdown.h"

#include <cctype>
#include <cstring>

#include <imgui.h>
#include <imgui_internal.h>

#include "ui/Theme.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace ui {

// --- Analyse ------------------------------------------------------------------

static size_t leadingSpaces(std::string_view s) {
    size_t n = 0;
    for (char c : s) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4;
        else break;
    }
    return n;
}

static bool isRule(const std::string& t) {
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (char x : t)
        if (x != c && x != ' ') return false;
    return true;
}

std::vector<MdBlock> parseMarkdown(std::string_view src) {
    std::vector<MdBlock> out;
    auto lines = util::splitLines(src);
    MdBlock para;
    auto flush = [&] {
        if (!para.text.empty()) out.push_back(para);
        para = MdBlock{};
    };
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        std::string t = util::trim(line);
        size_t indent = leadingSpaces(line);

        if (util::startsWith(t, "```") || util::startsWith(t, "~~~")) {
            flush();
            std::string fence = t.substr(0, 3);
            MdBlock b;
            b.type = MdBlock::Code;
            b.lang = util::trim(t.substr(3));
            for (++i; i < lines.size(); ++i) {
                if (util::startsWith(util::trim(lines[i]), fence)) break;
                std::string l = lines[i];
                // retire l'indentation commune de la clôture (blocs dans des listes)
                size_t strip = std::min(indent, leadingSpaces(l));
                b.text += l.substr(std::min(strip, l.size())) + "\n";
            }
            if (!b.text.empty()) b.text.pop_back();
            out.push_back(b);
            continue;
        }
        if (t.empty()) {
            flush();
            continue;
        }
        if (t[0] == '#') {
            size_t lvl = 0;
            while (lvl < t.size() && t[lvl] == '#') ++lvl;
            if (lvl <= 6 && lvl < t.size() && t[lvl] == ' ') {
                flush();
                out.push_back({MdBlock::Heading, (int)lvl, util::trim(t.substr(lvl))});
                continue;
            }
        }
        if (isRule(t)) {
            flush();
            out.push_back({MdBlock::Rule});
            continue;
        }
        if (t[0] == '>') {
            flush();
            std::string q = util::trim(t.substr(1));
            if (!out.empty() && out.back().type == MdBlock::Quote && i > 0 && !util::trim(lines[i - 1]).empty())
                out.back().text += "\n" + q;
            else
                out.push_back({MdBlock::Quote, 0, q});
            continue;
        }
        if (t[0] == '|') {
            flush();
            if (!out.empty() && out.back().type == MdBlock::Table && i > 0 && util::trim(lines[i - 1])[0] == '|')
                out.back().text += "\n" + t;
            else
                out.push_back({MdBlock::Table, 0, t});
            continue;
        }
        if (t.size() > 1 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
            flush();
            out.push_back({MdBlock::Bullet, (int)(indent / 2), util::trim(t.substr(2))});
            continue;
        }
        {
            size_t d = 0;
            while (d < t.size() && std::isdigit((unsigned char)t[d])) ++d;
            if (d > 0 && d < 4 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') {
                flush();
                MdBlock b{MdBlock::Numbered, (int)(indent / 2), util::trim(t.substr(d + 2))};
                b.number = t.substr(0, d + 1);
                out.push_back(b);
                continue;
            }
        }
        // Ligne de continuation d'un élément de liste
        if (para.text.empty() && indent >= 2 && !out.empty() &&
            (out.back().type == MdBlock::Bullet || out.back().type == MdBlock::Numbered) && i > 0 &&
            !util::trim(lines[i - 1]).empty()) {
            out.back().text += " " + t;
            continue;
        }
        // Paragraphe : on conserve les retours à la ligne (Claude s'en sert souvent).
        if (!para.text.empty()) para.text += "\n";
        para.text += t;
    }
    flush();
    return out;
}

// --- Texte enrichi en ligne ---------------------------------------------------

namespace {

enum SegKind { Normal, Bold, Italic, InlineCode, Link };
struct Seg {
    std::string text;
    SegKind kind = Normal;
    std::string url;
};

std::vector<Seg> parseInline(std::string_view s) {
    std::vector<Seg> segs;
    std::string cur;
    bool bold = false, italic = false;
    auto push = [&](SegKind forced = Normal) {
        if (cur.empty()) return;
        Seg sg;
        sg.text = cur;
        sg.kind = forced != Normal ? forced : bold ? Bold : italic ? Italic : Normal;
        segs.push_back(sg);
        cur.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '`') {
            size_t end = s.find('`', i + 1);
            if (end != std::string_view::npos) {
                push();
                cur = std::string(s.substr(i + 1, end - i - 1));
                push(InlineCode);
                i = end;
                continue;
            }
        }
        if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) {
            push();
            bold = !bold;
            ++i;
            continue;
        }
        if (c == '*') {
            bool canOpen = !italic && i + 1 < s.size() && s[i + 1] != ' ';
            bool canClose = italic && i > 0 && s[i - 1] != ' ';
            if (canOpen || canClose) {
                push();
                italic = !italic;
                continue;
            }
        }
        if (c == '[') {
            size_t close = s.find("](", i);
            size_t end = close == std::string_view::npos ? close : s.find(')', close);
            if (close != std::string_view::npos && end != std::string_view::npos && s.find('\n', i) > end) {
                push();
                Seg sg;
                sg.text = std::string(s.substr(i + 1, close - i - 1));
                sg.url = std::string(s.substr(close + 2, end - close - 2));
                sg.kind = Link;
                segs.push_back(sg);
                i = end;
                continue;
            }
        }
        cur += c;
    }
    push();
    return segs;
}

}  // namespace

void renderRichText(std::string_view text, const MdStyle& st, float wrapRight) {
    const Palette& pal = palette();
    float lineStartX = ImGui::GetCursorScreenPos().x;
    bool atLineStart = true;
    float lastMaxX = lineStartX;
    auto segs = parseInline(text);
    if (segs.empty()) {
        ImGui::NewLine();
        return;
    }
    for (auto& sg : segs) {
        ImFont* font = sg.kind == Bold ? st.bold : sg.kind == InlineCode ? st.mono
                       : (sg.kind == Italic && st.italic) ? st.italic : st.body;
        // Le code en ligne garde la taille courante (titres…) mais un peu réduite.
        float unscaled = ImGui::GetFontSize() / (ImGui::GetStyle().FontScaleMain * ImGui::GetStyle().FontScaleDpi);
        if (font) ImGui::PushFont(font, sg.kind == InlineCode ? unscaled * 0.84f : 0.0f);
        ImVec4 color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (sg.kind == InlineCode) color = pal.codeText;
        else if (sg.kind == Link) color = pal.link;
        else if (sg.kind == Italic && !st.italic) color = ImVec4(color.x * 0.85f, color.y * 0.85f, color.z * 0.85f, color.w);

        // Découpe en jetons « mot + espaces qui suivent » ; \n force un retour.
        const std::string& s = sg.text;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '\n') {
                if (atLineStart) ImGui::NewLine();
                atLineStart = true;
                ++i;
                continue;
            }
            size_t j = i;
            while (j < s.size() && s[j] != ' ' && s[j] != '\n') ++j;
            while (j < s.size() && s[j] == ' ') ++j;
            std::string_view tok(s.data() + i, j - i);
            i = j;
            if (atLineStart) {
                size_t k = 0;
                while (k < tok.size() && tok[k] == ' ') ++k;
                tok.remove_prefix(k);
                if (tok.empty()) continue;
            }
            // Largeur sans les espaces finaux, pour décider du retour à la ligne.
            std::string_view word = tok;
            while (!word.empty() && word.back() == ' ') word.remove_suffix(1);
            float wordW = ImGui::CalcTextSize(word.data(), word.data() + word.size()).x;
            if (!atLineStart) {
                if (lastMaxX + wordW > wrapRight) atLineStart = true;
                else ImGui::SameLine(0, 0);
            }
            // Mot plus long qu'une ligne entière : on le coupe.
            while (atLineStart && wordW > wrapRight - lineStartX && wrapRight - lineStartX > 20) {
                ImFont* f = ImGui::GetFont();
                const char* cut = f->CalcWordWrapPosition(ImGui::GetFontSize(), tok.data(), tok.data() + tok.size(),
                                                          wrapRight - lineStartX);
                if (cut <= tok.data()) cut = tok.data() + 1;
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(tok.data(), cut);
                ImGui::PopStyleColor();
                tok.remove_prefix(cut - tok.data());
                word = tok;
                while (!word.empty() && word.back() == ' ') word.remove_suffix(1);
                wordW = ImGui::CalcTextSize(word.data(), word.data() + word.size()).x;
            }
            if (tok.empty()) continue;
            ImVec2 pos = ImGui::GetCursorScreenPos();
            if (sg.kind == InlineCode) {
                ImVec2 sz = ImGui::CalcTextSize(word.data(), word.data() + word.size());
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x - 2, pos.y), ImVec2(pos.x + sz.x + 2, pos.y + sz.y),
                                                          col(pal.codeBg), 3.0f);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::TextUnformatted(tok.data(), tok.data() + tok.size());
            ImGui::PopStyleColor();
            if (sg.kind == Link) {
                ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y - 1), ImVec2(mn.x + wordW, mx.y - 1), col(color));
                    ImGui::SetTooltip("%s", sg.url.c_str());
                }
                if (ImGui::IsItemClicked()) util::openInShell(sg.url);
            }
            lastMaxX = ImGui::GetItemRectMax().x;
            atLineStart = false;
        }
        if (font) ImGui::PopFont();
    }
}

// --- Blocs --------------------------------------------------------------------

static void codeBlock(const MdBlock& b, const MdStyle& st, int id) {
    const Palette& pal = palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImDrawListSplitter splitter;
    splitter.Split(dl, 2);
    splitter.SetCurrentChannel(dl, 1);
    ImVec2 start = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float pad = ImGui::GetStyle().FramePadding.x;

    ImGui::PushID(id);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::Indent(pad);
    ImGui::TextColored(pal.dim, "%s", b.lang.empty() ? "code" : b.lang.c_str());
    ImGui::SameLine();
    float btnW = ImGui::CalcTextSize("Copier").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, width - pad * 2 - btnW - ImGui::GetItemRectSize().x - 8));
    if (ImGui::SmallButton("Copier")) ImGui::SetClipboardText(b.text.c_str());
    if (st.mono) ImGui::PushFont(st.mono, ImGui::GetStyle().FontSizeBase * 0.84f);
    ImGui::PushStyleColor(ImGuiCol_Text, pal.codeText);
    if (st.wrapCode) {
        ImGui::PushTextWrapPos(start.x + width - pad - ImGui::GetWindowPos().x);
        ImGui::TextUnformatted(b.text.c_str());
        ImGui::PopTextWrapPos();
    } else {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("code", ImVec2(width - pad * 2, 0), ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
        ImGui::TextUnformatted(b.text.c_str());
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleColor();
    if (st.mono) ImGui::PopFont();
    ImGui::Unindent(pad);
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::EndGroup();
    ImGui::PopID();

    ImVec2 end(start.x + width, ImGui::GetItemRectMax().y);
    splitter.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(start, end, col(pal.codeBg), 6.0f);
    splitter.Merge(dl);
}

static std::vector<std::string> tableCells(const std::string& row) {
    std::string r = util::trim(row);
    if (!r.empty() && r.front() == '|') r.erase(0, 1);
    if (!r.empty() && r.back() == '|') r.pop_back();
    auto cells = util::split(r, '|');
    for (auto& c : cells) c = util::trim(c);
    return cells;
}

static void table(const MdBlock& b, const MdStyle& st, int id) {
    auto rows = util::splitLines(b.text);
    std::vector<std::vector<std::string>> cells;
    for (auto& r : rows) {
        auto c = tableCells(r);
        bool sep = !c.empty();
        for (auto& x : c)
            if (x.find_first_not_of("-: ") != std::string::npos) sep = false;
        if (!sep) cells.push_back(c);
    }
    if (cells.empty()) return;
    int ncols = 0;
    for (auto& r : cells) ncols = std::max(ncols, (int)r.size());
    ncols = std::min(ncols, 32);
    ImGui::PushID(id);
    if (ImGui::BeginTable("t", ncols, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (size_t r = 0; r < cells.size(); ++r) {
            ImGui::TableNextRow(r == 0 ? ImGuiTableRowFlags_Headers : 0);
            for (int c = 0; c < ncols; ++c) {
                ImGui::TableSetColumnIndex(c);
                if (c >= (int)cells[r].size()) continue;
                float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                if (r == 0 && st.bold) ImGui::PushFont(st.bold, 0.0f);
                renderRichText(cells[r][c], st, right);
                if (r == 0 && st.bold) ImGui::PopFont();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void renderMarkdown(const std::vector<MdBlock>& blocks, const MdStyle& st, int idSeed) {
    const Palette& pal = palette();
    float baseSize = ImGui::GetStyle().FontSizeBase;
    for (size_t i = 0; i < blocks.size(); ++i) {
        const MdBlock& b = blocks[i];
        float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        switch (b.type) {
            case MdBlock::Paragraph:
                renderRichText(b.text, st, right);
                break;
            case MdBlock::Heading: {
                float scale = b.level == 1 ? 1.35f : b.level == 2 ? 1.2f : b.level == 3 ? 1.08f : 1.0f;
                if (i > 0) ImGui::Dummy(ImVec2(0, 2));
                ImGui::PushFont(st.bold, baseSize * scale);
                ImGui::PushStyleColor(ImGuiCol_Text, b.level <= 2 ? pal.accent : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                MdStyle hs = st;
                hs.body = st.bold ? st.bold : st.body;
                renderRichText(b.text, hs, right);
                ImGui::PopStyleColor();
                ImGui::PopFont();
                break;
            }
            case MdBlock::Bullet:
            case MdBlock::Numbered: {
                float ind = ImGui::GetFontSize() * (0.6f + b.level * 1.1f);
                ImGui::Indent(ind);
                ImVec2 p = ImGui::GetCursorScreenPos();
                float h = ImGui::GetTextLineHeight();
                if (b.type == MdBlock::Bullet) {
                    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.25f, p.y + h * 0.55f), h * 0.13f,
                                                                col(b.level == 0 ? pal.accent : pal.dim));
                    ImGui::Dummy(ImVec2(h * 0.75f, 0));
                } else {
                    ImGui::TextColored(pal.accent, "%s", b.number.c_str());
                }
                ImGui::SameLine(0, b.type == MdBlock::Bullet ? 0 : h * 0.3f);
                renderRichText(b.text, st, right);
                ImGui::Unindent(ind);
                break;
            }
            case MdBlock::Code:
                codeBlock(b, st, idSeed * 1000 + (int)i);
                break;
            case MdBlock::Quote: {
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::Indent(14);
                ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
                renderRichText(b.text, st, right);
                ImGui::PopStyleColor();
                ImGui::Unindent(14);
                ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + 3, ImGui::GetItemRectMax().y), col(pal.accent, 0.7f));
                break;
            }
            case MdBlock::Rule:
                ImGui::Separator();
                break;
            case MdBlock::Table:
                table(b, st, idSeed * 1000 + (int)i);
                break;
        }
    }
}

void selectableText(const char* id, const std::string& text, float width, ImFont* font) {
    if (font) ImGui::PushFont(font, 0.0f);
    ImGuiStyle& s = ImGui::GetStyle();
    float innerW = width - s.FramePadding.x * 2 - s.ScrollbarSize;
    float h = ImGui::CalcTextSize(text.c_str(), nullptr, false, innerW).y + s.FramePadding.y * 2 + ImGui::GetTextLineHeight();
    h = std::min(h, ImGui::GetFontSize() * 40);
    ImGui::InputTextMultiline(id, const_cast<char*>(text.c_str()), text.size() + 1, ImVec2(width, h),
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
    if (font) ImGui::PopFont();
}

}  // namespace ui
