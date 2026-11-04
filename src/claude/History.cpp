#include "claude/History.h"

#include <algorithm>
#include <chrono>
#include <fstream>

#include <nlohmann/json.hpp>

#include "util/Paths.h"
#include "util/Strings.h"

namespace claude {

using json = nlohmann::json;

std::string encodeProjectDir(const std::string& path) {
    std::wstring w = util::widen(path);
    std::string out;
    for (wchar_t c : w) {
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) out += (char)c;
        else if (c >= 0xDC00 && c <= 0xDFFF) continue;  // seconde moitié d'une paire UTF-16
        else out += '-';
    }
    return out;
}

static fs::path projectsRoot() { return util::claudeConfigDir() / "projects"; }

static int64_t fileTimeUnix(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return 0;
    auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
    return std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
}

static bool isRealPrompt(const json& msg) {
    if (!msg.contains("content")) return false;
    const json& c = msg["content"];
    std::string text;
    if (c.is_string()) text = c.get<std::string>();
    else if (c.is_array()) {
        for (auto& b : c) {
            if (b.value("type", "") == "text") text += b.value("text", "");
            else if (b.value("type", "") == "tool_result") return false;
        }
    }
    text = util::trim(text);
    return !text.empty() && text[0] != '<';  // <command-name>, <local-command-stdout>, <system-reminder>…
}

static std::string promptText(const json& msg) {
    const json& c = msg["content"];
    if (c.is_string()) return c.get<std::string>();
    std::string text;
    for (auto& b : c)
        if (b.value("type", "") == "text") text += b.value("text", "");
    return text;
}

static HistorySession scanSession(const fs::path& file) {
    HistorySession s;
    s.file = util::pathToUtf8(file);
    s.sessionId = util::pathToUtf8(file.stem());
    std::error_code ec;
    s.sizeBytes = (int64_t)fs::file_size(file, ec);
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        // Filtre rapide avant de parser : les lignes « blob » font parfois des centaines de Ko.
        bool isUser = line.find("\"type\":\"user\"") != std::string::npos;
        bool isTitle = line.find("\"type\":\"ai-title\"") != std::string::npos;
        bool isAssistant = !isUser && line.find("\"type\":\"assistant\"") != std::string::npos;
        if (!isUser && !isTitle && !(isAssistant && s.start == 0)) {
            if (isAssistant || line.find("\"timestamp\"") != std::string::npos) {
                auto p = line.find("\"timestamp\":\"");
                if (p != std::string::npos) {
                    int64_t t = util::unixFromIso(line.substr(p + 13, 24));
                    if (t > s.end) s.end = t;
                }
            }
            continue;
        }
        json j = json::parse(line, nullptr, false);
        if (!j.is_object()) continue;
        if (isTitle) {
            s.title = j.value("aiTitle", s.title);
            continue;
        }
        int64_t t = util::unixFromIso(j.value("timestamp", ""));
        if (t) {
            if (!s.start) s.start = t;
            s.end = std::max(s.end, t);
        }
        if (s.cwd.empty()) s.cwd = j.value("cwd", "");
        if (isUser && !j.value("isSidechain", false) && !j.value("isMeta", false) && j.contains("message") &&
            isRealPrompt(j["message"])) {
            s.userMessages++;
            if (s.firstPrompt.empty()) s.firstPrompt = util::firstLine(promptText(j["message"]), 100);
        }
    }
    if (s.title.empty()) s.title = s.firstPrompt.empty() ? s.sessionId.substr(0, 8) : s.firstPrompt;
    if (!s.end) s.end = fileTimeUnix(file);
    return s;
}

std::vector<HistoryProject> listHistoryProjects() {
    std::vector<HistoryProject> out;
    std::error_code ec;
    for (auto& d : fs::directory_iterator(projectsRoot(), ec)) {
        if (!d.is_directory()) continue;
        HistoryProject p;
        p.dirName = util::pathToUtf8(d.path().filename());
        for (auto& f : fs::directory_iterator(d.path(), ec)) {
            if (f.path().extension() != ".jsonl") continue;
            p.sessions++;
            p.lastModified = std::max(p.lastModified, fileTimeUnix(f.path()));
            if (p.cwd.empty()) {
                std::ifstream in(f.path(), std::ios::binary);
                std::string line;
                for (int i = 0; i < 40 && std::getline(in, line); ++i) {
                    auto pos = line.find("\"cwd\":\"");
                    if (pos == std::string::npos) continue;
                    json j = json::parse(line, nullptr, false);
                    if (j.is_object()) p.cwd = j.value("cwd", "");
                    if (!p.cwd.empty()) break;
                }
            }
        }
        if (p.sessions > 0) out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.lastModified > b.lastModified; });
    return out;
}

std::vector<HistorySession> listHistorySessions(const std::string& projectDirName) {
    std::vector<HistorySession> out;
    std::error_code ec;
    for (auto& f : fs::directory_iterator(projectsRoot() / util::pathFromUtf8(projectDirName), ec)) {
        if (f.path().extension() != ".jsonl") continue;
        auto s = scanSession(f.path());
        if (s.userMessages > 0) out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.end > b.end; });
    return out;
}

std::vector<HistorySession> listHistorySessionsForCwd(const std::string& cwd) {
    std::string enc = util::toLower(encodeProjectDir(cwd));
    std::vector<HistorySession> out;
    std::error_code ec;
    for (auto& d : fs::directory_iterator(projectsRoot(), ec)) {
        if (!d.is_directory() || util::toLower(util::pathToUtf8(d.path().filename())) != enc) continue;
        auto v = listHistorySessions(util::pathToUtf8(d.path().filename()));
        out.insert(out.end(), v.begin(), v.end());
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.end > b.end; });
    return out;
}

std::vector<Entry> loadHistoryTranscript(const std::string& file) {
    std::vector<Entry> out;
    std::ifstream in(util::pathFromUtf8(file), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        bool isUser = line.find("\"type\":\"user\"") != std::string::npos;
        bool isAssistant = line.find("\"type\":\"assistant\"") != std::string::npos;
        if (!isUser && !isAssistant) continue;
        json j = json::parse(line, nullptr, false);
        if (!j.is_object() || j.value("isSidechain", false) || !j.contains("message")) continue;
        std::string type = j.value("type", "");
        const json& msg = j["message"];
        int64_t t = util::unixFromIso(j.value("timestamp", ""));
        if (type == "user") {
            if (j.value("isMeta", false)) continue;
            const json& c = msg.contains("content") ? msg["content"] : json();
            if (c.is_array()) {
                for (auto& b : c) {
                    if (b.value("type", "") != "tool_result") continue;
                    std::string id = b.value("tool_use_id", "");
                    for (auto it = out.rbegin(); it != out.rend(); ++it)
                        if (it->kind == EntryKind::ToolUse && it->toolUseId == id) {
                            const json& rc = b.contains("content") ? b["content"] : json();
                            if (rc.is_string()) it->toolResult = rc.get<std::string>();
                            else if (rc.is_array())
                                for (auto& x : rc)
                                    if (x.value("type", "") == "text") it->toolResult += x.value("text", "");
                            it->toolError = b.value("is_error", false);
                            break;
                        }
                }
            }
            if (isRealPrompt(msg)) {
                Entry e;
                e.kind = EntryKind::User;
                e.text = promptText(msg);
                e.time = t;
                out.push_back(e);
            }
        } else {
            for (auto& b : msg.value("content", json::array())) {
                std::string bt = b.value("type", "");
                Entry e;
                e.time = t;
                e.messageId = msg.value("id", "");
                if (bt == "text") {
                    e.kind = EntryKind::Assistant;
                    e.text = b.value("text", "");
                } else if (bt == "tool_use") {
                    e.kind = EntryKind::ToolUse;
                    e.toolName = b.value("name", "");
                    e.toolUseId = b.value("id", "");
                    json input = b.value("input", json::object());
                    e.toolInput = input.dump(2, ' ', false, json::error_handler_t::replace);
                    e.text = toolSummary(e.toolName, input);
                    e.toolDone = true;
                } else {
                    continue;
                }
                out.push_back(e);
            }
        }
    }
    return out;
}

}  // namespace claude
