// Panneau « Usage » : l'équivalent de /usage en direct (limites 5 h / semaine),
// plus les statistiques de la session active.
#include <cfloat>

#include <imgui.h>

#include "app/App.h"
#include "ui/Theme.h"
#include "util/Strings.h"

namespace app {

void App::drawUsagePanel() {
    if (!ImGui::Begin("Usage", &showUsage_)) {
        ImGui::End();
        return;
    }
    const auto& pal = ui::palette();
    int64_t nowT = util::nowUnix();

    // --- Limites du forfait ---
    ImGui::SeparatorText(usage_.subscription.empty() ? "Limites du forfait" : ("Limites — " + usage_.subscription).c_str());
    if (usage_.windows.empty()) {
        ImGui::TextWrapped("Pas encore de mesure. Elle arrive avec la première réponse de Claude, "
                           "ou cliquez sur Actualiser (envoie une requête minuscule à Haiku).");
    }
    for (auto& w : usage_.windows) {
        ImGui::TextUnformatted(claude::windowLabel(w.name).c_str());
        if (w.status == "rejected") {
            ImGui::SameLine();
            ui::badge("limite atteinte", pal.err);
        } else if (w.status == "allowed_warning") {
            ImGui::SameLine();
            ui::badge("proche de la limite", pal.warn);
        }
        char overlay[64];
        std::snprintf(overlay, sizeof overlay, "%d %%", (int)(w.utilization * 100 + 0.5));
        ui::usageBar((float)w.utilization, overlay, ImGui::GetFrameHeight() * 0.9f, -1, w.name.c_str());
        if (w.resetsAt > 0) {
            int64_t left = w.resetsAt - nowT;
            std::string when = util::isoFromUnix(w.resetsAt);
            when = when.size() >= 16 ? when.substr(8, 2) + "/" + when.substr(5, 2) + " " + when.substr(11, 5) : when;
            ImGui::TextDisabled("Réinitialisation %s (%s)", left > 0 ? ("dans " + util::humanDuration(left)).c_str() : "imminente",
                                when.c_str());
        }
    }
    if (usage_.usingOverage) ui::badge("Crédits supplémentaires en cours d'utilisation", pal.warn);
    if (usage_.lastUpdate > 0) {
        ImGui::TextDisabled("Mesuré il y a %s", util::humanDuration(nowT - usage_.lastUpdate).c_str());
        ImGui::SameLine();
    }
    bool refreshing = false;
    for (auto& j : jobs_)
        if (j->target == "#usage") refreshing = true;
    ImGui::BeginDisabled(refreshing);
    if (ImGui::SmallButton(refreshing ? "Actualisation…" : "Actualiser")) refreshUsage();
    ImGui::EndDisabled();

    // Évolution de la fenêtre de 5 h sur les dernières 24 h
    if (usage_.history.size() >= 2) {
        std::vector<float> pts;
        for (auto& h : usage_.history)
            if (h.time > nowT - 86400) pts.push_back((float)h.fiveHour * 100.f);
        if (pts.size() >= 2) {
            ImGui::PlotLines("##hist5h", pts.data(), (int)pts.size(), 0, "fenêtre 5 h (24 h)", 0, 100,
                             ImVec2(-1, ImGui::GetFontSize() * 3));
        }
    }

    // --- Session active ---
    OpenConversation* oc = active();
    if (oc) {
        auto& s = *oc->session;
        auto& st = s.stats;
        ImGui::SeparatorText("Conversation active");
        if (ImGui::BeginTable("##sess", 2, ImGuiTableFlags_SizingStretchProp)) {
            auto row = [](const char* k, const std::string& v) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", k);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(v.c_str());
            };
            row("État", claude::stateLabel(s.state));
            row("Modèle", st.lastModel.empty() ? (s.config.model.empty() ? "par défaut" : s.config.model) : st.lastModel);
            row("Coût (équivalent API)", util::formatUsd(st.costUsd));
            row("Tours", std::to_string(st.turns));
            row("Jetons envoyés", util::humanTokens(st.inputTokens + st.cacheReadTokens + st.cacheWriteTokens));
            row("  dont cache lu", util::humanTokens(st.cacheReadTokens));
            row("  dont cache écrit", util::humanTokens(st.cacheWriteTokens));
            row("Jetons générés", util::humanTokens(st.outputTokens));
            row("Temps API", util::humanDuration((int64_t)st.apiSeconds));
            if (!s.claudeSessionId.empty()) row("Session", s.claudeSessionId.substr(0, 8) + "…");
            ImGui::EndTable();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Le coût est l'équivalent au tarif API : avec un abonnement, c'est le quota\n"
                              "ci-dessus qui est consommé, pas de l'argent.");
        if (st.contextWindow > 0) {
            float f = (float)st.contextTokens / (float)st.contextWindow;
            std::string ov = util::humanTokens(st.contextTokens) + " / " + util::humanTokens(st.contextWindow);
            ImGui::TextUnformatted("Fenêtre de contexte");
            ui::usageBar(f, ov.c_str(), ImGui::GetFrameHeight() * 0.9f, -1, ("##ctx" + s.localId).c_str());
        }
        if (st.turnHistory.size() >= 2) {
            std::vector<float> costs;
            for (auto& t : st.turnHistory) costs.push_back((float)(t.costUsd * 1000.0));
            ImGui::PlotHistogram("##costs", costs.data(), (int)costs.size(), 0, "coût par tour (m$)", 0, FLT_MAX,
                                 ImVec2(-1, ImGui::GetFontSize() * 3));
        }

        // Dernière liste de tâches de Claude (TodoWrite)
        for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it) {
            if (it->kind != claude::EntryKind::ToolUse || it->toolName != "TodoWrite" || !it->parentToolUseId.empty()) continue;
            auto input = nlohmann::json::parse(it->toolInput, nullptr, false);
            if (!input.is_object() || !input.contains("todos")) break;
            int done = 0, total = 0;
            for (auto& t : input["todos"]) {
                ++total;
                if (t.value("status", "") == "completed") ++done;
            }
            ImGui::SeparatorText(("Tâches de Claude (" + std::to_string(done) + "/" + std::to_string(total) + ")").c_str());
            for (auto& t : input["todos"]) {
                std::string stt = t.value("status", "");
                ImVec4 c = stt == "completed" ? pal.ok : stt == "in_progress" ? pal.warn : pal.dim;
                ImGui::TextColored(c, "%s", stt == "completed" ? "☑" : stt == "in_progress" ? "◐" : "☐");
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(t.value(stt == "in_progress" ? "activeForm" : "content", t.value("content", "")).c_str());
                ImGui::PopTextWrapPos();
            }
            break;
        }

        if (!s.mcpServers.empty() && ImGui::TreeNode("Serveurs MCP")) {
            for (auto& [name, status] : s.mcpServers) {
                ImVec4 c = status == "connected" ? pal.ok : status == "failed" ? pal.err : pal.dim;
                ImGui::TextColored(c, "●");
                ImGui::SameLine();
                ImGui::Text("%s  (%s)", name.c_str(), status.c_str());
            }
            ImGui::TreePop();
        }
    }

    // --- Toutes les conversations ouvertes ---
    if (open_.size() > 1) {
        double total = 0;
        int busy = 0;
        for (auto& o : open_) {
            total += o->session->stats.costUsd;
            if (o->session->state == claude::SessionState::Busy) ++busy;
        }
        ImGui::SeparatorText("Toutes les conversations ouvertes");
        ImGui::Text("%d ouvertes, %d actives  ·  %s", (int)open_.size(), busy, util::formatUsd(total).c_str());
    }
    if (!jobs_.empty()) {
        ImGui::SeparatorText("Tâches de fond");
        for (auto& j : jobs_) {
            ui::spinner((float)now_, pal.accent);
            ImGui::SameLine();
            ImGui::TextUnformatted(j->label.c_str());
        }
    }
    ImGui::End();
}

}  // namespace app
