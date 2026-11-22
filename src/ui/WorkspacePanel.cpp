// Panneau « Projets » : projets › thèmes › conversations, avec des icônes à la VS Code (projet, thème, agent,
// orchestrateur, conversation, fichiers), l'explorateur du dossier de travail, les fichiers de référence,
// l'inspecteur (notes, synthèses, récaps) et l'import des sessions de Claude Code.
#include <algorithm>
#include <cstdio>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/App.h"
#include "claude/Models.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace app {

namespace {

const char* kConvPayload = "CD_CONV";
constexpr double kDirRefreshS = 3.0;      // explorateur du dossier de travail : relecture au plus toutes les 3 s
constexpr size_t kMaxDirEntries = 400;    // au-delà : « … » (dossiers énormes)

// Dossiers lourds ou techniques, masqués dans l'explorateur (comme le fait VS Code par défaut pour .git).
bool hiddenDir(const std::string& n) {
    static const char* const names[] = {".git", "node_modules", "__pycache__", ".venv", "venv", ".vs", ".idea",
                                        ".cache", ".corbeille", ".mypy_cache", ".pytest_cache", ".DS_Store"};
    for (auto x : names)
        if (n == x) return true;
    return false;
}

// Met à jour le chemin des conversations ouvertes après un renommage de dossier.
void fixOpenPaths(std::vector<std::unique_ptr<OpenConversation>>& open, const std::string& oldPrefix, const std::string& newPrefix) {
    for (auto& oc : open)
        if (util::startsWith(oc->folderPath, oldPrefix)) oc->folderPath = newPrefix + oc->folderPath.substr(oldPrefix.size());
}

ImVec4 statusColor(const std::string& s) {
    const auto& p = ui::palette();
    if (s == "active") return p.warn;
    if (s == "done") return p.ok;
    if (s == "blocked") return p.err;
    return p.link;
}

std::string humanSize(uintmax_t n) {
    char buf[32];
    if (n < 1024) std::snprintf(buf, sizeof buf, "%u o", (unsigned)n);
    else if (n < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f Ko", n / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.1f Mo", n / (1024.0 * 1024.0));
    return buf;
}

// Ligne de la légende : icône + explication.
void legendRow(ui::Icon ic, const char* title, const char* text) {
    ui::icon(ic);
    ImGui::SameLine();
    ImGui::TextUnformatted(title);
    ImGui::SameLine();
    ImGui::TextDisabled("— %s", text);
}

}  // namespace

bool App::projectOrchestrated(const ws::Folder* project) {
    if (!project) return false;
    std::string path = util::pathToUtf8(project->path);
    if (orch_ && orchProject_ == path) return true;
    auto& c = orchFileCache_[path];
    if (now_ - c.first > 5.0 || c.first == 0) {
        std::error_code ec;
        c = {now_ + 1e-9, fs::exists(project->path / "_orchestrateur.json", ec)};
    }
    return c.second;
}

bool App::isAgentFolder(const ws::Folder* f) {
    return f && f->parent && f->parent->isProject && projectOrchestrated(f->parent);
}

bool App::isBrainConversation(const ws::Folder* f, const ws::ConversationMeta& m) {
    if (isOrchBrain(m.id)) return true;
    return f && f->isProject && m.title == "Orchestrateur" && projectOrchestrated(f);
}

void App::drawIconLegend() {
    ImGui::TextUnformatted("Que représentent les icônes ?");
    ImGui::Separator();
    legendRow(ui::Icon::Project, "Projet", "un dossier de code sur lequel Claude travaille");
    legendRow(ui::Icon::Folder, "Thème", "regroupe les conversations d'un sujet (Code, Infra, Docs…)");
    legendRow(ui::Icon::Agent, "Agent", "thème d'un projet orchestré : un membre de l'équipe");
    legendRow(ui::Icon::Orchestrator, "Orchestrateur", "la conversation qui pilote les agents");
    legendRow(ui::Icon::Chat, "Conversation", "un échange enregistré avec Claude (double-clic pour le reprendre)");
    legendRow(ui::Icon::History, "Session précédente",
              "devenue trop longue, la discussion a été résumée et poursuivie : les sessions sont rangées sous la plus "
              "récente (flèche)");
    legendRow(ui::Icon::WorkDir, "Dossier de travail", "les fichiers du projet (code, documents, données) que Claude lit et modifie");
    legendRow(ui::Icon::FileText, "Fichier", "document de référence (glissez-en un sur un dossier pour l'ajouter)");
    ImGui::Spacing();
    ImGui::TextDisabled("Couleur des fichiers :");
    ui::icon(ui::Icon::FileCode);
    ImGui::SameLine();
    ImGui::TextDisabled("code ");
    ImGui::SameLine();
    ui::icon(ui::Icon::FileData);
    ImGui::SameLine();
    ImGui::TextDisabled("données ");
    ImGui::SameLine();
    ui::icon(ui::Icon::FileImage);
    ImGui::SameLine();
    ImGui::TextDisabled("image ");
    ImGui::SameLine();
    ui::icon(ui::Icon::FilePdf);
    ImGui::SameLine();
    ImGui::TextDisabled("document");
    ImGui::Spacing();
    ui::icon(ui::Icon::Chat, 0, 1.0f);
    ImGui::SameLine();
    ImGui::TextDisabled("point orange : Claude travaille");
}

void App::addFilesTo(const std::string& folderPath, const std::vector<std::string>& files) {
    ws::Folder* f = workspace_.findFolderByPath(util::pathFromUtf8(folderPath));
    if (!f || files.empty()) return;
    std::vector<fs::path> src;
    for (auto& s : files) src.push_back(util::pathFromUtf8(s));
    std::string err;
    int n = workspace_.addFiles(f, src, &err);
    if (n > 0)
        toast((n > 1 ? std::to_string(n) + " fichiers ajoutés à « " : "Fichier ajouté à « ") + f->name +
              " » : Claude les verra dans les nouvelles conversations du dossier.");
    if (!err.empty()) toast(err, true);
}

bool App::dropOnProjects(float x, float y, const std::string& file) {
    if (!showWorkspace_ || page_ != 0) return false;
    if (x < projectsRect_[0] || x > projectsRect_[2] || y < projectsRect_[1] || y > projectsRect_[3]) return false;
    std::string target;
    for (auto& z : dropZones_)
        if (x >= z.x0 && x <= z.x1 && y >= z.y0 && y <= z.y1) target = z.folder;
    if (target.empty()) target = selectedFolder_;
    if (target.empty()) {
        toast("Lâchez le fichier sur un projet ou un thème pour l'y ajouter.", true);
        return true;
    }
    std::error_code ec;
    if (fs::is_directory(util::pathFromUtf8(file), ec)) {
        toast("Seuls les fichiers peuvent être ajoutés à un projet (pas les dossiers).", true);
        return true;
    }
    addFilesTo(target, {file});
    return true;
}

void App::drawWorkspacePanel() {
    dropZones_.clear();
    if (!ImGui::Begin("Projets", &showWorkspace_)) {
        projectsRect_[2] = projectsRect_[0] - 1;  // panneau masqué : aucune zone de dépôt
        ImGui::End();
        return;
    }
    {
        ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        projectsRect_[0] = p.x, projectsRect_[1] = p.y, projectsRect_[2] = p.x + sz.x, projectsRect_[3] = p.y + sz.y;
    }
    const auto& palp = ui::palette();
    if (ui::chip("+ Projet", &palp.link)) openNewProject_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Créer un projet (un dossier de travail + des thèmes)");
    ImGui::SameLine();
    if (ui::chip("Importer", &palp.dim)) {
        showHistory_ = true;
        histProjects_ = claude::listHistoryProjects();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Importer des sessions faites dans le terminal (claude)");
    ImGui::SameLine();
    if (ui::chip("⟳", &palp.dim)) {
        workspace_.rescan();
        dirCache_.clear();
        orchFileCache_.clear();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Relire le dossier des projets et les dossiers de travail");
    ImGui::SameLine();
    ui::chip("?", &palp.dim);
    if (ImGui::IsItemHovered() || optPanel_ == "projets:legende") {  // (capture de test : légende affichée)
        ImGui::BeginTooltip();
        drawIconLegend();
        ImGui::EndTooltip();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Rechercher (titre, récap, fichier)…", &treeFilter_);

    ImGui::BeginChild("##tree");
    if (!util::trim(treeFilter_).empty()) {
        // Résultats à plat
        std::function<void(ws::Folder*)> search = [&](ws::Folder* f) {
            for (auto& c : f->conversations)
                if (util::containsInsensitive(c.title, treeFilter_) || util::containsInsensitive(c.recap, treeFilter_)) {
                    drawConversationItem(f, c);
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", f->displayPath().c_str());
                }
            for (auto& file : f->files)
                if (util::containsInsensitive(util::pathToUtf8(file.filename()), treeFilter_)) {
                    drawFileItem(util::pathToUtf8(file), f);
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", f->displayPath().c_str());
                }
            for (auto& ch : f->children) search(ch.get());
        };
        for (auto& p : workspace_.projects()) search(p.get());
    } else {
        if (workspace_.projects().empty())
            ImGui::TextWrapped("Aucun projet pour l'instant. Démarrez-en un depuis l'accueil (idées, analyse de données, "
                               "rapport, logiciel… : le Designer organise le travail selon le type), ou avec « + Projet ». "
                               "Un projet = un dossier de fichiers, découpé en thèmes qui regroupent vos conversations.");
        for (size_t i = 0; i < workspace_.projects().size(); ++i) {
            drawFolderNode(workspace_.projects()[i].get());
            // la liste a pu changer (suppression) : on arrête l'itération
            if (i >= workspace_.projects().size()) break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

bool App::drawConversationItem(ws::Folder* f, ws::ConversationMeta& m, int session, int sessions,
                               const std::string& labelIn) {
    const auto& pal = ui::palette();
    bool isOpen = false, busy = false;
    for (auto& oc : open_)
        if (oc->session->localId == m.id) {
            isOpen = true;
            busy = oc->session->state == claude::SessionState::Busy || oc->session->state == claude::SessionState::Starting;
        }
    ImGui::PushID(m.id.c_str());
    if (renameTarget_ == m.id && !renameIsFolder_) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename", &renameBuffer_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            ws::ConversationMeta upd = m;
            upd.title = util::trim(renameBuffer_);
            if (!upd.title.empty()) {
                for (auto& oc : open_)
                    if (oc->session->localId == m.id) oc->session->title = upd.title;
                workspace_.updateConversationMeta(upd);
            }
            renameTarget_.clear();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) renameTarget_.clear();
        ImGui::PopID();
        return false;
    }
    bool brain = isBrainConversation(f, m);
    // Session d'une suite : la dernière porte le titre (et se déplie sur les précédentes), les précédentes leur rang.
    const bool chained = sessions > 1;
    const bool current = chained && session == sessions;
    const bool relayed = chained && !current;
    std::string label = std::string(m.pinned ? "★ " : "") + (labelIn.empty() ? m.title : labelIn);
    bool selected = selectedConv_ == m.id && selectedFile_.empty();
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (current) fl |= ImGuiTreeNodeFlags_OpenOnArrow;
    else fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected) fl |= ImGuiTreeNodeFlags_Selected;
    if (isOpen) ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
    else if (relayed) ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
    ui::Icon ic = relayed ? ui::Icon::History : brain ? ui::Icon::Orchestrator : ui::Icon::Chat;
    bool nodeOpen = ui::iconTreeNode("##conv", fl, ic, label.c_str(), 0, busy ? 1.0f : 0.0f);
    if (isOpen || relayed) ImGui::PopStyleColor();
    if (!current) nodeOpen = false;
    {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        dropZones_.push_back({a.x, a.y, b.x, b.y, util::pathToUtf8(f->path)});
    }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selectedConv_ = m.id;
        selectedFolder_ = util::pathToUtf8(f->path);
        selectedFile_.clear();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) && !ImGui::IsItemToggledOpen()) {
        if (brain && isOrchBrain(m.id)) openOrchestratorPage(true);
        else openConversation(m.id);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::BeginTooltip();
        ui::icon(ic);
        ImGui::SameLine();
        ImGui::TextUnformatted(m.title.c_str());
        ImGui::TextDisabled("%s · %s · %d messages · %s", brain ? "Orchestrateur" : "Conversation",
                            util::humanDate(m.updated).c_str(), m.messages, util::formatUsd(m.costUsd).c_str());
        if (current)
            ImGui::TextColored(pal.link, "Session %d (l'actuelle) : les %d précédentes ont été résumées pour repartir\n"
                                         "d'un contexte léger. Flèche à gauche : les afficher.",
                               session, sessions - 1);
        else if (relayed)
            ImGui::TextColored(pal.link, "Session %d sur %d : son contexte devenait trop long, elle a été résumée\n"
                                         "et poursuivie dans la session %d. Double-clic : la relire.",
                               session, sessions, session + 1);
        if (!m.recap.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
            ImGui::TextUnformatted(util::truncateUtf8(m.recap, 600).c_str());
            ImGui::PopTextWrapPos();
        } else {
            ImGui::TextDisabled("Pas encore de récap (clic droit › Générer le récap).");
        }
        ImGui::EndTooltip();
    }
    if (relayed && m.status == "done") {
        ImGui::SameLine();
        ImGui::TextDisabled("%s · résumée", util::humanDate(m.updated).c_str());
    } else if (!m.status.empty()) {
        ImGui::SameLine();
        ui::badge(ws::statusLabel(m.status), statusColor(m.status));
    }
    if (current) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d sessions", sessions);
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kConvPayload, m.id.c_str(), m.id.size() + 1);
        ui::icon(ui::Icon::Chat);
        ImGui::SameLine();
        ImGui::Text("Déplacer « %s »", m.title.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("##convctx")) {
        if (ImGui::MenuItem("Ouvrir / reprendre")) openConversation(m.id);
        if (ImGui::MenuItem("Renommer")) {
            renameTarget_ = m.id;
            renameIsFolder_ = false;
            renameBuffer_ = m.title;
        }
        if (ImGui::MenuItem("Générer le récap")) requestRecap(m.id);
        if (ImGui::BeginMenu("Statut")) {
            for (const char* st : {"", "idea", "active", "blocked", "done"}) {
                if (ImGui::MenuItem(*st ? ws::statusLabel(st) : "Aucun", nullptr, m.status == st)) {
                    ws::ConversationMeta upd = m;
                    upd.status = st;
                    workspace_.updateConversationMeta(upd);
                    for (auto& oc : open_)
                        if (oc->session->localId == m.id) oc->meta.status = st;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(m.pinned ? "Désépingler" : "Épingler")) {
            ws::ConversationMeta upd = m;
            upd.pinned = !m.pinned;
            workspace_.updateConversationMeta(upd);
            for (auto& oc : open_)
                if (oc->session->localId == m.id) oc->meta.pinned = upd.pinned;
        }
        if (ImGui::BeginMenu("Déplacer vers")) {
            for (auto& p : workspace_.projects()) drawMoveMenu(m.id, p.get());
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Nouvelle conversation dans ce dossier")) newConversation(f);
        if (ImGui::MenuItem("Afficher le fichier")) util::openInShell(util::pathToUtf8(f->path));
        ImGui::Separator();
        if (ImGui::MenuItem("Supprimer (corbeille)")) {
            std::string id = m.id;
            for (int i = (int)open_.size() - 1; i >= 0; --i)
                if (open_[i]->session->localId == id) {
                    open_[i]->session->stop();
                    open_.erase(open_.begin() + i);
                    if (active_ >= (int)open_.size()) active_ = (int)open_.size() - 1;
                }
            workspace_.removeConversation(id);
            ImGui::EndPopup();
            if (nodeOpen) ImGui::TreePop();
            ImGui::PopID();
            return false;  // `m` n'existe plus
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return nodeOpen;
}

bool App::drawChain(ws::Folder* f, const ws::ConversationChain& c, const std::string& label) {
    const size_t before = f->conversations.size();
    const int n = c.size();
    ws::ConversationMeta* last = c.latest();
    const std::string lastTitle = last->title;
    if (optPanel_ == "projets:sessions" && n > 2 && f->parent) {  // capture de test
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (selectedConv_.empty()) selectedConv_ = c.sessions[n - 2]->id;
    }
    bool open = drawConversationItem(f, *last, n > 1 ? n : 0, n > 1 ? n : 0, label);
    if (f->conversations.size() != before) {
        if (open) ImGui::TreePop();
        return false;
    }
    if (!open) return true;
    // Sessions précédentes, de la plus récente à la plus ancienne
    for (int k = n - 2; k >= 0; --k) {
        ws::ConversationMeta* m = c.sessions[k];
        std::string l = "Session " + std::to_string(k + 1);
        if (m->title != lastTitle) l += " · " + m->title;
        drawConversationItem(f, *m, k + 1, n, l);
        if (f->conversations.size() != before) {
            ImGui::TreePop();
            return false;
        }
    }
    ImGui::TreePop();
    return true;
}

void App::openProjectOrchestrator(ws::Folder* project) {
    if (!project) return;
    if (!(orch_ && orchProject_ == util::pathToUtf8(project->path))) {
        if (orch_ && orch_->running()) {
            toast("L'orchestrateur d'un autre projet travaille : mettez-le en pause pour ouvrir celui de « " +
                      project->name + " ».",
                  true);
            return;
        }
        if (!loadOrchestrator(project)) return;
    }
    openOrchestratorPage(true);
}

void App::drawOrchestratorNode(ws::Folder* project) {
    const auto& pal = ui::palette();
    const std::string ppath = util::pathToUtf8(project->path);
    const bool loaded = orch_ && orchProject_ == ppath;
    // Discussion du cerveau (copie : le dessin des agents peut modifier le dossier)
    std::string brainId, brainRecap;
    int brainSessions = 0;
    for (const auto& c : ws::conversationChains(*project))
        if (isBrainConversation(project, *c.latest())) {
            brainId = c.latest()->id;
            brainRecap = c.latest()->recap;
            brainSessions = c.size();
            break;
        }
    // État, en quelques mots
    const char* state = brainId.empty() ? "pas encore lancé" : "à l'arrêt";
    ImVec4 stateCol = pal.dim;
    bool active = false;
    if (loaded) {
        switch (orch_->state) {
            case claude::OrchState::Idle: state = brainId.empty() ? "pas encore lancé" : "prêt"; break;
            case claude::OrchState::Thinking: state = "réfléchit", stateCol = pal.accent, active = true; break;
            case claude::OrchState::Working: state = "les agents travaillent", stateCol = pal.accent, active = true; break;
            case claude::OrchState::WaitingHuman: state = "attend votre réponse", stateCol = pal.warn; break;
            case claude::OrchState::Paused: state = "en pause", stateCol = pal.warn; break;
            case claude::OrchState::UsagePause: state = "pause : limite d'usage", stateCol = pal.warn; break;
            case claude::OrchState::Finished: state = "terminé", stateCol = pal.ok; break;
        }
    }
    const int agents = (int)project->children.size();

    ImGui::PushID("##orchnode");
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
    if (!brainId.empty() && selectedConv_ == brainId && selectedFile_.empty()) fl |= ImGuiTreeNodeFlags_Selected;
    ImGui::PushFont(fontBold, 0.0f);
    bool open = ui::iconTreeNode("##orch", fl, ui::Icon::Orchestrator, "Orchestrateur", 0, active ? 1.0f : 0.0f);
    ImGui::PopFont();
    if (ImGui::IsItemClicked() && !brainId.empty()) {
        selectedConv_ = brainId;
        selectedFolder_ = ppath;
        selectedFile_.clear();
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) openProjectOrchestrator(project);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::BeginTooltip();
        ui::icon(ui::Icon::Orchestrator);
        ImGui::SameLine();
        ImGui::TextUnformatted("Orchestrateur du projet");
        ImGui::TextDisabled("Il répartit le travail entre les agents rangés dessous et suit leurs résultats.");
        ImGui::TextColored(stateCol, "État : %s", state);
        ImGui::TextDisabled("%d agent(s)%s", agents,
                            brainSessions > 1 ? (" · sa discussion en est à la session " + std::to_string(brainSessions)).c_str() : "");
        if (!brainRecap.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
            ImGui::TextUnformatted(util::truncateUtf8(brainRecap, 500).c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Clic : afficher / masquer l'équipe · double-clic : ouvrir la page Orchestrateur");
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    ui::badge(state, stateCol);
    ImGui::SameLine();
    ImGui::TextDisabled("%d agent%s", agents, agents > 1 ? "s" : "");
    if (ImGui::BeginPopupContextItem("##orchctx")) {
        if (ImGui::MenuItem("Ouvrir la page Orchestrateur")) openProjectOrchestrator(project);
        if (ImGui::MenuItem("Ouvrir sa discussion", nullptr, false, !brainId.empty())) {
            if (isOrchBrain(brainId)) openOrchestratorPage(true);
            else openConversation(brainId);
        }
        if (ImGui::MenuItem("Nouvel agent (thème)")) {
            std::string err;
            if (ws::Folder* nf = workspace_.createFolder(project, "Nouvel agent", &err)) {
                renameTarget_ = util::pathToUtf8(nf->path);
                renameIsFolder_ = true;
                renameBuffer_ = nf->name;
            } else {
                toast(err, true);
            }
        }
        ImGui::EndPopup();
    }
    if (open) {
        if (project->children.empty())
            ImGui::TextDisabled("Pas encore d'agent : l'orchestrateur en proposera au lancement.");
        for (size_t i = 0; i < project->children.size(); ++i) drawFolderNode(project->children[i].get());
        // Sa propre discussion (sessions regroupées), après l'équipe
        for (const auto& c : ws::conversationChains(*project))
            if (isBrainConversation(project, *c.latest())) {
                drawChain(project, c, "Discussion de l'orchestrateur");
                break;
            }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void App::drawMoveMenu(const std::string& convId, ws::Folder* f) {
    bool hasChildren = !f->children.empty();
    auto doMove = [&] {
        if (workspace_.moveConversation(convId, f))
            for (auto& oc : open_)
                if (oc->session->localId == convId) oc->folderPath = util::pathToUtf8(f->path);
    };
    if (!hasChildren) {
        if (ImGui::MenuItem(f->name.c_str())) doMove();
        return;
    }
    if (ImGui::BeginMenu(f->name.c_str())) {
        if (ImGui::MenuItem(("(ici) " + f->name).c_str())) doMove();
        ImGui::Separator();
        for (auto& c : f->children) drawMoveMenu(convId, c.get());
        ImGui::EndMenu();
    }
}

void App::drawFileItem(const std::string& path, ws::Folder* owner) {
    const auto& pal = ui::palette();
    fs::path p = util::pathFromUtf8(path);
    std::string name = util::pathToUtf8(p.filename());
    ui::Icon ic = ui::fileIcon(name);
    ImGui::PushID(path.c_str());
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selectedFile_ == path) fl |= ImGuiTreeNodeFlags_Selected;
    ui::iconTreeNode("##file", fl, ic, name.c_str());
    if (owner) {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        dropZones_.push_back({a.x, a.y, b.x, b.y, util::pathToUtf8(owner->path)});
    }
    if (ImGui::IsItemClicked()) {
        selectedFile_ = path;
        selectedFileOwner_ = owner ? util::pathToUtf8(owner->path) : std::string();
        selectedConv_.clear();
        if (owner) selectedFolder_ = util::pathToUtf8(owner->path);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) util::openInShell(path);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(path.c_str());
        ImGui::TextDisabled(owner ? "Document de référence : Claude le voit dans les conversations de « %s »."
                                  : "Fichier du dossier de travail%s",
                            owner ? owner->name.c_str() : "");
        ImGui::TextDisabled("Double-clic : ouvrir · clic droit : joindre à la conversation…");
        ImGui::EndTooltip();
    }
    if (ImGui::BeginPopupContextItem("##filectx")) {
        OpenConversation* oc = active();
        if (ImGui::MenuItem("Ouvrir")) util::openInShell(path);
        if (ImGui::MenuItem("Afficher dans l'Explorateur")) util::openInShell(util::pathToUtf8(p.parent_path()));
        if (ImGui::MenuItem("Joindre à la conversation active", nullptr, false, oc != nullptr)) attachFile(*oc, path);
        if (ImGui::MenuItem("Copier le chemin")) ImGui::SetClipboardText(path.c_str());
        if (owner) {
            ImGui::Separator();
            if (ImGui::MenuItem("Retirer du projet (corbeille)")) {
                if (!workspace_.removeFile(owner, p)) toast("Impossible de retirer ce fichier (ouvert ailleurs ?)", true);
                if (selectedFile_ == path) selectedFile_.clear();
                ImGui::EndPopup();
                ImGui::PopID();
                return;  // `path` peut référencer l'élément retiré
            }
        }
        ImGui::EndPopup();
    }
    (void)pal;
    ImGui::PopID();
}

void App::drawDirListing(const std::string& dir, int depth) {
    auto& L = dirCache_[dir];
    if (now_ - L.time > kDirRefreshS) {
        L.time = now_;
        L.entries.clear();
        L.truncated = false;
        std::error_code ec;
        for (auto it = fs::directory_iterator(util::pathFromUtf8(dir), fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::string n = util::pathToUtf8(it->path().filename());
            std::error_code e2;
            bool isDir = it->is_directory(e2);
            if (isDir && hiddenDir(n)) continue;
            if (L.entries.size() >= kMaxDirEntries) {
                L.truncated = true;
                break;
            }
            L.entries.push_back({n, util::pathToUtf8(it->path()), isDir});
        }
        std::sort(L.entries.begin(), L.entries.end(), [](const DirEntry& a, const DirEntry& b) {
            if (a.dir != b.dir) return a.dir;
            return util::toLower(a.name) < util::toLower(b.name);
        });
    }
    const auto entries = L.entries;  // copie : la relecture d'un sous-dossier peut modifier la carte
    if (entries.empty()) ImGui::TextDisabled("(vide)");
    for (auto& e : entries) {
        if (e.dir) {
            ImGui::PushID(e.path.c_str());
            bool open = ImGui::GetStateStorage()->GetInt(ImGui::GetID("##d"), 0) != 0;
            bool o = ui::iconTreeNode("##d", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                                  ImGuiTreeNodeFlags_SpanAvailWidth,
                                      open ? ui::Icon::FolderOpen : ui::Icon::Folder, e.name.c_str());
            if (ImGui::BeginPopupContextItem("##dirctx")) {
                OpenConversation* oc = active();
                if (ImGui::MenuItem("Afficher dans l'Explorateur")) util::openInShell(e.path);
                if (ImGui::MenuItem("Joindre à la conversation active", nullptr, false, oc != nullptr)) attachFile(*oc, e.path);
                if (ImGui::MenuItem("Copier le chemin")) ImGui::SetClipboardText(e.path.c_str());
                ImGui::EndPopup();
            }
            if (o) {
                if (depth < 24) drawDirListing(e.path, depth + 1);
                ImGui::TreePop();
            }
            ImGui::PopID();
        } else {
            drawFileItem(e.path, nullptr);
        }
    }
    if (L.truncated) ImGui::TextDisabled("… (dossier trop grand : affichage limité à %d éléments)", (int)kMaxDirEntries);
}

void App::drawWorkDirNode(ws::Folder* project) {
    std::string cwd = project->effectiveCwd();
    if (cwd.empty()) return;
    std::error_code ec;
    if (!fs::is_directory(util::pathFromUtf8(cwd), ec)) return;
    ImGui::PushID("##workdir");
    std::string leaf = util::pathToUtf8(util::pathFromUtf8(cwd).filename());
    claude::ProjectKind pk = claude::projectKindFromId(project->projectKind);
    bool codeProject = project->projectKind.empty() || pk == claude::ProjectKind::Software || pk == claude::ProjectKind::Hybrid;
    std::string label = std::string(codeProject ? "Dossier de travail" : "Fichiers du projet") + (leaf.empty() ? std::string() : " · " + leaf);
    if (optPanel_ == "projets:fichiers") {  // capture de test : explorateur déplié, un fichier sélectionné
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (selectedFile_.empty()) {
            selectedFile_ = util::pathToUtf8(util::pathFromUtf8(cwd) / "CMakeLists.txt");
            selectedFileOwner_.clear();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ui::palette().dim);
    bool open = ui::iconTreeNode("##wd", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_SpanAvailWidth,
                                 ui::Icon::WorkDir, label.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s\n%s\nClic droit sur un fichier : le joindre à la conversation.", cwd.c_str(),
                          codeProject ? "Le code du projet : Claude y lit et modifie les fichiers."
                                      : "Les fichiers du projet (documents, données…) : Claude y lit et y écrit.");
    if (ImGui::BeginPopupContextItem("##wdctx")) {
        if (ImGui::MenuItem("Ouvrir dans l'Explorateur")) util::openInShell(cwd);
        if (ImGui::MenuItem("Copier le chemin")) ImGui::SetClipboardText(cwd.c_str());
        if (ImGui::MenuItem("Relire")) dirCache_.clear();
        ImGui::EndPopup();
    }
    if (open) {
        drawDirListing(cwd, 0);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void App::drawFolderNode(ws::Folder* f) {
    static std::string pendingDelete;
    const auto& pal = ui::palette();
    std::string path = util::pathToUtf8(f->path);
    ImGui::PushID(path.c_str());
    if (renameTarget_ == path && renameIsFolder_) {
        ImGui::SetNextItemWidth(-1);
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename", &renameBuffer_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            std::string err;
            std::string old = path;
            saveAll();
            if (workspace_.renameFolder(f, renameBuffer_, &err)) {
                fixOpenPaths(open_, old, util::pathToUtf8(f->path));
                if (util::startsWith(selectedFolder_, old)) selectedFolder_ = util::pathToUtf8(f->path) + selectedFolder_.substr(old.size());
            } else {
                toast(err, true);
            }
            renameTarget_.clear();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) renameTarget_.clear();
        ImGui::PopID();
        return;
    }
    bool agent = isAgentFolder(f);
    // Agent : un clic le déplie (ses discussions), un double-clic ouvre sa conversation actuelle.
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!agent) fl |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (selectedFolder_ == path && selectedConv_.empty() && selectedFile_.empty()) fl |= ImGuiTreeNodeFlags_Selected;
    if (f->isProject || (!agent && f->parent && f->parent->isProject)) fl |= ImGuiTreeNodeFlags_DefaultOpen;
    int total = f->totalConversations();
    int discussions = ws::countDiscussions(*f);
    std::string label = f->name + (discussions && !agent ? "  (" + std::to_string(discussions) + ")" : "");
    const bool agentLive = agent && orch_ && orchProject_ == util::pathToUtf8(f->parent->path);
    AgentView av;
    if (agentLive) av = agentView(f->name);
    bool agentWorking = agentLive && av.working;
    if (agent && optPanel_ == "projets:sessions" && total > discussions) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    bool wasOpen = ImGui::GetStateStorage()->GetInt(ImGui::GetID("##folder"), (fl & ImGuiTreeNodeFlags_DefaultOpen) ? 1 : 0) != 0;
    ui::Icon ic = f->isProject ? ui::Icon::Project : agent ? ui::Icon::Agent : wasOpen ? ui::Icon::FolderOpen : ui::Icon::Folder;
    if (f->isProject) ImGui::PushFont(fontBold, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, f->isProject ? pal.accent : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    bool open = ui::iconTreeNode("##folder", fl, ic, label.c_str(), 0, agentWorking ? 1.0f : 0.0f);
    ImGui::PopStyleColor();
    if (f->isProject) ImGui::PopFont();
    {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        dropZones_.push_back({a.x, a.y, b.x, b.y, path});
    }
    if (ImGui::IsItemClicked() && (agent || !ImGui::IsItemToggledOpen())) {
        selectedFolder_ = path;
        selectedConv_.clear();
        selectedFile_.clear();
    }
    if (agent && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
        // Conversation actuelle de l'agent : celle que l'orchestrateur lui a confiée, sinon sa discussion la plus récente
        std::string id = agentLive ? av.convId : std::string();
        if (id.empty()) {
            auto chains = ws::conversationChains(*f);
            if (!chains.empty()) id = chains.front().latest()->id;
        }
        if (!id.empty()) openConversation(id);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::BeginTooltip();
        ui::icon(ic);
        ImGui::SameLine();
        ImGui::TextUnformatted(f->displayPath().c_str());
        if (f->isProject && !f->projectKind.empty()) {
            ui::icon(ui::projectKindIcon(claude::projectKindFromId(f->projectKind)));
            ImGui::SameLine();
            ImGui::TextUnformatted(claude::projectKindInfo(claude::projectKindFromId(f->projectKind)).label);
        }
        ImGui::TextDisabled("%s", f->isProject ? "Projet : son dossier de fichiers, ses thèmes et ses conversations."
                                  : agent    ? "Agent de l'orchestrateur : sa fiche de rôle est dans les notes du thème."
                                             : "Thème : regroupe les conversations d'un même sujet.");
        if (agentLive && av.task) ImGui::TextColored(av.color, "%s", av.label.c_str());
        ImGui::TextDisabled("%d discussion(s)%s%s", discussions,
                            total > discussions ? (" · " + std::to_string(total) + " sessions en tout").c_str() : "",
                            f->files.empty() ? "" : (" · " + std::to_string(f->files.size()) + " fichier(s)").c_str());
        if (total > discussions)
            ImGui::TextDisabled("Une discussion trop longue est résumée et poursuivie dans une nouvelle session :\n"
                                "les sessions d'une même discussion sont regroupées sous la plus récente.");
        if (agent) ImGui::TextDisabled("Clic : afficher ses discussions · double-clic : ouvrir sa conversation actuelle");
        ImGui::EndTooltip();
    }
    if (agent) {
        // Statut de l'agent : celui de sa tâche, quand l'orchestrateur du projet est chargé (le statut enregistré dans
        // la conversation, « En cours », resterait affiché une fois l'orchestration finie)
        if (agentLive && av.task) {
            ImGui::SameLine();
            ui::badge(av.label.c_str(), av.color);
        }
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kConvPayload)) {
            std::string id((const char*)pl->Data);
            if (workspace_.moveConversation(id, f))
                for (auto& oc : open_)
                    if (oc->session->localId == id) oc->folderPath = path;
        }
        ImGui::EndDragDropTarget();
    }
    bool removed = false;
    if (ImGui::BeginPopupContextItem("##folderctx")) {
        if (ImGui::MenuItem("Nouvelle conversation ici")) newConversation(f);
        if (ImGui::MenuItem(f->isProject ? "Nouveau thème" : "Nouveau sous-dossier")) {
            std::string err;
            if (ws::Folder* nf = workspace_.createFolder(f, f->isProject ? "Nouveau thème" : "Nouveau dossier", &err)) {
                renameTarget_ = util::pathToUtf8(nf->path);
                renameIsFolder_ = true;
                renameBuffer_ = nf->name;
            } else {
                toast(err, true);
            }
        }
        if (ImGui::MenuItem("Ajouter des fichiers…")) {
            openFileDialog([this, path](const std::vector<std::string>& files) { addFilesTo(path, files); });
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copie des documents de référence (cahier des charges, schémas…) dans ce dossier.\n"
                              "Claude reçoit leur chemin dans les nouvelles conversations du dossier.\n"
                              "Astuce : vous pouvez aussi les glisser depuis l'Explorateur sur le dossier.");
        if (ImGui::MenuItem("Renommer")) {
            renameTarget_ = path;
            renameIsFolder_ = true;
            renameBuffer_ = f->name;
        }
        if (ImGui::MenuItem("Générer la synthèse du dossier")) requestFolderRecap(f);
        if (ImGui::MenuItem("Récaps manquants des conversations")) {
            std::function<void(ws::Folder*)> rec = [&](ws::Folder* x) {
                for (auto& c : x->conversations)
                    if (c.recap.empty() || c.recapTime < c.updated - 60) requestRecap(c.id);
                for (auto& ch : x->children) rec(ch.get());
            };
            rec(f);
        }
        if (ImGui::MenuItem("Ouvrir dans l'Explorateur")) util::openInShell(path);
        if (!f->effectiveCwd().empty() && ImGui::MenuItem("Ouvrir le dossier de travail")) util::openInShell(f->effectiveCwd());
        ImGui::Separator();
        if (ImGui::MenuItem("Supprimer (corbeille)")) pendingDelete = path;
        ImGui::EndPopup();
    }
    // Confirmation de suppression (demandée depuis le menu contextuel)
    if (pendingDelete == path) {
        ImGui::OpenPopup("##confirmdel");
        pendingDelete.clear();
    }
    if (ImGui::BeginPopup("##confirmdel")) {
        ImGui::Text("Déplacer « %s » et ses %d conversation(s) dans la corbeille ?", f->name.c_str(), total);
        if (ImGui::Button("Supprimer")) {
            for (int i = (int)open_.size() - 1; i >= 0; --i)
                if (util::startsWith(open_[i]->folderPath, path)) closeConversation(i);
            removed = workspace_.removeFolder(f);
            if (!removed) toast("Suppression impossible (fichier ouvert ailleurs ?)", true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Annuler")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (removed) {
        if (open) ImGui::TreePop();
        ImGui::PopID();
        return;
    }
    if (open) {
        const bool orchestrated = f->isProject && projectOrchestrated(f);
        if (orchestrated) {
            // Projet orchestré : Orchestrateur › agents › discussions, puis le reste du projet
            drawOrchestratorNode(f);
        } else {
            if (f->isProject) drawWorkDirNode(f);
            for (size_t i = 0; i < f->children.size(); ++i) drawFolderNode(f->children[i].get());
        }
        // Discussions du dossier (les sessions d'une même discussion regroupées) ; celle de l'orchestrateur est
        // déjà sous son nœud.
        for (const auto& c : ws::conversationChains(*f)) {
            if (orchestrated && isBrainConversation(f, *c.latest())) continue;
            if (!drawChain(f, c)) break;
        }
        if (orchestrated) drawWorkDirNode(f);
        for (size_t i = 0; i < f->files.size(); ++i) {
            size_t before = f->files.size();
            drawFileItem(util::pathToUtf8(f->files[i]), f);
            if (f->files.size() != before) break;
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// Inspecteur d'un fichier (document de référence ou fichier du dossier de travail).
void App::drawFileInspector() {
    const auto& pal = ui::palette();
    fs::path p = util::pathFromUtf8(selectedFile_);
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        selectedFile_.clear();
        return;
    }
    std::string name = util::pathToUtf8(p.filename());
    ui::icon(ui::fileIcon(name));
    ImGui::SameLine();
    ImGui::PushFont(fontBold, 0.0f);
    ImGui::TextWrapped("%s", name.c_str());
    ImGui::PopFont();
    ws::Folder* owner = selectedFileOwner_.empty() ? nullptr : workspace_.findFolderByPath(util::pathFromUtf8(selectedFileOwner_));
    if (owner) ImGui::TextDisabled("Document de référence · %s", owner->displayPath().c_str());
    else ImGui::TextDisabled("Fichier du dossier de travail");
    uintmax_t size = fs::file_size(p, ec);
    ImGui::TextDisabled("%s", ec ? "" : humanSize(size).c_str());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", selectedFile_.c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Ouvrir")) util::openInShell(selectedFile_);
    ImGui::SameLine();
    OpenConversation* oc = active();
    ImGui::BeginDisabled(!oc);
    if (ImGui::Button("Joindre à la conversation")) attachFile(*oc, selectedFile_);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(oc ? "Image : jointe au message. Autre fichier : sa référence @chemin est ajoutée au message."
                             : "Ouvrez d'abord une conversation.");
    if (owner) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(pal.dim, "Claude reçoit le chemin de ce fichier dans les nouvelles conversations de « %s » "
                                    "et le lit quand il en a besoin.",
                           owner->name.c_str());
        ImGui::PopTextWrapPos();
    }
    // Aperçu des fichiers texte (début du fichier)
    ui::Icon ic = ui::fileIcon(name);
    if ((ic == ui::Icon::FileCode || ic == ui::Icon::FileText || ic == ui::Icon::FileData) && !ec && size < 4 * 1024 * 1024) {
        static std::string cachedPath, cachedText;
        static double cachedAt = -1e9;
        if (cachedPath != selectedFile_ || now_ - cachedAt > 3.0) {
            std::string all;
            util::readFile(p, all);
            cachedText = util::truncateUtf8(all, 12000);
            if (cachedText.size() < all.size()) cachedText += "\n…";
            cachedPath = selectedFile_;
            cachedAt = now_;
        }
        ImGui::SeparatorText("Aperçu");
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(fontMono, 0.0f);
        ImGui::TextUnformatted(cachedText.c_str());
        ImGui::PopFont();
        ImGui::EndChild();
    }
}

void App::drawInspector() {
    if (!ImGui::Begin("Inspecteur", &showInspector_)) {
        ImGui::End();
        return;
    }
    const auto& pal = ui::palette();
    ui::MdStyle md{fontUi, fontBold, fontMono, settings_.wrapCode, fontItalic};

    if (!selectedFile_.empty()) {
        drawFileInspector();
        if (!selectedFile_.empty()) {
            ImGui::End();
            return;
        }
    }
    if (!selectedConv_.empty()) {
        auto [folder, m] = workspace_.findConversation(selectedConv_);
        if (!m) {
            selectedConv_.clear();
        } else {
            ui::icon(isBrainConversation(folder, *m) ? ui::Icon::Orchestrator : ui::Icon::Chat);
            ImGui::SameLine();
            ImGui::PushFont(fontBold, 0.0f);
            ImGui::TextWrapped("%s", m->title.c_str());
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
            ImGui::TextWrapped("%s", folder->displayPath().c_str());
            std::string facts = std::to_string(m->messages) + " message" + (m->messages > 1 ? "s" : "");
            if (!m->model.empty()) facts += " · " + claude::modelDisplayName(m->model);
            facts += " · " + util::humanDate(m->updated);
            ImGui::TextWrapped("%s", facts.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Créée %s · modifiée %s\n%s en équivalent API", util::humanDate(m->created).c_str(),
                                  util::humanDate(m->updated).c_str(), util::formatUsd(m->costUsd).c_str());
            // Session d'une discussion : « Session 2/3 ‹ › » (la navigation prend effet à l'image suivante)
            {
                std::string prevId, nextId;
                int rank = -1, count = 0;
                for (const auto& c : ws::conversationChains(*folder)) {
                    int k = c.indexOf(m->id);
                    if (k < 0) continue;
                    rank = k, count = c.size();
                    if (k > 0) prevId = c.sessions[k - 1]->id;
                    if (k + 1 < c.size()) nextId = c.sessions[k + 1]->id;
                }
                if (count > 1) {
                    ui::icon(ui::Icon::History);
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(pal.link, "Session %d/%d", rank + 1, count);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\nQuand une discussion devient trop longue, elle est résumée et poursuivie dans\n"
                                          "une nouvelle session qui part de ce résumé.",
                                          rank + 1 == count ? "La session actuelle de la discussion." : "Une session précédente (résumée).");
                    ImGui::SameLine();
                    ImGui::BeginDisabled(prevId.empty());
                    if (ui::chip("‹##prevsess")) selectedConv_ = prevId;
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Session précédente");
                    ImGui::SameLine(0, 2);
                    ImGui::BeginDisabled(nextId.empty());
                    if (ui::chip("›##nextsess")) selectedConv_ = nextId;
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Session suivante");
                }
            }
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            bool resume = ImGui::Button("Reprendre");
            ImGui::PopStyleColor(2);
            if (resume) openConversation(m->id);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Rouvre la conversation : votre prochain message reprend la session Claude Code\n"
                                  "avec tout son historique.");
            ui::sameLineIfFits(ui::buttonWidth("↻ Repartir du récap"));
            ImGui::BeginDisabled(restartPending(m->id));
            if (ui::chip("↻ Repartir du récap", &pal.dim)) {
                std::string id = m->id;  // `m` peut être invalidé par l'ouverture
                if (openConversation(id, false)) restartFromRecap(id);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Remet le contexte à zéro : une conversation neuve reçoit le récap de celle-ci, avec les\n"
                                  "mêmes réglages. Plus léger que tout l'historique.");
            ImGui::SeparatorText("Récap");
            bool generating = false;
            for (auto& j : jobs_)
                if (j->target == m->id) generating = true;
            if (generating) {
                ui::spinner((float)now_, pal.accent);
                ImGui::SameLine();
                ImGui::TextDisabled("Génération du récap…");
            } else if (m->recap.empty()) {
                ImGui::TextDisabled("Pas encore de récap.");
                if (ui::chip("Générer le récap", &pal.link)) requestRecap(m->id);
            } else {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", util::humanDate(m->recapTime).c_str());
                ui::sameLineIfFits(ui::buttonWidth("Régénérer"));
                if (ui::chip("Régénérer", &pal.dim)) requestRecap(m->id);
                ImGui::SameLine(0, 2);
                if (ui::chip("Copier", &pal.dim)) ImGui::SetClipboardText(m->recap.c_str());
                ImGui::BeginChild("##recap", ImVec2(0, 0), ImGuiChildFlags_None);
                ui::renderMarkdown(ui::parseMarkdown(m->recap), md, 77777);
                ImGui::EndChild();
            }
        }
    }
    if (selectedConv_.empty()) {
        ws::Folder* f = workspace_.findFolderByPath(util::pathFromUtf8(selectedFolder_));
        if (!f) {
            ImGui::TextWrapped("Sélectionnez un projet, un thème, une conversation ou un fichier dans le panneau Projets "
                               "pour voir ses notes, sa synthèse, son récap ou son contenu.");
            ImGui::End();
            return;
        }
        ui::icon(f->isProject ? ui::Icon::Project : isAgentFolder(f) ? ui::Icon::Agent : ui::Icon::Folder);
        ImGui::SameLine();
        ImGui::PushFont(fontBold, 0.0f);
        ImGui::TextColored(pal.accent, "%s", f->displayPath().c_str());
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
        if (isAgentFolder(f)) ImGui::TextWrapped("Agent de l'orchestrateur : sa fiche de rôle est dans les notes ci-dessous.");
        ImGui::TextWrapped("%d discussion(s) · activité %s", ws::countDiscussions(*f), util::humanDate(f->lastActivity()).c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (ui::chip("+ Conversation ici", &pal.link)) newConversation(f);
        ui::sameLineIfFits(ui::buttonWidth("Synthèse du dossier"));
        const FolderSynthesis* syn = synthesisFor(f);
        bool synRunning = syn != nullptr;
        for (auto& j : jobs_)
            if (j->target == util::pathToUtf8(f->path)) synRunning = true;
        ImGui::BeginDisabled(synRunning);
        if (ui::chip("Synthèse du dossier", &pal.dim)) requestFolderRecap(f);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Génère d'abord le récap des conversations qui n'en ont pas (ou dont le récap est\n"
                              "dépassé), puis synthétise tous les récaps du dossier (et sous-dossiers).\n"
                              "La synthèse est injectée dans le contexte des nouvelles conversations.");
        if (synRunning) {
            ui::spinner((float)now_, ui::palette().accent);
            ImGui::SameLine();
            if (syn)
                ImGui::TextDisabled("Synthèse en préparation : récaps %d / %d…",
                                    syn->total - (int)syn->toStart.size() - (int)syn->running.size(), syn->total);
            else
                ImGui::TextDisabled("Synthèse en cours…");
        }
        if (f->isProject && f->name != "Divers") {
            ui::sameLineIfFits(ui::buttonWidth("Orchestrer…"));
            if (ui::chip("Orchestrer…", &pal.dim)) {
                if (!orch_ || !orch_->running()) loadOrchestrator(f);
                page_ = 2;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Chaque thème du projet devient un agent ; une conversation « Orchestrateur »\n"
                                  "leur délègue le travail et vérifie, jusqu'à atteindre l'objectif.");
        }
        if (f->isProject && f->name != "Divers") {
            ImGui::SeparatorText("Type de projet");
            claude::ProjectKind cur = claude::projectKindFromId(f->projectKind.empty() ? "software" : f->projectKind);
            ui::icon(ui::projectKindIcon(cur));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::min(ImGui::GetFontSize() * 16, ImGui::GetContentRegionAvail().x));
            if (ImGui::BeginCombo("##ptype", claude::projectKindInfo(cur).label)) {
                for (claude::ProjectKind k : claude::allProjectKinds()) {
                    if (k == claude::ProjectKind::Question) continue;
                    const auto& info = claude::projectKindInfo(k);
                    if (ImGui::Selectable(info.label, k == cur)) {
                        f->projectKind = info.id;
                        workspace_.saveFolderMeta(f);
                        // L'orchestrateur du projet suit (s'il est chargé et ne travaille pas)
                        if (orch_ && orchProject_ == util::pathToUtf8(f->path) && !orch_->running()) {
                            orch_->settings.projectKind = info.id;
                            orch_->changed = true;
                            saveOrchestrator();
                        }
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.tagline);
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ui::helpMarker("Le type guide Claude dans ce projet : méthode de travail, façon de vérifier (tests pour du\n"
                           "code, relecture et sources pour des documents, scripts relancés pour des données) et livrables.\n"
                           "Il s'applique aux nouvelles conversations et à l'orchestrateur.");
        }
        if (f->isProject) {
            bool codeKind = f->projectKind.empty() || f->projectKind == "software" || f->projectKind == "hybrid";
            ImGui::SeparatorText(codeKind ? "Dossier de travail du projet" : "Dossier des fichiers du projet");
            ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 6);
            if (ImGui::InputText("##pcwd", &f->cwd, ImGuiInputTextFlags_EnterReturnsTrue)) workspace_.saveFolderMeta(f);
            if (ImGui::IsItemDeactivatedAfterEdit()) workspace_.saveFolderMeta(f);
            ImGui::SameLine();
            if (ImGui::Button("Parcourir…")) {
                std::string p = util::pathToUtf8(f->path);
                openFolderDialog([this, p](const std::string& dir) {
                    if (ws::Folder* x = workspace_.findFolderByPath(util::pathFromUtf8(p))) {
                        x->cwd = dir;
                        workspace_.saveFolderMeta(x);
                    }
                }, f->cwd);
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
            ImGui::TextWrapped("Dossier de travail : %s", f->effectiveCwd().empty() ? "(par défaut)" : f->effectiveCwd().c_str());
            ImGui::PopStyleColor();
            if (ImGui::TreeNode("Dossier de travail spécifique")) {
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##fcwd", "hérité du projet si vide", &f->cwd);
                if (ImGui::IsItemDeactivatedAfterEdit()) workspace_.saveFolderMeta(f);
                ImGui::TreePop();
            }
        }
        ImGui::SeparatorText("Notes");
        ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
        ImGui::TextWrapped("Données à Claude au début de chaque nouvelle conversation de ce dossier.");
        ImGui::PopStyleColor();
        ImGui::InputTextMultiline("##notes", &f->notes, ImVec2(-1, ImGui::GetFontSize() * 7),
                                  ImGuiInputTextFlags_WordWrap | ImGuiInputTextFlags_AllowTabInput);
        if (ImGui::IsItemDeactivatedAfterEdit()) workspace_.saveFolderMeta(f);
        ImGui::SeparatorText("Synthèse");
        bool generating = false;
        for (auto& j : jobs_)
            if (j->target == util::pathToUtf8(f->path)) generating = true;
        if (generating) {
            ui::spinner((float)now_, pal.accent);
            ImGui::SameLine();
            ImGui::TextDisabled("Génération…");
        } else if (f->recap.empty()) {
            ImGui::TextDisabled("Aucune synthèse.");
        } else {
            ImGui::TextDisabled("Générée %s", util::humanDate(f->recapTime).c_str());
            ImGui::BeginChild("##frecap", ImVec2(0, 0));
            ui::renderMarkdown(ui::parseMarkdown(f->recap), md, 88888);
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

void App::drawNewProjectPopup() {
    if (openNewProject_) {
        ImGui::OpenPopup("Nouveau projet");
        openNewProject_ = false;
        npName_.clear();
        npCwd_.clear();
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Nouveau projet", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextDisabled("Un projet = un dossier de fichiers pour Claude, découpé en thèmes qui regroupent vos échanges.");
    // Type de projet : il règle les thèmes proposés et la façon de travailler de Claude
    {
        claude::ProjectKind cur = claude::projectKindFromId(npKind_);
        ui::icon(ui::projectKindIcon(cur));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
        if (ImGui::BeginCombo("Type", claude::projectKindInfo(cur).label)) {
            for (claude::ProjectKind k : claude::allProjectKinds()) {
                if (k == claude::ProjectKind::Question) continue;
                const auto& info = claude::projectKindInfo(k);
                if (ImGui::Selectable(info.label, k == cur)) {
                    npKind_ = info.id;
                    npFolders_.clear();
                    for (auto t : info.themes) npFolders_ += (npFolders_.empty() ? "" : ", ") + std::string(t);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", info.tagline);
            }
            ImGui::EndCombo();
        }
    }
    const bool npCode = claude::projectKindFromId(npKind_) == claude::ProjectKind::Software;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("Nom", npCode ? "ex. Platformer2D" : "ex. Étude de marché 2026", &npName_);
    std::string autoDir = util::pathToUtf8(util::documentsDir() / "Projets" /
                                           util::pathFromUtf8(util::sanitizeFileName(util::trim(npName_).empty() ? "Nouveau projet" : util::trim(npName_))));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
    ImGui::InputTextWithHint("##cwd", npCode ? "C:\\Dev\\MonProjet — vide : créé dans Documents\\Projets"
                                             : "Vide : un dossier est créé dans Documents\\Projets",
                             &npCwd_);
    ImGui::SameLine();
    if (ImGui::Button("Parcourir…")) openFolderDialog([this](const std::string& p) {
        npCwd_ = p;
        if (npName_.empty()) npName_ = util::pathToUtf8(util::pathFromUtf8(p).filename());
    });
    ImGui::SameLine();
    ImGui::TextUnformatted(npCode ? "Dossier de travail" : "Dossier du projet");
    if (npCwd_.empty()) ImGui::TextDisabled("Sera créé : %s", autoDir.c_str());
    if (!settings_.recentCwds.empty() && ImGui::BeginCombo("Récents", "Choisir un dossier récent…")) {
        for (auto& c : settings_.recentCwds)
            if (ImGui::Selectable(c.c_str())) {
                npCwd_ = c;
                if (npName_.empty()) npName_ = util::pathToUtf8(util::pathFromUtf8(c).filename());
            }
        ImGui::EndCombo();
    }
    ImGui::InputText("Thèmes", &npFolders_);
    ui::helpMarker("Sous-dossiers créés d'office, séparés par des virgules (modifiable ensuite).");
    std::error_code ec;
    bool cwdOk = npCwd_.empty() || fs::is_directory(util::pathFromUtf8(npCwd_), ec);
    if (!cwdOk) ImGui::TextColored(ui::palette().err, "Ce dossier n'existe pas.");
    ImGui::BeginDisabled(util::trim(npName_).empty() || !cwdOk);
    if (ImGui::Button("Créer", ImVec2(ImGui::GetFontSize() * 8, 0))) {
        std::string err;
        std::string cwd = npCwd_;
        if (cwd.empty()) {  // dossier créé pour l'utilisateur, plutôt que de laisser Claude travailler n'importe où
            std::error_code mk;
            fs::create_directories(util::pathFromUtf8(autoDir), mk);
            cwd = autoDir;
        }
        if (ws::Folder* p = workspace_.createProject(util::trim(npName_), cwd, &err)) {
            p->projectKind = npKind_;
            workspace_.saveFolderMeta(p);
            for (auto& t : util::split(npFolders_, ','))
                if (!util::trim(t).empty()) workspace_.createFolder(p, util::trim(t));
            selectedFolder_ = util::pathToUtf8(p->path);
            selectedConv_.clear();
            settings_.pushRecentCwd(cwd);
            ImGui::CloseCurrentPopup();
        } else {
            toast(err, true);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Annuler")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::drawHistoryImport() {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 50, ImGui::GetFontSize() * 30), ImGuiCond_FirstUseEver);
    // À l'ouverture, la fenêtre passe au premier plan (elle est ancrée derrière les conversations).
    // (plusieurs images de suite : l'éditeur peut reprendre le focus juste à l'ouverture)
    static int lastShownFrame = -10, firstShownFrame = 0;
    int frameNo = ImGui::GetFrameCount();
    if (frameNo - lastShownFrame > 1) firstShownFrame = frameNo;
    if (frameNo - firstShownFrame < 3) ImGui::SetNextWindowFocus();
    lastShownFrame = frameNo;
    if (!ImGui::Begin("Importer depuis Claude Code", &showHistory_)) {
        ImGui::End();
        return;
    }
    if (histProjects_.empty()) histProjects_ = claude::listHistoryProjects();
    if (histProject_ < 0 && !histProjects_.empty()) {  // le dossier le plus récent par défaut
        histProject_ = 0;
        histSessions_ = claude::listHistorySessions(histProjects_[0].dirName);
    }
    ws::Folder* dest = workspace_.findFolderByPath(util::pathFromUtf8(selectedFolder_));
    ImGui::TextWrapped("Sessions enregistrées par Claude Code (%s). Une session importée garde son identifiant : "
                       "votre prochain message la reprend exactement là où elle s'était arrêtée.",
                       util::pathToUtf8(util::claudeConfigDir() / "projects").c_str());
    ImGui::TextColored(ui::palette().accent, "Destination : %s", dest ? dest->displayPath().c_str() : "Divers (sélectionnez un dossier dans l'arbre)");

    ImGui::BeginChild("##hp", ImVec2(ImGui::GetFontSize() * 16, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    for (int i = 0; i < (int)histProjects_.size(); ++i) {
        auto& p = histProjects_[i];
        std::string label = (p.cwd.empty() ? p.dirName : p.cwd) + "  (" + std::to_string(p.sessions) + ")";
        if (ImGui::Selectable(label.c_str(), histProject_ == i)) {
            histProject_ = i;
            histSessions_ = claude::listHistorySessions(p.dirName);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dernière activité : %s", util::humanDate(p.lastModified).c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##hs", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (histProject_ < 0) {
        ImGui::TextDisabled("← Choisissez un dossier.");
    } else {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##hf", "Filtrer…", &histFilter_);
        if (ImGui::BeginTable("##sessions", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Titre", ImGuiTableColumnFlags_WidthStretch);
            float pad = ImGui::GetStyle().FramePadding.x * 2 + 8;
            ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("aujourd'hui 00:00").x + 8);
            ImGui::TableSetupColumn("Msg", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Msg 000").x);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Importer").x + pad);
            ImGui::TableHeadersRow();
            for (auto& hs : histSessions_) {
                if (!histFilter_.empty() && !util::containsInsensitive(hs.title, histFilter_) &&
                    !util::containsInsensitive(hs.firstPrompt, histFilter_))
                    continue;
                ImGui::PushID(hs.sessionId.c_str());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(hs.title.c_str());
                if (ImGui::IsItemHovered() && !hs.firstPrompt.empty()) ImGui::SetTooltip("%s", hs.firstPrompt.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", util::humanDate(hs.end).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%d", hs.userMessages);
                ImGui::TableSetColumnIndex(3);
                if (ImGui::SmallButton("Importer")) {
                    openImported(hs, dest);
                    toast("Importée : " + hs.title);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace app
