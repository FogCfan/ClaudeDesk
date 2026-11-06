#include "util/Paths.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "util/Strings.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <cstdio>
#include <thread>

#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace util {

static std::vector<std::string> g_launchArgs;

void setLaunchArgs(int argc, char** argv) {
    g_launchArgs.assign(argv, argv + argc);
}

#ifdef _WIN32

static fs::path envPath(const wchar_t* name, const fs::path& fallback) {
    if (const wchar_t* v = _wgetenv(name); v && *v) return fs::path(v);
    return fallback;
}

fs::path homeDir() { return envPath(L"USERPROFILE", fs::current_path()); }

static fs::path configRoot() { return envPath(L"APPDATA", homeDir()); }

#else

fs::path homeDir() {
    if (const char* h = std::getenv("HOME"); h && *h) return fs::path(h);
    if (const passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_dir) return fs::path(pw->pw_dir);
    return fs::current_path();
}

static fs::path configRoot() {
#if defined(__APPLE__)
    return homeDir() / "Library" / "Application Support";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return fs::path(x);
    return homeDir() / ".config";
#endif
}

#endif

fs::path appDataDir() {
    fs::path p = configRoot() / "ClaudeDesk";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

fs::path documentsDir() { return homeDir() / "Documents"; }
fs::path claudeConfigDir() { return homeDir() / ".claude"; }

fs::path pathFromUtf8(const std::string& s) {
#ifdef _WIN32
    return fs::path(widen(s));
#else
    return fs::path(s);
#endif
}

std::string pathToUtf8(const fs::path& p) {
#ifdef _WIN32
    return narrow(p.wstring());
#else
    return p.string();
#endif
}

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    if (startsWith(out, "\xEF\xBB\xBF")) out.erase(0, 3);  // BOM
    return true;
}

bool readBinary(const fs::path& p, std::vector<unsigned char>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

bool writeFileAtomic(const fs::path& p, const std::string& content) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), (std::streamsize)content.size());
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {  // ex. fichier verrouillé : on tente une copie
        fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp);
        return !ec;
    }
    return true;
}

#ifndef _WIN32
// Lance un programme détaché (sans shell) ; un thread récupère son code de sortie.
static bool spawnDetached(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    std::vector<std::string> store = args;
    std::vector<char*> argv;
    for (auto& a : store) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (::posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return false;
    std::thread([pid] {
        int st = 0;
        ::waitpid(pid, &st, 0);
    }).detach();
    return true;
}

static std::string selfExePath() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    buf.resize(std::char_traits<char>::length(buf.c_str()));
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::path(buf), ec);
    return ec ? buf : canon.string();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
#endif
}
#endif

bool relaunchSelf() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
#else
    std::string exe = selfExePath();
    if (exe.empty()) return false;
    std::vector<std::string> args = g_launchArgs;
    if (args.empty()) args.push_back(exe);
    else args[0] = exe;
    // posix_spawn (et non posix_spawnp) : chemin absolu de l'exécutable courant.
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    return ::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) == 0;
#endif
}

void openInShell(const std::string& target) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", widen(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    spawnDetached({"open", target});
#else
    spawnDetached({"xdg-open", target});
#endif
}

void importLoginShellPath() {
#ifndef _WIN32
    // Lancée depuis le Finder / le Dock, l'application n'hérite que d'un PATH minimal
    // (/usr/bin:/bin:…) : ni Homebrew, ni node, ni ~/.local/bin. On récupère celui du shell
    // de connexion de l'utilisateur, comme le font les éditeurs de code.
    const char* shell = std::getenv("SHELL");
    std::string sh = shell && *shell ? shell : "/bin/zsh";
    std::string cmd = "'" + sh + "' -l -i -c 'printf \"\\n__CDPATH__%s__CDEND__\\n\" \"$PATH\"' </dev/null 2>/dev/null";
    FILE* f = ::popen(cmd.c_str(), "r");
    if (!f) return;
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    ::pclose(f);
    auto a = out.find("__CDPATH__");
    auto b = out.find("__CDEND__", a == std::string::npos ? 0 : a);
    if (a == std::string::npos || b == std::string::npos) return;
    std::string loginPath = out.substr(a + 10, b - a - 10);
    if (loginPath.empty()) return;
    // PATH du shell d'abord, puis ce qui existait déjà et n'y figure pas.
    std::string merged = loginPath;
    auto have = split(loginPath, ':');
    if (const char* cur = std::getenv("PATH"))
        for (auto& d : split(cur, ':'))
            if (!d.empty() && std::find(have.begin(), have.end(), d) == have.end()) merged += ":" + d;
    ::setenv("PATH", merged.c_str(), 1);
#endif
}

}  // namespace util
