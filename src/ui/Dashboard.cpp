// Page secondaire : tableau de bord des conversations en cours.
// Une carte par conversation ouverte (état, progression, dernières actions,
// autorisations en un clic, réponse rapide) + un fil d'activité global.
// S'affiche dans la fenêtre principale (Ctrl+2) ou dans sa propre fenêtre (Ctrl+Maj+D).
#include <algorithm>
#include <cfloat>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/App.h"
#include "ui/Anim.h"
#include "ui/Theme.h"
#include "util/Strings.h"

namespace app {

using claude::EntryKind;
using claude::SessionState;
using json = nlohmann::json;

namespace {

enum class CardState { Attention, Working, Starting, Ready, Sleeping, Failed };

CardState cardState(const claude::ClaudeSession& s) {
    if (!s.permissions.empty()) return CardState::Attention;
    if (s.state == SessionState::Busy) return CardState::Working;
    if (s.state == SessionState::Starting) return CardState::Starting;
    if (!s.entries.empty() && s.entries.back().kind == EntryKind::Error) return CardState::Failed;
    if (s.state == SessionState::Idle) return CardState::Ready;
    return CardState::Sleeping;
}

ImVec4 stateColor(CardState st) {
    const auto& p = ui::palette();
    switch (st) {
        case CardState::Attention: return p.warn;
        case CardState::Working: return p.accent;
        case CardState::Starting: return p.link;
        case CardState::Ready: return p.ok;
        case CardState::Failed: return p.err;
        case CardState::Sleeping: return p.dim;
    }
    return p.dim;
}

const char* stateText(CardState st) {
    switch (st) {
        case CardState::Attention: return "Attend votre décision";
        case CardState::Working: return "Claude travaille";
        case CardState::Starting: return "Démarrage";
        case CardState::Ready: return "Prêt — à vous";
        case CardState::Failed: return "Erreur";
        case CardState::Sleeping: return "En veille";
    }
    return "";
}

int statePriority(CardState st) {
    switch (st) {
        case CardState::Attention: return 0;
        case CardState::Failed: return 1;
        case CardState::Working: return 2;
        case CardState::Starting: return 3;
        case CardState::Ready: return 4;
        case CardState::Sleeping: return 5;
    }
    return 6;
}

// Dernière liste de tâches de Claude (TodoWrite) : terminées / total / tâche en cours.
struct TodoProgress {
    int done = 0, total = 0;
    std::string current;
};
TodoProgress todoProgress(const claude::ClaudeSession& s) {
    TodoProgress tp;
    for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it) {
        if (it->kind != EntryKind::ToolUse || it->toolName != "TodoWrite" || !it->parentToolUseId.empty()) continue;
        json in = json::parse(it->toolInput, nullptr, false);
        if (!in.is_object() || !in.contains("todos")) break;
        for (auto& t : in["todos"]) {
            ++tp.total;
            std::string st = t.value("status", "");
            if (st == "completed") ++tp.done;
            if (st == "in_progress" && tp.current.empty()) tp.current = t.value("activeForm", t.value("content", ""));
        }
        break;
    }
    return tp;
}

std::string permissionSummary(const claude::PermissionRequest& p) {
    auto str = [&](const char* k) {
        return p.input.is_object() && p.input.contains(k) && p.input[k].is_string() ? p.input[k].get<std::string>()
                                                                                   : std::string();
    };
    if (p.toolName == "Bash" || p.toolName == "PowerShell") return str("command");
    if (!str("file_path").empty()) return str("file_path");
    if (!str("url").empty()) return str("url");
    return p.description;
}

std::string clock(int64_t t) {
    std::string iso = util::isoFromUnix(t);
    return iso.size() >= 19 ? iso.substr(11, 8) : iso;
}

}  // namespace

// ---------------------------------------------------------------------------

void App::drawPageSwitch() {
    const auto& pal = ui::palette();
    int attention = 0, working = 0;
    for (auto& oc : open_) {
        if (!oc->session->permissions.empty()) ++attention;
        else if (oc->session->state == SessionState::Busy) ++working;
    }
    ImGui::Dummy(ImVec2(ImGui::GetFontSize(), 0));
    auto pageButton = [&](const char* label, bool selected) {
        ImGui::PushStyleColor(ImGuiCol_Button, selected ? pal.accentSoft : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, selected ? pal.accent : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        bool r = ImGui::SmallButton(label);
        ImGui::PopStyleColor(2);
        return r;
    };
    if (pageButton("Cockpit", page_ == 0 && !dash_)) page_ = 0;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Page principale : conversations, espace de travail, usage, configuration (Ctrl+1)");
    std::string lbl = "Tableau de bord";
    if (working) lbl += "  ● " + std::to_string(working);
    if (attention) lbl += "  ⚠ " + std::to_string(attention);
    lbl += "###dashpage";
    if (pageButton(lbl.c_str(), page_ == 1 || dash_ != nullptr)) {
        if (dash_) SDL_RaiseWindow(dash_->window);
        else page_ = 1;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Toutes les conversations en cours d'un coup d'œil (Ctrl+2)\n"
                          "● = Claude travaille   ⚠ = attend votre décision");
    if (pageButton(dash_ ? "⧉ Rattacher" : "⧉", false)) {
        if (dash_) closeDashboardWindow();
        else openDashboardWindow();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(dash_ ? "Fermer la fenêtre séparée du tableau de bord (Ctrl+Maj+D)"
                                : "Ouvrir le tableau de bord dans une fenêtre séparée (Ctrl+Maj+D)\n"
                                  "— pratique sur un deuxième écran.");
}

void App::drawDashboard(bool detached) {
    const auto& pal = ui::palette();
    float fs = ImGui::GetFontSize();

    // Raccourcis propres à la fenêtre détachée
    if (detached) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_D, ImGuiInputFlags_RouteGlobal)) dash_->closeRequested = true;
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_1, ImGuiInputFlags_RouteGlobal)) SDL_RaiseWindow(window_);
    }

    // --- En-tête : compteurs + usage ---
    int nAttention = 0, nWorking = 0, nReady = 0;
    for (auto& oc : open_) {
        CardState st = cardState(*oc->session);
        if (st == CardState::Attention) ++nAttention;
        else if (st == CardState::Working || st == CardState::Starting) ++nWorking;
        else if (st == CardState::Ready) ++nReady;
    }
    float rowY = ImGui::GetCursorPosY();
    ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::TextColored(pal.accent, "Conversations en cours");
    float titleH = ImGui::GetItemRectSize().y;
    ImGui::PopFont();
    float midY = rowY + (titleH - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::SameLine(0, fs);
    ImGui::SetCursorPosY(midY);
    ui::badge((std::to_string(nWorking) + " en cours").c_str(), pal.accent);
    ImGui::SameLine();
    ImGui::SetCursorPosY(midY);
    ui::badge((std::to_string(nAttention) + " attendent votre décision").c_str(), nAttention ? pal.warn : pal.dim);
    ImGui::SameLine();
    ImGui::SetCursorPosY(midY);
    ui::badge((std::to_string(nReady) + " prêtes").c_str(), pal.ok);

    // Usage compact à droite
    {
        float barW = fs * 7.5f;
        float totalW = 0;
        std::vector<const claude::RateWindow*> ws;
        for (auto& w : usage_.windows)
            if (w.name == "five_hour" || w.name == "seven_day") ws.push_back(&w), totalW += barW + fs;
        if (!ws.empty()) {
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - totalW));
            for (auto* w : ws) {
                char buf[48];
                std::snprintf(buf, sizeof buf, "%s : %d %%", w->name == "five_hour" ? "5 h" : "7 jours",
                              (int)(w->utilization * 100 + 0.5));
                ImGui::SetCursorPosY(midY);
                ui::usageBar((float)w->utilization, buf, ImGui::GetTextLineHeight(), barW, w->name.c_str());
                if (ImGui::IsItemHovered() && w->resetsAt)
                    ImGui::SetTooltip("%s — réinitialisation dans %s", claude::windowLabel(w->name).c_str(),
                                      util::humanDuration(w->resetsAt - util::nowUnix()).c_str());
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
    }

    // --- Barre d'outils ---
    if (ImGui::Button("+ Nouvelle conversation")) {
        OpenConversation* oc = newConversation(nullptr);
        oc->focusDashDraft = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Créée dans le dossier sélectionné de l'espace de travail (ou « Divers »)");
    ImGui::SameLine();
    ImGui::TextDisabled("Afficher :");
    ImGui::SameLine();
    ImGui::RadioButton("toutes", &dashFilter_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("actives", &dashFilter_, 1);
    ImGui::SameLine();
    ImGui::RadioButton("à traiter", &dashFilter_, 2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Conversations qui attendent une autorisation ou une réponse de votre part");
    ImGui::SameLine();
    ImGui::BeginDisabled(nWorking == 0);
    if (ImGui::Button("Tout interrompre"))
        for (auto& oc : open_)
            if (oc->session->state == SessionState::Busy) oc->session->interrupt();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (detached) {
        if (ImGui::Button("Rattacher à la fenêtre principale")) dash_->closeRequested = true;
    } else {
        if (ImGui::Button("Ouvrir dans une fenêtre séparée")) openDashboardWindow();
        ImGui::SameLine();
        if (ImGui::Button("Retour au cockpit")) page_ = 0;
    }
    ImGui::Separator();

    // --- Disposition : cartes + fil d'activité ---
    ImVec2 avail = ImGui::GetContentRegionAvail();
    bool side = avail.x > fs * 52;
    float feedW = side ? std::clamp(avail.x * 0.28f, fs * 18, fs * 30) : avail.x;
    float feedH = side ? avail.y : std::max(fs * 8, avail.y * 0.3f);
    ImVec2 cardsSize = side ? ImVec2(avail.x - feedW - ImGui::GetStyle().ItemSpacing.x, avail.y)
                            : ImVec2(avail.x, avail.y - feedH - ImGui::GetStyle().ItemSpacing.y);

    ImGui::BeginChild("##cards", cardsSize);
    if (open_.empty()) {
        ImGui::Dummy(ImVec2(0, fs));
        ImGui::TextColored(pal.dim, "Aucune conversation ouverte.");
        ImGui::TextWrapped("Le tableau de bord montre toutes les conversations ouvertes dans des onglets. Ouvrez-en "
                           "depuis l'espace de travail (double-clic), ou créez-en une avec le bouton ci-dessus : vous "
                           "pourrez lancer plusieurs tâches en parallèle et les suivre d'ici.");
    }
    // Tri : ce qui demande votre attention d'abord.
    std::vector<int> order;
    for (int i = 0; i < (int)open_.size(); ++i) {
        CardState st = cardState(*open_[i]->session);
        if (dashFilter_ == 1 && st != CardState::Working && st != CardState::Starting && st != CardState::Attention) continue;
        if (dashFilter_ == 2 && st != CardState::Attention && st != CardState::Ready && st != CardState::Failed) continue;
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return statePriority(cardState(*open_[a]->session)) < statePriority(cardState(*open_[b]->session));
    });
    if (!open_.empty() && order.empty()) ImGui::TextDisabled("Rien à afficher avec ce filtre.");
    float minCardW = fs * 23;
    float innerW = ImGui::GetContentRegionAvail().x;
    int cols = std::max(1, (int)((innerW + ImGui::GetStyle().ItemSpacing.x) / (minCardW + ImGui::GetStyle().ItemSpacing.x)));
    cols = std::min(cols, std::max(1, (int)order.size()));
    float cardW = (innerW - ImGui::GetStyle().ItemSpacing.x * (cols - 1)) / cols;
    float cardH = std::max(fs * 17, std::min(fs * 24, (cardsSize.y - ImGui::GetStyle().ItemSpacing.y) / 2));
    for (size_t k = 0; k < order.size(); ++k) {
        if (k % cols != 0) ImGui::SameLine();
        drawDashboardCard(*open_[order[k]], order[k], cardW, cardH);
    }
    ImGui::EndChild();

    if (side) ImGui::SameLine();
    ImGui::BeginChild("##feed", ImVec2(feedW, feedH), ImGuiChildFlags_Borders);
    drawActivityFeed();
    ImGui::EndChild();
}

void App::drawDashboardCard(OpenConversation& oc, int index, float width, float height) {
    auto& s = *oc.session;
    const auto& pal = ui::palette();
    CardState st = cardState(s);
    ImVec4 sc = stateColor(st);
    bool strong = st == CardState::Attention || st == CardState::Working || st == CardState::Failed;

    ImGui::PushID(s.localId.c_str());
    // Halo lumineux qui respire autour des cartes qui travaillent ou vous attendent
    float glow = 0;
    if (strong && settings_.glowEffects && settings_.animations)
        glow = ui::anim::pulse(st == CardState::Attention ? 1.1f : 0.6f, (float)index);
    {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1(p0.x + width, p0.y + height);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (int k = 1; k <= 5 && glow > 0; ++k) {
            float e = k * 2.0f;
            dl->AddRect(ImVec2(p0.x - e, p0.y - e), ImVec2(p1.x + e, p1.y + e), ui::col(sc, (0.16f - k * 0.028f) * (0.35f + 0.65f * glow)),
                        8.0f + e, 0, 2.0f);
        }
    }
    ImVec4 border = sc;
    if (glow > 0) border.w = 0.55f + 0.45f * glow;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.cardBg);
    ImGui::PushStyleColor(ImGuiCol_Border, strong ? border : ImGui::GetStyleColorVec4(ImGuiCol_Border));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, strong ? 2.0f : 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::BeginChild("##card", ImVec2(width, height), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    // --- Ligne 1 : état + titre + actions ---
    float btnW = ImGui::CalcTextSize("Ouvrir").x + ImGui::CalcTextSize("Stop").x + ImGui::GetStyle().FramePadding.x * 4 + 16;
    ImGui::TextColored(sc, "●");
    ImGui::SameLine();
    ImGui::PushFont(fontBold, 0.0f);
    float titleW = ImGui::GetContentRegionAvail().x - btnW;
    std::string title = s.title;
    while (title.size() > 4 && ImGui::CalcTextSize(title.c_str()).x > titleW) title = util::truncateUtf8(title, title.size() - 4);
    ImGui::TextUnformatted(title.c_str());
    ImGui::PopFont();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(double-clic : ouvrir dans le cockpit)", s.title.c_str());
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) focusConversation(index);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - btnW + 8));
    if (ImGui::SmallButton("Ouvrir")) focusConversation(index);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Afficher dans le cockpit (fenêtre principale)");
    ImGui::SameLine();
    ImGui::BeginDisabled(s.state != SessionState::Busy);
    if (ImGui::SmallButton("Stop")) s.interrupt();
    ImGui::EndDisabled();

    // --- Ligne 2 : état détaillé ---
    std::string stLine = stateText(st);
    if (st == CardState::Working || st == CardState::Starting)
        stLine += " · " + util::humanDuration((int64_t)(now_ - s.turnStartTime));
    ImGui::TextColored(sc, "%s", stLine.c_str());
    if (st == CardState::Working && !s.statusText.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("— %s", util::truncateUtf8(s.statusText, 60).c_str());
    }
    // --- Ligne 3 : emplacement + modèle + contexte + coût ---
    {
        ws::Folder* f = folderOf(oc);
        std::string info = f ? f->displayPath() : "Non classée";
        std::string model = !s.activeModel.empty() ? s.activeModel : s.config.model.empty() ? "défaut" : s.config.model;
        info += "  ·  " + model;
        if (s.stats.contextWindow > 0)
            info += "  ·  contexte " + std::to_string((int)(100.0 * s.stats.contextTokens / s.stats.contextWindow)) + " %";
        info += "  ·  " + util::formatUsd(s.stats.costUsd);
        ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
        ImGui::TextUnformatted(util::truncateUtf8(info, 140).c_str());
        ImGui::PopStyleColor();
    }

    // --- Zone basse réservée : autorisation ou réponse rapide ---
    float lineH = ImGui::GetFrameHeightWithSpacing();
    float bottomH = !s.permissions.empty() ? lineH * 3.2f : lineH * 2.3f;

    // --- Corps défilant : progression, dernières actions, dernière réponse ---
    ImGui::BeginChild("##body", ImVec2(0, -bottomH), ImGuiChildFlags_None);
    TodoProgress tp = todoProgress(s);
    if (tp.total > 0) {
        std::string ov = std::to_string(tp.done) + " / " + std::to_string(tp.total) + " tâches";
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, tp.done == tp.total ? pal.ok : pal.accent);
        ImGui::ProgressBar((float)tp.done / tp.total, ImVec2(-1, ImGui::GetTextLineHeight()), ov.c_str());
        ImGui::PopStyleColor();
        if (!tp.current.empty()) {
            ImGui::TextColored(pal.warn, "◐");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", tp.current.c_str());
        }
    }
    // Dernières actions (outils)
    std::vector<const claude::Entry*> tools;
    for (auto it = s.entries.rbegin(); it != s.entries.rend() && tools.size() < 4; ++it) {
        if (it->kind == EntryKind::User) break;  // seulement le tour en cours / dernier tour
        if (it->kind == EntryKind::ToolUse && it->parentToolUseId.empty()) tools.push_back(&*it);
    }
    if (!tools.empty()) {
        ImGui::TextDisabled("Dernières actions");
        for (auto it = tools.rbegin(); it != tools.rend(); ++it) {
            const claude::Entry* e = *it;
            bool running = !e->toolDone && s.state == SessionState::Busy;
            ImGui::TextColored(running ? pal.warn : e->toolError ? pal.err : e->toolDone ? pal.ok : pal.dim, "%s",
                               running ? "◌" : e->toolError ? "✗" : e->toolDone ? "✓" : "·");
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
            ImGui::TextUnformatted(util::truncateUtf8(e->text, 90).c_str());
            ImGui::PopStyleColor();
        }
    }
    // Dernier message (réponse de Claude, ou le vôtre si Claude n'a pas encore répondu)
    const claude::Entry* last = nullptr;
    for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it) {
        if (!it->parentToolUseId.empty()) continue;
        if (it->kind == EntryKind::Assistant && !it->text.empty()) {
            last = &*it;
            break;
        }
        if (it->kind == EntryKind::User) {
            last = &*it;
            break;
        }
    }
    if (last) {
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::TextColored(last->kind == EntryKind::User ? pal.accent : pal.dim, "%s",
                           last->kind == EntryKind::User ? "Votre dernier message" : (last->streaming ? "Claude écrit…" : "Dernière réponse"));
        // Extrait lisible : sans les clôtures de blocs de code ni les marques de titre.
        std::string txt;
        for (auto& l : util::splitLines(last->text)) {
            std::string t = util::trim(l);
            if (util::startsWith(t, "```")) continue;
            while (!t.empty() && t[0] == '#') t.erase(0, 1);
            if (!txt.empty()) txt += '\n';
            txt += util::trim(t).empty() ? std::string() : (l.size() && l[0] == '#' ? util::trim(t) : l);
        }
        // Pendant la rédaction on montre la fin, sinon le début.
        if (last->streaming && txt.size() > 600) {
            size_t cut = txt.size() - 600;
            while (cut < txt.size() && ((unsigned char)txt[cut] & 0xC0) == 0x80) ++cut;
            txt = "…" + txt.substr(cut);
        } else {
            txt = util::truncateUtf8(txt, 700);
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(txt.c_str());
        ImGui::PopTextWrapPos();
        if (last->streaming) ImGui::SetScrollHereY(1.0f);
    } else if (s.entries.empty()) {
        ImGui::TextDisabled("Conversation vide : écrivez votre premier message ci-dessous.");
    }
    ImGui::EndChild();

    // --- Bas de carte ---
    ImGui::Separator();
    if (!s.permissions.empty()) {
        claude::PermissionRequest& p = s.permissions.front();
        if (p.isQuestion || p.toolName == "ExitPlanMode") {
            ImGui::TextColored(pal.warn, "%s", p.isQuestion ? "Claude vous pose une question." : "Claude propose un plan à valider.");
            if (ImGui::Button("Répondre dans le cockpit")) focusConversation(index);
        } else {
            ImGui::TextColored(pal.warn, "Autoriser %s ?", p.toolName.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.codeText);
            ImGui::PushFont(fontMono, ImGui::GetStyle().FontSizeBase * 0.84f);
            std::string sum = util::firstLine(permissionSummary(p), 70);
            ImGui::TextUnformatted(sum.c_str());
            ImGui::PopFont();
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.input.dump(2, ' ', false, json::error_handler_t::replace).c_str());
            if (ImGui::Button("Autoriser")) s.respondPermission(p.requestId, true);
            ImGui::SameLine();
            if (p.suggestions.is_array() && !p.suggestions.empty()) {
                if (ImGui::Button("Toujours")) {
                    for (auto& sg : p.suggestions)
                        if (sg.value("type", "") == "setMode") s.config.permissionMode = sg.value("mode", s.config.permissionMode);
                    s.respondPermission(p.requestId, true, {}, nullptr, true);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Autoriser ce type d'action pour toute la session");
                ImGui::SameLine();
            }
            if (ImGui::Button("Refuser")) s.respondPermission(p.requestId, false, oc.denyReason);
            ImGui::SameLine();
            if (ImGui::SmallButton("Détails…")) focusConversation(index);
        }
    } else {
        float sendW = ImGui::CalcTextSize("Envoyer").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - sendW - ImGui::GetStyle().ItemSpacing.x);
        if (oc.focusDashDraft) {
            ImGui::SetKeyboardFocusHere();
            oc.focusDashDraft = false;
        }
        const char* hint = s.state == SessionState::Busy ? "Ajouter une consigne (mise en file)…" : "Réponse rapide… (Entrée)";
        bool enter = ImGui::InputTextWithHint("##quick", hint, &oc.dashDraft, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        bool click = ImGui::Button("Envoyer");
        if ((enter || click) && !util::trim(oc.dashDraft).empty()) {
            sendText(oc, oc.dashDraft);
            oc.dashDraft.clear();
            oc.focusDashDraft = enter;
        }
        // Rappel du dossier de travail quand la conversation n'a pas encore démarré
        if (s.entries.empty()) ImGui::TextDisabled("%s", util::truncateUtf8(s.cwd, 80).c_str());
        else if (st == CardState::Ready && !oc.meta.recap.empty()) ImGui::TextDisabled("Récap disponible (Inspecteur)");
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void App::drawActivityFeed() {
    const auto& pal = ui::palette();
    ImGui::PushFont(fontBold, 0.0f);
    ImGui::TextUnformatted("Fil d'activité");
    ImGui::PopFont();
    ImGui::SameLine();
    static bool onlyImportant = false;
    ImGui::Checkbox("important", &onlyImportant);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Masquer les appels d'outils");
    ImGui::SameLine();
    if (ImGui::SmallButton("Effacer")) activity_.clear();
    ImGui::Separator();
    ImGui::BeginChild("##events");
    if (activity_.empty())
        ImGui::TextWrapped("Les actions de Claude dans toutes les conversations ouvertes s'afficheront ici : "
                           "outils utilisés, demandes d'autorisation, fins de tour, erreurs.");
    int n = 0;
    for (auto& ev : activity_) {
        if (onlyImportant && ev.kind == ActivityKind::Tool) continue;
        if (++n > 250) break;
        ImVec4 c = ev.kind == ActivityKind::Attention ? pal.warn
                   : ev.kind == ActivityKind::Done    ? pal.ok
                   : ev.kind == ActivityKind::Error   ? pal.err
                   : ev.kind == ActivityKind::Tool    ? pal.dim
                                                      : pal.link;
        const char* icon = ev.kind == ActivityKind::Attention ? "⚠"
                           : ev.kind == ActivityKind::Done    ? "✓"
                           : ev.kind == ActivityKind::Error   ? "✗"
                           : ev.kind == ActivityKind::Tool    ? "·"
                                                              : "›";
        ImGui::PushID(n);
        ImGui::TextDisabled("%s", clock(ev.time).c_str());
        ImGui::SameLine();
        ImGui::TextColored(c, "%s", icon);
        ImGui::SameLine();
        // Titre cliquable : ouvre la conversation
        std::string t = util::truncateUtf8(ev.title, 28);
        ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
        if (ImGui::Selectable(t.c_str(), false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(t.c_str()))) {
            for (int i = 0; i < (int)open_.size(); ++i)
                if (open_[i]->session->localId == ev.convId) focusConversation(i);
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ev.kind == ActivityKind::Tool ? pal.dim : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(ev.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

}  // namespace app
