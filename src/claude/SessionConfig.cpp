#include "claude/SessionConfig.h"

#include "util/Strings.h"

namespace claude {

static std::string joinComma(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) {
        if (util::trim(s).empty()) continue;
        if (!out.empty()) out += ',';
        out += util::trim(s);
    }
    return out;
}

bool SessionConfig::needsRestartComparedTo(const SessionConfig& o) const {
    return effort != o.effort || fallbackModel != o.fallbackModel || allowedTools != o.allowedTools ||
           disallowedTools != o.disallowedTools || enabledTools != o.enabledTools ||
           systemPromptMode != o.systemPromptMode || systemPrompt != o.systemPrompt || addDirs != o.addDirs ||
           maxBudgetUsd != o.maxBudgetUsd || extraArgs != o.extraArgs || chrome != o.chrome || outputStyle != o.outputStyle;
}

std::vector<std::string> SessionConfig::toArgs(const std::string& resumeId, const std::string& promptFile,
                                               bool hasExtraContext) const {
    std::vector<std::string> a = {"-p", "--input-format", "stream-json", "--output-format", "stream-json",
                                  "--verbose", "--include-partial-messages", "--permission-prompt-tool", "stdio"};
    if (!resumeId.empty()) a.insert(a.end(), {"--resume", resumeId});
    if (!model.empty()) a.insert(a.end(), {"--model", model});
    if (!fallbackModel.empty()) a.insert(a.end(), {"--fallback-model", fallbackModel});
    if (!effort.empty()) a.insert(a.end(), {"--effort", effort});
    if (!permissionMode.empty() && permissionMode != "default") a.insert(a.end(), {"--permission-mode", permissionMode});
    if (auto s = joinComma(allowedTools); !s.empty()) a.insert(a.end(), {"--allowedTools", s});
    if (auto s = joinComma(disallowedTools); !s.empty()) a.insert(a.end(), {"--disallowedTools", s});
    if (!enabledTools.empty()) a.insert(a.end(), {"--tools", joinComma(enabledTools)});
    for (const auto& d : addDirs)
        if (!util::trim(d).empty()) a.insert(a.end(), {"--add-dir", util::trim(d)});
    if (maxBudgetUsd > 0) a.insert(a.end(), {"--max-budget-usd", std::to_string(maxBudgetUsd)});
    if (chrome) a.push_back("--chrome");

    // Le prompt système passe par un fichier : il peut être long (contexte cowork)
    // et la ligne de commande Windows est limitée à 32 767 caractères.
    if (!promptFile.empty()) {
        bool replace = systemPromptMode == SystemPromptMode::Replace && !util::trim(systemPrompt).empty();
        if (replace)
            a.insert(a.end(), {"--system-prompt-file", promptFile});
        else if (hasExtraContext || (systemPromptMode == SystemPromptMode::Append && !util::trim(systemPrompt).empty()))
            a.insert(a.end(), {"--append-system-prompt-file", promptFile});
    }
    for (auto& tok : util::split(extraArgs, ' '))
        if (!util::trim(tok).empty()) a.push_back(util::trim(tok));
    return a;
}

void to_json(nlohmann::json& j, const SessionConfig& c) {
    j = {{"model", c.model},
         {"fallbackModel", c.fallbackModel},
         {"effort", c.effort},
         {"permissionMode", c.permissionMode},
         {"allowedTools", c.allowedTools},
         {"disallowedTools", c.disallowedTools},
         {"enabledTools", c.enabledTools},
         {"systemPromptMode", (int)c.systemPromptMode},
         {"systemPrompt", c.systemPrompt},
         {"addDirs", c.addDirs},
         {"maxBudgetUsd", c.maxBudgetUsd},
         {"extraArgs", c.extraArgs},
         {"outputStyle", c.outputStyle},
         {"chrome", c.chrome}};
}

void from_json(const nlohmann::json& j, SessionConfig& c) {
    c.model = j.value("model", c.model);
    c.fallbackModel = j.value("fallbackModel", c.fallbackModel);
    c.effort = j.value("effort", c.effort);
    c.permissionMode = j.value("permissionMode", c.permissionMode);
    c.allowedTools = j.value("allowedTools", c.allowedTools);
    c.disallowedTools = j.value("disallowedTools", c.disallowedTools);
    c.enabledTools = j.value("enabledTools", c.enabledTools);
    c.systemPromptMode = (SystemPromptMode)j.value("systemPromptMode", (int)c.systemPromptMode);
    c.systemPrompt = j.value("systemPrompt", c.systemPrompt);
    c.addDirs = j.value("addDirs", c.addDirs);
    c.maxBudgetUsd = j.value("maxBudgetUsd", c.maxBudgetUsd);
    c.extraArgs = j.value("extraArgs", c.extraArgs);
    c.outputStyle = j.value("outputStyle", c.outputStyle);
    c.chrome = j.value("chrome", c.chrome);
}

const std::vector<Choice>& modelChoices() {
    static const std::vector<Choice> v = {
        {"", "Par défaut", "Le modèle configuré dans Claude Code (/model)."},
        {"opus", "Opus (dernier)", "Alias : le plus récent des Opus. Le plus capable pour le code complexe."},
        {"sonnet", "Sonnet (dernier)", "Alias : bon compromis vitesse / qualité."},
        {"haiku", "Haiku (dernier)", "Alias : rapide et économique, idéal pour les petites tâches."},
        {"fable", "Fable (dernier)", "Alias : le modèle Fable le plus récent."},
        {"claude-opus-5-5", "Opus 5.5", "Identifiant complet, figé."},
        {"claude-sonnet-5-5", "Sonnet 5.5", "Identifiant complet, figé."},
        {"claude-haiku-5-5", "Haiku 5.5", "Identifiant complet, figé."},
        {"claude-fable-5-1", "Fable 5.1", "Identifiant complet, figé."},
    };
    return v;
}

const std::vector<Choice>& effortChoices() {
    static const std::vector<Choice> v = {
        {"", "Par défaut", "Effort choisi par Claude Code."},
        {"low", "Faible", "Réponses rapides, peu de réflexion. Pour les questions simples."},
        {"medium", "Moyen", "Équilibré."},
        {"high", "Élevé", "Plus de réflexion avant d'agir."},
        {"xhigh", "Très élevé", "Réflexion approfondie, plus lent et plus coûteux."},
        {"max", "Maximum", "Effort maximal. Consomme beaucoup de quota."},
    };
    return v;
}

const std::vector<Choice>& permissionModeChoices() {
    static const std::vector<Choice> v = {
        {"default", "Normal", "Demande la permission avant chaque action sensible."},
        {"acceptEdits", "Accepter les modifs", "Les modifications de fichiers sont acceptées sans demander."},
        {"auto", "Auto", "Claude décide seul de ce qui est sûr ; demande pour le reste."},
        {"plan", "Plan", "Lecture seule : Claude explore et propose un plan sans rien modifier."},
        {"dontAsk", "Ne pas demander", "Tout ce qui demanderait une permission est refusé automatiquement."},
        {"bypassPermissions", "Sans permissions", "DANGEREUX : tout est autorisé sans confirmation."},
    };
    return v;
}

const std::vector<std::string>& knownTools() {
    static const std::vector<std::string> v = {
        "Bash", "PowerShell", "Read", "Edit", "Write", "Glob", "Grep", "NotebookEdit", "WebFetch", "WebSearch",
        "Task", "Skill", "TodoWrite", "AskUserQuestion", "LSP", "Monitor"};
    return v;
}

}  // namespace claude
