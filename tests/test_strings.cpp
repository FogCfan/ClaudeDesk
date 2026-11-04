#include <doctest/doctest.h>

#include "claude/CommandLine.h"
#include "claude/History.h"
#include "claude/SessionConfig.h"
#include "util/Strings.h"

TEST_CASE("quoteArg suit les règles CommandLineToArgvW") {
    using claude::quoteArg;
    CHECK(quoteArg("simple") == "simple");
    CHECK(quoteArg("") == "\"\"");
    CHECK(quoteArg("a b") == "\"a b\"");
    CHECK(quoteArg("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(quoteArg("C:\\dir with space\\") == "\"C:\\dir with space\\\\\"");
    CHECK(quoteArg("{\"outputStyle\":\"Concise\"}") == "\"{\\\"outputStyle\\\":\\\"Concise\\\"}\"");
}

TEST_CASE("utilitaires texte") {
    CHECK(util::truncateUtf8("héllo", 2) == "h…");  // ne coupe pas le é en deux
    CHECK(util::firstLine("\n  \n  Bonjour le monde\nsuite") == "Bonjour le monde");
    CHECK(util::sanitizeFileName("a/b:c?") == "a_b_c_");
    CHECK(util::sanitizeFileName("  fin. ") == "fin");
    CHECK(util::base64Encode({'M', 'a', 'n'}) == "TWFu");
    CHECK(util::base64Encode({'M', 'a'}) == "TWE=");
    CHECK(util::base64Encode({'M'}) == "TQ==");
    CHECK(util::humanTokens(12345) == "12,3 k");
    CHECK(util::newUuid().size() == 36);
    CHECK(util::unixFromIso("1970-01-01T00:01:00.000Z") == 60);
}

TEST_CASE("encodage des dossiers de projet Claude Code") {
    CHECK(claude::encodeProjectDir("C:\\Dev") == "C--Dev");
    CHECK(claude::encodeProjectDir("C:\\Dev\\Créateur d'interfaces Windows") == "C--Dev-Cr-ateur-d-interfaces-Windows");
}

TEST_CASE("SessionConfig -> arguments") {
    claude::SessionConfig c;
    c.model = "opus";
    c.effort = "high";
    c.permissionMode = "plan";
    c.allowedTools = {"Read", " Bash(git *) "};
    auto a = c.toArgs("abc", "", false);
    auto has = [&](const std::string& k, const std::string& v) {
        for (size_t i = 0; i + 1 < a.size(); ++i)
            if (a[i] == k && a[i + 1] == v) return true;
        return false;
    };
    CHECK(has("--resume", "abc"));
    CHECK(has("--model", "opus"));
    CHECK(has("--effort", "high"));
    CHECK(has("--permission-mode", "plan"));
    CHECK(has("--allowedTools", "Read,Bash(git *)"));
    CHECK(has("--permission-prompt-tool", "stdio"));

    claude::SessionConfig d = c;
    d.model = "sonnet";
    CHECK_FALSE(d.needsRestartComparedTo(c));  // le modèle se change à chaud
    d.effort = "low";
    CHECK(d.needsRestartComparedTo(c));

    nlohmann::json j = c;
    auto back = j.get<claude::SessionConfig>();
    CHECK(back.allowedTools == c.allowedTools);
    CHECK(back.effort == "high");
}
