#pragma once
// Orchestrateur : une conversation « cerveau », à la racine d'un projet cowork,
// pilote les conversations des agents du projet (un thème = un agent) jusqu'à ce
// que l'objectif soit atteint.
//
//   cerveau ──@@DELEGUER agent──▶ agents (en parallèle, limité)
//      ▲                              │
//      └──── comptes rendus ◀─────────┘   … cycle suivant, jusqu'à @@TERMINE
//
// Logique pure (sans SDL ni ImGui) : l'application fournit un OrchestratorHost
// qui ouvre les conversations, envoie les messages et donne leur état.
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "claude/Transcript.h"

namespace claude {

// --- Directives émises par le cerveau à la fin de chaque réponse ---------------
struct Delegation {
    std::string agent;
    std::string task;
};

struct Directives {
    std::vector<Delegation> delegations;
    bool finished = false;
    std::string summary;      // @@TERMINE
    bool needHuman = false;
    std::string question;     // @@HUMAIN
    bool empty() const { return delegations.empty() && !finished && !needHuman; }
};

// Analyse les blocs « @@DELEGUER <agent> / @@HUMAIN / @@TERMINE … @@FIN ».
Directives parseDirectives(const std::string& text);
// Retrouve un agent par son nom exact, son numéro (« 02 ») ou un fragment unique ; -1 sinon.
int matchAgent(const std::string& name, const std::vector<std::string>& agents);
// Texte produit par Claude depuis le dernier message utilisateur (réponse du tour).
std::string lastTurnText(const std::vector<Entry>& entries);

// --- Interface avec l'application ---------------------------------------------
struct ParticipantStatus {
    bool exists = false;      // conversation ouverte
    bool busy = false;        // tour en cours (ou démarrage)
    bool needsHuman = false;  // permission / question en attente
    bool stopped = false;     // aucun processus (arrêté ou planté)
    int turns = 0;            // tours terminés (stats)
    double costUsd = 0;       // coût cumulé de la conversation
    std::string lastText;     // réponse du dernier tour
};

struct AgentInfo {
    std::string name;   // nom du thème
    std::string brief;  // résumé de son rôle (notes du dossier)
};

class OrchestratorHost {
public:
    virtual ~OrchestratorHost() = default;
    virtual std::vector<AgentInfo> agents() = 0;
    // Ouvre (ou crée) la conversation du cerveau / d'un agent ; renvoie son id ("" = échec).
    virtual std::string openBrain(const std::string& convHint, const std::string& systemPrompt) = 0;
    virtual std::string openAgent(const std::string& agent, const std::string& convHint) = 0;
    virtual bool ensureOpen(const std::string& convId) = 0;
    // Envoie un message (en appliquant le mode de permission) ; false = échec.
    virtual bool send(const std::string& convId, const std::string& text, const std::string& permissionMode) = 0;
    virtual ParticipantStatus status(const std::string& convId) = 0;
    virtual double fiveHourUtilization() = 0;  // 0..1 ; < 0 = inconnu
    virtual int64_t fiveHourResetsAt() = 0;    // secondes Unix ; 0 = inconnu
    virtual int64_t now() = 0;                 // secondes Unix
    virtual void onEvent(const std::string& /*text*/, bool /*important*/) {}
};

// --- Moteur -------------------------------------------------------------------
struct OrchestratorSettings {
    std::string objective;
    int maxCycles = 25;
    double maxCostUsd = 25.0;           // 0 = illimité
    int maxParallel = 3;
    std::string workerMode = "auto";    // mode de permission des agents
    std::string brainMode = "auto";     // mode de permission du cerveau
    double usagePause = 0.90;           // pause si la fenêtre de 5 h dépasse ce taux (0 = jamais)
};

enum class OrchState {
    Idle,          // jamais lancé
    Thinking,      // le cerveau réfléchit
    Working,       // les agents travaillent
    WaitingHuman,  // le cerveau a posé une question (@@HUMAIN)
    Paused,        // pause (utilisateur, budget, cycles, incident)
    UsagePause,    // pause automatique : limite de 5 h proche
    Finished,      // @@TERMINE
};
const char* orchStateLabel(OrchState s);

struct OrchTask {
    std::string agent, task, convId;
    int cycle = 0;
    int turnsAtSend = 0;
    double costAtSend = 0;
    bool sent = false, done = false, failed = false, notifiedHuman = false;
    std::string report;
    int64_t sentAt = 0, doneAt = 0;
};

struct OrchLogEntry {
    int64_t time = 0;
    std::string text;
    bool important = false;
};

class Orchestrator {
public:
    OrchestratorSettings settings;
    OrchState state = OrchState::Idle;
    std::string pauseReason;
    int cycle = 0;
    double spentUsd = 0;
    std::string brainConvId;
    std::map<std::string, std::string> agentConv;  // agent -> conversation utilisée
    std::vector<OrchTask> tasks;                   // tâches du cycle en cours
    std::deque<OrchLogEntry> log;                  // plus récent en dernier
    std::string finalSummary, humanQuestion;
    bool changed = false;                          // à sauvegarder

    // Nouveau départ avec settings.objective (réinitialise cycles, dépense, journal).
    bool start(OrchestratorHost& h);
    void pause(const std::string& reason);
    void resume(OrchestratorHost& h);
    void stop();  // arrêt définitif (les tours en cours continuent dans leurs conversations)
    void tick(OrchestratorHost& h);

    bool running() const;  // Thinking / Working / WaitingHuman / UsagePause
    OrchState resumeState() const { return resumeState_; }

    static std::string systemPrompt();
    std::string firstMessage(const std::vector<AgentInfo>& agents) const;

    nlohmann::json toJson() const;
    // Une orchestration rechargée en cours de route est mise en pause (reprise manuelle).
    void fromJson(const nlohmann::json& j);

private:
    bool sendBrain(OrchestratorHost& h, const std::string& text);
    void handleBrainAnswer(OrchestratorHost& h, const std::string& text);
    void tickThinking(OrchestratorHost& h);
    void tickWorking(OrchestratorHost& h);
    void dispatch(OrchestratorHost& h);
    bool budgetOk(OrchestratorHost& h);
    bool usageOk(OrchestratorHost& h);
    std::string buildReport() const;
    void addLog(OrchestratorHost* h, const std::string& text, bool important = false);
    void pauseFrom(OrchState from, const std::string& reason, OrchestratorHost* h);

    OrchState resumeState_ = OrchState::Idle;
    int brainTurnsAtSend_ = 0;
    double brainCostAtSend_ = 0;
    std::string lastBrainMessage_;
    int nudges_ = 0;
    bool brainHumanNotified_ = false;
};

}  // namespace claude
