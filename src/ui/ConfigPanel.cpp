// Panneau « Configuration » : modèle, effort, mode, outils, prompt système,
// options avancées de la conversation active (ou réglages par défaut), et
// préférences de l'application.
#include <algorithm>
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

bool radioChoice(const claude::Choice& c, std::string& value) {
    bool changed = false;
    if (ImGui::RadioButton(c.label, value == c.value)) {
        value = c.value;
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", c.help);
    return changed;
}

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

void App::drawConfigWindow() {
    if (!ImGui::Begin("Configuration", &showConfig_)) {
        ImGui::End();
        return;
    }
    const auto& pal = ui::palette();
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

    ImGui::TextColored(pal.dim, "%s", s ? ("Conversation : " + util::truncateUtf8(s->title, 50)).c_str()
                                         : "Réglages par défaut des nouvelles conversations");

    int selectTab = configTab_;
    configTab_ = -1;
    auto tabFlags = [&](int idx) { return selectTab == idx ? ImGuiTabItemFlags_SetSelected : 0; };
    float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;

    if (ImGui::BeginTabBar("##cfgtabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        // ---------------------------------------------------------------- Modèle
        if (ImGui::BeginTabItem("Modèle", nullptr, tabFlags(0))) {
            ImGui::BeginChild("##c0", ImVec2(0, -footer));
            ImGui::SeparatorText("Modèle");
            if (s && !s->models.empty()) {
                for (auto& m : s->models) {
                    std::string v = m.value == "default" ? "" : m.value;
                    if (ImGui::RadioButton(m.displayName.c_str(), editConfig_.model == v)) editConfig_.model = v;
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", m.description.c_str());
                }
            } else {
                for (auto& c : claude::modelChoices()) radioChoice(c, editConfig_.model);
            }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            ImGui::InputTextWithHint("Identifiant personnalisé", "ex. claude-opus-5-5", &editConfig_.model);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
            ImGui::InputTextWithHint("Modèle de secours", "si le principal est surchargé", &editConfig_.fallbackModel);

            ImGui::SeparatorText("Effort de réflexion");
            for (auto& c : claude::effortChoices()) radioChoice(c, editConfig_.effort);

            ImGui::SeparatorText("Style de réponse");
            std::vector<std::string> styles = {"default", "Concise", "Explanatory", "Learning", "Proactive"};
            if (s && !s->outputStyles.empty()) styles = s->outputStyles;
            std::string cur = editConfig_.outputStyle.empty() ? "default" : editConfig_.outputStyle;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::BeginCombo("Style de sortie", cur.c_str())) {
                for (auto& st : styles)
                    if (ImGui::Selectable(st.c_str(), st == cur)) editConfig_.outputStyle = st == "default" ? "" : st;
                ImGui::EndCombo();
            }
            ui::helpMarker("Styles de sortie de Claude Code (/output-style). Les vôtres se trouvent dans ~/.claude/output-styles.");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        // ---------------------------------------------------------------- Mode
        if (ImGui::BeginTabItem("Mode", nullptr, tabFlags(1))) {
            ImGui::BeginChild("##c1", ImVec2(0, -footer));
            ImGui::SeparatorText("Mode de permission");
            for (auto& c : claude::permissionModeChoices()) radioChoice(c, editConfig_.permissionMode);
            if (editConfig_.permissionMode == "bypassPermissions")
                ImGui::TextColored(pal.err, "Attention : aucune confirmation ne sera demandée.");
            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextWrapped("Le mode et le modèle s'appliquent immédiatement, même pendant une session. "
                               "Les autres réglages relancent la session en arrière-plan (--resume) : "
                               "l'historique est conservé.");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        // ---------------------------------------------------------------- Outils
        if (ImGui::BeginTabItem("Outils", nullptr, tabFlags(2))) {
            ImGui::BeginChild("##c2", ImVec2(0, -footer));
            std::vector<std::string> tools = claude::knownTools();
            if (s && !s->tools.empty()) {
                tools.clear();
                for (auto& t : s->tools)
                    if (!util::startsWith(t, "mcp__")) tools.push_back(t);
            }
            bool restrict = !editConfig_.enabledTools.empty();
            if (ImGui::Checkbox("Restreindre les outils disponibles", &restrict)) {
                if (restrict) editConfig_.enabledTools = {"Read", "Glob", "Grep"};
                else editConfig_.enabledTools.clear();
            }
            ui::helpMarker("Coché : seuls les outils marqués « Disponible » existent pour Claude (--tools).\n"
                           "Décoché : jeu d'outils par défaut.");
            if (ImGui::BeginTable("##tools", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, ImGui::GetFontSize() * 18))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Outil");
                ImGui::TableSetupColumn("Disponible", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Toujours autoriser", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Interdire", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                for (auto& t : tools) {
                    ImGui::PushID(t.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(t.c_str());
                    ImGui::TableSetColumnIndex(1);
                    bool avail = !restrict || contains(editConfig_.enabledTools, t);
                    ImGui::BeginDisabled(!restrict);
                    if (ImGui::Checkbox("##avail", &avail)) {
                        if (avail) editConfig_.enabledTools.push_back(t);
                        else removeAll(editConfig_.enabledTools, t);
                    }
                    ImGui::EndDisabled();
                    ImGui::TableSetColumnIndex(2);
                    bool allow = contains(editConfig_.allowedTools, t);
                    if (ImGui::Checkbox("##allow", &allow)) {
                        removeAll(editConfig_.allowedTools, t);
                        if (allow) {
                            editConfig_.allowedTools.push_back(t);
                            removeAll(editConfig_.disallowedTools, t);
                        }
                    }
                    ImGui::TableSetColumnIndex(3);
                    bool deny = contains(editConfig_.disallowedTools, t);
                    if (ImGui::Checkbox("##deny", &deny)) {
                        removeAll(editConfig_.disallowedTools, t);
                        if (deny) {
                            editConfig_.disallowedTools.push_back(t);
                            removeAll(editConfig_.allowedTools, t);
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::SeparatorText("Règles fines");
            ImGui::TextDisabled("Syntaxe Claude Code, ex. Bash(git *), Edit(src/**), WebFetch(domain:github.com)");
            static std::string allowBuf, denyBuf;
            ImGui::TextUnformatted("Toujours autorisées :");
            std::vector<std::string> customAllow, customDeny;
            for (auto& r : editConfig_.allowedTools)
                if (!contains(tools, r)) customAllow.push_back(r);
            for (auto& r : editConfig_.disallowedTools)
                if (!contains(tools, r)) customDeny.push_back(r);
            auto before = customAllow;
            stringList("allow", customAllow, allowBuf, "Bash(npm test)");
            if (customAllow != before) {
                std::vector<std::string> v;
                for (auto& r : editConfig_.allowedTools)
                    if (contains(tools, r)) v.push_back(r);
                v.insert(v.end(), customAllow.begin(), customAllow.end());
                editConfig_.allowedTools = v;
            }
            ImGui::TextUnformatted("Interdites :");
            before = customDeny;
            stringList("deny", customDeny, denyBuf, "Bash(rm *)");
            if (customDeny != before) {
                std::vector<std::string> v;
                for (auto& r : editConfig_.disallowedTools)
                    if (contains(tools, r)) v.push_back(r);
                v.insert(v.end(), customDeny.begin(), customDeny.end());
                editConfig_.disallowedTools = v;
            }
            if (s && !s->mcpServers.empty()) {
                ImGui::SeparatorText("Serveurs MCP");
                for (auto& [name, status] : s->mcpServers) ImGui::BulletText("%s — %s", name.c_str(), status.c_str());
            }
            ImGui::Checkbox("Intégration Chrome (--chrome)", &editConfig_.chrome);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        // ---------------------------------------------------------------- Prompt système
        if (ImGui::BeginTabItem("Prompt système", nullptr, tabFlags(3))) {
            ImGui::BeginChild("##c3", ImVec2(0, -footer));
            int mode = (int)editConfig_.systemPromptMode;
            ImGui::RadioButton("Par défaut", &mode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Ajouter au prompt de Claude Code", &mode, 1);
            ImGui::SameLine();
            ImGui::RadioButton("Remplacer entièrement", &mode, 2);
            editConfig_.systemPromptMode = (claude::SystemPromptMode)mode;
            if (mode == 2)
                ImGui::TextColored(pal.warn, "Remplacer retire les instructions de Claude Code (outils, sécurité, style) : à manier avec soin.");

            // Modèles de prompts
            static int preset = -1;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
            if (ImGui::BeginCombo("##preset", preset >= 0 && preset < (int)settings_.promptPresets.size()
                                                  ? settings_.promptPresets[preset].name.c_str()
                                                  : "Modèles…")) {
                for (int i = 0; i < (int)settings_.promptPresets.size(); ++i)
                    if (ImGui::Selectable(settings_.promptPresets[i].name.c_str(), preset == i)) {
                        preset = i;
                        editConfig_.systemPrompt = settings_.promptPresets[i].text;
                        if (editConfig_.systemPromptMode == claude::SystemPromptMode::Default)
                            editConfig_.systemPromptMode = claude::SystemPromptMode::Append;
                    }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            static std::string presetName;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            ImGui::InputTextWithHint("##pname", "Nom du modèle", &presetName);
            ImGui::SameLine();
            if (ImGui::Button("Enregistrer comme modèle") && !util::trim(presetName).empty()) {
                bool found = false;
                for (auto& p : settings_.promptPresets)
                    if (p.name == presetName) p.text = editConfig_.systemPrompt, found = true;
                if (!found) settings_.promptPresets.push_back({presetName, editConfig_.systemPrompt});
                settings_.save(settingsPath_);
                toast("Modèle enregistré : " + presetName);
            }
            if (preset >= 0 && preset < (int)settings_.promptPresets.size()) {
                ImGui::SameLine();
                if (ImGui::Button("Supprimer le modèle")) {
                    settings_.promptPresets.erase(settings_.promptPresets.begin() + preset);
                    preset = -1;
                }
            }
            ImGui::BeginDisabled(mode == 0);
            ImGui::InputTextMultiline("##sysprompt", &editConfig_.systemPrompt,
                                      ImVec2(-1, std::max(ImGui::GetFontSize() * 8, ImGui::GetContentRegionAvail().y * 0.55f)),
                                      ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_AllowTabInput);
            ImGui::EndDisabled();
            if (oc) {
                std::string ctx = oc->injectContext ? contextFor(*oc) : "";
                if (ImGui::TreeNode(("Contexte cowork injecté (" + util::humanTokens((int64_t)ctx.size()) + " car.)###ctx").c_str())) {
                    if (ctx.empty())
                        ImGui::TextDisabled("Aucun (désactivé ou conversation hors dossier).");
                    else
                        ui::selectableText("##ctxview", ctx, ImGui::GetContentRegionAvail().x, fontMono);
                    ImGui::TreePop();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        // ---------------------------------------------------------------- Avancé
        if (ImGui::BeginTabItem("Avancé", nullptr, tabFlags(4))) {
            ImGui::BeginChild("##c4", ImVec2(0, -footer));
            if (s) {
                ImGui::SeparatorText("Dossier de travail");
                ImGui::BeginDisabled(!s->claudeSessionId.empty());
                ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 6);
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
                if (!s->claudeSessionId.empty())
                    ImGui::TextDisabled("Figé : la session Claude Code est liée à ce dossier (--resume).");
            }
            ImGui::SeparatorText("Dossiers supplémentaires accessibles (--add-dir)");
            static std::string dirBuf;
            stringList("dirs", editConfig_.addDirs, dirBuf, "C:\\chemin\\vers\\dossier");
            if (ImGui::SmallButton("Choisir un dossier…"))
                openFolderDialog([this](const std::string& p) { editConfig_.addDirs.push_back(p); });
            ImGui::SeparatorText("Limites");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            ImGui::InputDouble("Budget max ($, 0 = illimité)", &editConfig_.maxBudgetUsd, 0.5, 1.0, "%.2f");
            ImGui::SeparatorText("Options supplémentaires");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##extra", "ex. --strict-mcp-config --bare", &editConfig_.extraArgs);
            ImGui::TextDisabled("Ajoutées telles quelles à la ligne de commande de Claude Code.");
            if (s && s->processRunning()) {
                ImGui::SeparatorText("Session en cours");
                ImGui::TextDisabled("Claude Code %s · modèle %s · mode %s", s->cliVersion.c_str(), s->activeModel.c_str(),
                                    s->activePermissionMode.c_str());
                if (ImGui::Button("Redémarrer la session")) s->restart();
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        // ---------------------------------------------------------------- Application
        if (ImGui::BeginTabItem("Application", nullptr, tabFlags(5))) {
            ImGui::BeginChild("##c5", ImVec2(0, -footer));
            ImGui::SeparatorText("Claude Code");
            ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 6);
            if (ImGui::InputTextWithHint("##exe", "détection automatique", &settings_.claudeExe))
                claudeExe_ = settings_.claudeExe.empty() ? claude::findClaudeExecutable() : settings_.claudeExe;
            ImGui::SameLine();
            if (ImGui::Button("Détecter")) {
                settings_.claudeExe.clear();
                claudeExe_ = claude::findClaudeExecutable();
            }
            ImGui::TextColored(claudeExe_.empty() ? pal.err : pal.dim, "Utilisé : %s",
                               claudeExe_.empty() ? "introuvable" : claudeExe_.c_str());

            ImGui::SeparatorText("Espace de travail (mode cowork)");
            ImGui::TextDisabled("Racine : %s", util::pathToUtf8(workspace_.root()).c_str());
            if (ImGui::Button("Ouvrir dans l'Explorateur")) util::openInShell(util::pathToUtf8(workspace_.root()));
            ImGui::SameLine();
            if (ImGui::Button("Changer de dossier…"))
                openFolderDialog([this](const std::string& p) {
                    settings_.workspaceRoot = p;
                    saveAll();
                    workspace_.open(util::pathFromUtf8(p));
                    settings_.save(settingsPath_);
                });
            ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 6);
            ImGui::InputTextWithHint("##defcwd", "dossier de travail par défaut (sinon : profil utilisateur)", &settings_.defaultCwd);
            ImGui::SameLine();
            if (ImGui::Button("Parcourir…##cwd")) openFolderDialog([this](const std::string& p) { settings_.defaultCwd = p; });
            ImGui::Checkbox("Injecter le contexte du dossier dans les nouvelles conversations", &settings_.injectContext);
            ImGui::Checkbox("Récap automatique à la fermeture d'une conversation", &settings_.autoRecap);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            ImGui::InputText("Modèle des récaps", &settings_.recapModel);
            ui::helpMarker("Haiku suffit et consomme très peu de quota.");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
            ImGui::SliderInt("Sauvegarde auto (s)", &settings_.autosaveSeconds, 1, 60);

            ImGui::SeparatorText("Éditeur");
            ImGui::Checkbox("Entrée envoie le message (Maj+Entrée = nouvelle ligne)", &settings_.enterSends);
            ImGui::TextDisabled("Sinon : Entrée = nouvelle ligne, Ctrl+Entrée = envoyer.");

            ImGui::SeparatorText("Affichage");
            const char* themes[] = {"Sombre", "Clair", "Crépuscule"};
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::Combo("Thème", &settings_.theme, themes, 3)) applyTheme();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            if (ImGui::SliderFloat("Taille du texte", &settings_.fontSize, 11, 32, "%.0f px")) {
                ImGui::GetStyle().FontSizeBase = settings_.fontSize;
                for (auto& o : open_) o->cache.clear();
            }
            ImGui::Checkbox("Afficher la réflexion de Claude", &settings_.showThinking);
            ImGui::Checkbox("Déplier les appels d'outils", &settings_.expandTools);
            ImGui::Checkbox("Retour à la ligne dans les blocs de code", &settings_.wrapCode);
            ImGui::Checkbox("Faire clignoter la barre des tâches quand Claude a fini", &settings_.notifyOnFinish);
            ImGui::Checkbox("Confirmer le mode « sans permissions »", &settings_.confirmBypass);

            ImGui::SeparatorText("Accueil");
            ImGui::TextDisabled("Lancements : %d. Le guide s'ouvre au 1er lancement, puis un rappel s'affiche les 10 suivants.",
                                settings_.launchCount);
            if (ImGui::Button("Revoir l'accueil des nouveaux utilisateurs")) {
                settings_.launchCount = 0;
                settings_.save(settingsPath_);
                toast("Le guide s'ouvrira au prochain lancement, puis les rappels reprendront.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Ouvrir le guide maintenant")) {
                showGuide_ = true;
                guideSection_ = 0;
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
            if (ImGui::Button("Enregistrer les préférences")) {
                settings_.save(settingsPath_);
                toast("Préférences enregistrées.");
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        // ---------------------------------------------------------------- Graphismes
        if (ImGui::BeginTabItem("Graphismes", nullptr, tabFlags(6))) {
            ImGui::BeginChild("##c6", ImVec2(0, -footer));
            ImGuiIO& io = ImGui::GetIO();
            ImGui::SeparatorText("Fluidité");
            char vs[96];
            std::snprintf(vs, sizeof vs, "Synchronisée sur l'écran (VSync, %.0f Hz)", displayHz_);
            ImGui::RadioButton(vs, &settings_.fpsMode, 0);
            ui::helpMarker("Une image par rafraîchissement de l'écran : le plus fluide sans déchirement.");
            ImGui::RadioButton("Limite personnalisée", &settings_.fpsMode, 1);
            if (settings_.fpsMode == 1) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
                ImGui::SliderInt("##cap", &settings_.fpsCap, 30, 480, "%d i/s");
            }
            ImGui::RadioButton("Illimitée (charge maximale du processeur graphique)", &settings_.fpsMode, 2);
            ImGui::Checkbox("Économie d'énergie : ne redessiner qu'en cas d'activité", &settings_.ecoIdle);
            ui::helpMarker("Au repos (aucune animation, Claude inactif), l'interface attend vos actions au lieu de\n"
                           "redessiner en continu. Désactivé = animations toujours parfaitement fluides.");
            ImGui::Checkbox("Limiter à 30 i/s quand ClaudeDesk n'a pas le focus", &settings_.ecoBackground);
            ImGui::Checkbox("Afficher les images/seconde dans la barre d'état", &settings_.showFps);
            ImGui::TextDisabled("Actuellement : %.0f images/s (%.2f ms par image)", io.Framerate, 1000.0f / std::max(1.0f, io.Framerate));

            ImGui::SeparatorText("Animations");
            ImGui::Checkbox("Animations activées", &settings_.animations);
            ImGui::BeginDisabled(!settings_.animations);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
            ImGui::SliderFloat("Vitesse", &settings_.animSpeed, 0.5f, 2.0f, "x %.2f");
            ImGui::Checkbox("Défilement fluide (molette, Page préc./suiv., Début/Fin)", &settings_.smoothScroll);
            ImGui::Checkbox("Halos lumineux (cartes actives, visite guidée)", &settings_.glowEffects);
            ImGui::EndDisabled();
            // Aperçu en direct
            ImGui::TextDisabled("Aperçu :");
            ImGui::SameLine();
            ui::spinner(0, ui::palette().accent);
            ImGui::SameLine();
            float demo = 0.5f + 0.45f * (float)std::sin(ImGui::GetTime() * 0.8);
            ui::usageBar(demo, "", ImGui::GetTextLineHeight() * 0.8f, ImGui::GetFontSize() * 10, "##demobar");

            ImGui::SeparatorText("Qualité");
            if (ImGui::Checkbox("Courbes et coins arrondis haute définition", &settings_.highQuality)) applyTheme();
            const char* fr[] = {"FreeType, lissage léger (recommandé, net)", "FreeType, sans alignement sur la grille (plus doux)",
                                "stb_truetype (moteur de base d'ImGui)"};
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
            if (ImGui::Combo("Rendu du texte", &settings_.fontRenderer, fr, 3)) fontReloadPending_ = true;
            ImGui::TextDisabled("Avec FreeType, les emoji s'affichent en couleur.");

            ImGui::SeparatorText("Processeur graphique");
            ImGui::Text("Moteur de rendu actif : %s", rendererName_.c_str());
            auto label = [](const std::string& n) -> std::string {
                if (n.empty()) return "Automatique (le meilleur disponible)";
                if (n == "direct3d12") return "Direct3D 12";
                if (n == "direct3d11") return "Direct3D 11";
                if (n == "direct3d") return "Direct3D 9";
                if (n == "vulkan") return "Vulkan";
                if (n == "opengl") return "OpenGL";
                if (n == "opengles2") return "OpenGL ES 2";
                if (n == "gpu") return "SDL GPU (Direct3D 12 / Vulkan moderne)";
                if (n == "software") return "Logiciel (processeur, sans GPU)";
                return n;
            };
            static std::string pendingDriver = "#";
            if (pendingDriver == "#") pendingDriver = settings_.renderDriver;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
            if (ImGui::BeginCombo("Moteur", label(pendingDriver).c_str())) {
                if (ImGui::Selectable(label("").c_str(), pendingDriver.empty())) pendingDriver.clear();
                for (int i = 0; i < SDL_GetNumRenderDrivers(); ++i) {
                    std::string n = SDL_GetRenderDriver(i) ? SDL_GetRenderDriver(i) : "";
                    if (!n.empty() && ImGui::Selectable(label(n).c_str(), pendingDriver == n)) pendingDriver = n;
                }
                ImGui::EndCombo();
            }
            if (pendingDriver != settings_.renderDriver) {
                ImGui::TextColored(ui::palette().warn, "Changer de moteur nécessite de relancer ClaudeDesk.");
                if (ImGui::Button("Appliquer et relancer")) {
                    settings_.renderDriver = pendingDriver;
                    restartApp();
                }
                ImGui::SameLine();
                if (ImGui::Button("Annuler##drv")) pendingDriver = settings_.renderDriver;
            }
            ImGui::TextDisabled("Sur les portables à double carte graphique, ClaudeDesk demande la carte dédiée.");
            ImGui::Dummy(ImVec2(0, 4));
            if (ImGui::Button("Enregistrer les préférences##gfx")) {
                settings_.save(settingsPath_);
                toast("Préférences graphiques enregistrées.");
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // --- Pied : appliquer ---
    bool changed = nlohmann::json(editConfig_) != nlohmann::json(base);
    bool restart = s && s->processRunning() && editConfig_.needsRestartComparedTo(base);
    ImGui::Separator();
    if (changed) {
        ImGui::TextColored(pal.warn, restart ? "Modifications non appliquées — la session sera relancée (historique conservé)."
                                             : "Modifications non appliquées.");
    } else {
        ImGui::TextDisabled("À jour.");
    }
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button(s ? "Appliquer à la conversation" : "Appliquer")) {
        if (s) s->applyConfig(editConfig_);
        else {
            settings_.defaultConfig = editConfig_;
            settings_.save(settingsPath_);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Annuler")) editConfig_ = base;
    ImGui::EndDisabled();
    if (s) {
        ImGui::SameLine();
        if (ImGui::Button("Enregistrer comme défaut")) {
            settings_.defaultConfig = editConfig_;
            settings_.save(settingsPath_);
            toast("Réglages par défaut enregistrés pour les nouvelles conversations.");
        }
    }
    ImGui::End();
}

}  // namespace app
