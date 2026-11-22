#include "ui/Theme.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

#include "ui/Anim.h"

namespace ui {

static Palette g_palette;

const Palette& palette() { return g_palette; }

static ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }

void applyTheme(ThemeId id, float scale, bool compact) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    if (id == ThemeId::Light) ImGui::StyleColorsLight(&s);
    else ImGui::StyleColorsDark(&s);

    // Style épuré, à la VS Code : coins à peine arrondis, séparateurs et barres de défilement fins, onglets droits.
    s.WindowRounding = 4;
    s.ChildRounding = 4;
    s.FrameRounding = 3;
    s.PopupRounding = 4;
    s.GrabRounding = 3;
    s.TabRounding = 0;
    s.ScrollbarRounding = 6;
    s.ScrollbarSize = 10;
    s.WindowPadding = compact ? ImVec2(8, 6) : ImVec2(10, 10);
    s.FramePadding = compact ? ImVec2(6, 3) : ImVec2(8, 5);
    s.ItemSpacing = compact ? ImVec2(6, 4) : ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(6, compact ? 4 : 5);
    s.IndentSpacing = compact ? 13 : 16;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBarBorderSize = 1;
    s.TabBarOverlineSize = 2;
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(compact ? 12 : 16, compact ? 2 : 3);
    s.DockingSeparatorSize = 1;

    ImVec4* c = s.Colors;
    Palette& p = g_palette;
    if (id == ThemeId::Light) {
        p.accent = rgb(201, 100, 66);
        p.accentSoft = rgb(201, 100, 66, 0.15f);
        p.userBg = rgb(240, 234, 226);
        p.cardBg = rgb(246, 244, 240);
        p.codeBg = rgb(238, 236, 232);
        p.codeText = rgb(70, 60, 120);
        p.dim = rgb(120, 116, 110);
        p.ok = rgb(40, 140, 70);
        p.warn = rgb(200, 130, 20);
        p.err = rgb(200, 50, 50);
        p.link = rgb(30, 100, 200);
        p.diffAdd = rgb(30, 130, 50);
        p.diffDel = rgb(190, 50, 50);
        c[ImGuiCol_WindowBg] = rgb(250, 249, 246);
        c[ImGuiCol_ChildBg] = rgb(250, 249, 246, 0);
        c[ImGuiCol_PopupBg] = rgb(255, 255, 255);
        c[ImGuiCol_FrameBg] = rgb(236, 233, 228);
        c[ImGuiCol_FrameBgHovered] = rgb(228, 222, 214);
        c[ImGuiCol_FrameBgActive] = rgb(222, 214, 204);
        c[ImGuiCol_TitleBg] = rgb(238, 235, 230);
        c[ImGuiCol_TitleBgActive] = rgb(232, 226, 218);
        c[ImGuiCol_MenuBarBg] = rgb(240, 237, 232);
        c[ImGuiCol_Header] = p.accentSoft;
        c[ImGuiCol_HeaderHovered] = rgb(0, 0, 0, 0.05f);
        c[ImGuiCol_HeaderActive] = rgb(201, 100, 66, 0.30f);
        c[ImGuiCol_Button] = rgb(232, 228, 222);
        c[ImGuiCol_ButtonHovered] = rgb(222, 216, 208);
        c[ImGuiCol_ButtonActive] = rgb(201, 100, 66, 0.35f);
        c[ImGuiCol_Tab] = rgb(236, 233, 228);
        c[ImGuiCol_TabHovered] = rgb(0, 0, 0, 0.05f);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = rgb(0, 0, 0, 0.14f);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(0, 0, 0, 0.24f);
        c[ImGuiCol_ScrollbarGrabActive] = rgb(0, 0, 0, 0.32f);
        c[ImGuiCol_TabSelected] = rgb(250, 249, 246);
        c[ImGuiCol_TabSelectedOverline] = p.accent;
        c[ImGuiCol_TabDimmedSelected] = rgb(244, 242, 238);
        c[ImGuiCol_CheckMark] = p.accent;
        c[ImGuiCol_CheckboxSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.22f);  // sinon : bleu d'ImGui
        c[ImGuiCol_SliderGrab] = p.accent;
        c[ImGuiCol_SliderGrabActive] = rgb(180, 85, 55);
        c[ImGuiCol_TextSelectedBg] = rgb(201, 100, 66, 0.30f);
        c[ImGuiCol_NavCursor] = p.accent;
        c[ImGuiCol_TextLink] = p.link;
        c[ImGuiCol_PlotHistogram] = p.accent;
        c[ImGuiCol_PlotLines] = p.accent;
        c[ImGuiCol_SeparatorHovered] = p.accent;
        c[ImGuiCol_DockingPreview] = rgb(201, 100, 66, 0.5f);
    } else {
        bool dusk = id == ThemeId::Dusk;
        p.accent = rgb(217, 119, 87);
        p.accentSoft = rgb(217, 119, 87, 0.18f);
        p.userBg = dusk ? rgb(52, 44, 62) : rgb(46, 42, 38);
        p.cardBg = dusk ? rgb(36, 32, 46) : rgb(34, 33, 31);
        p.codeBg = dusk ? rgb(24, 22, 32) : rgb(22, 22, 21);
        p.codeText = rgb(214, 200, 255);
        p.dim = rgb(150, 145, 138);
        p.ok = rgb(110, 200, 120);
        p.warn = rgb(235, 180, 70);
        p.err = rgb(240, 100, 95);
        p.link = rgb(120, 170, 255);
        p.diffAdd = rgb(110, 200, 120);
        p.diffDel = rgb(240, 110, 100);
        ImVec4 bg = dusk ? rgb(28, 25, 36) : rgb(27, 26, 25);
        ImVec4 bg2 = dusk ? rgb(34, 30, 44) : rgb(33, 32, 30);
        c[ImGuiCol_Text] = rgb(236, 232, 226);
        c[ImGuiCol_TextDisabled] = p.dim;
        c[ImGuiCol_WindowBg] = bg;
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = bg2;
        c[ImGuiCol_Border] = dusk ? rgb(60, 52, 76) : rgb(55, 53, 50);
        c[ImGuiCol_FrameBg] = dusk ? rgb(44, 39, 56) : rgb(42, 41, 39);
        c[ImGuiCol_FrameBgHovered] = dusk ? rgb(54, 48, 68) : rgb(52, 50, 47);
        c[ImGuiCol_FrameBgActive] = dusk ? rgb(62, 55, 78) : rgb(60, 57, 53);
        c[ImGuiCol_TitleBg] = bg;
        c[ImGuiCol_TitleBgActive] = bg2;
        c[ImGuiCol_MenuBarBg] = dusk ? rgb(32, 28, 41) : rgb(31, 30, 28);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = rgb(255, 255, 255, 0.10f);
        c[ImGuiCol_ScrollbarGrabHovered] = rgb(255, 255, 255, 0.18f);
        c[ImGuiCol_ScrollbarGrabActive] = rgb(255, 255, 255, 0.26f);
        c[ImGuiCol_CheckMark] = p.accent;
        c[ImGuiCol_CheckboxSelectedBg] = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.22f);  // sinon : bleu d'ImGui
        c[ImGuiCol_SliderGrab] = p.accent;
        c[ImGuiCol_SliderGrabActive] = rgb(235, 140, 105);
        c[ImGuiCol_Button] = dusk ? rgb(46, 41, 59) : rgb(42, 41, 39);
        c[ImGuiCol_ButtonHovered] = dusk ? rgb(60, 54, 76) : rgb(56, 54, 51);
        c[ImGuiCol_ButtonActive] = rgb(217, 119, 87, 0.55f);
        c[ImGuiCol_Header] = p.accentSoft;
        c[ImGuiCol_HeaderHovered] = rgb(255, 255, 255, 0.06f);
        c[ImGuiCol_HeaderActive] = rgb(217, 119, 87, 0.30f);
        c[ImGuiCol_Separator] = c[ImGuiCol_Border];
        c[ImGuiCol_SeparatorHovered] = p.accent;
        c[ImGuiCol_SeparatorActive] = p.accent;
        c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered] = p.accentSoft;
        c[ImGuiCol_ResizeGripActive] = p.accent;
        c[ImGuiCol_Tab] = bg;
        c[ImGuiCol_TabHovered] = rgb(255, 255, 255, 0.07f);
        c[ImGuiCol_TabSelected] = bg2;
        c[ImGuiCol_TabSelectedOverline] = p.accent;
        c[ImGuiCol_TabDimmed] = bg;
        c[ImGuiCol_TabDimmedSelected] = bg2;
        c[ImGuiCol_DockingPreview] = rgb(217, 119, 87, 0.5f);
        c[ImGuiCol_PlotHistogram] = p.accent;
        c[ImGuiCol_PlotLines] = p.accent;
        c[ImGuiCol_TextSelectedBg] = rgb(217, 119, 87, 0.35f);
        c[ImGuiCol_NavCursor] = p.accent;
        c[ImGuiCol_TextLink] = p.link;
    }
    s.ScaleAllSizes(scale);
}

void spinner(float t, const ImVec4& color) {
    (void)t;
    // Arc qui tourne et respire : fluide à toute fréquence d'images.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = h * 0.34f;
    ImVec2 c(p.x + r + 2, p.y + h * 0.52f);
    double tm = anim::time();
    float start = (float)(tm * 5.5);
    float len = 1.3f + 1.0f * (float)std::sin(tm * 2.6);
    float th = std::max(1.5f, r * 0.30f);
    dl->AddCircle(c, r, col(color, 0.18f), 32, th);
    dl->PathArcTo(c, r, start, start + len, 32);
    dl->PathStroke(col(color), 0, th);
    anim::markBusy();
    ImGui::Dummy(ImVec2(r * 2 + 4, h));
}

void badge(const char* text, const ImVec4& color) {
    ImVec2 sz = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float padX = 6, padY = 1;
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + sz.x + padX * 2, p.y + sz.y + padY * 2), col(color, 0.22f),
                                              (sz.y + padY * 2) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + padX, p.y + padY));
    ImGui::TextColored(color, "%s", text);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y));
    ImGui::Dummy(ImVec2(sz.x + padX * 2, sz.y + padY * 2));
}

void helpMarker(const char* text) {
    // Petit « ? » cerclé et discret (au lieu de « (?) ») ; le texte d'aide au survol.
    ImGui::SameLine();
    const float lh = ImGui::GetTextLineHeight();
    const float d = lh * 0.72f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(d, lh));
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + d * 0.5f, p.y + lh * 0.5f);
    ImVec4 cl = hov ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : palette().dim;
    cl.w *= hov ? 1.0f : 0.7f;
    dl->AddCircle(c, d * 0.5f, col(cl), 20, 1.2f);
    float qs = d * 0.78f;
    ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(qs, FLT_MAX, 0, "?");
    dl->AddText(ImGui::GetFont(), qs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col(cl), "?");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

float buttonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

void sameLineIfFits(float w) {
    ImGui::SameLine();
    // NewLine juste après SameLine annule ce dernier : l'élément suivant repart au début de la ligne d'en dessous.
    if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
}

float fitItemWidth(float desired, const char* label) {
    float avail = ImGui::GetContentRegionAvail().x;
    if (label && *label) {
        float lw = ImGui::CalcTextSize(label, nullptr, true).x;
        if (lw > 0) avail -= lw + ImGui::GetStyle().ItemInnerSpacing.x;
    }
    return std::max(ImGui::GetFontSize() * 4.0f, std::min(desired, avail));
}

void sideNote(const char* text) {
    if (!text || !*text) return;
    ImGui::SameLine();
    float avail = ImGui::GetContentRegionAvail().x;
    bool below = avail < ImGui::GetFontSize() * 9.0f && avail < ImGui::CalcTextSize(text).x;
    float indent = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    if (below) {
        ImGui::NewLine();
        ImGui::Indent(indent);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    if (below) ImGui::Unindent(indent);
}

ImVec4 levelColor(float f) {
    const Palette& p = palette();
    auto lerp = [](const ImVec4& a, const ImVec4& b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    };
    f = std::clamp(f, 0.0f, 1.0f);
    return f < 0.5f ? lerp(p.ok, p.warn, f / 0.5f) : lerp(p.warn, p.err, std::min(1.0f, (f - 0.5f) / 0.35f));
}

// Piste fine + remplissage coloré, entre x0 et x1, centrée verticalement sur [y, y + h].
static void slimTrack(ImDrawList* dl, float x0, float x1, float y, float h, float shown) {
    float th = std::max(3.0f, std::min(h * 0.42f, ImGui::GetFontSize() * 0.34f));
    float ty = y + (h - th) * 0.5f;
    dl->AddRectFilled(ImVec2(x0, ty), ImVec2(x1, ty + th), col(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg)), th * 0.5f);
    float fw = (x1 - x0) * shown;
    if (shown > 0.001f) dl->AddRectFilled(ImVec2(x0, ty), ImVec2(x0 + std::max(th, fw), ty + th), col(levelColor(shown)), th * 0.5f);
}

void usageBar(float f, const char* overlay, float height, float width, const char* animId) {
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    float shown = animId ? anim::smooth(ImGui::GetID(animId), f, 5.0f, 0.0f) : f;
    float w = width < 0 ? ImGui::GetContentRegionAvail().x : width;
    float h = height > 0 ? height : ImGui::GetFrameHeight();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Valeur à droite, barre fine à gauche
    float textW = 0;
    if (overlay && *overlay) {
        ImVec2 ts = ImGui::CalcTextSize(overlay);
        textW = ts.x + ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(pos.x + w - ts.x, pos.y + (h - ts.y) * 0.5f), col(ImGui::GetStyleColorVec4(ImGuiCol_Text)), overlay);
    }
    float x1 = pos.x + std::max(ImGui::GetFontSize() * 1.5f, w - textW);
    slimTrack(dl, pos.x, x1, pos.y, h, shown);
}

void ringGauge(float f, float d, const char* bigText, const char* caption, ImFont* bigFont, const char* animId) {
    f = std::clamp(f, 0.0f, 1.0f);
    float shown = animId ? anim::smooth(ImGui::GetID(animId), f, 5.0f, 0.0f) : f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(d, d));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + d * 0.5f, p.y + d * 0.5f);
    float th = std::max(4.0f, d * 0.085f);
    float r = d * 0.5f - th * 0.5f - 1;
    dl->AddCircle(c, r, col(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg)), 64, th);
    if (shown > 0.002f) {
        const float a0 = -3.14159265f * 0.5f;
        dl->PathArcTo(c, r, a0, a0 + shown * 3.14159265f * 2.0f, 64);
        dl->PathStroke(col(levelColor(shown)), 0, th);
    }
    ImFont* font = bigFont ? bigFont : ImGui::GetFont();
    float bs = d * 0.26f;
    if (bigText && *bigText) {
        ImVec2 ts = font->CalcTextSizeA(bs, FLT_MAX, 0, bigText);
        float y = caption && *caption ? c.y - ts.y * 0.62f : c.y - ts.y * 0.5f;
        dl->AddText(font, bs, ImVec2(c.x - ts.x * 0.5f, y), col(ImGui::GetStyleColorVec4(ImGuiCol_Text)), bigText);
    }
    if (caption && *caption) {
        float cs = std::max(ImGui::GetFontSize() * 0.75f, d * 0.10f);
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(cs, FLT_MAX, 0, caption);
        dl->AddText(ImGui::GetFont(), cs, ImVec2(c.x - ts.x * 0.5f, c.y + bs * 0.42f), col(palette().dim), caption);
    }
}

void miniRing(float f, float size, const char* animId) {
    f = std::clamp(f, 0.0f, 1.0f);
    float shown = animId ? anim::smooth(ImGui::GetID(animId), f, 5.0f, 0.0f) : f;
    float lh = ImGui::GetTextLineHeight();
    float d = size > 0 ? size : lh * 0.85f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(d, lh));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + d * 0.5f, p.y + lh * 0.5f);
    float th = std::max(2.0f, d * 0.2f);
    float r = d * 0.5f - th * 0.5f;
    dl->AddCircle(c, r, col(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg)), 32, th);
    if (shown > 0.002f) {
        dl->PathArcTo(c, r, -3.14159265f * 0.5f, -3.14159265f * 0.5f + shown * 3.14159265f * 2.0f, 32);
        dl->PathStroke(col(levelColor(shown)), 0, th);
    }
}

bool chip(const char* label, const ImVec4* color) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
    if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
    bool r = ImGui::Button(label);
    ImGui::PopStyleColor(color ? 4 : 3);
    return r;
}

bool checkboxWrapped(const char* label, bool* v, const char* help) {
    ImGui::PushID(label);
    bool changed = ImGui::Checkbox("##cb", v);
    bool hov = ImGui::IsItemHovered();
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float wrap = ImGui::GetContentRegionAvail().x;
    ImVec2 sz = ImGui::CalcTextSize(label, nullptr, false, wrap);
    // Le libellé est cliquable, comme celui d'une case ordinaire
    if (ImGui::InvisibleButton("##lbl", ImVec2(std::max(1.0f, std::min(wrap, sz.x)), std::max(sz.y, ImGui::GetFrameHeight())))) {
        *v = !*v;
        changed = true;
    }
    hov = hov || ImGui::IsItemHovered();
    float dy = sz.y < ImGui::GetFrameHeight() ? (ImGui::GetFrameHeight() - sz.y) * 0.5f : 0;
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x, p.y + dy),
                                        col(ImGui::GetStyleColorVec4(ImGuiCol_Text)), label, nullptr, wrap);
    if (help && hov) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
        ImGui::TextUnformatted(help);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return changed;
}

bool segmented(const char* id, int* value, const char* const* labels, int count) {
    const Palette& p = palette();
    bool changed = false;
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, ImGui::GetStyle().ItemSpacing.y));
    for (int i = 0; i < count; ++i) {
        if (i > 0) sameLineIfFits(buttonWidth(labels[i]));
        bool sel = *value == i;
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p.accent);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        }
        ImGui::PushID(i);
        if (ImGui::Button(labels[i]) && !sel) {
            *value = i;
            changed = true;
        }
        ImGui::PopID();
        if (sel) ImGui::PopStyleColor(3);
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

std::string shortPath(const std::string& path, int keep) {
    std::vector<size_t> seps;
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] == '\\' || path[i] == '/') seps.push_back(i);
    if ((int)seps.size() <= keep) return path;
    return "…" + path.substr(seps[seps.size() - keep]);
}

void meterRow(const char* label, float f, const char* value, const char* note, ImFont* valueFont, const char* animId) {
    f = std::clamp(f, 0.0f, 1.0f);
    float shown = animId ? anim::smooth(ImGui::GetID(animId), f, 5.0f, 0.0f) : f;
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::TextUnformatted(label);
    if (value && *value) {
        ImFont* vf = valueFont ? valueFont : ImGui::GetFont();
        ImVec2 ts = vf->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0, value);
        dl->AddText(vf, ImGui::GetFontSize(), ImVec2(p.x + w - ts.x, p.y), col(ImGui::GetStyleColorVec4(ImGuiCol_Text)), value);
    }
    ImVec2 q = ImGui::GetCursorScreenPos();
    float bh = lh * 0.55f;
    ImGui::Dummy(ImVec2(w, bh));
    slimTrack(dl, q.x, q.x + w, q.y, bh, shown);
    if (note && *note) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette().dim);
        ImGui::TextWrapped("%s", note);
        ImGui::PopStyleColor();
    }
}

}  // namespace ui
