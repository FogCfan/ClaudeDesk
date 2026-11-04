#include "claude/UsageTracker.h"

#include <algorithm>

#include "util/Paths.h"
#include "util/Strings.h"

namespace claude {

void UsageTracker::onRateLimitEvent(const nlohmann::json& info) {
    if (!info.is_object()) return;
    overageStatus = info.value("overageStatus", overageStatus);
    usingOverage = info.value("isUsingOverage", usingOverage);
    auto upsert = [&](const std::string& name, const nlohmann::json& w) {
        RateWindow* rw = nullptr;
        for (auto& x : windows)
            if (x.name == name) rw = &x;
        if (!rw) {
            windows.push_back({name});
            rw = &windows.back();
        }
        if (w.contains("utilization") && w["utilization"].is_number()) rw->utilization = w["utilization"].get<double>();
        if (w.contains("resetsAt") && w["resetsAt"].is_number()) rw->resetsAt = w["resetsAt"].get<int64_t>();
        rw->status = w.value("status", rw->status);
    };
    if (info.contains("unifiedWindows") && info["unifiedWindows"].is_object()) {
        for (auto& [name, w] : info["unifiedWindows"].items()) upsert(name, w);
    } else if (info.contains("rateLimitType")) {
        upsert(info.value("rateLimitType", "five_hour"), info);
    }
    // Ordre d'affichage stable : 5 h puis 7 jours puis le reste.
    std::stable_sort(windows.begin(), windows.end(), [](const RateWindow& a, const RateWindow& b) {
        auto rank = [](const std::string& n) { return n == "five_hour" ? 0 : n == "seven_day" ? 1 : 2; };
        return rank(a.name) < rank(b.name);
    });
    lastUpdate = util::nowUnix();
    UsageSample s{lastUpdate};
    if (auto* w = find("five_hour")) s.fiveHour = w->utilization;
    if (auto* w = find("seven_day")) s.sevenDay = w->utilization;
    history.push_back(s);
    while (!history.empty() && history.front().time < lastUpdate - 8 * 86400) history.erase(history.begin());
    dirty = true;
}

const RateWindow* UsageTracker::find(const std::string& name) const {
    for (auto& w : windows)
        if (w.name == name) return &w;
    return nullptr;
}

void UsageTracker::load(const std::string& path) {
    std::string txt;
    if (!util::readFile(util::pathFromUtf8(path), txt)) return;
    auto j = nlohmann::json::parse(txt, nullptr, false);
    if (!j.is_object()) return;
    windows.clear();
    for (auto& w : j.value("windows", nlohmann::json::array()))
        windows.push_back({w.value("name", ""), w.value("utilization", 0.0), w.value("resetsAt", (int64_t)0),
                           w.value("status", "")});
    history.clear();
    for (auto& h : j.value("history", nlohmann::json::array()))
        history.push_back({h.value("t", (int64_t)0), h.value("h5", 0.0), h.value("d7", 0.0)});
    lastUpdate = j.value("lastUpdate", (int64_t)0);
    overageStatus = j.value("overageStatus", "");
    subscription = j.value("subscription", "");
}

void UsageTracker::save(const std::string& path) const {
    nlohmann::json j;
    j["windows"] = nlohmann::json::array();
    for (auto& w : windows)
        j["windows"].push_back({{"name", w.name}, {"utilization", w.utilization}, {"resetsAt", w.resetsAt}, {"status", w.status}});
    j["history"] = nlohmann::json::array();
    for (auto& h : history) j["history"].push_back({{"t", h.time}, {"h5", h.fiveHour}, {"d7", h.sevenDay}});
    j["lastUpdate"] = lastUpdate;
    j["overageStatus"] = overageStatus;
    j["subscription"] = subscription;
    util::writeFileAtomic(util::pathFromUtf8(path), j.dump(1));
}

std::string windowLabel(const std::string& name) {
    if (name == "five_hour") return "Session (fenêtre 5 h)";
    if (name == "seven_day") return "Semaine (tous modèles)";
    if (name == "seven_day_opus") return "Semaine (Opus)";
    if (name == "seven_day_sonnet") return "Semaine (Sonnet)";
    return name;
}

nlohmann::json SessionStats::toJson() const {
    nlohmann::json turnsJ = nlohmann::json::array();
    for (auto& t : turnHistory)
        turnsJ.push_back({{"t", t.time}, {"c", t.costUsd}, {"i", t.inputTokens}, {"o", t.outputTokens}, {"d", t.durationS}});
    return {{"costUsd", costUsd},           {"inputTokens", inputTokens},   {"outputTokens", outputTokens},
            {"cacheRead", cacheReadTokens}, {"cacheWrite", cacheWriteTokens}, {"turns", turns},
            {"contextTokens", contextTokens}, {"contextWindow", contextWindow}, {"apiSeconds", apiSeconds},
            {"lastModel", lastModel},       {"turnHistory", turnsJ}};
}

void SessionStats::fromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    costUsd = j.value("costUsd", 0.0);
    inputTokens = j.value("inputTokens", (int64_t)0);
    outputTokens = j.value("outputTokens", (int64_t)0);
    cacheReadTokens = j.value("cacheRead", (int64_t)0);
    cacheWriteTokens = j.value("cacheWrite", (int64_t)0);
    turns = j.value("turns", 0);
    contextTokens = j.value("contextTokens", (int64_t)0);
    contextWindow = j.value("contextWindow", (int64_t)0);
    apiSeconds = j.value("apiSeconds", 0.0);
    lastModel = j.value("lastModel", "");
    turnHistory.clear();
    for (auto& t : j.value("turnHistory", nlohmann::json::array()))
        turnHistory.push_back({t.value("t", (int64_t)0), t.value("c", 0.0), t.value("i", (int64_t)0),
                               t.value("o", (int64_t)0), t.value("d", 0.0)});
}

}  // namespace claude
