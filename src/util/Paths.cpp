#include "util/Paths.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "util/Strings.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace util {

static fs::path envPath(const wchar_t* name, const fs::path& fallback) {
#ifdef _WIN32
    if (const wchar_t* v = _wgetenv(name); v && *v) return fs::path(v);
#else
    (void)name;
#endif
    return fallback;
}

fs::path homeDir() { return envPath(L"USERPROFILE", fs::current_path()); }

fs::path appDataDir() {
    fs::path p = envPath(L"APPDATA", homeDir()) / "ClaudeDesk";
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
    tmp += L".tmp";
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
    return false;
#endif
}

void openInShell(const std::string& target) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", widen(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)target;
#endif
}

}  // namespace util
