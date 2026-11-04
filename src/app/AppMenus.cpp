// Barre de menus, barre d'état, fenêtres d'aide et journal.
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "app/App.h"
#include "ui/Theme.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace app {

void App::drawMenuBar() {
    OpenConversation* oc = active();
    claude::ClaudeSession* s = oc ? oc->session.get() : nullptr;
    if (!ImGui::BeginMainMenuBar()) return;
    {
        ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        menuBarRect_[0] = p.x, menuBarRect_[1] = p.y, menuBarRect_[2] = p.x + sz.x, menuBarRect_[3] = p.y + sz.y;
    }
    if (ImGui::BeginMenu("Fichier")) {
        if (ImGui::MenuItem("Nouvelle conversation", "Ctrl+N")) newConversation(nullptr);
        if (ImGui::MenuItem("Nouveau projet…")) openNewProject_ = true;
        if (ImGui::MenuItem("Importer depuis Claude Code…")) {
            showHistory_ = true;
            histProjects_ = claude::listHistoryProjects();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Tout enregistrer", "Ctrl+S")) {
            saveAll();
            settings_.save(settingsPath_);
        }
        if (ImGui::MenuItem("Exporter la conversation (Markdown)", nullptr, false, oc != nullptr)) exportMarkdown(*oc);
        if (ImGui::MenuItem("Ouvrir le dossier de l'espace de travail")) util::openInShell(util::pathToUtf8(workspace_.root()));
        ImGui::Separator();
        if (ImGui::MenuItem("Quitter", "Alt+F4")) running_ = false;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Session", s != nullptr)) {
        bool busy = s->state == claude::SessionState::Busy;
        if (ImGui::MenuItem("Interrompre", "Échap", false, busy)) s->interrupt();
        if (ImGui::MenuItem("Redémarrer la session")) s->restart();
        ImGui::Separator();
        if (ImGui::MenuItem("Compacter le contexte (/compact)")) {
            oc->draft = "/compact";
            sendDraft(*oc);
        }
        if (ImGui::MenuItem("Occupation du contexte (/context)")) {
            oc->draft = "/context";
            sendDraft(*oc);
        }
        if (ImGui::MenuItem("Générer le récap", "Ctrl+Maj+R")) requestRecap(s->localId);
        if (ImGui::MenuItem("Copier la dernière réponse", "Ctrl+Maj+C")) {
            for (auto it = s->entries.rbegin(); it != s->entries.rend(); ++it)
                if (it->kind == claude::EntryKind::Assistant && it->parentToolUseId.empty()) {
                    ImGui::SetClipboardText(it->text.c_str());
                    break;
                }
        }
        if (ImGui::MenuItem("Copier l'identifiant de session", nullptr, false, !s->claudeSessionId.empty()))
            ImGui::SetClipboardText(s->claudeSessionId.c_str());
        if (ImGui::MenuItem("Reprendre dans le terminal", nullptr, false, !s->claudeSessionId.empty())) {
            std::string cmd = "claude --resume " + s->claudeSessionId;
            ImGui::SetClipboardText(cmd.c_str());
            toast("Commande copiée : " + cmd);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Fermer l'onglet", "Ctrl+W")) closeConversation(active_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Configuration")) {
        if (s) {
            if (ImGui::BeginMenu("Modèle")) {
                for (auto& c : claude::modelChoices())
                    if (ImGui::MenuItem(c.label, nullptr, s->config.model == c.value)) s->setModel(c.value);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Effort")) {
                for (auto& c : claude::effortChoices())
                    if (ImGui::MenuItem(c.label, nullptr, s->config.effort == c.value)) {
                        claude::SessionConfig cfg = s->config;
                        cfg.effort = c.value;
                        s->applyConfig(cfg);
                    }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Mode de permission")) {
                for (auto& c : claude::permissionModeChoices())
                    if (ImGui::MenuItem(c.label, nullptr, s->config.permissionMode == c.value)) s->setPermissionMode(c.value);
                ImGui::EndMenu();
            }
            ImGui::Separator();
        }
        const char* tabs[] = {"Modèle et effort…", "Mode…", "Outils…", "Prompt système…", "Avancé…", "Application…",
                              "Graphismes et fluidité…"};
        for (int i = 0; i < 7; ++i)
            if (ImGui::MenuItem(tabs[i], i == 0 ? "Ctrl+," : nullptr)) {
                showConfig_ = true;
                configTab_ = i;
                ImGui::SetWindowFocus("Configuration");
            }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Affichage")) {
        ImGui::MenuItem("Espace de travail", nullptr, &showWorkspace_);
        ImGui::MenuItem("Inspecteur", nullptr, &showInspector_);
        ImGui::MenuItem("Usage", nullptr, &showUsage_);
        ImGui::MenuItem("Configuration", nullptr, &showConfig_);
        ImGui::MenuItem("Journal du CLI", nullptr, &showLog_);
        ImGui::Separator();
        if (ImGui::MenuItem("Cockpit", "Ctrl+1", page_ == 0 && !dash_)) page_ = 0;
        if (ImGui::MenuItem("Tableau de bord des conversations", "Ctrl+2", page_ == 1 || dash_)) {
            if (dash_) SDL_RaiseWindow(dash_->window);
            else page_ = 1;
        }
        if (ImGui::MenuItem("Tableau de bord dans une fenêtre séparée", "Ctrl+Maj+D", dash_ != nullptr)) {
            if (dash_) closeDashboardWindow();
            else openDashboardWindow();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Thème")) {
            const char* themes[] = {"Sombre", "Clair", "Crépuscule"};
            for (int i = 0; i < 3; ++i)
                if (ImGui::MenuItem(themes[i], nullptr, settings_.theme == i)) {
                    settings_.theme = i;
                    applyTheme();
                }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Agrandir le texte", "Ctrl++")) {
            settings_.fontSize = std::min(32.0f, settings_.fontSize + 1);
            ImGui::GetStyle().FontSizeBase = settings_.fontSize;
        }
        if (ImGui::MenuItem("Réduire le texte", "Ctrl+-")) {
            settings_.fontSize = std::max(11.0f, settings_.fontSize - 1);
            ImGui::GetStyle().FontSizeBase = settings_.fontSize;
        }
        ImGui::MenuItem("Afficher la réflexion", nullptr, &settings_.showThinking);
        ImGui::MenuItem("Déplier les outils", nullptr, &settings_.expandTools);
        ImGui::MenuItem("Retour à la ligne dans le code", nullptr, &settings_.wrapCode);
        ImGui::Separator();
        if (ImGui::MenuItem("Réinitialiser la disposition")) {
            resetLayout_ = true;
            showWorkspace_ = showInspector_ = showUsage_ = showConfig_ = true;
        }
        ImGui::MenuItem("Démo ImGui (debug)", nullptr, &showDemo_);
        ImGui::EndMenu();
    }
    bool aideOpen = ImGui::BeginMenu("Aide");
    {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        aideMenuRect_[0] = a.x, aideMenuRect_[1] = a.y, aideMenuRect_[2] = b.x, aideMenuRect_[3] = b.y;
    }
    if (aideOpen) {
        if (showGuideReminder_) {  // ouvrir le menu Aide répond au rappel
            showGuideReminder_ = false;
            reminderFadeOut_ = now_;
        }
        ImGui::MenuItem("Guide de démarrage", "Maj+F1", &showGuide_);
        if (ImGui::MenuItem("Visite guidée de l'interface")) startTour();
        if (ImGui::MenuItem("Créer / ouvrir le projet exemple")) createExampleProject();
        ImGui::Separator();
        ImGui::MenuItem("Raccourcis clavier", "F1", &showShortcuts_);
        ImGui::MenuItem("Journal du CLI", nullptr, &showLog_);
        ImGui::MenuItem("À propos", nullptr, &showAbout_);
        ImGui::EndMenu();
    }
    drawPageSwitch();

    // À droite : rappel de l'usage 5 h
    if (auto* w = usage_.find("five_hour")) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "5 h : %d %%", (int)(w->utilization * 100 + 0.5));
        float width = ImGui::GetFontSize() * 6;
        ImGui::SameLine(ImGui::GetWindowWidth() - width - 12);
        ui::usageBar((float)w->utilization, buf, ImGui::GetTextLineHeight(), -1, "##mb5h");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Usage de la fenêtre de 5 h (détails dans le panneau Usage)");
        if (ImGui::IsItemClicked()) showUsage_ = true;
    }
    ImGui::EndMainMenuBar();
}

void App::drawStatusBar() {
    const auto& pal = ui::palette();
    float h = ImGui::GetFrameHeight();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 2));
    if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, h, flags)) {
        ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        statusBarRect_[0] = p.x, statusBarRect_[1] = p.y, statusBarRect_[2] = p.x + sz.x, statusBarRect_[3] = p.y + sz.y;
        if (ImGui::BeginMenuBar()) {
            OpenConversation* oc = active();
            if (oc) {
                auto& s = *oc->session;
                ImVec4 c = s.state == claude::SessionState::Busy ? pal.warn
                           : s.state == claude::SessionState::Idle ? pal.ok
                                                                   : pal.dim;
                ImGui::TextColored(c, "●");
                ImGui::TextUnformatted(claude::stateLabel(s.state));
                if (s.state == claude::SessionState::Busy) {
                    ImGui::TextDisabled("%s", util::humanDuration((int64_t)(now_ - s.turnStartTime)).c_str());
                    if (!s.statusText.empty()) ImGui::TextDisabled("%s", util::truncateUtf8(s.statusText, 50).c_str());
                }
                ImGui::Separator();
                std::string model = !s.activeModel.empty() ? s.activeModel : s.config.model.empty() ? "modèle par défaut" : s.config.model;
                ImGui::TextUnformatted(model.c_str());
                if (!s.config.effort.empty()) ImGui::TextDisabled("effort %s", s.config.effort.c_str());
                ImGui::TextDisabled("mode %s", s.config.permissionMode.c_str());
                ImGui::Separator();
                if (s.stats.contextWindow > 0)
                    ImGui::TextDisabled("contexte %d %%", (int)(100.0 * s.stats.contextTokens / s.stats.contextWindow));
                ImGui::TextDisabled("%s", util::formatUsd(s.stats.costUsd).c_str());
                if (s.restartPending) ImGui::TextColored(pal.warn, "redémarrage en attente");
            } else {
                ImGui::TextDisabled("Aucune conversation ouverte");
            }
            if (!jobs_.empty()) {
                ImGui::Separator();
                ImGui::TextColored(pal.accent, "%d tâche(s) de fond", (int)jobs_.size());
            }
            if (claudeExe_.empty()) {
                ImGui::Separator();
                ImGui::TextColored(pal.err, "Claude Code introuvable");
            }
            if (settings_.showFps) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "%.0f i/s · %.1f ms · %s", ImGui::GetIO().Framerate,
                              1000.0f / std::max(1.0f, ImGui::GetIO().Framerate), rendererName_.c_str());
                ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, ImGui::GetWindowWidth() - ImGui::CalcTextSize(buf).x - 16));
                ImGui::TextDisabled("%s", buf);
            }
            ImGui::EndMenuBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void App::drawShortcutsWindow() {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 30, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Raccourcis clavier", &showShortcuts_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    drawShortcutTables();
    ImGui::End();
}

void App::drawShortcutTables() {
    auto section = [](const char* title, std::initializer_list<std::pair<const char*, const char*>> items) {
        ImGui::SeparatorText(title);
        if (ImGui::BeginTable(title, 2, ImGuiTableFlags_SizingStretchProp)) {
            for (auto& [k, v] : items) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ui::palette().accent, "%s", k);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(v);
            }
            ImGui::EndTable();
        }
    };
    section("Éditeur", {{"Entrée", "Envoyer (réglable)"},
                        {"Maj+Entrée / Ctrl+Entrée", "Nouvelle ligne"},
                        {"Ctrl+C / X / V", "Copier / couper / coller"},
                        {"Ctrl+Z / Ctrl+Y", "Annuler / rétablir"},
                        {"Ctrl+A", "Tout sélectionner"},
                        {"Ctrl+← / →", "Mot précédent / suivant"},
                        {"Maj+flèches, souris", "Sélectionner"},
                        {"Double-clic", "Sélectionner un mot"},
                        {"Ctrl+Retour arrière", "Effacer le mot"},
                        {"Ctrl+↑ / ↓", "Messages précédents (historique)"},
                        {"/", "Commandes (Tab ou Entrée pour compléter)"},
                        {"Glisser-déposer", "Joindre une image / un fichier"}});
    section("Conversations", {{"Ctrl+N", "Nouvelle conversation"},
                              {"Ctrl+W", "Fermer l'onglet"},
                              {"Ctrl+PgSuiv / PgPréc", "Onglet suivant / précédent"},
                              {"Échap", "Interrompre Claude"},
                              {"Ctrl+F", "Rechercher dans la conversation"},
                              {"Ctrl+L", "Aller à l'éditeur"},
                              {"Ctrl+Maj+C", "Copier la dernière réponse"},
                              {"Ctrl+Maj+R", "Générer le récap"},
                              {"Alt+A / Alt+T / Alt+R", "Autoriser / toujours / refuser"}});
    section("Pages", {{"Ctrl+1", "Cockpit (page principale)"},
                      {"Ctrl+2", "Tableau de bord des conversations"},
                      {"Ctrl+Maj+D", "Tableau de bord dans une fenêtre séparée"}});
    section("Application", {{"Ctrl+S", "Tout enregistrer"},
                            {"Ctrl+,", "Configuration"},
                            {"Ctrl++ / Ctrl+- / Ctrl+0", "Taille du texte"},
                            {"Ctrl+molette", "Taille du texte"},
                            {"F1", "Raccourcis clavier"},
                            {"Maj+F1", "Guide de démarrage"}});
}

void App::drawAboutWindow() {
    if (!ImGui::Begin("À propos", &showAbout_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.4f);
    ImGui::TextColored(ui::palette().accent, "ClaudeDesk 0.1");
    ImGui::PopFont();
    ImGui::TextUnformatted("Interface de bureau C++ (SDL3 + Dear ImGui) pour Claude Code.");
    ImGui::TextDisabled("Pilote « claude -p » en stream-json : vos réglages, outils, MCP et CLAUDE.md s'appliquent.");
    ImGui::Separator();
    ImGui::Text("Claude Code : %s", claudeExe_.empty() ? "introuvable" : claudeExe_.c_str());
    if (auto* oc = active(); oc && !oc->session->cliVersion.empty()) ImGui::Text("Version : %s", oc->session->cliVersion.c_str());
    ImGui::Text("Espace de travail : %s", util::pathToUtf8(workspace_.root()).c_str());
    ImGui::Text("Réglages : %s", settingsPath_.c_str());
    ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
    ImGui::End();
}

void App::drawLogWindow() {
    if (!ImGui::Begin("Journal", &showLog_)) {
        ImGui::End();
        return;
    }
    OpenConversation* oc = active();
    if (!oc) {
        ImGui::TextDisabled("Aucune conversation active.");
        ImGui::End();
        return;
    }
    auto& log = oc->session->stderrLog;
    if (ImGui::SmallButton("Effacer")) log.clear();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copier")) {
        std::string all;
        for (auto& l : log) all += l + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Sortie d'erreur du processus Claude Code (%zu lignes)", log.size());
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(fontMono, ImGui::GetStyle().FontSizeBase * 0.84f);
    for (auto& l : log) ImGui::TextUnformatted(l.c_str());
    ImGui::PopFont();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace app
