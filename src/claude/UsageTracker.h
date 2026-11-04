#pragma once
// Suivi de l'usage : limites du forfait (équivalent de /usage, alimenté par les
// événements `rate_limit_event` du CLI) et statistiques par session.
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace claude {

struct RateWindow {
    std::string name;        // five_hour, seven_day, seven_day_opus…
    double utilization = 0;  // 0..1
    int64_t resetsAt = 0;    // secondes Unix
    std::string status;      // allowed, allowed_warning, rejected
};

struct UsageSample {
    int64_t time = 0;
    double fiveHour = 0, sevenDay = 0;
};

class UsageTracker {
public:
    std::vector<RateWindow> windows;
    std::string overageStatus;
    bool usingOverage = false;
    int64_t lastUpdate = 0;
    std::vector<UsageSample> history;  // une mesure par événement (gardées 8 jours)
    std::string subscription;          // « Claude Pro »… (réponse initialize)

    void onRateLimitEvent(const nlohmann::json& info);
    const RateWindow* find(const std::string& name) const;

    void load(const std::string& path);
    void save(const std::string& path) const;
    bool dirty = false;
};

std::string windowLabel(const std::string& name);  // « Session (5 h) », « Semaine »…

struct TurnStat {
    int64_t time = 0;
    double costUsd = 0;
    int64_t inputTokens = 0, outputTokens = 0;
    double durationS = 0;
};

struct SessionStats {
    double costUsd = 0;
    int64_t inputTokens = 0, outputTokens = 0, cacheReadTokens = 0, cacheWriteTokens = 0;
    int turns = 0;
    int64_t contextTokens = 0;   // taille du contexte au dernier appel
    int64_t contextWindow = 0;   // fenêtre du modèle (ex. 1 000 000)
    double apiSeconds = 0;
    std::vector<TurnStat> turnHistory;
    std::string lastModel;

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
};

}  // namespace claude
