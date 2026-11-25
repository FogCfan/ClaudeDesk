// Signaux d'attention : quand une section change d'état alors qu'on ne la regarde pas (l'orchestrateur pose une
// question, un agent ouvre une conversation, le Designer a fini…), son bouton de page ou son onglet clignote
// quelques fois, puis reste teinté jusqu'à ce qu'on l'ouvre (signal « durable ») ou s'efface seul.
// Économie d'énergie : seul le clignotement initial (2,4 s) et le fondu demandent des images en continu ;
// la teinte durable respire lentement au rythme de la boucle au repos, sans la réveiller.
#include <algorithm>
#include <cmath>
#include <set>

#include "app/App.h"
#include "ui/Anim.h"
#include "ui/Theme.h"

namespace app {

namespace {
constexpr double kBlinkS = 2.4;  // phase de clignotement franc (3 battements)
constexpr double kFadeS = 4.0;   // signal passager : durée totale avant effacement
constexpr double kStartupS = 4.0;  // au lancement, les conversations rouvertes ne sont pas « nouvelles »
}  // namespace

void App::flash(const std::string& key, const ImVec4& color, bool sticky) {
    auto it = flashes_.find(key);
    // Même signal déjà en cours (même couleur) : on ne relance pas le clignotement à chaque image.
    if (it != flashes_.end() && it->second.color.x == color.x && it->second.color.y == color.y &&
        it->second.color.z == color.z && (it->second.sticky || !sticky))
        return;
    flashes_[key] = {now_, color, sticky};
}

void App::clearFlash(const std::string& key) { flashes_.erase(key); }

ImVec4 App::flashColor(const std::string& key) {
    auto it = flashes_.find(key);
    if (it == flashes_.end()) return ImVec4(0, 0, 0, 0);
    const Flash& f = it->second;
    const double t = now_ - f.start;
    float a;
    if (!ui::anim::enabled()) {
        a = 0.45f;  // animations désactivées : teinte fixe
    } else if (t < kBlinkS) {
        a = 0.15f + 0.6f * (float)(0.5 + 0.5 * std::cos(t * 2.0 * 3.14159265 * 1.25));  // part de la crête
        ui::anim::markBusy();
    } else if (f.sticky) {
        a = 0.28f + 0.12f * (float)std::cos((t - kBlinkS) * 2.0 * 3.14159265 * 0.4);
    } else if (t < kFadeS) {
        a = 0.35f * (float)(1.0 - (t - kBlinkS) / (kFadeS - kBlinkS));
        ui::anim::markBusy();
    } else {
        flashes_.erase(it);
        return ImVec4(0, 0, 0, 0);
    }
    return ImVec4(f.color.x, f.color.y, f.color.z, a);
}

void App::tickAttention() {
    const auto& pal = ui::palette();
    const bool startup = now_ < kStartupS;

    // Ce qu'on regarde n'a plus besoin de signal.
    clearFlash("page" + std::to_string(page_));
    if (dash_) clearFlash("page1");
    if (page_ == 0)
        if (OpenConversation* oc = active()) clearFlash("tab:" + oc->session->localId);

    // --- Orchestrateur ---
    int st = orch_ ? (int)orch_->state : -1;
    if (attnInit_ && st != prevOrchState_ && orch_ && page_ != 2) {
        using claude::OrchState;
        switch (orch_->state) {
            case OrchState::WaitingHuman: flash("page2", pal.warn, true); break;      // question pour vous
            case OrchState::Finished: flash("page2", pal.ok, true); break;            // bilan prêt
            case OrchState::UsagePause:
            case OrchState::Paused: flash("page2", pal.warn, false); break;          // arrêt inattendu
            default: break;
        }
    }
    if (orch_ && orch_->state != claude::OrchState::WaitingHuman && flashes_.count("page2") &&
        flashes_["page2"].color.x == pal.warn.x && flashes_["page2"].sticky)
        clearFlash("page2");  // question répondue ailleurs (carte du tableau de bord…)
    prevOrchState_ = st;

    // --- Designer : conception terminée ---
    bool dBusy = designerBusy();
    if (attnInit_ && prevDesignerBusy_ && !dBusy && page_ != 3) flash("page3", pal.ok, true);
    prevDesignerBusy_ = dBusy;

    // --- Conversations ---
    std::set<std::string> present;
    for (int i = 0; i < (int)open_.size(); ++i) {
        auto& oc = *open_[i];
        const auto& s = *oc.session;
        const std::string& id = s.localId;
        if (isOrchBrain(id)) continue;
        present.insert(id);
        const std::string key = "tab:" + id;
        const bool viewed = page_ == 0 && i == active_;
        const bool busy = s.state == claude::SessionState::Busy;
        auto it = attnConvs_.find(id);
        if (it == attnConvs_.end()) {
            // Ouverte en arrière-plan (par l'orchestrateur : nouvel agent, rotation de contexte) : on la signale.
            if (attnInit_ && !startup && !oc.hiddenTab && !viewed && selectTab_ != i) {
                flash(key, pal.accent, true);
                if (page_ != 0) flash("page0", pal.accent, false);
            }
            attnConvs_[id] = {s.permissions.size(), busy};
            continue;
        }
        AttnConv& prev = it->second;
        if (!oc.hiddenTab && !viewed) {
            if (s.permissions.size() > prev.perms) {  // attend une décision
                flash(key, pal.warn, true);
                if (page_ != 0 && page_ != 1 && !dash_) flash("page1", pal.warn, false);
            } else if (prev.busy && !busy && s.permissions.empty()) {
                flash(key, pal.ok, false);  // réponse arrivée
            }
        }
        if (s.permissions.empty() && flashes_.count(key) && flashes_[key].color.x == pal.warn.x)
            clearFlash(key);  // décision prise ailleurs (tableau de bord)
        prev = {s.permissions.size(), busy};
    }
    for (auto it = attnConvs_.begin(); it != attnConvs_.end();)
        if (!present.count(it->first)) {
            clearFlash("tab:" + it->first);
            it = attnConvs_.erase(it);
        } else {
            ++it;
        }
    attnInit_ = true;
}

}  // namespace app
