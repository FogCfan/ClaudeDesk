#include "app/App.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h>
#include <imgui_freetype.h>

#include "claude/CommandLine.h"
#include "ui/Anim.h"
#include "ui/Theme.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace app {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Boucle principale
// ---------------------------------------------------------------------------

int App::run(int argc, char** argv) {
    // Options (tests et automatisation) :
    //   --data-dir <dossier>      réglages / disposition ailleurs que dans %APPDATA%
    //   --workspace <dossier>     racine de l'espace de travail
    //   --send "<texte>"          ouvre une conversation et envoie ce message
    //   --screenshot <f.bmp>      capture la fenêtre puis quitte…
    //   --after <secondes>        …après ce délai (défaut 3 s ; avec --send : après la réponse)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--data-dir") optDataDir_ = next();
        else if (a == "--workspace") optWorkspace_ = next();
        else if (a == "--send") optSend_ = next();
        else if (a == "--screenshot") optScreenshot_ = next();
        else if (a == "--after") optAfter_ = std::atof(next().c_str());
        else if (a == "--cwd") optCwd_ = next();
        else if (a == "--panel") optPanel_ = next();
        else if (a == "--nowait") optNoWait_ = true;
    }
    if (!init()) return 1;
    if (optPanel_ == "history") showHistory_ = true;
    if (optPanel_ == "shortcuts") showShortcuts_ = true;
    if (optPanel_ == "newproject") openNewProject_ = true;
    if (optPanel_ == "light") settings_.theme = 1, applyTheme();
    if (optPanel_ == "dashboard") page_ = 1;
    if (optPanel_ == "detached") openDashboardWindow();
    if (optPanel_.rfind("guide", 0) == 0) {
        showGuide_ = true;
        if (optPanel_.size() > 5) guideSection_ = std::atoi(optPanel_.c_str() + 5);
    }
    if (optPanel_ == "gfx") showConfig_ = true, configTab_ = 6, settings_.showFps = true;
    if (optPanel_.rfind("tour", 0) == 0) {
        startTour();
        if (optPanel_.size() > 4) tourStep_ = std::atoi(optPanel_.c_str() + 4);
    }
    if (optPanel_ == "welcome") showWelcomeModal_ = true;
    if (optPanel_ == "example") createExampleProject();
    // Accueil des nouveaux utilisateurs : 1er lancement → le guide s'ouvre ; lancements 2 à 11 → un rappel
    // animé indique où le retrouver ; ensuite plus rien (sauf réinitialisation).
    settings_.launchCount++;
    if (optPanel_ == "reminder") settings_.launchCount = std::max(2, settings_.launchCount);
    if (settings_.launchCount == 1) {
        showGuide_ = true;
        guideSection_ = 0;
        settings_.tutorialSeen = true;
    } else if (settings_.launchCount <= 11) {
        showGuideReminder_ = true;
    }
    settings_.save(settingsPath_);
    if (settings_.dashDetached && optPanel_.empty()) openDashboardWindow();
    if (!optSend_.empty()) {
        OpenConversation* oc = newConversation(nullptr, optCwd_);
        oc->draft = optSend_;
        sendDraft(*oc);
    }
    int idleFrames = 0;
    uint64_t lastFrameNs = SDL_GetTicksNS();
    while (running_) {
        bool busy = !jobs_.empty() || !toasts_.empty() || ui::anim::busy() || tourStep_ >= 0;
        for (auto& oc : open_)
            if (oc->session->state == claude::SessionState::Busy || oc->session->state == claude::SessionState::Starting)
                busy = true;
        SDL_Event ev;
        bool gotEvent = false;
        auto handleEvent = [&](SDL_Event& ev) {
            gotEvent = true;
            // Chaque contexte ImGui ne retient que les événements de sa propre fenêtre.
            ImGui::SetCurrentContext(mainCtx_);
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (dash_) {
                ImGui::SetCurrentContext(dash_->ctx);
                ImGui_ImplSDL3_ProcessEvent(&ev);
                ImGui::SetCurrentContext(mainCtx_);
            }
            switch (ev.type) {
                case SDL_EVENT_QUIT: running_ = false; break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    if (ev.window.windowID == SDL_GetWindowID(window_)) running_ = false;
                    else if (dash_ && ev.window.windowID == SDL_GetWindowID(dash_->window)) dash_->closeRequested = true;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_GAINED: windowFocused_ = true; break;
                case SDL_EVENT_WINDOW_FOCUS_LOST: windowFocused_ = false; break;
                case SDL_EVENT_DROP_FILE:
                    if (ev.drop.windowID == SDL_GetWindowID(window_))
                        if (auto* oc = active(); oc && ev.drop.data) attachFile(*oc, ev.drop.data);
                    break;
                default: break;
            }
        };
        // Mode économie : au repos, on attend les événements au lieu de redessiner en continu.
        if (settings_.ecoIdle && !busy && idleFrames > 4 && SDL_WaitEventTimeout(&ev, 250)) handleEvent(ev);
        while (SDL_PollEvent(&ev)) handleEvent(ev);
        idleFrames = gotEvent ? 0 : idleFrames + 1;
        if (dash_ && dash_->closeRequested) closeDashboardWindow();
        if (fontReloadPending_) {
            applyFontLoader(true);
            fontReloadPending_ = false;
        }
        applyVSync();
        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) {
            now_ = SDL_GetTicks() / 1000.0;
            processAsyncResults();
            dashboardFrame();  // le tableau de bord détaché reste vivant
            SDL_DelayPrecise((uint64_t)(1e9 / std::max(30.0f, displayHz_)));
            continue;
        }
        frame();
        // Cadence : limite choisie, ou 30 i/s en arrière-plan si demandé.
        {
            bool focused = windowFocused_ || (dash_ && (SDL_GetWindowFlags(dash_->window) & SDL_WINDOW_INPUT_FOCUS));
            int cap = settings_.fpsMode == 1 ? std::max(10, settings_.fpsCap) : 0;
            if (!focused && settings_.ecoBackground) cap = cap ? std::min(cap, 30) : 30;
            if (cap > 0) {
                uint64_t target = 1000000000ull / (uint64_t)cap;
                uint64_t el = SDL_GetTicksNS() - lastFrameNs;
                if (el < target) SDL_DelayPrecise(target - el);
            }
            lastFrameNs = SDL_GetTicksNS();
        }
    }
    shutdown();
    return 0;
}

bool App::init() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init : %s", SDL_GetError());
        return false;
    }
    fs::path dataDir = optDataDir_.empty() ? util::appDataDir() : util::pathFromUtf8(optDataDir_);
    {
        std::error_code ec;
        fs::create_directories(dataDir, ec);
    }
    settingsPath_ = util::pathToUtf8(dataDir / "settings.json");
    usagePath_ = util::pathToUtf8(dataDir / "usage.json");
    iniPath_ = util::pathToUtf8(dataDir / "layout.ini");
    settings_.load(settingsPath_);
    usage_.load(usagePath_);

    dpiScale_ = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (dpiScale_ <= 0) dpiScale_ = 1.0f;
    window_ = SDL_CreateWindow("ClaudeDesk", (int)(1480 * dpiScale_), (int)(920 * dpiScale_),
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window_) return false;
    // Moteur de rendu GPU choisi (Direct3D 12/11, Vulkan, OpenGL…), sinon le meilleur disponible.
    if (!settings_.renderDriver.empty()) renderer_ = SDL_CreateRenderer(window_, settings_.renderDriver.c_str());
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (!renderer_) return false;
    rendererName_ = SDL_GetRendererName(renderer_) ? SDL_GetRendererName(renderer_) : "?";
    if (const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_)); dm && dm->refresh_rate > 0)
        displayHz_ = dm->refresh_rate;
    applyVSync();
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);

    IMGUI_CHECKVERSION();
    mainCtx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(mainCtx_);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = iniPath_.c_str();
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigInputTextCursorBlink = true;
    io.ConfigDockingWithShift = false;
    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);
    mainFonts_ = loadFonts();
    useFonts(mainFonts_);
    applyTheme();

    claudeExe_ = settings_.claudeExe.empty() ? claude::findClaudeExecutable() : settings_.claudeExe;
    fs::path root = !optWorkspace_.empty()             ? util::pathFromUtf8(optWorkspace_)
                    : settings_.workspaceRoot.empty() ? util::documentsDir() / "ClaudeDesk"
                                                      : util::pathFromUtf8(settings_.workspaceRoot);
    if (!workspace_.open(root)) toast("Impossible d'ouvrir l'espace de travail : " + util::pathToUtf8(root), true);
    if (claudeExe_.empty())
        toast("Claude Code introuvable : indiquez son chemin dans Configuration › Application.", true);

    // Réouvre les conversations de la dernière fois.
    for (auto& id : settings_.openTabs) openConversation(id);
    for (size_t i = 0; i < open_.size(); ++i)
        if (open_[i]->session->localId == settings_.activeTab) selectTab_ = (int)i;
    return true;
}

void App::useFonts(const FontSet& f) {
    fontItalic = f.italic ? f.italic : f.ui;
    fontUi = f.ui;
    fontBold = f.bold;
    fontMono = f.mono;
}

void App::applyVSync() {
    int want = settings_.fpsMode == 0 ? 1 : 0;
    if (want == vsyncApplied_ || !renderer_) return;
    SDL_SetRenderVSync(renderer_, want);
    vsyncApplied_ = want;
}

// Choisit le moteur de rendu des polices (FreeType ou stb_truetype) pour l'atlas du contexte courant.
void App::applyFontLoader(bool force) {
    auto apply = [&](ImFontAtlas* atlas) {
        const ImFontLoader* ft = ImGuiFreeType::GetFontLoader();
        const ImFontLoader* stb = ImFontAtlasGetFontLoaderForStbTruetype();
        const ImFontLoader* want = settings_.fontRenderer == 2 ? stb : ft;
        unsigned flags = ImGuiFreeTypeLoaderFlags_LoadColor;  // emoji en couleur
        if (settings_.fontRenderer == 0) flags |= ImGuiFreeTypeLoaderFlags_LightHinting;
        if (settings_.fontRenderer == 1) flags |= ImGuiFreeTypeLoaderFlags_NoHinting;
        atlas->FontLoaderFlags = flags;
        if (force && atlas->FontLoader == want) atlas->SetFontLoader(want == ft ? stb : ft);  // reconstruit les glyphes
        atlas->SetFontLoader(want);
    };
    if (!force) {
        apply(ImGui::GetIO().Fonts);
        return;
    }
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(mainCtx_);
    apply(ImGui::GetIO().Fonts);
    if (dash_) {
        ImGui::SetCurrentContext(dash_->ctx);
        apply(ImGui::GetIO().Fonts);
        dash_->fontRendererApplied = settings_.fontRenderer;
    }
    ImGui::SetCurrentContext(prev);
}

// Remet toutes les préférences à zéro (les conversations de l'espace de travail sont conservées).
void App::resetPreferences() {
    std::string root = settings_.workspaceRoot;
    settings_ = Settings();
    settings_.load("");  // valeurs par défaut (modèles de prompts…)
    settings_.workspaceRoot = root;
    settings_.save(settingsPath_);
    std::error_code ec;
    fs::remove(util::pathFromUtf8(iniPath_), ec);  // disposition des panneaux
    ImGui::GetIO().IniFilename = nullptr;          // ne pas la réécrire en quittant
    open_.clear();
    if (util::relaunchSelf()) running_ = false;
    else toast("Préférences réinitialisées : relancez ClaudeDesk.", false);
}

void App::restartApp() {
    saveAll();
    settings_.save(settingsPath_);
    if (util::relaunchSelf()) running_ = false;
    else toast("Relance impossible : redémarrez ClaudeDesk à la main.", true);
}

FontSet App::loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    applyFontLoader(false);
    const char* fontsDir = "C:/Windows/Fonts/";
    auto file = [&](const char* name) { return std::string(fontsDir) + name; };
    auto exists = [](const std::string& p) {
        std::error_code ec;
        return fs::exists(util::pathFromUtf8(p), ec);
    };
    auto addWithFallbacks = [&](const char* main) -> ImFont* {
        ImFont* f = nullptr;
        if (exists(file(main))) f = io.Fonts->AddFontFromFileTTF(file(main).c_str(), settings_.fontSize);
        if (!f) return io.Fonts->AddFontDefault();
        ImFontConfig merge;
        merge.MergeMode = true;
        for (const char* fb : {"seguisym.ttf", "seguiemj.ttf", "msyh.ttc"})
            if (exists(file(fb))) io.Fonts->AddFontFromFileTTF(file(fb).c_str(), settings_.fontSize, &merge);
        return f;
    };
    FontSet set;
    set.ui = addWithFallbacks("segoeui.ttf");
    set.bold = addWithFallbacks("segoeuib.ttf");
    set.mono = addWithFallbacks("consola.ttf");
    set.italic = addWithFallbacks("segoeuii.ttf");
    io.FontDefault = set.ui;
    return set;
}

void App::applyTheme() {
    ui::applyTheme((ui::ThemeId)settings_.theme, dpiScale_ * settings_.uiScale);
    ImGuiStyle& s = ImGui::GetStyle();
    s.FontSizeBase = settings_.fontSize;
    s.FontScaleDpi = dpiScale_;
    s.FontScaleMain = settings_.uiScale;
    s.AntiAliasedLines = s.AntiAliasedFill = s.AntiAliasedLinesUseTex = true;
    s.CurveTessellationTol = settings_.highQuality ? 0.5f : 1.25f;
    s.CircleTessellationMaxError = settings_.highQuality ? 0.12f : 0.30f;
}

void App::shutdown() {
    settings_.openTabs.clear();
    for (auto& oc : open_)
        if (!oc->session->entries.empty()) settings_.openTabs.push_back(oc->session->localId);
    if (auto* oc = active()) settings_.activeTab = oc->session->localId;
    bool detached = dash_ != nullptr;
    closeDashboardWindow();
    settings_.dashDetached = detached;
    saveAll();
    // Ferme toutes les entrées d'abord : les processus se terminent en parallèle.
    for (auto& oc : open_) oc->session->stop();
    open_.clear();
    jobs_.clear();
    settings_.save(settingsPath_);
    usage_.save(usagePath_);
    ImGui::SetCurrentContext(mainCtx_);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(mainCtx_);
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

void App::frame() {
    now_ = SDL_GetTicks() / 1000.0;
    processAsyncResults();

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ui::anim::newFrame(settings_.animations, settings_.animSpeed, true);

    handleGlobalShortcuts();
    drawMenuBar();
    drawStatusBar();
    if (page_ == 1 && dash_) page_ = 0;  // tableau de bord déjà dans sa propre fenêtre
    // Fondu à l'arrivée sur une page
    if (page_ != lastPage_) {
        lastPage_ = page_;
        pageFadeStart_ = now_;
    }
    float pageAlpha = settings_.animations ? ui::anim::easeOutCubic((float)((now_ - pageFadeStart_) / (0.28 / settings_.animSpeed))) : 1.0f;
    if (pageAlpha < 1.0f) ui::anim::markBusy();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.05f, pageAlpha));
    // Page « tableau de bord » : la disposition du cockpit est gardée en vie mais masquée.
    ImGuiID dock = ImGui::DockSpaceOverViewport(0, nullptr, page_ == 1 ? ImGuiDockNodeFlags_KeepAliveOnly : ImGuiDockNodeFlags_None);
    {
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
        static bool checked = false;
        if (page_ == 0 && (resetLayout_ || (!checked && node && node->IsLeafNode() && node->Windows.Size == 0))) {
            buildDefaultLayout(dock);
            resetLayout_ = false;
        }
        if (page_ == 0) checked = true;
    }

    if (page_ == 1) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        // (pas de NoBringToFrontOnFocus : la fenêtre passerait derrière l'hôte du dockspace)
        ImGui::Begin("##dashpage", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDocking);
        drawDashboard(false);
        ImGui::End();
    } else {
        if (showWorkspace_) drawWorkspacePanel();
        if (showInspector_) drawInspector();
        drawConversationArea();
        if (showUsage_) drawUsagePanel();
        if (showConfig_) drawConfigWindow();
        if (showLog_) drawLogWindow();
    }
    ImGui::PopStyleVar();  // fondu de page
    if (showHistory_) drawHistoryImport();
    if (showGuide_) drawGuideWindow();
    drawWelcomeModal();
    drawGuideReminder();
    drawTour();
    if (showShortcuts_) drawShortcutsWindow();
    if (showAbout_) drawAboutWindow();
    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
    drawNewProjectPopup();

    // Notifications éphémères en bas à droite.
    while (!toasts_.empty() && toasts_.front().until < now_) toasts_.pop_front();
    if (!toasts_.empty()) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        // Glisse depuis la droite à l'arrivée du plus récent
        float slide = settings_.animations ? 1.0f - ui::anim::easeOutCubic((float)((now_ - toasts_.back().start) / 0.3)) : 0.0f;
        if (slide > 0) ui::anim::markBusy();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 16 + slide * 60, vp->WorkPos.y + vp->WorkSize.y - 16),
                                ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.95f);
        ImGui::Begin("##toasts", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking);
        for (auto& t : toasts_) {
            float a = settings_.animations ? std::clamp((float)((t.until - now_) / 0.35), 0.0f, 1.0f) *
                                                 std::clamp((float)((now_ - t.start) / 0.2), 0.0f, 1.0f)
                                           : 1.0f;
            ImVec4 c = t.error ? ui::palette().err : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            c.w *= a;
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
            ImGui::TextColored(c, "%s", t.text.c_str());
            ImGui::PopTextWrapPos();
        }
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) toasts_.clear();
        ImGui::End();
    }

    // Titre de la fenêtre
    static std::string lastTitle;
    std::string title = "ClaudeDesk";
    if (page_ == 1) title = "Tableau de bord — ClaudeDesk";
    else if (auto* oc = active()) title = oc->session->title + " — ClaudeDesk";
    if (title != lastTitle) {
        SDL_SetWindowTitle(window_, title.c_str());
        lastTitle = title;
    }

    ImGui::Render();
    const ImVec4& bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    SDL_SetRenderScale(renderer_, ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(renderer_, bg.x, bg.y, bg.z, 1.0f);
    SDL_RenderClear(renderer_);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
    // Capture automatique (tests) : la fenêtre seule, jamais l'écran entier.
    if (!optScreenshot_.empty()) {
        bool waiting = false;
        for (auto& oc : open_)
            if ((oc->session->state == claude::SessionState::Busy || oc->session->state == claude::SessionState::Starting) &&
                oc->session->permissions.empty())
                waiting = true;
        if (now_ > optAfter_ && (optNoWait_ || !waiting || now_ > optAfter_ + 120)) {
            if (SDL_Surface* surf = SDL_RenderReadPixels(renderer_, nullptr)) {
                SDL_SaveBMP(surf, optScreenshot_.c_str());
                SDL_DestroySurface(surf);
            }
            running_ = false;
        }
    }
    SDL_RenderPresent(renderer_);
    dashboardFrame();
}

// ---------------------------------------------------------------------------
// Tableau de bord dans une deuxième fenêtre système (son propre contexte ImGui)
// ---------------------------------------------------------------------------

void App::openDashboardWindow() {
    if (dash_) {
        SDL_RaiseWindow(dash_->window);
        return;
    }
    auto d = std::make_unique<SecondaryWindow>();
    d->window = SDL_CreateWindow("Tableau de bord — ClaudeDesk", (int)(1120 * dpiScale_), (int)(780 * dpiScale_),
                                 SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!d->window) {
        toast(std::string("Impossible d'ouvrir la fenêtre : ") + SDL_GetError(), true);
        return;
    }
    if (!settings_.renderDriver.empty()) d->renderer = SDL_CreateRenderer(d->window, settings_.renderDriver.c_str());
    if (!d->renderer) d->renderer = SDL_CreateRenderer(d->window, nullptr);
    if (!d->renderer) {
        SDL_DestroyWindow(d->window);
        toast(std::string("Impossible d'ouvrir la fenêtre : ") + SDL_GetError(), true);
        return;
    }
    SDL_SetRenderVSync(d->renderer, 0);  // la fenêtre principale cadence déjà la boucle
    d->ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(d->ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigInputTextCursorBlink = true;
    ImGui_ImplSDL3_InitForSDLRenderer(d->window, d->renderer);
    ImGui_ImplSDLRenderer3_Init(d->renderer);
    d->fonts = loadFonts();
    d->fontRendererApplied = settings_.fontRenderer;
    applyTheme();
    d->themeApplied = settings_.theme;
    ImGui::SetCurrentContext(mainCtx_);
    useFonts(mainFonts_);
    dash_ = std::move(d);
    settings_.dashDetached = true;
    if (page_ == 1) page_ = 0;
}

void App::closeDashboardWindow() {
    if (!dash_) return;
    ImGui::SetCurrentContext(dash_->ctx);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(dash_->ctx);
    ImGui::SetCurrentContext(mainCtx_);
    useFonts(mainFonts_);
    SDL_DestroyRenderer(dash_->renderer);
    SDL_DestroyWindow(dash_->window);
    dash_.reset();
    settings_.dashDetached = false;
}

void App::dashboardFrame() {
    if (!dash_ || (SDL_GetWindowFlags(dash_->window) & SDL_WINDOW_MINIMIZED)) return;
    ImGui::SetCurrentContext(dash_->ctx);
    useFonts(dash_->fonts);
    if (dash_->themeApplied != settings_.theme) {
        applyTheme();
        dash_->themeApplied = settings_.theme;
    }
    ImGui::GetStyle().FontSizeBase = settings_.fontSize;
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ui::anim::newFrame(settings_.animations, settings_.animSpeed, false);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##dashroot", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    drawDashboard(true);
    ImGui::End();
    ImGui::Render();
    const ImVec4& bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    SDL_SetRenderScale(dash_->renderer, ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
    SDL_SetRenderDrawColorFloat(dash_->renderer, bg.x, bg.y, bg.z, 1.0f);
    SDL_RenderClear(dash_->renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), dash_->renderer);
    if (!optScreenshot_.empty() && !running_) {  // capture de test : aussi la fenêtre détachée
        if (SDL_Surface* surf = SDL_RenderReadPixels(dash_->renderer, nullptr)) {
            std::string p = optScreenshot_;
            p.insert(p.size() > 4 ? p.size() - 4 : p.size(), "_dash");
            SDL_SaveBMP(surf, p.c_str());
            SDL_DestroySurface(surf);
        }
    }
    SDL_RenderPresent(dash_->renderer);
    ImGui::SetCurrentContext(mainCtx_);
    useFonts(mainFonts_);
}

void App::focusConversation(int index) {
    if (index < 0 || index >= (int)open_.size()) return;
    page_ = 0;
    selectTab_ = index;
    active_ = index;
    open_[index]->focusEditor = true;
    SDL_RaiseWindow(window_);
}

void App::pushActivity(const OpenConversation& oc, ActivityKind kind, const std::string& text) {
    activity_.push_front({util::nowUnix(), oc.session->localId, oc.session->title, text, kind});
    while (activity_.size() > 400) activity_.pop_back();
}

void App::buildDefaultLayout(unsigned int dock) {
    ImGui::DockBuilderRemoveNode(dock);
    ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
    ImGuiID left, rest, right, center, leftTop, leftBottom, rightTop, rightBottom;
    ImGui::DockBuilderSplitNode(dock, ImGuiDir_Left, 0.21f, &left, &rest);
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.30f, &right, &center);
    ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.40f, &leftBottom, &leftTop);
    ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.58f, &rightBottom, &rightTop);
    ImGui::DockBuilderDockWindow("Espace de travail", leftTop);
    ImGui::DockBuilderDockWindow("Inspecteur", leftBottom);
    ImGui::DockBuilderDockWindow("Conversations", center);
    ImGui::DockBuilderDockWindow("Usage", rightTop);
    ImGui::DockBuilderDockWindow("Configuration", rightBottom);
    ImGui::DockBuilderDockWindow("Journal", rightBottom);
    ImGui::DockBuilderDockWindow("Importer depuis Claude Code", center);
    ImGui::DockBuilderFinish(dock);
}

// ---------------------------------------------------------------------------
// Tâches de fond : sessions, récaps, boîtes de dialogue, sauvegarde auto
// ---------------------------------------------------------------------------

void App::processAsyncResults() {
    {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard lock(dialogMutex_);
            pending.swap(dialogResults_);
        }
        for (auto& f : pending) f();
    }
    for (auto& oc : open_) {
        auto& s = *oc->session;
        size_t perms = s.permissions.size();
        s.update(now_);
        if (s.permissions.size() > perms && !windowFocused_ && settings_.notifyOnFinish)
            SDL_FlashWindow(window_, SDL_FLASH_UNTIL_FOCUSED);
        // Journal d'activité : nouvelles entrées et nouvelles demandes
        if (oc->seenEntries == (size_t)-1 || oc->seenEntries > s.entries.size()) oc->seenEntries = s.entries.size();
        for (size_t k = oc->seenEntries; k < s.entries.size(); ++k) {
            const claude::Entry& e = s.entries[k];
            if (!e.parentToolUseId.empty()) continue;
            if (e.kind == claude::EntryKind::ToolUse) pushActivity(*oc, ActivityKind::Tool, e.text);
            else if (e.kind == claude::EntryKind::Error) pushActivity(*oc, ActivityKind::Error, util::firstLine(e.text, 120));
            else if (e.kind == claude::EntryKind::User) pushActivity(*oc, ActivityKind::Info, "Vous : " + util::firstLine(e.text, 100));
        }
        oc->seenEntries = s.entries.size();
        if (s.permissions.size() > oc->seenPermissions) {
            const auto& p = s.permissions.back();
            pushActivity(*oc, ActivityKind::Attention,
                         p.isQuestion ? "pose une question" : "attend votre autorisation : " + p.toolName);
        }
        oc->seenPermissions = s.permissions.size();
        bool busy = s.state == claude::SessionState::Busy;
        if (busy && !oc->wasBusy) oc->scrollToBottom = true;
        oc->wasBusy = busy;
        if (s.dirty && !s.entries.empty() && now_ - oc->lastSave > std::max(1, settings_.autosaveSeconds)) saveConversation(*oc);
    }
    for (auto& j : jobs_) j->update();
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](auto& j) { return j->finished(); }), jobs_.end());

    static double lastUsageSave = 0;
    if (usage_.dirty && now_ - lastUsageSave > 10) {
        usage_.save(usagePath_);
        usage_.dirty = false;
        lastUsageSave = now_;
    }
}

void App::toast(const std::string& text, bool error) {
    toasts_.push_back({text, now_ + (error ? 8.0 : 4.0), error, now_});
    if (toasts_.size() > 5) toasts_.pop_front();
}

// ---------------------------------------------------------------------------
// Conversations
// ---------------------------------------------------------------------------

OpenConversation* App::active() {
    if (active_ >= 0 && active_ < (int)open_.size()) return open_[active_].get();
    return nullptr;
}

void App::wireSession(claude::ClaudeSession& s) {
    s.claudeExe = claudeExe_;
    s.promptDir = util::pathToUtf8(util::pathFromUtf8(settingsPath_).parent_path() / "prompts");
    s.onRateLimit = [this](const json& info) { usage_.onRateLimitEvent(info); };
    s.onTurnFinished = [this](claude::ClaudeSession& sess) {
        if (!sess.accountPlan.empty()) usage_.subscription = sess.accountPlan;
        for (auto& oc : open_)
            if (oc->session.get() == &sess) {
                saveConversation(*oc);
                std::string txt = "a terminé";
                if (!sess.stats.turnHistory.empty()) {
                    const auto& t = sess.stats.turnHistory.back();
                    txt += " (" + util::humanDuration((int64_t)t.durationS) + ", " + util::formatUsd(t.costUsd) + ")";
                }
                pushActivity(*oc, ActivityKind::Done, txt);
            }
        if (!windowFocused_ && settings_.notifyOnFinish) SDL_FlashWindow(window_, SDL_FLASH_UNTIL_FOCUSED);
    };
}

ws::Folder* App::folderOf(const OpenConversation& oc) {
    if (oc.folderPath.empty()) return nullptr;
    return workspace_.findFolderByPath(util::pathFromUtf8(oc.folderPath));
}

ws::Folder* App::defaultFolder() {
    for (auto& p : workspace_.projects())
        if (p->name == "Divers") return p.get();
    std::string cwd = settings_.defaultCwd.empty() ? util::pathToUtf8(util::homeDir()) : settings_.defaultCwd;
    ws::Folder* f = workspace_.createProject("Divers", cwd);
    if (f) {
        f->notes = "Conversations rapides, hors projet.";
        workspace_.saveFolderMeta(f);
    }
    return f;
}

std::string App::contextFor(OpenConversation& oc) {
    std::string ctx;
    if (oc.injectContext)
        if (ws::Folder* f = folderOf(oc)) ctx = workspace_.buildContext(f, oc.session->localId);
    // Conversation sans session Claude Code (exemple, session perdue…) : Claude reçoit son propre fil.
    auto& s = *oc.session;
    bool hasHistory = false;
    for (auto& e : s.entries)
        if (e.kind == claude::EntryKind::User || e.kind == claude::EntryKind::Assistant) hasHistory = true;
    if (s.claudeSessionId.empty() && hasHistory) {
        ctx += "\n\n# Cette conversation jusqu'ici\n"
               "L'utilisateur poursuit une conversation commencée auparavant (sans session Claude Code active). "
               "Reprends-la naturellement.\n";
        if (!util::trim(oc.meta.recap).empty()) ctx += "\n## Récapitulatif\n" + oc.meta.recap + "\n";
        std::string md = claude::transcriptToMarkdown(s.entries, 400);
        const size_t maxTail = 24000;
        if (md.size() > maxTail) {
            size_t cut = md.size() - maxTail;
            while (cut < md.size() && ((unsigned char)md[cut] & 0xC0) == 0x80) ++cut;
            md = "(…)\n" + md.substr(cut);
        }
        ctx += "\n## Échanges\n" + md;
    }
    return ctx;
}

OpenConversation* App::newConversation(ws::Folder* folder, const std::string& cwdOverride) {
    if (!folder) folder = workspace_.findFolderByPath(util::pathFromUtf8(selectedFolder_));
    if (!folder) folder = defaultFolder();
    auto oc = std::make_unique<OpenConversation>();
    oc->session = std::make_unique<claude::ClaudeSession>();
    auto& s = *oc->session;
    s.config = settings_.defaultConfig;
    s.cwd = cwdOverride;
    if (s.cwd.empty() && folder) s.cwd = folder->effectiveCwd();
    if (s.cwd.empty()) s.cwd = settings_.defaultCwd.empty() ? util::pathToUtf8(util::homeDir()) : settings_.defaultCwd;
    wireSession(s);
    oc->folderPath = folder ? util::pathToUtf8(folder->path) : "";
    oc->injectContext = settings_.injectContext;
    oc->meta.id = s.localId;
    open_.push_back(std::move(oc));
    selectTab_ = (int)open_.size() - 1;
    active_ = selectTab_;
    settings_.pushRecentCwd(open_.back()->session->cwd);
    return open_.back().get();
}

OpenConversation* App::openConversation(const std::string& id) {
    for (size_t i = 0; i < open_.size(); ++i)
        if (open_[i]->session->localId == id) {
            selectTab_ = (int)i;
            return open_[i].get();
        }
    auto [folder, meta] = workspace_.findConversation(id);
    if (!meta) return nullptr;
    json j;
    if (!workspace_.loadConversation(*meta, j)) {
        toast("Lecture impossible : " + util::pathToUtf8(meta->file), true);
        return nullptr;
    }
    auto oc = std::make_unique<OpenConversation>();
    oc->session = std::make_unique<claude::ClaudeSession>();
    auto& s = *oc->session;
    s.fromJson(j);
    if (s.cwd.empty()) s.cwd = folder->effectiveCwd();
    wireSession(s);
    oc->folderPath = util::pathToUtf8(folder->path);
    oc->meta = *meta;
    oc->injectContext = j.value("injectContext", settings_.injectContext);
    oc->lastSave = now_;
    open_.push_back(std::move(oc));
    selectTab_ = (int)open_.size() - 1;
    return open_.back().get();
}

OpenConversation* App::openImported(const claude::HistorySession& hs, ws::Folder* folder) {
    // Déjà importée ?
    std::function<std::string(ws::Folder*)> find = [&](ws::Folder* f) -> std::string {
        for (auto& c : f->conversations)
            if (c.claudeSessionId == hs.sessionId) return c.id;
        for (auto& ch : f->children)
            if (auto r = find(ch.get()); !r.empty()) return r;
        return {};
    };
    for (auto& p : workspace_.projects())
        if (auto id = find(p.get()); !id.empty()) {
            toast("Cette session est déjà dans l'espace de travail.");
            return openConversation(id);
        }
    OpenConversation* oc = newConversation(folder, hs.cwd);
    auto& s = *oc->session;
    s.entries = claude::loadHistoryTranscript(hs.file);
    s.claudeSessionId = hs.sessionId;
    s.title = hs.title;
    s.addSystemNote("Session importée depuis Claude Code (" + util::humanDate(hs.end) +
                    "). Votre prochain message la reprendra avec tout son contexte.");
    oc->meta.created = hs.start;
    oc->injectContext = false;  // la session reprise a déjà son propre contexte
    saveConversation(*oc);
    return oc;
}

void App::closeConversation(int index) {
    if (index < 0 || index >= (int)open_.size()) return;
    auto& oc = *open_[index];
    saveConversation(oc);
    if (settings_.autoRecap && oc.meta.messages >= 2 && oc.meta.recapTime < oc.meta.updated - 60)
        requestRecap(oc.session->localId);
    oc.session->stop();
    open_.erase(open_.begin() + index);
    if (active_ >= (int)open_.size()) active_ = (int)open_.size() - 1;
}

void App::saveConversation(OpenConversation& oc) {
    auto& s = *oc.session;
    if (s.entries.empty()) return;
    ws::Folder* f = folderOf(oc);
    if (!f) {
        f = defaultFolder();
        if (!f) return;
        oc.folderPath = util::pathToUtf8(f->path);
    }
    json j = s.toJson();
    j["injectContext"] = oc.injectContext;
    if (ws::ConversationMeta* m = workspace_.saveConversation(f, j, &oc.meta)) oc.meta = *m;
    s.dirty = false;
    oc.lastSave = now_;
}

void App::saveAll() {
    for (auto& oc : open_)
        if (oc->session->dirty) saveConversation(*oc);
}

void App::sendText(OpenConversation& oc, const std::string& text, const std::vector<claude::ImageAttachment>& images) {
    if (util::trim(text).empty() && images.empty()) return;
    auto& s = *oc.session;
    if (claudeExe_.empty()) {
        toast("Claude Code introuvable : configurez son chemin (Configuration › Application).", true);
        return;
    }
    s.claudeExe = claudeExe_;
    if (!s.processRunning()) s.extraContext = contextFor(oc);
    settings_.pushPromptHistory(text);
    s.send(text, images);
    oc.scrollToBottom = true;
    saveConversation(oc);
}

void App::sendDraft(OpenConversation& oc) {
    std::string text = oc.draft;
    if (util::trim(text).empty() && oc.attachments.empty()) return;
    if (claudeExe_.empty()) {
        toast("Claude Code introuvable : configurez son chemin (Configuration › Application).", true);
        return;
    }
    sendText(oc, text, oc.attachments);
    oc.draft.clear();
    oc.replaceDraft.clear();
    oc.hasReplace = true;
    oc.attachments.clear();
    oc.focusEditor = true;
    oc.scrollToBottom = true;
    oc.historyIndex = -1;
    saveConversation(oc);
}

void App::attachFile(OpenConversation& oc, const std::string& path) {
    std::string ext = util::toLower(util::pathToUtf8(util::pathFromUtf8(path).extension()));
    std::string mime = ext == ".png" ? "image/png" : (ext == ".jpg" || ext == ".jpeg") ? "image/jpeg"
                       : ext == ".gif" ? "image/gif" : ext == ".webp" ? "image/webp" : "";
    if (!mime.empty()) {
        std::vector<unsigned char> data;
        if (!util::readBinary(util::pathFromUtf8(path), data)) {
            toast("Lecture impossible : " + path, true);
            return;
        }
        if (data.size() > 5 * 1024 * 1024) {
            toast("Image trop lourde (5 Mo max).", true);
            return;
        }
        oc.attachments.push_back({mime, util::base64Encode(data), util::pathToUtf8(util::pathFromUtf8(path).filename())});
        toast("Image jointe : " + oc.attachments.back().name);
    } else {
        // Fichier texte / dossier : on insère sa référence pour que Claude le lise.
        std::string ref = "@\"" + path + "\" ";
        oc.draft += (oc.draft.empty() || oc.draft.back() == ' ' || oc.draft.back() == '\n' ? "" : " ") + ref;
        oc.replaceDraft = oc.draft;
        oc.hasReplace = true;
        oc.cursorToEnd = true;
    }
    oc.focusEditor = true;
}

void App::exportMarkdown(OpenConversation& oc) {
    std::string md = "# " + oc.session->title + "\n\n";
    if (!oc.meta.recap.empty()) md += "## Récapitulatif\n\n" + oc.meta.recap + "\n\n---\n\n";
    md += claude::transcriptToMarkdown(oc.session->entries, 2000);
    fs::path out = oc.meta.file.empty() ? util::documentsDir() / (util::sanitizeFileName(oc.session->title) + ".md")
                                        : fs::path(oc.meta.file).replace_extension("").replace_extension(".md");
    if (util::writeFileAtomic(out, md)) {
        toast("Exporté : " + util::pathToUtf8(out));
        util::openInShell(util::pathToUtf8(out));
    } else {
        toast("Export impossible.", true);
    }
}

// ---------------------------------------------------------------------------
// Récaps (cowork)
// ---------------------------------------------------------------------------

void App::requestRecap(const std::string& convId) {
    if (claudeExe_.empty()) return;
    for (auto& j : jobs_)
        if (j->target == convId) return;  // déjà en cours
    std::vector<claude::Entry> entries;
    std::string title, folderPath, cwd;
    OpenConversation* openOc = nullptr;
    for (auto& oc : open_)
        if (oc->session->localId == convId) openOc = oc.get();
    if (openOc) {
        saveConversation(*openOc);
        entries = openOc->session->entries;
        title = openOc->session->title;
        cwd = openOc->session->cwd;
        if (auto* f = folderOf(*openOc)) folderPath = f->displayPath();
    } else {
        auto [folder, meta] = workspace_.findConversation(convId);
        if (!meta) return;
        json j;
        if (!workspace_.loadConversation(*meta, j)) return;
        claude::ClaudeSession tmp;
        tmp.fromJson(j);
        entries = std::move(tmp.entries);
        title = meta->title;
        cwd = tmp.cwd;
        folderPath = folder->displayPath();
    }
    if (entries.size() < 2) {
        toast("Conversation trop courte pour un récap.");
        return;
    }
    std::error_code ec;
    if (cwd.empty() || !fs::is_directory(util::pathFromUtf8(cwd), ec)) cwd = util::pathToUtf8(util::homeDir());
    auto job = std::make_unique<claude::OneShot>();
    job->label = "Récap : " + util::truncateUtf8(title, 40);
    job->target = convId;
    job->onDone = [this, convId](claude::OneShot& j) {
        if (!j.ok()) {
            toast("Échec du récap : " + util::truncateUtf8(j.error(), 300), true);
            return;
        }
        auto [folder, meta] = workspace_.findConversation(convId);
        if (!meta) return;
        ws::ConversationMeta m = *meta;
        m.recap = util::trim(j.result());
        m.recapTime = util::nowUnix();
        for (auto& oc : open_)
            if (oc->session->localId == convId) {
                oc->meta.recap = m.recap;
                oc->meta.recapTime = m.recapTime;
                m.title = oc->session->title;
            }
        workspace_.updateConversationMeta(m);
        toast("Récap prêt : " + util::truncateUtf8(m.title, 50));
    };
    std::string prompt = claude::conversationRecapPrompt(title, folderPath, claude::transcriptToMarkdown(entries, 300));
    if (job->start(claudeExe_, settings_.recapModel, prompt, cwd)) jobs_.push_back(std::move(job));
}

void App::requestFolderRecap(ws::Folder* f) {
    if (!f || claudeExe_.empty()) return;
    std::string recaps;
    std::function<void(ws::Folder*, int)> collect = [&](ws::Folder* x, int depth) {
        for (auto& c : x->conversations)
            if (!c.recap.empty()) recaps += "\n## " + x->displayPath() + " — " + c.title + "\n" + c.recap + "\n";
        if (depth < 3)
            for (auto& ch : x->children) collect(ch.get(), depth + 1);
    };
    collect(f, 0);
    if (recaps.empty()) {
        toast("Aucun récap de conversation dans ce dossier : générez d'abord les récaps des conversations.", true);
        return;
    }
    std::string path = util::pathToUtf8(f->path);
    auto job = std::make_unique<claude::OneShot>();
    job->label = "Synthèse : " + f->name;
    job->target = path;
    job->onDone = [this, path](claude::OneShot& j) {
        if (!j.ok()) {
            toast("Échec de la synthèse : " + util::truncateUtf8(j.error(), 300), true);
            return;
        }
        if (ws::Folder* folder = workspace_.findFolderByPath(util::pathFromUtf8(path))) {
            folder->recap = util::trim(j.result());
            folder->recapTime = util::nowUnix();
            workspace_.saveFolderMeta(folder);
            toast("Synthèse prête : " + folder->name);
        }
    };
    std::string cwd = f->effectiveCwd();
    std::error_code ec;
    if (cwd.empty() || !fs::is_directory(util::pathFromUtf8(cwd), ec)) cwd = util::pathToUtf8(util::homeDir());
    if (job->start(claudeExe_, settings_.recapModel, claude::folderRecapPrompt(f->displayPath(), recaps), cwd))
        jobs_.push_back(std::move(job));
}

void App::refreshUsage() {
    if (claudeExe_.empty()) return;
    for (auto& j : jobs_)
        if (j->target == "#usage") return;
    auto job = std::make_unique<claude::OneShot>();
    job->label = "Actualisation de l'usage";
    job->target = "#usage";
    job->streamJson = true;
    job->onEvent = [this](const json& j) {
        if (j.value("type", "") == "rate_limit_event") usage_.onRateLimitEvent(j.value("rate_limit_info", json::object()));
    };
    job->onDone = [this](claude::OneShot& j) {
        if (!j.ok()) toast("Actualisation impossible : " + util::truncateUtf8(j.error(), 200), true);
    };
    if (job->start(claudeExe_, "haiku", "Réponds uniquement : ok", util::pathToUtf8(util::homeDir()))) jobs_.push_back(std::move(job));
}

// ---------------------------------------------------------------------------
// Boîtes de dialogue natives (SDL3)
// ---------------------------------------------------------------------------

namespace {
struct DialogCtx {
    App* app;
    std::mutex* mutex;
    std::vector<std::function<void()>>* results;
    std::function<void(const std::vector<std::string>&)> cb;
};
void SDLCALL dialogCallback(void* userdata, const char* const* filelist, int) {
    auto* ctx = static_cast<DialogCtx*>(userdata);
    std::vector<std::string> files;
    if (filelist)
        for (int i = 0; filelist[i]; ++i) files.emplace_back(filelist[i]);
    if (!files.empty()) {
        std::lock_guard lock(*ctx->mutex);
        auto cb = ctx->cb;
        ctx->results->push_back([cb, files] { cb(files); });
    }
    delete ctx;
}
}  // namespace

void App::openFolderDialog(std::function<void(const std::string&)> cb, const std::string& start) {
    auto* ctx = new DialogCtx{this, &dialogMutex_, &dialogResults_,
                              [cb](const std::vector<std::string>& f) { cb(f.front()); }};
    SDL_ShowOpenFolderDialog(dialogCallback, ctx, window_, start.empty() ? nullptr : start.c_str(), false);
}

void App::openFileDialog(std::function<void(const std::vector<std::string>&)> cb) {
    auto* ctx = new DialogCtx{this, &dialogMutex_, &dialogResults_, std::move(cb)};
    static const SDL_DialogFileFilter filters[] = {{"Images", "png;jpg;jpeg;gif;webp"}, {"Tous les fichiers", "*"}};
    SDL_ShowOpenFileDialog(dialogCallback, ctx, window_, filters, 2, nullptr, true);
}

// ---------------------------------------------------------------------------
// Raccourcis globaux
// ---------------------------------------------------------------------------

void App::handleGlobalShortcuts() {
    const ImGuiInputFlags g = ImGuiInputFlags_RouteGlobal;
    OpenConversation* oc = active();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, g)) newConversation(nullptr);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_W, g) && oc) closeConversation(active_);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, g)) {
        saveAll();
        settings_.save(settingsPath_);
        toast("Enregistré.");
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageDown, g) && !open_.empty()) selectTab_ = (active_ + 1) % (int)open_.size();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageUp, g) && !open_.empty())
        selectTab_ = (active_ + (int)open_.size() - 1) % (int)open_.size();
    if (ImGui::Shortcut(ImGuiKey_Escape, g) && oc && oc->session->state == claude::SessionState::Busy) oc->session->interrupt();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, g) && oc) oc->focusSearch = oc->showSearch = !oc->showSearch;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_L, g) && oc) oc->focusEditor = true;
    if (ImGui::Shortcut(ImGuiKey_F1, g)) showShortcuts_ = !showShortcuts_;
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F1, g)) showGuide_ = !showGuide_;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_1, g)) page_ = 0;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_2, g)) {
        if (dash_) SDL_RaiseWindow(dash_->window);
        else page_ = 1;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_D, g)) {
        if (dash_) closeDashboardWindow();
        else openDashboardWindow();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Comma, g)) showConfig_ = true, configTab_ = 0;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_R, g) && oc) requestRecap(oc->session->localId);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C, g) && oc) {
        for (auto it = oc->session->entries.rbegin(); it != oc->session->entries.rend(); ++it)
            if (it->kind == claude::EntryKind::Assistant && it->parentToolUseId.empty()) {
                ImGui::SetClipboardText(it->text.c_str());
                toast("Dernière réponse copiée.");
                break;
            }
    }
    auto setFont = [&](float size) {
        settings_.fontSize = std::clamp(size, 11.0f, 32.0f);
        ImGui::GetStyle().FontSizeBase = settings_.fontSize;
        for (auto& o : open_) o->cache.clear();
    };
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Equal, g) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, g))
        setFont(settings_.fontSize + 1);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Minus, g) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, g))
        setFont(settings_.fontSize - 1);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_0, g) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Keypad0, g)) setFont(17);
    // Ctrl + molette : taille du texte
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && io.MouseWheel != 0 && !io.WantTextInput) setFont(settings_.fontSize + (io.MouseWheel > 0 ? 1 : -1));
}

}  // namespace app
