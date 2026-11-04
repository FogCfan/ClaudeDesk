#pragma once
// Transcript d'une conversation, tel qu'affiché et sauvegardé par ClaudeDesk.
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace claude {

enum class EntryKind {
    User,       // message de l'utilisateur
    Assistant,  // texte de Claude
    Thinking,   // réflexion (souvent masquée par l'API)
    ToolUse,    // appel d'outil (+ son résultat)
    System,     // info de ClaudeDesk (redémarrage, erreur, changement de modèle…)
    Error,
};

struct Entry {
    EntryKind kind = EntryKind::System;
    std::string text;            // texte (markdown) ou résumé
    std::string messageId;       // id du message API (regroupement)
    // Outils
    std::string toolName;
    std::string toolUseId;
    std::string toolInput;       // JSON joliment indenté
    std::string toolResult;
    bool toolError = false;
    bool toolDone = false;
    std::string parentToolUseId; // non vide : émis par un sous-agent
    // Méta
    int64_t time = 0;
    bool streaming = false;      // en cours de réception (non sauvegardé)
    int attachments = 0;         // images jointes (messages utilisateur)
};

// Résumé d'une ligne pour un appel d'outil (« Read src/main.cpp », « Bash: git status »…).
std::string toolSummary(const std::string& toolName, const nlohmann::json& input);

nlohmann::json entryToJson(const Entry& e);
Entry entryFromJson(const nlohmann::json& j);

// Conversation au format Markdown (export, récap).
// `maxToolChars` : longueur max conservée pour les entrées/sorties d'outils (0 = les omettre).
std::string transcriptToMarkdown(const std::vector<Entry>& entries, size_t maxToolChars = 400);

}  // namespace claude
