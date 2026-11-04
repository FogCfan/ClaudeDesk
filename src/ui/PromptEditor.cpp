// Éditeur de message : texte multi-ligne avec les raccourcis habituels
// (Ctrl+C/V/X/Z/Y/A, Ctrl+flèches, Maj+sélection, souris, double-clic…),
// barre rapide (modèle, effort, mode, pièces jointes) et autocomplétion « / ».
#include <algorithm>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "app/App.h"
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

void App::drawQuickBar(OpenConversation& oc) {
    auto& s = *oc.session;
    const auto& pal = ui::palette();
    float comboW = ImGui::GetFontSize() * 8.5f;

    // Modèle (changé à chaud)
    {
        std::string current = s.config.model.empty() ? "Modèle par défaut" : s.config.model;
        for (auto& m : s.models)
            if (m.value == s.config.model) current = m.displayName;
        for (auto& c : claude::modelChoices())
            if (s.models.empty() && c.value == s.config.model) current = c.label;
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##model", current.c_str())) {
            if (!s.models.empty()) {
                for (auto& m : s.models) {
                    std::string v = m.value == "default" ? "" : m.value;
                    if (ImGui::Selectable(m.displayName.c_str(), v == s.config.model)) s.setModel(v);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.description.c_str());
                }
            } else {
                for (auto& c : claude::modelChoices()) {
                    if (ImGui::Selectable(c.label, c.value == s.config.model)) s.setModel(c.value);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.help);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Modèle (appliqué immédiatement)");
    }
    ImGui::SameLine();
    // Effort (redémarrage transparent avec --resume)
    {
        std::string current = "Effort : défaut";
        for (auto& c : claude::effortChoices())
            if (c.value == s.config.effort && !s.config.effort.empty()) current = std::string("Effort : ") + c.label;
        ImGui::SetNextItemWidth(comboW);
        if (ImGui::BeginCombo("##effort", current.c_str())) {
            for (auto& c : claude::effortChoices()) {
                if (ImGui::Selectable(c.label, c.value == s.config.effort)) {
                    claude::SessionConfig cfg = s.config;
                    cfg.effort = c.value;
                    s.applyConfig(cfg);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.help);
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Effort de réflexion. Le changer relance la session en arrière-plan\n"
                              "(l'historique est conservé grâce à --resume).");
    }
    ImGui::SameLine();
    // Mode de permission (changé à chaud)
    {
        std::string current = s.config.permissionMode;
        for (auto& c : claude::permissionModeChoices())
            if (c.value == s.config.permissionMode) current = c.label;
        bool danger = s.config.permissionMode == "bypassPermissions";
        if (danger) ImGui::PushStyleColor(ImGuiCol_FrameBg, ui::palette().err);
        ImGui::SetNextItemWidth(comboW);
        static std::string pendingBypass;
        if (ImGui::BeginCombo("##mode", ("Mode : " + current).c_str())) {
            for (auto& c : claude::permissionModeChoices()) {
                if (ImGui::Selectable(c.label, c.value == s.config.permissionMode)) {
                    if (std::string(c.value) == "bypassPermissions" && settings_.confirmBypass) pendingBypass = s.localId;
                    else s.setPermissionMode(c.value);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.help);
            }
            ImGui::EndCombo();
        }
        if (danger) ImGui::PopStyleColor();
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
    }
    ImGui::SameLine();
    if (ImGui::Button("Joindre…")) {
        std::string id = s.localId;
        openFileDialog([this, id](const std::vector<std::string>& files) {
            for (auto& o : open_)
                if (o->session->localId == id)
                    for (auto& f : files) attachFile(*o, f);
        });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Joindre des images ou référencer des fichiers (glisser-déposer marche aussi)");
    ImGui::SameLine();
    ImGui::Checkbox("Contexte cowork", &oc.injectContext);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted("Injecte dans le prompt système les notes du projet, les synthèses et les récaps\n"
                               "des conversations précédentes du dossier (au démarrage de la session).");
        if (s.processRunning() || !s.claudeSessionId.empty())
            ImGui::TextColored(pal.warn, "Session déjà démarrée : s'appliquera à une nouvelle session.");
        ImGui::EndTooltip();
    }

    // État compact à droite : contexte utilisé
    if (s.stats.contextWindow > 0) {
        float f = (float)s.stats.contextTokens / (float)s.stats.contextWindow;
        std::string txt = "Contexte " + std::to_string((int)(f * 100)) + " %";
        float w = ImGui::GetFontSize() * 7;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - w));
        ui::usageBar(f, txt.c_str(), ImGui::GetFrameHeight(), -1, "##ctxbar");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s / %s jetons dans la fenêtre de contexte.\nTapez /compact pour la résumer.",
                              util::humanTokens(s.stats.contextTokens).c_str(), util::humanTokens(s.stats.contextWindow).c_str());
    }

    // Pièces jointes
    if (!oc.attachments.empty()) {
        for (size_t i = 0; i < oc.attachments.size(); ++i) {
            ImGui::PushID((int)i);
            ui::badge(oc.attachments[i].name.c_str(), pal.link);
            ImGui::SameLine(0, 2);
            if (ImGui::SmallButton("x")) {
                oc.attachments.erase(oc.attachments.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            ImGui::PopID();
        }
        ImGui::NewLine();
    }
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

    float sendW = ImGui::GetFontSize() * 6.5f;
    ImVec2 size(ImGui::GetContentRegionAvail().x - sendW - ImGui::GetStyle().ItemSpacing.x,
                std::max(ImGui::GetFrameHeight() * 2, ImGui::GetContentRegionAvail().y - 4));
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
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + st.FramePadding.x, p.y + st.FramePadding.y), ui::col(pal.dim), hint.c_str());
    }
    if (submitted && !slash) sendDraft(oc);
    else if (submitted) oc.focusEditor = true;

    ImGui::SameLine();
    ImGui::BeginGroup();
    bool busy = s.state == claude::SessionState::Busy;
    float bh = (size.y - ImGui::GetStyle().ItemSpacing.y) * (busy ? 0.5f : 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(pal.accent.x * 1.1f, pal.accent.y * 1.1f, pal.accent.z * 1.1f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::BeginDisabled(oc.draft.empty() && oc.attachments.empty());
    if (ImGui::Button(busy ? "Envoyer\n(en file)" : "Envoyer", ImVec2(sendW, bh))) sendDraft(oc);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    if (busy) {
        if (ImGui::Button("Stop", ImVec2(sendW, bh))) s.interrupt();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Interrompre le tour en cours (Échap)");
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
