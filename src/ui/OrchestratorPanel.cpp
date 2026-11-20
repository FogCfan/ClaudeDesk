// Fenêtre « Orchestrateur » : un projet cowork piloté automatiquement.
// Chaque thème (dossier) du projet est un agent ; une conversation « Orchestrateur »
// à la racine du projet décide, délègue et vérifie (voir claude/Orchestrator.h).
#include <algorithm>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/App.h"
#include "ui/Theme.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace app {

using json = nlohmann::json;

namespace {

const char* kOrchFile = "_orchestrateur.json";
const char* kDefaultObjective =
    "Terminer la construction du logiciel en suivant le plan du projet (docs/PLAN.md s'il existe), "
    "phase par phase, avec des tests qui passent à chaque étape.";

// Résumé du rôle d'un agent : premières lignes utiles des notes de son dossier.
std::string agentBrief(const std::string& notes) {
    std::string out;
    for (const std::string& raw : util::splitLines(notes)) {
        std::string l = util::trim(raw);
        if (l.empty() || l[0] == '#' || l[0] == '(' || l[0] == '|' || util::startsWith(l, "---")) continue;
        if (!out.empty()) out += " ";
        out += l;
        if (out.size() > 220) break;
    }
    return util::truncateUtf8(out, 240);
}

std::string clockOf(int64_t t) {
    std::string iso = util::isoFromUnix(t);
    return iso.size() >= 16 ? iso.substr(11, 5) : iso;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hôte : relie le moteur aux conversations de l'application
// ---------------------------------------------------------------------------

class AppOrchHost : public claude::OrchestratorHost {
public:
    explicit AppOrchHost(App& a) : app(a) {}
    App& app;

    OpenConversation* find(const std::string& id) {
        for (auto& oc : app.open_)
            if (oc->session->localId == id) return oc.get();
        return nullptr;
    }

    std::vector<claude::AgentInfo> agents() override {
        std::vector<claude::AgentInfo> v;
        if (ws::Folder* p = app.orchProjectFolder())
            for (auto& ch : p->children) v.push_back({ch->name, agentBrief(ch->notes)});
        return v;
    }

    std::string openBrain(const std::string& hint, const std::string& systemPrompt) override {
        OpenConversation* oc = nullptr;
        if (!hint.empty() && ensureOpen(hint)) oc = find(hint);
        if (!oc) {
            ws::Folder* p = app.orchProjectFolder();
            if (!p) return {};
            oc = app.newConversation(p, {}, false);
            if (!oc) return {};
            oc->session->title = "Orchestrateur";
            oc->meta.pinned = true;
            oc->meta.status = "active";
        }
        auto& s = *oc->session;
        if (s.config.systemPrompt != systemPrompt || s.config.systemPromptMode != claude::SystemPromptMode::Append) {
            claude::SessionConfig cfg = s.config;
            cfg.systemPromptMode = claude::SystemPromptMode::Append;
            cfg.systemPrompt = systemPrompt;
            if (s.processRunning()) s.applyConfig(cfg);
            else s.config = cfg;
        }
        return s.localId;
    }

    std::string openAgent(const std::string& agent, const std::string& hint) override {
        if (!hint.empty() && ensureOpen(hint)) return hint;
        ws::Folder* p = app.orchProjectFolder();
        if (!p) return {};
        ws::Folder* f = nullptr;
        for (auto& ch : p->children)
            if (ch->name == agent) f = ch.get();
        if (!f) return {};
        // Conversation existante du thème : épinglée d'abord, puis la plus récente.
        const ws::ConversationMeta* best = nullptr;
        for (auto& m : f->conversations)
            if (!best || (m.pinned && !best->pinned) || (m.pinned == best->pinned && m.updated > best->updated)) best = &m;
        if (best)
            if (OpenConversation* oc = app.openConversation(best->id, false)) return oc->session->localId;
        OpenConversation* oc = app.newConversation(f, {}, false);
        if (!oc) return {};
        oc->session->title = "Tâches de l'orchestrateur";
        return oc->session->localId;
    }

    bool ensureOpen(const std::string& id) override {
        if (id.empty()) return false;
        return find(id) || app.openConversation(id, false);
    }

    bool send(const std::string& id, const std::string& text, const std::string& mode) override {
        OpenConversation* oc = find(id);
        if (!oc || app.claudeExe_.empty()) return false;
        auto& s = *oc->session;
        if (!mode.empty() && s.config.permissionMode != mode) {
            if (s.processRunning()) s.setPermissionMode(mode);
            else s.config.permissionMode = mode;
        }
        if (oc->meta.status.empty() || oc->meta.status == "idea") oc->meta.status = "active";
        app.sendText(*oc, text, {}, false);
        return s.state == claude::SessionState::Busy || s.state == claude::SessionState::Starting;
    }

    claude::ParticipantStatus status(const std::string& id) override {
        claude::ParticipantStatus st;
        OpenConversation* oc = find(id);
        if (!oc) return st;
        auto& s = *oc->session;
        st.exists = true;
        st.busy = s.state == claude::SessionState::Busy || s.state == claude::SessionState::Starting || s.queuedMessages > 0;
        st.needsHuman = !s.permissions.empty();
        st.stopped = s.state == claude::SessionState::Stopped;
        st.turns = s.stats.turns;
        st.costUsd = s.stats.costUsd;
        if (!st.busy) st.lastText = claude::lastTurnText(s.entries);
        return st;
    }

    double fiveHourUtilization() override {
        const claude::RateWindow* w = app.usage_.find("five_hour");
        return w ? w->utilization : -1.0;
    }
    int64_t fiveHourResetsAt() override {
        const claude::RateWindow* w = app.usage_.find("five_hour");
        return w ? w->resetsAt : 0;
    }
    int64_t now() override { return util::nowUnix(); }

    void onEvent(const std::string& text, bool important) override {
        if (important) app.toast("Orchestrateur : " + text);
        if (OpenConversation* oc = find(app.orch_ ? app.orch_->brainConvId : std::string()))
            app.pushActivity(*oc, important ? ActivityKind::Attention : ActivityKind::Info, text);
    }
};

// ---------------------------------------------------------------------------
// Cycle de vie
// ---------------------------------------------------------------------------

ws::Folder* App::orchProjectFolder() {
    if (orchProject_.empty()) return nullptr;
    return workspace_.findFolderByPath(util::pathFromUtf8(orchProject_));
}

int App::indexOfConversation(const std::string& id) const {
    for (int i = 0; i < (int)open_.size(); ++i)
        if (open_[i]->session->localId == id) return i;
    return -1;
}

bool App::loadOrchestrator(ws::Folder* project) {
    if (!project || !project->isProject) return false;
    if (orch_ && orch_->running()) return false;
    if (orch_) saveOrchestrator();
    orch_ = std::make_unique<claude::Orchestrator>();
    orchProject_ = util::pathToUtf8(project->path);
    std::string txt;
    if (util::readFile(project->path / kOrchFile, txt)) {
        try {
            orch_->fromJson(json::parse(txt));
        } catch (const std::exception& e) {
            toast(std::string("Orchestrateur illisible : ") + e.what(), true);
        }
    }
    if (orch_->settings.objective.empty()) orch_->settings.objective = kDefaultObjective;
    return true;
}

void App::saveOrchestrator() {
    if (!orch_) return;
    ws::Folder* f = orchProjectFolder();
    if (!f) return;
    if (orch_->state == claude::OrchState::Idle && orch_->log.empty() && orch_->brainConvId.empty()) return;  // rien à garder
    util::writeFileAtomic(f->path / kOrchFile, orch_->toJson().dump(1));
    orch_->changed = false;
    orchLastSave_ = now_;
}

bool App::orchestratorStart() {
    if (!orch_) return false;
    AppOrchHost h(*this);
    bool ok = orch_->start(h);
    saveOrchestrator();
    return ok;
}

void App::tickOrchestrator() {
    if (!orch_) return;
    if (!orchProjectFolder()) {
        if (orch_->running()) orch_->pause("le projet n'existe plus dans l'espace de travail");
        return;
    }
    AppOrchHost h(*this);
    orch_->tick(h);
    if (orch_->changed && (now_ - orchLastSave_ > 2.0 || !orch_->running())) saveOrchestrator();
}

// ---------------------------------------------------------------------------
// Fenêtre
// ---------------------------------------------------------------------------

void App::drawOrchestratorWindow() {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 40, ImGui::GetFontSize() * 42), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Orchestrateur", &showOrchestrator_)) {
        ImGui::End();
        return;
    }
    const auto& pal = ui::palette();
    AppOrchHost host(*this);

    // Projet par défaut : celui sélectionné dans l'arbre.
    if (!orch_) {
        ws::Folder* sel = workspace_.findFolderByPath(util::pathFromUtf8(selectedFolder_));
        if (sel) sel = sel->project();
        if (sel && sel->name != "Divers") loadOrchestrator(sel);
    }

    // --- Projet ---
    ws::Folder* proj = orchProjectFolder();
    bool locked = orch_ && orch_->running();
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 2);
    if (ImGui::BeginCombo("##orchproj", proj ? proj->name.c_str() : "Choisir un projet…")) {
        for (auto& p : workspace_.projects()) {
            if (p->name == "Divers") continue;
            std::string label = p->name + "  (" + std::to_string(p->children.size()) + " agents)";
            if (ImGui::Selectable(label.c_str(), p.get() == proj)) loadOrchestrator(p.get());
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Mettez l'orchestration en pause pour changer de projet.");
    ImGui::SameLine();
    ui::helpMarker("Chaque thème (dossier) du projet est un agent : ses notes décrivent son rôle.\n"
                   "Une conversation « Orchestrateur » est créée à la racine du projet : elle examine le dépôt,\n"
                   "délègue des tâches aux agents (plusieurs en parallèle), lit leurs comptes rendus, vérifie,\n"
                   "et recommence jusqu'à atteindre l'objectif. Elle s'arrête d'elle-même sur une question\n"
                   "pour vous, le budget, le nombre de cycles ou la limite d'usage de 5 h.");

    if (!orch_ || !proj) {
        ImGui::Spacing();
        ImGui::TextWrapped("Choisissez un projet du mode cowork dont les thèmes sont des agents "
                           "(ex. « E7 Twin »). L'orchestrateur les fera travailler ensemble jusqu'à l'objectif.");
        ImGui::End();
        return;
    }
    auto& o = *orch_;
    if (proj->children.empty())
        ImGui::TextColored(pal.warn, "Ce projet n'a aucun thème : créez un dossier par agent (avec son rôle dans les notes).");

    // --- État ---
    ImGui::Spacing();
    ImVec4 sc = o.state == claude::OrchState::Finished       ? pal.ok
                : o.state == claude::OrchState::WaitingHuman ? pal.warn
                : o.state == claude::OrchState::Paused || o.state == claude::OrchState::UsagePause ? pal.warn
                : o.running()                                ? pal.accent
                                                             : pal.dim;
    if (o.state == claude::OrchState::Thinking || o.state == claude::OrchState::Working) {
        ui::spinner((float)now_, pal.accent);
        ImGui::SameLine();
    }
    ui::badge(claude::orchStateLabel(o.state), sc);
    ImGui::SameLine();
    ImGui::TextDisabled("cycle %d%s · %s%s", o.cycle,
                        o.settings.maxCycles > 0 ? (" / " + std::to_string(o.settings.maxCycles)).c_str() : "",
                        util::formatUsd(o.spentUsd).c_str(),
                        o.settings.maxCostUsd > 0 ? (" / " + util::formatUsd(o.settings.maxCostUsd)).c_str() : "");
    if (o.settings.maxCostUsd > 0) {
        char ov[64];
        std::snprintf(ov, sizeof ov, "budget %d %%", (int)(o.spentUsd / o.settings.maxCostUsd * 100 + 0.5));
        ui::usageBar((float)std::min(1.0, o.spentUsd / o.settings.maxCostUsd), ov, ImGui::GetFrameHeight() * 0.7f, -1, "##orchbudget");
    }
    if (!o.pauseReason.empty() && (o.state == claude::OrchState::Paused || o.state == claude::OrchState::UsagePause)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal.warn, "%s", o.pauseReason.c_str());
        ImGui::PopTextWrapPos();
    }
    if (o.state == claude::OrchState::WaitingHuman) {
        ImGui::SeparatorText("Question pour vous");
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(o.humanQuestion.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("Répondez dans la conversation « Orchestrateur » : l'orchestration reprend toute seule.");
    }
    if (o.state == claude::OrchState::Finished && !o.finalSummary.empty()) {
        ImGui::SeparatorText("Bilan");
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(o.finalSummary.c_str());
        ImGui::PopTextWrapPos();
    }

    // --- Contrôles ---
    ImGui::Spacing();
    bool paused = o.state == claude::OrchState::Paused || o.state == claude::OrchState::UsagePause;
    if (!o.running() && !paused) {
        ImGui::BeginDisabled(util::trim(o.settings.objective).empty() || proj->children.empty() || claudeExe_.empty());
        if (ImGui::Button(o.state == claude::OrchState::Idle && o.cycle == 0 ? "▶ Démarrer" : "▶ Relancer")) orchestratorStart();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Lance l'orchestrateur sur l'objectif ci-dessous.\n"
                              "Une orchestration relancée repart de zéro (cycles, budget, journal),\n"
                              "mais la conversation « Orchestrateur » garde sa mémoire.");
    }
    if (o.running()) {
        if (ImGui::Button("⏸ Pause")) {
            o.pause("mise en pause par vous (les tours en cours se terminent)");
            saveOrchestrator();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Aucune nouvelle tâche n'est envoyée ; les agents finissent leur tour en cours.");
    }
    if (paused) {
        if (ImGui::Button("▶ Reprendre")) {
            o.resume(host);
            saveOrchestrator();
        }
    }
    if (o.state != claude::OrchState::Idle) {
        ImGui::SameLine();
        if (ImGui::Button("■ Arrêter")) {
            o.stop();
            saveOrchestrator();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Arrête l'orchestration. Les tours en cours ne sont pas interrompus.");
    }
    if (!o.brainConvId.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Conversation de l'orchestrateur")) {
            if (host.ensureOpen(o.brainConvId)) focusConversation(indexOfConversation(o.brainConvId));
        }
    }

    // --- Objectif et réglages ---
    if (ImGui::CollapsingHeader("Objectif et réglages", o.running() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(o.running());
        ImGui::TextDisabled("Objectif");
        if (ImGui::InputTextMultiline("##objective", &o.settings.objective, ImVec2(-1, ImGui::GetTextLineHeight() * 5),
                                      ImGuiInputTextFlags_WordWrap))
            o.changed = true;
        ImGui::EndDisabled();
        float w = ImGui::GetFontSize() * 9;
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderInt("Agents en parallèle", &o.settings.maxParallel, 1, 6)) o.changed = true;
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputInt("Cycles maximum (0 = illimité)", &o.settings.maxCycles)) {
            o.settings.maxCycles = std::max(0, o.settings.maxCycles);
            o.changed = true;
        }
        ImGui::SetNextItemWidth(w);
        if (ImGui::InputDouble("Budget en $ (0 = illimité)", &o.settings.maxCostUsd, 1.0, 10.0, "%.2f")) {
            o.settings.maxCostUsd = std::max(0.0, o.settings.maxCostUsd);
            o.changed = true;
        }
        ImGui::SameLine();
        ui::helpMarker("Coût cumulé des tours de l'orchestrateur et des agents, tel que rapporté par Claude Code.\n"
                       "Avec un abonnement, c'est une estimation de consommation, pas une facture.");
        auto modeCombo = [&](const char* label, std::string& v) {
            ImGui::SetNextItemWidth(w);
            const char* cur = v.c_str();
            for (auto& c : claude::permissionModeChoices())
                if (v == c.value) cur = c.label;
            if (ImGui::BeginCombo(label, cur)) {
                for (auto& c : claude::permissionModeChoices()) {
                    if (ImGui::Selectable(c.label, v == c.value)) {
                        v = c.value;
                        o.changed = true;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.help);
                }
                ImGui::EndCombo();
            }
        };
        modeCombo("Mode des agents", o.settings.workerMode);
        modeCombo("Mode de l'orchestrateur", o.settings.brainMode);
        ImGui::SameLine();
        ui::helpMarker("« Auto » : Claude exécute seul ce qui est sûr et ne demande que pour le reste.\n"
                       "Les demandes restantes apparaissent dans la conversation concernée et le tableau de bord ;\n"
                       "l'orchestration attend votre décision.");
        int pct = (int)(o.settings.usagePause * 100 + 0.5);
        ImGui::SetNextItemWidth(w);
        if (ImGui::SliderInt("Pause si la limite de 5 h dépasse", &pct, 0, 100, pct == 0 ? "jamais" : "%d %%")) {
            o.settings.usagePause = pct / 100.0;
            o.changed = true;
        }
    }

    // --- Agents ---
    ImGui::SeparatorText("Agents");
    if (ImGui::BeginTable("##agents", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Agent", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("État", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9);
        ImGui::TableSetupColumn("Tâche en cours", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();
        int row = 0;
        for (auto& ch : proj->children) {
            const std::string& name = ch->name;
            ImGui::TableNextRow();
            ImGui::PushID(row++);
            ImGui::TableNextColumn();
            auto it = o.agentConv.find(name);
            std::string convId = it != o.agentConv.end() ? it->second : std::string();
            if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_None)) {
                std::string id = convId.empty() ? host.openAgent(name, {}) : convId;
                if (host.ensureOpen(id)) focusConversation(indexOfConversation(id));
            }
            if (ImGui::IsItemHovered() && !ch->notes.empty()) ImGui::SetTooltip("%s", agentBrief(ch->notes).c_str());
            ImGui::TableNextColumn();
            const claude::OrchTask* task = nullptr;
            for (auto& t : o.tasks)
                if (t.agent == name) task = &t;
            claude::ParticipantStatus st = convId.empty() ? claude::ParticipantStatus{} : host.status(convId);
            if (task && task->sent && !task->done) {
                if (st.needsHuman) ImGui::TextColored(pal.warn, "⚠ vous attend");
                else {
                    ui::spinner((float)now_, pal.accent);
                    ImGui::SameLine();
                    ImGui::TextColored(pal.accent, "travaille");
                }
            } else if (task && !task->sent && !task->done) {
                ImGui::TextDisabled("en file");
            } else if (task && task->done) {
                ImGui::TextColored(task->failed ? pal.err : pal.ok, task->failed ? "✗ échec" : "✓ terminé");
            } else {
                ImGui::TextDisabled("—");
            }
            ImGui::TableNextColumn();
            if (task) {
                ImGui::TextUnformatted(util::firstLine(task->task, 90).c_str());
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35);
                    ImGui::TextUnformatted(util::truncateUtf8(task->task, 1500).c_str());
                    if (task->done && !task->report.empty()) {
                        ImGui::Separator();
                        ImGui::TextUnformatted(util::truncateUtf8(task->report, 1500).c_str());
                    }
                    ImGui::PopTextWrapPos();
                    ImGui::EndTooltip();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // --- Journal ---
    ImGui::SeparatorText("Journal");
    ImGui::BeginChild("##orchlog", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (o.log.empty()) ImGui::TextDisabled("Les décisions de l'orchestrateur et l'avancement des agents s'afficheront ici.");
    for (auto& l : o.log) {
        ImGui::TextDisabled("%s", clockOf(l.time).c_str());
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0);
        if (l.important) ImGui::TextColored(pal.accent, "%s", l.text.c_str());
        else ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopTextWrapPos();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace app
