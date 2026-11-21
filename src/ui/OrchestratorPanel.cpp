// Fenêtre « Orchestrateur » : un projet cowork piloté automatiquement.
// Chaque thème (dossier) du projet est un agent ; une conversation « Orchestrateur »
// à la racine du projet décide, délègue et vérifie (voir claude/Orchestrator.h).
#include <algorithm>
#include <cfloat>
#include <cmath>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/App.h"
#include "ui/Anim.h"
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
// Phrases entières (coupées à la fin d'une phrase, sinon d'un mot), sans Markdown :
// le résumé est inséré dans un message, un « ** » ou un « ` » orphelin casserait la mise en forme.
std::string agentBrief(const std::string& notes) {
    std::string out;
    for (const std::string& raw : util::splitLines(notes)) {
        std::string l = util::trim(raw);
        if (l.empty() || l[0] == '#' || l[0] == '(' || l[0] == '|' || util::startsWith(l, "---")) continue;
        if (!out.empty()) out += " ";
        out += l;
        if (out.size() > 400) break;
    }
    util::replaceAll(out, "**", "");
    util::replaceAll(out, "`", "");
    const size_t maxLen = 200;
    if (out.size() <= maxLen) return out;
    size_t cut = std::string::npos;
    for (size_t i = 0; i + 1 < out.size() && i < maxLen; ++i)
        if ((out[i] == '.' || out[i] == '!' || out[i] == '?') && out[i + 1] == ' ' && i > 40) cut = i + 1;
    if (cut != std::string::npos) return out.substr(0, cut);
    std::string t = util::truncateUtf8(out, maxLen);
    size_t sp = t.rfind(' ');
    return (sp != std::string::npos && sp > 40 ? t.substr(0, sp) : t) + "…";
}

// Texte indicatif dans un champ multiligne vide (ImGui n'en propose que pour une ligne).
void placeholder(const std::string& value, const char* hint) {
    if (!value.empty() || ImGui::IsItemActive()) return;
    ImVec2 p = ImGui::GetItemRectMin();
    ImVec2 pad = ImGui::GetStyle().FramePadding;
    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), hint);
}

std::string clockOf(int64_t t) {
    std::string iso = util::isoFromUnix(t);
    return iso.size() >= 16 ? iso.substr(11, 5) : iso;
}

std::string pctText(double f) { return std::to_string((int)std::lround(f * 100)) + " %"; }

struct Alias {
    const char* value;
    const char* label;
};
const Alias kModels[] = {{"haiku", "Haiku"}, {"sonnet", "Sonnet"}, {"opus", "Opus"}, {"fable", "Fable"}};

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

    bool send(const std::string& id, const std::string& text, const claude::SendOptions& opt) override {
        OpenConversation* oc = find(id);
        if (!oc || app.claudeExe_.empty()) return false;
        auto& s = *oc->session;
        if (!opt.permissionMode.empty() && s.config.permissionMode != opt.permissionMode) {
            if (s.processRunning()) s.setPermissionMode(opt.permissionMode);
            else s.config.permissionMode = opt.permissionMode;
        }
        // Le modèle se change à chaud ; l'effort relance la session (--resume) avant l'envoi.
        if (!opt.model.empty() && s.config.model != opt.model) s.setModel(opt.model);
        if (!opt.effort.empty() && s.config.effort != opt.effort) {
            claude::SessionConfig cfg = s.config;
            cfg.effort = opt.effort;
            s.applyConfig(cfg);
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
        st.context = s.stats.contextWindow > 0 ? (double)s.stats.contextTokens / (double)s.stats.contextWindow : 0.0;
        if (!st.busy) st.lastText = claude::lastTurnText(s.entries);
        return st;
    }

    double utilization(const std::string& window) override {
        const claude::RateWindow* w = app.usage_.find(window);
        return w ? w->utilization : -1.0;
    }
    int64_t resetsAt(const std::string& window) override {
        const claude::RateWindow* w = app.usage_.find(window);
        return w ? w->resetsAt : 0;
    }
    bool isSubscription() override { return !app.usage_.subscription.empty() || app.usage_.find("five_hour") != nullptr; }
    std::string planName() override { return app.usage_.subscription; }
    int64_t now() override { return util::nowUnix(); }

    // Rotation de contexte : récap (Haiku) de la conversation, puis nouvelle conversation dans le même dossier ;
    // la mémoire cowork (notes, synthèse, récaps dont celui-ci) y est injectée au démarrage.
    std::string rotate(const std::string& agent, const std::string& convId) override {
        if (!app.rotating_.count(convId)) {
            app.rotating_.insert(convId);
            app.requestRecap(convId);
            return {};
        }
        for (auto& j : app.jobs_)
            if (j->target == convId && !j->finished()) return {};  // récap en cours
        app.rotating_.erase(convId);
        ws::Folder* p = app.orchProjectFolder();
        if (!p) return {};
        ws::Folder* folder = p;
        if (!agent.empty())
            for (auto& ch : p->children)
                if (ch->name == agent) folder = ch.get();
        OpenConversation* old = find(convId);
        OpenConversation* oc = app.newConversation(folder, {}, false);
        if (!oc) return {};
        auto& s = *oc->session;
        if (old) s.config = old->session->config;  // modèle, effort, mode, prompt système
        s.title = agent.empty() ? "Orchestrateur" : "Tâches de l'orchestrateur";
        if (agent.empty()) {
            oc->meta.pinned = true;
            s.config.systemPromptMode = claude::SystemPromptMode::Append;
            s.config.systemPrompt = claude::Orchestrator::systemPrompt();
        }
        oc->meta.status = "active";
        oc->injectContext = true;
        std::string nid = s.localId;
        if (old) {
            old->meta.status = "done";
            old->meta.pinned = false;
            app.saveConversation(*old);
            int idx = app.indexOfConversation(convId);
            if (idx >= 0) app.closeConversation(idx);  // libère son processus ; elle reste dans l'espace de travail
        }
        return nid;
    }

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
    orchFormSeq_ = -1;
    std::string txt;
    if (util::readFile(project->path / kOrchFile, txt)) {
        try {
            orch_->fromJson(json::parse(txt));
        } catch (const std::exception& e) {
            toast(std::string("Orchestrateur illisible : ") + e.what(), true);
        }
    } else {
        claude::applyBudgetPreset(orch_->settings, claude::BudgetPreset::Medium);
    }
    if (orch_->settings.objective.empty()) orch_->settings.objective = kDefaultObjective;
    orchSeenQuestion_ = orch_->questionSeq;
    return true;
}

void App::saveOrchestrator() {
    if (!orch_) return;
    ws::Folder* f = orchProjectFolder();
    if (!f) return;
    if (orch_->state == claude::OrchState::Idle && orch_->log.empty() && orch_->brainConvId.empty() && !orch_->changed)
        return;  // rien à garder
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

void App::orchestratorResume() {
    if (!orch_) return;
    AppOrchHost h(*this);
    orch_->resume(h);
    saveOrchestrator();
}

void App::tickOrchestrator() {
    if (!orch_) return;
    if (!orchProjectFolder()) {
        if (orch_->running()) orch_->pause("le projet n'existe plus dans l'espace de travail");
        return;
    }
    AppOrchHost h(*this);
    orch_->tick(h);
    // Nouvelle question : la fenêtre système clignote ; la barre d'état et le sélecteur de pages le signalent.
    if (orch_->state == claude::OrchState::WaitingHuman && orch_->questionSeq != orchSeenQuestion_) {
        orchSeenQuestion_ = orch_->questionSeq;
        if (!windowFocused_) SDL_FlashWindow(window_, SDL_FLASH_UNTIL_FOCUSED);
    }
    if (orch_->changed && (now_ - orchLastSave_ > 2.0 || !orch_->running())) saveOrchestrator();
}

// ---------------------------------------------------------------------------
// Page « Orchestrateur »
// ---------------------------------------------------------------------------

bool App::isOrchBrain(const std::string& convId) const {
    return orch_ && !convId.empty() && orch_->brainConvId == convId;
}

void App::openOrchestratorPage(bool discussion) {
    if (dash_ && ImGui::GetCurrentContext() != mainCtx_) SDL_RaiseWindow(window_);
    page_ = 2;
    if (discussion) {
        orchShowDiscussion_ = true;
        if (orch_ && !orch_->brainConvId.empty()) {
            AppOrchHost host(*this);
            host.ensureOpen(orch_->brainConvId);
        }
    }
    SDL_RaiseWindow(window_);
}

void App::autoLoadOrchestrator() {
    if (orch_) return;
    ws::Folder* best = nullptr;
    fs::file_time_type bestT{};
    for (auto& p : workspace_.projects()) {
        std::error_code ec;
        auto f = p->path / kOrchFile;
        if (!fs::exists(f, ec)) continue;
        auto t = fs::last_write_time(f, ec);
        if (!best || t > bestT) best = p.get(), bestT = t;
    }
    if (best) loadOrchestrator(best);
}

// État + budget + commandes.
// Jauge de budget : remplissage jusqu'à cible + tolérance, repère de la cible, zone de tolérance teintée.
void App::drawBudgetGauge(double f, double tol, float width, float height) {
    const auto& pal = ui::palette();
    tol = std::max(0.0, tol);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float span = (float)(1.0 + tol);
    float xt = p0.x + width * (1.0f / span);  // position de la cible
    ImVec2 p1(p0.x + width, p0.y + height);
    float r = height * 0.5f;
    dl->AddRectFilled(p0, p1, ui::col(pal.cardBg), r);
    if (tol > 0) dl->AddRectFilled(ImVec2(xt, p0.y), p1, ui::col(pal.warn, 0.16f), r, ImDrawFlags_RoundCornersRight);
    float fill = (float)std::clamp(f / span, 0.0, 1.0);
    ImVec4 c = f >= 1.0 + tol ? pal.err : f >= 1.0 ? pal.warn : f >= 0.75 ? ImVec4(0.90f, 0.72f, 0.25f, 1) : pal.ok;
    if (fill > 0) dl->AddRectFilled(p0, ImVec2(p0.x + std::max(width * fill, r * 2), p1.y), ui::col(c), r);
    // Repères : phase finale (75 %) et cible
    float x75 = p0.x + width * (0.75f / span);
    dl->AddLine(ImVec2(x75, p0.y + 2), ImVec2(x75, p1.y - 2), ui::col(pal.dim, 0.6f), 1.0f);
    dl->AddLine(ImVec2(xt, p0.y - 3), ImVec2(xt, p1.y + 3), ui::col(ImGui::GetStyleColorVec4(ImGuiCol_Text), 0.9f), 2.0f);
    dl->AddRect(p0, p1, ui::col(ImGui::GetStyleColorVec4(ImGuiCol_Border)), r);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Trait fin : phase finale (75 %%) — l'orchestrateur resserre le périmètre.\n"
                          "Trait épais : la cible (100 %%).\n"
                          "Zone orangée : dépassement toléré (+%d %%) pour terminer proprement ; arrêt ferme au bout.",
                          (int)std::lround(tol * 100));
}

// Carte « Budget » : chiffres lisibles, jauge, quota du compte, relance.
void App::drawOrchestratorBudgetCard(bool sub) {
    const auto& pal = ui::palette();
    auto& o = *orch_;
    AppOrchHost host(*this);
    float fs = ImGui::GetFontSize();
    const claude::RateWindow* five = usage_.find("five_hour");
    const claude::RateWindow* week = usage_.find("seven_day");
    int64_t nowT = util::nowUnix();
    double tol = std::max(0.0, o.settings.budgetTolerance);
    bool idle = o.state == claude::OrchState::Idle;

    // Titre + relance
    ImGui::PushFont(fontBold, 0.0f);
    ImGui::TextUnformatted("Budget");
    ImGui::PopFont();
    ImGui::SameLine();
    if (sub)
        ImGui::TextDisabled("abonnement%s · session de 5 h en cours",
                            usage_.subscription.empty() ? "" : (" " + usage_.subscription).c_str());
    else
        ImGui::TextDisabled("API · en dollars");
    bool limited = sub ? o.settings.sessionShare > 0 : o.settings.maxCostUsd > 0;
    double spent = sub ? o.windowSpent : o.budgetSpentUsd();
    if (!idle && limited) {
        const char* lbl = "↺ Relancer le budget";
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(lbl).x -
                                                             ImGui::GetStyle().FramePadding.x * 2));
        ImGui::BeginDisabled(spent <= 1e-9);
        if (ImGui::SmallButton(lbl)) {
            o.resetBudget(host);
            saveOrchestrator();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Remet le compteur à 0 pour prolonger le travail au même niveau.\n"
                              "La limite reste la même, sauf s'il reste moins que cela dans la session :\n"
                              "elle est alors ramenée à ce qui reste (abonnement).");
    }

    auto fmt = [&](double v) { return sub ? pctText(v) : util::formatUsd(v); };
    auto stat = [&](const char* label, const std::string& value, ImVec4 color, float w) {
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", label);
        ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.55f);
        ImGui::TextColored(color, "%s", value.c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SameLine(0, std::max(fs, w - ImGui::GetItemRectSize().x));
    };

    if (idle) {
        // Avant le départ : ce que représentera le budget
        if (!limited) {
            stat("Limite", "Aucune", pal.dim, fs * 8);
            ImGui::NewLine();
            ImGui::TextDisabled(sub ? "Seuls les plafonds d'usage arrêtent l'orchestration ; elle enchaîne les sessions."
                                    : "Aucune limite de dépense.");
        } else if (sub && five) {
            double room = std::max(0.0, o.ceiling() - five->utilization);
            stat("Alloué pour cette session", "≈ " + pctText(o.settings.sessionShare * room), pal.accent, fs * 14);
            ImGui::NewLine();
            ImGui::TextDisabled("%s de ce qui reste avant le plafond (%s) · renouvelé à chaque nouvelle session",
                                pctText(o.settings.sessionShare).c_str(), pctText(room).c_str());
        } else if (sub) {
            stat("Alloué", pctText(o.settings.sessionShare) + " du restant", pal.accent, fs * 14);
            ImGui::NewLine();
            ImGui::TextDisabled("Quota mesuré au démarrage.");
        } else {
            stat("Limite", util::formatUsd(o.settings.maxCostUsd), pal.accent, fs * 10);
            ImGui::NewLine();
        }
    } else {
        double allowance = sub ? o.sessionAllowance() : o.settings.maxCostUsd;
        double f = limited ? o.budgetFraction() : -1;
        ImVec4 vc = f >= 1.0 + tol ? pal.err : f >= 1.0 ? pal.warn : f >= 0.75 ? ImVec4(0.90f, 0.72f, 0.25f, 1) : pal.ok;
        float colW = fs * 8.5f;
        stat("Consommé", fmt(spent), limited ? vc : ImGui::GetStyleColorVec4(ImGuiCol_Text), colW);
        if (limited) {
            bool capped = sub && five && o.settings.usagePause > 0 && five->utilization >= o.settings.usagePause;
            stat("Limite", capped ? std::string("—") : fmt(allowance), ImGui::GetStyleColorVec4(ImGuiCol_Text), colW);
            stat("Reste", capped ? std::string("—") : fmt(std::max(0.0, allowance - spent)), pal.dim, colW);
            ImGui::BeginGroup();
            ImGui::TextDisabled("Utilisé");
            ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.55f);
            ImGui::TextColored(vc, "%d %%", (int)std::lround(f * 100));
            ImGui::PopFont();
            ImGui::EndGroup();
            drawBudgetGauge(f, tol, -1, fs * 0.85f);
            bool atCeiling = sub && five && o.settings.usagePause > 0 && five->utilization >= o.settings.usagePause;
            if (atCeiling)
                ImGui::TextColored(pal.warn, "Plafond du compte atteint (%s ≥ %s) : l'orchestration attend la prochaine session "
                                             "de 5 h%s, puis reprend avec un budget renouvelé.",
                                   pctText(five->utilization).c_str(), pctText(o.settings.usagePause).c_str(),
                                   five->resetsAt > nowT ? (" (dans " + util::humanDuration(five->resetsAt - nowT) + ")").c_str() : "");
            else if (f >= 1.0 + tol)
                ImGui::TextColored(pal.err, "Budget épuisé (cible + tolérance).");
            else if (f >= 1.0)
                ImGui::TextColored(pal.warn, "Cible atteinte : dépassement toléré jusqu'à %s pour terminer proprement.",
                                   pctText(1.0 + tol).c_str());
            else if (f >= 0.75)
                ImGui::TextColored(ImVec4(0.90f, 0.72f, 0.25f, 1), "Phase finale : l'orchestrateur resserre le périmètre.");
        } else {
            ImGui::NewLine();
            ImGui::TextDisabled("Sans limite%s.", sub ? " : seuls les plafonds d'usage arrêtent l'orchestration" : "");
        }
    }
    // Quota du compte et équivalent API
    if (sub && five) {
        std::string reset = five->resetsAt > nowT ? ", réinitialisation dans " + util::humanDuration(five->resetsAt - nowT) : "";
        ImGui::TextDisabled("Compte : fenêtre de 5 h à %s (plafond %s%s) · semaine à %s", pctText(five->utilization).c_str(),
                            pctText(o.settings.usagePause).c_str(), reset.c_str(), week ? pctText(week->utilization).c_str() : "?");
    }
    if (sub && o.spentUsd > 0) ImGui::TextDisabled("Équivalent API indicatif : %s (non facturé)", util::formatUsd(o.spentUsd).c_str());
    else if (!sub && o.spentUsd > o.budgetSpentUsd() + 1e-9)
        ImGui::TextDisabled("Total depuis le départ : %s", util::formatUsd(o.spentUsd).c_str());
}

// Avancement + budget, côte à côte quand la place le permet.
void App::drawOrchestratorStatus() {
    const auto& pal = ui::palette();
    AppOrchHost host(*this);
    auto& o = *orch_;
    ws::Folder* proj = orchProjectFolder();
    float fs = ImGui::GetFontSize();
    bool sub = o.state == claude::OrchState::Idle ? claude::Orchestrator::resolveSubscription(o.settings, host.isSubscription())
                                                  : o.subscription;
    float avail = ImGui::GetContentRegionAvail().x;
    bool wide = avail > fs * 62;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float budgetW = wide ? std::clamp(avail * 0.5f, fs * 36, fs * 58) : avail;
    float progW = wide ? avail - budgetW - gap : avail;
    float cardH = ImGui::GetTextLineHeightWithSpacing() * 8.4f;
    auto beginCard = [&](const char* id, float w) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.cardBg);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::BeginChild(id, ImVec2(w, wide ? cardH : 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | (wide ? 0 : ImGuiChildFlags_AutoResizeY));
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    };

    ImGui::Spacing();
    // --- Avancement ---
    beginCard("##progress", progW);
    {
        ImVec4 sc = o.state == claude::OrchState::Finished                                        ? pal.ok
                    : o.state == claude::OrchState::WaitingHuman                                  ? pal.warn
                    : o.state == claude::OrchState::Paused || o.state == claude::OrchState::UsagePause ? pal.warn
                    : o.running()                                                                 ? pal.accent
                                                                                                  : pal.dim;
        ImGui::PushFont(fontBold, 0.0f);
        ImGui::TextUnformatted("Avancement");
        ImGui::PopFont();
        ImGui::SameLine();
        if (o.state == claude::OrchState::Thinking || o.state == claude::OrchState::Working) {
            ui::spinner((float)now_, pal.accent);
            ImGui::SameLine();
        }
        ui::badge(o.brainPaused() ? "En pause jusqu'à la prochaine session" : claude::orchStateLabel(o.state), sc);
        ImGui::SameLine();
        ImGui::TextDisabled("cycle %d%s", o.cycle, o.settings.maxCycles > 0 ? (" / " + std::to_string(o.settings.maxCycles)).c_str() : "");
        if (o.finalPhase()) {
            ImGui::SameLine();
            ui::badge("conclusion demandée", pal.warn);
        }
        // Résumé de l'équipe
        int working = 0, waiting = 0, done = 0, failed = 0, queued = 0;
        if (proj)
            for (auto& ch : proj->children) {
                AgentView v = agentView(ch->name);
                working += v.working;
                waiting += v.attention;
                if (v.task && v.task->done) (v.task->failed ? failed : done)++;
                if (v.task && !v.task->sent && !v.task->done) ++queued;
            }
        std::string team = std::to_string(proj ? proj->children.size() : 0) + " agents";
        if (working) team += " · " + std::to_string(working) + " au travail";
        if (waiting) team += " · " + std::to_string(waiting) + " vous attendent";
        if (queued) team += " · " + std::to_string(queued) + " en file";
        if (done) team += " · " + std::to_string(done) + " terminé(s)";
        if (failed) team += " · " + std::to_string(failed) + " échec(s)";
        ImGui::TextDisabled("Équipe : %s", team.c_str());
        if (!o.log.empty()) {
            std::string ev = o.log.back().text;
            float room = ImGui::GetContentRegionAvail().x - fs * 6;
            while (ev.size() > 8 && ImGui::CalcTextSize(ev.c_str()).x > room) ev = util::truncateUtf8(ev, ev.size() - 6);
            ImGui::TextDisabled("Dernier événement :");
            ImGui::SameLine();
            ImGui::TextUnformatted((ev + (ev != o.log.back().text ? "…" : "")).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", o.log.back().text.c_str());
        }
        if (!o.pauseReason.empty() && (o.state == claude::OrchState::Paused || o.state == claude::OrchState::UsagePause)) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(pal.warn, "%s", o.pauseReason.c_str());
            ImGui::PopTextWrapPos();
        }
        // Commandes
        ImGui::Spacing();
        bool paused = o.state == claude::OrchState::Paused || o.state == claude::OrchState::UsagePause;
        if (!o.running() && !paused) {
            ImGui::BeginDisabled(util::trim(o.settings.objective).empty() || !proj || proj->children.empty() || claudeExe_.empty());
            if (ImGui::Button(o.state == claude::OrchState::Idle && o.cycle == 0 ? "▶ Démarrer" : "▶ Relancer")) orchestratorStart();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Lance l'orchestrateur sur l'objectif (onglet Réglages).\n"
                                  "Une orchestration relancée repart de zéro (cycles, budget, journal),\n"
                                  "mais la conversation « Orchestrateur » garde sa mémoire.");
            ImGui::SameLine();
        }
        if (o.running()) {
            if (ImGui::Button("⏸ Pause")) {
                o.pause("mise en pause par vous (les tours en cours se terminent)");
                saveOrchestrator();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aucune nouvelle tâche n'est envoyée ; les agents finissent leur tour en cours.");
            ImGui::SameLine();
        }
        if (paused) {
            if (ImGui::Button("▶ Reprendre")) {
                o.resume(host);
                saveOrchestrator();
            }
            ImGui::SameLine();
        }
        if (o.state != claude::OrchState::Idle) {
            if (ImGui::Button("■ Arrêter")) {
                o.stop();
                saveOrchestrator();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Arrête l'orchestration. Les tours en cours ne sont pas interrompus.");
        }
    }
    ImGui::EndChild();
    if (wide) ImGui::SameLine(0, gap);

    // --- Budget ---
    beginCard("##budgetcard", budgetW);
    drawOrchestratorBudgetCard(sub);
    ImGui::EndChild();

    if (o.state == claude::OrchState::Finished && !o.finalSummary.empty()) {
        std::string first;
        for (auto& l : util::splitLines(o.finalSummary)) {
            std::string t = util::trim(l);
            util::replaceAll(t, "**", "");
            if (!t.empty()) {
                first = t;
                break;
            }
        }
        ImGui::TextColored(pal.ok, "✓");
        ImGui::SameLine();
        float room = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Lire le bilan").x - fs * 3;
        std::string line = first;
        while (line.size() > 8 && ImGui::CalcTextSize(line.c_str()).x > room) line = util::truncateUtf8(line, line.size() - 6);
        ImGui::TextUnformatted((line + (line != first ? "…" : "")).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Lire le bilan")) orchShowBilan_ = true;
    }
    ImGui::Spacing();
}

// Pause décidée par l'orchestrateur lui-même (@@PAUSE) : une information, pas une question.
void App::drawOrchestratorPauseNote() {
    const auto& pal = ui::palette();
    AppOrchHost host(*this);
    auto& o = *orch_;
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(pal.link.x, pal.link.y, pal.link.z, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(pal.link.x, pal.link.y, pal.link.z, 0.06f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::max(ImGui::GetFontSize() * 12,
                                                                              ImGui::GetMainViewport()->WorkSize.y * 0.40f)));
    ImGui::BeginChild("##brainpause", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(fontBold, 0.0f);
    ImGui::TextColored(pal.link, "⏸ L'orchestrateur s'est mis en pause");
    ImGui::PopFont();
    int64_t nowS = host.now();
    if (o.state == claude::OrchState::UsagePause && o.pauseUntil > nowS)
        ImGui::TextDisabled("Rien à faire de votre côté : il reprendra seul à la prochaine session de 5 h (dans %s), avec son plan.",
                            util::humanDuration(o.pauseUntil - nowS).c_str());
    else if (o.state == claude::OrchState::UsagePause)
        ImGui::TextDisabled("Rien à faire de votre côté : il reprendra seul à la prochaine session de 5 h, avec son plan.");
    else
        ImGui::TextDisabled("Mise en pause manuelle par-dessus : cliquez sur Reprendre pour qu'il reprenne son plan.");
    ImGui::Spacing();
    {
        ui::MdStyle md{fontUi, fontBold, fontMono, settings_.wrapCode, fontItalic};
        static std::string cachedText;
        static std::vector<ui::MdBlock> cachedBlocks;
        if (cachedText != o.pauseNote) {
            cachedText = o.pauseNote;
            cachedBlocks = ui::parseMarkdown(o.pauseNote);
        }
        ui::renderMarkdown(cachedBlocks, md, 977001);
    }
    ImGui::Spacing();
    if (ImGui::Button("▶ Reprendre maintenant")) {
        o.resume(host);
        saveOrchestrator();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Relance l'orchestrateur tout de suite avec son plan de reprise, sans attendre la session suivante.");
    ImGui::SameLine();
    if (ImGui::Button("Voir la discussion")) openOrchestratorPage(true);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// Carte de réponse aux questions @@HUMAIN.
void App::drawOrchestratorQuestion() {
    const auto& pal = ui::palette();
    AppOrchHost host(*this);
    auto& o = *orch_;
    ws::Folder* proj = orchProjectFolder();
    (void)proj;
    (void)pal;
    // --- Question de l'orchestrateur ---------------------------------------------------------
    if (o.state == claude::OrchState::WaitingHuman) {
        std::vector<claude::HumanQuestion> qs = o.questions;
        if (qs.empty()) {
            claude::HumanQuestion q;
            q.title = q.text = o.humanQuestion;
            qs.push_back(q);
        }
        if (orchFormSeq_ != o.questionSeq || orchAnswerSel_.size() != qs.size()) {
            orchFormSeq_ = o.questionSeq;
            orchAnswerSel_.assign(qs.size(), -1);
            orchAnswerOther_.assign(qs.size(), std::string());
            orchAnswerExtra_.clear();
            // Option marquée « (recommandé) » présélectionnée
            for (size_t i = 0; i < qs.size(); ++i)
                for (size_t k = 0; k < qs[i].options.size(); ++k)
                    if (util::containsInsensitive(qs[i].options[k], "recommand")) {
                        orchAnswerSel_[i] = (int)k;
                        break;
                    }
        }
        float pulse = settings_.animations ? 0.6f + 0.4f * (float)std::sin(now_ * 3.0) : 1.0f;
        if (settings_.animations) ui::anim::markBusy();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(pal.warn.x, pal.warn.y, pal.warn.z, pulse));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(pal.warn.x, pal.warn.y, pal.warn.z, 0.07f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        // Hauteur limitée (défilement au-delà) : l'équipe et la discussion restent visibles en dessous.
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, std::max(ImGui::GetFontSize() * 14,
                                                                                  ImGui::GetMainViewport()->WorkSize.y * 0.45f)));
        ImGui::BeginChild("##question", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushFont(fontBold, 0.0f);
        ImGui::TextColored(pal.warn, qs.size() > 1 ? "❓ L'orchestrateur a %d questions pour vous" : "❓ L'orchestrateur a besoin de vous",
                           (int)qs.size());
        ImGui::PopFont();
        ImGui::TextDisabled("L'orchestration est en attente. Elle reprend dès votre réponse.");
        ui::MdStyle md{fontUi, fontBold, fontMono, settings_.wrapCode, fontItalic};
        bool anyAnswer = !util::trim(orchAnswerExtra_).empty();
        for (size_t i = 0; i < qs.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::Spacing();
            if (qs.size() > 1) {
                if (i > 0) ImGui::Separator();
                ImGui::TextColored(pal.accent, "Question %d / %d", (int)i + 1, (int)qs.size());
            }
            ui::renderMarkdown(ui::parseMarkdown(qs[i].text), md, 880000 + (int)i);
            int& sel = orchAnswerSel_[i];
            for (size_t k = 0; k < qs[i].options.size(); ++k) {
                bool on = sel == (int)k;
                bool rec = util::containsInsensitive(qs[i].options[k], "recommand");
                ImGui::PushID((int)k);
                if (on) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.35f));
                std::string label = std::string(on ? "◉  " : "○  ") + qs[i].options[k];
                if (ImGui::Selectable(label.c_str(), on, ImGuiSelectableFlags_None)) sel = (int)k;
                if (on) ImGui::PopStyleColor();
                if (rec && !on && ImGui::IsItemHovered()) ImGui::SetTooltip("Option recommandée par l'orchestrateur");
                ImGui::PopID();
            }
            if (!qs[i].options.empty()) {
                bool other = sel == -2;
                if (ImGui::Selectable(other ? "◉  Autre :" : "○  Autre :", other, ImGuiSelectableFlags_None,
                                      ImVec2(ImGui::CalcTextSize("○  Autre :").x, 0)))
                    sel = -2;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputTextWithHint("##other", "votre propre réponse", &orchAnswerOther_[i])) sel = -2;
            } else {
                // Réponses suggérées dans la question (« réponds « … » ») : un clic remplit la réponse.
                for (size_t k = 0; k < qs[i].suggestions.size(); ++k) {
                    ImGui::PushID(500 + (int)k);
                    std::string b = "Répondre « " + qs[i].suggestions[k] + " »";
                    if (ImGui::Button(b.c_str())) orchAnswerOther_[i] = qs[i].suggestions[k];
                    ImGui::PopID();
                    ImGui::SameLine();
                }
                if (!qs[i].suggestions.empty()) ImGui::NewLine();
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextMultiline("##free", &orchAnswerOther_[i], ImVec2(-1, ImGui::GetTextLineHeight() * 2.5f),
                                          ImGuiInputTextFlags_WordWrap);
                placeholder(orchAnswerOther_[i], "Votre réponse…");
                if (!util::trim(orchAnswerOther_[i]).empty()) sel = -2;
            }
            if (sel >= 0 || (sel == -2 && !util::trim(orchAnswerOther_[i]).empty())) anyAnswer = true;
            ImGui::PopID();
        }
        ImGui::Spacing();
        ImGui::TextDisabled("Précisions pour l'orchestrateur (facultatif)");
        ImGui::InputTextMultiline("##extra", &orchAnswerExtra_, ImVec2(-1, ImGui::GetTextLineHeight() * 2.5f),
                                  ImGuiInputTextFlags_WordWrap);
        placeholder(orchAnswerExtra_, "Une consigne pour la suite, une contrainte, un contexte…");
        bool submit = false;
        ImGui::BeginDisabled(!anyAnswer);
        ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
        submit = ImGui::Button("Envoyer la réponse");
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (anyAnswer && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_Enter, false))
            submit = true;
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+Entrée");
        ImGui::SameLine();
        if (ImGui::Button("Laisse-le décider"))
            if (o.answerHuman(host, "Je te laisse décider : choisis l'option la plus prudente (ou celle que tu "
                                    "recommandes), justifie-la en une phrase, et continue."))
                saveOrchestrator();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("L'orchestrateur tranche lui-même et continue.");
        ImGui::SameLine();
        if (ImGui::Button("Voir la discussion"))
            openOrchestratorPage(true);
        if (submit) {
            std::string answer;
            for (size_t i = 0; i < qs.size(); ++i) {
                std::string a = orchAnswerSel_[i] >= 0 ? qs[i].options[orchAnswerSel_[i]] : util::trim(orchAnswerOther_[i]);
                std::string title = qs[i].title.empty() ? util::firstLine(qs[i].text, 140) : qs[i].title;
                if (qs.size() > 1) answer += "**" + std::to_string(i + 1) + ". " + title + "**\n";
                else if (!qs[i].options.empty()) answer += "**" + title + "**\n";
                answer += (a.empty() ? "(pas de réponse : décide)" : a) + "\n\n";
            }
            if (!util::trim(orchAnswerExtra_).empty()) answer += "Précisions : " + util::trim(orchAnswerExtra_) + "\n";
            if (o.answerHuman(host, util::trim(answer))) saveOrchestrator();
            else toast("Réponse impossible pour l'instant : la conversation de l'orchestrateur est occupée.", true);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

}

// Réglages : page simple (niveaux de budget) ou avancée (onglets).
void App::drawOrchestratorSettings() {
    const auto& pal = ui::palette();
    AppOrchHost host(*this);
    auto& o = *orch_;
    ws::Folder* proj = orchProjectFolder();
    (void)proj;
    (void)pal;
    // --- Réglages ------------------------------------------------------------------------------
    if (orchSettingsTab_ > 0) orchAdvanced_ = true;
    {
        float w = ImGui::GetFontSize() * 10;
        auto changedIf = [&](bool c) {
            if (c) o.changed = true;
        };
        bool subNow = o.running() ? o.subscription : claude::Orchestrator::resolveSubscription(o.settings, host.isSubscription());
        int64_t nowT = util::nowUnix();
        const claude::RateWindow* five = usage_.find("five_hour");
        const claude::RateWindow* week = usage_.find("seven_day");
        double rem = five ? std::max(0.0, (o.settings.usagePause > 0 ? o.settings.usagePause : 1.0) - five->utilization) : 1.0;
        std::string resetIn = five && five->resetsAt > nowT ? util::humanDuration(five->resetsAt - nowT) : std::string();

        if (!orchAdvanced_) {
            // ---------------- Page simple : objectif + niveau de budget ----------------
            ImGui::PushFont(fontBold, 0.0f);
            ImGui::TextUnformatted("Objectif");
            ImGui::PopFont();
            ImGui::BeginDisabled(o.running());
            changedIf(ImGui::InputTextMultiline("##objective", &o.settings.objective, ImVec2(-1, ImGui::GetTextLineHeight() * 4),
                                                ImGuiInputTextFlags_WordWrap));
            ImGui::EndDisabled();
            ImGui::TextDisabled("Un objectif vérifiable donne de meilleurs résultats : « … avec les tests qui passent ».");

            ImGui::Spacing();
            ImGui::PushFont(fontBold, 0.0f);
            ImGui::TextUnformatted("Budget");
            ImGui::PopFont();
            ImGui::SameLine();
            if (subNow)
                ImGui::TextDisabled("— abonnement%s : part de votre quota",
                                    usage_.subscription.empty() ? "" : (" " + usage_.subscription).c_str());
            else
                ImGui::TextDisabled("— facturation API : en dollars");
            ImGui::SameLine();
            ui::helpMarker("Le budget est la contrainte principale d'une orchestration : chaque niveau règle d'un coup\n"
                           "la dépense autorisée, les modèles permis, l'effort maximal, le nombre d'agents en parallèle\n"
                           "et le nombre de cycles. L'orchestrateur est prévenu à 75 % du budget et doit conclure\n"
                           "quand il est épuisé. Les réglages avancés permettent d'ajuster chaque paramètre.");

            // Cartes des niveaux
            float avail = ImGui::GetContentRegionAvail().x;
            float gap = ImGui::GetStyle().ItemSpacing.x;
            int cols = avail > ImGui::GetFontSize() * 46 ? 4 : 2;
            float cw = (avail - gap * (cols - 1)) / cols;
            ImVec2 pad = ImGui::GetStyle().FramePadding;
            // Hauteur commune : celle du résumé le plus long une fois renvoyé à la ligne.
            float ch = 0;
            for (claude::BudgetPreset p : claude::kBudgetPresets) {
                std::string sum = claude::budgetPresetSummary(p, subNow, five ? rem : -1.0);
                float th = fontUi->CalcTextSizeA(ImGui::GetFontSize() * 0.92f, FLT_MAX, cw - pad.x * 3, sum.c_str()).y;
                ch = std::max(ch, th);
            }
            ch += ImGui::GetTextLineHeightWithSpacing() * 1.15f + pad.y * 5;
            bool anyMatch = false;
            int idx = 0;
            for (claude::BudgetPreset p : claude::kBudgetPresets) {
                if (idx % cols) ImGui::SameLine(0, gap);
                ImGui::PushID(idx++);
                ImVec2 p0 = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::InvisibleButton("##preset", ImVec2(cw, ch));
                bool hov = ImGui::IsItemHovered();
                bool active = claude::budgetPresetMatches(o.settings, p);
                anyMatch |= active;
                ImVec2 p1(p0.x + cw, p0.y + ch);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec4 bg = active ? ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.20f) : pal.cardBg;
                if (hov && !active) bg.w = std::min(1.0f, bg.w + 0.15f);
                dl->AddRectFilled(p0, p1, ui::col(bg), 8.0f);
                dl->AddRect(p0, p1, ui::col(active ? pal.accent : hov ? pal.dim : ImGui::GetStyleColorVec4(ImGuiCol_Border)), 8.0f, 0,
                            active ? 2.0f : 1.0f);
                ImVec2 tp(p0.x + pad.x * 1.5f, p0.y + pad.y * 2);
                std::string title = std::string(active ? "● " : "") + claude::budgetPresetLabel(p, subNow);
                dl->AddText(fontBold, ImGui::GetFontSize(), tp, ui::col(active ? pal.accent : ImGui::GetStyleColorVec4(ImGuiCol_Text)),
                            title.c_str());
                std::string sum = claude::budgetPresetSummary(p, subNow, five ? rem : -1.0);
                dl->AddText(fontUi, ImGui::GetFontSize() * 0.92f, ImVec2(tp.x, tp.y + ImGui::GetTextLineHeightWithSpacing() * 1.15f),
                            ui::col(pal.dim), sum.c_str(), nullptr, cw - pad.x * 3);
                if (hov) ImGui::SetTooltip("%s\nCliquez pour appliquer ce niveau.", sum.c_str());
                if (clicked) {
                    claude::applyBudgetPreset(o.settings, p);
                    o.changed = true;
                }
                ImGui::PopID();
            }
            if (!anyMatch)
                ImGui::TextColored(pal.link, "Réglages personnalisés (ajustés dans les réglages avancés). Cliquez un niveau pour y revenir.");

            // État du quota et conséquences du niveau choisi
            ImGui::PushTextWrapPos(0);
            if (subNow && five) {
                ImGui::TextDisabled("Votre quota : fenêtre de 5 h à %s%s · semaine à %s.", pctText(five->utilization).c_str(),
                                    resetIn.empty() ? "" : (" (réinitialisation dans " + resetIn + ")").c_str(),
                                    week ? pctText(week->utilization).c_str() : "?");
                if (rem < 0.05)
                    ImGui::TextColored(pal.warn, "Il ne reste que %s avant le plafond de %s : l'orchestration se mettra vite en "
                                                 "pause, puis reprendra seule à la réinitialisation%s.",
                                       pctText(rem).c_str(), pctText(o.settings.usagePause).c_str(),
                                       resetIn.empty() ? "" : (" (dans " + resetIn + ")").c_str());
                else if (o.settings.sessionShare > 0)
                    ImGui::TextDisabled("Pour cette session : ≈ %s de la fenêtre alloués à l'orchestration (%s des %s qui "
                                        "restent avant le plafond). Le budget se renouvelle à chaque nouvelle session de 5 h.",
                                        pctText(o.settings.sessionShare * rem).c_str(), pctText(o.settings.sessionShare).c_str(),
                                        pctText(rem).c_str());
                else
                    ImGui::TextDisabled("Sans limite : l'orchestration enchaîne les sessions, en pause à %s de chaque fenêtre.",
                                        pctText(o.settings.usagePause).c_str());
            } else if (subNow) {
                ImGui::TextDisabled("Votre quota sera mesuré dès la première réponse de Claude.");
            }
            if (o.settings.budgetTolerance > 0)
                ImGui::TextDisabled("Dépassement toléré : +%s du budget pour finir proprement plutôt que de bâcler la fin "
                                    "(réglable dans les réglages avancés).",
                                    pctText(o.settings.budgetTolerance).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("Réglages avancés ▸")) orchAdvanced_ = true;
            ImGui::SameLine();
            ImGui::TextDisabled("budget détaillé, modèles, parallélisme, modes de permission");
        } else if (ImGui::Button("◂ Réglages simples")) {
            orchAdvanced_ = false;
        }

        auto tabFlags = [&](int i) {
            if (orchSettingsTab_ != i) return ImGuiTabItemFlags_None;
            orchSettingsTab_ = -1;
            return ImGuiTabItemFlags_SetSelected;
        };
        if (orchAdvanced_ && ImGui::BeginTabBar("##orchtabs")) {
        if (ImGui::BeginTabItem("Budget détaillé", nullptr, tabFlags(1))) {
            bool detected = host.isSubscription();
            ImGui::BeginDisabled(o.running());
            ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("Type de budget").x + ImGui::GetStyle().ItemInnerSpacing.x * 2));
            std::string autoLabel = std::string("Automatique — ") +
                                    (detected ? "abonnement détecté" + (usage_.subscription.empty() ? std::string() : " (" + usage_.subscription + ")")
                                              : "facturation API");
            const char* modes[] = {"auto", "subscription", "api"};
            std::string labels[] = {autoLabel, "Abonnement : part du quota", "API : dollars"};
            int cur = o.settings.budgetMode == "subscription" ? 1 : o.settings.budgetMode == "api" ? 2 : 0;
            if (ImGui::BeginCombo("Type de budget", labels[cur].c_str())) {
                for (int i = 0; i < 3; ++i)
                    if (ImGui::Selectable(labels[i].c_str(), cur == i)) {
                        o.settings.budgetMode = modes[i];
                        o.changed = true;
                    }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            bool sub = o.running() ? o.subscription : claude::Orchestrator::resolveSubscription(o.settings, detected);
            ImGui::Spacing();
            if (sub) {
                if (!five) {
                    ImGui::TextWrapped("Pas encore de mesure du quota : elle arrive avec la première réponse de Claude.");
                    if (ImGui::SmallButton("Mesurer maintenant")) refreshUsage();
                } else {
                    std::string reset = five->resetsAt > nowT ? " · réinitialisation dans " + util::humanDuration(five->resetsAt - nowT) : "";
                    ImGui::Text("Fenêtre de 5 h : %s utilisés%s", pctText(five->utilization).c_str(), reset.c_str());
                    if (week) ImGui::TextDisabled("Semaine : %s utilisés", pctText(week->utilization).c_str());
                }

                ImGui::Spacing();
                ImGui::TextUnformatted("Part de ce qui reste dans la session, allouée à l'orchestration");
                int share = (int)std::lround(o.settings.sessionShare * 100);
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("##share", &share, 0, 100, share == 0 ? "sans limite" : "%d %% du restant")) {
                    o.settings.sessionShare = share / 100.0;
                    o.changed = true;
                }
                auto quick = [&](const char* label, double v) {
                    std::string l = std::string(label) + (v > 0 && five ? " (≈ " + pctText(v * rem) + ")" : std::string());
                    if (ImGui::SmallButton(l.c_str())) {
                        o.settings.sessionShare = v;
                        o.changed = true;
                    }
                    ImGui::SameLine();
                };
                quick("Tout le restant", 1.0);
                quick("La moitié", 0.5);
                quick("Un quart", 0.25);
                quick("Sans limite", 0.0);
                ImGui::NewLine();
                if (o.windowSpent > 0 || o.windowBase >= 0) {
                    if (ImGui::SmallButton("Relancer le budget (compteur à 0, même limite)")) {
                        o.resetBudget(host);
                        saveOrchestrator();
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Considère que l'orchestration n'a encore rien consommé dans cette session de 5 h\n"
                                          "et recalcule sa part sur ce qui reste maintenant.");
                }
                ImGui::PushTextWrapPos(0);
                if (o.settings.sessionShare <= 0)
                    ImGui::TextDisabled("Sans limite : seuls les plafonds ci-dessous arrêtent l'orchestration, qui enchaîne les sessions.");
                else if (five)
                    ImGui::TextDisabled("Pour cette session : ≈ %s de la fenêtre (il reste %s avant le plafond). Le budget se "
                                        "renouvelle à chaque nouvelle session de 5 h.",
                                        pctText(o.settings.sessionShare * rem).c_str(), pctText(rem).c_str());
                ImGui::PopTextWrapPos();

                ImGui::Spacing();
                int p5 = (int)std::lround(o.settings.usagePause * 100), p7 = (int)std::lround(o.settings.weeklyPause * 100);
                ImGui::SetNextItemWidth(w);
                if (ImGui::SliderInt("Plafond de la fenêtre de 5 h", &p5, 0, 100, p5 == 0 ? "aucun" : "%d %%")) {
                    o.settings.usagePause = p5 / 100.0;
                    o.changed = true;
                }
                ImGui::SameLine();
                ui::helpMarker("Au-delà, l'orchestration se met en pause et reprend seule à la réinitialisation\n"
                               "de la fenêtre : vous gardez de la marge pour travailler à côté.");
                ImGui::SetNextItemWidth(w);
                if (ImGui::SliderInt("Plafond hebdomadaire", &p7, 0, 100, p7 == 0 ? "aucun" : "%d %%")) {
                    o.settings.weeklyPause = p7 / 100.0;
                    o.changed = true;
                }
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("Le quota est mesuré pour tout le compte : vos autres conversations comptent aussi.");
                ImGui::PopTextWrapPos();
            } else {
                ImGui::SetNextItemWidth(w);
                if (ImGui::InputDouble("Budget en $ (0 = illimité)", &o.settings.maxCostUsd, 1.0, 10.0, "%.2f")) {
                    o.settings.maxCostUsd = std::max(0.0, o.settings.maxCostUsd);
                    o.changed = true;
                }
                for (double v : {5.0, 10.0, 25.0, 50.0}) {
                    std::string l = util::formatUsd(v);
                    if (ImGui::SmallButton(l.c_str())) {
                        o.settings.maxCostUsd = v;
                        o.changed = true;
                    }
                    ImGui::SameLine();
                }
                if (ImGui::SmallButton("Illimité")) {
                    o.settings.maxCostUsd = 0;
                    o.changed = true;
                }
                ImGui::TextDisabled("Coût cumulé des tours, tel que rapporté par Claude Code.");
            }
            ImGui::Spacing();
            {
                int tolPct = (int)std::lround(o.settings.budgetTolerance * 100);
                ImGui::SetNextItemWidth(w);
                if (ImGui::SliderInt("Dépassement toléré", &tolPct, 0, 50, tolPct == 0 ? "aucun" : "+%d %%")) {
                    o.settings.budgetTolerance = tolPct / 100.0;
                    o.changed = true;
                }
                ImGui::SameLine();
                ui::helpMarker("Marge au-delà de la cible, pour que l'orchestrateur ne bâcle pas la fin :\n"
                               "- en approchant de la cible, il sait qu'il peut finir sans se précipiter (il réduit\n"
                               "  le périmètre plutôt que la qualité) ;\n"
                               "- une fois la cible atteinte, il termine proprement ce qui est en cours, sans lancer\n"
                               "  de nouveau chantier ;\n"
                               "- l'arrêt ferme n'intervient qu'à cible + tolérance.");
            }
            changedIf(ImGui::Checkbox("Obtenir un résultat dans ce budget", &o.settings.strictBudget));
            ImGui::SameLine();
            ui::helpMarker("Activé : à 75 % du budget, l'orchestrateur passe en « phase finale » (l'essentiel,\n"
                           "des modèles économiques) ; une fois le budget épuisé, plus aucune tâche n'est\n"
                           "envoyée et il doit conclure avec un bilan de ce qui marche et de ce qui manque.\n"
                           "Ce dernier tour de conclusion peut dépasser légèrement le budget.\n\n"
                           "Désactivé : l'orchestration se met simplement en pause à la limite.");
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Modèles", nullptr, tabFlags(2))) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("L'orchestrateur choisit le modèle et l'effort de chaque tâche "
                                "(« @@DELEGUER agent | modele=sonnet effort=medium »), dans les limites ci-dessous.");
            ImGui::PopTextWrapPos();
            ImGui::TextUnformatted("Modèles autorisés");
            for (auto& m : kModels) {
                auto& al = o.settings.allowedModels;
                bool on = std::find(al.begin(), al.end(), m.value) != al.end();
                ImGui::SameLine();
                if (ImGui::Checkbox(m.label, &on)) {
                    if (on) al.push_back(m.value);
                    else if (al.size() > 1) al.erase(std::remove(al.begin(), al.end(), std::string(m.value)), al.end());
                    o.changed = true;
                }
            }
            auto effortCombo = [&](const char* label, std::string& v, const char* emptyLabel) {
                ImGui::SetNextItemWidth(w);
                const char* cur = emptyLabel;
                for (auto& c : claude::effortChoices())
                    if (v == c.value && *c.value) cur = c.label;
                if (ImGui::BeginCombo(label, cur)) {
                    for (auto& c : claude::effortChoices()) {
                        const char* l = *c.value ? c.label : emptyLabel;
                        if (ImGui::Selectable(l, v == c.value)) {
                            v = c.value;
                            o.changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            };
            auto modelCombo = [&](const char* label, std::string& v, const char* emptyLabel) {
                ImGui::SetNextItemWidth(w);
                const char* cur = emptyLabel;
                for (auto& m : kModels)
                    if (v == m.value) cur = m.label;
                if (ImGui::BeginCombo(label, cur)) {
                    if (ImGui::Selectable(emptyLabel, v.empty())) v.clear(), o.changed = true;
                    for (auto& m : kModels)
                        if (ImGui::Selectable(m.label, v == m.value)) v = m.value, o.changed = true;
                    ImGui::EndCombo();
                }
            };
            effortCombo("Effort maximal", o.settings.maxEffort, "Aucun plafond");
            ImGui::SeparatorText("Agents (si l'orchestrateur ne précise rien)");
            modelCombo("Modèle##agents", o.settings.defaultWorkerModel, "Celui de la conversation");
            effortCombo("Effort##agents", o.settings.defaultWorkerEffort, "Celui de la conversation");
            ImGui::SeparatorText("Orchestrateur");
            modelCombo("Modèle##brain", o.settings.brainModel, "Celui de la conversation");
            effortCombo("Effort##brain", o.settings.brainEffort, "Celui de la conversation");
            ImGui::SameLine();
            ui::helpMarker("L'orchestrateur raisonne sur tout le projet : un modèle capable (Opus) avec un effort\n"
                           "élevé y est souvent rentable, les agents se contentant de modèles plus économiques.");
            if (o.settings.allowedModels.size() < 4) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("Liste restreinte : une conversation sans modèle précisé utilisera le plus capable des "
                                    "modèles autorisés.");
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Exécution", nullptr, tabFlags(3))) {
            ImGui::SetNextItemWidth(w);
            changedIf(ImGui::SliderInt("Agents en parallèle", &o.settings.maxParallel, 1, 6));
            changedIf(ImGui::Checkbox("Comptes rendus au fil de l'eau", &o.settings.rollingReports));
            ImGui::SameLine();
            ui::helpMarker("Activé : dès qu'un agent termine alors que d'autres travaillent encore, l'orchestrateur reçoit\n"
                           "son compte rendu et peut aussitôt confier une nouvelle tâche aux agents libres (ou répondre\n"
                           "@@ATTENDRE). L'équipe reste occupée, au prix de quelques tours de plus pour l'orchestrateur.\n"
                           "Désactivé : il attend que toute l'équipe ait fini avant de décider de la suite.");
            ImGui::SetNextItemWidth(w);
            if (ImGui::InputInt("Cycles maximum (0 = illimité)", &o.settings.maxCycles)) {
                o.settings.maxCycles = std::max(0, o.settings.maxCycles);
                o.changed = true;
            }
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
            ImGui::SeparatorText("Contexte");
            changedIf(ImGui::Checkbox("Alléger régulièrement le contexte (récap + nouvelle conversation)", &o.settings.contextRotation));
            ImGui::SameLine();
            ui::helpMarker("Comme dans le reste de ClaudeDesk : plutôt que de laisser une conversation grossir (chaque tour\n"
                           "relit tout son contexte, ce qui consomme du quota), ClaudeDesk en fait le récap, ouvre une\n"
                           "conversation neuve dans le même dossier et y injecte la mémoire du projet (notes, synthèse,\n"
                           "récaps). Vaut pour les agents et pour l'orchestrateur.");
            ImGui::BeginDisabled(!o.settings.contextRotation);
            int ctxPct = (int)std::lround(o.settings.rotateAtContext * 100);
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderInt("Seuil de contexte", &ctxPct, 15, 90, "%d %%")) {
                o.settings.rotateAtContext = ctxPct / 100.0;
                o.changed = true;
            }
            ImGui::SetNextItemWidth(w);
            if (ImGui::SliderInt("Ou après N tâches par conversation", &o.settings.rotateAfterTasks, 0, 12,
                                 o.settings.rotateAfterTasks == 0 ? "jamais" : "%d"))
                o.changed = true;
            ImGui::EndDisabled();
            ImGui::SeparatorText("Permissions");
            modeCombo("Mode des agents", o.settings.workerMode);
            modeCombo("Mode de l'orchestrateur", o.settings.brainMode);
            ImGui::SameLine();
            ui::helpMarker("« Auto » : Claude exécute seul ce qui est sûr et ne demande que pour le reste.\n"
                           "Les demandes restantes apparaissent dans la conversation concernée et le tableau de bord ;\n"
                           "l'orchestration attend votre décision.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
        if (orchSettingsTab_ == 0) orchSettingsTab_ = -1;
    }

}

// Journal des décisions + questions passées.
void App::drawOrchestratorLog() {
    const auto& pal = ui::palette();
    AppOrchHost host(*this);
    auto& o = *orch_;
    ws::Folder* proj = orchProjectFolder();
    (void)proj;
    (void)pal;
    // --- Questions passées ---
    if (!o.qaHistory.empty()) {
        std::string hdr = "Questions et réponses (" + std::to_string(o.qaHistory.size()) + ")";
        if (ImGui::CollapsingHeader(hdr.c_str())) {
            for (auto it = o.qaHistory.rbegin(); it != o.qaHistory.rend(); ++it) {
                ImGui::TextDisabled("%s", clockOf(it->time).c_str());
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(pal.warn, "❓ %s", util::firstLine(it->question, 200).c_str());
                ImGui::TextUnformatted(("→ " + util::truncateUtf8(it->answer, 400)).c_str());
                ImGui::PopTextWrapPos();
                ImGui::Spacing();
            }
        }
    }

    // --- Journal ---
    ImGui::SeparatorText("Journal");
    ImGui::BeginChild("##orchlog", ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 8)),
                      ImGuiChildFlags_Borders);
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
}

// Statut d'un agent de l'équipe, d'après sa tâche du cycle et sa conversation.
App::AgentView App::agentView(const std::string& name) {
    const auto& pal = ui::palette();
    AgentView v;
    v.name = name;
    if (!orch_) return v;
    auto& o = *orch_;
    AppOrchHost host(*this);
    auto it = o.agentConv.find(name);
    v.convId = it != o.agentConv.end() ? it->second : std::string();
    // La tâche en cours prime ; sinon la plus récente (en file, ou terminée).
    for (auto& t : o.tasks)
        if (t.agent == name && !(v.task && v.task->sent && !v.task->done)) v.task = &t;
    claude::ParticipantStatus st = v.convId.empty() ? claude::ParticipantStatus{} : host.status(v.convId);
    v.status = st;
    if (v.task && v.task->sent && !v.task->done) {
        if (st.needsHuman) v.label = "Vous attend", v.icon = "⚠", v.color = pal.warn, v.attention = true;
        else if (st.busy) v.label = "Au travail", v.icon = "●", v.color = pal.accent, v.working = true;
        else v.label = "Interrompu (repartira à la reprise)", v.icon = "⏸", v.color = pal.warn;
    } else if (v.task && !v.task->done) {
        v.label = "En file", v.icon = "◌", v.color = pal.link;
    } else if (v.task && v.task->failed) {
        v.label = "Échec", v.icon = "✗", v.color = pal.err;
    } else if (v.task) {
        v.label = "Terminé", v.icon = "✓", v.color = pal.ok;
    } else if (st.busy) {
        v.label = "Occupé (hors orchestration)", v.icon = "●", v.color = pal.link;
    } else {
        v.label = "Au repos", v.icon = "·", v.color = pal.dim;
    }
    return v;
}

// L'équipe : une carte par agent (statut, modèle, tâche du cycle, dernière action).
void App::drawOrchestratorTeam(bool compact) {
    if (!orch_) return;
    ws::Folder* proj = orchProjectFolder();
    if (!proj) return;
    const auto& pal = ui::palette();
    auto& o = *orch_;
    AppOrchHost host(*this);
    float fs = ImGui::GetFontSize();
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float minW = compact ? fs * 13.5f : fs * 16.0f;
    int cols = std::max(1, (int)((avail + gap) / (minW + gap)));
    float cw = (avail - gap * (cols - 1)) / cols;
    float lh = ImGui::GetTextLineHeightWithSpacing();
    float ch = lh * (compact ? 4.6f : 6.2f) + ImGui::GetStyle().WindowPadding.y * 2;
    int k = 0;
    for (auto& chf : proj->children) {
        AgentView v = agentView(chf->name);
        if (k % cols) ImGui::SameLine(0, gap);
        ImGui::PushID(k++);
        float glow = (v.attention || v.working) && settings_.animations ? ui::anim::pulse(v.attention ? 1.1f : 0.6f, (float)k) : 0.0f;
        if (glow > 0) ui::anim::markBusy();
        ImVec4 border = v.color;
        border.w = v.attention || v.working ? 0.55f + 0.45f * glow : 0.35f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.cardBg);
        ImGui::PushStyleColor(ImGuiCol_Border, border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, v.attention || v.working ? 2.0f : 1.0f);
        ImGui::BeginChild("##agent", ImVec2(cw, ch), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        // Nom + ouvrir
        ImGui::PushFont(fontBold, 0.0f);
        std::string name = chf->name;
        float nameW = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Ouvrir").x - ImGui::GetStyle().FramePadding.x * 2 - 8;
        while (name.size() > 4 && ImGui::CalcTextSize(name.c_str()).x > nameW) name = util::truncateUtf8(name, name.size() - 4);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopFont();
        if (ImGui::IsItemHovered() && !chf->notes.empty()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(fs * 28);
            ImGui::TextUnformatted(agentBrief(chf->notes).c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Ouvrir").x -
                                                               ImGui::GetStyle().FramePadding.x * 2));
        if (ImGui::SmallButton("Ouvrir")) {
            std::string id = v.convId.empty() ? host.openAgent(chf->name, {}) : v.convId;
            if (host.ensureOpen(id)) focusConversation(indexOfConversation(id));
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Afficher la conversation de cet agent dans le cockpit");
        // Statut
        if (v.working) {
            ui::spinner((float)now_, v.color);
            ImGui::SameLine();
        } else {
            ImGui::TextColored(v.color, "%s", v.icon);
            ImGui::SameLine();
        }
        std::string stl = v.label;
        if (v.working && v.task && v.task->sentAt > 0) stl += " · " + util::humanDuration(util::nowUnix() - v.task->sentAt);
        ImGui::TextColored(v.color, "%s", stl.c_str());
        if (v.status.context > 0) {
            ImGui::SameLine();
            bool heavy = v.status.context >= o.settings.rotateAtContext && o.settings.contextRotation;
            ImGui::TextColored(heavy ? pal.warn : pal.dim, "· contexte %d %%", (int)std::lround(v.status.context * 100));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Occupation de la fenêtre de contexte de sa conversation.\n"
                                  "Au-delà de %d %%, la prochaine tâche part dans une conversation neuve,\n"
                                  "avec le récap de celle-ci (moins de contexte relu = moins de quota).",
                                  (int)std::lround(o.settings.rotateAtContext * 100));
        }
        if (v.task && (v.task->sent || v.task->done) && (!v.task->model.empty() || !v.task->effort.empty())) {
            ImGui::SameLine();
            std::string m = (v.task->model.empty() ? std::string("défaut") : v.task->model) + (v.task->effort.empty() ? "" : " · " + v.task->effort);
            ImGui::TextDisabled("(%s)", m.c_str());
        }
        // Tâche du cycle
        ImGui::PushTextWrapPos(0);
        if (v.task) {
            std::string t = util::firstLine(v.task->task, compact ? 110 : 200);
            ImGui::TextUnformatted(t.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::PushTextWrapPos(fs * 35);
                ImGui::TextUnformatted(util::truncateUtf8(v.task->task, 1500).c_str());
                if (v.task->done && !v.task->report.empty()) {
                    ImGui::Separator();
                    ImGui::TextUnformatted(util::truncateUtf8(v.task->report, 1500).c_str());
                }
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
        } else {
            ImGui::TextDisabled("Pas de tâche dans ce cycle.");
        }
        ImGui::PopTextWrapPos();
        // Dernière action de l'agent
        if (!compact && !v.convId.empty())
            for (auto& oc : open_)
                if (oc->session->localId == v.convId) {
                    auto& es = oc->session->entries;
                    for (auto it = es.rbegin(); it != es.rend(); ++it) {
                        if (it->kind == claude::EntryKind::User) break;
                        if (it->kind == claude::EntryKind::ToolUse && it->parentToolUseId.empty()) {
                            ImGui::TextColored(pal.dim, "› %s", util::truncateUtf8(it->text, 70).c_str());
                            break;
                        }
                    }
                }
        ImGui::EndChild();
        ImGui::PopID();
    }
    (void)o;
}

// La discussion du cerveau, intégrée à la page (et nulle part ailleurs).
void App::drawOrchestratorDiscussion() {
    const auto& pal = ui::palette();
    if (!orch_ || orch_->brainConvId.empty()) {
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
        ImGui::TextColored(pal.dim, "La discussion avec l'orchestrateur apparaîtra ici dès le démarrage.");
        ImGui::TextWrapped("Vous y verrez son raisonnement, les tâches qu'il confie à l'équipe, ses questions et son bilan. "
                           "Vous pourrez aussi lui écrire directement : précision, changement de cap, priorité…");
        return;
    }
    AppOrchHost host(*this);
    host.ensureOpen(orch_->brainConvId);
    OpenConversation* oc = host.find(orch_->brainConvId);
    if (!oc) {
        ImGui::TextColored(pal.warn, "Conversation de l'orchestrateur introuvable.");
        return;
    }
    ImGui::PushID(oc->session->localId.c_str());
    drawConversation(*oc);
    ImGui::PopID();
}

void App::drawOrchestratorPage() {
    const auto& pal = ui::palette();
    float fs = ImGui::GetFontSize();
    AppOrchHost host(*this);

    // Projet par défaut : celui sélectionné dans l'arbre, sinon le dernier orchestré.
    if (!orch_) {
        ws::Folder* sel = workspace_.findFolderByPath(util::pathFromUtf8(selectedFolder_));
        if (sel) sel = sel->project();
        if (sel && sel->name != "Divers") loadOrchestrator(sel);
        else autoLoadOrchestrator();
    }

    // --- En-tête ---
    ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.45f);
    ImGui::TextColored(pal.accent, "Orchestrateur");
    ImGui::PopFont();
    ImGui::SameLine(0, fs);
    ws::Folder* proj = orchProjectFolder();
    bool locked = orch_ && orch_->running();
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(fs * 16);
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
    ui::helpMarker("L'orchestrateur est un chef de projet automatique. Chaque thème (dossier) du projet est un agent\n"
                   "de son équipe ; il examine le dépôt, confie des tâches aux agents (plusieurs en parallèle, avec le\n"
                   "modèle adapté), lit leurs comptes rendus, vérifie, et recommence jusqu'à atteindre l'objectif.\n"
                   "Il s'arrête sur une question pour vous, le budget, le nombre de cycles ou les limites d'usage.");
    {
        const char* back = "Tableau de bord";
        float bw = ImGui::CalcTextSize(back).x + ImGui::CalcTextSize("Cockpit").x + ImGui::GetStyle().FramePadding.x * 4 +
                   ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - bw));
        if (ImGui::Button(back)) page_ = dash_ ? 0 : 1;
        ImGui::SameLine();
        if (ImGui::Button("Cockpit")) page_ = 0;
    }

    if (!orch_ || !proj) {
        ImGui::Spacing();
        ImGui::TextWrapped("Choisissez un projet du mode cowork dont les thèmes sont des agents (ex. « E7 Twin »). "
                           "L'orchestrateur les fera travailler ensemble jusqu'à l'objectif.");
        return;
    }
    if (proj->children.empty())
        ImGui::TextColored(pal.warn, "Ce projet n'a aucun thème : créez un dossier par agent (avec son rôle dans les notes).");

    drawOrchestratorStatus();
    if (orch_->state == claude::OrchState::WaitingHuman) drawOrchestratorQuestion();
    else if (orch_->brainPaused()) drawOrchestratorPauseNote();
    ImGui::Separator();

    // --- Corps : panneau de gauche (équipe / réglages / journal) + discussion à droite ---
    ImVec2 avail = ImGui::GetContentRegionAvail();
    bool wide = avail.x > fs * 62;
    bool showDisc = orchShowDiscussion_;
    orchShowDiscussion_ = false;
    bool showBilan = orchShowBilan_;
    orchShowBilan_ = false;
    auto tabFlags = [&](bool sel) { return sel ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None; };
    bool wantSettings = orchSettingsTab_ >= 0;
    auto leftTabs = [&](bool withDiscussion) {
        if (!ImGui::BeginTabBar("##orchpage")) return;
        if (!orch_->finalSummary.empty() &&
            ImGui::BeginTabItem("Bilan ✓", nullptr, tabFlags(showBilan))) {
            ImGui::BeginChild("##bilanscroll");
            ui::MdStyle md{fontUi, fontBold, fontMono, settings_.wrapCode, fontItalic};
            ui::renderMarkdown(ui::parseMarkdown(orch_->finalSummary), md, 889000);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        std::string team = "Équipe (" + std::to_string(proj->children.size()) + ")";
        if (ImGui::BeginTabItem(team.c_str(), nullptr, tabFlags(false))) {
            ImGui::BeginChild("##teamscroll");
            drawOrchestratorTeam(false);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (withDiscussion && ImGui::BeginTabItem("Discussion", nullptr, tabFlags(showDisc))) {
            drawOrchestratorDiscussion();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Réglages", nullptr, tabFlags(wantSettings))) {
            ImGui::BeginChild("##settingsscroll");
            drawOrchestratorSettings();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        std::string journal = "Journal (" + std::to_string(orch_->log.size()) + ")";
        if (ImGui::BeginTabItem((journal + "###journal").c_str())) {
            drawOrchestratorLog();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    };
    if (wide) {
        float leftW = std::clamp(avail.x * 0.42f, fs * 26, fs * 48);
        ImGui::BeginChild("##orchleft", ImVec2(leftW, 0));
        leftTabs(false);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(pal.accent.x, pal.accent.y, pal.accent.z, 0.45f));
        ImGui::BeginChild("##orchright", ImVec2(0, 0), ImGuiChildFlags_Borders);
        ImGui::PopStyleColor();
        ImGui::PushFont(fontBold, 0.0f);
        ImGui::TextColored(pal.accent, "Discussion avec l'orchestrateur");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("— son raisonnement, ses tâches pour l'équipe, ses questions ; vous pouvez lui écrire.");
        drawOrchestratorDiscussion();
        ImGui::EndChild();
    } else {
        leftTabs(true);
    }
}

}  // namespace app
