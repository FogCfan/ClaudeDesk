// Connexion au compte Claude : détection des erreurs d'authentification et état `claude auth status`.
#include <doctest/doctest.h>

#include "claude/Auth.h"
#include "claude/ClaudeSession.h"

using namespace claude;

namespace {
const char* kExpired =
    "Failed to authenticate: this login reached its time limit and cannot be renewed. Run `claude auth login` to sign "
    "in again, or use `claude setup-token` and set CLAUDE_CODE_OAUTH_TOKEN for unattended jobs";
}

TEST_CASE("auth : messages d'erreur reconnus") {
    CHECK(isAuthErrorText(kExpired));
    CHECK(isAuthErrorText("Invalid API key · Fix external API key"));
    CHECK(isAuthErrorText("Not logged in · Please run /login"));
    CHECK(isAuthErrorText("OAuth token has expired. Please obtain a new token or refresh your existing token."));
    CHECK_FALSE(isAuthErrorText("Budget maximal atteint pour cette session."));
    CHECK_FALSE(isAuthErrorText("Tour interrompu."));
    CHECK_FALSE(isAuthErrorText("API Error: 529 Overloaded"));
}

TEST_CASE("auth : sortie de claude auth status") {
    AuthStatus st = parseAuthStatus(R"({
  "loggedIn": true,
  "authMethod": "claude.ai",
  "apiProvider": "firstParty",
  "loginExpiresAt": "2026-12-21T16:58:58.153Z",
  "email": "moi@exemple.fr",
  "orgName": "Org",
  "subscriptionType": "pro"
})");
    CHECK(st.known);
    CHECK(st.loggedIn);
    CHECK(st.email == "moi@exemple.fr");
    CHECK(st.planLabel() == "Claude Pro");
    CHECK_FALSE(st.apiKey());
    REQUIRE(st.expiresAt > 0);
    CHECK(st.usable(st.expiresAt - 10));
    CHECK_FALSE(st.usable(st.expiresAt + 10));  // connexion expirée : « connecté » mais inutilisable
    CHECK(st.expired(st.expiresAt + 10));

    AuthStatus off = parseAuthStatus("{\"loggedIn\": false}\n");
    CHECK(off.known);
    CHECK_FALSE(off.usable(0));

    AuthStatus junk = parseAuthStatus("error: unknown command 'auth'");
    CHECK_FALSE(junk.known);
    CHECK_FALSE(junk.error.empty());
}

TEST_CASE("auth : tour refusé faute de connexion (lignes réelles du CLI)") {
    ClaudeSession s;
    s.state = SessionState::Busy;
    int authErrors = 0, finished = 0;
    s.onAuthError = [&](ClaudeSession&) { ++authErrors; };
    s.onTurnFinished = [&](ClaudeSession&) { ++finished; };
    s.handleLine(R"({"type":"system","subtype":"init","session_id":"sid-a","tools":[],"model":"claude-haiku-5-5"})");
    s.handleLine(R"({"type":"assistant","message":{"id":"7ee6","model":"<synthetic>","role":"assistant","type":"message","content":[{"type":"text","text":"Invalid API key · Fix external API key"}]},"parent_tool_use_id":null,"session_id":"sid-a","error":"authentication_failed"})");
    CHECK(s.entries.empty());  // le message assistant fabriqué n'est pas affiché comme une réponse
    s.handleLine(R"({"type":"result","subtype":"success","is_error":true,"duration_ms":10,"duration_api_ms":0,"total_cost_usd":0,"result":"Invalid API key · Fix external API key","session_id":"sid-a","usage":{"input_tokens":0,"output_tokens":0}})");
    REQUIRE(s.entries.size() == 1);
    CHECK(s.entries[0].kind == EntryKind::Error);
    CHECK(isAuthErrorText(s.entries[0].text));
    CHECK(s.authFailed);
    CHECK(authErrors == 1);
    CHECK(finished == 1);
    CHECK(s.state == SessionState::Idle);
    CHECK(s.stats.turns == 0);  // pas une réponse : l'orchestrateur ne la prend pas pour le travail d'un agent

    // Tour suivant réussi : l'échec est oublié.
    s.state = SessionState::Busy;
    s.handleLine(R"({"type":"assistant","message":{"id":"msg_2","model":"claude-haiku-5-5","role":"assistant","content":[{"type":"text","text":"Bonjour"}]},"parent_tool_use_id":null})");
    s.handleLine(R"({"type":"result","subtype":"success","is_error":false,"duration_ms":10,"total_cost_usd":0.001,"session_id":"sid-a","usage":{"input_tokens":2,"output_tokens":3}})");
    CHECK_FALSE(s.authFailed);
    CHECK(s.stats.turns == 1);
    CHECK(authErrors == 1);
}

TEST_CASE("auth : erreur reconnue au texte du résultat seul") {
    ClaudeSession s;
    s.state = SessionState::Busy;
    s.handleLine(std::string(R"({"type":"result","subtype":"success","is_error":true,"result":")") + kExpired +
                 R"(","session_id":"sid-b","usage":{}})");
    CHECK(s.authFailed);
    CHECK(s.stats.turns == 0);
    // Une erreur ordinaire n'est pas un problème de connexion.
    ClaudeSession t;
    t.state = SessionState::Busy;
    t.handleLine(R"({"type":"result","subtype":"error_max_budget_usd","is_error":true,"session_id":"sid-c","usage":{}})");
    CHECK_FALSE(t.authFailed);
    CHECK(t.stats.turns == 1);
}
