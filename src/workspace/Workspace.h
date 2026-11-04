#pragma once
// Espace de travail « cowork » : une arborescence de vrais dossiers sur le disque.
//
//   <racine>/
//     <Projet>/                 _dossier.json (kind=project, cwd, notes, récap)
//       <Thème>/                _dossier.json (kind=folder, notes, récap)
//         <Sous-thème>/ …
//         Titre [ab12cd34].conv.json   une conversation (transcript + récap + stats)
//       Titre [..].conv.json    conversations sur le projet en général
//     .corbeille/               éléments supprimés (récupérables à la main)
//
// Chaque niveau peut contenir des conversations : l'échange sur un sujet précis
// vit dans son thème, l'échange sur le projet entier à la racine du projet.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace ws {

namespace fs = std::filesystem;

struct ConversationMeta {
    std::string id;
    std::string title;
    fs::path file;
    std::string claudeSessionId;
    std::string recap;
    int64_t recapTime = 0;
    int64_t created = 0, updated = 0;
    int messages = 0;
    double costUsd = 0;
    std::string model;
    bool pinned = false;
    std::string status;  // "", "en cours", "terminé", "bloqué"
};

struct Folder {
    std::string name;
    fs::path path;
    bool isProject = false;
    std::string cwd;     // dossier de travail de Claude (projets ; hérité par les sous-dossiers)
    std::string notes;   // notes libres, injectées dans le contexte
    std::string recap;   // synthèse générée des conversations du dossier
    int64_t recapTime = 0;
    std::string icon;    // petite étiquette affichée (« code », « infra »…), optionnel
    Folder* parent = nullptr;
    std::vector<std::unique_ptr<Folder>> children;
    std::vector<ConversationMeta> conversations;

    std::string displayPath() const;  // « Projet › Code › Rendu »
    const Folder* project() const;
    Folder* project();
    std::string effectiveCwd() const;
    int totalConversations() const;
    int64_t lastActivity() const;
};

class Workspace {
public:
    bool open(const fs::path& root);  // crée la racine si besoin puis scanne
    void rescan();
    const fs::path& root() const { return root_; }
    std::vector<std::unique_ptr<Folder>>& projects() { return projects_; }

    Folder* createProject(const std::string& name, const std::string& cwd, std::string* error = nullptr);
    Folder* createFolder(Folder* parent, const std::string& name, std::string* error = nullptr);
    bool renameFolder(Folder* f, const std::string& newName, std::string* error = nullptr);
    bool removeFolder(Folder* f);  // déplacé dans .corbeille
    bool saveFolderMeta(Folder* f);

    // Conversations
    // Écrit la conversation (JSON complet produit par ClaudeSession::toJson, enrichi ici).
    ConversationMeta* saveConversation(Folder* folder, const nlohmann::json& sessionJson, const ConversationMeta* metaIn);
    bool loadConversation(const ConversationMeta& meta, nlohmann::json& out) const;
    bool moveConversation(const std::string& id, Folder* dest);
    bool removeConversation(const std::string& id);
    bool updateConversationMeta(const ConversationMeta& meta);  // titre, récap, statut, épingle

    // Recherche
    Folder* findFolderByPath(const fs::path& p);
    std::pair<Folder*, ConversationMeta*> findConversation(const std::string& id);

    // Contexte injecté dans le prompt système d'une nouvelle session du dossier.
    std::string buildContext(const Folder* folder, const std::string& excludeConversationId = {},
                             size_t maxChars = 40000) const;

private:
    std::unique_ptr<Folder> scanFolder(const fs::path& dir, Folder* parent, bool isProject);
    static void loadFolderMeta(Folder& f);
    static bool readConversationMeta(const fs::path& file, ConversationMeta& m);
    Folder* findFolderRec(Folder* f, const fs::path& p);

    fs::path root_;
    std::vector<std::unique_ptr<Folder>> projects_;
};

std::string conversationFileName(const std::string& title, const std::string& id);
const char* statusLabel(const std::string& status);

}  // namespace ws
