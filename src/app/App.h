#pragma once
// Application ClaudeDesk : fenêtre SDL3 + Dear ImGui (docking), onglets de
// conversations, panneaux d'usage / configuration / espace de travail.
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/Settings.h"
#include "claude/ClaudeSession.h"
#include "claude/History.h"
#include "claude/OneShot.h"
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

    // Éditeur
    std::string draft;
    std::vector<claude::ImageAttachment> attachments;
    bool focusEditor = true;
    bool cursorToEnd = false;
    std::string replaceDraft;          // remplacement demandé du texte de l'éditeur (via callback)
    bool hasReplace = false;
    int historyIndex = -1;
    float editorHeight = 130.0f;
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
    std::set<int> selectable;          // entrées affichées en mode « sélection de texte »
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
    void drawConversationItem(ws::Folder* f, ws::ConversationMeta& m);
    void drawInspector();                     // WorkspacePanel.cpp
    void drawMoveMenu(const std::string& convId, ws::Folder* f);
    void drawHistoryImport();                 // WorkspacePanel.cpp
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
    OpenConversation* newConversation(ws::Folder* folder, const std::string& cwdOverride = {});
    OpenConversation* openConversation(const std::string& id);
    OpenConversation* openImported(const claude::HistorySession& hs, ws::Folder* folder);
    void closeConversation(int index);
    void saveConversation(OpenConversation& oc);
    void saveAll();
    void sendDraft(OpenConversation& oc);
    void sendText(OpenConversation& oc, const std::string& text, const std::vector<claude::ImageAttachment>& images = {});
    void pushActivity(const OpenConversation& oc, ActivityKind kind, const std::string& text);
    void focusConversation(int index);  // affiche la conversation dans le cockpit
    void requestRecap(const std::string& convId);
    void requestFolderRecap(ws::Folder* f);
    void refreshUsage();
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

    // --- Données ---
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool running_ = true;
    std::string optDataDir_, optWorkspace_, optSend_, optScreenshot_, optCwd_;
    double optAfter_ = 3.0;
    std::string optPanel_;
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

    // Pages : 0 = cockpit, 1 = tableau de bord des conversations
    int page_ = 0;
    int dashFilter_ = 0;           // 0 toutes, 1 actives, 2 attendent une action
    std::deque<ActivityEvent> activity_;

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

    // Nouveau projet
    std::string npName_, npCwd_, npFolders_ = "Code, Infra, Docs, Idées";

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
