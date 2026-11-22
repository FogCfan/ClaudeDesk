// Panneau « Configuration » : modèle, effort, mode, outils, prompt système,
// options avancées de la conversation active (ou réglages par défaut), et
// préférences de l'application.
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/App.h"
#include "claude/CommandLine.h"
#include "ui/Theme.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace app {

namespace {

bool contains(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }
void removeAll(std::vector<std::string>& v, const std::string& x) { v.erase(std::remove(v.begin(), v.end(), x), v.end()); }

void stringList(const char* id, std::vector<std::string>& list, std::string& buffer, const char* hint) {
    ImGui::PushID(id);
    for (size_t i = 0; i < list.size(); ++i) {
        ImGui::PushID((int)i);
        if (ImGui::SmallButton("x")) {
            list.erase(list.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(list[i].c_str());
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 5);
    bool enter = ImGui::InputTextWithHint("##new", hint, &buffer, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Ajouter") || enter) && !util::trim(buffer).empty()) {
        list.push_back(util::trim(buffer));
        buffer.clear();
    }
    ImGui::PopID();
}

}  // namespace

// ---------------------------------------------------------------------------
// Modèles : cartes par famille (capacité, consommation, accès selon l'abonnement), effort en une rangée
// ---------------------------------------------------------------------------

namespace {

ImVec4 accessColor(claude::ModelAccess a) {
    const auto& pal = ui::palette();
    switch (a) {
        case claude::ModelAccess::Included: return pal.ok;
        case claude::ModelAccess::NeedsCredits: return pal.warn;
        case claude::ModelAccess::Unavailable: return pal.err;
        case claude::ModelAccess::Unknown: break;
    }
    return pal.dim;
}

const char* accessShort(claude::ModelAccess a) {
    switch (a) {
        case claude::ModelAccess::Included: return "inclus";
        case claude::ModelAccess::NeedsCredits: return "crédits requis";
        case claude::ModelAccess::Unavailable: return "indisponible";
        case claude::ModelAccess::Unknown: break;
    }
    return "non vérifié";
}

// Rangée de pastilles « ●●●○ ».
void dots(ImDrawList* dl, ImVec2 p, float r, int n, int of, ImU32 on, ImU32 off) {
    for (int i = 0; i < of; ++i) dl->AddCircleFilled(ImVec2(p.x + r + i * r * 2.7f, p.y + r), r, i < n ? on : off);
}

// Carte de choix, style épuré : pastille radio, titre, badge à droite, une ligne d'explication (repliée au besoin),
// jusqu'à deux jauges en pastilles (« capacité ●●○○ »), liseré de couleur facultatif à gauche (niveau de risque…).
struct SelectCard {
    const char* id = "";
    std::string title, text;
    const char* badge = nullptr;
    ImVec4 badgeColor{};
    struct Meter {
        const char* label = nullptr;
        int value = 0;
        ImVec4 color{};
    } meters[2];
    ImVec4 stripe{};  // w = 0 : sans liseré
    bool selected = false, muted = false;
};

bool drawSelectCard(const SelectCard& c, float w, ImFont* bold, ImFont* uiFont) {
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    const float lh = ImGui::GetTextLineHeight();
    const float pad = fs * 0.55f;
    const bool hasMeters = c.meters[0].label != nullptr;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float tx0 = pad + fs * 1.35f + (c.stripe.w > 0 ? 3 : 0);
    const float wrapW = std::max(fs * 4, w - tx0 - pad);
    const float textH = c.text.empty() ? 0.0f : uiFont->CalcTextSizeA(fs * 0.92f, FLT_MAX, wrapW, c.text.c_str()).y;
    const float h = pad * 2 + lh + (textH > 0 ? textH + lh * 0.05f : 0.0f) + (hasMeters ? lh * 1.05f : 0.0f);
    ImGui::PushID(c.id);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##card", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    ImVec2 p1(p0.x + w, p0.y + h);
    const float rnd = ImGui::GetStyle().FrameRounding + 1;
    ImVec4 bg = c.selected ? ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.12f) : pal.cardBg;
    if (hov && !c.selected) bg = ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered);
    dl->AddRectFilled(p0, p1, ui::col(bg), rnd);
    dl->AddRect(p0, p1, ui::col(c.selected ? pal.accent : ImGui::GetStyleColorVec4(ImGuiCol_Border)), rnd, 0, c.selected ? 1.5f : 1.0f);
    if (c.stripe.w > 0) dl->AddRectFilled(p0, ImVec2(p0.x + 3, p1.y), ui::col(c.stripe), rnd, ImDrawFlags_RoundCornersLeft);
    float x0 = p0.x + (c.stripe.w > 0 ? 3 : 0);
    ImVec2 rc(x0 + pad + fs * 0.42f, p0.y + pad + lh * 0.5f);
    dl->AddCircle(rc, fs * 0.38f, ui::col(c.selected ? pal.accent : pal.dim), 0, 1.4f);
    if (c.selected) dl->AddCircleFilled(rc, fs * 0.21f, ui::col(pal.accent));
    float tx = p0.x + tx0;
    ImVec4 titleCol = c.muted && !c.selected ? pal.dim : c.selected ? pal.accent : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    dl->AddText(bold, fs, ImVec2(tx, p0.y + pad), ui::col(titleCol), c.title.c_str());
    if (c.badge) {
        float bs = fs * 0.85f;
        float bw = uiFont->CalcTextSizeA(bs, FLT_MAX, 0, c.badge).x + fs * 0.7f;
        ImVec2 b0(p1.x - pad - bw, p0.y + pad), b1(p1.x - pad, p0.y + pad + lh);
        ImVec4 bc = c.badgeColor;
        dl->AddRectFilled(b0, b1, ui::col(ImVec4(bc.x, bc.y, bc.z, 0.16f)), lh * 0.5f);
        dl->AddText(uiFont, bs, ImVec2(b0.x + fs * 0.35f, b0.y + (lh - bs) * 0.5f), ui::col(bc), c.badge);
    }
    float y = p0.y + pad + lh * 1.05f;
    if (textH > 0) {
        dl->AddText(uiFont, fs * 0.92f, ImVec2(tx, y), ui::col(pal.dim), c.text.c_str(), nullptr, wrapW);
        y += textH + lh * 0.05f;
    }
    if (hasMeters) {
        float r = fs * 0.16f, ms = fs * 0.85f, x = tx;
        ImU32 off = ui::col(ImGui::GetStyleColorVec4(ImGuiCol_Border));
        for (auto& m : c.meters) {
            if (!m.label) continue;
            dl->AddText(uiFont, ms, ImVec2(x, y), ui::col(pal.dim), m.label);
            x += uiFont->CalcTextSizeA(ms, FLT_MAX, 0, m.label).x + fs * 0.4f;
            dots(dl, ImVec2(x, y + ms * 0.5f - r), r, m.value, 4, ui::col(m.color), off);
            x += r * 2.7f * 4 + fs * 0.9f;
        }
    }
    ImGui::PopID();
    return clicked;
}

std::string planDisplay(const std::string& plan) {
    if (plan.empty()) return "abonnement inconnu";
    std::string p = plan;
    p[0] = (char)std::toupper((unsigned char)p[0]);
    return "Claude " + p;
}

}  // namespace

bool App::drawModelPickerItems(claude::ClaudeSession* s, const std::string& current, std::string& chosen) {
    const auto& pal = ui::palette();
    const std::string plan = accountPlan();
    bool picked = false;
    if (ImGui::Selectable("Par défaut (réglage de Claude Code)", current.empty())) chosen.clear(), picked = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Le modèle choisi dans Claude Code (/model).");
    ImGui::Separator();
    for (auto& f : claude::modelFamilies()) {
        claude::ModelAccess a = settings_.modelAccess.access(f.alias, plan);
        bool ok = a == claude::ModelAccess::Included || a == claude::ModelAccess::Unknown;
        std::string label = std::string(f.name) + " " + f.version;
        const claude::ModelFamily* cur = claude::findModelFamily(current);
        bool sel = cur == &f && (current == f.alias || current == f.fullId);
        if (!ok) ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
        if (ImGui::Selectable((label + "##" + f.alias).c_str(), sel)) chosen = f.alias, picked = true;
        bool hov = ImGui::IsItemHovered();
        if (!ok) ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextColored(ok ? pal.dim : accessColor(a), "%s", ok ? f.tagline : accessShort(a));
        if (hov)
            ImGui::SetTooltip("%s — %s\n%s (%s)", label.c_str(), f.uses, claude::modelAccessLabel(a), planDisplay(plan).c_str());
    }
    // Versions figées (anciennes, ou épinglées) : repliées, pour les besoins précis.
    ImGui::Separator();
    if (ImGui::BeginMenu("Version précise…")) {
        std::vector<std::pair<std::string, std::string>> fixed;  // identifiant, libellé
        if (s && !s->models.empty()) {
            for (auto& m : s->models) {
                bool alias = m.value == "default";
                for (auto& f : claude::modelFamilies()) alias = alias || m.value == f.alias;
                if (!alias) fixed.push_back({m.value, m.displayName});
            }
        } else {
            for (auto& f : claude::modelFamilies()) fixed.push_back({f.fullId, std::string(f.name) + " " + f.version});
        }
        for (auto& [id, name] : fixed) {
            bool ok = modelUsable(id);
            if (!ok) ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
            if (ImGui::MenuItem(name.c_str(), id.c_str(), current == id)) chosen = id, picked = true;
            if (!ok) ImGui::PopStyleColor();
        }
        ImGui::EndMenu();
    }
    return picked;
}

void App::drawModelCards(std::string& model, claude::ClaudeSession* s) {
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    const std::string plan = accountPlan();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
    ImGui::TextWrapped("Le modèle fait le travail : plus il est capable, plus il consomme de quota. Votre compte : %s.",
                       planDisplay(plan).c_str());
    ImGui::PopStyleColor();

    // Carte de modèle : titre, badge d'accès, explication, capacité et consommation (composant SelectCard).
    auto card = [&](const char* id, const std::string& title, const char* tagline, int power, int cost,
                    claude::ModelAccess access, bool showAccess, bool selected, float w) -> bool {
        SelectCard c;
        c.id = id;
        c.title = title;
        c.text = tagline;
        c.selected = selected;
        c.muted = !(access == claude::ModelAccess::Included || access == claude::ModelAccess::Unknown);
        if (showAccess) c.badge = accessShort(access), c.badgeColor = accessColor(access);
        if (power > 0) {
            c.meters[0] = {"capacité", power, pal.link};
            c.meters[1] = {"quota", cost, pal.warn};
        }
        return drawSelectCard(c, w, fontBold, fontUi);
    };
    (void)dl;

    float avail = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const int cols = avail >= fs * 30 ? 2 : 1;
    const float cw = cols == 2 ? (avail - gap) * 0.5f : avail;

    // Par défaut : le modèle choisi dans Claude Code
    {
        std::string resolved;
        if (s)
            for (auto& m : s->models)
                if (m.value == "default") {
                    size_t dot = m.description.find("\xC2\xB7");  // « Opus 5.5 · … »
                    if (dot != std::string::npos) resolved = util::trim(m.description.substr(0, dot));
                }
        std::string tag = resolved.empty() ? "Le modèle choisi dans Claude Code (/model)."
                                           : "Le modèle choisi dans Claude Code (/model) : " + resolved + " aujourd'hui.";
        if (card("default", "Par défaut", tag.c_str(), 0, 0, claude::ModelAccess::Included, false, model.empty(), avail))
            model.clear();
    }
    int i = 0;
    for (auto& f : claude::modelFamilies()) {
        if (cols == 2 && i % 2 == 1) ImGui::SameLine(0, gap);
        claude::ModelAccess a = settings_.modelAccess.access(f.alias, plan);
        bool sel = model == f.alias || model == f.fullId;
        std::string title = std::string(f.name) + " " + f.version;
        if (card(f.alias, title, f.tagline, f.power, f.cost, a, true, sel, cw)) model = f.alias;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::PushFont(fontBold, 0.0f);
            ImGui::TextUnformatted(title.c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("Pour : %s.", f.uses);
            ImGui::TextColored(accessColor(a), "%s (%s)%s", claude::modelAccessLabel(a), planDisplay(plan).c_str(),
                               settings_.modelAccess.observed(f.alias, plan) ? " — vérifié" : "");
            if (a == claude::ModelAccess::NeedsCredits)
                ImGui::TextDisabled("Il ne puise pas dans le quota de l'abonnement mais dans des crédits achetés à part\n"
                                    "(claude.ai › Réglages › Usage). Sans crédits, Claude Code refuse le message.");
            if (a == claude::ModelAccess::Unknown)
                ImGui::TextDisabled("ClaudeDesk le saura au premier message : un refus est retenu, et ce modèle n'est\n"
                                    "alors plus proposé à l'orchestrateur.");
            ImGui::EndTooltip();
        }
        ++i;
    }
    // Choix hors abonnement : on prévient
    if (const claude::ModelFamily* f = claude::findModelFamily(model)) {
        claude::ModelAccess a = settings_.modelAccess.access(f->alias, plan);
        if (a == claude::ModelAccess::NeedsCredits || a == claude::ModelAccess::Unavailable) {
            ImGui::PushStyleColor(ImGuiCol_Text, accessColor(a));
            ImGui::TextWrapped("%s", a == claude::ModelAccess::NeedsCredits
                                         ? (std::string(f->name) + " n'est pas inclus dans votre abonnement : sans crédits "
                                            "d'usage achetés à part, vos messages seront refusés.").c_str()
                                         : (std::string(f->name) + " n'est pas accessible avec ce compte.").c_str());
            ImGui::PopStyleColor();
        }
    }

    // Avancé : version figée, identifiant libre, modèle de secours
    ImGui::Spacing();
    bool customModel = !model.empty() && !claude::findModelFamily(model);
    bool pinned = false;
    for (auto& f : claude::modelFamilies()) pinned = pinned || (model != f.alias && claude::findModelFamily(model) == &f);
    if (customModel || pinned) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNode("Options avancées : version précise, modèle de secours")) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
        ImGui::TextWrapped("Les noms ci-dessus suivent toujours la dernière version. Pour figer une version (ou en garder "
                           "une ancienne), choisissez-la ici.");
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 14, "Version"));
        if (ImGui::BeginCombo("Version", pinned || customModel ? model.c_str() : "dernière version (recommandé)")) {
            std::string chosen;
            if (drawModelPickerItems(s, model, chosen)) model = chosen;
            ImGui::EndCombo();
        }
        ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 14, "Identifiant"));
        ImGui::InputTextWithHint("Identifiant", "ex. claude-opus-5-5", &model);
        ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 14, "Modèle de secours"));
        ImGui::InputTextWithHint("Modèle de secours", "si le principal est surchargé", &editConfig_.fallbackModel);
        ui::helpMarker("Utilisé par Claude Code quand le modèle principal est surchargé (--fallback-model).");
        if (!settings_.modelAccess.seen.empty()) {
            if (ImGui::SmallButton("Oublier les accès vérifiés")) {
                settings_.modelAccess.seen.clear();
                settings_.save(settingsPath_);
            }
            ui::helpMarker("Après un changement d'abonnement ou l'achat de crédits : ClaudeDesk réapprendra au prochain message.");
        }
        ImGui::TreePop();
    }
}

void App::drawEffortSelector(std::string& effort, claude::ClaudeSession* s) {
    const auto& pal = ui::palette();
    const auto& choices = claude::effortChoices();
    (void)s;
    ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
    ImGui::TextWrapped("Combien Claude réfléchit avant de répondre. Plus d'effort : réponses plus soignées, mais plus "
                       "lentes et plus gourmandes en quota.");
    ImGui::PopStyleColor();
    const claude::Choice* cur = &choices[0];
    for (auto& c : choices) {
        bool sel = effort == c.value;
        if (sel) cur = &c;
        float w = ImGui::CalcTextSize(c.label).x + ImGui::GetStyle().FramePadding.x * 2;
        if (&c != &choices[0]) ui::sameLineIfFits(w);
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        }
        if (ImGui::Button(c.label)) effort = c.value;
        if (sel) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.help);
    }
    ImGui::TextColored(pal.link, "%s", cur->help);
}

// Mode de permission : ce que Claude peut faire sans vous demander. Cartes rangées par autonomie croissante ;
// les deux modes extrêmes (tout refuser, tout autoriser) sont à part, repliés.
void App::drawPermissionCards(std::string& mode) {
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    struct ModeCard {
        const char* value;
        const char* title;
        const char* text;
        int autonomy;  // 1 à 4
        ImVec4 stripe;
    };
    const ModeCard main[] = {
        {"plan", "Plan", "Lecture seule : Claude explore et propose un plan, sans rien modifier.", 1, pal.ok},
        {"default", "Normal", "Claude demande votre accord avant de modifier un fichier ou de lancer une commande.", 2, pal.ok},
        {"acceptEdits", "Accepter les modifications", "Claude modifie les fichiers sans demander ; il demande encore pour les commandes.", 3, pal.link},
        {"auto", "Auto", "Claude juge seul ce qui est sûr et ne vous demande que le reste.", 4, pal.link},
    };
    const ModeCard extra[] = {
        {"dontAsk", "Ne rien demander", "Tout ce qui demanderait votre accord est refusé : Claude continue sans.", 0, pal.warn},
        {"bypassPermissions", "Sans permissions", "Tout est autorisé, sans aucune confirmation. À réserver aux dossiers jetables.", 0, pal.err},
    };
    ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
    ImGui::TextWrapped("Ce que Claude peut faire sans vous demander. S'applique tout de suite, même pendant qu'il travaille.");
    ImGui::PopStyleColor();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const int cols = avail >= fs * 30 ? 2 : 1;
    const float cw = cols == 2 ? (avail - gap) * 0.5f : avail;
    auto draw = [&](const ModeCard& m, float w) {
        SelectCard c;
        c.id = m.value;
        c.title = m.title;
        c.text = m.text;
        c.selected = mode == m.value;
        c.stripe = m.stripe;
        if (m.autonomy > 0) c.meters[0] = {"autonomie", m.autonomy, pal.accent};
        if (std::string(m.value) == "default") c.badge = "recommandé", c.badgeColor = pal.ok;
        if (drawSelectCard(c, w, fontBold, fontUi)) mode = m.value;
    };
    for (int i = 0; i < 4; ++i) {
        if (cols == 2 && i % 2 == 1) ImGui::SameLine(0, gap);
        draw(main[i], cw);
    }
    bool advanced = mode == "dontAsk" || mode == "bypassPermissions";
    ImGui::Spacing();
    if (advanced) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("Modes extrêmes", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        for (auto& m : extra) draw(m, ImGui::GetContentRegionAvail().x);
        if (mode == "bypassPermissions")
            ImGui::TextColored(pal.err, "Attention : aucune confirmation ne sera demandée, même pour supprimer des fichiers.");
        ImGui::TreePop();
    }
}

// Préréglages d'énergie : quatre cartes cliquables, l'alimentation actuelle, et le réglage du mode automatique.
void App::drawEnergyPresets() {
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    ImGui::SeparatorText("Énergie");
    ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
    ImGui::TextWrapped("Choisissez comment ClaudeDesk utilise la batterie et la carte graphique : un clic suffit, "
                       "les réglages détaillés ci-dessous suivent.");
    ImGui::PopStyleColor();
    const int modes[] = {EnergyAuto, EnergyPerformance, EnergyBalanced, EnergySaver};
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    int cols = avail > fs * 30 ? 2 : 1;
    float cw = (avail - gap * (cols - 1)) / cols;
    ImVec2 pad = ImGui::GetStyle().FramePadding;
    // Hauteur commune : la plus grande description (avec retour à la ligne)
    float ch = 0;
    for (int m : modes) {
        ImVec2 t = fontUi->CalcTextSizeA(fs * 0.92f, FLT_MAX, cw - pad.x * 3, energyModeSummary(m));
        ch = std::max(ch, t.y);
    }
    ch += ImGui::GetTextLineHeightWithSpacing() * 1.25f + pad.y * 4;
    for (int i = 0; i < 4; ++i) {
        int m = modes[i];
        if (i % cols) ImGui::SameLine(0, gap);
        ImGui::PushID(m);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("##energy", ImVec2(cw, ch));
        bool hov = ImGui::IsItemHovered();
        bool active = settings_.energyMode == m;
        ImVec2 p1(p0.x + cw, p0.y + ch);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec4 bg = active ? ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.20f) : pal.cardBg;
        if (hov && !active) bg.w = std::min(1.0f, bg.w + 0.15f);
        dl->AddRectFilled(p0, p1, ui::col(bg), 8.0f);
        dl->AddRect(p0, p1, ui::col(active ? pal.accent : hov ? pal.dim : ImGui::GetStyleColorVec4(ImGuiCol_Border)), 8.0f, 0,
                    active ? 2.0f : 1.0f);
        ImVec2 tp(p0.x + pad.x * 1.5f, p0.y + pad.y * 2);
        std::string title = std::string(active ? "● " : "") + energyModeLabel(m);
        if (m == EnergyAuto && hasBattery_) title += "  (recommandé)";
        dl->AddText(fontBold, fs, tp, ui::col(active ? pal.accent : ImGui::GetStyleColorVec4(ImGuiCol_Text)), title.c_str());
        dl->AddText(fontUi, fs * 0.92f, ImVec2(tp.x, tp.y + ImGui::GetTextLineHeightWithSpacing() * 1.15f), ui::col(pal.dim),
                    energyModeSummary(m), nullptr, cw - pad.x * 3);
        if (clicked) setEnergyMode(m);
        ImGui::PopID();
    }
    if (settings_.energyMode == EnergyCustom)
        ImGui::TextColored(pal.link, "Réglages personnalisés. Cliquez un mode pour y revenir.");

    // Alimentation actuelle
    std::string power;
    if (!hasBattery_ && !onBattery_) {
        power = "Alimentation : secteur (pas de batterie détectée)";
    } else {
        power = onBattery_ ? "Alimentation : sur batterie" : "Alimentation : sur secteur";
        if (batteryPct_ >= 0) power += " · batterie " + std::to_string(batteryPct_) + " %";
        if (onBattery_ && batterySecs_ > 0) power += " · environ " + util::humanDuration(batterySecs_) + " restantes";
    }
    ImGui::TextDisabled("%s", power.c_str());
    if (settings_.energyMode == EnergyAuto) {
        ImGui::TextDisabled("Appliqué maintenant : %s", energyApplied_ > 0 ? energyModeLabel(energyApplied_) : "—");
        const char* fixed[] = {"Performance", "Équilibré", "Économie de batterie"};
        int ac = settings_.energyOnAc - 1, bat = settings_.energyOnBattery - 1;
        ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 11, "sur secteur"));
        if (ImGui::Combo("sur secteur", &ac, fixed, 3)) {
            settings_.energyOnAc = ac + 1;
            tickEnergy(true);
        }
        ui::sameLineIfFits(fs * 18);
        ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 11, "sur batterie"));
        if (ImGui::Combo("sur batterie", &bat, fixed, 3)) {
            settings_.energyOnBattery = bat + 1;
            tickEnergy(true);
        }
    }
#ifdef _WIN32
    if (hasBattery_) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
        ImGui::TextWrapped("Portable à deux cartes graphiques : ClaudeDesk demande la carte dédiée, plus gourmande. Pour "
                           "économiser davantage sur batterie, choisissez « Économie d'énergie » pour ClaudeDesk dans les "
                           "paramètres graphiques de Windows (effet au prochain lancement).");
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Ouvrir les paramètres graphiques de Windows")) util::openInShell("ms-settings:display-advancedgraphics");
    }
#endif
}

// ---------------------------------------------------------------------------
// Fenêtre « Configuration » : navigation par sections (à la manière des réglages de VS Code).
//   Conversation : 0 Modèle · 1 Mode · 2 Outils · 3 Instructions · 4 Avancé
//   Application  : 5 Général · 6 Performances · 7 Apparence
// Panneau large : liste des sections à gauche ; étroit : sélecteur déroulant en tête.
// ---------------------------------------------------------------------------

namespace {
struct ConfigSection {
    const char* name;
    const char* hint;  // une ligne, dans le sélecteur
};
const ConfigSection kSections[] = {
    {"Modèle", "modèle, effort de réflexion, style"},
    {"Mode", "ce que Claude peut faire sans demander"},
    {"Outils", "outils disponibles et autorisations"},
    {"Instructions", "consignes permanentes pour Claude"},
    {"Avancé", "dossiers, budget, ligne de commande"},
    {"Général", "Claude Code, projets, comportement"},
    {"Performances", "énergie, fluidité, carte graphique"},
    {"Apparence", "thème, police, densité, messages"},
};
constexpr int kSectionCount = 8;
constexpr int kFirstAppSection = 5;
}  // namespace

void App::drawConfigWindow() {
    if (!ImGui::Begin("Configuration", &showConfig_)) {
        ImGui::End();
        return;
    }
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    OpenConversation* oc = active();
    claude::ClaudeSession* s = oc ? oc->session.get() : nullptr;
    std::string owner = s ? s->localId : "";
    if (owner != editConfigOwner_) {
        editConfig_ = s ? s->config : settings_.defaultConfig;
        editConfigOwner_ = owner;
    }
    const claude::SessionConfig& base = s ? s->config : settings_.defaultConfig;
    // Le modèle et le mode peuvent changer depuis la barre rapide : on les resynchronise.
    if (s) {
        static std::string lastModel, lastMode;
        if (base.model != lastModel) editConfig_.model = base.model;
        if (base.permissionMode != lastMode) editConfig_.permissionMode = base.permissionMode;
        lastModel = base.model;
        lastMode = base.permissionMode;
    }

    static int section = 0;
    if (configTab_ >= 0) section = std::clamp(configTab_, 0, kSectionCount - 1);
    configTab_ = -1;
    const bool convSection = section < kFirstAppSection;
    // Hauteur réservée au pied (appliquer) : mesurée à l'image précédente.
    static float footerMeasured = -1.0f;
    float footer = footerMeasured >= 0 ? footerMeasured : ImGui::GetFrameHeightWithSpacing() * 2.2f;

    const bool wide = ImGui::GetContentRegionAvail().x >= fs * 30;
    auto groupTitle = [&](int i) -> const char* {
        if (i == 0) return s ? "Cette conversation" : "Nouvelles conversations";
        if (i == kFirstAppSection) return "Application";
        return nullptr;
    };
    if (wide) {
        // Liste des sections à gauche
        ImGui::BeginChild("##cfgnav", ImVec2(fs * 9.5f, 0), ImGuiChildFlags_None);
        for (int i = 0; i < kSectionCount; ++i) {
            if (const char* g = groupTitle(i)) {
                if (i > 0) ImGui::Spacing();
                ImGui::TextDisabled("%s", g);
            }
            if (ImGui::Selectable(kSections[i].name, section == i)) section = i;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", kSections[i].hint);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginGroup();
    } else {
        // Sélecteur déroulant : la section courante, son groupe
        std::string cur = std::string(kSections[section].name) + "  ·  " +
                          (section < kFirstAppSection ? (s ? "cette conversation" : "nouvelles conversations") : "application");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##cfgsection", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < kSectionCount; ++i) {
                if (const char* g = groupTitle(i)) {
                    if (i > 0) ImGui::Separator();
                    ImGui::TextDisabled("%s", g);
                }
                if (ImGui::Selectable(kSections[i].name, section == i)) section = i;
                ImGui::SameLine();
                ImGui::TextDisabled("%s", kSections[i].hint);
            }
            ImGui::EndCombo();
        }
    }
    if (convSection && s) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
        ImGui::TextWrapped("Conversation « %s »", util::truncateUtf8(s->title, 50).c_str());
        ImGui::PopStyleColor();
    }

    ImGui::BeginChild("##cfgbody", ImVec2(0, convSection ? -footer : 0));
    switch (section) {
        case 0:
            drawModelCards(editConfig_.model, s);
            ImGui::SeparatorText("Effort de réflexion");
            drawEffortSelector(editConfig_.effort, s);
            ImGui::SeparatorText("Style de réponse");
            {
                std::vector<std::string> styles = {"default", "Concise", "Explanatory", "Learning", "Proactive"};
                if (s && !s->outputStyles.empty()) styles = s->outputStyles;
                std::string cur = editConfig_.outputStyle.empty() ? "default" : editConfig_.outputStyle;
                ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 12));
                if (ImGui::BeginCombo("##style", cur == "default" ? "Par défaut" : cur.c_str())) {
                    for (auto& st : styles)
                        if (ImGui::Selectable(st == "default" ? "Par défaut" : st.c_str(), st == cur))
                            editConfig_.outputStyle = st == "default" ? "" : st;
                    ImGui::EndCombo();
                }
                ui::helpMarker("Styles de sortie de Claude Code (/output-style) : plus concis, plus pédagogique…\n"
                               "Les vôtres se trouvent dans ~/.claude/output-styles.");
            }
            break;
        case 1: drawPermissionCards(editConfig_.permissionMode); break;
        case 2: drawConfigTools(s); break;
        case 3: drawConfigInstructions(oc); break;
        case 4: drawConfigAdvanced(s); break;
        case 5: drawConfigGeneral(); break;
        case 6: drawConfigPerformance(); break;
        case 7: drawConfigAppearance(); break;
    }
    ImGui::EndChild();

    // --- Pied : appliquer (sections de conversation seulement, et seulement s'il y a quelque chose à appliquer) ---
    float footerTop = ImGui::GetCursorPosY();
    if (convSection) {
        bool changed = nlohmann::json(editConfig_) != nlohmann::json(base);
        bool restart = s && s->processRunning() && editConfig_.needsRestartComparedTo(base);
        if (changed) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.warn);
            ImGui::TextWrapped("%s", restart ? "Modifications non appliquées — la session sera relancée (historique conservé)."
                                             : "Modifications non appliquées.");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            bool apply = ImGui::Button(s ? "Appliquer à la conversation" : "Appliquer");
            ImGui::PopStyleColor(2);
            if (apply) {
                if (s) s->applyConfig(editConfig_);
                else {
                    settings_.defaultConfig = editConfig_;
                    settings_.save(settingsPath_);
                }
            }
            ui::sameLineIfFits(ui::buttonWidth("Annuler"));
            if (ImGui::Button("Annuler")) editConfig_ = base;
        }
        if (s) {
            if (changed) ui::sameLineIfFits(ui::buttonWidth("Enregistrer comme défaut"));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
            bool save = ImGui::SmallButton("Enregistrer comme défaut");
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ces réglages serviront aux nouvelles conversations.");
            if (save) {
                settings_.defaultConfig = editConfig_;
                settings_.save(settingsPath_);
                toast("Réglages par défaut enregistrés pour les nouvelles conversations.");
            }
        }
    }
    footerMeasured = ImGui::GetCursorPosY() - footerTop;
    if (wide) ImGui::EndGroup();
    ImGui::End();
}

// --- Outils : une ligne par outil, nom lisible + « Demander / Toujours / Jamais » ---------------------------------
void App::drawConfigTools(claude::ClaudeSession* s) {
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    std::vector<std::string> tools = claude::knownTools();
    if (s && !s->tools.empty()) {
        tools.clear();
        for (auto& t : s->tools)
            if (!util::startsWith(t, "mcp__")) tools.push_back(t);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
    ImGui::TextWrapped("Pour chaque outil : Claude demande votre accord, l'utilise toujours sans demander, ou jamais.");
    ImGui::PopStyleColor();
    bool restrict = !editConfig_.enabledTools.empty();
    if (ui::checkboxWrapped("Ne donner à Claude que certains outils", &restrict,
                            "Coché : seuls les outils cochés existent pour Claude (--tools). Décoché : tous les outils.")) {
        if (restrict) editConfig_.enabledTools = {"Read", "Glob", "Grep"};
        else editConfig_.enabledTools.clear();
    }
    ImGui::Spacing();
    const char* modes[] = {"Demander", "Toujours", "Jamais"};
    for (auto& t : tools) {
        ImGui::PushID(t.c_str());
        bool avail = !restrict || contains(editConfig_.enabledTools, t);
        if (restrict) {
            if (ImGui::Checkbox("##avail", &avail)) {
                if (avail) editConfig_.enabledTools.push_back(t);
                else removeAll(editConfig_.enabledTools, t);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Outil disponible pour Claude");
            ImGui::SameLine();
        }
        float rowStartX = ImGui::GetCursorPosX();
        // Place réservée au sélecteur à droite ; le nom s'adapte (nom technique seulement s'il reste de la place,
        // nom lisible raccourci sinon — le nom complet est dans l'infobulle).
        const float comboW = fs * 6.2f;
        const float room = ImGui::GetContentRegionAvail().x - comboW - ImGui::GetStyle().ItemSpacing.x;
        std::string label = claude::toolLabel(t);
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        bool showId = label != t && ImGui::CalcTextSize(label.c_str()).x + sp + ImGui::CalcTextSize(t.c_str()).x <= room;
        std::string shown = label;
        while (ImGui::CalcTextSize(shown.c_str()).x > room && shown.size() > 4) {
            shown = util::truncateUtf8(shown, std::max<size_t>(3, shown.size() > 6 ? shown.size() - 4 : 3));
            if (shown.find("…") == std::string::npos) shown += "…";
        }
        ImGui::AlignTextToFramePadding();
        if (!avail) ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
        ImGui::TextUnformatted(shown.c_str());
        if (!avail) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)", label.c_str(), t.c_str());
        if (showId) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", t.c_str());
        }
        int cur = contains(editConfig_.allowedTools, t) ? 1 : contains(editConfig_.disallowedTools, t) ? 2 : 0;
        // Sélecteur compact aligné à droite ; coloré seulement s'il s'écarte du réglage par défaut (« Demander »)
        ImGui::SameLine();
        float ax = ImGui::GetContentRegionAvail().x;
        if (ax > comboW) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ax - comboW);
        ImGui::SetNextItemWidth(comboW);
        ImGui::BeginDisabled(!avail);
        if (cur != 0) ImGui::PushStyleColor(ImGuiCol_Text, cur == 1 ? pal.ok : pal.err);
        bool open = ImGui::BeginCombo("##mode", modes[cur]);
        if (cur != 0) ImGui::PopStyleColor();
        if (open) {
            for (int k = 0; k < 3; ++k)
                if (ImGui::Selectable(modes[k], cur == k) && k != cur) {
                    removeAll(editConfig_.allowedTools, t);
                    removeAll(editConfig_.disallowedTools, t);
                    if (k == 1) editConfig_.allowedTools.push_back(t);
                    if (k == 2) editConfig_.disallowedTools.push_back(t);
                }
            ImGui::EndCombo();
        }
        (void)rowStartX;
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::Spacing();
    ui::checkboxWrapped("Navigateur Chrome (Claude peut piloter Chrome)", &editConfig_.chrome,
                        "Intégration Chrome de Claude Code (--chrome) : navigation, captures, tests d'interfaces web.");
    if (s && !s->mcpServers.empty()) {
        ImGui::SeparatorText("Serveurs MCP");
        for (auto& [name, status] : s->mcpServers) {
            ImVec4 c = status == "connected" ? pal.ok : status == "failed" ? pal.err : pal.dim;
            ImGui::TextColored(c, "●");
            ImGui::SameLine();
            ImGui::TextUnformatted(name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status.c_str());
        }
    }
    // Règles fines : repliées (syntaxe de Claude Code)
    std::vector<std::string> customAllow, customDeny;
    for (auto& r : editConfig_.allowedTools)
        if (!contains(tools, r)) customAllow.push_back(r);
    for (auto& r : editConfig_.disallowedTools)
        if (!contains(tools, r)) customDeny.push_back(r);
    ImGui::Spacing();
    if (!customAllow.empty() || !customDeny.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("Règles précises", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
        ImGui::TextWrapped("Syntaxe de Claude Code, ex. Bash(git *), Edit(src/**), WebFetch(domain:github.com).");
        ImGui::PopStyleColor();
        static std::string allowBuf, denyBuf;
        ImGui::TextUnformatted("Toujours autorisées");
        auto before = customAllow;
        stringList("allow", customAllow, allowBuf, "Bash(npm test)");
        if (customAllow != before) {
            std::vector<std::string> v;
            for (auto& r : editConfig_.allowedTools)
                if (contains(tools, r)) v.push_back(r);
            v.insert(v.end(), customAllow.begin(), customAllow.end());
            editConfig_.allowedTools = v;
        }
        ImGui::TextUnformatted("Interdites");
        before = customDeny;
        stringList("deny", customDeny, denyBuf, "Bash(rm *)");
        if (customDeny != before) {
            std::vector<std::string> v;
            for (auto& r : editConfig_.disallowedTools)
                if (contains(tools, r)) v.push_back(r);
            v.insert(v.end(), customDeny.begin(), customDeny.end());
            editConfig_.disallowedTools = v;
        }
        ImGui::TreePop();
    }
    (void)fs;
}

// --- Instructions (prompt système) ----------------------------------------------------------------------------------
void App::drawConfigInstructions(OpenConversation* oc) {
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
    ImGui::TextWrapped("Des consignes données à Claude à chaque message : ton, langue, règles du projet…");
    ImGui::PopStyleColor();
    int mode = (int)editConfig_.systemPromptMode;
    const char* modes[] = {"Aucune", "Ajouter", "Remplacer"};
    if (ui::segmented("##spmode", &mode, modes, 3)) editConfig_.systemPromptMode = (claude::SystemPromptMode)mode;
    const char* explain[] = {"Claude Code utilise ses propres instructions, sans rien de plus.",
                             "Vos consignes s'ajoutent à celles de Claude Code (recommandé).",
                             "Vos consignes remplacent celles de Claude Code : outils, sécurité et style compris."};
    ImGui::PushStyleColor(ImGuiCol_Text, mode == 2 ? pal.warn : pal.dim);
    ImGui::TextWrapped("%s", explain[std::clamp(mode, 0, 2)]);
    ImGui::PopStyleColor();
    if (mode != 0) {
        ImGui::InputTextMultiline("##sysprompt", &editConfig_.systemPrompt,
                                  ImVec2(-1, std::max(fs * 8, ImGui::GetContentRegionAvail().y * 0.5f)),
                                  ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_AllowTabInput);
    }
    // Modèles de consignes : choisir, enregistrer, supprimer
    static int preset = -1;
    if (preset >= (int)settings_.promptPresets.size()) preset = -1;
    ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 12));
    if (ImGui::BeginCombo("##preset", preset >= 0 ? settings_.promptPresets[preset].name.c_str() : "Modèles de consignes…")) {
        for (int i = 0; i < (int)settings_.promptPresets.size(); ++i)
            if (ImGui::Selectable(settings_.promptPresets[i].name.c_str(), preset == i)) {
                preset = i;
                editConfig_.systemPrompt = settings_.promptPresets[i].text;
                if (editConfig_.systemPromptMode == claude::SystemPromptMode::Default)
                    editConfig_.systemPromptMode = claude::SystemPromptMode::Append;
            }
        if (settings_.promptPresets.empty()) ImGui::TextDisabled("Aucun modèle enregistré.");
        ImGui::EndCombo();
    }
    if (mode != 0) {
        ui::sameLineIfFits(ui::buttonWidth("Enregistrer…"));
        if (ImGui::Button("Enregistrer…")) ImGui::OpenPopup("##savepreset");
        if (ImGui::BeginPopup("##savepreset")) {
            static std::string presetName;
            ImGui::TextUnformatted("Nom du modèle de consignes");
            ImGui::SetNextItemWidth(fs * 14);
            bool enter = ImGui::InputText("##pname", &presetName, ImGuiInputTextFlags_EnterReturnsTrue);
            if ((ImGui::Button("Enregistrer") || enter) && !util::trim(presetName).empty()) {
                bool found = false;
                for (auto& p : settings_.promptPresets)
                    if (p.name == presetName) p.text = editConfig_.systemPrompt, found = true;
                if (!found) settings_.promptPresets.push_back({presetName, editConfig_.systemPrompt});
                settings_.save(settingsPath_);
                toast("Modèle enregistré : " + presetName);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    if (preset >= 0) {
        ui::sameLineIfFits(ui::buttonWidth("Supprimer"));
        if (ImGui::Button("Supprimer")) {
            settings_.promptPresets.erase(settings_.promptPresets.begin() + preset);
            preset = -1;
        }
    }
    if (oc) {
        std::string ctx = oc->injectContext ? contextFor(*oc) : "";
        ImGui::Spacing();
        if (ImGui::TreeNodeEx(("Mémoire du projet ajoutée (" + util::humanTokens((int64_t)ctx.size()) + " caractères)###ctx").c_str(),
                              ImGuiTreeNodeFlags_SpanAvailWidth)) {
            if (ctx.empty()) ImGui::TextDisabled("Aucune (désactivée ou conversation hors projet).");
            else ui::selectableText("##ctxview", ctx, ImGui::GetContentRegionAvail().x, fontMono);
            ImGui::TreePop();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Notes, synthèses et récaps du dossier, donnés à Claude au début de la conversation.");
    }
}

// --- Avancé ----------------------------------------------------------------------------------------------------------
void App::drawConfigAdvanced(claude::ClaudeSession* s) {
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    if (s) {
        ImGui::SeparatorText("Dossier de travail");
        ImGui::BeginDisabled(!s->claudeSessionId.empty());
        ImGui::SetNextItemWidth(-ui::buttonWidth("Parcourir…") - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##cwd", &s->cwd);
        ImGui::SameLine();
        if (ImGui::Button("Parcourir…")) {
            std::string id = s->localId;
            openFolderDialog([this, id](const std::string& p) {
                for (auto& o : open_)
                    if (o->session->localId == id) o->session->cwd = p;
            }, s->cwd);
        }
        ImGui::EndDisabled();
        if (!s->claudeSessionId.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
            ImGui::TextWrapped("Figé : la session de Claude Code est liée à ce dossier.");
            ImGui::PopStyleColor();
        }
    }
    ImGui::SeparatorText("Dossiers supplémentaires");
    ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
    ImGui::TextWrapped("Dossiers que Claude peut aussi lire et modifier (--add-dir).");
    ImGui::PopStyleColor();
    static std::string dirBuf;
    stringList("dirs", editConfig_.addDirs, dirBuf, "C:\\chemin\\vers\\dossier");
    if (ImGui::SmallButton("Choisir un dossier…"))
        openFolderDialog([this](const std::string& p) { editConfig_.addDirs.push_back(p); });

    ImGui::SeparatorText("Budget maximal");
    ImGui::SetNextItemWidth(fs * 7);
    ImGui::InputDouble("##budget", &editConfig_.maxBudgetUsd, 0, 0, "%.2f $");
    if (editConfig_.maxBudgetUsd <= 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("illimité");
    }
    ui::helpMarker("Coût maximal de la conversation, en dollars (tarif API) ; 0 = illimité. Avec un abonnement,\n"
                   "c'est le quota qui compte : ce plafond sert surtout avec une clé d'API.");

    ImGui::SeparatorText("Ligne de commande");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##extra", "options en plus, ex. --strict-mcp-config", &editConfig_.extraArgs);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ajoutées telles quelles à la commande qui lance Claude Code.");
    if (s && s->processRunning()) {
        ImGui::SeparatorText("Session en cours");
        ImGui::TextDisabled("Claude Code %s · %s · mode %s", s->cliVersion.c_str(), claude::modelDisplayName(s->activeModel).c_str(),
                            s->activePermissionMode.c_str());
        if (ImGui::SmallButton("Redémarrer la session")) s->restart();
    }
}

// --- Général -----------------------------------------------------------------------------------------------------------
void App::drawConfigGeneral() {
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    ImGui::SeparatorText("Claude Code");
    if (claudeExe_.empty()) {
        ImGui::TextColored(pal.err, "Claude Code introuvable.");
    } else {
        ImGui::TextColored(pal.ok, "●");
        ImGui::SameLine();
        ImGui::TextUnformatted("Claude Code trouvé");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", claudeExe_.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ui::shortPath(claudeExe_, 1).c_str());
    }
    if (ImGui::TreeNodeEx("Choisir l'exécutable à la main", claudeExe_.empty() ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::SetNextItemWidth(-ui::buttonWidth("Détecter") - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::InputTextWithHint("##exe", "détection automatique", &settings_.claudeExe))
            claudeExe_ = settings_.claudeExe.empty() ? claude::findClaudeExecutable() : settings_.claudeExe;
        ImGui::SameLine();
        if (ImGui::Button("Détecter")) {
            settings_.claudeExe.clear();
            claudeExe_ = claude::findClaudeExecutable();
        }
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Projets");
    std::string root = util::pathToUtf8(workspace_.root());
    ImGui::TextUnformatted("Dossier des projets");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ui::shortPath(root, 2).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", root.c_str());
    if (ImGui::SmallButton("Ouvrir")) util::openInShell(root);
    ImGui::SameLine();
    if (ImGui::SmallButton("Changer…"))
        openFolderDialog([this](const std::string& p) {
            settings_.workspaceRoot = p;
            saveAll();
            workspace_.open(util::pathFromUtf8(p));
            settings_.save(settingsPath_);
        });
    ImGui::Spacing();
    ImGui::TextUnformatted("Dossier de travail par défaut");
    ImGui::SetNextItemWidth(-ui::buttonWidth("Parcourir…") - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##defcwd", "votre dossier personnel", &settings_.defaultCwd);
    ImGui::SameLine();
    if (ImGui::Button("Parcourir…##cwd")) openFolderDialog([this](const std::string& p) { settings_.defaultCwd = p; });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pour les conversations hors projet.");
    ui::checkboxWrapped("Donner à Claude la mémoire du projet (notes, synthèses, récaps)", &settings_.injectContext,
                        "Au début de chaque nouvelle conversation d'un dossier, Claude reçoit son contexte.");
    ui::checkboxWrapped("Récap automatique en fermant une conversation", &settings_.autoRecap,
                        "Un court résumé, pour que les prochaines conversations s'en souviennent.");
    {
        const char* recapIds[] = {"haiku", "sonnet"};
        const char* recapNames[] = {"Haiku (économe)", "Sonnet"};
        int r = settings_.recapModel == "sonnet" ? 1 : 0;
        ImGui::TextUnformatted("Modèle des récaps");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 9));
        if (ImGui::Combo("##recapmodel", &r, recapNames, 2)) settings_.recapModel = recapIds[r];
    }

    ImGui::SeparatorText("Comportement");
    ui::checkboxWrapped("Entrée envoie le message (Maj+Entrée : nouvelle ligne)", &settings_.enterSends,
                        "Décoché : Entrée = nouvelle ligne, Ctrl+Entrée = envoyer.");
    ui::checkboxWrapped("Faire clignoter la barre des tâches quand Claude a fini", &settings_.notifyOnFinish);
    ui::checkboxWrapped("Mesurer l'usage automatiquement", &settings_.autoRefreshUsage,
                        "Au lancement, puis quand la mesure a plus de 5 min. La commande /usage est gratuite :\n"
                        "elle ne consomme aucun quota.");
    ui::checkboxWrapped("Demander confirmation pour le mode « Sans permissions »", &settings_.confirmBypass);
    {
        ImGui::TextUnformatted("Sauvegarde des conversations");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 8));
        ImGui::SliderInt("##autosave", &settings_.autosaveSeconds, 1, 60, "toutes les %d s");
    }

    ImGui::Spacing();
    if (ImGui::TreeNodeEx("Accueil et réinitialisation", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        if (ImGui::Button("Ouvrir le guide")) {
            showGuide_ = true;
            guideSection_ = 0;
        }
        ui::sameLineIfFits(ui::buttonWidth("Revoir l'accueil"));
        if (ImGui::Button("Revoir l'accueil")) {
            settings_.launchCount = 0;
            settings_.save(settingsPath_);
            toast("Le guide s'ouvrira au prochain lancement, puis les rappels reprendront.");
        }
        if (ImGui::Button("Réinitialiser les préférences…")) ImGui::OpenPopup("Réinitialiser ClaudeDesk ?");
        if (ImGui::BeginPopupModal("Réinitialiser ClaudeDesk ?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Tous les réglages, la disposition des panneaux et l'accueil reviennent à zéro,");
            ImGui::TextUnformatted("puis ClaudeDesk redémarre. Vos projets et conversations sont conservés.");
            if (ImGui::Button("Réinitialiser et redémarrer")) {
                ImGui::CloseCurrentPopup();
                resetPreferences();
            }
            ImGui::SameLine();
            if (ImGui::Button("Annuler")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::TreePop();
    }
}

// --- Apparence ---------------------------------------------------------------------------------------------------------
void App::drawConfigAppearance() {
    const float fs = ImGui::GetFontSize();
    ImGui::SeparatorText("Thème");
    const char* themes[] = {"Sombre", "Clair", "Crépuscule"};
    if (ui::segmented("##theme", &settings_.theme, themes, 3)) applyTheme();
    int dens = settings_.compactUi ? 1 : 0;
    const char* densities[] = {"Confortable", "Compacte"};
    ImGui::TextDisabled("Densité");
    ImGui::SameLine();
    if (ui::segmented("##density", &dens, densities, 2)) {
        settings_.compactUi = dens == 1;
        applyTheme();
    }

    ImGui::SeparatorText("Texte");
#if defined(_WIN32)
    const char* sysUi = "Système (Segoe UI)";
    const char* sysMono = "Système (Consolas)";
#elif defined(__APPLE__)
    const char* sysUi = "Système (Arial)";
    const char* sysMono = "Système (Menlo)";
#else
    const char* sysUi = "Système (DejaVu Sans)";
    const char* sysMono = "Système (DejaVu Sans Mono)";
#endif
    auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(fs * 7.5f);
        ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 13));
    };
    const char* uiIds[] = {"plex", "noto", "inter", "system"};
    const char* uiNames[] = {"IBM Plex Sans", "Noto Sans", "Inter", sysUi};
    int uiSel = 3;
    for (int k = 0; k < 4; ++k)
        if (settings_.uiFont == uiIds[k]) uiSel = k;
    row("Police");
    if (ImGui::Combo("##uifont", &uiSel, uiNames, 4)) {
        settings_.uiFont = uiIds[uiSel];
        fontFamilyReloadPending_ = true;
    }
    int mono = settings_.monoFont == "jetbrains" ? 0 : 1;
    const char* monoNames[] = {"JetBrains Mono", sysMono};
    row("Code");
    if (ImGui::Combo("##monofont", &mono, monoNames, 2)) {
        settings_.monoFont = mono == 0 ? "jetbrains" : "system";
        fontFamilyReloadPending_ = true;
    }
    row("Taille");
    if (ImGui::SliderFloat("##fontsize", &settings_.fontSize, 11, 32, "%.0f px")) {
        ImGui::GetStyle().FontSizeBase = settings_.fontSize;
        for (auto& o : open_) o->cache.clear();
    }
    row("Épaisseur");
    if (ImGui::SliderFloat("##fontweight", &settings_.fontWeight, 0.8f, 1.8f, "%.1f")) fontFamilyReloadPending_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Épaissit les traits fins : texte plus dense et plus net sur fond sombre.");
    {
        const int order[] = {3, 0, 1, 2};
        const char* fr[] = {"Net (recommandé)", "Lissage léger", "Doux (sans grille)", "Moteur de base"};
        int sel = 0;
        for (int k = 0; k < 4; ++k)
            if (settings_.fontRenderer == order[k]) sel = k;
        row("Rendu");
        if (ImGui::Combo("##fontrender", &sel, fr, 4)) {
            settings_.fontRenderer = order[sel];
            fontReloadPending_ = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Net : FreeType avec le hinting de la police, chaque trait tombe sur un pixel.\n"
                              "Doux : lettres plus rondes mais un peu floues.");
    }

    ImGui::SeparatorText("Messages");
    ui::checkboxWrapped("Afficher la réflexion de Claude", &settings_.showThinking);
    ui::checkboxWrapped("Déplier les appels d'outils", &settings_.expandTools);
    ui::checkboxWrapped("Retour à la ligne dans les blocs de code", &settings_.wrapCode);
}

// --- Performances ------------------------------------------------------------------------------------------------------
void App::drawConfigPerformance() {
    const float fs = ImGui::GetFontSize();
    ImGuiIO& io = ImGui::GetIO();
    drawEnergyPresets();
    ImGui::Spacing();
    if (ImGui::TreeNodeEx("Réglages détaillés", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        ImGui::SeparatorText("Fluidité");
        char vs[96];
        std::snprintf(vs, sizeof vs, "Écran (%.0f Hz)", displayHz_);
        const char* fps[] = {vs, "Limite", "Illimitée"};
        ui::segmented("##fps", &settings_.fpsMode, fps, 3);
        if (settings_.fpsMode == 1) {
            ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 10));
            ImGui::SliderInt("##cap", &settings_.fpsCap, 30, 480, "%d images/s");
        }
        ui::checkboxWrapped("Ne redessiner qu'en cas d'activité", &settings_.ecoIdle,
                            "Au repos, l'interface attend vos actions au lieu de redessiner en continu.");
        ui::checkboxWrapped("Ralentir quand ClaudeDesk est en arrière-plan", &settings_.ecoBackground);
        if (settings_.ecoBackground) {
            ImGui::Indent();
            ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 10));
            ImGui::SliderInt("##bgfps", &settings_.backgroundFps, 5, 60, "%d images/s");
            ImGui::Unindent();
        }
        ui::checkboxWrapped("Afficher les images/s dans la barre d'état", &settings_.showFps);
        ImGui::TextDisabled("Actuellement : %.0f images/s", io.Framerate);

        ImGui::SeparatorText("Animations");
        ui::checkboxWrapped("Animations", &settings_.animations);
        ImGui::BeginDisabled(!settings_.animations);
        ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 10));
        ImGui::SliderFloat("##animspeed", &settings_.animSpeed, 0.5f, 2.0f, "vitesse x %.2f");
        ui::checkboxWrapped("Défilement fluide", &settings_.smoothScroll);
        ui::checkboxWrapped("Halos lumineux (cartes actives, visite guidée)", &settings_.glowEffects);
        ImGui::EndDisabled();
        if (ui::checkboxWrapped("Courbes et coins haute définition", &settings_.highQuality)) applyTheme();

        ImGui::SeparatorText("Carte graphique");
        ImGui::TextDisabled("Moteur actif : %s", rendererName_.c_str());
        auto label = [](const std::string& n) -> std::string {
            if (n.empty()) return "Automatique";
            if (n == "direct3d12") return "Direct3D 12";
            if (n == "direct3d11") return "Direct3D 11";
            if (n == "direct3d") return "Direct3D 9";
            if (n == "vulkan") return "Vulkan";
            if (n == "opengl") return "OpenGL";
            if (n == "opengles2") return "OpenGL ES 2";
            if (n == "gpu") return "SDL GPU";
            if (n == "software") return "Logiciel (sans GPU)";
            return n;
        };
        static std::string pendingDriver = "#";
        if (pendingDriver == "#") pendingDriver = settings_.renderDriver;
        ImGui::SetNextItemWidth(ui::fitItemWidth(fs * 12));
        if (ImGui::BeginCombo("##driver", label(pendingDriver).c_str())) {
            if (ImGui::Selectable(label("").c_str(), pendingDriver.empty())) pendingDriver.clear();
            for (int i = 0; i < SDL_GetNumRenderDrivers(); ++i) {
                std::string n = SDL_GetRenderDriver(i) ? SDL_GetRenderDriver(i) : "";
                if (!n.empty() && ImGui::Selectable(label(n).c_str(), pendingDriver == n)) pendingDriver = n;
            }
            ImGui::EndCombo();
        }
        if (pendingDriver != settings_.renderDriver) {
            ImGui::TextColored(ui::palette().warn, "Prend effet au redémarrage.");
            if (ImGui::SmallButton("Relancer maintenant")) {
                settings_.renderDriver = pendingDriver;
                restartApp();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Annuler##drv")) pendingDriver = settings_.renderDriver;
        }
        ImGui::TreePop();
    }
}

}  // namespace app
