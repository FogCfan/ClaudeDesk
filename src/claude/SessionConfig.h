#pragma once
// Configuration d'une session Claude Code : tout ce qui se traduit en options
// de ligne de commande (modèle, effort, mode, outils, prompt système…).
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace claude {

enum class SystemPromptMode { Default, Append, Replace };

struct SessionConfig {
    std::string model = "";             // "" = modèle par défaut du CLI ; sinon alias (opus, sonnet…) ou id complet
    std::string fallbackModel = "";
    std::string effort = "";            // "" (défaut), low, medium, high, xhigh, max
    std::string permissionMode = "default";  // default, acceptEdits, auto, plan, dontAsk, bypassPermissions
    std::vector<std::string> allowedTools;     // règles toujours autorisées (ex. "Bash(git *)")
    std::vector<std::string> disallowedTools;  // règles interdites
    std::vector<std::string> enabledTools;     // vide = jeu par défaut ; sinon liste --tools
    SystemPromptMode systemPromptMode = SystemPromptMode::Default;
    std::string systemPrompt;           // texte ajouté / de remplacement
    std::vector<std::string> addDirs;
    double maxBudgetUsd = 0.0;          // 0 = illimité
    std::string extraArgs;              // options supplémentaires, séparées par des espaces
    std::string outputStyle;            // "" / default, Concise, Explanatory, Learning…
    bool chrome = false;

    // Options qui exigent un redémarrage du processus pour être appliquées
    // (le modèle et le mode, eux, se changent à chaud).
    bool needsRestartComparedTo(const SessionConfig& other) const;

    // Construit les arguments. `resumeId` non vide => --resume ; `promptFile`
    // = fichier contenant le prompt système effectif (prompt + contexte cowork).
    std::vector<std::string> toArgs(const std::string& resumeId, const std::string& promptFile,
                                    bool hasExtraContext) const;
};

void to_json(nlohmann::json& j, const SessionConfig& c);
void from_json(const nlohmann::json& j, SessionConfig& c);

// Listes proposées dans l'interface.
struct Choice {
    const char* value;
    const char* label;
    const char* help;
};
const std::vector<Choice>& modelChoices();
const std::vector<Choice>& effortChoices();
const std::vector<Choice>& permissionModeChoices();
const std::vector<std::string>& knownTools();

}  // namespace claude
