#include "ui/Theme.h"

#include <algorithm>
#include <cmath>

#include "ui/Anim.h"

namespace ui {

static Palette g_palette;

const Palette& palette() { return g_palette; }

static ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }

void applyTheme(ThemeId id, float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    if (id == ThemeId::Light) ImGui::StyleColorsLight(&s);
    else ImGui::StyleColorsDark(&s);

    s.WindowRounding = 6;
    s.ChildRounding = 6;
    s.FrameRounding = 5;
    s.PopupRounding = 6;
    s.GrabRounding = 4;
    s.TabRounding = 5;
    s.ScrollbarRounding = 8;
    s.ScrollbarSize = 13;
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(6, 5);
    s.IndentSpacing = 16;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBarBorderSize = 1;
    s.SeparatorTextBorderSize = 2;
    s.DockingSeparatorSize = 3;

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
        c[ImGuiCol_HeaderHovered] = rgb(201, 100, 66, 0.25f);
        c[ImGuiCol_HeaderActive] = rgb(201, 100, 66, 0.35f);
        c[ImGuiCol_Button] = rgb(230, 225, 218);
        c[ImGuiCol_ButtonHovered] = rgb(220, 205, 192);
        c[ImGuiCol_ButtonActive] = rgb(210, 190, 175);
        c[ImGuiCol_Tab] = rgb(232, 228, 222);
        c[ImGuiCol_TabHovered] = rgb(201, 100, 66, 0.3f);
        c[ImGuiCol_TabSelected] = rgb(250, 249, 246);
        c[ImGuiCol_TabSelectedOverline] = p.accent;
        c[ImGuiCol_TabDimmedSelected] = rgb(244, 242, 238);
        c[ImGuiCol_CheckMark] = p.accent;
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
        c[ImGuiCol_CheckMark] = p.accent;
        c[ImGuiCol_SliderGrab] = p.accent;
        c[ImGuiCol_SliderGrabActive] = rgb(235, 140, 105);
        c[ImGuiCol_Button] = dusk ? rgb(52, 46, 66) : rgb(48, 46, 43);
        c[ImGuiCol_ButtonHovered] = rgb(217, 119, 87, 0.55f);
        c[ImGuiCol_ButtonActive] = rgb(217, 119, 87, 0.8f);
        c[ImGuiCol_Header] = p.accentSoft;
        c[ImGuiCol_HeaderHovered] = rgb(217, 119, 87, 0.28f);
        c[ImGuiCol_HeaderActive] = rgb(217, 119, 87, 0.40f);
        c[ImGuiCol_Separator] = c[ImGuiCol_Border];
        c[ImGuiCol_SeparatorHovered] = p.accent;
        c[ImGuiCol_SeparatorActive] = p.accent;
        c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ResizeGripHovered] = p.accentSoft;
        c[ImGuiCol_ResizeGripActive] = p.accent;
        c[ImGuiCol_Tab] = bg;
        c[ImGuiCol_TabHovered] = rgb(217, 119, 87, 0.35f);
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
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void usageBar(float f, const char* overlay, float height, float width, const char* animId) {
    const Palette& p = palette();
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    float shown = animId ? anim::smooth(ImGui::GetID(animId), f, 5.0f, 0.0f) : f;
    float w = width < 0 ? ImGui::GetContentRegionAvail().x : width;
    float h = height > 0 ? height : ImGui::GetFrameHeight();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rounding = h * 0.5f;
    // Couleur interpolée vert → orange → rouge
    auto lerp = [](const ImVec4& a, const ImVec4& b, float t) {
        return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    };
    ImVec4 c = shown < 0.5f ? lerp(p.ok, p.warn, shown / 0.5f) : lerp(p.warn, p.err, std::min(1.0f, (shown - 0.5f) / 0.35f));
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg)), rounding);
    float fw = std::max(shown > 0.001f ? h : 0.0f, w * shown);
    if (fw > 0) {
        dl->AddRectFilled(pos, ImVec2(pos.x + fw, pos.y + h), col(c, 0.92f), rounding);
        // reflet discret sur la moitié haute
        dl->AddRectFilled(ImVec2(pos.x + 2, pos.y + 1), ImVec2(pos.x + fw - 2, pos.y + h * 0.45f), col(ImVec4(1, 1, 1, 1), 0.10f),
                          rounding);
    }
    if (overlay && *overlay) {
        ImVec2 ts = ImGui::CalcTextSize(overlay);
        ImVec2 tp(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f);
        dl->AddText(ImVec2(tp.x + 1, tp.y + 1), col(ImVec4(0, 0, 0, 1), 0.45f), overlay);
        dl->AddText(tp, col(ImGui::GetStyleColorVec4(ImGuiCol_Text)), overlay);
    }
}

}  // namespace ui
