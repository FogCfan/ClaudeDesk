#include "claude/CommandLine.h"

#include <cstdlib>

#ifndef _WIN32
#include <unistd.h>
#endif

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
    std::error_code ec;
#ifdef _WIN32
    if (const wchar_t* p = _wgetenv(L"PATH"))
        for (auto& d : util::split(util::narrow(p), ';'))
            if (!d.empty()) dirs.push_back(util::pathFromUtf8(d));
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
#else
    // PATH (complété au démarrage par celui du shell de connexion, cf. util::importLoginShellPath),
    // puis les emplacements d'installation habituels.
    if (const char* p = std::getenv("PATH"))
        for (auto& d : util::split(p, ':'))
            if (!d.empty()) dirs.push_back(fs::path(d));
    fs::path home = util::homeDir();
    for (fs::path d : {home / ".local" / "bin", home / ".claude" / "local", home / ".npm-global" / "bin",
                       home / ".bun" / "bin", fs::path("/opt/homebrew/bin"), fs::path("/usr/local/bin")})
        dirs.push_back(d);
    for (const auto& d : dirs) {
        fs::path c = d / "claude";
        if (fs::is_regular_file(c, ec) && ::access(c.c_str(), X_OK) == 0) return util::pathToUtf8(c);
    }
#endif
    return {};
}

}  // namespace claude
