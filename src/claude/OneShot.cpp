#include "claude/OneShot.h"

#include <nlohmann/json.hpp>

#include "util/Strings.h"

namespace claude {

bool OneShot::start(const std::string& exe, const std::string& model, const std::string& prompt, const std::string& cwd) {
    process_ = std::make_unique<Process>();
    std::vector<std::string> args = {"-p", "--output-format", streamJson ? "stream-json" : "json",
                                     "--no-session-persistence", "--tools", "", "--disable-slash-commands"};
    if (streamJson) args.push_back("--verbose");
    if (!model.empty()) args.insert(args.end(), {"--model", model});
    if (!process_->start(exe, args, cwd)) {
        finished_ = true;
        error_ = process_->error();
        if (onDone) onDone(*this);
        return false;
    }
    process_->write(prompt);
    process_->closeStdin();
    return true;
}

void OneShot::update() {
    if (finished_ || !process_) return;
    auto consume = [&] {
        for (auto& l : process_->poll()) {
            if (l.isStderr) {
                stderr_ += l.text + "\n";
            } else if (streamJson) {
                auto j = nlohmann::json::parse(l.text, nullptr, false);
                if (!j.is_object()) continue;
                if (j.value("type", "") == "result") stdout_ = l.text;
                if (onEvent) onEvent(j);
            } else {
                stdout_ += l.text + "\n";
            }
        }
    };
    consume();
    if (process_->running()) return;
    consume();
    process_->shutdown(0);
    finished_ = true;
    auto j = nlohmann::json::parse(stdout_, nullptr, false);
    if (j.is_object()) {
        result_ = j.value("result", "");
        cost_ = j.value("total_cost_usd", 0.0);
        ok_ = !j.value("is_error", false) && !result_.empty();
        if (!ok_) error_ = result_.empty() ? "Réponse vide" : result_;
    } else {
        error_ = util::trim(stderr_.empty() ? stdout_ : stderr_);
        if (error_.empty()) error_ = "Claude Code n'a rien renvoyé.";
    }
    if (onDone) onDone(*this);
}

std::string conversationRecapPrompt(const std::string& title, const std::string& folderPath,
                                    const std::string& transcript) {
    return "Tu prépares le récapitulatif d'une conversation de travail entre un développeur et Claude Code, "
           "pour qu'il puisse reprendre le travail plus tard (ou qu'une autre session reparte de ce contexte).\n"
           "Conversation : « " + title + " » — rangée dans : " + folderPath + ".\n\n"
           "Rédige en français, en Markdown concis, avec exactement ces sections :\n"
           "### Objectif\n### Ce qui a été fait\n### Décisions et points importants\n"
           "### Fichiers concernés\n### Reste à faire\n\n"
           "Sois factuel, cite les noms de fichiers/fonctions exacts, pas de phrases d'introduction. "
           "Réponds uniquement avec le récap.\n\n"
           "<transcript>\n" + util::truncateUtf8(transcript, 120000) + "\n</transcript>";
}

std::string folderRecapPrompt(const std::string& folderPath, const std::string& recaps) {
    return "Voici les récapitulatifs des conversations de travail rangées dans le dossier « " + folderPath +
           " » d'un projet. Fais-en une synthèse en français, en Markdown concis :\n"
           "### État actuel\n### Décisions structurantes\n### Problèmes ouverts / reste à faire\n\n"
           "Réponds uniquement avec la synthèse.\n\n<recaps>\n" + util::truncateUtf8(recaps, 120000) + "\n</recaps>";
}

}  // namespace claude
