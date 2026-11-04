#include "util/Strings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace util {

std::wstring widen(std::string_view utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), out.data(), n);
    return out;
#else
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

std::string narrow(std::wstring_view wide) {
#ifdef _WIN32
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), n, nullptr, nullptr);
    return out;
#else
    return std::string(wide.begin(), wide.end());
#endif
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return std::string(s.substr(b, e - b));
}

std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

bool startsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
bool endsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(s.size() - p.size()) == p; }

bool containsInsensitive(std::string_view h, std::string_view n) {
    if (n.empty()) return true;
    return toLower(h).find(toLower(n)) != std::string::npos;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::vector<std::string> splitLines(std::string_view s) {
    auto lines = split(s, '\n');
    for (auto& l : lines)
        if (!l.empty() && l.back() == '\r') l.pop_back();
    return lines;
}

void replaceAll(std::string& s, std::string_view from, std::string_view to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string truncateUtf8(std::string_view s, size_t maxBytes) {
    if (s.size() <= maxBytes) return std::string(s);
    size_t cut = maxBytes;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;  // pas au milieu d'un caractère
    return std::string(s.substr(0, cut)) + "…";
}

std::string firstLine(std::string_view s, size_t maxBytes) {
    for (auto& line : splitLines(s)) {
        auto t = trim(line);
        if (!t.empty()) return truncateUtf8(t, maxBytes);
    }
    return {};
}

std::string sanitizeFileName(std::string_view name) {
    std::string out;
    for (unsigned char c : name) {
        if (c < 32 || std::string_view("<>:\"/\\|?*").find((char)c) != std::string_view::npos)
            out += '_';
        else
            out += (char)c;
    }
    out = trim(out);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();  // interdit par Windows
    if (out.empty()) out = "sans-nom";
    if (out.size() > 80) out = truncateUtf8(out, 80), out.resize(out.size() - std::string("…").size());
    return out;
}

std::string base64Encode(const std::vector<uint8_t>& data) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i + 1 == data.size()) {
        uint32_t v = data[i] << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == data.size()) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::tm localTm(int64_t t) {
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    return tm;
}

std::string isoFromUnix(int64_t t) {
    std::tm tm = localTm(t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

int64_t unixFromIso(std::string_view iso) {
    std::tm tm{};
    int Y, M, D, h = 0, m = 0, s = 0;
    std::string str(iso);
    if (std::sscanf(str.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) < 3) return 0;
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = m; tm.tm_sec = s; tm.tm_isdst = -1;
    // Les horodatages Claude Code finissent par 'Z' (UTC) ; les nôtres sont locaux.
    if (!str.empty() && str.back() == 'Z') {
#ifdef _WIN32
        return (int64_t)_mkgmtime(&tm);
#else
        return (int64_t)timegm(&tm);
#endif
    }
    return (int64_t)std::mktime(&tm);
}

std::string humanDate(int64_t t) {
    if (t <= 0) return "—";
    std::tm tm = localTm(t);
    std::tm now = localTm(nowUnix());
    char buf[32];
    if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday)
        std::strftime(buf, sizeof buf, "aujourd'hui %H:%M", &tm);
    else if (tm.tm_year == now.tm_year)
        std::strftime(buf, sizeof buf, "%d/%m %H:%M", &tm);
    else
        std::strftime(buf, sizeof buf, "%d/%m/%Y", &tm);
    return buf;
}

std::string humanDuration(int64_t s) {
    char buf[48];
    if (s < 0) s = 0;
    if (s < 60) std::snprintf(buf, sizeof buf, "%lld s", (long long)s);
    else if (s < 3600) std::snprintf(buf, sizeof buf, "%lld min", (long long)(s / 60));
    else if (s < 86400) std::snprintf(buf, sizeof buf, "%lld h %02lld", (long long)(s / 3600), (long long)((s % 3600) / 60));
    else std::snprintf(buf, sizeof buf, "%lld j %lld h", (long long)(s / 86400), (long long)((s % 86400) / 3600));
    return buf;
}

static std::string commaDecimal(std::string s) {
    for (char& c : s) if (c == '.') c = ',';
    return s;
}

std::string humanTokens(int64_t n) {
    char buf[32];
    if (n < 1000) std::snprintf(buf, sizeof buf, "%lld", (long long)n);
    else if (n < 1000000) std::snprintf(buf, sizeof buf, "%.1f k", n / 1000.0);
    else std::snprintf(buf, sizeof buf, "%.2f M", n / 1e6);
    return commaDecimal(buf);
}

std::string formatUsd(double usd) {
    char buf[32];
    std::snprintf(buf, sizeof buf, usd < 1.0 ? "%.4f $" : "%.2f $", usd);
    return commaDecimal(buf);
}

std::string newUuid() {
    static std::mt19937_64 rng{std::random_device{}() ^ (uint64_t)nowUnix()};
    uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;  // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;  // variante RFC 4122
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx",
                  (unsigned)(a >> 32), (unsigned)((a >> 16) & 0xFFFF), (unsigned)(a & 0xFFFF),
                  (unsigned)(b >> 48), (unsigned long long)(b & 0xFFFFFFFFFFFFull));
    return buf;
}

}  // namespace util
