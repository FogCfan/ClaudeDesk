#include "claude/Transcript.h"

#include "util/Strings.h"

namespace claude {

static std::string str(const nlohmann::json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return {};
}

std::string toolSummary(const std::string& name, const nlohmann::json& in) {
    auto shortPath = [](std::string p) {
        auto pos = p.find_last_of("/\\");
        if (pos != std::string::npos && p.size() > 60) p = "…" + p.substr(pos);
        return p;
    };
    std::string detail;
    if (name == "Read" || name == "Write" || name == "Edit" || name == "NotebookEdit")
        detail = shortPath(str(in, "file_path").empty() ? str(in, "notebook_path") : str(in, "file_path"));
    else if (name == "Bash" || name == "PowerShell")
        detail = str(in, "description").empty() ? str(in, "command") : str(in, "description");
    else if (name == "Glob" || name == "Grep")
        detail = str(in, "pattern");
    else if (name == "WebFetch")
        detail = str(in, "url");
    else if (name == "WebSearch")
        detail = str(in, "query");
    else if (name == "Task" || name == "Agent")
        detail = str(in, "description");
    else if (name == "Skill")
        detail = str(in, "skill");
    else if (name == "TodoWrite")
        detail = "liste de tâches";
    else if (in.is_object() && !in.empty()) {
        for (auto& [k, v] : in.items())
            if (v.is_string()) {
                detail = v.get<std::string>();
                break;
            }
    }
    detail = util::firstLine(detail, 90);
    return detail.empty() ? name : name + " · " + detail;
}

static const char* kindName(EntryKind k) {
    switch (k) {
        case EntryKind::User: return "user";
        case EntryKind::Assistant: return "assistant";
        case EntryKind::Thinking: return "thinking";
        case EntryKind::ToolUse: return "tool";
        case EntryKind::System: return "system";
        case EntryKind::Error: return "error";
    }
    return "system";
}

static EntryKind kindFromName(const std::string& s) {
    if (s == "user") return EntryKind::User;
    if (s == "assistant") return EntryKind::Assistant;
    if (s == "thinking") return EntryKind::Thinking;
    if (s == "tool") return EntryKind::ToolUse;
    if (s == "error") return EntryKind::Error;
    return EntryKind::System;
}

nlohmann::json entryToJson(const Entry& e) {
    nlohmann::json j = {{"kind", kindName(e.kind)}, {"text", e.text}, {"time", e.time}};
    if (!e.messageId.empty()) j["messageId"] = e.messageId;
    if (e.kind == EntryKind::ToolUse) {
        j["toolName"] = e.toolName;
        j["toolUseId"] = e.toolUseId;
        j["toolInput"] = e.toolInput;
        j["toolResult"] = e.toolResult;
        j["toolError"] = e.toolError;
    }
    if (!e.parentToolUseId.empty()) j["parent"] = e.parentToolUseId;
    if (e.attachments) j["attachments"] = e.attachments;
    return j;
}

Entry entryFromJson(const nlohmann::json& j) {
    Entry e;
    e.kind = kindFromName(j.value("kind", "system"));
    e.text = j.value("text", "");
    e.time = j.value("time", (int64_t)0);
    e.messageId = j.value("messageId", "");
    e.toolName = j.value("toolName", "");
    e.toolUseId = j.value("toolUseId", "");
    e.toolInput = j.value("toolInput", "");
    e.toolResult = j.value("toolResult", "");
    e.toolError = j.value("toolError", false);
    e.toolDone = !e.toolResult.empty();  // sans résultat : outil interrompu
    e.parentToolUseId = j.value("parent", "");
    e.attachments = j.value("attachments", 0);
    return e;
}

std::string transcriptToMarkdown(const std::vector<Entry>& entries, size_t maxToolChars) {
    std::string md;
    for (const auto& e : entries) {
        switch (e.kind) {
            case EntryKind::User:
                md += "## 🧑 Utilisateur\n\n" + e.text + "\n\n";
                break;
            case EntryKind::Assistant:
                md += "## 🤖 Claude\n\n" + e.text + "\n\n";
                break;
            case EntryKind::ToolUse:
                if (maxToolChars == 0) {
                    md += "> 🔧 " + e.text + "\n\n";
                } else {
                    md += "> 🔧 **" + e.text + "**\n\n```\n" + util::truncateUtf8(e.toolInput, maxToolChars) + "\n```\n";
                    if (!e.toolResult.empty())
                        md += "Résultat" + std::string(e.toolError ? " (erreur)" : "") + " :\n```\n" +
                              util::truncateUtf8(e.toolResult, maxToolChars) + "\n```\n\n";
                }
                break;
            case EntryKind::Error:
                md += "> ⚠️ " + e.text + "\n\n";
                break;
            default:
                break;
        }
    }
    return md;
}

}  // namespace claude
