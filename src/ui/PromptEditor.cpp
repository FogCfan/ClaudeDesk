// Éditeur de message : texte multi-ligne avec les raccourcis habituels
// (Ctrl+C/V/X/Z/Y/A, Ctrl+flèches, Maj+sélection, souris, double-clic…),
// barre rapide (modèle, effort, mode, pièces jointes) et autocomplétion « / ».
#include <algorithm>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "app/App.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "util/Strings.h"

namespace app {

namespace {

int editorCallback(ImGuiInputTextCallbackData* data) {
    auto* oc = static_cast<OpenConversation*>(data->UserData);
    if (oc->hasReplace) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, oc->replaceDraft.c_str());
        oc->hasReplace = false;
        oc->cursorToEnd = true;
    }
    if (oc->cursorToEnd) {
        data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
        oc->cursorToEnd = false;
    }
    return 0;
}

struct SlashItem {
    std::string name, description;
};

std::vector<SlashItem> slashCandidates(const claude::ClaudeSession& s, const std::string& prefix) {
    static const std::vector<SlashItem> builtins = {
        {"compact", "Résume l'historique pour libérer du contexte"},
        {"context", "Affiche l'occupation de la fenêtre de contexte"},
        {"cost", "Coût et durée de la session"},
        {"init", "Crée un CLAUDE.md décrivant le projet"},
        {"review", "Revue de code des changements"},
        {"security-review", "Revue de sécurité des changements"},
    };
    std::vector<SlashItem> all = builtins;
    for (auto& c : s.commands) {
        bool dup = false;
        for (auto& b : all)
            if (b.name == c.name) dup = true;
        if (!dup) all.push_back({c.name, c.description});
    }
    std::vector<SlashItem> out;
    std::string p = util::toLower(prefix);
    for (auto& c : all)
        if (util::startsWith(util::toLower(c.name), p)) out.push_back(c);
    for (auto& c : all)  // puis les correspondances au milieu du nom
        if (!util::startsWith(util::toLower(c.name), p) && util::containsInsensitive(c.name, p)) out.push_back(c);
    return out;
}

}  // namespace

// Barre sous le fil : modèle, effort, mode, joindre, mémoire (puces plates qui passent à la ligne au besoin),
// contexte à droite. Sa hauteur est mesurée (oc.quickBarHeight) pour réserver la bonne place sous le fil.
void App::drawQuickBar(OpenConversation& oc) {
    auto& s = *oc.session;
    const auto& pal = ui::palette();
    const float y0 = ImGui::GetCursorPosY();
    bool first = true;
    auto next = [&](const char* label) {
        if (!first) ui::sameLineIfFits(ui::buttonWidth(label));
        first = false;
    };

    // Modèle (changé à chaud)
    {
        std::string label = (s.config.model.empty() ? std::string("Modèle par défaut") : claude::modelDisplayName(s.config.model)) +
                            "  ▾###qmodel";
        bool ok = modelUsable(s.config.model);
        next(label.c_str());
        if (ui::chip(label.c_str(), ok ? nullptr : &pal.warn)) ImGui::OpenPopup("##qmodelpop");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(ok ? "Modèle (changé immédiatement)" : "Ce modèle n'est pas inclus dans votre abonnement");
        if (ImGui::BeginPopup("##qmodelpop")) {
            std::string chosen;
            if (drawModelPickerItems(&s, s.config.model, chosen)) s.setModel(chosen);
            ImGui::EndPopup();
        }
    }
    // Effort (relance transparente avec --resume)
    {
        std::string cur = "auto";
        for (auto& c : claude::effortChoices())
            if (c.value == s.config.effort && !s.config.effort.empty()) cur = util::toLower(c.label);
        std::string label = "Effort " + cur + "  ▾###qeffort";
        next(label.c_str());
        if (ui::chip(label.c_str(), &pal.dim)) ImGui::OpenPopup("##qeffortpop");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Effort de réflexion. Le changer relance la session en arrière-plan (historique conservé).");
        if (ImGui::BeginPopup("##qeffortpop")) {
            for (auto& c : claude::effortChoices()) {
                if (ImGui::Selectable(c.label, c.value == s.config.effort)) {
                    claude::SessionConfig cfg = s.config;
                    cfg.effort = c.value;
                    s.applyConfig(cfg);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", c.help);
            }
            ImGui::EndPopup();
        }
    }
    // Mode de permission (changé à chaud)
    static std::string pendingBypass;
    {
        std::string cur = s.config.permissionMode;
        for (auto& c : claude::permissionModeChoices())
            if (c.value == s.config.permissionMode) cur = c.label;
        bool danger = s.config.permissionMode == "bypassPermissions";
        std::string label = "Mode " + util::toLower(cur) + "  ▾###qmode";
        next(label.c_str());
        if (ui::chip(label.c_str(), danger ? &pal.err : &pal.dim)) ImGui::OpenPopup("##qmodepop");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ce que Claude peut faire sans vous demander (changé immédiatement)");
        if (ImGui::BeginPopup("##qmodepop")) {
            for (auto& c : claude::permissionModeChoices()) {
                bool bad = std::string(c.value) == "bypassPermissions";
                if (bad) ImGui::PushStyleColor(ImGuiCol_Text, pal.err);
                if (ImGui::Selectable(c.label, c.value == s.config.permissionMode)) {
                    if (bad && settings_.confirmBypass) pendingBypass = s.localId;
                    else s.setPermissionMode(c.value);
                }
                if (bad) ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", c.help);
            }
            ImGui::EndPopup();
        }
    }
    if (pendingBypass == s.localId) {
        ImGui::OpenPopup("Confirmer le mode sans permissions");
        pendingBypass.clear();
    }
    if (ImGui::BeginPopupModal("Confirmer le mode sans permissions", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(pal.err, "Claude pourra exécuter n'importe quelle commande et modifier n'importe quel fichier");
        ImGui::TextColored(pal.err, "sans vous demander. À réserver aux environnements jetables.");
        if (ImGui::Button("Activer quand même")) {
            s.setPermissionMode("bypassPermissions");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Annuler")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    // Joindre
    next("+ Joindre");
    if (ui::chip("+ Joindre", &pal.dim)) {
        std::string id = s.localId;
        openFileDialog([this, id](const std::vector<std::string>& files) {
            for (auto& o : open_)
                if (o->session->localId == id)
                    for (auto& f : files) attachFile(*o, f);
        });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Joindre des images ou des fichiers (glisser-déposer marche aussi)");
    // Mémoire du projet : bascule
    {
        const char* label = oc.injectContext ? "✓ Mémoire###qmem" : "Mémoire###qmem";
        next(label);
        if (ui::chip(label, oc.injectContext ? &pal.accent : &pal.dim)) oc.injectContext = !oc.injectContext;
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Mémoire du projet : notes, synthèses et récaps du dossier donnés à Claude au début\n"
                                   "de la conversation. Clic : activer / désactiver.");
            if (s.processRunning() || !s.claudeSessionId.empty())
                ImGui::TextColored(pal.warn, "Session déjà démarrée : s'appliquera à une nouvelle session.");
            ImGui::EndTooltip();
        }
    }
    // Contexte, à droite (sur la ligne si la place le permet, sinon dessous — jamais par-dessus le reste)
    if (s.stats.contextWindow > 0) {
        float f = (float)s.stats.contextTokens / (float)s.stats.contextWindow;
        char txt[32];
        std::snprintf(txt, sizeof txt, "contexte %d %%", (int)(f * 100 + 0.5f));
        const float lh = ImGui::GetTextLineHeight();
        const float w = lh + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(txt).x + ImGui::GetStyle().FramePadding.x;
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < w + ImGui::GetFontSize()) ImGui::NewLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
        ImGui::AlignTextToFramePadding();
        ImGui::BeginGroup();
        ui::miniRing(f, lh * 0.85f, "##ctxring");
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TextColored(f > 0.7f ? pal.warn : pal.dim, "%s", txt);
        ImGui::EndGroup();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s / %s jetons dans la fenêtre de contexte.\nTapez /compact pour la résumer.",
                              util::humanTokens(s.stats.contextTokens).c_str(), util::humanTokens(s.stats.contextWindow).c_str());
    }

    // Pièces jointes
    if (!oc.attachments.empty()) {
        for (size_t i = 0; i < oc.attachments.size(); ++i) {
            ImGui::PushID((int)i);
            if (i > 0) ui::sameLineIfFits(ImGui::CalcTextSize(oc.attachments[i].name.c_str()).x + ImGui::GetFontSize() * 2);
            ui::badge(oc.attachments[i].name.c_str(), pal.link);
            ImGui::SameLine(0, 2);
            if (ImGui::SmallButton("×")) {
                oc.attachments.erase(oc.attachments.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }
    oc.quickBarHeight = ImGui::GetCursorPosY() - y0;
}

void App::drawSlashPopup(OpenConversation& oc, const std::string& prefix) {
    auto items = slashCandidates(*oc.session, prefix);
    if (items.empty()) return;
    int n = std::min((int)items.size(), 8);
    oc.slashSelection = std::clamp(oc.slashSelection, 0, (int)items.size() - 1);

    // Navigation clavier : on prend la main sur ces touches avant l'éditeur.
    ImGuiID owner = ImGui::GetID("##slashpopup");
    for (ImGuiKey k : {ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Tab, ImGuiKey_Enter, ImGuiKey_KeypadEnter})
        ImGui::SetKeyOwner(k, owner);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, ImGuiInputFlags_Repeat, owner))
        oc.slashSelection = (oc.slashSelection + 1) % (int)items.size();
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, ImGuiInputFlags_Repeat, owner))
        oc.slashSelection = (oc.slashSelection + (int)items.size() - 1) % (int)items.size();
    bool accept = ImGui::IsKeyPressed(ImGuiKey_Tab, 0, owner) || ImGui::IsKeyPressed(ImGuiKey_Enter, 0, owner) ||
                  ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, 0, owner);

    // Fenêtre flottante juste au-dessus de l'éditeur
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float h = n * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y * 2;
    ImGui::SetNextWindowPos(ImVec2(pos.x, pos.y - h - 4));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, h));
    ImGui::Begin("##slash", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_Tooltip);
    int first = std::clamp(oc.slashSelection - n + 1, 0, std::max(0, (int)items.size() - n));
    for (int i = first; i < first + n && i < (int)items.size(); ++i) {
        bool sel = i == oc.slashSelection;
        if (ImGui::Selectable(("/" + items[i].name).c_str(), sel, 0, ImVec2(ImGui::GetFontSize() * 11, 0))) {
            oc.slashSelection = i;
            accept = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", util::truncateUtf8(items[i].description, 70).c_str());
    }
    ImGui::End();
    if (accept) {
        oc.draft = "/" + items[oc.slashSelection].name + " ";
        oc.replaceDraft = oc.draft;
        oc.hasReplace = true;
        oc.focusEditor = true;
        oc.slashSelection = 0;
    }
}

void App::drawPromptEditor(OpenConversation& oc) {
    auto& s = *oc.session;
    const auto& pal = ui::palette();
    ImVec2 zoneMin = ImGui::GetCursorScreenPos();
    drawQuickBar(oc);

    // Autocomplétion des commandes « / » (premier mot, sans espace)
    bool slash = !oc.draft.empty() && oc.draft[0] == '/' && oc.draft.find_first_of(" \n") == std::string::npos;
    if (slash) drawSlashPopup(oc, oc.draft.substr(1));

    // Historique des messages : Ctrl+Haut / Ctrl+Bas
    bool editorFocused = oc.focusEditor;
    auto& hist = settings_.promptHistory;
    ImGuiID editorId = ImGui::GetID("##editor");
    if (ImGui::GetActiveID() == editorId && !hist.empty()) {
        int dir = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && ImGui::GetIO().KeyCtrl) dir = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && ImGui::GetIO().KeyCtrl) dir = 1;
        if (dir) {
            int idx = oc.historyIndex < 0 ? (int)hist.size() : oc.historyIndex;
            idx = std::clamp(idx + dir, 0, (int)hist.size());
            oc.historyIndex = idx;
            oc.replaceDraft = idx < (int)hist.size() ? hist[idx] : "";
            oc.draft = oc.replaceDraft;
            oc.hasReplace = true;
        }
    }

    // Boutons Envoyer / Arrêter compacts (icônes) à droite de l'éditeur
    const float btn = ImGui::GetFrameHeight() * 1.45f;
    ImVec2 size(ImGui::GetContentRegionAvail().x - btn - ImGui::GetStyle().ItemSpacing.x,
                std::max(ImGui::GetFrameHeight() * 2, ImGui::GetContentRegionAvail().y - 4));
    const float editorTop = ImGui::GetCursorPosY();
    if (oc.focusEditor) {
        ImGui::SetKeyboardFocusHere();
        oc.focusEditor = false;
        oc.cursorToEnd = true;
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_EnterReturnsTrue |
                                ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_AllowTabInput;
    if (settings_.enterSends) flags |= ImGuiInputTextFlags_CtrlEnterForNewLine;
    ImGui::PushFont(fontUi, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    bool submitted = ImGui::InputTextMultiline("##editor", &oc.draft, size, flags, editorCallback, &oc);
    bool editorActive = ImGui::IsItemActive();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (!editorActive) oc.hasReplace = false;  // le texte inactif est relu depuis oc.draft
    (void)editorFocused;

    // Texte d'aide quand l'éditeur est vide
    if (oc.draft.empty()) {
        ImVec2 p = ImGui::GetItemRectMin();
        ImGuiStyle& st = ImGui::GetStyle();
        std::string hint = settings_.enterSends
                               ? "Écrivez à Claude…   Entrée : envoyer · Maj+Entrée : nouvelle ligne · / : commandes"
                               : "Écrivez à Claude…   Ctrl+Entrée : envoyer · / : commandes";
        // Étroit : seulement l'invitation (les raccourcis sont dans l'infobulle des boutons et l'aide F1)
        if (ImGui::CalcTextSize(hint.c_str()).x > ImGui::GetItemRectSize().x - st.FramePadding.x * 2) hint = "Écrivez à Claude…";
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + st.FramePadding.x, p.y + st.FramePadding.y), ui::col(pal.dim), hint.c_str());
    }
    if (submitted && !slash) sendDraft(oc);
    else if (submitted) oc.focusEditor = true;

    ImGui::SameLine();
    ImGui::BeginGroup();
    {
        bool busy = s.state == claude::SessionState::Busy;
        bool canSend = !(oc.draft.empty() && oc.attachments.empty());
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Bouton carré à icône : fond orange s'il est actif, discret sinon
        auto iconButton = [&](const char* id, ui::Icon icon, bool primary, bool enabled) -> bool {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::BeginDisabled(!enabled);
            bool clicked = ImGui::InvisibleButton(id, ImVec2(btn, btn));
            bool hov = ImGui::IsItemHovered();
            ImGui::EndDisabled();
            ImVec4 bg = primary && enabled ? pal.accent : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
            if (hov && enabled) bg = primary ? ImVec4(std::min(1.0f, bg.x * 1.12f), std::min(1.0f, bg.y * 1.12f), std::min(1.0f, bg.z * 1.12f), 1)
                                             : ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered);
            dl->AddRectFilled(p, ImVec2(p.x + btn, p.y + btn), ui::col(bg), ImGui::GetStyle().FrameRounding + 2);
            float is = btn * 0.46f;
            ImU32 ic = primary && enabled ? ui::col(ImVec4(1, 1, 1, 1)) : ui::col(enabled ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : pal.dim);
            ui::drawIcon(dl, icon, ImVec2(p.x + (btn - is) * 0.5f, p.y + (btn - is) * 0.5f), is, ic);
            return clicked && enabled;
        };
        // Alignés en bas de l'éditeur : Arrêter (pendant un tour) au-dessus d'Envoyer
        float yBottom = editorTop + size.y - btn;
        if (busy) {
            ImGui::SetCursorPosY(std::max(editorTop, yBottom - btn - ImGui::GetStyle().ItemSpacing.y));
            if (iconButton("##stop", ui::Icon::Stop, false, true)) s.interrupt();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Interrompre le tour en cours (Échap)");
        }
        ImGui::SetCursorPosY(yBottom);
        if (iconButton("##send", ui::Icon::Send, true, canSend)) sendDraft(oc);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(busy ? "Envoyer (le message attendra la fin du tour)" : "Envoyer");
    }
    ImGui::EndGroup();
    ImVec2 zoneMax = ImGui::GetItemRectMax();
    editorRect_[0] = zoneMin.x;
    editorRect_[1] = zoneMin.y;
    editorRect_[2] = zoneMax.x;
    editorRect_[3] = zoneMax.y;
    editorRectFrame_ = ImGui::GetFrameCount();
}

}  // namespace app
