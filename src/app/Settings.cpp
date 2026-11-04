#include "app/Settings.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "util/Paths.h"
#include "util/Strings.h"

namespace app {

using json = nlohmann::json;

void Settings::load(const std::string& path) {
    std::string txt;
    if (!util::readFile(util::pathFromUtf8(path), txt)) {
        promptPresets = {
            {"Concis", "Réponds de façon concise et va droit au but. Pas de récapitulatif en fin de réponse."},
            {"Pédagogue", "Explique ton raisonnement étape par étape, comme à un étudiant ingénieur. "
                          "Donne des exemples concrets."},
            {"Revue de code", "Agis comme un relecteur exigeant : cherche les bugs, les cas limites et les "
                              "problèmes de performance avant de proposer des changements."},
        };
        return;
    }
    json j = json::parse(txt, nullptr, false);
    if (!j.is_object()) return;
    claudeExe = j.value("claudeExe", claudeExe);
    workspaceRoot = j.value("workspaceRoot", workspaceRoot);
    defaultCwd = j.value("defaultCwd", defaultCwd);
    if (j.contains("defaultConfig")) defaultConfig = j["defaultConfig"].get<claude::SessionConfig>();
    promptPresets.clear();
    for (auto& p : j.value("promptPresets", json::array())) promptPresets.push_back({p.value("name", ""), p.value("text", "")});
    fontSize = j.value("fontSize", fontSize);
    uiScale = j.value("uiScale", uiScale);
    theme = j.value("theme", theme);
    enterSends = j.value("enterSends", enterSends);
    showThinking = j.value("showThinking", showThinking);
    expandTools = j.value("expandTools", expandTools);
    wrapCode = j.value("wrapCode", wrapCode);
    notifyOnFinish = j.value("notifyOnFinish", notifyOnFinish);
    confirmBypass = j.value("confirmBypass", confirmBypass);
    tutorialSeen = j.value("tutorialSeen", tutorialSeen);
    // Installations antérieures au compteur : le premier lancement a déjà eu lieu.
    launchCount = j.value("launchCount", tutorialSeen || j.contains("openTabs") ? 1 : 0);
    fpsMode = j.value("fpsMode", fpsMode);
    fpsCap = j.value("fpsCap", fpsCap);
    ecoIdle = j.value("ecoIdle", ecoIdle);
    ecoBackground = j.value("ecoBackground", ecoBackground);
    showFps = j.value("showFps", showFps);
    animations = j.value("animations", animations);
    animSpeed = j.value("animSpeed", animSpeed);
    smoothScroll = j.value("smoothScroll", smoothScroll);
    glowEffects = j.value("glowEffects", glowEffects);
    highQuality = j.value("highQuality", highQuality);
    fontRenderer = j.value("fontRenderer", fontRenderer);
    renderDriver = j.value("renderDriver", renderDriver);
    dashDetached = j.value("dashDetached", dashDetached);
    dashShowIdle = j.value("dashShowIdle", dashShowIdle);
    injectContext = j.value("injectContext", injectContext);
    autoRecap = j.value("autoRecap", autoRecap);
    recapModel = j.value("recapModel", recapModel);
    autosaveSeconds = j.value("autosaveSeconds", autosaveSeconds);
    recentCwds = j.value("recentCwds", recentCwds);
    promptHistory = j.value("promptHistory", promptHistory);
    openTabs = j.value("openTabs", openTabs);
    activeTab = j.value("activeTab", activeTab);
}

void Settings::save(const std::string& path) const {
    json presets = json::array();
    for (auto& p : promptPresets) presets.push_back({{"name", p.name}, {"text", p.text}});
    json j = {{"claudeExe", claudeExe},       {"workspaceRoot", workspaceRoot}, {"defaultCwd", defaultCwd},
              {"defaultConfig", defaultConfig}, {"promptPresets", presets},     {"fontSize", fontSize},
              {"uiScale", uiScale},           {"theme", theme},                 {"enterSends", enterSends},
              {"showThinking", showThinking}, {"expandTools", expandTools},     {"wrapCode", wrapCode},
              {"notifyOnFinish", notifyOnFinish}, {"confirmBypass", confirmBypass},
              {"injectContext", injectContext}, {"autoRecap", autoRecap},       {"recapModel", recapModel},
              {"autosaveSeconds", autosaveSeconds}, {"recentCwds", recentCwds}, {"promptHistory", promptHistory},
              {"openTabs", openTabs}, {"activeTab", activeTab}, {"tutorialSeen", tutorialSeen}, {"launchCount", launchCount},
              {"dashDetached", dashDetached}, {"dashShowIdle", dashShowIdle},
              {"fpsMode", fpsMode}, {"fpsCap", fpsCap}, {"ecoIdle", ecoIdle}, {"ecoBackground", ecoBackground},
              {"showFps", showFps}, {"animations", animations}, {"animSpeed", animSpeed}, {"smoothScroll", smoothScroll},
              {"glowEffects", glowEffects}, {"highQuality", highQuality}, {"fontRenderer", fontRenderer},
              {"renderDriver", renderDriver}};
    util::writeFileAtomic(util::pathFromUtf8(path), j.dump(2, ' ', false, json::error_handler_t::replace));
}

void Settings::pushRecentCwd(const std::string& cwd) {
    if (cwd.empty()) return;
    recentCwds.erase(std::remove(recentCwds.begin(), recentCwds.end(), cwd), recentCwds.end());
    recentCwds.insert(recentCwds.begin(), cwd);
    if (recentCwds.size() > 12) recentCwds.resize(12);
}

void Settings::pushPromptHistory(const std::string& text) {
    if (util::trim(text).empty()) return;
    promptHistory.erase(std::remove(promptHistory.begin(), promptHistory.end(), text), promptHistory.end());
    promptHistory.push_back(text);
    if (promptHistory.size() > 100) promptHistory.erase(promptHistory.begin());
}

}  // namespace app
