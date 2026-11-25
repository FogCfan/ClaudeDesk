#include <doctest/doctest.h>

#include "claude/ClaudeSession.h"
#include "claude/Models.h"
#include "claude/Orchestrator.h"

using namespace claude;

TEST_CASE("familles de modèles et noms lisibles") {
    REQUIRE(modelFamilies().size() == 4);
    CHECK(findModelFamily("opus") == &modelFamilies()[2]);
    CHECK(findModelFamily("claude-fable-5-2") == &modelFamilies()[3]);
    CHECK(findModelFamily("claude-haiku-4-5-20251001") == &modelFamilies()[0]);
    CHECK(findModelFamily("") == nullptr);
    CHECK(findModelFamily("default") == nullptr);
    CHECK(modelDisplayName("") == "Par défaut");
    CHECK(modelDisplayName("opus") == "Opus 5.5");
    CHECK(modelDisplayName("claude-fable-5-2") == "Fable 5.2");
    CHECK(modelDisplayName("claude-opus-4-8") == "claude-opus-4-8");  // version figée : l'identifiant
}

TEST_CASE("accès aux modèles selon l'abonnement") {
    // Vérifié avec un compte Pro : Fable demande des crédits d'usage, Opus répond.
    CHECK(expectedModelAccess("fable", "pro") == ModelAccess::NeedsCredits);
    CHECK(expectedModelAccess("opus", "pro") == ModelAccess::Included);
    CHECK(expectedModelAccess("haiku", "Claude Pro") == ModelAccess::Included);
    CHECK(expectedModelAccess("opus", "max") == ModelAccess::Included);
    CHECK(expectedModelAccess("fable", "max") == ModelAccess::Unknown);  // pas vérifié : on laisse essayer
    CHECK(expectedModelAccess("sonnet", "") == ModelAccess::Unknown);

    // Messages réels de la CLI
    CHECK(modelAccessFromError("rate_limit", "You're out of usage credits. Switch to another model, or manage usage "
                                             "credits at https://claude.ai/settings/usage, to continue.") ==
          ModelAccess::NeedsCredits);
    CHECK(modelAccessFromError("model_not_found", "There's an issue with the selected model (x).") == ModelAccess::Unavailable);
    CHECK(modelAccessFromError("rate_limit", "You've hit your session limit") == ModelAccess::Unknown);
    CHECK(modelAccessFromError("", "") == ModelAccess::Unknown);

    ModelAccessBook book;
    CHECK_FALSE(book.usable("fable", "pro"));
    CHECK(book.usable("opus", "pro"));
    CHECK(book.usable("", "pro"));  // défaut
    CHECK(book.usable("fable", "max"));
    CHECK(book.blocked("pro") == std::vector<std::string>{"fable"});
    // Observation : ne vaut que pour l'abonnement où elle a été faite
    book.record("opus", "max", ModelAccess::Unavailable, 100);
    CHECK_FALSE(book.usable("opus", "max"));
    CHECK(book.usable("opus", "pro"));
    // Une réponse reçue confirme l'accès… sauf hors abonnement (crédits achetés : ça reste payant)
    book.recordSuccess("claude-opus-5-5", "max", 200);
    CHECK(book.usable("opus", "max"));
    book.recordSuccess("claude-fable-5-2", "pro", 200);
    CHECK_FALSE(book.usable("fable", "pro"));
    // Aller-retour JSON
    ModelAccessBook copy;
    copy.fromJson(book.toJson());
    CHECK(copy.access("opus", "max") == ModelAccess::Included);
    CHECK(copy.observed("opus", "max"));
}

TEST_CASE("orchestrateur : jamais de modèle hors abonnement") {
    std::vector<std::string> all = {"haiku", "sonnet", "opus", "fable"};
    auto usable = usableModels(all, {"fable"});
    CHECK(usable == std::vector<std::string>{"haiku", "sonnet", "opus"});
    CHECK(clampModel("fable", usable) == "opus");
    CHECK(clampModel("sonnet", usable) == "sonnet");
    CHECK(usableModels({}, {"fable"}) == std::vector<std::string>{"haiku", "sonnet", "opus"});
    CHECK(usableModels(all, {}) == all);
    CHECK(usableModels({"fable"}, {"fable"}) == std::vector<std::string>{"fable"});  // jamais vide
}

TEST_CASE("session : Fable refusé faute de crédits") {
    ClaudeSession s;
    s.config.model = "fable";
    int refused = 0, finished = 0;
    std::vector<std::string> answered;
    s.onModelRefused = [&](ClaudeSession&) { ++refused; };
    s.onTurnFinished = [&](ClaudeSession&) { ++finished; };
    s.onModelAnswered = [&](ClaudeSession&, const std::string& m) { answered.push_back(m); };
    s.handleLine(R"({"type":"system","subtype":"init","session_id":"sid-f","tools":[],"model":"claude-fable-5-2"})");
    s.handleLine(R"({"type":"assistant","message":{"id":"x1","model":"<synthetic>","role":"assistant","type":"message","content":[{"type":"text","text":"You're out of usage credits. Switch to another model, or manage usage credits at https://claude.ai/settings/usage?from=cc_cli_limit_message, to continue."}]},"parent_tool_use_id":null,"session_id":"sid-f","error":"rate_limit"})");
    CHECK(s.entries.empty());
    s.handleLine(R"({"type":"result","subtype":"success","is_error":true,"duration_ms":10,"duration_api_ms":0,"total_cost_usd":0,"result":"You're out of usage credits. Switch to another model, or manage usage credits at https://claude.ai/settings/usage?from=cc_cli_limit_message, to continue.","session_id":"sid-f","usage":{"input_tokens":0,"output_tokens":0}})");
    REQUIRE(s.entries.size() == 1);
    CHECK(s.entries[0].kind == EntryKind::Error);
    CHECK(isModelRefusalText(s.entries[0].text));
    CHECK(s.entries[0].text.find("Fable 5.2") != std::string::npos);
    CHECK(s.modelRefused == ModelAccess::NeedsCredits);
    CHECK(s.modelRefusedName == "fable");
    CHECK(refused == 1);
    CHECK(finished == 1);
    CHECK(s.stats.turns == 0);  // pas une réponse : l'orchestrateur renvoie la tâche avec un autre modèle
    CHECK(answered.empty());

    // Tour suivant réussi avec Opus : le refus est oublié, l'accès à Opus confirmé.
    s.config.model = "opus";
    s.handleLine(R"({"type":"assistant","message":{"id":"x2","model":"claude-opus-5-5","role":"assistant","type":"message","content":[{"type":"text","text":"OK"}],"usage":{"input_tokens":3,"output_tokens":1}},"parent_tool_use_id":null,"session_id":"sid-f"})");
    s.handleLine(R"({"type":"result","subtype":"success","is_error":false,"duration_ms":10,"duration_api_ms":5,"total_cost_usd":0.006,"result":"OK","session_id":"sid-f","usage":{"input_tokens":3,"output_tokens":1}})");
    CHECK(s.modelRefused == ModelAccess::Unknown);
    CHECK(s.stats.turns == 1);
    CHECK(answered == std::vector<std::string>{"claude-opus-5-5"});
}

TEST_CASE("session : modèle inexistant") {
    ClaudeSession s;
    s.config.model = "claude-inexistant-9";
    s.handleLine(R"({"type":"assistant","message":{"id":"y1","model":"<synthetic>","role":"assistant","type":"message","content":[{"type":"text","text":"There's an issue with the selected model (claude-inexistant-9). It may not exist or you may not have access to it. Run --model to pick a different model."}]},"session_id":"s","error":"model_not_found"})");
    s.handleLine(R"({"type":"result","subtype":"success","is_error":true,"total_cost_usd":0,"result":"There's an issue with the selected model (claude-inexistant-9). It may not exist or you may not have access to it. Run --model to pick a different model.","session_id":"s"})");
    REQUIRE(s.entries.size() == 1);
    CHECK(s.modelRefused == ModelAccess::Unavailable);
    CHECK(isModelRefusalText(s.entries[0].text));
}

TEST_CASE("limite de 5 h : ni refus de modèle ni réponse") {
    const std::string txt = "You've hit your 5-hour limit · resets 8:30am (Europe/Paris) · out of usage credits";
    CHECK(isUsageLimitText(txt));
    CHECK(usageLimitWindow(txt) == "five_hour");
    CHECK(usageLimitWindow("You've hit your weekly limit · resets Mon 9am") == "seven_day");
    CHECK_FALSE(isUsageLimitText("You're out of usage credits. Switch to another model"));
    CHECK(modelAccessFromError("rate_limit", txt) == ModelAccess::Unknown);  // avant : NeedsCredits → Opus bloqué

    ClaudeSession s;
    s.config.model = "opus";
    int refused = 0, limited = 0, finished = 0;
    s.onModelRefused = [&](ClaudeSession&) { ++refused; };
    s.onUsageLimit = [&](ClaudeSession&) { ++limited; };
    s.onTurnFinished = [&](ClaudeSession&) { ++finished; };
    s.handleLine(R"({"type":"assistant","message":{"id":"l1","model":"<synthetic>","role":"assistant","type":"message","content":[{"type":"text","text":"You've hit your 5-hour limit · resets 8:30am (Europe/Paris) · out of usage credits"}]},"session_id":"s","error":"rate_limit"})");
    s.handleLine(R"({"type":"result","subtype":"success","is_error":true,"total_cost_usd":0,"result":"You've hit your 5-hour limit · resets 8:30am (Europe/Paris) · out of usage credits","session_id":"s"})");
    CHECK(refused == 0);
    CHECK(limited == 1);
    CHECK(finished == 1);
    CHECK(s.modelRefused == ModelAccess::Unknown);
    CHECK(s.usageLimited == "five_hour");
    CHECK(s.stats.turns == 0);  // le tour repartira après la réinitialisation
    REQUIRE(s.entries.size() == 1);
    CHECK(s.entries[0].text.find("8:30am") != std::string::npos);
}

TEST_CASE("accès aux modèles : l'ancienne observation erronée d'Opus est ignorée") {
    ModelAccessBook book;
    book.fromJson(nlohmann::json::parse(R"({"opus":{"access":2,"plan":"pro","when":1},"fable":{"access":2,"plan":"pro","when":1}})"));
    CHECK(book.usable("opus", "pro"));
    CHECK_FALSE(book.usable("fable", "pro"));
    book.record("sonnet", "pro", ModelAccess::NeedsCredits, 2);  // jamais vrai pour un modèle inclus
    CHECK(book.usable("sonnet", "pro"));
}
