#include "claude/ClaudeSession.h"

#include <algorithm>

#include "util/Paths.h"
#include "util/Strings.h"

namespace claude {

using json = nlohmann::json;

const char* stateLabel(SessionState s) {
    switch (s) {
        case SessionState::Stopped: return "En veille";
        case SessionState::Starting: return "Démarrage…";
        case SessionState::Idle: return "Prêt";
        case SessionState::Busy: return "En cours…";
    }
    return "?";
}

static std::string jstr(const json& j, const char* key, const std::string& def = {}) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

static int64_t jint(const json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_number()) return j[key].get<int64_t>();
    return 0;
}

// Contenu d'un tool_result : chaîne ou tableau de blocs.
static std::string flattenContent(const json& c) {
    if (c.is_string()) return c.get<std::string>();
    std::string out;
    if (c.is_array()) {
        for (auto& b : c) {
            std::string t = jstr(b, "type");
            if (t == "text") out += jstr(b, "text") + "\n";
            else if (t == "image") out += "[image]\n";
            else if (t == "tool_reference") out += "[outil : " + jstr(b, "tool_name") + "]\n";
        }
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

ClaudeSession::ClaudeSession() : localId(util::newUuid()) {}

ClaudeSession::~ClaudeSession() { stop(); }

bool ClaudeSession::processRunning() const { return process_ && process_->running(); }

void ClaudeSession::addSystemNote(const std::string& text, bool error) {
    Entry e;
    e.kind = error ? EntryKind::Error : EntryKind::System;
    e.text = text;
    e.time = util::nowUnix();
    entries.push_back(std::move(e));
    dirty = true;
}

bool ClaudeSession::startProcess() {
    if (claudeExe.empty()) {
        addSystemNote("Exécutable Claude Code introuvable. Indiquez son chemin dans Configuration > Application.", true);
        return false;
    }
    // Prompt système effectif (texte de l'utilisateur + contexte cowork) dans un fichier.
    std::string promptFile;
    std::string prompt;
    if (config.systemPromptMode != SystemPromptMode::Default) prompt = config.systemPrompt;
    if (!util::trim(extraContext).empty()) prompt += (prompt.empty() ? "" : "\n\n") + extraContext;
    if (!util::trim(prompt).empty()) {
        fs::path dir = util::pathFromUtf8(promptDir.empty() ? util::pathToUtf8(util::appDataDir() / "prompts") : promptDir);
        fs::path f = dir / (localId + ".md");
        if (util::writeFileAtomic(f, prompt)) promptFile = util::pathToUtf8(f);
    }
    bool resume = !claudeSessionId.empty();
    auto args = config.toArgs(resume ? claudeSessionId : "", promptFile, !util::trim(extraContext).empty());
    if (resume && freshPrompt_) args.insert(args.end(), {"--system-prompt-snapshot", "off"});
    if (!config.outputStyle.empty() && config.outputStyle != "default")
        args.insert(args.end(), {"--settings", json{{"outputStyle", config.outputStyle}}.dump()});

    process_ = std::make_unique<Process>();
    if (!process_->start(claudeExe, args, cwd)) {
        addSystemNote(process_->error(), true);
        process_.reset();
        return false;
    }
    triedResume_ = resume;
    sawInit_ = false;
    lastProcessCost_ = 0;
    state = SessionState::Starting;
    statusText = resume ? "Reprise de la session…" : "Démarrage de Claude Code…";
    initRequestId_ = "init-" + std::to_string(++controlCounter_);
    writeJson({{"type", "control_request"}, {"request_id", initRequestId_}, {"request", {{"subtype", "initialize"}}}});
    return true;
}

void ClaudeSession::writeJson(const json& j) {
    if (!process_) return;
    process_->write(j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n");
}

void ClaudeSession::sendControl(const std::string& subtype, json extra) {
    if (!processRunning()) return;
    extra["subtype"] = subtype;
    writeJson({{"type", "control_request"}, {"request_id", "ctl-" + std::to_string(++controlCounter_)}, {"request", extra}});
}

void ClaudeSession::send(const std::string& text, const std::vector<ImageAttachment>& images) {
    if (util::trim(text).empty() && images.empty()) return;
    Entry e;
    e.kind = EntryKind::User;
    e.text = text;
    e.time = util::nowUnix();
    e.attachments = (int)images.size();
    entries.push_back(e);
    if (title == "Nouvelle conversation" || title.empty()) title = util::firstLine(text, 60);
    dirty = true;
    lastUserText_ = text;
    lastUserImages_ = images;

    json content;
    if (images.empty()) {
        content = text;
    } else {
        content = json::array();
        for (auto& img : images)
            content.push_back({{"type", "image"},
                               {"source", {{"type", "base64"}, {"media_type", img.mediaType}, {"data", img.base64}}}});
        if (!text.empty()) content.push_back({{"type", "text"}, {"text", text}});
    }
    json msg = {{"type", "user"}, {"message", {{"role", "user"}, {"content", content}}}};

    if (restartPending && state != SessionState::Busy) {
        restartPending = false;
        stop();
    }
    if (!processRunning()) {
        if (!startProcess()) return;
    }
    if (state == SessionState::Busy) ++queuedMessages;
    writeJson(msg);
    if (state != SessionState::Busy) turnStartTime = lastNow_;
    state = SessionState::Busy;
    statusText = "Envoi…";
    thinkingTokens = 0;
}

void ClaudeSession::interrupt() {
    if (!processRunning() || state != SessionState::Busy) return;
    sendControl("interrupt");
    permissions.clear();
    statusText = "Interruption…";
}

void ClaudeSession::setModel(const std::string& model) {
    config.model = model;
    if (processRunning()) sendControl("set_model", {{"model", model.empty() ? "default" : model}});
    addSystemNote("Modèle : " + (model.empty() ? std::string("par défaut") : model));
}

void ClaudeSession::setPermissionMode(const std::string& mode) {
    config.permissionMode = mode;
    if (processRunning()) sendControl("set_permission_mode", {{"mode", mode}});
    addSystemNote("Mode de permission : " + mode);
}

void ClaudeSession::applyConfig(const SessionConfig& cfg) {
    SessionConfig old = config;
    if (cfg.model != old.model) setModel(cfg.model);
    if (cfg.permissionMode != old.permissionMode) setPermissionMode(cfg.permissionMode);
    bool restartNeeded = cfg.needsRestartComparedTo(old);
    if (cfg.systemPrompt != old.systemPrompt || cfg.systemPromptMode != old.systemPromptMode) freshPrompt_ = true;
    config = cfg;
    dirty = true;
    if (restartNeeded && processRunning()) {
        if (state == SessionState::Busy) {
            restartPending = true;
            addSystemNote("Configuration modifiée : elle s'appliquera à la fin du tour en cours.");
        } else {
            stop();
            addSystemNote("Configuration modifiée : la session reprendra avec les nouveaux réglages au prochain message.");
        }
    }
}

void ClaudeSession::respondPermission(const std::string& requestId, bool allow, const std::string& denyMessage,
                                      const json& updatedInput, bool alwaysAllow) {
    auto it = std::find_if(permissions.begin(), permissions.end(),
                           [&](const PermissionRequest& p) { return p.requestId == requestId; });
    if (it == permissions.end()) return;
    json resp;
    if (allow) {
        resp = {{"behavior", "allow"}, {"updatedInput", updatedInput.is_null() ? it->input : updatedInput}};
        if (alwaysAllow && it->suggestions.is_array() && !it->suggestions.empty())
            resp["updatedPermissions"] = it->suggestions;
    } else {
        resp = {{"behavior", "deny"}, {"message", denyMessage.empty() ? "L'utilisateur a refusé cette action." : denyMessage}};
    }
    writeJson({{"type", "control_response"},
               {"response", {{"subtype", "success"}, {"request_id", requestId}, {"response", resp}}}});
    permissions.erase(it);
    statusText = allow ? "Exécution…" : "Refusé";
}

void ClaudeSession::stop() {
    if (process_) {
        process_->shutdown(1500);
        for (auto& l : process_->poll())
            if (l.isStderr) stderrLog.push_back(l.text);
        process_.reset();
    }
    for (auto& e : entries) e.streaming = false;
    permissions.clear();
    queuedMessages = 0;
    state = SessionState::Stopped;
    statusText.clear();
}

void ClaudeSession::restart() {
    stop();
    addSystemNote("Session arrêtée — elle reprendra au prochain message.");
}

void ClaudeSession::update(double now) {
    lastNow_ = now;
    if (!process_) return;
    for (auto& l : process_->poll()) {
        if (l.isStderr) {
            if (!util::trim(l.text).empty()) stderrLog.push_back(l.text);
            if (stderrLog.size() > 500) stderrLog.erase(stderrLog.begin(), stderrLog.begin() + 100);
        } else {
            handleLine(l.text, now);
        }
    }
    if (!process_->running()) {
        // Lire ce qui reste avant de conclure.
        for (auto& l : process_->poll()) {
            if (l.isStderr) stderrLog.push_back(l.text);
            else handleLine(l.text, now);
        }
        int code = process_->exitCode();
        bool wasBusy = state == SessionState::Busy;
        std::string errTail;
        for (size_t i = stderrLog.size() > 6 ? stderrLog.size() - 6 : 0; i < stderrLog.size(); ++i)
            errTail += stderrLog[i] + "\n";
        process_->shutdown(0);
        process_.reset();
        state = SessionState::Stopped;
        statusText.clear();
        permissions.clear();
        for (auto& e : entries) e.streaming = false;

        bool notFound = util::containsInsensitive(errTail, "no conversation found") ||
                        util::containsInsensitive(errTail, "not found");
        if (triedResume_ && !sawInit_ && notFound && !lastUserText_.empty()) {
            addSystemNote("La session Claude Code d'origine est introuvable : nouvelle session (le récap est injecté s'il existe).");
            claudeSessionId.clear();
            if (startProcess()) {
                json content = lastUserText_;
                writeJson({{"type", "user"}, {"message", {{"role", "user"}, {"content", content}}}});
                state = SessionState::Busy;
            }
            return;
        }
        if (wasBusy || code != 0)
            addSystemNote("Le processus Claude Code s'est arrêté (code " + std::to_string(code) + ")." +
                              (errTail.empty() ? "" : "\n" + errTail),
                          code != 0);
        return;
    }
    if (restartPending && state == SessionState::Idle) {
        restartPending = false;
        stop();
    }
}

void ClaudeSession::handleLine(const std::string& line, double now) {
    if (util::trim(line).empty()) return;
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        stderrLog.push_back("[stdout] " + line);
        return;
    }
    std::string type = jstr(j, "type");
    std::string parent = jstr(j, "parent_tool_use_id");
    if (type == "system") {
        std::string sub = jstr(j, "subtype");
        if (sub == "init") handleInit(j);
        else if (sub == "status") {
            std::string st = jstr(j, "status");
            if (st == "requesting") statusText = "Requête à l'API…";
            else if (st == "compacting") statusText = "Compactage du contexte…";
            else if (!st.empty()) statusText = st;
        } else if (sub == "thinking_tokens") {
            thinkingTokens = jint(j, "estimated_tokens");
            statusText = "Réflexion…";
        } else if (sub == "compact_boundary") {
            addSystemNote("Contexte compacté.");
        } else if (sub == "api_retry" || sub == "api_error") {
            statusText = "Nouvelle tentative API…";
        }
    } else if (type == "assistant") {
        handleAssistant(j.value("message", json::object()), parent);
    } else if (type == "user") {
        handleUser(j.value("message", json::object()), parent);
    } else if (type == "stream_event") {
        handleStreamEvent(j.value("event", json::object()), parent);
    } else if (type == "rate_limit_event") {
        if (onRateLimit) onRateLimit(j.value("rate_limit_info", json::object()));
    } else if (type == "result") {
        handleResult(j, now);
    } else if (type == "control_request") {
        handleControlRequest(j);
    } else if (type == "control_response") {
        handleControlResponse(j);
    }
}

void ClaudeSession::handleInit(const json& j) {
    sawInit_ = true;
    std::string sid = jstr(j, "session_id");
    if (!sid.empty() && sid != claudeSessionId) {
        claudeSessionId = sid;
        dirty = true;
    }
    tools.clear();
    for (auto& t : j.value("tools", json::array()))
        if (t.is_string()) tools.push_back(t.get<std::string>());
    mcpServers.clear();
    for (auto& m : j.value("mcp_servers", json::array())) mcpServers.emplace_back(jstr(m, "name"), jstr(m, "status"));
    activeModel = jstr(j, "model", activeModel);
    activePermissionMode = jstr(j, "permissionMode", activePermissionMode);
    cliVersion = jstr(j, "claude_code_version", cliVersion);
    if (stats.lastModel.empty()) stats.lastModel = activeModel;
}

void ClaudeSession::handleControlResponse(const json& j) {
    json r = j.value("response", json::object());
    std::string id = jstr(r, "request_id");
    if (jstr(r, "subtype") == "error") {
        addSystemNote("Commande refusée par Claude Code : " + jstr(r, "error"), true);
        return;
    }
    json resp = r.value("response", json::object());
    if (id == initRequestId_) {
        commands.clear();
        for (auto& c : resp.value("commands", json::array()))
            commands.push_back({jstr(c, "name"), jstr(c, "description"), jstr(c, "argumentHint")});
        models.clear();
        for (auto& m : resp.value("models", json::array())) {
            ModelInfo mi{jstr(m, "value"), jstr(m, "displayName"), jstr(m, "description")};
            for (auto& e : m.value("supportedEffortLevels", json::array()))
                if (e.is_string()) mi.effortLevels.push_back(e.get<std::string>());
            models.push_back(mi);
        }
        outputStyles.clear();
        for (auto& s : resp.value("available_output_styles", json::array()))
            if (s.is_string()) outputStyles.push_back(s.get<std::string>());
        if (resp.contains("account")) accountPlan = jstr(resp["account"], "subscriptionType");
        activePermissionMode = jstr(resp, "current_permission_mode", activePermissionMode);
        cliVersion = jstr(resp, "claude_code_version", cliVersion);
        if (state == SessionState::Starting) {
            state = SessionState::Idle;
            statusText.clear();
        }
    } else if (resp.contains("model")) {
        activeModel = jstr(resp, "model", activeModel);
    } else if (resp.contains("mode")) {
        activePermissionMode = jstr(resp, "mode", activePermissionMode);
    }
}

void ClaudeSession::handleControlRequest(const json& j) {
    json req = j.value("request", json::object());
    std::string sub = jstr(req, "subtype");
    std::string id = jstr(j, "request_id");
    if (sub == "can_use_tool") {
        PermissionRequest p;
        p.requestId = id;
        p.toolName = jstr(req, "tool_name");
        p.toolUseId = jstr(req, "tool_use_id");
        p.description = jstr(req, "description");
        p.input = req.value("input", json::object());
        p.suggestions = req.value("permission_suggestions", json::array());
        p.isQuestion = p.toolName == "AskUserQuestion";
        permissions.push_back(std::move(p));
        statusText = "En attente de votre décision…";
        return;
    }
    // Requête non gérée : on répond une erreur pour ne pas bloquer le CLI.
    writeJson({{"type", "control_response"},
               {"response", {{"subtype", "error"}, {"request_id", id}, {"error", "Non pris en charge par ClaudeDesk : " + sub}}}});
}

Entry* ClaudeSession::findTool(const std::string& toolUseId) {
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
        if (it->kind == EntryKind::ToolUse && it->toolUseId == toolUseId) return &*it;
    return nullptr;
}

void ClaudeSession::handleStreamEvent(const json& ev, const std::string& parent) {
    if (!parent.empty()) return;  // seuls les messages principaux sont diffusés en direct
    std::string t = jstr(ev, "type");
    if (t == "content_block_start") {
        json cb = ev.value("content_block", json::object());
        std::string bt = jstr(cb, "type");
        if (bt == "text" || bt == "thinking") {
            Entry e;
            e.kind = bt == "text" ? EntryKind::Assistant : EntryKind::Thinking;
            e.streaming = true;
            e.time = util::nowUnix();
            entries.push_back(e);
            streamingEntry_ = (int)entries.size() - 1;
            statusText = bt == "text" ? "Rédaction…" : "Réflexion…";
        } else if (bt == "tool_use") {
            statusText = "Prépare : " + jstr(cb, "name") + "…";
        }
    } else if (t == "content_block_delta") {
        if (streamingEntry_ < 0 || streamingEntry_ >= (int)entries.size()) return;
        json d = ev.value("delta", json::object());
        std::string dt = jstr(d, "type");
        if (dt == "text_delta") entries[streamingEntry_].text += jstr(d, "text");
        else if (dt == "thinking_delta") entries[streamingEntry_].text += jstr(d, "thinking");
    } else if (t == "content_block_stop") {
        streamingEntry_ = -1;
    }
}

void ClaudeSession::handleAssistant(const json& msg, const std::string& parent) {
    std::string mid = jstr(msg, "id");
    auto finalizeStreaming = [&](EntryKind kind, const std::string& text) -> bool {
        if (!parent.empty()) return false;
        for (auto it = entries.rbegin(); it != entries.rend() && it != entries.rbegin() + 8; ++it) {
            if (it->streaming && it->kind == kind) {
                if (!text.empty() || kind == EntryKind::Assistant) it->text = text;
                it->streaming = false;
                it->messageId = mid;
                return true;
            }
        }
        return false;
    };
    for (auto& b : msg.value("content", json::array())) {
        std::string bt = jstr(b, "type");
        Entry e;
        e.time = util::nowUnix();
        e.messageId = mid;
        e.parentToolUseId = parent;
        if (bt == "text") {
            std::string text = jstr(b, "text");
            if (finalizeStreaming(EntryKind::Assistant, text)) continue;
            e.kind = EntryKind::Assistant;
            e.text = text;
        } else if (bt == "thinking" || bt == "redacted_thinking") {
            if (finalizeStreaming(EntryKind::Thinking, jstr(b, "thinking"))) continue;
            e.kind = EntryKind::Thinking;
            e.text = jstr(b, "thinking");
        } else if (bt == "tool_use" || bt == "server_tool_use") {
            e.kind = EntryKind::ToolUse;
            e.toolName = jstr(b, "name");
            e.toolUseId = jstr(b, "id");
            json input = b.value("input", json::object());
            e.toolInput = input.dump(2, ' ', false, json::error_handler_t::replace);
            e.text = toolSummary(e.toolName, input);
            if (parent.empty()) statusText = "Outil : " + e.text;
        } else {
            continue;
        }
        entries.push_back(std::move(e));
        dirty = true;
    }
    if (parent.empty() && msg.contains("usage")) {
        const json& u = msg["usage"];
        int64_t ctx = jint(u, "input_tokens") + jint(u, "cache_creation_input_tokens") + jint(u, "cache_read_input_tokens") +
                      jint(u, "output_tokens");
        if (ctx > 0) stats.contextTokens = ctx;
        std::string model = jstr(msg, "model");
        if (!model.empty()) stats.lastModel = model;
    }
}

void ClaudeSession::handleUser(const json& msg, const std::string& parent) {
    const json& c = msg.contains("content") ? msg["content"] : json();
    if (c.is_array()) {
        for (auto& b : c) {
            if (jstr(b, "type") != "tool_result") continue;
            if (Entry* tool = findTool(jstr(b, "tool_use_id"))) {
                tool->toolResult = flattenContent(b.contains("content") ? b["content"] : json());
                tool->toolError = b.value("is_error", false);
                tool->toolDone = true;
                dirty = true;
            }
        }
    } else if (c.is_string() && parent.empty()) {
        // Sortie d'une commande locale (/cost, /context…) renvoyée comme message utilisateur.
        std::string s = c.get<std::string>();
        if (util::startsWith(s, "<local-command-stdout>")) {
            util::replaceAll(s, "<local-command-stdout>", "");
            util::replaceAll(s, "</local-command-stdout>", "");
            addSystemNote(util::trim(s));
        }
    }
    if (parent.empty() && state == SessionState::Busy) statusText = "Analyse du résultat…";
}

void ClaudeSession::handleResult(const json& j, double now) {
    for (auto& e : entries) e.streaming = false;
    streamingEntry_ = -1;

    TurnStat ts;
    ts.time = util::nowUnix();
    ts.durationS = jint(j, "duration_ms") / 1000.0;
    if (j.contains("model_usage_since_last_result") && j["model_usage_since_last_result"].is_object()) {
        for (auto& [model, mu] : j["model_usage_since_last_result"].items()) {
            ts.costUsd += mu.value("costUSD", 0.0);
            ts.inputTokens += mu.value("inputTokens", (int64_t)0) + mu.value("cacheReadInputTokens", (int64_t)0) +
                              mu.value("cacheCreationInputTokens", (int64_t)0);
            ts.outputTokens += mu.value("outputTokens", (int64_t)0);
            stats.cacheReadTokens += mu.value("cacheReadInputTokens", (int64_t)0);
            stats.cacheWriteTokens += mu.value("cacheCreationInputTokens", (int64_t)0);
            stats.inputTokens += mu.value("inputTokens", (int64_t)0);
        }
    } else {
        double total = j.value("total_cost_usd", 0.0);
        ts.costUsd = std::max(0.0, total - lastProcessCost_);
        json u = j.value("usage", json::object());
        ts.inputTokens = jint(u, "input_tokens") + jint(u, "cache_read_input_tokens") + jint(u, "cache_creation_input_tokens");
        ts.outputTokens = jint(u, "output_tokens");
        stats.cacheReadTokens += jint(u, "cache_read_input_tokens");
        stats.cacheWriteTokens += jint(u, "cache_creation_input_tokens");
        stats.inputTokens += jint(u, "input_tokens");
    }
    lastProcessCost_ = j.value("total_cost_usd", lastProcessCost_);
    if (j.contains("modelUsage") && j["modelUsage"].is_object()) {
        for (auto& [model, mu] : j["modelUsage"].items()) {
            int64_t cw = mu.value("contextWindow", (int64_t)0);
            if (model == stats.lastModel || stats.contextWindow == 0) stats.contextWindow = cw;
        }
    }
    stats.costUsd += ts.costUsd;
    stats.outputTokens += ts.outputTokens;
    stats.apiSeconds += jint(j, "duration_api_ms") / 1000.0;
    stats.turns++;
    stats.turnHistory.push_back(ts);
    if (stats.turnHistory.size() > 400) stats.turnHistory.erase(stats.turnHistory.begin());

    std::string sub = jstr(j, "subtype");
    bool isError = j.value("is_error", false) || (sub != "success" && !sub.empty());
    if (isError) {
        std::string msg = jstr(j, "result");
        if (msg.empty() && j.contains("errors"))
            for (auto& e : j["errors"]) msg += (e.is_string() ? e.get<std::string>() : e.dump()) + "\n";
        if (sub == "error_max_budget_usd") msg = "Budget maximal atteint pour cette session.";
        if (sub == "error_during_execution" && msg.empty()) msg = "Tour interrompu.";
        addSystemNote(msg.empty() ? "Le tour s'est terminé avec une erreur (" + sub + ")." : msg, sub != "error_during_execution");
    }
    std::string sid = jstr(j, "session_id");
    if (!sid.empty() && sid != claudeSessionId) claudeSessionId = sid;

    if (queuedMessages > 0) {
        --queuedMessages;
        turnStartTime = now;
    } else {
        state = SessionState::Idle;
        statusText.clear();
    }
    dirty = true;
    if (onTurnFinished) onTurnFinished(*this);
}

json ClaudeSession::toJson() const {
    json ents = json::array();
    for (auto& e : entries)
        if (!e.streaming) ents.push_back(entryToJson(e));
    return {{"id", localId},       {"title", title},           {"claudeSessionId", claudeSessionId},
            {"cwd", cwd},          {"config", config},          {"stats", stats.toJson()},
            {"entries", ents},     {"version", 1}};
}

void ClaudeSession::fromJson(const json& j) {
    localId = j.value("id", localId);
    title = j.value("title", title);
    claudeSessionId = j.value("claudeSessionId", "");
    cwd = j.value("cwd", cwd);
    if (j.contains("config")) config = j["config"].get<SessionConfig>();
    stats.fromJson(j.value("stats", json::object()));
    entries.clear();
    for (auto& e : j.value("entries", json::array())) entries.push_back(entryFromJson(e));
    dirty = false;
}

}  // namespace claude
