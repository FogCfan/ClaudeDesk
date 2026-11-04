// Rejoue des lignes réelles du protocole stream-json de Claude Code.
#include <doctest/doctest.h>

#include "claude/ClaudeSession.h"

using namespace claude;

TEST_CASE("streaming de texte puis message complet") {
    ClaudeSession s;
    s.state = SessionState::Busy;
    s.handleLine(R"({"type":"system","subtype":"init","session_id":"sid-1","tools":["Bash","Read"],"model":"claude-haiku-5-5","permissionMode":"default","claude_code_version":"2.1.318","mcp_servers":[{"name":"x","status":"connected"}]})");
    CHECK(s.claudeSessionId == "sid-1");
    CHECK(s.tools.size() == 2);
    CHECK(s.activeModel == "claude-haiku-5-5");

    s.handleLine(R"({"type":"stream_event","event":{"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}},"parent_tool_use_id":null})");
    s.handleLine(R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Bon"}},"parent_tool_use_id":null})");
    s.handleLine(R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"jour"}},"parent_tool_use_id":null})");
    REQUIRE(s.entries.size() == 1);
    CHECK(s.entries[0].streaming);
    CHECK(s.entries[0].text == "Bonjour");
    s.handleLine(R"({"type":"stream_event","event":{"type":"content_block_stop","index":0},"parent_tool_use_id":null})");
    s.handleLine(R"({"type":"assistant","message":{"model":"claude-haiku-5-5","id":"msg_1","role":"assistant","content":[{"type":"text","text":"Bonjour !"}],"usage":{"input_tokens":2,"cache_creation_input_tokens":100,"cache_read_input_tokens":1000,"output_tokens":5}},"parent_tool_use_id":null})");
    REQUIRE(s.entries.size() == 1);  // le message complet remplace l'entrée diffusée
    CHECK_FALSE(s.entries[0].streaming);
    CHECK(s.entries[0].text == "Bonjour !");
    CHECK(s.stats.contextTokens == 1107);

    bool finished = false;
    s.onTurnFinished = [&](ClaudeSession&) { finished = true; };
    s.handleLine(R"({"type":"result","subtype":"success","is_error":false,"duration_ms":1901,"duration_api_ms":1641,"total_cost_usd":0.0023,"session_id":"sid-1","usage":{"input_tokens":2,"output_tokens":4},"modelUsage":{"claude-haiku-5-5":{"inputTokens":2,"outputTokens":4,"costUSD":0.0023,"contextWindow":1000000}},"model_usage_since_last_result":{"claude-haiku-5-5":{"inputTokens":2,"outputTokens":4,"cacheReadInputTokens":1000,"cacheCreationInputTokens":100,"costUSD":0.0023,"contextWindow":1000000}}})");
    CHECK(finished);
    CHECK(s.state == SessionState::Idle);
    CHECK(s.stats.turns == 1);
    CHECK(s.stats.costUsd == doctest::Approx(0.0023));
    CHECK(s.stats.contextWindow == 1000000);
    CHECK(s.stats.outputTokens == 4);
}

TEST_CASE("appel d'outil, résultat et demande de permission") {
    ClaudeSession s;
    s.state = SessionState::Busy;
    s.handleLine(R"({"type":"assistant","message":{"id":"m2","content":[{"type":"tool_use","id":"toolu_1","name":"Write","input":{"file_path":"C:\\tmp\\a.txt","content":"hi"}}]},"parent_tool_use_id":null})");
    REQUIRE(s.entries.size() == 1);
    CHECK(s.entries[0].kind == EntryKind::ToolUse);
    CHECK(s.entries[0].text == "Write · C:\\tmp\\a.txt");

    s.handleLine(R"({"type":"control_request","request_id":"req-9","request":{"subtype":"can_use_tool","tool_name":"Write","input":{"file_path":"C:\\tmp\\a.txt","content":"hi"},"permission_suggestions":[{"type":"setMode","mode":"acceptEdits","destination":"session"}],"tool_use_id":"toolu_1"}})");
    REQUIRE(s.permissions.size() == 1);
    CHECK(s.permissions[0].toolName == "Write");
    CHECK_FALSE(s.permissions[0].isQuestion);
    s.respondPermission("req-9", false, "non");
    CHECK(s.permissions.empty());

    s.handleLine(R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","content":"refusé","is_error":true,"tool_use_id":"toolu_1"}]},"parent_tool_use_id":null})");
    CHECK(s.entries[0].toolResult == "refusé");
    CHECK(s.entries[0].toolError);
}

TEST_CASE("limites d'usage") {
    UsageTracker u;
    u.onRateLimitEvent(nlohmann::json::parse(R"({"status":"allowed","resetsAt":1793829000,"rateLimitType":"five_hour","overageStatus":"rejected","isUsingOverage":false,"unifiedWindows":{"seven_day":{"utilization":0.24,"resetsAt":1794031200,"status":"allowed"},"five_hour":{"utilization":0.12,"resetsAt":1793829000,"status":"allowed"}}})"));
    REQUIRE(u.windows.size() == 2);
    CHECK(u.windows[0].name == "five_hour");  // trié : 5 h d'abord
    CHECK(u.find("seven_day")->utilization == doctest::Approx(0.24));
    CHECK(u.history.size() == 1);
}

TEST_CASE("sérialisation d'une session") {
    ClaudeSession s;
    s.title = "Test";
    s.claudeSessionId = "abc";
    s.config.model = "opus";
    s.addSystemNote("note");
    auto j = s.toJson();
    ClaudeSession t;
    t.fromJson(j);
    CHECK(t.title == "Test");
    CHECK(t.claudeSessionId == "abc");
    CHECK(t.config.model == "opus");
    CHECK(t.entries.size() == 1);
    CHECK(t.localId == s.localId);
}
