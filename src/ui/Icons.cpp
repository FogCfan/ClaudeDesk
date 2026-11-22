#include "ui/Icons.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

#include "ui/Anim.h"
#include "ui/Theme.h"
#include "util/Strings.h"

namespace ui {

namespace {

bool lightTheme() {
    ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    return bg.x * 0.3f + bg.y * 0.59f + bg.z * 0.11f > 0.5f;
}

ImVec4 pick(ImVec4 dark, ImVec4 light) { return lightTheme() ? light : dark; }

ImU32 darker(ImU32 c, float k) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x * k, v.y * k, v.z * k, v.w));
}

// Marques intérieures (yeux du robot, lignes d'un document…) : couleur du fond, pour un effet « découpé ».
ImU32 cutColor() { return ImGui::GetColorU32(ImGuiCol_WindowBg); }

}  // namespace

ImVec4 iconColor(Icon icon) {
    switch (icon) {
        case Icon::Project: return palette().accent;
        case Icon::Folder:
        case Icon::FolderOpen: return pick(ImVec4(0.86f, 0.71f, 0.42f, 1), ImVec4(0.78f, 0.58f, 0.18f, 1));
        case Icon::Agent: return pick(ImVec4(0.38f, 0.80f, 0.66f, 1), ImVec4(0.10f, 0.58f, 0.46f, 1));
        case Icon::Orchestrator: return palette().accent;
        case Icon::Chat: return pick(ImVec4(0.52f, 0.66f, 0.96f, 1), ImVec4(0.22f, 0.42f, 0.86f, 1));
        case Icon::WorkDir: return pick(ImVec4(0.62f, 0.62f, 0.68f, 1), ImVec4(0.42f, 0.42f, 0.48f, 1));
        case Icon::File: return pick(ImVec4(0.70f, 0.70f, 0.72f, 1), ImVec4(0.45f, 0.45f, 0.48f, 1));
        case Icon::FileCode: return pick(ImVec4(0.40f, 0.64f, 0.96f, 1), ImVec4(0.16f, 0.40f, 0.80f, 1));
        case Icon::FileText: return pick(ImVec4(0.80f, 0.80f, 0.84f, 1), ImVec4(0.40f, 0.40f, 0.46f, 1));
        case Icon::FileData: return pick(ImVec4(0.92f, 0.78f, 0.30f, 1), ImVec4(0.72f, 0.54f, 0.04f, 1));
        case Icon::FileImage: return pick(ImVec4(0.72f, 0.52f, 0.92f, 1), ImVec4(0.52f, 0.28f, 0.76f, 1));
        case Icon::FilePdf: return pick(ImVec4(0.92f, 0.42f, 0.40f, 1), ImVec4(0.78f, 0.20f, 0.18f, 1));
        case Icon::Trash: return palette().dim;
        case Icon::History: return palette().dim;
        case Icon::Send:
        case Icon::Stop: return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        case Icon::KindAuto: return palette().accent;
        case Icon::KindSoftware: return pick(ImVec4(0.40f, 0.64f, 0.96f, 1), ImVec4(0.16f, 0.40f, 0.80f, 1));
        case Icon::KindConcept: return pick(ImVec4(0.96f, 0.80f, 0.30f, 1), ImVec4(0.78f, 0.56f, 0.02f, 1));
        case Icon::KindData: return pick(ImVec4(0.38f, 0.80f, 0.66f, 1), ImVec4(0.10f, 0.58f, 0.46f, 1));
        case Icon::KindDocument: return pick(ImVec4(0.92f, 0.52f, 0.46f, 1), ImVec4(0.74f, 0.26f, 0.20f, 1));
        case Icon::KindHybrid: return pick(ImVec4(0.72f, 0.56f, 0.94f, 1), ImVec4(0.50f, 0.30f, 0.78f, 1));
        case Icon::KindQuestion: return pick(ImVec4(0.52f, 0.66f, 0.96f, 1), ImVec4(0.22f, 0.42f, 0.86f, 1));
    }
    return palette().dim;
}

Icon projectKindIcon(claude::ProjectKind k) {
    switch (k) {
        case claude::ProjectKind::Software: return Icon::KindSoftware;
        case claude::ProjectKind::Concept: return Icon::KindConcept;
        case claude::ProjectKind::Data: return Icon::KindData;
        case claude::ProjectKind::Document: return Icon::KindDocument;
        case claude::ProjectKind::Hybrid: return Icon::KindHybrid;
        case claude::ProjectKind::Question: return Icon::KindQuestion;
        default: return Icon::KindAuto;
    }
}

Icon fileIcon(const std::string& fileName) {
    std::string n = util::toLower(fileName);
    size_t dot = n.rfind('.');
    std::string ext = dot == std::string::npos ? std::string() : n.substr(dot + 1);
    static const char* const code[] = {"c",   "cc",  "cpp", "cxx", "h",    "hpp",  "hh",   "inl", "py",   "js",
                                       "mjs", "ts",  "tsx", "jsx", "rs",   "go",   "java", "kt",  "cs",   "swift",
                                       "m",   "mm",  "lua", "rb",  "php",  "sh",   "bat",  "cmd", "ps1",  "glsl",
                                       "hlsl", "vert", "frag", "comp", "html", "css", "scss", "vue", "svelte", "sql",
                                       "cmake", "zig", "dart", "r", "jl"};
    static const char* const data[] = {"json", "yaml", "yml", "toml", "ini", "cfg", "conf", "csv", "tsv", "xml",
                                       "lock", "env", "props", "plist", "gltf"};
    static const char* const image[] = {"png", "jpg", "jpeg", "gif", "webp", "bmp", "svg", "ico", "tga", "psd"};
    static const char* const text[] = {"md", "txt", "rst", "log", "adoc", "tex"};
    static const char* const doc[] = {"pdf", "doc", "docx", "odt", "ppt", "pptx", "xls", "xlsx"};
    for (auto e : code)
        if (ext == e) return Icon::FileCode;
    if (n == "cmakelists.txt" || n == "makefile" || n == "dockerfile") return Icon::FileCode;
    for (auto e : data)
        if (ext == e) return Icon::FileData;
    for (auto e : image)
        if (ext == e) return Icon::FileImage;
    for (auto e : text)
        if (ext == e) return Icon::FileText;
    for (auto e : doc)
        if (ext == e) return Icon::FilePdf;
    if (n == "readme" || n == "license" || n == ".gitignore") return Icon::FileText;
    return Icon::File;
}

void drawIcon(ImDrawList* dl, Icon icon, ImVec2 p, float s, ImU32 col, float activity) {
    if (!col) col = ImGui::ColorConvertFloat4ToU32(iconColor(icon));
    const ImU32 cut = cutColor();
    auto P = [&](float x, float y) { return ImVec2(p.x + x * s, p.y + y * s); };
    const float r = s * 0.10f;  // arrondi
    const float lw = std::max(1.0f, s * 0.085f);

    auto folder = [&](bool open, ImU32 c) {
        dl->AddRectFilled(P(0.06f, 0.18f), P(0.46f, 0.36f), c, r);           // onglet
        if (!open) {
            dl->AddRectFilled(P(0.06f, 0.28f), P(0.94f, 0.84f), c, r);
        } else {
            dl->AddRectFilled(P(0.06f, 0.28f), P(0.88f, 0.84f), darker(c, 0.72f), r);  // fond
            dl->AddQuadFilled(P(0.20f, 0.44f), P(1.00f, 0.44f), P(0.88f, 0.84f), P(0.06f, 0.84f), c);  // rabat
        }
    };
    auto document = [&](ImU32 c) {
        // Feuille à coin replié
        ImVec2 pts[5] = {P(0.18f, 0.06f), P(0.60f, 0.06f), P(0.84f, 0.30f), P(0.84f, 0.94f), P(0.18f, 0.94f)};
        dl->AddConvexPolyFilled(pts, 5, c);
        dl->AddTriangleFilled(P(0.60f, 0.06f), P(0.84f, 0.30f), P(0.60f, 0.30f), darker(c, 0.65f));
    };

    switch (icon) {
        case Icon::Project: {
            folder(false, col);
            // Losange découpé : « ceci est un projet », pas un simple dossier
            ImVec2 c = P(0.50f, 0.57f);
            float d = s * 0.15f;
            dl->AddQuadFilled(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y), cut);
            break;
        }
        case Icon::Folder: folder(false, col); break;
        case Icon::FolderOpen: folder(true, col); break;
        case Icon::WorkDir: {
            folder(false, col);
            // Chevrons « </> » : le code du projet
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.40f, 0.44f), P(0.28f, 0.56f), P(0.40f, 0.68f)}.data(), 3, cut, 0, lw);
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.60f, 0.44f), P(0.72f, 0.56f), P(0.60f, 0.68f)}.data(), 3, cut, 0, lw);
            break;
        }
        case Icon::Agent: {
            // Tête de robot : antenne, visage, yeux
            dl->AddLine(P(0.50f, 0.30f), P(0.50f, 0.13f), col, lw);
            dl->AddCircleFilled(P(0.50f, 0.11f), s * 0.075f, col);
            dl->AddRectFilled(P(0.14f, 0.30f), P(0.86f, 0.88f), col, s * 0.16f);
            dl->AddRectFilled(P(0.05f, 0.50f), P(0.14f, 0.70f), col, s * 0.03f);  // oreilles
            dl->AddRectFilled(P(0.86f, 0.50f), P(0.95f, 0.70f), col, s * 0.03f);
            dl->AddCircleFilled(P(0.36f, 0.56f), s * 0.085f, cut);
            dl->AddCircleFilled(P(0.64f, 0.56f), s * 0.085f, cut);
            dl->AddLine(P(0.38f, 0.76f), P(0.62f, 0.76f), cut, lw);
            break;
        }
        case Icon::Orchestrator: {
            // Chef d'orchestre : un nœud central relié à trois agents
            ImVec2 c = P(0.50f, 0.52f);
            ImVec2 a[3] = {P(0.15f, 0.18f), P(0.85f, 0.18f), P(0.50f, 0.92f)};
            for (auto& q : a) dl->AddLine(c, q, col, lw);
            for (auto& q : a) dl->AddCircleFilled(q, s * 0.12f, col);
            dl->AddCircleFilled(c, s * 0.22f, col);
            dl->AddCircleFilled(c, s * 0.09f, cut);
            break;
        }
        case Icon::Chat: {
            // Bulle de dialogue avec sa queue, trois points découpés
            dl->AddRectFilled(P(0.04f, 0.12f), P(0.96f, 0.70f), col, s * 0.18f);
            dl->AddTriangleFilled(P(0.22f, 0.66f), P(0.46f, 0.66f), P(0.18f, 0.92f), col);
            for (int i = 0; i < 3; ++i) dl->AddCircleFilled(P(0.30f + 0.20f * i, 0.41f), s * 0.065f, cut);
            break;
        }
        case Icon::KindAuto: {
            // Étincelles : une grande, une petite
            auto star = [&](float cx, float cy, float R, float r2) {
                ImVec2 c = P(cx, cy);
                float a = R * s, b = r2 * s;
                ImVec2 pts[8] = {ImVec2(c.x, c.y - a), ImVec2(c.x + b, c.y - b), ImVec2(c.x + a, c.y), ImVec2(c.x + b, c.y + b),
                                 ImVec2(c.x, c.y + a), ImVec2(c.x - b, c.y + b), ImVec2(c.x - a, c.y), ImVec2(c.x - b, c.y - b)};
                dl->AddConcavePolyFilled(pts, 8, col);
            };
            star(0.42f, 0.56f, 0.40f, 0.11f);
            star(0.80f, 0.20f, 0.18f, 0.05f);
            break;
        }
        case Icon::KindSoftware: {
            dl->AddRectFilled(P(0.04f, 0.10f), P(0.96f, 0.90f), col, s * 0.12f);
            dl->AddRectFilled(P(0.04f, 0.10f), P(0.96f, 0.26f), darker(col, 0.7f), s * 0.12f, ImDrawFlags_RoundCornersTop);
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.38f, 0.40f), P(0.24f, 0.57f), P(0.38f, 0.74f)}.data(), 3, cut, 0, lw);
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.62f, 0.40f), P(0.76f, 0.57f), P(0.62f, 0.74f)}.data(), 3, cut, 0, lw);
            dl->AddLine(P(0.55f, 0.38f), P(0.45f, 0.76f), cut, lw);
            break;
        }
        case Icon::KindConcept:
        case Icon::KindHybrid: {
            bool hybrid = icon == Icon::KindHybrid;
            float k = hybrid ? 0.72f : 1.0f;  // ampoule réduite à gauche pour l'hybride
            auto Q = [&](float x, float y) { return P(x * k, y * k + (hybrid ? 0.0f : 0.0f)); };
            dl->AddCircleFilled(Q(0.50f, 0.40f), s * 0.32f * k, col);
            dl->AddRectFilled(Q(0.36f, 0.62f), Q(0.64f, 0.80f), col, s * 0.04f);
            dl->AddRectFilled(Q(0.40f, 0.82f), Q(0.60f, 0.94f), darker(col, 0.7f), s * 0.04f);
            dl->AddLine(Q(0.42f, 0.42f), Q(0.50f, 0.56f), cut, lw * 0.8f);
            dl->AddLine(Q(0.58f, 0.42f), Q(0.50f, 0.56f), cut, lw * 0.8f);
            if (hybrid) {
                // Petite fenêtre de code en bas à droite
                ImU32 c2 = ImGui::ColorConvertFloat4ToU32(iconColor(Icon::KindSoftware));
                dl->AddRectFilled(P(0.46f, 0.46f), P(1.00f, 0.96f), cut, s * 0.10f);
                dl->AddRectFilled(P(0.50f, 0.50f), P(0.96f, 0.92f), c2, s * 0.08f);
                dl->AddPolyline(std::array<ImVec2, 3>{P(0.66f, 0.60f), P(0.58f, 0.71f), P(0.66f, 0.82f)}.data(), 3, cut, 0, lw * 0.8f);
                dl->AddPolyline(std::array<ImVec2, 3>{P(0.80f, 0.60f), P(0.88f, 0.71f), P(0.80f, 0.82f)}.data(), 3, cut, 0, lw * 0.8f);
            }
            break;
        }
        case Icon::KindData: {
            dl->AddRectFilled(P(0.10f, 0.56f), P(0.30f, 0.90f), col, s * 0.04f);
            dl->AddRectFilled(P(0.40f, 0.30f), P(0.60f, 0.90f), col, s * 0.04f);
            dl->AddRectFilled(P(0.70f, 0.10f), P(0.90f, 0.90f), col, s * 0.04f);
            dl->AddLine(P(0.04f, 0.94f), P(0.96f, 0.94f), col, lw);
            break;
        }
        case Icon::KindDocument: {
            ImVec2 pts[5] = {P(0.16f, 0.04f), P(0.62f, 0.04f), P(0.86f, 0.28f), P(0.86f, 0.96f), P(0.16f, 0.96f)};
            dl->AddConvexPolyFilled(pts, 5, col);
            dl->AddTriangleFilled(P(0.62f, 0.04f), P(0.86f, 0.28f), P(0.62f, 0.28f), darker(col, 0.65f));
            for (int i = 0; i < 2; ++i) dl->AddLine(P(0.28f, 0.42f + 0.14f * i), P(0.74f, 0.42f + 0.14f * i), cut, lw);
            // Coche : un rapport vérifié
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.30f, 0.76f), P(0.40f, 0.85f), P(0.58f, 0.68f)}.data(), 3, cut, 0, lw * 1.2f);
            break;
        }
        case Icon::KindQuestion: {
            dl->AddRectFilled(P(0.04f, 0.08f), P(0.96f, 0.72f), col, s * 0.18f);
            dl->AddTriangleFilled(P(0.22f, 0.68f), P(0.46f, 0.68f), P(0.16f, 0.94f), col);
            ImFont* f = ImGui::GetFont();
            float fsz = s * 0.62f;
            ImVec2 ts = f->CalcTextSizeA(fsz, FLT_MAX, 0, "?");
            dl->AddText(f, fsz, ImVec2(p.x + (s - ts.x) * 0.5f, p.y + s * 0.40f - ts.y * 0.5f), cut, "?");
            break;
        }
        case Icon::History: {
            // Horloge entourée d'une flèche de retour : « avant »
            ImVec2 c = P(0.54f, 0.54f);
            float R = s * 0.38f;
            dl->PathArcTo(c, R, -2.4f, 2.9f, 20);
            dl->PathStroke(col, 0, lw);
            ImVec2 a(c.x + R * std::cos(-2.4f), c.y + R * std::sin(-2.4f));
            dl->AddTriangleFilled(ImVec2(a.x - s * 0.16f, a.y - s * 0.04f), ImVec2(a.x + s * 0.10f, a.y - s * 0.06f),
                                  ImVec2(a.x - s * 0.04f, a.y + s * 0.18f), col);
            dl->AddLine(c, P(0.54f, 0.32f), col, lw);
            dl->AddLine(c, P(0.70f, 0.64f), col, lw);
            break;
        }
        case Icon::Send: {
            // Avion en papier : pointe à droite, encoche à l'arrière
            ImVec2 pts[4] = {P(0.08f, 0.10f), P(0.96f, 0.50f), P(0.08f, 0.90f), P(0.24f, 0.50f)};
            dl->AddConcavePolyFilled(pts, 4, col);
            break;
        }
        case Icon::Stop: {
            dl->AddRectFilled(P(0.18f, 0.18f), P(0.82f, 0.82f), col, s * 0.12f);
            break;
        }
        case Icon::Trash: {
            dl->AddRectFilled(P(0.14f, 0.18f), P(0.86f, 0.28f), col, s * 0.03f);
            dl->AddRectFilled(P(0.38f, 0.08f), P(0.62f, 0.18f), col, s * 0.03f);
            dl->AddRectFilled(P(0.22f, 0.30f), P(0.78f, 0.94f), col, s * 0.06f);
            break;
        }
        default: {  // fichiers
            document(col);
            if (icon == Icon::FileCode) {
                dl->AddPolyline(std::array<ImVec2, 3>{P(0.43f, 0.46f), P(0.31f, 0.60f), P(0.43f, 0.74f)}.data(), 3, cut, 0, lw);
                dl->AddPolyline(std::array<ImVec2, 3>{P(0.59f, 0.46f), P(0.71f, 0.60f), P(0.59f, 0.74f)}.data(), 3, cut, 0, lw);
            } else if (icon == Icon::FileText || icon == Icon::FilePdf) {
                for (int i = 0; i < 3; ++i)
                    dl->AddLine(P(0.30f, 0.46f + 0.15f * i), P(i == 2 ? 0.56f : 0.72f, 0.46f + 0.15f * i), cut, lw);
            } else if (icon == Icon::FileData) {
                dl->AddText(nullptr, s * 0.62f, P(0.27f, 0.36f), cut, "{}");
            } else if (icon == Icon::FileImage) {
                dl->AddTriangleFilled(P(0.26f, 0.84f), P(0.46f, 0.52f), P(0.64f, 0.84f), cut);
                dl->AddTriangleFilled(P(0.50f, 0.84f), P(0.64f, 0.64f), P(0.78f, 0.84f), cut);
                dl->AddCircleFilled(P(0.66f, 0.46f), s * 0.07f, cut);
            }
            break;
        }
    }
    if (activity > 0) {
        // Point « au travail » qui pulse, en bas à droite
        float t = (float)ImGui::GetTime();
        float a = 0.55f + 0.45f * std::sin(t * 5.0f);
        ImVec2 c = P(0.88f, 0.86f);
        dl->AddCircleFilled(c, s * 0.20f, cut);
        dl->AddCircleFilled(c, s * 0.14f, ImGui::ColorConvertFloat4ToU32(ImVec4(palette().warn.x, palette().warn.y, palette().warn.z, a)));
        anim::markBusy();
    }
}

void icon(Icon ic, ImU32 col, float activity) {
    float s = ImGui::GetFontSize();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float dy = (ImGui::GetTextLineHeight() - s) * 0.5f;
    ImGui::Dummy(ImVec2(s, ImGui::GetTextLineHeight()));
    drawIcon(ImGui::GetWindowDrawList(), ic, ImVec2(p.x, p.y + dy), s, col, activity);
}

bool iconTreeNode(const char* strId, ImGuiTreeNodeFlags flags, Icon ic, const char* label, ImU32 col, float activity) {
    const float s = ImGui::GetFontSize();
    // Espaces réservés à l'icône (calculés pour la police courante)
    static float cachedSize = -1;
    static std::string pad;
    if (cachedSize != s) {
        float sp = std::max(1.0f, ImGui::CalcTextSize(" ").x);
        pad.assign((size_t)std::ceil(s * 1.3f / sp), ' ');
        cachedSize = s;
    }
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool open = ImGui::TreeNodeEx(strId, flags, "%s%s", pad.c_str(), label);
    const ImGuiStyle& st = ImGui::GetStyle();
    bool framed = (flags & ImGuiTreeNodeFlags_Framed) != 0;
    float x = p0.x + s + (framed ? st.FramePadding.x * 3 : st.FramePadding.x * 2);
    ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    float y = mn.y + (mx.y - mn.y - s) * 0.5f;
    drawIcon(ImGui::GetWindowDrawList(), ic, ImVec2(x, y), s, col, activity);
    return open;
}

}  // namespace ui
