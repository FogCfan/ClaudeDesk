#include "claude/CommandLine.h"

#include <cstdlib>

#include "util/Paths.h"
#include "util/Strings.h"

namespace claude {

std::string quoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == '\\') ++i, ++backslashes;
        if (i == arg.size()) {
            out.append(backslashes * 2, '\\');  // doublés avant le guillemet final
            break;
        }
        if (arg[i] == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
        } else {
            out.append(backslashes, '\\');
            out += arg[i];
        }
    }
    out += '"';
    return out;
}

std::string buildCommandLine(const std::string& exe, const std::vector<std::string>& args) {
    std::string cmd = quoteArg(exe);
    for (const auto& a : args) cmd += ' ' + quoteArg(a);
    return cmd;
}

std::string findClaudeExecutable() {
    std::vector<fs::path> dirs;
#ifdef _WIN32
    if (const wchar_t* p = _wgetenv(L"PATH"))
        for (auto& d : util::split(util::narrow(p), ';'))
            if (!d.empty()) dirs.push_back(util::pathFromUtf8(d));
#endif
    std::error_code ec;
    for (const auto& d : dirs)
        if (fs::exists(d / "claude.exe", ec)) return util::pathToUtf8(d / "claude.exe");
    for (const auto& d : dirs) {
        // Installation npm : claude.cmd lance node_modules\@anthropic-ai\claude-code\bin\claude.exe
        if (fs::exists(d / "claude.cmd", ec)) {
            fs::path native = d / "node_modules" / "@anthropic-ai" / "claude-code" / "bin" / "claude.exe";
            if (fs::exists(native, ec)) return util::pathToUtf8(native);
        }
    }
    fs::path local = util::homeDir() / ".local" / "bin" / "claude.exe";
    if (fs::exists(local, ec)) return util::pathToUtf8(local);
    return {};
}

}  // namespace claude
