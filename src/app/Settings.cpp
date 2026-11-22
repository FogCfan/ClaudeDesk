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
    // Passage aux polices fournies (Inter) : l'ancienne taille par défaut (17) passe à 18.
    if (!j.contains("uiFont") && fontSize == 17.0f) fontSize = 18.0f;
    uiFont = j.value("uiFont", uiFont);
    monoFont = j.value("monoFont", monoFont);
    fontWeight = j.value("fontWeight", fontWeight);
    if (j.contains("modelAccess")) modelAccess.fromJson(j["modelAccess"]);
    uiScale = j.value("uiScale", uiScale);
    theme = j.value("theme", theme);
    compactUi = j.value("compactUi", compactUi);
    enterSends = j.value("enterSends", enterSends);
    showThinking = j.value("showThinking", showThinking);
    expandTools = j.value("expandTools", expandTools);
    wrapCode = j.value("wrapCode", wrapCode);
    notifyOnFinish = j.value("notifyOnFinish", notifyOnFinish);
    autoRefreshUsage = j.value("autoRefreshUsage", autoRefreshUsage);
    confirmBypass = j.value("confirmBypass", confirmBypass);
    tutorialSeen = j.value("tutorialSeen", tutorialSeen);
    // Installations antérieures au compteur : le premier lancement a déjà eu lieu.
    launchCount = j.value("launchCount", tutorialSeen || j.contains("openTabs") ? 1 : 0);
    fpsMode = j.value("fpsMode", fpsMode);
    fpsCap = j.value("fpsCap", fpsCap);
    ecoIdle = j.value("ecoIdle", ecoIdle);
    ecoBackground = j.value("ecoBackground", ecoBackground);
    backgroundFps = std::clamp(j.value("backgroundFps", backgroundFps), 5, 240);
    showFps = j.value("showFps", showFps);
    animations = j.value("animations", animations);
    animSpeed = j.value("animSpeed", animSpeed);
    smoothScroll = j.value("smoothScroll", smoothScroll);
    glowEffects = j.value("glowEffects", glowEffects);
    highQuality = j.value("highQuality", highQuality);
    fontRenderer = j.value("fontRenderer", fontRenderer);
    // Inter est dessinée pour un rendu sans grille (lettres régulièrement espacées) : l'ancien défaut (rendu léger)
    // passe au rendu sans grille avec les nouvelles polices.
    // Polices v2 : Inter sans grille paraissait flou. Passage à IBM Plex Sans, rendu avec le hinting de la police et
    // traits épaissis — sauf choix explicite de l'utilisateur (police du système, Noto).
    if (j.value("fontsVersion", 0) < 2) {
        if (uiFont == "inter") uiFont = "plex";
        if (fontRenderer == 0 || fontRenderer == 1) fontRenderer = 3;
        if (!j.contains("fontWeight")) fontWeight = 1.3f;
        fontsVersion = 2;
    }
    renderDriver = j.value("renderDriver", renderDriver);
    // Avant les préréglages d'énergie : réglages d'origine = Automatique (même rendu sur un fixe), sinon on garde
    // les réglages de l'utilisateur (Personnalisé).
    energyMode = j.value("energyMode", energyPresetMatches(*this, EnergyPerformance) ? (int)EnergyAuto : (int)EnergyCustom);
    energyOnAc = std::clamp(j.value("energyOnAc", energyOnAc), 1, 3);
    energyOnBattery = std::clamp(j.value("energyOnBattery", energyOnBattery), 1, 3);
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
    util::writeFileAtomic(util::pathFromUtf8(path), dump());
}

std::string Settings::dump() const {
    json presets = json::array();
    for (auto& p : promptPresets) presets.push_back({{"name", p.name}, {"text", p.text}});
    json j = {{"claudeExe", claudeExe},       {"workspaceRoot", workspaceRoot}, {"defaultCwd", defaultCwd},
              {"defaultConfig", defaultConfig}, {"promptPresets", presets},     {"fontSize", fontSize},
              {"uiScale", uiScale},           {"theme", theme}, {"compactUi", compactUi},                 {"enterSends", enterSends},
              {"showThinking", showThinking}, {"expandTools", expandTools},     {"wrapCode", wrapCode},
              {"notifyOnFinish", notifyOnFinish}, {"autoRefreshUsage", autoRefreshUsage}, {"confirmBypass", confirmBypass},
              {"injectContext", injectContext}, {"autoRecap", autoRecap},       {"recapModel", recapModel},
              {"autosaveSeconds", autosaveSeconds}, {"recentCwds", recentCwds}, {"promptHistory", promptHistory},
              {"openTabs", openTabs}, {"activeTab", activeTab}, {"tutorialSeen", tutorialSeen}, {"launchCount", launchCount},
              {"dashDetached", dashDetached}, {"dashShowIdle", dashShowIdle},
              {"fpsMode", fpsMode}, {"fpsCap", fpsCap}, {"ecoIdle", ecoIdle}, {"ecoBackground", ecoBackground},
              {"showFps", showFps}, {"animations", animations}, {"animSpeed", animSpeed}, {"smoothScroll", smoothScroll},
              {"glowEffects", glowEffects}, {"highQuality", highQuality}, {"fontRenderer", fontRenderer},
              {"renderDriver", renderDriver}, {"backgroundFps", backgroundFps}, {"energyMode", energyMode},
              {"energyOnAc", energyOnAc}, {"energyOnBattery", energyOnBattery},
              {"uiFont", uiFont}, {"monoFont", monoFont}, {"fontWeight", fontWeight}, {"fontsVersion", fontsVersion}, {"modelAccess", modelAccess.toJson()}};
    return j.dump(2, ' ', false, json::error_handler_t::replace);
}

const char* energyModeLabel(int mode) {
    switch (mode) {
        case EnergyAuto: return "Automatique";
        case EnergyPerformance: return "Performance";
        case EnergyBalanced: return "Équilibré";
        case EnergySaver: return "Économie de batterie";
        default: return "Personnalisé";
    }
}

const char* energyModeSummary(int mode) {
    switch (mode) {
        case EnergyAuto: return "S'adapte tout seul : Performance sur secteur, Économie quand le portable passe sur batterie.";
        case EnergyPerformance: return "Le plus beau et le plus fluide : rendu continu à la fréquence de l'écran, toutes les animations.";
        case EnergyBalanced: return "Fluide quand vous agissez, au repos sinon : ne redessine que s'il se passe quelque chose ; 30 i/s en arrière-plan.";
        case EnergySaver: return "Pour tenir sur batterie : 30 i/s max, 10 i/s en arrière-plan, sans animations ni effets, rendu au repos.";
        default: return "Vos propres réglages (ci-dessous).";
    }
}

namespace {
struct EnergyValues {
    int fpsMode, fpsCap;
    bool ecoIdle, ecoBackground;
    int backgroundFps;
    bool animations, smoothScroll, glowEffects, highQuality;
};
bool energyValues(int preset, EnergyValues& v) {
    switch (preset) {
        case EnergyPerformance: v = {0, 144, false, false, 30, true, true, true, true}; return true;
        case EnergyBalanced: v = {0, 144, true, true, 30, true, true, true, true}; return true;
        case EnergySaver: v = {1, 30, true, true, 10, false, false, false, false}; return true;
        default: return false;
    }
}
}  // namespace

void applyEnergyPreset(Settings& s, int preset) {
    EnergyValues v;
    if (!energyValues(preset, v)) return;
    s.fpsMode = v.fpsMode;
    if (v.fpsMode == 1) s.fpsCap = v.fpsCap;
    s.ecoIdle = v.ecoIdle;
    s.ecoBackground = v.ecoBackground;
    s.backgroundFps = v.backgroundFps;
    s.animations = v.animations;
    s.smoothScroll = v.smoothScroll;
    s.glowEffects = v.glowEffects;
    s.highQuality = v.highQuality;
}

bool energyPresetMatches(const Settings& s, int preset) {
    EnergyValues v;
    if (!energyValues(preset, v)) return false;
    // La limite n'importe que si elle est active ; la cadence d'arrière-plan, que si elle est réduite.
    return s.fpsMode == v.fpsMode && (v.fpsMode != 1 || s.fpsCap == v.fpsCap) && s.ecoIdle == v.ecoIdle &&
           s.ecoBackground == v.ecoBackground && (!v.ecoBackground || s.backgroundFps == v.backgroundFps) &&
           s.animations == v.animations && s.smoothScroll == v.smoothScroll && s.glowEffects == v.glowEffects &&
           s.highQuality == v.highQuality;
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
