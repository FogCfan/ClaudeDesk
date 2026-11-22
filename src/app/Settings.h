#pragma once
// Réglages de l'application, sauvegardés dans %APPDATA%\ClaudeDesk\settings.json
// (macOS : ~/Library/Application Support/ClaudeDesk/settings.json).
#include <string>
#include <vector>

#include "claude/SessionConfig.h"

namespace app {

struct PromptPreset {
    std::string name;
    std::string text;
};

// Préréglages d'énergie (valeurs de Settings::energyMode).
enum EnergyMode { EnergyAuto = 0, EnergyPerformance = 1, EnergyBalanced = 2, EnergySaver = 3, EnergyCustom = 4 };
const char* energyModeLabel(int mode);
const char* energyModeSummary(int mode);

struct Settings;
// Applique un préréglage fixe (Performance, Équilibré, Économie) aux réglages graphiques.
void applyEnergyPreset(Settings& s, int preset);
// Les réglages graphiques correspondent-ils exactement à ce préréglage ?
bool energyPresetMatches(const Settings& s, int preset);

struct Settings {
    std::string claudeExe;              // vide = détection automatique
    std::string workspaceRoot;          // vide = Documents\ClaudeDesk
    std::string defaultCwd;             // dossier de travail par défaut hors projet
    claude::SessionConfig defaultConfig;
    std::vector<PromptPreset> promptPresets;

    // Éditeur / affichage
    float fontSize = 17.0f;
    float uiScale = 1.0f;
    int theme = 0;                      // 0 sombre, 1 clair, 2 crépuscule
    bool enterSends = true;             // Entrée envoie, Maj+Entrée = nouvelle ligne
    bool showThinking = true;
    bool expandTools = false;
    bool wrapCode = false;
    bool notifyOnFinish = true;         // fait clignoter la barre des tâches
    bool autoRefreshUsage = true;       // mesure l'usage au lancement et quand la mesure est périmée
    bool confirmBypass = true;
    bool tutorialSeen = false;          // ancien indicateur (migration)
    int launchCount = 0;                // 1er lancement : guide ; 2 à 11 : rappel ; ensuite rien

    // Graphismes
    int fpsMode = 0;                    // 0 VSync (fréquence de l'écran), 1 limite personnalisée, 2 illimité
    int fpsCap = 144;
    bool ecoIdle = false;               // ne redessiner qu'en cas d'activité
    bool ecoBackground = false;         // cadence réduite (backgroundFps) quand l'application n'a pas le focus
    int backgroundFps = 30;             // cadence en arrière-plan (et réduite) quand ecoBackground
    bool showFps = false;
    bool animations = true;
    float animSpeed = 1.0f;
    bool smoothScroll = true;
    bool glowEffects = true;
    bool highQuality = true;            // courbes / cercles plus finement tessellés
    int fontRenderer = 0;               // 0 FreeType léger, 1 FreeType sans grille, 2 stb_truetype
    std::string renderDriver;           // "" = auto ; direct3d12, direct3d11, vulkan, opengl, gpu, software          // premier lancement : proposer la visite guidée
    // Énergie : préréglage choisi (EnergyMode). Automatique = Performance sur secteur, Économie sur batterie
    // (réglables). Modifier un réglage de détail fait passer en « Personnalisé ».
    int energyMode = 0;
    int energyOnAc = 1;                 // préréglage sur secteur (mode automatique)
    int energyOnBattery = 3;            // préréglage sur batterie (mode automatique)
    bool dashDetached = false;          // tableau de bord dans sa propre fenêtre
    bool dashShowIdle = true;

    // Cowork
    bool injectContext = true;          // contexte du dossier dans les nouvelles sessions
    bool autoRecap = false;             // récap automatique à la fermeture d'une conversation
    std::string recapModel = "haiku";
    int autosaveSeconds = 5;

    std::vector<std::string> recentCwds;
    std::vector<std::string> promptHistory;  // derniers messages envoyés (rappel avec Ctrl+Haut)
    std::vector<std::string> openTabs;       // conversations ouvertes à la fermeture
    std::string activeTab;

    void load(const std::string& path);
    void save(const std::string& path) const;
    void pushRecentCwd(const std::string& cwd);
    void pushPromptHistory(const std::string& text);
};

}  // namespace app
