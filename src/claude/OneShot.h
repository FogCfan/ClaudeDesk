#pragma once
// Appel ponctuel et asynchrone de Claude (sans outils, sans persistance) :
// sert à générer les récaps de conversations et de dossiers.
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "claude/Process.h"

namespace claude {

class OneShot {
public:
    std::string label;    // ex. « Récap : Refonte du parser »
    std::string target;   // identifiant libre (id de conversation, chemin de dossier…)
    std::function<void(OneShot&)> onDone;
    std::function<void(const nlohmann::json&)> onEvent;  // mode flux : chaque message JSON
    bool streamJson = false;
    bool slashCommands = false;  // laisse le CLI interpréter les commandes (« /usage » : locale, sans appel au modèle)
    bool defaultTools = false;   // outils par défaut du CLI (sinon aucun outil : --tools "")
    std::string permissionMode;  // --permission-mode (plan, default…) ; vide = celui du CLI
    std::string effort;          // --effort ; vide = celui du CLI
    bool partialMessages = false;  // mode flux : --include-partial-messages (stream_event, texte au fil de l'eau)
    // --thinking-display (option cachée de la CLI, vérifiée en 2.1.332) : « summarized » = résumé de la réflexion
    // envoyé en direct (thinking_delta) ; sans elle, les blocs de réflexion arrivent vides.
    std::string thinkingDisplay;

    void cancel();  // arrête le processus ; onDone est appelé avec l'erreur « Annulé »

    bool start(const std::string& exe, const std::string& model, const std::string& prompt, const std::string& cwd);
    void update();        // appelle onDone quand c'est fini
    bool finished() const { return finished_; }
    bool ok() const { return ok_; }
    const std::string& result() const { return result_; }
    const std::string& error() const { return error_; }
    double costUsd() const { return cost_; }

private:
    std::unique_ptr<Process> process_;
    std::string stdout_, stderr_;
    bool finished_ = false, ok_ = false;
    std::string result_, error_;
    double cost_ = 0;
};

// Prompts de récap.
std::string conversationRecapPrompt(const std::string& title, const std::string& folderPath,
                                    const std::string& transcriptMarkdown);
std::string folderRecapPrompt(const std::string& folderPath, const std::string& recaps);

}  // namespace claude
