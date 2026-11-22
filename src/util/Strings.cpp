#include "util/Strings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
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
    // wchar_t fait 32 bits hors Windows : décodage UTF-8 -> points de code.
    std::wstring out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = (unsigned char)utf8[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > utf8.size()) {
            out += (wchar_t)0xFFFD;
            ++i;
            continue;
        }
        uint32_t cp = len == 1 ? c : c & (0xFF >> (len + 1));
        for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)utf8[i + k] & 0x3F);
        out += (wchar_t)cp;
        i += len;
    }
    return out;
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
    std::string out;
    out.reserve(wide.size());
    for (wchar_t wc : wide) {
        uint32_t cp = (uint32_t)wc;
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) out += (char)(0xC0 | (cp >> 6)), out += (char)(0x80 | (cp & 0x3F));
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
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
    // Les horodatages Claude Code finissent par 'Z' ou '+00:00' (UTC, éventuellement décalé) ;
    // les nôtres, sans suffixe, sont locaux.
    auto utc = [&]() -> int64_t {
#ifdef _WIN32
        return (int64_t)_mkgmtime(&tm);
#else
        return (int64_t)timegm(&tm);
#endif
    };
    if (!str.empty() && str.back() == 'Z') return utc();
    size_t tpos = str.find('T');
    if (tpos != std::string::npos) {
        size_t sign = str.find_first_of("+-", tpos);
        int oh = 0, om = 0;
        if (sign != std::string::npos && std::sscanf(str.c_str() + sign + 1, "%d:%d", &oh, &om) >= 1) {
            int64_t off = (int64_t)oh * 3600 + (int64_t)om * 60;
            return utc() - (str[sign] == '-' ? -off : off);
        }
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

std::string humanWhen(int64_t t) {
    if (t <= 0) return "—";
    std::tm tm = localTm(t);
    std::tm now = localTm(nowUnix());
    // Écart en jours calendaires (minuit local à minuit local).
    std::tm a = tm, b = now;
    a.tm_hour = b.tm_hour = 12;
    a.tm_min = b.tm_min = a.tm_sec = b.tm_sec = 0;
    a.tm_isdst = b.tm_isdst = -1;
    long long days = (long long)std::llround(std::difftime(std::mktime(&a), std::mktime(&b)) / 86400.0);
    char hm[16];
    std::strftime(hm, sizeof hm, "%H:%M", &tm);
    if (days == 0) return std::string("aujourd'hui ") + hm;
    if (days == 1) return std::string("demain ") + hm;
    if (days == -1) return std::string("hier ") + hm;
    static const char* jours[] = {"dim.", "lun.", "mar.", "mer.", "jeu.", "ven.", "sam."};
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s %02d/%02d %s", jours[tm.tm_wday % 7], tm.tm_mday, tm.tm_mon + 1, hm);
    return buf;
}

int localMinuteOfDay(int64_t t) {
    std::tm tm = localTm(t);
    return tm.tm_hour * 60 + tm.tm_min;
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
