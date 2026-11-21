#pragma once
// Orchestrateur : une conversation « cerveau », à la racine d'un projet cowork,
// pilote les conversations des agents du projet (un thème = un agent) jusqu'à ce
// que l'objectif soit atteint.
//
//   cerveau ──@@DELEGUER agent | modele=… effort=…──▶ agents (en parallèle, limité)
//      ▲                                                  │
//      └──────────────── comptes rendus ◀─────────────────┘   … jusqu'à @@TERMINE
//
// Budget : en dollars (API à la carte) ou en part du quota de l'abonnement
// (fenêtre de 5 h, mesurée par les rate_limit_event). En mode « strict », le
// cerveau est prévenu à l'approche de la limite puis doit conclure.
//
// Logique pure (sans SDL ni ImGui) : l'application fournit un OrchestratorHost.
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
    std::string model;   // demandé par le cerveau (« opus »…), vide = défaut
    std::string effort;  // low … max, vide = défaut
};

struct HumanQuestion {
    std::string title;                     // intitulé court (première ligne, sans Markdown)
    std::string text;                      // la question complète, sans les options (Markdown)
    std::vector<std::string> options;      // choix cliquables, éventuellement vide
    std::vector<std::string> suggestions;  // réponses suggérées dans le texte (« réponds « … » »)
};

struct Directives {
    std::vector<Delegation> delegations;
    bool finished = false;
    std::string summary;                   // @@TERMINE
    bool wait = false;                     // @@ATTENDRE : attendre les comptes rendus des agents encore au travail
    bool pause = false;                    // @@PAUSE : suspendre jusqu'à la prochaine session de 5 h
    std::string pauseNote;                 //   état + plan de reprise (aussi : un @@HUMAIN qui n'est qu'une annonce de pause)
    bool needHuman = false;
    std::string question;                  // @@HUMAIN : texte brut complet
    std::vector<HumanQuestion> questions;  // @@HUMAIN : une entrée par bloc, options détectées
    bool empty() const { return delegations.empty() && !finished && !needHuman && !pause; }
};

// Un bloc @@HUMAIN qui n'est en réalité qu'une annonce de pause (« Orchestration en pause : quota presque épuisé…
// Relance-moi après la réinitialisation ») : aucune question, aucun choix, et un vocabulaire de pause.
bool looksLikePauseNotice(const std::string& body);

// Analyse les blocs « @@DELEGUER <agent> [| modele=… effort=…] / @@HUMAIN / @@TERMINE / @@ATTENDRE … @@FIN ».
Directives parseDirectives(const std::string& text);
// Sépare une question de ses options. Sont des options : les cases « - [ ] … » où qu'elles soient,
// sinon une liste finale d'au moins deux puces / numéros de premier niveau (les sous-puces
// indentées et les listes suivies de texte restent du contexte).
HumanQuestion splitQuestion(const std::string& body);
// Un bloc @@HUMAIN peut contenir plusieurs questions numérotées (« 1. **Licences…** … 2. … ») :
// elles sont séparées ; le texte qui précède la première sert de contexte.
std::vector<HumanQuestion> splitQuestions(const std::string& body);
// Retrouve un agent par son nom exact, son numéro (« 02 ») ou un fragment unique ; -1 sinon.
int matchAgent(const std::string& name, const std::vector<std::string>& agents);
// Texte produit par Claude depuis le dernier message utilisateur (réponse du tour).
std::string lastTurnText(const std::vector<Entry>& entries);

// Découpe une réponse du cerveau pour l'affichage : texte libre et blocs de directives.
struct TextSegment {
    enum Kind { Text, Delegate, Human, Done, Wait, Pause } kind = Text;
    std::string header;  // après le mot-clé (« 02 Build | modele=… »)
    std::string body;
};
std::vector<TextSegment> splitDirectiveSegments(const std::string& text);

// Message envoyé au cerveau par ClaudeDesk (mission, comptes rendus, relances…) : son
// intitulé court (« Comptes rendus du cycle 3 »), ou "" pour un message écrit par l'humain.
std::string orchestratorMessageTitle(const std::string& userText);

// Gamme de modèles : 1 haiku, 2 sonnet, 3 opus, 4 fable, 0 inconnu.
int modelTier(const std::string& model);
// Ramène un modèle demandé dans la liste autorisée (vide = tous). Si la liste est
// restreinte, un modèle vide ou inconnu devient le plus capable des modèles autorisés.
std::string clampModel(const std::string& requested, const std::vector<std::string>& allowed);
// Plafonne l'effort (low < medium < high < xhigh < max) ; inconnu -> "".
std::string clampEffort(const std::string& requested, const std::string& maxEffort);

// --- Interface avec l'application ---------------------------------------------
struct ParticipantStatus {
    bool exists = false;      // conversation ouverte
    bool busy = false;        // tour en cours (ou démarrage)
    bool needsHuman = false;  // permission / question en attente
    bool stopped = false;     // aucun processus (arrêté ou planté)
    int turns = 0;            // tours terminés (stats)
    double costUsd = 0;       // coût cumulé de la conversation
    double context = 0;       // occupation de la fenêtre de contexte (0..1), 0 si inconnue
    std::string lastText;     // réponse du dernier tour
};

struct AgentInfo {
    std::string name;   // nom du thème
    std::string brief;  // résumé de son rôle (notes du dossier)
};

struct SendOptions {
    std::string permissionMode;
    std::string model;   // vide = inchangé
    std::string effort;  // vide = inchangé
};

class OrchestratorHost {
public:
    virtual ~OrchestratorHost() = default;
    virtual std::vector<AgentInfo> agents() = 0;
    // Ouvre (ou crée) la conversation du cerveau / d'un agent ; renvoie son id ("" = échec).
    virtual std::string openBrain(const std::string& convHint, const std::string& systemPrompt) = 0;
    virtual std::string openAgent(const std::string& agent, const std::string& convHint) = 0;
    virtual bool ensureOpen(const std::string& convId) = 0;
    // Envoie un message (après avoir appliqué mode, modèle, effort) ; false = échec.
    virtual bool send(const std::string& convId, const std::string& text, const SendOptions& opt) = 0;
    virtual ParticipantStatus status(const std::string& convId) = 0;
    // Abonnement : fenêtres de limite (« five_hour », « seven_day »).
    virtual double utilization(const std::string& window) = 0;  // 0..1 ; < 0 = inconnu
    virtual int64_t resetsAt(const std::string& window) = 0;    // secondes Unix ; 0 = inconnu
    virtual bool isSubscription() = 0;                          // compte avec abonnement détecté
    virtual std::string planName() { return {}; }               // « Claude Max »…
    virtual int64_t now() = 0;                                  // secondes Unix
    virtual void onEvent(const std::string& /*text*/, bool /*important*/) {}
    // Rotation de contexte : récap de la conversation puis nouvelle conversation dans le même dossier (la mémoire
    // cowork — notes, synthèse, récaps — y est injectée). Renvoie "" tant que le récap est en cours, puis l'id de la
    // nouvelle conversation. `agent` vide = la conversation de l'orchestrateur.
    virtual std::string rotate(const std::string& /*agent*/, const std::string& /*convId*/) { return {}; }
};

// --- Moteur -------------------------------------------------------------------
struct OrchestratorSettings {
    std::string objective;
    int maxCycles = 25;
    int maxParallel = 3;
    // Comptes rendus au fil de l'eau : dès qu'un agent termine alors que d'autres travaillent encore (et qu'une place
    // se libère), le cerveau reçoit un compte rendu intermédiaire et peut relancer aussitôt les agents libres.
    bool rollingReports = true;
    std::string workerMode = "auto";   // mode de permission des agents
    std::string brainMode = "auto";    // mode de permission du cerveau

    // Budget
    std::string budgetMode = "auto";   // auto (détecté), subscription, api
    double maxCostUsd = 25.0;          // API : dollars (0 = illimité)
    double sessionShare = 0.5;         // abonnement : part de ce qui RESTE dans la session de 5 h en cours (avant le
                                       // plafond), renouvelée à chaque nouvelle session ; 0 = sans limite
    double usagePause = 0.90;          // plafond fenêtre 5 h : pause puis reprise à la réinitialisation (0 = jamais)
    double weeklyPause = 0.95;         // plafond hebdomadaire (0 = jamais)
    bool strictBudget = true;          // « résultat dans le budget » : phase finale puis conclusion obligatoire
    double budgetTolerance = 0.15;     // dépassement toléré au-delà de la cible (part du budget) : finir sans bâcler

    // Modèles
    std::vector<std::string> allowedModels = {"haiku", "sonnet", "opus", "fable"};
    std::string maxEffort;             // plafond d'effort ("" = aucun)
    std::string defaultWorkerModel;    // si le cerveau n'en précise pas ("" = celui de la conversation)
    std::string defaultWorkerEffort;
    std::string brainModel;            // modèle / effort de l'orchestrateur ("" = inchangés)
    std::string brainEffort;

    // Contexte : comme le reste de l'application, on allège régulièrement (récap + nouvelle conversation)
    bool contextRotation = true;
    double rotateAtContext = 0.45;     // part de la fenêtre de contexte au-delà de laquelle on repart à neuf
    int rotateAfterTasks = 6;          // ou après ce nombre de tâches dans la même conversation (0 = jamais)
};

enum class OrchState {
    Idle,          // jamais lancé
    Thinking,      // le cerveau réfléchit
    Working,       // les agents travaillent
    WaitingHuman,  // le cerveau a posé une question (@@HUMAIN)
    Paused,        // pause (utilisateur, budget, cycles, incident)
    UsagePause,    // pause automatique : plafond d'usage de l'abonnement
    Finished,      // @@TERMINE
};
const char* orchStateLabel(OrchState s);

// --- Niveaux de budget (réglages simples) --------------------------------------
// Chaque niveau fixe d'un coup le budget, les modèles autorisés, l'effort, le
// parallélisme et les cycles ; les réglages avancés restent modifiables ensuite
// (l'orchestration est alors « personnalisée »).
enum class BudgetPreset { Low, Medium, High, Max };
constexpr BudgetPreset kBudgetPresets[] = {BudgetPreset::Low, BudgetPreset::Medium, BudgetPreset::High, BudgetPreset::Max};
const char* budgetPresetLabel(BudgetPreset p, bool subscription);   // « Bas budget »… « Max abonnement »
// Ce que le niveau implique, en deux lignes. `remaining` : part de la session encore utilisable (0..1), -1 si inconnue.
std::string budgetPresetSummary(BudgetPreset p, bool subscription, double remaining = -1);
void applyBudgetPreset(OrchestratorSettings& s, BudgetPreset p);     // ne touche ni à l'objectif ni aux modes
bool budgetPresetMatches(const OrchestratorSettings& s, BudgetPreset p);

struct OrchTask {
    std::string agent, task, convId;
    std::string model, effort;  // demandés puis réellement appliqués
    bool modelChosen = false;   // le cerveau a choisi le modèle (sinon : défaut des agents)
    int cycle = 0;
    int turnsAtSend = 0;
    double costAtSend = 0;
    bool sent = false, done = false, failed = false, notifiedHuman = false;
    bool reported = false;      // compte rendu transmis au cerveau
    std::string report;
    int64_t sentAt = 0, doneAt = 0;
};

struct OrchLogEntry {
    int64_t time = 0;
    std::string text;
    bool important = false;
};

struct QaEntry {
    int64_t time = 0;
    std::string question, answer;
};

class Orchestrator {
public:
    OrchestratorSettings settings;
    OrchState state = OrchState::Idle;
    std::string pauseReason;
    int cycle = 0;
    double spentUsd = 0;           // coût cumulé rapporté par Claude Code (équivalent API sous abonnement)
    double usageSpent = 0;         // part de fenêtre de 5 h consommée depuis le départ, toutes sessions confondues
    // Abonnement : budget de la session de 5 h en cours
    // windowBase : usage de la fenêtre qui ne revient pas à l'orchestration (au départ, puis consommation faite
    // ailleurs pendant qu'elle ne travaille pas) ; -1 : inconnu. Ce qui reste = plafond − windowBase − windowSpent.
    double windowBase = -1;
    double windowSpent = 0;        // part de fenêtre consommée PAR l'orchestration dans cette session
    // Part allouée pour cette session : sessionShare × (plafond − windowBase). Toujours à jour (niveau, plafond, usage).
    double sessionAllowance() const;
    // « Relancer le budget » : compteur à 0, même limite qu'avant — plafonnée par ce qui reste réellement dans la
    // session (abonnement) ; en API, compteur en dollars remis à 0 avec la même limite.
    void resetBudget(OrchestratorHost& h);
    double budgetSpentUsd() const { return spentUsd - costBaseline_; }  // API : dépense comptée dans le budget
    double ceiling() const { return settings.usagePause > 0 ? settings.usagePause : 1.0; }
    bool subscription = false;     // budget effectif : abonnement (sinon dollars)
    std::string brainConvId;
    std::map<std::string, std::string> agentConv;  // agent -> conversation utilisée
    std::vector<OrchTask> tasks;                   // tâches en cours, en file, ou terminées pas encore traitées par le cerveau
    std::deque<OrchLogEntry> log;                  // plus récent en dernier
    std::string finalSummary;
    std::string humanQuestion;                     // texte brut de la dernière question
    std::vector<HumanQuestion> questions;          // questions en attente (structurées)
    int questionSeq = 0;                           // change à chaque nouvelle question
    std::deque<QaEntry> qaHistory;                 // questions posées et vos réponses
    // Pause décidée par l'orchestrateur (@@PAUSE) : son message, et la réinitialisation de la fenêtre de 5 h
    // attendue pour reprendre seul (0 = inconnue : reprise manuelle, ou dès qu'elle est connue).
    std::string pauseNote;
    int64_t pauseUntil = 0;
    bool brainPaused() const { return !pauseNote.empty() && (state == OrchState::UsagePause || state == OrchState::Paused); }
    bool changed = false;                          // à sauvegarder

    // Nouveau départ avec settings.objective (réinitialise cycles, dépense, journal).
    bool start(OrchestratorHost& h);
    void pause(const std::string& reason);
    void resume(OrchestratorHost& h);
    void stop();  // arrêt définitif (les tours en cours continuent dans leurs conversations)
    void tick(OrchestratorHost& h);
    // Réponse de l'utilisateur à une question @@HUMAIN (envoyée au cerveau).
    bool answerHuman(OrchestratorHost& h, const std::string& answer);

    bool running() const;  // Thinking / Working / WaitingHuman / UsagePause
    OrchState resumeState() const { return resumeState_; }
    bool finalPhase() const { return finalRequested_; }
    int tasksInFlight() const;  // tâches envoyées non terminées + tâches en file

    // Part du budget consommée (0..1+), -1 si illimité.
    double budgetFraction() const;
    std::string budgetLabel() const;  // « 12 % / 40 % de la fenêtre 5 h » ou « 3,20 $ / 25,00 $ »
    static bool resolveSubscription(const OrchestratorSettings& s, bool hostSubscription);

    static std::string systemPrompt();
    std::string firstMessage(const std::vector<AgentInfo>& agents, const std::string& planName = {}) const;

    nlohmann::json toJson() const;
    // Une orchestration rechargée en cours de route est mise en pause (reprise manuelle).
    void fromJson(const nlohmann::json& j);

private:
    bool sendBrain(OrchestratorHost& h, const std::string& text);
    void handleBrainAnswer(OrchestratorHost& h, const std::string& text);
    void tickThinking(OrchestratorHost& h);
    void tickWorking(OrchestratorHost& h);
    void collectTasks(OrchestratorHost& h);   // relève les tâches terminées (quel que soit l'état du cerveau)
    void reviveTasks(OrchestratorHost& h);    // rouvre les conversations des tâches en vol ; renvoie celles interrompues
    bool sendReport(OrchestratorHost& h, bool partial);
    bool partialReportUseful(OrchestratorHost& h) const;
    bool canDispatch() const {
        return state == OrchState::Thinking || state == OrchState::Working || state == OrchState::WaitingHuman;
    }
    void trackUsage(OrchestratorHost& h);
    void dispatch(OrchestratorHost& h);
    // Arrêt ferme : cible + tolérance. Entre les deux, on termine proprement ce qui est en cours.
    bool exhausted() const { double f = budgetFraction(); return f >= 1.0 + std::max(0.0, settings.budgetTolerance); }
    bool usageOk(OrchestratorHost& h);
    std::string budgetNotes() const;
    // Comptes rendus non encore transmis + agents toujours au travail ; `partial` : d'autres agents travaillent encore.
    std::string buildReport(bool partial, int64_t now) const;
    std::string progressSection(int64_t now) const;
    void markReported();
    void addLog(OrchestratorHost* h, const std::string& text, bool important = false);
    void pauseFrom(OrchState from, const std::string& reason, OrchestratorHost* h);
    void budgetPause(OrchestratorHost& h);
    void brainPause(OrchestratorHost& h, const std::string& note);
    std::string resumeAfterPauseMessage() const;

    OrchState resumeState_ = OrchState::Idle;
    int brainTurnsAtSend_ = 0;
    double brainCostAtSend_ = 0;
    std::string lastBrainMessage_;
    int nudges_ = 0;
    bool brainHumanNotified_ = false;
    bool finalRequested_ = false;
    double lastFiveHour_ = -1;
    double lastActiveAt_ = -1e18;  // dernier instant où l'orchestrateur ou un agent travaillait
    int64_t windowResetsAt_ = 0;
    std::map<std::string, int> convTasks_;  // tâches confiées par conversation (rotation)
    bool needsRotation(const ParticipantStatus& st, const std::string& convId) const;
    std::string brainRefresher() const;      // rappel de l'objectif et de l'équipe pour un cerveau tout neuf
    bool reviveOnTick_ = false;              // rechargée avec une question en attente et des tâches en vol
    double allowanceOverride_ = -1;  // limite figée par « Relancer le budget » (-1 : calcul normal)
    double overrideShare_ = -1;      // niveau en vigueur lors de la relance (un autre niveau annule la limite figée)
    double costBaseline_ = 0;        // API : dépense déjà comptée avant la dernière relance
    bool teamActive(OrchestratorHost& h);
    std::string usageWindow_;  // fenêtre responsable de la pause d'usage
    // Dernier état connu du quota (pour informer le cerveau)
    double quota5h_ = -1, quota7d_ = -1;
    int64_t reset5h_ = 0, snapNow_ = 0;
    void snapshotQuota(OrchestratorHost& h);
    void beginWindow(double usage);  // nouvelle session de 5 h : budget recalculé sur ce qui reste
};

}  // namespace claude
