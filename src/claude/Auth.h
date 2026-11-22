#pragma once
// Connexion du CLI à un compte Claude : détection des erreurs d'authentification,
// état (`claude auth status --json`, commande locale et gratuite) et lancement de
// `claude auth login` dans une fenêtre de terminal (TerminalProcess, Process.h).
#include <cstdint>
#include <memory>
#include <string>

#include "claude/Process.h"

namespace claude {

// Message d'erreur du CLI qui signifie « pas (ou plus) connecté » : connexion expirée ou révoquée,
// clé API invalide, /login demandé… (ex. « Failed to authenticate: this login reached its time limit… »).
bool isAuthErrorText(const std::string& text);

struct AuthStatus {
    bool known = false;        // l'état a pu être lu
    bool loggedIn = false;
    std::string method;        // « claude.ai », « api_key », « oauth_token »…
    std::string email, orgName, plan;
    int64_t expiresAt = 0;     // fin de validité de la connexion (secondes Unix ; 0 = inconnue)
    std::string error;         // known == false : pourquoi
    bool expired(int64_t now) const { return expiresAt > 0 && now >= expiresAt; }
    bool usable(int64_t now) const { return known && loggedIn && !expired(now); }
    bool apiKey() const { return method == "api_key"; }
    std::string planLabel() const;  // « Claude Pro », « Claude Max »… ; "" si inconnu
};

// Analyse la sortie de `claude auth status --json`.
AuthStatus parseAuthStatus(const std::string& output);

// `claude auth status --json` en arrière-plan.
class AuthChecker {
public:
    bool start(const std::string& exe);
    // true une fois terminé (le résultat est alors dans status()).
    bool update();
    bool finished() const { return finished_; }
    const AuthStatus& status() const { return status_; }

private:
    std::unique_ptr<Process> process_;
    std::string out_, err_;
    bool finished_ = false;
    AuthStatus status_;
};

}  // namespace claude
