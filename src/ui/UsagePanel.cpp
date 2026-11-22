// Panneau « Usage » : l'équivalent de /usage en direct (limites 5 h / semaine),
// plus les statistiques de la session active.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

#include <imgui.h>

#include "app/App.h"
#include "claude/Models.h"
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
            ImGui::Separator();
            ImGui::TextDisabled("Zone claire : session en cours · pointillés : depuis la dernière mesure");
            ImGui::EndTooltip();
        }
    }
}

}  // namespace

// Panneau épuré : l'essentiel d'un coup d'œil (anneau de la session de 5 h, semaine), le reste replié.
void App::drawUsagePanel() {
    if (!ImGui::Begin("Usage", &showUsage_)) {
        ImGui::End();
        return;
    }
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    int64_t nowT = util::nowUnix();
    bool refreshing = false;
    for (auto& j : jobs_)
        if (j->target == "#usage") refreshing = true;

    // --- Quota de l'abonnement ---------------------------------------------------------------------------------
    ImGui::SeparatorText(usage_.subscription.empty() ? "Quota" : ("Quota · " + usage_.subscription).c_str());
    if (usage_.windows.empty()) {
        dimWrapped("Pas encore de mesure. L'actualisation lance /usage, qui ne consomme aucun quota.");
        ImGui::BeginDisabled(refreshing);
        if (ImGui::Button(refreshing ? "Mesure…" : "Mesurer maintenant")) refreshUsage();
        ImGui::EndDisabled();
    }
    // Échéance lisible : « dans 1 h 04 »
    auto resetText = [&](const claude::RateWindow& w, bool expired) -> std::string {
        if (w.resetsAt <= 0) return {};
        if (expired) return w.name == "five_hour" ? "terminée : la suivante démarre à votre prochain message" : "réinitialisée";
        return "repart à 0 dans " + util::humanDuration(w.resetsAt - nowT);
    };
    auto statusBadge = [&](const claude::RateWindow& w, bool expired) {
        if (expired) return;
        if (w.status == "rejected") ui::badge("limite atteinte", pal.err);
        else if (w.status == "allowed_warning") ui::badge("proche de la limite", pal.warn);
    };

    // Session de 5 h : anneau + texte à côté (ou dessous si le panneau est étroit)
    if (const claude::RateWindow* five = usage_.find("five_hour")) {
        const bool expired = five->resetsAt > 0 && nowT >= five->resetsAt;
        const float used = expired ? 0.0f : (float)five->utilization;
        char big[16];
        std::snprintf(big, sizeof big, "%d %%", (int)(used * 100 + 0.5f));
        const float d = std::clamp(ImGui::GetContentRegionAvail().x * 0.36f, fs * 5.0f, fs * 7.0f);
        const bool side = ImGui::GetContentRegionAvail().x >= d + fs * 9;
        ImGui::Spacing();
        if (!side) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - d) * 0.5f);
        ui::ringGauge(used, d, big, "utilisé", fontBold, "##ring5h");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(fs * 22);
            ImGui::TextUnformatted("Part du quota de la session en cours. Une session dure 5 h à partir de votre premier "
                                   "message ; à son terme, le compteur repart de 0 %.");
            if (!expired && five->resetsAt > 0) ImGui::TextDisabled("Réinitialisation : %s", util::humanWhen(five->resetsAt).c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (side) {
            ImGui::SameLine(0, fs);
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(0, d * 0.5f - ImGui::GetTextLineHeightWithSpacing() * 1.2f));
            ImGui::PushFont(fontBold, 0.0f);
            ImGui::TextUnformatted("Session de 5 h");
            ImGui::PopFont();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(pal.dim, "%s", resetText(*five, expired).c_str());
            ImGui::PopTextWrapPos();
            statusBadge(*five, expired);
            ImGui::EndGroup();
        } else {
            std::string t = "Session de 5 h · " + resetText(*five, expired);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(t.c_str()).x) * 0.5f));
            ImGui::TextColored(pal.dim, "%s", t.c_str());
            statusBadge(*five, expired);
        }
        ImGui::Spacing();
    }
    // Les autres fenêtres (semaine, semaine Opus…) : une ligne chacune
    for (auto& w : usage_.windows) {
        if (w.name == "five_hour") continue;
        const bool expired = w.resetsAt > 0 && nowT >= w.resetsAt;
        const float used = expired ? 0.0f : (float)w.utilization;
        char val[16];
        std::snprintf(val, sizeof val, "%d %%", (int)(used * 100 + 0.5f));
        ui::meterRow(claude::windowLabel(w.name).c_str(), used, val, resetText(w, expired).c_str(), fontBold, w.name.c_str());
        if (!expired && (w.status == "rejected" || w.status == "allowed_warning")) statusBadge(w, expired);
    }
    if (usage_.usingOverage) ui::badge("crédits supplémentaires en cours d'utilisation", pal.warn);
    // Fraîcheur de la mesure + actualiser, en une ligne discrète
    if (usage_.lastUpdate > 0) {
        ImGui::TextDisabled("Mesuré il y a %s", util::humanDuration(nowT - usage_.lastUpdate).c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(refreshing);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        if (ImGui::SmallButton(refreshing ? "…##usagerefresh" : "⟳##usagerefresh")) refreshUsage();
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("Actualiser (commande /usage : gratuite, sans quota consommé)");
    }
    if (!usage_.history.empty() && ImGui::TreeNodeEx("Historique sur 24 h", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        drawFiveHourHistory(usage_, nowT);
        ImGui::TreePop();
    }

    // --- Conversation active -------------------------------------------------------------------------------------
    if (OpenConversation* oc = active()) {
        auto& s = *oc->session;
        auto& st = s.stats;
        ImGui::SeparatorText("Conversation active");
        // Modèle et état sur une ligne
        std::string model = st.lastModel.empty() ? claude::modelDisplayName(s.config.model) : claude::modelDisplayName(st.lastModel);
        bool busy = s.state == claude::SessionState::Busy || s.state == claude::SessionState::Starting;
        if (busy) {
            ui::spinner((float)now_, pal.accent);
            ImGui::SameLine();
        }
        ImGui::TextUnformatted(model.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("· %s", claude::stateLabel(s.state));
        if (st.contextWindow > 0) {
            float f = (float)st.contextTokens / (float)st.contextWindow;
            char val[16];
            std::snprintf(val, sizeof val, "%d %%", (int)(f * 100 + 0.5f));
            std::string note = util::humanTokens(st.contextTokens) + " / " + util::humanTokens(st.contextWindow) + " jetons";
            if (f > 0.6f) note += " · /compact pour résumer";
            ui::meterRow("Contexte", f, val, note.c_str(), fontBold, ("##ctx" + s.localId).c_str());
        }
        ImGui::TextDisabled("%d tour%s · %s en équivalent API", st.turns, st.turns > 1 ? "s" : "", util::formatUsd(st.costUsd).c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Le coût est l'équivalent au tarif API : avec un abonnement, c'est le quota\n"
                              "ci-dessus qui est consommé, pas de l'argent.");

        // Liste de tâches de Claude (TodoWrite) : utile, gardée visible
        for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it) {
            if (it->kind != claude::EntryKind::ToolUse || it->toolName != "TodoWrite" || !it->parentToolUseId.empty()) continue;
            auto input = nlohmann::json::parse(it->toolInput, nullptr, false);
            if (!input.is_object() || !input.contains("todos")) break;
            int done = 0, total = 0;
            for (auto& t : input["todos"]) {
                ++total;
                if (t.value("status", "") == "completed") ++done;
            }
            ImGui::SeparatorText(("Tâches de Claude · " + std::to_string(done) + "/" + std::to_string(total)).c_str());
            for (auto& t : input["todos"]) {
                std::string stt = t.value("status", "");
                ImVec4 c = stt == "completed" ? pal.ok : stt == "in_progress" ? pal.warn : pal.dim;
                ImGui::TextColored(c, "%s", stt == "completed" ? "☑" : stt == "in_progress" ? "◐" : "☐");
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f);
                if (stt == "completed") ImGui::TextDisabled("%s", t.value("content", "").c_str());
                else ImGui::TextUnformatted(t.value(stt == "in_progress" ? "activeForm" : "content", t.value("content", "")).c_str());
                ImGui::PopTextWrapPos();
            }
            break;
        }

        // Détails techniques : repliés
        if (ImGui::TreeNodeEx("Détails", ImGuiTreeNodeFlags_SpanAvailWidth)) {
            if (ImGui::BeginTable("##sess", 2, ImGuiTableFlags_SizingStretchProp)) {
                auto row = [](const char* k, const std::string& v) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("%s", k);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(v.c_str());
                };
                row("Jetons envoyés", util::humanTokens(st.inputTokens + st.cacheReadTokens + st.cacheWriteTokens));
                row("  dont cache lu", util::humanTokens(st.cacheReadTokens));
                row("  dont cache écrit", util::humanTokens(st.cacheWriteTokens));
                row("Jetons générés", util::humanTokens(st.outputTokens));
                row("Temps API", util::humanDuration((int64_t)st.apiSeconds));
                if (!s.claudeSessionId.empty()) row("Session", s.claudeSessionId.substr(0, 8) + "…");
                ImGui::EndTable();
            }
            if (st.turnHistory.size() >= 2) {
                std::vector<float> costs;
                for (auto& t : st.turnHistory) costs.push_back((float)(t.costUsd * 1000.0));
                ImGui::PlotHistogram("##costs", costs.data(), (int)costs.size(), 0, "coût par tour (m$)", 0, FLT_MAX,
                                     ImVec2(-1, fs * 3));
            }
            if (!s.mcpServers.empty()) {
                ImGui::TextDisabled("Serveurs MCP");
                for (auto& [name, status] : s.mcpServers) {
                    ImVec4 c = status == "connected" ? pal.ok : status == "failed" ? pal.err : pal.dim;
                    ImGui::TextColored(c, "●");
                    ImGui::SameLine();
                    ImGui::Text("%s  (%s)", name.c_str(), status.c_str());
                }
            }
            ImGui::TreePop();
        }
    }

    // --- En arrière-plan -----------------------------------------------------------------------------------------
    int busyN = 0;
    for (auto& o : open_)
        if (o->session->state == claude::SessionState::Busy) ++busyN;
    if (open_.size() > 1 || !jobs_.empty()) {
        ImGui::Spacing();
        if (open_.size() > 1)
            ImGui::TextDisabled("%d conversations ouvertes%s", (int)open_.size(),
                                busyN ? (", " + std::to_string(busyN) + " au travail").c_str() : "");
        for (auto& j : jobs_) {
            ui::spinner((float)now_, pal.accent);
            ImGui::SameLine();
            ImGui::TextDisabled("%s", j->label.c_str());
        }
    }
    ImGui::End();
}

}  // namespace app
