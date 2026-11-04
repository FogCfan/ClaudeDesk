// Espace de travail « cowork » : projets › thèmes › conversations, inspecteur
// (notes, synthèses, récaps) et import des sessions de Claude Code.
#include <algorithm>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/App.h"
#include "ui/Theme.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace app {

namespace {

const char* kConvPayload = "CD_CONV";

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

}  // namespace

void App::drawWorkspacePanel() {
    if (!ImGui::Begin("Espace de travail", &showWorkspace_)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("+ Projet")) openNewProject_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Créer un projet (un dossier de travail + des thèmes)");
    ImGui::SameLine();
    if (ImGui::Button("Importer")) {
        showHistory_ = true;
        histProjects_ = claude::listHistoryProjects();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Importer des sessions faites dans le terminal (claude)");
    ImGui::SameLine();
    if (ImGui::Button("⟳")) workspace_.rescan();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Relire le dossier de l'espace de travail");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Rechercher (titre, récap)…", &treeFilter_);

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
            for (auto& ch : f->children) search(ch.get());
        };
        for (auto& p : workspace_.projects()) search(p.get());
    } else {
        if (workspace_.projects().empty())
            ImGui::TextWrapped("Aucun projet. Créez-en un avec « + Projet » : un projet = un dossier de travail, "
                               "découpé en thèmes (Code, Infra, Docs…) qui regroupent vos conversations.");
        for (size_t i = 0; i < workspace_.projects().size(); ++i) {
            drawFolderNode(workspace_.projects()[i].get());
            // la liste a pu changer (suppression) : on arrête l'itération
            if (i >= workspace_.projects().size()) break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void App::drawConversationItem(ws::Folder* f, ws::ConversationMeta& m) {
    const auto& pal = ui::palette();
    bool isOpen = false, busy = false;
    for (auto& oc : open_)
        if (oc->session->localId == m.id) {
            isOpen = true;
            busy = oc->session->state == claude::SessionState::Busy;
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
        return;
    }
    std::string label = std::string(m.pinned ? "★ " : "") + (busy ? "● " : "") + m.title;
    bool selected = selectedConv_ == m.id;
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected) fl |= ImGuiTreeNodeFlags_Selected;
    if (isOpen) ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
    ImGui::TreeNodeEx("##conv", fl, "%s", label.c_str());
    if (isOpen) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selectedConv_ = m.id;
        selectedFolder_ = util::pathToUtf8(f->path);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) openConversation(m.id);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(m.title.c_str());
        ImGui::TextDisabled("%s · %d messages · %s", util::humanDate(m.updated).c_str(), m.messages, util::formatUsd(m.costUsd).c_str());
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
    if (!m.status.empty()) {
        ImGui::SameLine();
        ui::badge(ws::statusLabel(m.status), statusColor(m.status));
    }
    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload(kConvPayload, m.id.c_str(), m.id.size() + 1);
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
            ImGui::PopID();
            return;  // `m` n'existe plus
        }
        ImGui::EndPopup();
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
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selectedFolder_ == path && selectedConv_.empty()) fl |= ImGuiTreeNodeFlags_Selected;
    if (f->isProject || (f->parent && f->parent->isProject)) fl |= ImGuiTreeNodeFlags_DefaultOpen;
    int total = f->totalConversations();
    std::string label = f->name + (total ? "  (" + std::to_string(total) + ")" : "");
    if (f->isProject) ImGui::PushFont(fontBold, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, f->isProject ? pal.accent : ImGui::GetStyleColorVec4(ImGuiCol_Text));
    bool open = ImGui::TreeNodeEx("##folder", fl, "%s %s", f->isProject ? "◆" : "▪", label.c_str());
    ImGui::PopStyleColor();
    if (f->isProject) ImGui::PopFont();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selectedFolder_ = path;
        selectedConv_.clear();
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
        if (ImGui::MenuItem("Nouveau sous-dossier")) {
            std::string err;
            if (ws::Folder* nf = workspace_.createFolder(f, "Nouveau dossier", &err)) {
                renameTarget_ = util::pathToUtf8(nf->path);
                renameIsFolder_ = true;
                renameBuffer_ = nf->name;
            } else {
                toast(err, true);
            }
        }
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
        for (size_t i = 0; i < f->children.size(); ++i) drawFolderNode(f->children[i].get());
        for (size_t i = 0; i < f->conversations.size(); ++i) {
            size_t before = f->conversations.size();
            drawConversationItem(f, f->conversations[i]);
            if (f->conversations.size() != before) break;
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void App::drawInspector() {
    if (!ImGui::Begin("Inspecteur", &showInspector_)) {
        ImGui::End();
        return;
    }
    const auto& pal = ui::palette();
    ui::MdStyle md{fontUi, fontBold, fontMono, settings_.wrapCode, fontItalic};

    if (!selectedConv_.empty()) {
        auto [folder, m] = workspace_.findConversation(selectedConv_);
        if (!m) {
            selectedConv_.clear();
        } else {
            ImGui::PushFont(fontBold, 0.0f);
            ImGui::TextWrapped("%s", m->title.c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("%s", folder->displayPath().c_str());
            ImGui::TextDisabled("Créée %s · modifiée %s", util::humanDate(m->created).c_str(), util::humanDate(m->updated).c_str());
            ImGui::TextDisabled("%d messages · %s%s", m->messages, util::formatUsd(m->costUsd).c_str(),
                                m->model.empty() ? "" : (" · " + m->model).c_str());
            if (ImGui::Button("Reprendre")) openConversation(m->id);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Rouvre la conversation : votre prochain message reprend la session Claude Code\n"
                                  "avec tout son historique (--resume).");
            ImGui::SameLine();
            if (ImGui::Button("Repartir de ce contexte")) {
                OpenConversation* oc = newConversation(folder);
                oc->injectContext = true;
                if (m->recap.empty()) requestRecap(m->id);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Nouvelle conversation dans le même dossier, qui reçoit les récaps du dossier\n"
                                  "(dont celui-ci) au lieu de tout l'historique : plus léger en contexte.");
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
                if (ImGui::Button("Générer le récap")) requestRecap(m->id);
            } else {
                ImGui::TextDisabled("Généré %s", util::humanDate(m->recapTime).c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Régénérer")) requestRecap(m->id);
                ImGui::SameLine();
                if (ImGui::SmallButton("Copier")) ImGui::SetClipboardText(m->recap.c_str());
                ImGui::BeginChild("##recap", ImVec2(0, 0), ImGuiChildFlags_None);
                ui::renderMarkdown(ui::parseMarkdown(m->recap), md, 77777);
                ImGui::EndChild();
            }
        }
    }
    if (selectedConv_.empty()) {
        ws::Folder* f = workspace_.findFolderByPath(util::pathFromUtf8(selectedFolder_));
        if (!f) {
            ImGui::TextWrapped("Sélectionnez un projet, un dossier ou une conversation dans l'espace de travail pour voir "
                               "ses notes, sa synthèse et ses récaps.");
            ImGui::End();
            return;
        }
        ImGui::PushFont(fontBold, 0.0f);
        ImGui::TextColored(pal.accent, "%s", f->displayPath().c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("%d conversations · activité %s", f->totalConversations(), util::humanDate(f->lastActivity()).c_str());
        if (ImGui::Button("Nouvelle conversation ici")) newConversation(f);
        ImGui::SameLine();
        if (ImGui::Button("Synthèse du dossier")) requestFolderRecap(f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Synthétise les récaps des conversations du dossier (et sous-dossiers).\n"
                              "Elle est injectée dans le contexte des nouvelles conversations.");
        if (f->isProject) {
            ImGui::SeparatorText("Dossier de travail du projet");
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
            ImGui::TextDisabled("Dossier de travail : %s", f->effectiveCwd().empty() ? "(par défaut)" : f->effectiveCwd().c_str());
            if (ImGui::TreeNode("Dossier de travail spécifique")) {
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##fcwd", "hérité du projet si vide", &f->cwd);
                if (ImGui::IsItemDeactivatedAfterEdit()) workspace_.saveFolderMeta(f);
                ImGui::TreePop();
            }
        }
        ImGui::SeparatorText("Notes");
        ImGui::TextDisabled("Injectées dans le contexte de chaque nouvelle conversation de ce dossier.");
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
    ImGui::TextDisabled("Un projet = un dossier de travail pour Claude, découpé en thèmes qui regroupent vos échanges.");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("Nom", "ex. Platformer2D", &npName_);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
    ImGui::InputTextWithHint("##cwd", "C:\\Dev\\MonProjet", &npCwd_);
    ImGui::SameLine();
    if (ImGui::Button("Parcourir…")) openFolderDialog([this](const std::string& p) {
        npCwd_ = p;
        if (npName_.empty()) npName_ = util::pathToUtf8(util::pathFromUtf8(p).filename());
    });
    ImGui::SameLine();
    ImGui::TextUnformatted("Dossier de travail");
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
        if (ws::Folder* p = workspace_.createProject(util::trim(npName_), npCwd_, &err)) {
            for (auto& t : util::split(npFolders_, ','))
                if (!util::trim(t).empty()) workspace_.createFolder(p, util::trim(t));
            selectedFolder_ = util::pathToUtf8(p->path);
            selectedConv_.clear();
            settings_.pushRecentCwd(npCwd_);
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
