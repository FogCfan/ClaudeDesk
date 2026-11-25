#pragma once
// Application ClaudeDesk : fenêtre SDL3 + Dear ImGui (docking), onglets de
// conversations, panneaux d'usage / configuration / espace de travail.
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "app/Settings.h"
#include "claude/Auth.h"
#include "claude/ClaudeSession.h"
#include "claude/Designer.h"
#include "claude/History.h"
#include "claude/OneShot.h"
#include "claude/Orchestrator.h"
#include "claude/ProjectKind.h"
#include "claude/UsageTracker.h"
#include "ui/Markdown.h"
#include "workspace/Workspace.h"

struct SDL_Window;
struct SDL_Renderer;
struct ImFont;
struct ImGuiContext;

namespace app {

// Cache de rendu d'une entrée (markdown analysé + hauteur mesurée).
struct EntryCache {
    double appear = -1;   // 0 : à animer à la première apparition ; >0 : instant d'apparition ; -1 : pas d'animation
    size_t textLen = (size_t)-1;
    bool streaming = false;
    std::vector<ui::MdBlock> blocks;
    float height = 0, width = 0;
    // Discussion de l'orchestrateur : réponse découpée en texte libre / directives (cartes).
    size_t segLen = (size_t)-1;
    std::vector<claude::TextSegment> segs;
    std::vector<std::vector<ui::MdBlock>> segBlocks;
};

// Polices d'un contexte ImGui (chaque fenêtre système a son propre atlas).
struct FontSet {
    ImFont* italic = nullptr;
    ImFont* ui = nullptr;
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;
};

// Deuxième fenêtre système (tableau de bord détaché) : son propre contexte ImGui.
struct SecondaryWindow {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    ImGuiContext* ctx = nullptr;
    FontSet fonts;
    bool closeRequested = false;
    int themeApplied = -1;
    int fontRendererApplied = -1;
};

// Journal d'activité, toutes conversations confondues (tableau de bord).
enum class ActivityKind { Info, Tool, Attention, Done, Error };
struct ActivityEvent {
    int64_t time = 0;
    std::string convId;
    std::string title;
    std::string text;
    ActivityKind kind = ActivityKind::Info;
};

struct OpenConversation {
    std::unique_ptr<claude::ClaudeSession> session;
    std::string folderPath;            // dossier de l'espace de travail (UTF-8)
    ws::ConversationMeta meta;         // récap, statut, épingle…
    bool injectContext = true;
    // Onglet fermé alors qu'un agent de l'orchestrateur y travaille : la conversation continue sans onglet
    // (l'orchestrateur la suit toujours) ; elle revient si on la rouvre, et se libère une fois inutile.
    bool hiddenTab = false;

    // Éditeur
    std::string draft;
    std::vector<claude::ImageAttachment> attachments;
    bool focusEditor = true;
    bool cursorToEnd = false;
    std::string replaceDraft;          // remplacement demandé du texte de l'éditeur (via callback)
    bool hasReplace = false;
    int historyIndex = -1;
    float editorHeight = 130.0f;
    float quickBarHeight = 0;           // hauteur mesurée de la barre sous le fil (puces qui passent à la ligne)
    float permPanelHeight = 0;
    int slashSelection = 0;

    // Vue
    int hoveredEntry = -1;
    float scrollTarget = 1e9f;          // défilement fluide : position visée
    float scrollSetLast = -1;           // dernière position imposée (détecte la barre de défilement)
    bool scrollInit = false;
    bool scrollToBottom = true;
    bool stickToBottom = true;
    std::string search;
    bool showSearch = false;
    bool focusSearch = false;
    std::set<int> selectable;          // entrées affichées en mode « sélection de texte » (clés < 0 : « tout afficher » des outils)
    bool hasTextSelection() const { return !selectable.empty() && *selectable.rbegin() >= 0; }
    void clearTextSelection() { selectable.erase(selectable.lower_bound(0), selectable.end()); }
    std::set<int> expanded;            // outils / réflexions dépliés
    std::set<int> collapsedOverride;   // outils repliés alors que « tout déplier » est actif
    std::vector<EntryCache> cache;
    double lastSave = 0;
    bool wasBusy = false;

    // Tableau de bord
    std::string dashDraft;             // réponse rapide depuis la carte
    size_t seenEntries = (size_t)-1;   // pour le journal d'activité (-1 : pas encore initialisé)
    size_t seenPermissions = 0;
    bool focusDashDraft = false;

    // Permission / question en cours d'édition
    std::string denyReason;
    std::unordered_map<std::string, std::string> answers;        // question -> réponse
    std::unordered_map<std::string, std::string> otherAnswers;   // question -> texte « Autre »
};

struct Toast {
    std::string text;
    double until;
    bool error;
    double start = 0;
};

class App {
public:
    int run(int argc, char** argv);
    // Redessine pendant le redimensionnement en direct (boucle modale de Windows), appelé depuis un event watch SDL.
    // Tick : minuteur de la boucle modale (dessine toujours) ; Resize : nouvelle taille (dessine si elle change) ;
    // Move : fenêtre déplacée (dessine à la cadence choisie).
    enum class LiveEvent { Tick, Resize, Move };
    void onLiveResize(uint32_t windowId, LiveEvent kind);

private:
    // --- Cycle de vie ---
    bool init();
    void shutdown();
    FontSet loadFonts();
    void applyFontLoader(bool force);
    void applyVSync();
    void restartApp();
    void useFonts(const FontSet& f);
    void applyTheme();
    void frame();
    bool inFrame_ = false;
    int drawnW_ = 0, drawnH_ = 0;  // taille (pixels) de la dernière image dessinée
    uint64_t frameStartNs_ = 0;    // début de la dernière image (cadence pendant un déplacement)
    void buildDefaultLayout(unsigned int dockId);
    void processAsyncResults();

    // --- Panneaux (ui/*.cpp) ---
    void drawMenuBar();                       // AppMenus.cpp
    void drawStatusBar();                     // AppMenus.cpp
    void drawShortcutsWindow();               // AppMenus.cpp
    void drawAboutWindow();                   // AppMenus.cpp
    void drawLogWindow();                     // AppMenus.cpp
    void drawNewProjectPopup();               // WorkspacePanel.cpp
    void drawWorkspacePanel();                // WorkspacePanel.cpp
    void drawFolderNode(ws::Folder* f);       // WorkspacePanel.cpp
    // Ligne d'une conversation. Session d'une suite : `session` = son rang (1 = la plus ancienne) sur `sessions` ;
    // la dernière d'une suite se déplie sur les sessions précédentes : renvoie true si dépliée (TreePop à faire).
    // `label` remplace le titre affiché. Renvoie false (sans TreePop à faire) si la conversation a été supprimée.
    bool drawConversationItem(ws::Folder* f, ws::ConversationMeta& m, int session = 0, int sessions = 0,
                              const std::string& label = {});
    // Une discussion (suite de sessions) : la session actuelle, dépliable sur les précédentes. Renvoie false si le
    // dossier a changé pendant le dessin (suppression, déplacement) : arrêter l'itération.
    bool drawChain(ws::Folder* f, const ws::ConversationChain& c, const std::string& label = {});
    // Projet orchestré : le nœud « Orchestrateur » et, en dessous, ses agents (thèmes) puis sa discussion.
    void drawOrchestratorNode(ws::Folder* project);
    void openProjectOrchestrator(ws::Folder* project);
    void drawInspector();                     // WorkspacePanel.cpp
    void drawMoveMenu(const std::string& convId, ws::Folder* f);
    void drawHistoryImport();                 // WorkspacePanel.cpp
    // Panneau Projets : icônes, fichiers, explorateur du dossier de travail (WorkspacePanel.cpp)
    void drawWorkDirNode(ws::Folder* project);
    void drawDirListing(const std::string& dir, int depth);
    void drawFileItem(const std::string& path, ws::Folder* owner);  // owner : dossier de projet (null = dossier de travail)
    void drawFileInspector();
    void drawIconLegend();
    void addFilesTo(const std::string& folderPath, const std::vector<std::string>& files);
    bool dropOnProjects(float x, float y, const std::string& file);  // fichier lâché sur un dossier du panneau Projets
    bool projectOrchestrated(const ws::Folder* project);
    bool isAgentFolder(const ws::Folder* f);
    bool isBrainConversation(const ws::Folder* f, const ws::ConversationMeta& m);
    struct DirEntry {
        std::string name, path;
        bool dir = false;
    };
    struct DirListing {
        double time = -1e9;
        std::vector<DirEntry> entries;
        bool truncated = false;
    };
    std::map<std::string, DirListing> dirCache_;
    std::map<std::string, std::pair<double, bool>> orchFileCache_;  // projet → (vérifié à, a un _orchestrateur.json)
    struct DropZone {
        float x0, y0, x1, y1;
        std::string folder;
    };
    std::vector<DropZone> dropZones_;   // zones des dossiers du panneau Projets à la dernière image
    float projectsRect_[4] = {0, 0, 0, 0};
    std::string selectedFile_;          // fichier sélectionné (chemin UTF-8)
    std::string selectedFileOwner_;     // dossier de projet qui le contient ("" = dossier de travail)
    void drawConversationArea();              // ConversationView.cpp
    void drawConversation(OpenConversation& oc);
    void drawEntry(OpenConversation& oc, int index, float width);
    void drawToolEntry(OpenConversation& oc, int index, float width);
    void drawPromptEditor(OpenConversation& oc); // PromptEditor.cpp
    void drawQuickBar(OpenConversation& oc);     // PromptEditor.cpp
    void drawSlashPopup(OpenConversation& oc, const std::string& prefix);
    void drawWelcome();                       // ConversationView.cpp
    void drawUsagePanel();                    // UsagePanel.cpp
    void drawConfigWindow();                  // ConfigPanel.cpp
    void drawPermissionPanel(OpenConversation& oc);  // PermissionPanel.cpp
    void drawPageSwitch();                    // Dashboard.cpp
    void drawDashboard(bool detached);        // Dashboard.cpp
    void drawDashboardCard(OpenConversation& oc, int index, float width, float height);
    void drawActivityFeed();
    void drawShortcutTables();                // AppMenus.cpp
    // Page « Orchestrateur » (OrchestratorPanel.cpp)
    void drawOrchestratorPage();
    void drawOrchestratorStatus();
    void drawOrchestratorBudgetCard(bool subscription);
    void drawBudgetGauge(double fraction, double tolerance, float width, float height);
    // Session de 5 h du compte (0 → 100 %) : usage hors orchestration, part de l'orchestration, budget alloué, plafond.
    void drawSessionBar(double used, double base, double spent, double allowance, double tolerance, double ceiling,
                        float width, float height);
    void drawOrchestratorQuestion();
    void drawOrchestratorPauseNote();  // pause décidée par l'orchestrateur (@@PAUSE)
    void drawOrchestratorSettings();
    void drawOrchestratorLog();
    void drawOrchestratorTeam(bool compact);
    void drawOrchestratorDiscussion();
    void drawOrchestrationHero(bool detached);   // carte d'orchestration du tableau de bord (Dashboard.cpp)
    bool drawBrainEntry(OpenConversation& oc, int index, float width);  // ConversationView.cpp
    void drawGuideWindow();                   // Tutorial.cpp
    void drawTour();                          // Tutorial.cpp
    void drawWelcomeModal();                  // Tutorial.cpp
    void drawGuideReminder();                 // Tutorial.cpp
    void resetPreferences();
    void createExampleProject();              // Tutorial.cpp
    void startTour();

    // --- Deuxième fenêtre ---
    void openDashboardWindow();
    void closeDashboardWindow();
    void dashboardFrame();

    // --- Actions ---
    // `focus` = false : ouverte en arrière-plan, sans changer l'onglet actif (orchestrateur).
    OpenConversation* newConversation(ws::Folder* folder, const std::string& cwdOverride = {}, bool focus = true);
    OpenConversation* openConversation(const std::string& id, bool focus = true);
    OpenConversation* openImported(const claude::HistorySession& hs, ws::Folder* folder);
    void closeConversation(int index);
    bool orchKeepsAlive(const OpenConversation& oc) const;  // agent suivi par l'orchestrateur : jamais coupé
    int firstVisibleTab() const;
    void releaseHiddenTabs();
    double lastHiddenCheck_ = 0;

    // Signaux d'attention (Attention.cpp) : bouton de page ou onglet qui clignote quand sa section change d'état
    // hors de la vue. Clés : "page0".."page3", "tab:<id>".
    struct Flash {
        double start = 0;
        ImVec4 color;
        bool sticky = false;  // reste teinté jusqu'à ce qu'on regarde
    };
    struct AttnConv {
        size_t perms = 0;
        bool busy = false;
    };
    std::map<std::string, Flash> flashes_;
    std::map<std::string, AttnConv> attnConvs_;
    bool attnInit_ = false;
    int prevOrchState_ = -1;
    bool prevDesignerBusy_ = false;
    void flash(const std::string& key, const ImVec4& color, bool sticky);
    void clearFlash(const std::string& key);
    ImVec4 flashColor(const std::string& key);  // w = intensité (0 : aucun signal)
    void tickAttention();
    void saveConversation(OpenConversation& oc);
    void saveAll();
    void sendDraft(OpenConversation& oc);
    // `remember` = false : ne pas ajouter le texte à l'historique des prompts (envois automatiques).
    void sendText(OpenConversation& oc, const std::string& text, const std::vector<claude::ImageAttachment>& images = {},
                  bool remember = true);
    void pushActivity(const OpenConversation& oc, ActivityKind kind, const std::string& text);
    void focusConversation(int index);  // affiche la conversation dans le cockpit
    void requestRecap(const std::string& convId);
    void requestFolderRecap(ws::Folder* f);   // récaps manquants/périmés d'abord, puis synthèse
    void runFolderSynthesis(ws::Folder* f);
    void pumpFolderSyntheses();
    // Récaps (absents ou périmés) d'une liste de conversations, 3 à la fois, sans synthèse ni fermeture.
    void requestRecaps(const std::vector<std::string>& convIds);
    // Jamais fait, ou la conversation a avancé depuis.
    static bool needsRecap(const ws::ConversationMeta& m) {
        return m.messages >= 1 && (m.recap.empty() || m.recapTime < m.updated - 60);
    }
    struct FolderSynthesis {                  // synthèse en préparation : récaps à générer avant
        std::string path;                     // dossier (UTF-8) ; vide = simple lot de récaps (requestRecaps)
        std::deque<std::string> toStart;      // conversations dont le récap reste à lancer
        std::set<std::string> running;        // récaps en cours
        int total = 0;
        bool synthesize = true;               // une fois les récaps faits : synthèse du dossier
    };
    std::vector<FolderSynthesis> syntheses_;
    std::set<std::string> rotating_;          // orchestrateur : conversations dont le récap de rotation est lancé
    // « Repartir du récap » : récap (si absent ou périmé), puis conversation neuve dans le même dossier qui le reçoit
    // en contexte ; l'ancienne passe « terminée » et se ferme (elle reste dans l'espace de travail).
    void restartFromRecap(const std::string& convId);
    void pumpRestarts();                       // termine les « Repartir du récap » dont le récap est prêt
    // Conversation neuve qui succède à `convId` (même réglages, contexte cowork injecté) ; nullptr si impossible.
    OpenConversation* replaceConversation(const std::string& convId, ws::Folder* folder, const std::string& title);
    std::map<std::string, int64_t> restarting_;  // conversation → recapTime au lancement du récap
    bool restartPending(const std::string& convId) const { return restarting_.count(convId) > 0; }
    const FolderSynthesis* synthesisFor(const ws::Folder* f) const;
    void refreshUsage(bool silent = false);
    void autoRefreshUsage(bool atLaunch);    // actualise si la mesure est absente ou périmée
    void exportMarkdown(OpenConversation& oc);
    void attachFile(OpenConversation& oc, const std::string& path);
    void handleGlobalShortcuts();
    void toast(const std::string& text, bool error = false);
    void wireSession(claude::ClaudeSession& s);
    ws::Folder* folderOf(const OpenConversation& oc);
    ws::Folder* defaultFolder();
    std::string contextFor(OpenConversation& oc);
    OpenConversation* active();
    void openFolderDialog(std::function<void(const std::string&)> cb, const std::string& start = {});
    void openFileDialog(std::function<void(const std::vector<std::string>&)> cb);

    // --- Orchestrateur (OrchestratorPanel.cpp) ---
    friend class AppOrchHost;
    struct AgentView {                       // un agent de l'équipe, vu depuis l'interface
        std::string name, convId, label;
        const char* icon = "·";
        ImVec4 color{0.5f, 0.5f, 0.5f, 1};
        bool working = false, attention = false;
        const claude::OrchTask* task = nullptr;
        claude::ParticipantStatus status;
    };
    AgentView agentView(const std::string& name);
    bool isOrchBrain(const std::string& convId) const;
    void openOrchestratorPage(bool discussion = false);
    void autoLoadOrchestrator();
    void tickOrchestrator();
    bool loadOrchestrator(ws::Folder* project);
    void saveOrchestrator();
    bool orchestratorStart();
    void orchestratorResume();
    bool orchestratorAnswer(const std::string& answer);  // répond à la question en attente (WaitingHuman)
    void orchestratorRefreshBrain();  // prompt et outils du cerveau à jour avant un message direct de l'humain
    ws::Folder* orchProjectFolder();
    int indexOfConversation(const std::string& id) const;

    // --- Énergie (préréglages : App.cpp ; interface : ConfigPanel.cpp) ---
    void tickEnergy(bool force = false);      // source d'alimentation (mode automatique) et préréglage appliqué
    void setEnergyMode(int mode);             // choix de l'utilisateur (menu, carte)
    void drawEnergyPresets();                 // cartes de préréglages (ConfigPanel.cpp)
    int energyApplied_ = -1;                  // préréglage fixe actuellement appliqué (-1 : aucun / personnalisé)
    double energyChecked_ = -1e9;
    bool onBattery_ = false, hasBattery_ = false;
    int batteryPct_ = -1, batterySecs_ = -1;

    // --- Connexion au compte Claude (LoginPanel.cpp) ---
    void checkAuth();                         // `claude auth status` en arrière-plan
    void tickAuth();                          // suit la vérification et la connexion en cours
    void drawLoginWindow();
    void openLogin(const std::string& reason = {});
    void startLogin(bool console);            // `claude auth login` dans une fenêtre de terminal
    void onAuthError(claude::ClaudeSession& s);
    // Modèles : abonnement du compte (« pro », « max »… ; "" inconnu), refus d'un modèle (hors abonnement…),
    // familles utilisables. Voir claude/Models.h.
    std::string accountPlan() const;
    void onModelRefused(claude::ClaudeSession& s);
    void onUsageLimit(claude::ClaudeSession& s);
    void orchestratorLimitReached(const std::string& win);  // OrchestratorPanel.cpp
    bool modelUsable(const std::string& model) const { return settings_.modelAccess.usable(model, accountPlan()); }
    void drawModelRefusedCard(OpenConversation& oc, int index, float width);  // ConversationView.cpp
    void drawModelCards(std::string& model, claude::ClaudeSession* s);         // ConfigPanel.cpp
    // Éléments d'un menu / d'une liste de modèles (barre rapide, menu Configuration) : défaut, les quatre familles
    // avec leur accès, puis les versions figées. Renvoie true et remplit `chosen` quand un modèle est choisi.
    bool drawModelPickerItems(claude::ClaudeSession* s, const std::string& current, std::string& chosen);
    void drawEffortSelector(std::string& effort, claude::ClaudeSession* s);  // ConfigPanel.cpp
    void drawPermissionCards(std::string& mode);                              // ConfigPanel.cpp
    // Sections de la Configuration (ConfigPanel.cpp)
    void drawConfigTools(claude::ClaudeSession* s);
    void drawConfigInstructions(OpenConversation* oc);
    void drawConfigAdvanced(claude::ClaudeSession* s);
    void drawConfigGeneral();
    void drawConfigAppearance();
    void drawConfigPerformance();
    // Réglages enregistrés d'eux-mêmes quand ils changent (plus de bouton « Enregistrer les préférences »)
    void autosaveSettings();
    std::string settingsSnapshot_;
    double settingsCheckedAt_ = 0;
    // Polices : rechargement complet (changement de police), entre deux images.
    bool fontFamilyReloadPending_ = false;
    void reloadFontFamilies();
    void onLoggedIn();
    bool orchManages(const std::string& convId) const;  // conversation reprise par l'orchestrateur lui-même
    std::vector<OpenConversation*> authFailedConversations(bool excludeOrch);
    int resendAuthFailed();                   // renvoie les messages restés sans réponse ; renvoie leur nombre
    void drawAuthErrorCard(OpenConversation& oc, int index, float width);  // ConversationView.cpp
    void drawAuthStatusBadge();               // barre d'état (AppMenus.cpp)
    claude::AuthStatus auth_;
    std::unique_ptr<claude::AuthChecker> authCheck_;
    double authLastCheck_ = -1e9;
    bool authProblem_ = false;                // un envoi a échoué faute de connexion depuis la dernière connexion réussie
    std::string authReason_;                  // message d'erreur qui a ouvert la fenêtre
    bool showLogin_ = false, focusLogin_ = false;
    std::unique_ptr<claude::TerminalProcess> loginTerm_;
    double loginStartedAt_ = -1;              // < 0 : aucune connexion en cours
    int64_t loginBaseline_ = 0;               // expiresAt avant la connexion (détecte une connexion neuve)
    std::string loginBaselineEmail_;
    bool loginJustSucceeded_ = false;
    bool loginTermExited_ = false;            // la fenêtre de terminal s'est fermée (connexion finie ou abandonnée)
    std::string loginError_;
    bool orchPausedForAuth_ = false;

    // --- Designer (DesignerPanel.cpp) : projet + équipe d'agents + orchestrateur conçus à partir d'une description ---
    void drawDesignerPage();
    // Carte de choix d'un type de projet (Designer, accueil) ; renvoie true au clic.
    bool drawKindCard(claude::ProjectKind k, bool selected, float width, float height);
    float kindCardHeight(float width);                 // hauteur commune des cartes pour cette largeur
    void openDesignerFor(claude::ProjectKind k);       // Designer, étape « Décrire », type présélectionné
    void askQuestion(const std::string& question);     // « Juste une question » : conversation dans Divers
    std::string designerQuestion_;
    void designerRun(bool refine);  // conception (ou questions de clarification), ou affinage de la conception actuelle
    void designerCancel();
    void designerCreate();          // projet cowork, thèmes, fichiers de départ, orchestrateur réglé
    void designerReset();
    bool designerBusy() const;
    std::string designerSuggestedCwd();
    void loadDesigner();
    void saveDesigner();
    claude::DesignerDraft designer_;
    std::string designerPath_;
    bool designerDirty_ = false;
    double designerSaved_ = 0;
    double designerStarted_ = 0;
    bool designerRefining_ = false;
    std::vector<std::string> designerActivity_;  // ce que fait Claude pendant la conception (une ligne par étape)
    std::string designerError_, designerRaw_, designerPlanText_;
    // Échange complet de la dernière conception (fenêtre « Échange avec Claude ») : demande, réflexion, réponse.
    struct DesignerBlock {
        int kind = 0;  // 0 réflexion, 1 réponse, 2 outil
        std::string text;
    };
    std::string designerPrompt_;
    std::vector<DesignerBlock> designerExchange_;
    int designerHeadPara_ = -1;     // paragraphe de réflexion affiché sur la dernière ligne d'activité
    bool designerLineTransient_ = false;  // la dernière ligne d'activité sera remplacée (« Réflexion… », compteur)
    void designerSay(const std::string& line, bool transient);
    bool designerShowExchange_ = false;
    void drawDesignerExchange();
    bool designerConfirmReset_ = false;
    double designerConfirmAt_ = 0;
    int designerOpenRole_ = -1;     // fiche de rôle à déplier (agent ajouté)
    int designerFilePreview_ = 0;

    // --- Données ---
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool running_ = true;
    std::string optDataDir_, optWorkspace_, optSend_, optScreenshot_, optCwd_;
    double optAfter_ = 3.0;
    std::string optPanel_;
    std::string optDesign_;        // --design "<description>" : lance le Designer (tests)
    std::string optOrchestrate_;   // --orchestrate "<objectif>" : lance l'orchestrateur sur le 1er projet
    bool optNoWait_ = false;
    ImGuiContext* mainCtx_ = nullptr;
    bool fontReloadPending_ = false;
    int vsyncApplied_ = -1;
    float displayHz_ = 60.0f;
    std::string rendererName_;
    int lastPage_ = 0;
    double pageFadeStart_ = -10;
    FontSet mainFonts_;
    std::unique_ptr<SecondaryWindow> dash_;

    // Pages : 0 = cockpit, 1 = tableau de bord des conversations, 2 = orchestrateur, 3 = designer
    int page_ = 0;
    int dashFilter_ = 0;           // 0 toutes, 1 actives, 2 attendent une action
    std::deque<ActivityEvent> activity_;   // plus récent en tête
    uint64_t activitySeq_ = 0;             // nombre total d'entrées ajoutées (repère des « nouvelles »)
    uint64_t feedSeenSeq_ = 0;             // dernière entrée vue avec le fil tout en bas
    bool feedJump_ = false;                // ramener le fil tout en bas à la prochaine frame

    // Tutoriel
    int tourStep_ = -1;
    int tourEnteredStep_ = -1;
    bool showGuide_ = false;
    int guideSection_ = 0;
    bool showWelcomeModal_ = false;
    float editorRect_[4] = {0, 0, 0, 0};     // zone de l'éditeur (pour la visite guidée)
    float menuBarRect_[4] = {0, 0, 0, 0};
    float statusBarRect_[4] = {0, 0, 0, 0};
    int editorRectFrame_ = -10;
    float aideMenuRect_[4] = {0, 0, 0, 0};
    bool showGuideReminder_ = false;
    double reminderStart_ = 0;
    double reminderFadeOut_ = -1;
    float dpiScale_ = 1.0f;
    double now_ = 0;
    double lastAutoUsage_ = -1e9;            // dernière actualisation automatique (horloge de l'app)
    bool windowFocused_ = true;

    Settings settings_;
    std::string settingsPath_, usagePath_, iniPath_;
    std::string claudeExe_;
    claude::UsageTracker usage_;
    ws::Workspace workspace_;

    std::vector<std::unique_ptr<OpenConversation>> open_;
    int active_ = -1;
    int selectTab_ = -1;
    std::vector<std::unique_ptr<claude::OneShot>> jobs_;
    std::deque<Toast> toasts_;

    // Sélection dans l'arbre
    std::string selectedFolder_;   // chemin UTF-8
    std::string selectedConv_;     // id
    std::string treeFilter_;
    std::string renameTarget_, renameBuffer_;
    bool renameIsFolder_ = false;

    // Fenêtres
    bool showWorkspace_ = true, showUsage_ = true, showConfig_ = true, showInspector_ = true;
    bool showLog_ = false, showShortcuts_ = false, showAbout_ = false, showHistory_ = false, showDemo_ = false;
    int configTab_ = -1;           // onglet à sélectionner dans Configuration
    bool openNewProject_ = false;
    bool resetLayout_ = false;

    // Orchestrateur : une seule orchestration à la fois
    std::unique_ptr<claude::Orchestrator> orch_;
    std::string orchProject_;      // chemin UTF-8 du projet piloté
    bool orchShowDiscussion_ = false;
    bool orchShowBilan_ = false;             // page Orchestrateur : sélectionner l'onglet « Bilan »        // page Orchestrateur : afficher la discussion (onglet)
    double orchLastSave_ = 0;
    int orchSeenQuestion_ = -1;              // dernière question signalée (ouverture de la fenêtre)
    int orchFormSeq_ = -1;                   // question à laquelle correspond le formulaire de réponse
    std::vector<int> orchAnswerSel_;         // option choisie par question (-1 aucune, -2 « Autre »)
    std::vector<std::string> orchAnswerOther_;
    std::string orchAnswerExtra_;
    // Option cliquée dans le fil de la discussion du cerveau (envoyée hors du rendu, voir tickOrchestrator).
    std::string orchQuickAnswer_, orchQuickAnswerConv_;
    int orchSettingsTab_ = -1;              // onglet des réglages à sélectionner (-1 : aucun)
    bool orchAdvanced_ = false;              // réglages avancés affichés (sinon : niveaux de budget)

    // Nouveau projet
    std::string npName_, npCwd_, npFolders_ = "Code, Infra, Docs, Idées";
    std::string npKind_ = "software";    // type du projet à créer (claude/ProjectKind)

    // Import d'historique
    std::vector<claude::HistoryProject> histProjects_;
    std::vector<claude::HistorySession> histSessions_;
    int histProject_ = -1;
    std::string histFilter_;

    // Configuration en cours d'édition (copie appliquée sur demande)
    claude::SessionConfig editConfig_;
    std::string editConfigOwner_ = "#";  // id de la conversation éditée ("" = défauts)
    std::string newRule_;

    // Résultats des boîtes de dialogue (appelées depuis un autre thread)
    std::mutex dialogMutex_;
    std::vector<std::function<void()>> dialogResults_;

public:
    // Polices accessibles aux fonctions de rendu
    ImFont* fontUi = nullptr;
    ImFont* fontBold = nullptr;
    ImFont* fontMono = nullptr;
    ImFont* fontItalic = nullptr;
};

}  // namespace app
