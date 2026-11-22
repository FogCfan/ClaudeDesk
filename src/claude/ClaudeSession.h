#pragma once
// Une conversation pilotée : processus `claude -p` en stream-json bidirectionnel.
// Gère le protocole (messages, streaming, permissions, commandes de contrôle),
// le transcript affiché et les statistiques d'usage.
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "claude/Models.h"
#include "claude/Process.h"
#include "claude/SessionConfig.h"
#include "claude/Transcript.h"
#include "claude/UsageTracker.h"

namespace claude {

enum class SessionState {
    Stopped,   // pas de processus (démarré au premier envoi)
    Starting,  // processus lancé, en attente d'initialisation
    Idle,      // prêt
    Busy,      // un tour est en cours
};

struct PermissionRequest {
    std::string requestId;
    std::string toolName;
    std::string toolUseId;
    std::string description;
    nlohmann::json input;
    nlohmann::json suggestions;  // permission_suggestions (règles « toujours autoriser »)
    bool isQuestion = false;     // AskUserQuestion : l'utilisateur doit choisir des réponses
};

struct SlashCommand {
    std::string name, description, argumentHint;
};

struct ModelInfo {
    std::string value, displayName, description;
    std::vector<std::string> effortLevels;
};

struct ImageAttachment {
    std::string mediaType;  // image/png…
    std::string base64;
    std::string name;
};

class ClaudeSession {
public:
    ClaudeSession();
    ~ClaudeSession();

    // --- Identité / configuration (renseignées par l'application) ---
    std::string localId;          // id de la conversation dans l'espace de travail
    std::string title = "Nouvelle conversation";
    std::string claudeSessionId;  // id de session Claude Code (pour --resume)
    std::string cwd;
    SessionConfig config;
    std::string extraContext;     // contexte « cowork » injecté dans le prompt système
    std::string claudeExe;
    std::string promptDir;        // où écrire le fichier de prompt système

    // --- État observable ---
    std::vector<Entry> entries;
    SessionStats stats;
    SessionState state = SessionState::Stopped;
    std::string statusText;       // « requête… », « réflexion… », nom de l'outil en cours
    int64_t thinkingTokens = 0;
    double turnStartTime = 0;     // secondes (horloge de l'application)
    std::deque<PermissionRequest> permissions;
    std::vector<std::string> stderrLog;
    int queuedMessages = 0;       // messages envoyés pendant un tour en cours

    // Infos remontées par le CLI
    std::vector<std::string> tools;
    std::vector<SlashCommand> commands;
    std::vector<ModelInfo> models;
    std::vector<std::string> outputStyles;
    std::vector<std::pair<std::string, std::string>> mcpServers;  // nom, statut
    std::string activeModel, activePermissionMode, cliVersion, accountPlan;

    bool dirty = false;           // modifié depuis la dernière sauvegarde
    bool restartPending = false;  // config modifiée : redémarrage au prochain tour
    // Le dernier message n'a pas été traité faute de connexion valide (cf. claude/Auth.h) ; effacé au prochain tour réussi.
    bool authFailed = false;
    // Le dernier tour a été refusé à cause du modèle (hors abonnement, inexistant) : carte « changer de modèle ».
    // Effacé au prochain tour réussi.
    ModelAccess modelRefused = ModelAccess::Unknown;
    std::string modelRefusedName;  // modèle demandé

    // Callbacks (optionnels)
    std::function<void(const nlohmann::json&)> onRateLimit;
    std::function<void(ClaudeSession&)> onTurnFinished;
    std::function<void(ClaudeSession&)> onAuthError;  // tour refusé : pas (ou plus) connecté
    std::function<void(ClaudeSession&)> onModelRefused;  // tour refusé à cause du modèle (voir modelRefused)
    // Un vrai modèle a répondu (identifiant résolu, ex. claude-opus-5-5) : l'accès est confirmé.
    std::function<void(ClaudeSession&, const std::string&)> onModelAnswered;

    // --- Actions ---
    void send(const std::string& text, const std::vector<ImageAttachment>& images = {});
    void interrupt();
    void setModel(const std::string& model);
    void setPermissionMode(const std::string& mode);
    // Applique une nouvelle configuration ; redémarre le processus (avec --resume) si besoin.
    void applyConfig(const SessionConfig& cfg);
    void respondPermission(const std::string& requestId, bool allow, const std::string& denyMessage = {},
                           const nlohmann::json& updatedInput = nullptr, bool alwaysAllow = false);
    void stop();
    void restart();               // arrêt + reprise (--resume) au prochain envoi
    // Renvoie le dernier message (après une reconnexion) dans un processus neuf, sans le répéter dans le fil.
    bool resendLast();
    void addSystemNote(const std::string& text, bool error = false);

    // À appeler à chaque image : lit les sorties du processus et met à jour l'état.
    void update(double now);
    bool processRunning() const;

    // Traite une ligne JSON du CLI (public pour les tests).
    void handleLine(const std::string& line, double now = 0);

    // --- Persistance (format ClaudeDesk) ---
    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);

private:
    bool startProcess();
    void writeJson(const nlohmann::json& j);
    void sendControl(const std::string& subtype, nlohmann::json extra = nlohmann::json::object());
    void flushPendingSends();
    static nlohmann::json userMessage(const std::string& text, const std::vector<ImageAttachment>& images);

    void handleAssistant(const nlohmann::json& msg, const std::string& parent);
    void handleUser(const nlohmann::json& msg, const std::string& parent);
    void handleStreamEvent(const nlohmann::json& ev, const std::string& parent);
    void handleControlRequest(const nlohmann::json& j);
    void handleControlResponse(const nlohmann::json& j);
    void handleResult(const nlohmann::json& j, double now);
    void handleInit(const nlohmann::json& j);
    Entry* findTool(const std::string& toolUseId);

    std::unique_ptr<Process> process_;
    std::vector<nlohmann::json> pendingSends_;  // messages en attente du démarrage
    int controlCounter_ = 0;
    std::string initRequestId_;
    double lastProcessCost_ = 0;   // total_cost_usd est cumulatif par processus
    std::string lastUsageMsgId_;   // un message assistant arrive en plusieurs événements (un par bloc)
    bool sawInit_ = false;
    bool triedResume_ = false;
    std::string lastUserText_;     // pour relancer sans --resume si la session est introuvable
    std::vector<ImageAttachment> lastUserImages_;
    int streamingEntry_ = -1;      // index de l'entrée en cours de streaming
    double lastNow_ = 0;
    bool freshPrompt_ = false;     // prompt système modifié : ne pas réutiliser l'instantané
    ModelAccess modelErrorPending_ = ModelAccess::Unknown;  // message assistant « modèle refusé », résultat à venir
    bool authErrorPending_ = false;  // message assistant « authentication_failed » reçu, résultat à venir
};

const char* stateLabel(SessionState s);

}  // namespace claude
