// Panneau « Usage » : l'équivalent de /usage en direct (limites 5 h / semaine),
// plus les statistiques de la session active.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

#include <imgui.h>

#include "app/App.h"
#include "ui/Theme.h"
#include "util/Strings.h"

namespace app {

namespace {

void dimWrapped(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

// Courbe de la session de 5 h sur les dernières 24 h : axe des temps réel (heures locales), graduations en %,
// zone de la session en cours, et infobulle lisible (date, % session, % semaine) au survol.
void drawFiveHourHistory(const claude::UsageTracker& u, int64_t nowT) {
    const auto& pal = ui::palette();
    const int64_t span = 24 * 3600, t0 = nowT - span;
    const claude::UsageSample* before = nullptr;  // dernière mesure avant la fenêtre affichée (continuité à gauche)
    std::vector<const claude::UsageSample*> pts;
    for (auto& h : u.history) {
        if (h.time < t0) before = &h;
        else if (h.time <= nowT) pts.push_back(&h);
    }
    if (pts.empty()) return;

    ImGui::Spacing();
    ImGui::TextDisabled("Session 5 h — 24 dernières heures");
    const float fs = ImGui::GetFontSize();
    const float axisW = ImGui::CalcTextSize("100 %").x + fs * 0.4f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float totalW = std::max(ImGui::GetContentRegionAvail().x, fs * 8);
    const float plotH = fs * 4.5f;
    ImGui::Dummy(ImVec2(totalW, plotH + fs * 1.3f));
    const ImVec2 a(origin.x + axisW, origin.y + fs * 0.4f), b(origin.x + totalW - fs * 0.3f, origin.y + fs * 0.4f + plotH);
    auto X = [&](int64_t t) { return a.x + (b.x - a.x) * (float)(t - t0) / (float)span; };
    auto Y = [&](double v) { return b.y - (b.y - a.y) * (float)std::clamp(v, 0.0, 1.0); };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, ui::col(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg)), fs * 0.25f);

    // Session en cours : de (réinitialisation − 5 h) à maintenant.
    if (const claude::RateWindow* w = u.find("five_hour"); w && w->resetsAt > nowT) {
        int64_t start = std::max(t0, w->resetsAt - 5 * 3600);
        dl->AddRectFilled(ImVec2(X(start), a.y), ImVec2(b.x, b.y), ui::col(pal.accent, 0.14f));
        dl->AddLine(ImVec2(X(start), a.y), ImVec2(X(start), b.y), ui::col(pal.accent, 0.45f));
    }
    // Graduations horizontales
    for (int pct : {25, 50, 75, 100}) {
        float y = Y(pct / 100.0);
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), pct == 100 ? ui::col(pal.err, 0.45f) : ui::col(pal.dim, 0.18f));
    }
    for (int pct : {0, 50, 100}) {
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "%d %%", pct);
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(a.x - ts.x - fs * 0.3f, Y(pct / 100.0) - ts.y * 0.5f), ui::col(pal.dim), lbl);
    }
    // Graduations verticales : toutes les 6 h, aux heures locales rondes (00:00, 06:00, 12:00, 18:00).
    for (int64_t t = t0 - (t0 % 60) + 60; t <= nowT; t += 60) {
        int m = util::localMinuteOfDay(t);
        if (m % 360 != 0) {
            t += (int64_t)(359 - m % 360) * 60;  // saute à la minute précédant la prochaine heure ronde
            continue;
        }
        float x = X(t);
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), ui::col(pal.dim, 0.18f));
        char lbl[8];
        std::snprintf(lbl, sizeof lbl, "%02d:00", m / 60);
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        float lx = std::clamp(x - ts.x * 0.5f, a.x, b.x - ts.x);
        dl->AddText(ImVec2(lx, b.y + fs * 0.15f), ui::col(pal.dim), lbl);
    }

    // Courbe : segments droits entre mesures rapprochées ; après un trou (> 20 min), la valeur est tenue puis
    // saute (une réinitialisation se lit alors comme une marche, pas comme une pente).
    std::vector<ImVec2> line;
    if (before) line.push_back(ImVec2(a.x, Y(before->fiveHour)));
    int64_t prevT = before ? t0 : 0;
    double prevV = before ? before->fiveHour : 0;
    for (auto* s : pts) {
        if (!line.empty() && s->time - prevT > 20 * 60) line.push_back(ImVec2(X(s->time), Y(prevV)));
        line.push_back(ImVec2(X(s->time), Y(s->fiveHour)));
        prevT = s->time;
        prevV = s->fiveHour;
    }
    const ImU32 lineCol = ui::col(pal.accent);
    // Remplissage : quadrilatères sous chaque segment, sans anticrénelage (sinon les bords communs dessinent des rayures).
    const ImDrawListFlags oldFlags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (size_t i = 1; i < line.size(); ++i) {
        ImVec2 q[4] = {line[i - 1], line[i], ImVec2(line[i].x, b.y), ImVec2(line[i - 1].x, b.y)};
        dl->AddConvexPolyFilled(q, 4, ui::col(pal.accent, 0.18f));
    }
    dl->Flags = oldFlags;
    if (line.size() >= 2) dl->AddPolyline(line.data(), (int)line.size(), lineCol, ImDrawFlags_None, 1.6f);
    if (line.size() == 1) dl->AddCircleFilled(line[0], 2.5f, lineCol);
    // Depuis la dernière mesure : valeur inconnue, prolongée en pointillés — et ramenée à 0 à la réinitialisation.
    auto dashed = [&](ImVec2 p, ImVec2 q) {
        float len = std::hypot(q.x - p.x, q.y - p.y);
        for (float d = 0; d < len; d += 6) {
            float e = std::min(d + 3, len);
            dl->AddLine(ImVec2(p.x + (q.x - p.x) * d / len, p.y + (q.y - p.y) * d / len),
                        ImVec2(p.x + (q.x - p.x) * e / len, p.y + (q.y - p.y) * e / len), ui::col(pal.accent, 0.55f), 1.2f);
        }
    };
    ImVec2 tail = line.back();
    const claude::RateWindow* five = u.find("five_hour");
    if (five && five->resetsAt > pts.back()->time && five->resetsAt <= nowT) {
        ImVec2 r(X(five->resetsAt), tail.y);
        dashed(tail, r);
        dashed(r, ImVec2(r.x, b.y));
        tail = ImVec2(r.x, b.y);
    }
    dashed(ImVec2(tail.x + 3, tail.y), ImVec2(b.x, tail.y));

    // Survol : la mesure la plus proche du curseur.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b)) {
        float mx = ImGui::GetIO().MousePos.x;
        const claude::UsageSample* best = nullptr;
        float bestD = FLT_MAX;
        for (auto* s : pts)
            if (float d = std::fabs(X(s->time) - mx); d < bestD) bestD = d, best = s;
        if (best) {
            ImVec2 p(X(best->time), Y(best->fiveHour));
            dl->AddLine(ImVec2(p.x, a.y), ImVec2(p.x, b.y), ui::col(ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.35f));
            dl->AddCircleFilled(p, 4.0f, lineCol);
            ImGui::BeginTooltip();
            ImGui::Text("%s  (il y a %s)", util::humanWhen(best->time).c_str(), util::humanDuration(nowT - best->time).c_str());
            ImGui::Separator();
            ImGui::Text("Session 5 h : %d %% utilisé", (int)(best->fiveHour * 100 + 0.5));
            ImGui::TextDisabled("Semaine : %d %% utilisé", (int)(best->sevenDay * 100 + 0.5));
            ImGui::EndTooltip();
        }
    }
    const claude::RateWindow* fiveNow = u.find("five_hour");
    dimWrapped(fiveNow && fiveNow->resetsAt > nowT ? "Zone claire : session en cours  ·  pointillés : depuis la dernière mesure"
                                                   : "Pointillés : depuis la dernière mesure (aucune session en cours)");
}

}  // namespace

void App::drawUsagePanel() {
    if (!ImGui::Begin("Usage", &showUsage_)) {
        ImGui::End();
        return;
    }
    const auto& pal = ui::palette();
    int64_t nowT = util::nowUnix();

    // --- Limites du forfait ---
    ImGui::SeparatorText(usage_.subscription.empty() ? "Limites du forfait" : ("Limites — " + usage_.subscription).c_str());
    bool refreshing = false;
    for (auto& j : jobs_)
        if (j->target == "#usage") refreshing = true;
    if (usage_.windows.empty()) {
        ImGui::TextWrapped("Pas encore de mesure. Cliquez sur Actualiser : ClaudeDesk lance /usage, "
                           "qui ne consomme aucun quota.");
    }
    for (auto& w : usage_.windows) {
        const bool fiveHour = w.name == "five_hour";
        // Date de réinitialisation dépassée : la mesure gardée est périmée, le compteur est reparti de 0.
        const bool expired = w.resetsAt > 0 && nowT >= w.resetsAt;
        const double used = expired ? 0.0 : w.utilization;
        ImGui::TextUnformatted(claude::windowLabel(w.name).c_str());
        if (expired) {
            ImGui::SameLine();
            ui::badge("réinitialisée", pal.ok);
        } else if (w.status == "rejected") {
            ImGui::SameLine();
            ui::badge("limite atteinte", pal.err);
        } else if (w.status == "allowed_warning") {
            ImGui::SameLine();
            ui::badge("proche de la limite", pal.warn);
        }
        char overlay[64];
        std::snprintf(overlay, sizeof overlay, "%d %% utilisé", (int)(used * 100 + 0.5));
        ui::usageBar((float)used, overlay, ImGui::GetFrameHeight() * 0.9f, -1, w.name.c_str());
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24);
            if (fiveHour)
                ImGui::TextUnformatted("Part du quota de la session en cours. Une session dure 5 h à partir de votre "
                                       "premier message ; à son terme, le compteur repart de 0 %.");
            else
                ImGui::TextUnformatted("Quota de la semaine. Il se réinitialise à date fixe, une fois par semaine.");
            if (!expired) ImGui::Text("Reste : %d %%", std::max(0, 100 - (int)(w.utilization * 100 + 0.5)));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (w.resetsAt > 0) {
            std::string when = util::humanWhen(w.resetsAt);
            if (!expired)
                ImGui::TextDisabled("Repart de 0 dans %s  ·  %s", util::humanDuration(w.resetsAt - nowT).c_str(), when.c_str());
            else if (fiveHour)
                dimWrapped("Session terminée " + when + " : la suivante démarre à votre prochain message.");
            else
                ImGui::TextDisabled("Réinitialisée %s.", when.c_str());
        }
    }
    if (usage_.usingOverage) ui::badge("Crédits supplémentaires en cours d'utilisation", pal.warn);
    if (usage_.lastUpdate > 0) {
        ImGui::TextDisabled("Mesuré il y a %s", util::humanDuration(nowT - usage_.lastUpdate).c_str());
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(refreshing);
    if (ImGui::SmallButton(refreshing ? "Actualisation…" : "Actualiser")) refreshUsage();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("Lance la commande /usage de Claude Code : gratuite, sans quota consommé,\n"
                          "et elle fonctionne même quand la limite est atteinte.");

    drawFiveHourHistory(usage_, nowT);

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
