// Connexion au compte Claude : état (`claude auth status`), fenêtre qui accompagne la (re)connexion
// (`claude auth login` dans une fenêtre de terminal), reprise des messages refusés faute de connexion.
#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "app/App.h"
#include "ui/Anim.h"
#include "ui/Theme.h"
#include "util/Strings.h"

namespace app {

namespace {
constexpr double kLoginPollS = 2.0;            // pendant une connexion : vérification toutes les 2 s
constexpr double kPeriodicCheckS = 6 * 3600.0; // sinon : de temps en temps (expiration à venir)
constexpr int64_t kExpirySoonS = 3 * 86400;    // « expire bientôt » : moins de 3 jours

bool mentionsTimeLimit(const std::string& s) {
    return util::containsInsensitive(s, "time limit") || util::containsInsensitive(s, "expired") ||
           util::containsInsensitive(s, "expir");
}
}  // namespace

void App::checkAuth() {
    if (authCheck_ || claudeExe_.empty()) return;
    authCheck_ = std::make_unique<claude::AuthChecker>();
    authCheck_->start(claudeExe_);
    authLastCheck_ = now_;
}

void App::openLogin(const std::string& reason) {
    if (!reason.empty()) authReason_ = reason;
    if (!showLogin_) focusLogin_ = true;
    showLogin_ = true;
    if (loginStartedAt_ < 0) loginJustSucceeded_ = false;
    checkAuth();
}

void App::startLogin(bool console) {
    loginError_.clear();
    loginJustSucceeded_ = false;
    loginTermExited_ = false;
    int64_t now = util::nowUnix();
    loginBaseline_ = auth_.usable(now) ? auth_.expiresAt : -1;
    loginBaselineEmail_ = auth_.email;
    std::vector<std::string> args = {"auth", "login", console ? "--console" : "--claudeai"};
    if (!console && !auth_.email.empty()) args.insert(args.end(), {"--email", auth_.email});
    loginTerm_ = std::make_unique<claude::TerminalProcess>();
    if (!loginTerm_->start(claudeExe_, args, "Connexion à Claude")) {
        loginError_ = loginTerm_->error();
        loginTerm_.reset();
        return;
    }
    loginStartedAt_ = now_;
}

bool App::orchManages(const std::string& convId) const {
    if (!orch_ || !(orch_->running() || orch_->state == claude::OrchState::Paused)) return false;
    if (orch_->brainConvId == convId) return true;
    for (auto& t : orch_->tasks)
        if (t.convId == convId && !t.done) return true;
    return false;
}

std::vector<OpenConversation*> App::authFailedConversations(bool excludeOrch) {
    std::vector<OpenConversation*> out;
    for (auto& oc : open_)
        if (oc->session->authFailed && !(excludeOrch && orchManages(oc->session->localId))) out.push_back(oc.get());
    return out;
}

int App::resendAuthFailed() {
    int n = 0;
    for (OpenConversation* oc : authFailedConversations(true))
        if (oc->session->resendLast()) ++n;
    return n;
}

void App::onAuthError(claude::ClaudeSession& s) {
    for (auto& oc : open_)
        if (oc->session.get() == &s) pushActivity(*oc, ActivityKind::Error, "connexion à Claude refusée");
    std::string reason;
    for (auto it = s.entries.rbegin(); it != s.entries.rend(); ++it)
        if (it->kind == claude::EntryKind::Error) {
            reason = it->text;
            break;
        }
    authProblem_ = true;
    // L'orchestrateur s'arrête net : sinon chaque agent échouerait à son tour. Il reprendra après la connexion.
    if (orch_ && orch_->running()) {
        orch_->pause("connexion à Claude expirée ou refusée : reconnectez-vous (fenêtre « Connexion à Claude »)");
        orchPausedForAuth_ = true;
        saveOrchestrator();
    }
    if (loginStartedAt_ < 0) openLogin(reason);
}

void App::onLoggedIn() {
    authProblem_ = false;
    loginJustSucceeded_ = true;
    loginStartedAt_ = -1;
    loginTerm_.reset();
    toast("Connecté à Claude" + (auth_.email.empty() ? std::string() : " : " + auth_.email));
    // Les processus déjà lancés peuvent garder l'ancienne connexion en mémoire : ils repartiront (--resume)
    // au prochain message.
    for (auto& oc : open_)
        if (oc->session->state == claude::SessionState::Idle) oc->session->stop();
    if (orchPausedForAuth_ && orch_ && orch_->state == claude::OrchState::Paused) {
        orchestratorResume();
        toast("L'orchestrateur reprend son travail.");
    }
    orchPausedForAuth_ = false;
}

void App::tickAuth() {
    if (authCheck_ && authCheck_->update()) {
        const claude::AuthStatus st = authCheck_->status();
        authCheck_.reset();
        bool wasKnown = auth_.known;
        auth_ = st;
        int64_t now = util::nowUnix();
        if (loginStartedAt_ >= 0 && optPanel_ != "login:attente") {
            bool fresh = auth_.expiresAt != loginBaseline_ || auth_.email != loginBaselineEmail_ ||
                         (loginTermExited_ && loginTerm_ && loginTerm_->tracked() && loginTerm_->exitCode() == 0);
            if (auth_.usable(now) && fresh) onLoggedIn();
        } else if (!wasKnown && auth_.known && !auth_.usable(now) && optScreenshot_.empty()) {
            // Au lancement : pas (ou plus) connecté → on le dit tout de suite, avant le premier message.
            openLogin(auth_.expired(now) ? "Votre connexion à Claude a expiré." : "");
        } else if (!wasKnown && auth_.usable(now) && auth_.expiresAt > 0 && auth_.expiresAt - now < kExpirySoonS) {
            toast("Votre connexion à Claude expire " + util::humanWhen(auth_.expiresAt) +
                  " : Aide › Compte Claude pour la renouveler.");
        }
    }
    if (loginStartedAt_ >= 0) {
        if (loginTerm_ && loginTerm_->tracked() && !loginTermExited_ && !loginTerm_->running()) {
            loginTermExited_ = true;
            checkAuth();  // dernière vérification : la connexion vient peut-être d'aboutir
        }
        if (!authCheck_ && now_ - authLastCheck_ >= kLoginPollS) checkAuth();
    } else if (!authCheck_ && now_ - authLastCheck_ >= kPeriodicCheckS) {
        checkAuth();
    }
}

void App::drawAuthStatusBadge() {
    const auto& pal = ui::palette();
    int64_t now = util::nowUnix();
    std::string label;
    ImVec4 c = pal.err;
    if (loginStartedAt_ >= 0) {
        label = "Connexion à Claude en cours…";
        c = pal.accent;
    } else if (authProblem_ || (auth_.known && !auth_.usable(now))) {
        label = "⚠ Non connecté à Claude — cliquez pour vous connecter";
        c.w *= 0.65f + 0.35f * (float)std::sin(now_ * 3.0);
        ui::anim::markBusy();
    } else if (auth_.usable(now) && auth_.expiresAt > 0 && auth_.expiresAt - now < kExpirySoonS) {
        label = "Connexion Claude : expire dans " + util::humanDuration(auth_.expiresAt - now);
        c = pal.warn;
    }
    if (label.empty()) return;
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, c);
    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(label.c_str())))
        openLogin();
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ouvre la fenêtre « Connexion à Claude »");
}

void App::drawLoginWindow() {
    if (!showLogin_) return;
    const auto& pal = ui::palette();
    const float em = ImGui::GetFontSize();
    const int64_t now = util::nowUnix();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.42f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(em * 30, vp->WorkSize.x - 16), 0),
                                        ImVec2(std::min(em * 40, vp->WorkSize.x - 16), vp->WorkSize.y - 16));
    if (focusLogin_) {
        ImGui::SetNextWindowFocus();
        focusLogin_ = false;
    }
    if (!ImGui::Begin("Connexion à Claude", &showLogin_,
                      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    auto title = [&](const char* text, const ImVec4& color) {
        ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.25f);
        ImGui::TextColored(color, "%s", text);
        ImGui::PopFont();
        ImGui::Spacing();
    };
    auto para = [&](const std::string& text, bool dim = false) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + em * 36);
        if (dim) ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
        ImGui::TextUnformatted(text.c_str());
        if (dim) ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
    };
    auto step = [&](int n, bool done, const std::string& text) {
        ImGui::TextColored(done ? pal.ok : pal.accent, done ? " ✓ " : " %d.", n);
        ImGui::SameLine();
        para(text, done);
    };
    auto accountLine = [&] {
        std::string who = auth_.email.empty() ? std::string("compte Claude") : auth_.email;
        std::string plan = auth_.planLabel();
        if (auth_.apiKey()) plan = "clé API";
        ImGui::TextColored(pal.ok, "✓ Connecté : %s", who.c_str());
        if (!plan.empty()) {
            ImGui::SameLine();
            ui::badge(plan.c_str(), pal.accent);
        }
        if (auth_.expiresAt > 0)
            ImGui::TextDisabled("Connexion valable jusqu'à %s (dans %s).", util::humanWhen(auth_.expiresAt).c_str(),
                                util::humanDuration(std::max<int64_t>(0, auth_.expiresAt - now)).c_str());
    };
    auto primary = [&](const char* label) {
        ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(pal.accent.x * 1.1f, pal.accent.y * 1.1f, pal.accent.z * 1.1f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        bool r = ImGui::Button(label, ImVec2(0, em * 2.0f));
        ImGui::PopStyleColor(3);
        return r;
    };

    if (claudeExe_.empty()) {
        title("Claude Code est introuvable", pal.err);
        para("ClaudeDesk pilote le programme Claude Code, qui n'a pas été trouvé sur cet ordinateur. Installez-le "
             "(https://claude.com/claude-code), ou indiquez son chemin dans Configuration › Application, puis relancez "
             "ClaudeDesk.");
    } else if (loginJustSucceeded_) {
        // --- Connexion réussie ---
        title("Vous êtes connecté", pal.ok);
        accountLine();
        ImGui::Spacing();
        auto failed = authFailedConversations(true);
        if (!failed.empty()) {
            para(std::to_string(failed.size()) + (failed.size() > 1 ? " messages n'ont pas pu être traités" : " message n'a pas pu être traité") +
                 " pendant la coupure :");
            for (auto* oc : failed) ImGui::BulletText("%s", util::truncateUtf8(oc->session->title, 70).c_str());
            ImGui::Spacing();
            std::string lbl = failed.size() > 1 ? "Les renvoyer (" + std::to_string(failed.size()) + ")" : std::string("Le renvoyer");
            if (primary(lbl.c_str())) {
                int n = resendAuthFailed();
                toast(std::to_string(n) + (n > 1 ? " messages renvoyés." : " message renvoyé."));
            }
            ImGui::SameLine();
        }
        if (orch_ && orch_->running()) para("L'orchestrateur a repris là où il s'était arrêté.", true);
        if (ImGui::Button("Fermer", ImVec2(0, em * 2.0f))) showLogin_ = false;
    } else if (loginStartedAt_ >= 0) {
        // --- Connexion en cours : on accompagne pas à pas ---
        title("Connexion en cours…", pal.accent);
#ifdef __APPLE__
        step(1, true, "Le Terminal s'est ouvert avec la commande de connexion.");
#else
        step(1, true, "Une fenêtre de terminal s'est ouverte avec la commande de connexion.");
#endif
        step(2, false, "Votre navigateur affiche la page de connexion de Claude : connectez-vous avec le compte de "
                       "votre abonnement, puis cliquez sur « Autoriser ».");
        step(3, false, "Si le terminal demande un code, copiez celui que le navigateur affiche, collez-le dans le "
                       "terminal (clic droit ou Ctrl+V), puis appuyez sur Entrée.");
        step(4, false, "Revenez ici : ClaudeDesk détecte la connexion tout seul, et le terminal se ferme.");
        ImGui::Spacing();
        ui::spinner((float)now_, pal.accent);
        ImGui::SameLine();
        ImGui::TextDisabled("En attente de la connexion… (%s)", util::humanDuration((int64_t)(now_ - loginStartedAt_)).c_str());
        ui::anim::markBusy();
        if (loginTermExited_ && !authCheck_) {
            ImGui::Spacing();
            ImGui::TextColored(pal.warn, "La fenêtre de connexion s'est fermée sans que la connexion aboutisse.");
            para("Vous l'avez peut-être fermée trop tôt, ou la page du navigateur a été annulée. Recommencez : "
                 "c'est sans risque.", true);
            if (primary("Réessayer")) startLogin(false);
            ImGui::SameLine();
        }
        if (ImGui::TreeNode("Le navigateur ne s'est pas ouvert ?")) {
            para("Le terminal affiche un lien qui commence par https://claude.ai/… : copiez-le (sélection puis Entrée, "
                 "ou clic droit › Copier) et ouvrez-le dans votre navigateur.", true);
            para("Le terminal ne s'est pas ouvert ? Ouvrez-en un vous-même et tapez : claude auth login", true);
            ImGui::TreePop();
        }
        if (ImGui::Button("Annuler")) {
            loginStartedAt_ = -1;
            loginTerm_.reset();
        }
        ImGui::SameLine();
        if (ImGui::Button("Vérifier maintenant")) checkAuth();
    } else if (!auth_.known && authCheck_) {
        title("Connexion à Claude", pal.accent);
        ui::spinner((float)now_, pal.accent);
        ImGui::SameLine();
        ImGui::TextDisabled("Vérification de votre connexion…");
        ui::anim::markBusy();
    } else if (!authProblem_ && auth_.usable(now)) {
        // --- Tout va bien (ouverte depuis le menu) ---
        title("Compte Claude", pal.accent);
        accountLine();
        ImGui::Spacing();
        para("Claude Code demande de se reconnecter de temps en temps (environ une fois par mois). ClaudeDesk vous "
             "préviendra quelques jours avant, dans la barre d'état.", true);
        ImGui::Spacing();
        if (ImGui::Button("Se reconnecter / changer de compte", ImVec2(0, em * 2.0f))) startLogin(false);
        ImGui::SameLine();
        if (ImGui::Button("Fermer", ImVec2(0, em * 2.0f))) showLogin_ = false;
    } else {
        // --- Pas (ou plus) connecté ---
        bool apiKey = auth_.apiKey();
        bool expired = auth_.expired(now) || mentionsTimeLimit(authReason_);
        title(apiKey ? "Votre clé API est refusée" : expired ? "Votre connexion à Claude a expiré" : "Connexion à Claude nécessaire",
              pal.warn);
        if (apiKey) {
            para("Claude Code utilise une clé API (variable d'environnement ANTHROPIC_API_KEY ou réglage apiKeyHelper) "
                 "et l'API la refuse : elle est invalide, révoquée ou sans crédit.");
            ImGui::Spacing();
            para("• Pour continuer avec cette méthode : corrigez la clé, puis relancez ClaudeDesk.\n"
                 "• Pour utiliser plutôt votre abonnement Claude (Pro, Max) : retirez la variable ANTHROPIC_API_KEY, "
                 "relancez ClaudeDesk, puis connectez-vous ici.", true);
        } else if (expired) {
            para("Par sécurité, Claude Code demande de se reconnecter régulièrement. Il suffit de vous reconnecter "
                 "une fois : cela prend moins d'une minute.");
        } else {
            para("Claude Code n'est connecté à aucun compte, ou sa connexion n'est plus valide. Connectez-vous avec "
                 "votre compte Claude pour pouvoir discuter.");
        }
        ImGui::Spacing();
        para("Rien n'est perdu : vos conversations sont intactes, et les messages refusés pourront être renvoyés en "
             "un clic une fois connecté.", true);
        if (orchPausedForAuth_) para("L'orchestrateur est en pause ; il reprendra tout seul après la connexion.", true);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted("Ce qui va se passer :");
        para("une fenêtre de terminal s'ouvre, puis votre navigateur affiche la page de connexion de Claude. "
             "Connectez-vous, autorisez l'accès, et revenez ici.", true);
        ImGui::Spacing();
        if (primary("Se connecter avec mon compte Claude")) startLogin(false);
        ImGui::SameLine();
        if (ImGui::Button("Vérifier à nouveau", ImVec2(0, em * 2.0f))) checkAuth();
        if (!loginError_.empty()) ImGui::TextColored(pal.err, "%s", loginError_.c_str());
        ImGui::Spacing();
        if (ImGui::TreeNode("Autres méthodes de connexion")) {
            para("Compte API (facturation à l'usage sur console.anthropic.com) :", true);
            if (ImGui::SmallButton("Se connecter avec un compte Console Anthropic")) startLogin(true);
            ImGui::Spacing();
            para("Tâches sans surveillance (orchestrations de nuit…) : dans un terminal, « claude setup-token » crée un "
                 "jeton valable un an ; placez-le dans la variable d'environnement CLAUDE_CODE_OAUTH_TOKEN puis "
                 "relancez ClaudeDesk.", true);
            ImGui::TreePop();
        }
        if (!authReason_.empty() && ImGui::TreeNode("Message d'erreur de Claude Code")) {
            para(authReason_, true);
            ImGui::TreePop();
        }
    }
    ImGui::End();
}

}  // namespace app
