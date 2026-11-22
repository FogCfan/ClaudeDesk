// Page « Designer » (Ctrl+4) : à partir d'une description, Claude conçoit le projet et l'équipe d'agents qui va le
// réaliser (par défaut en mode plan : il peut lire un dossier existant sans rien modifier). On ajuste la conception
// (à la main, ou en demandant à Claude d'affiner), puis ClaudeDesk crée le projet cowork, un thème par agent (sa
// fiche en notes), les fichiers de départ du dépôt et l'orchestrateur réglé.
//   1 Décrire → 2 Clarifier (si Claude a des questions) → 3 Ajuster → 4 Créer
// Le moteur (prompts, analyse, validation, fichiers) est dans claude/Designer.
#include <algorithm>
#include <filesystem>
#include <iterator>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/App.h"
#include "ui/Anim.h"
#include "ui/Theme.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace app {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const char* kDesignerJob = "#designer";
const char* kStepNames[] = {"Décrire", "Clarifier", "Ajuster", "Créer"};
const char* kTeamSizes[] = {"Auto", "Petite (2–3)", "Moyenne (4–6)", "Grande (7–10)"};
struct Opt {
    const char* value;
    const char* label;
};
const Opt kDesignerModels[] = {{"opus", "Opus — conception la plus fine (conseillé)"},
                               {"sonnet", "Sonnet — rapide et solide"},
                               {"fable", "Fable"},
                               {"haiku", "Haiku — esquisse rapide et économique"}};
const Opt kAgentModels[] = {{"haiku", "Haiku"}, {"sonnet", "Sonnet"}, {"opus", "Opus"}, {"fable", "Fable"}};
const Opt kAgentEfforts[] = {{"low", "low"}, {"medium", "medium"}, {"high", "high"}};
const Opt kBudgets[] = {{"low", "Bas"}, {"medium", "Moyen"}, {"high", "Élevé"}, {"max", "Max"}};

// Zone de texte multiligne avec retour à la ligne et texte indicatif quand elle est vide.
bool multiline(const char* id, std::string& s, float lines, const char* hint) {
    float h = ImGui::GetTextLineHeight() * lines + ImGui::GetStyle().FramePadding.y * 2;
    bool changed = ImGui::InputTextMultiline(id, &s, ImVec2(-1, h), ImGuiInputTextFlags_WordWrap);
    if (s.empty() && hint && !ImGui::IsItemActive()) {
        ImVec2 p = ImGui::GetItemRectMin();
        const ImGuiStyle& st = ImGui::GetStyle();
        float wrap = ImGui::GetItemRectSize().x - st.FramePadding.x * 2;
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                                            ImVec2(p.x + st.FramePadding.x, p.y + st.FramePadding.y),
                                            ui::col(ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)), hint, nullptr, wrap);
    }
    return changed;
}

bool beginCard(const char* id) {
    return ImGui::BeginChild(id, ImVec2(0, 0),
                             ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}

void cardTitle(const char* title, ImFont* bold) {
    ImGui::PushFont(bold, 0.0f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
}

void dimWrapped(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::PopStyleColor();
}

bool optCombo(const char* id, std::string& v, const Opt* opts, size_t n, float width) {
    const char* cur = v.c_str();
    for (size_t i = 0; i < n; ++i)
        if (v == opts[i].value) cur = opts[i].label;
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(id, cur)) {
        for (size_t i = 0; i < n; ++i)
            if (ImGui::Selectable(opts[i].label, v == opts[i].value)) v = opts[i].value, changed = true;
        ImGui::EndCombo();
    }
    return changed;
}

// État d'un dossier visé : 0 non renseigné, 1 n'existe pas, 2 vide, 3 non vide, 4 n'est pas un dossier.
int folderState(const std::string& folder, int* count = nullptr) {
    std::string f = util::trim(folder);
    if (f.empty()) return 0;
    std::error_code ec;
    fs::path p = util::pathFromUtf8(f);
    if (!fs::exists(p, ec)) return 1;
    if (!fs::is_directory(p, ec)) return 4;
    int n = 0;
    for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (++n >= 999) break;
    if (count) *count = n;
    return n == 0 ? 2 : 3;
}

std::string baseName(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

// Ce que fait Claude, en une ligne (fil d'activité pendant la conception).
std::string describeTool(const json& c) {
    std::string name = c.value("name", "");
    const json in = c.value("input", json::object());
    if (name == "Read") return "Lecture : " + baseName(in.value("file_path", ""));
    if (name == "Glob") return "Recherche de fichiers : " + in.value("pattern", "");
    if (name == "Grep") return "Recherche : « " + util::truncateUtf8(in.value("pattern", ""), 60) + " »";
    if (name == "Bash" || name == "PowerShell") return "Commande : " + util::truncateUtf8(util::firstLine(in.value("command", ""), 70), 70);
    if (name == "ExitPlanMode") return "Plan rendu";
    return name;
}

std::string lowerStr(const std::string& s) { return util::toLower(s); }

}  // namespace

// ---------------------------------------------------------------------------
// Persistance
// ---------------------------------------------------------------------------

void App::loadDesigner() {
    std::string txt;
    if (util::readFile(util::pathFromUtf8(designerPath_), txt)) designer_.fromJson(json::parse(txt, nullptr, false));
}

void App::saveDesigner() {
    if (designerPath_.empty()) return;
    util::writeFileAtomic(util::pathFromUtf8(designerPath_), designer_.toJson().dump(1));
    designerDirty_ = false;
    designerSaved_ = now_;
}

bool App::designerBusy() const {
    for (auto& j : jobs_)
        if (j->target == kDesignerJob && !j->finished()) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void App::designerRun(bool refine) {
    if (claudeExe_.empty()) {
        toast("Claude Code introuvable : le designer ne peut pas travailler.", true);
        return;
    }
    if (designerBusy()) return;
    auto& d = designer_;
    std::string prompt = refine ? claude::refinePrompt(d.req, d.design, d.feedback) : claude::designPrompt(d.req);
    auto job = std::make_unique<claude::OneShot>();
    job->label = refine ? "Designer : affinage" : "Designer : conception";
    job->target = kDesignerJob;
    job->streamJson = true;
    job->permissionMode = d.permissionMode;
    job->effort = d.effort;
    // Les outils (lecture du dossier) ne servent que pour un dossier existant à analyser ; sinon, aucun outil.
    std::string cwd = util::pathToUtf8(util::homeDir());
    if (d.req.analyzeFolder && folderState(d.req.folder) == 3) {
        job->defaultTools = true;
        cwd = util::trim(d.req.folder);
    }
    designerActivity_.clear();
    designerError_.clear();
    designerRaw_.clear();
    designerPlanText_.clear();
    designerStarted_ = now_;
    designerRefining_ = refine;
    job->onEvent = [this](const json& j) {
        if (j.value("type", "") != "assistant" || !j.contains("message")) return;
        for (auto& c : j["message"].value("content", json::array())) {
            std::string t = c.value("type", "");
            std::string line;
            if (t == "tool_use") {
                line = describeTool(c);
                // Mode plan : la conception peut arriver dans le plan rendu plutôt que dans la réponse finale.
                if (c.value("name", "") == "ExitPlanMode" && c.contains("input"))
                    designerPlanText_ = c["input"].value("plan", "");
            } else if (t == "text") {
                line = "Rédaction de la conception…";
            } else if (t == "thinking") {
                line = "Réflexion…";
            }
            if (line.empty() || (!designerActivity_.empty() && designerActivity_.back() == line)) continue;
            designerActivity_.push_back(line);
            if (designerActivity_.size() > 60) designerActivity_.erase(designerActivity_.begin());
        }
    };
    job->onDone = [this, refine](claude::OneShot& j) {
        designer_.lastCostUsd = j.costUsd();
        if (j.error() == "Annulé") {
            designerActivity_.push_back("Annulé.");
            return;
        }
        claude::DesignResponse r = claude::parseDesignResponse(j.result());
        if (!r.ok && !designerPlanText_.empty()) r = claude::parseDesignResponse(designerPlanText_);
        if (!r.ok) {
            designerError_ = (!j.ok() && j.result().empty()) ? j.error() : r.error;
            designerRaw_ = j.result().empty() ? designerPlanText_ : j.result();
            toast("Designer : " + util::truncateUtf8(designerError_, 200), true);
            return;
        }
        auto& d = designer_;
        if (r.hasDesign) {
            if (!d.design.empty()) {
                d.history.push_back(d.design);
                if (d.history.size() > 10) d.history.erase(d.history.begin());
            }
            d.design = r.design;
            d.questions.clear();
            d.step = 2;
            d.createdProject.clear();
            if (refine) d.feedback.clear();
            toast(refine ? "Conception affinée." : "Conception prête : relisez-la et ajustez-la, puis créez le projet.");
        } else {
            d.questions = r.questions;
            d.step = 1;
        }
        designerDirty_ = true;
        saveDesigner();
    };
    if (job->start(claudeExe_, d.model, prompt, cwd)) jobs_.push_back(std::move(job));
}

void App::designerCancel() {
    for (auto& j : jobs_)
        if (j->target == kDesignerJob) j->cancel();
}

// Dossier de travail proposé à la création : celui de la demande, sinon <parent habituel>\<nom du projet>.
std::string App::designerSuggestedCwd() {
    const auto& d = designer_;
    if (!util::trim(d.req.folder).empty()) return util::trim(d.req.folder);
    fs::path parent;
    for (auto& p : workspace_.projects())
        if (!p->cwd.empty()) {
            parent = util::pathFromUtf8(p->cwd).parent_path();
            if (!parent.empty()) break;
        }
    if (parent.empty()) parent = util::homeDir() / "Projets";
    std::string name = util::sanitizeFileName(d.design.name);
    return util::pathToUtf8(parent / util::pathFromUtf8(name.empty() ? "Nouveau projet" : name));
}

void App::designerCreate() {
    auto& d = designer_;
    claude::DesignCheck check = claude::checkDesign(d.design);
    if (!check.errors.empty()) {
        toast(check.errors.front(), true);
        return;
    }
    std::string cwd = util::trim(d.cwd);
    if (cwd.empty()) {
        toast("Choisissez le dossier de travail du projet.", true);
        return;
    }
    for (auto& p : workspace_.projects())
        if (lowerStr(p->name) == lowerStr(util::trim(d.design.name))) {
            toast("Un projet « " + p->name + " » existe déjà dans l'espace de travail : renommez celui-ci.", true);
            return;
        }
    std::error_code ec;
    fs::path root = util::pathFromUtf8(cwd);
    fs::create_directories(root, ec);
    if (!fs::is_directory(root, ec)) {
        toast("Impossible de créer le dossier de travail : " + cwd, true);
        return;
    }
    std::string err;
    ws::Folder* proj = workspace_.createProject(util::trim(d.design.name), cwd, &err);
    if (!proj) {
        toast("Création du projet impossible : " + err, true);
        return;
    }
    proj->notes = claude::projectNotes(d.design);
    workspace_.saveFolderMeta(proj);
    int agents = 0;
    for (auto& a : d.design.agents) {
        ws::Folder* f = workspace_.createFolder(proj, util::trim(a.name), &err);
        if (!f) {
            toast("Agent « " + a.name + " » : " + err, true);
            continue;
        }
        f->notes = claude::agentNotes(a);
        workspace_.saveFolderMeta(f);
        ++agents;
    }
    // Fichiers de départ : jamais d'écrasement d'un fichier existant.
    int written = 0, kept = 0;
    for (auto& sf : claude::scaffoldFiles(d.design, d.writeClaudeMd, d.writePlan, d.writeAgentDocs)) {
        fs::path fp = root / util::pathFromUtf8(sf.path);
        if (fs::exists(fp, ec)) {
            ++kept;
            continue;
        }
        fs::create_directories(fp.parent_path(), ec);
        if (util::writeFileAtomic(fp, sf.content)) ++written;
    }
    // Orchestrateur réglé d'après la conception (niveau de budget, objectif et jalons, visuel, navigateur).
    {
        claude::Orchestrator o;
        claude::applyDesign(d.design, o.settings);
        util::writeFileAtomic(proj->path / "_orchestrateur.json", o.toJson().dump(1));
    }
    d.createdProject = util::pathToUtf8(proj->path);
    selectedFolder_ = d.createdProject;
    designerDirty_ = true;
    saveDesigner();

    std::string msg = "Projet « " + proj->name + " » créé : " + std::to_string(agents) + " agents";
    if (written || kept)
        msg += ", " + std::to_string(written) + " fichier(s) de départ" + (kept ? " (" + std::to_string(kept) + " déjà présents, conservés)" : "");
    toast(msg + ".");
    if (orch_ && orch_->running()) {
        toast("Une orchestration est en cours : celle de « " + proj->name +
              " » est prête ; choisissez ce projet dans la page Orchestrateur quand vous voudrez.");
        return;
    }
    if (!loadOrchestrator(proj)) return;
    if (d.openOrchestrator || d.startNow) page_ = 2;
    if (d.startNow) orchestratorStart();
}

void App::designerReset() {
    claude::DesignerDraft fresh;
    // Les réglages du designer et des options de création sont gardés.
    fresh.model = designer_.model;
    fresh.effort = designer_.effort;
    fresh.permissionMode = designer_.permissionMode;
    fresh.req.budget = designer_.req.budget;
    fresh.req.allowQuestions = designer_.req.allowQuestions;
    fresh.openOrchestrator = designer_.openOrchestrator;
    designer_ = fresh;
    designerError_.clear();
    designerRaw_.clear();
    designerActivity_.clear();
    designerDirty_ = true;
}

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------

void App::drawDesignerPage() {
    const auto& pal = ui::palette();
    const float fs = ImGui::GetFontSize();
    auto& d = designer_;
    const bool busy = designerBusy();
    if (designerDirty_ && now_ - designerSaved_ > 2.0) saveDesigner();
    auto dirtyIf = [&](bool c) {
        if (c) designerDirty_ = true;
        return c;
    };

    // --- En-tête ---
    ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.45f);
    ImGui::TextColored(pal.accent, "Designer");
    ImGui::PopFont();
    ImGui::SameLine(0, fs);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Décrivez un projet : Claude conçoit l'équipe d'agents et règle l'orchestrateur.");
    {
        const char* reset = designerConfirmReset_ ? "Effacer ce design ?" : "Nouveau design";
        float bw = ui::buttonWidth(reset) + ui::buttonWidth("Cockpit") + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + fs, ImGui::GetWindowContentRegionMax().x - bw));
        ImGui::BeginDisabled(busy);
        if (designerConfirmReset_) ImGui::PushStyleColor(ImGuiCol_Text, pal.warn);
        if (ImGui::Button(reset)) {
            if (designerConfirmReset_ || (d.design.empty() && util::trim(d.req.description).empty())) {
                designerReset();
                designerConfirmReset_ = false;
            } else {
                designerConfirmReset_ = true;
                designerConfirmAt_ = now_;
            }
        }
        if (designerConfirmReset_) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        if (designerConfirmReset_ && now_ - designerConfirmAt_ > 4) designerConfirmReset_ = false;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Repartir d'une page blanche (les réglages du designer sont gardés).");
        ImGui::SameLine();
        if (ImGui::Button("Cockpit")) page_ = 0;
    }

    // --- Étapes ---
    ImGui::Spacing();
    for (int i = 0; i < 4; ++i) {
        bool reachable = i == 0 || (i == 1 && !d.questions.empty()) || (i >= 2 && !d.design.empty());
        bool current = d.step == i;
        bool done = i < d.step && reachable;
        if (i > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("›");
            ImGui::SameLine();
        }
        std::string label = std::string(done ? "✓ " : "") + std::to_string(i + 1) + "  " + kStepNames[i] + "##step" + std::to_string(i);
        ImGui::PushStyleColor(ImGuiCol_Button, current ? pal.accentSoft : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, current ? pal.accent : done ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                                                       : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::BeginDisabled(!reachable || busy);
        if (ImGui::Button(label.c_str()) && !current) {
            d.step = i;
            designerDirty_ = true;
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        if (i == 1 && !reachable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Seulement si Claude a besoin de précisions avant de concevoir.");
    }
    ImGui::Separator();

    // --- Conception en cours ---
    if (busy) {
        ImGui::Spacing();
        beginCard("##dsgbusy");
        ui::spinner((float)now_, pal.accent);
        ImGui::SameLine();
        int el = (int)(now_ - designerStarted_);
        ImGui::Text("%s… %d:%02d", designerRefining_ ? "Claude affine la conception" : "Claude conçoit le projet", el / 60, el % 60);
        ImGui::SameLine();
        ImGui::TextDisabled("· %s · mode %s", d.model.c_str(), d.permissionMode.c_str());
        float cw = ui::buttonWidth("Annuler");
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - cw));
        if (ImGui::Button("Annuler")) designerCancel();
        size_t from = designerActivity_.size() > 6 ? designerActivity_.size() - 6 : 0;
        for (size_t i = from; i < designerActivity_.size(); ++i) {
            bool last = i + 1 == designerActivity_.size();
            if (last) ImGui::TextUnformatted(("› " + designerActivity_[i]).c_str());
            else ImGui::TextDisabled("  %s", designerActivity_[i].c_str());
        }
        if (designerActivity_.empty()) ImGui::TextDisabled("  Démarrage de Claude Code…");
        ImGui::EndChild();
        ui::anim::markBusy();
    } else if (!designerError_.empty()) {
        ImGui::Spacing();
        beginCard("##dsgerr");
        ImGui::TextColored(pal.err, "La conception n'a pas abouti : %s", designerError_.c_str());
        if (!designerRaw_.empty() && ImGui::TreeNode("Voir la réponse de Claude")) {
            std::string raw = designerRaw_;
            ImGui::InputTextMultiline("##raw", &raw, ImVec2(-1, fs * 10), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
            ImGui::TreePop();
        }
        if (ImGui::SmallButton("Réessayer")) designerRun(designerRefining_);
        ImGui::SameLine();
        if (ImGui::SmallButton("Fermer")) designerError_.clear();
        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::BeginChild("##dsgbody", ImVec2(0, 0), ImGuiChildFlags_None);
    const float avail = ImGui::GetContentRegionAvail().x;
    const bool wide = avail > fs * 52;
    const bool subNow = !usage_.subscription.empty() || usage_.find("five_hour") != nullptr;
    ImGui::BeginDisabled(busy);

    // ======================= 1. Décrire =======================
    if (d.step == 0) {
        float leftW = wide ? std::max(fs * 30, avail * 0.58f) : avail;
        ImGui::BeginChild("##dsgdesc", ImVec2(leftW, 0));
        cardTitle("Que voulez-vous construire ?", fontBold);
        dirtyIf(multiline("##desc", d.req.description, 7,
                          "Ex. : une application web de recettes avec planning de la semaine et liste de courses ; un "
                          "jeu de Pong en C++ avec SDL3 ; un outil en ligne de commande qui renomme des photos d'après "
                          "leur date… Plus c'est précis (public, plateforme, fonctionnalités clés), meilleure est l'équipe."));
        ImGui::Spacing();
        cardTitle("Dossier de travail", fontBold);
        float browseW = ui::buttonWidth("Parcourir…");
        ImGui::SetNextItemWidth(std::max(fs * 8, ImGui::GetContentRegionAvail().x - browseW - ImGui::GetStyle().ItemSpacing.x));
        std::string before = d.req.folder;
        if (dirtyIf(ImGui::InputTextWithHint("##folder", "Facultatif : un dossier existant à reprendre, ou à créer", &d.req.folder)))
            if (folderState(d.req.folder) == 3 && folderState(before) != 3) d.req.analyzeFolder = true;
        ImGui::SameLine();
        if (ImGui::Button("Parcourir…"))
            openFolderDialog([this](const std::string& p) {
                designer_.req.folder = p;
                designer_.req.analyzeFolder = folderState(p) == 3;
                designerDirty_ = true;
            });
        int count = 0;
        switch (folderState(d.req.folder, &count)) {
            case 0: ImGui::TextDisabled("Vous pourrez aussi le choisir à la dernière étape."); break;
            case 1: ImGui::TextColored(pal.ok, "✚ Nouveau dossier : il sera créé avec le projet."); break;
            case 2: ImGui::TextDisabled("Dossier existant, vide."); break;
            case 3:
                ImGui::TextDisabled("Dossier existant (%s%d éléments).", count >= 999 ? "plus de " : "", count);
                dirtyIf(ImGui::Checkbox("Analyser son contenu", &d.req.analyzeFolder));
                ImGui::SameLine();
                ui::helpMarker("Le designer lit la structure, le README, les fichiers de build et le code principal\n"
                               "(lecture seule, une quinzaine de fichiers au plus) pour bâtir l'équipe sur l'existant :\n"
                               "les agents se partagent alors le code déjà là.");
                break;
            default: ImGui::TextColored(pal.err, "Ce chemin n'est pas un dossier."); break;
        }

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Préférences (facultatif)", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextUnformatted("Technologies imposées");
            ImGui::SetNextItemWidth(-1);
            dirtyIf(ImGui::InputTextWithHint("##tech", "Ex. : C++20 et SDL3 — vide : le designer choisit", &d.req.tech));
            ImGui::TextUnformatted("Contraintes");
            dirtyIf(multiline("##constraints", d.req.constraints, 2,
                              "Ex. : doit tourner hors ligne ; aucune dépendance payante ; Windows et macOS"));
            ImGui::TextUnformatted("Taille de l'équipe");
            for (int i = 0; i < 4; ++i) {
                if (i) ui::sameLineIfFits(ImGui::CalcTextSize(kTeamSizes[i]).x + ImGui::GetFrameHeight() * 1.5f);
                if (ImGui::RadioButton(kTeamSizes[i], d.req.teamSize == i)) d.req.teamSize = i, designerDirty_ = true;
            }
            ImGui::TextUnformatted("Budget de l'orchestration");
            ImGui::SameLine();
            ui::helpMarker("Le niveau de budget de l'orchestrateur (modèles autorisés, part du quota, agents en\n"
                           "parallèle). Le designer en tient compte pour dimensionner l'équipe ; modifiable ensuite.");
            for (size_t i = 0; i < 4; ++i) {
                claude::BudgetPreset p = claude::budgetPresetFromName(kBudgets[i].value);
                const char* lbl = claude::budgetPresetLabel(p, subNow);
                if (i) ui::sameLineIfFits(ImGui::CalcTextSize(lbl).x + ImGui::GetFrameHeight() * 1.5f);
                if (ImGui::RadioButton(lbl, d.req.budget == kBudgets[i].value)) d.req.budget = kBudgets[i].value, designerDirty_ = true;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", claude::budgetPresetSummary(p, subNow).c_str());
            }
            dirtyIf(ImGui::Checkbox("Claude peut d'abord poser des questions", &d.req.allowQuestions));
            ImGui::SameLine();
            ui::helpMarker("S'il manque une information qui change vraiment la conception (plateforme, langage,\n"
                           "périmètre), Claude pose jusqu'à 4 questions avec des réponses à cliquer. Sinon, il fait\n"
                           "des choix raisonnables et les signale dans le résumé.");
        }
        std::string effortLbl = d.effort.empty() ? "effort par défaut" : "effort " + d.effort;
        std::string head = "Réglages du designer — " + d.model + " · mode " + d.permissionMode + " · " + effortLbl + "###dsgsettings";
        if (ImGui::CollapsingHeader(head.c_str())) {
            float w = std::min(fs * 20, ImGui::GetContentRegionAvail().x * 0.6f);
            dirtyIf(optCombo("Modèle##dsg", d.model, kDesignerModels, std::size(kDesignerModels), w));
            {
                const auto& ch = claude::effortChoices();
                const char* cur = ch.front().label;
                for (auto& c : ch)
                    if (d.effort == c.value) cur = c.label;
                ImGui::SetNextItemWidth(w);
                if (ImGui::BeginCombo("Effort##dsg", cur)) {
                    for (auto& c : ch)
                        if (ImGui::Selectable(c.label, d.effort == c.value)) d.effort = c.value, designerDirty_ = true;
                    ImGui::EndCombo();
                }
            }
            {
                const char* cur = d.permissionMode.c_str();
                for (auto& c : claude::permissionModeChoices())
                    if (d.permissionMode == c.value) cur = c.label;
                ImGui::SetNextItemWidth(w);
                if (ImGui::BeginCombo("Mode##dsg", cur)) {
                    for (auto& c : claude::permissionModeChoices()) {
                        if (std::string(c.value) == "bypassPermissions") continue;
                        if (ImGui::Selectable(c.label, d.permissionMode == c.value)) d.permissionMode = c.value, designerDirty_ = true;
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.help);
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ui::helpMarker("Plan (par défaut) : Claude lit le dossier et conçoit sans rien modifier.\n"
                               "Le designer ne fait qu'une conception : c'est ClaudeDesk qui crée le projet et les fichiers\n"
                               "à la dernière étape. Les autres modes ne changent que ce qu'il peut faire en lisant le dossier.");
            }
            if (d.permissionMode != "plan")
                ImGui::TextColored(pal.warn, "Hors mode plan, Claude pourrait modifier le dossier analysé pendant la conception.");
        }

        ImGui::Spacing();
        bool canRun = !util::trim(d.req.description).empty() && !busy && folderState(d.req.folder) != 4;
        ImGui::BeginDisabled(!canRun);
        ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        bool go = ImGui::Button("✦  Concevoir le projet", ImVec2(std::min(fs * 16, ImGui::GetContentRegionAvail().x), fs * 2.2f));
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Ctrl+Entrée");
        if (canRun && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)) go = true;
        if (go && canRun) {
            d.req.answered.clear();  // nouvelle description : les anciennes réponses ne valent plus
            designerRun(false);
        }
        dimWrapped("Compter 1 à 3 minutes avec Opus (davantage en analysant un gros dossier). La conception consomme du "
                   "quota comme une conversation ; rien n'est créé avant l'étape 4.");
        ImGui::EndChild();

        if (wide) {
            ImGui::SameLine();
            ImGui::BeginChild("##dsghelp", ImVec2(0, 0));
            beginCard("##dsgwhat");
            cardTitle("Ce que le designer va produire", fontBold);
            ImGui::Spacing();
            auto item = [&](const char* icon, const char* title, const char* text) {
                ImGui::TextColored(pal.accent, "%s", icon);
                ImGui::SameLine();
                ImGui::PushFont(fontBold, 0.0f);
                ImGui::TextUnformatted(title);
                ImGui::PopFont();
                ImGui::Indent(fs * 1.4f);
                dimWrapped(text);
                ImGui::Unindent(fs * 1.4f);
                ImGui::Spacing();
            };
            item("◆", "Le projet", "Nom, résumé, technologies et conventions (build, tests, style).");
            item("◆", "L'équipe d'agents", "En général 3 à 6 agents. Chacun possède une partie du dépôt et reçoit une "
                                         "fiche de rôle : sa mission, ses fichiers, ses frontières, ses vérifications, le "
                                         "modèle conseillé. Un agent peut être équipé du navigateur.");
            item("◆", "L'orchestrateur", "Son objectif (livrable et critères de fin), les jalons dans l'ordre, le "
                                       "niveau de budget.");
            item("◆", "Les fichiers de départ", "CLAUDE.md, docs/PLAN.md et une fiche par agent dans docs/agents/ — "
                                              "jamais d'écrasement d'un fichier existant.");
            ImGui::Separator();
            dimWrapped("Vous relisez et ajustez tout à l'étape 3, à la main ou en demandant à Claude d'affiner. Le "
                       "projet est créé à l'étape 4, prêt à être orchestré.");
            ImGui::EndChild();
            if (!d.design.empty()) {
                ImGui::Spacing();
                beginCard("##dsgresume");
                ImGui::Text("Conception en cours : %s (%d agents)", d.design.name.c_str(), (int)d.design.agents.size());
                if (ImGui::SmallButton("Reprendre l'ajustement ›")) d.step = 2;
                ImGui::EndChild();
            }
            ImGui::EndChild();
        }
    }

    // ======================= 2. Clarifier =======================
    else if (d.step == 1) {
        float w = std::min(avail, fs * 46);
        ImGui::BeginChild("##dsgq", ImVec2(w, 0));
        cardTitle("Quelques précisions avant de concevoir", fontBold);
        dimWrapped("Ces réponses changent la conception. La réponse recommandée par Claude est présélectionnée.");
        ImGui::Spacing();
        for (size_t qi = 0; qi < d.questions.size(); ++qi) {
            auto& q = d.questions[qi];
            ImGui::PushID((int)qi);
            beginCard("##q");
            ImGui::PushFont(fontBold, 0.0f);
            ImGui::TextWrapped("%d. %s", (int)qi + 1, q.question.c_str());
            ImGui::PopFont();
            if (!q.why.empty()) dimWrapped(q.why);
            for (size_t oi = 0; oi < q.options.size(); ++oi) {
                std::string lbl = q.options[oi] + ((int)oi == q.recommended ? "   (recommandé)" : "");
                if (ImGui::RadioButton(lbl.c_str(), q.choice == (int)oi)) q.choice = (int)oi, designerDirty_ = true;
            }
            if (ImGui::RadioButton("Autre :", q.choice == -2)) q.choice = -2, designerDirty_ = true;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##other", "votre réponse", &q.other)) q.choice = -2, designerDirty_ = true;
            if (ImGui::RadioButton("Au choix de Claude", q.choice == -1)) q.choice = -1, designerDirty_ = true;
            ImGui::EndChild();
            ImGui::PopID();
            ImGui::Spacing();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        bool go = ImGui::Button("✦  Concevoir avec ces réponses", ImVec2(0, fs * 2.2f));
        ImGui::PopStyleColor(2);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)) go = true;
        ImGui::SameLine();
        if (ImGui::Button("Laisser Claude trancher", ImVec2(0, fs * 2.2f))) {
            for (auto& q : d.questions) q.choice = -1;
            go = true;
        }
        if (go && !busy) {
            d.req.answered = d.questions;
            designerRun(false);
        }
        ImGui::SameLine();
        if (ImGui::Button("‹ Modifier la description", ImVec2(0, fs * 2.2f))) d.step = 0;
        ImGui::EndChild();
    }

    // ======================= 3. Ajuster =======================
    else if (d.step == 2) {
        auto& ds = d.design;
        claude::DesignCheck check = claude::checkDesign(ds);
        // Barre d'action
        {
            ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            ImGui::BeginDisabled(!check.errors.empty());
            if (ImGui::Button("Continuer : créer le projet ›")) {
                if (util::trim(d.cwd).empty()) d.cwd = designerSuggestedCwd();
                d.step = 3;
                designerDirty_ = true;
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(2);
            if (!check.errors.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Corrigez d'abord les points en rouge.");
            ImGui::SameLine();
            ImGui::BeginDisabled(d.history.empty());
            std::string undo = "↶ Version précédente" + (d.history.empty() ? std::string() : " (" + std::to_string(d.history.size()) + ")");
            if (ImGui::Button(undo.c_str())) {
                d.design = d.history.back();
                d.history.pop_back();
                designerDirty_ = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Revenir à la conception d'avant la dernière génération (vos modifications de celle-ci sont perdues).");
            if (d.lastCostUsd > 0) {
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("Dernière génération : %s (équivalent API)", util::formatUsd(d.lastCostUsd).c_str());
            }
        }
        for (auto& e : check.errors) ImGui::TextColored(pal.err, "✕ %s", e.c_str());
        for (auto& w : check.warnings) ImGui::TextColored(pal.warn, "⚠ %s", w.c_str());
        ImGui::Spacing();

        float leftW = wide ? std::max(fs * 26, avail * 0.40f) : avail;
        ImGui::BeginChild("##dsgleft", ImVec2(leftW, 0), wide ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY);
        // Projet
        beginCard("##dsgproj");
        cardTitle("Projet", fontBold);
        ImGui::SetNextItemWidth(-1);
        dirtyIf(ImGui::InputTextWithHint("##name", "Nom du projet", &ds.name));
        dirtyIf(multiline("##summary", ds.summary, 4, "Résumé : ce qui sera construit, les choix faits"));
        {
            std::string stack;
            for (auto& s : ds.stack) stack += (stack.empty() ? "" : ", ") + s;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##stack", "Technologies, séparées par des virgules", &stack)) {
                ds.stack.clear();
                for (auto& s : util::split(stack, ','))
                    if (!util::trim(s).empty()) ds.stack.push_back(util::trim(s));
                designerDirty_ = true;
            }
        }
        if (ImGui::TreeNode("Conventions (build, tests, style)")) {
            dirtyIf(multiline("##conv", ds.conventions, 5, "Ex. : npm test doit passer ; textes en français"));
            ImGui::TreePop();
        }
        ImGui::EndChild();
        ImGui::Spacing();

        // Orchestrateur
        beginCard("##dsgorch");
        cardTitle("Orchestrateur", fontBold);
        ImGui::TextUnformatted("Objectif");
        ImGui::SameLine();
        ui::helpMarker("Ce que l'orchestrateur doit livrer et comment savoir que c'est fini. Un objectif vérifiable\n"
                       "(« … avec les tests qui passent ») donne de meilleurs résultats.");
        dirtyIf(multiline("##objective", ds.objective, 5, "Livrable final, critères d'acceptation, priorités"));
        ImGui::TextUnformatted("Jalons, dans l'ordre");
        int removeAt = -1, upAt = -1;
        for (size_t i = 0; i < ds.milestones.size(); ++i) {
            ImGui::PushID((int)i);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%d.", (int)i + 1);
            ImGui::SameLine();
            float bw = ui::buttonWidth("↑") + ui::buttonWidth("✕") + ImGui::GetStyle().ItemSpacing.x * 2;
            ImGui::SetNextItemWidth(std::max(fs * 6, ImGui::GetContentRegionAvail().x - bw));
            dirtyIf(ImGui::InputText("##m", &ds.milestones[i]));
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive() &&
                ImGui::CalcTextSize(ds.milestones[i].c_str()).x > ImGui::GetItemRectSize().x - fs)
                ImGui::SetTooltip("%s", ds.milestones[i].c_str());
            ImGui::SameLine();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::Button("↑")) upAt = (int)i;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("✕")) removeAt = (int)i;
            ImGui::PopID();
        }
        if (upAt > 0) std::swap(ds.milestones[(size_t)upAt], ds.milestones[(size_t)upAt - 1]), designerDirty_ = true;
        if (removeAt >= 0) ds.milestones.erase(ds.milestones.begin() + removeAt), designerDirty_ = true;
        if (ImGui::SmallButton("+ Jalon")) ds.milestones.emplace_back(), designerDirty_ = true;
        ImGui::Spacing();
        ImGui::TextUnformatted("Budget");
        for (size_t i = 0; i < 4; ++i) {
            claude::BudgetPreset p = claude::budgetPresetFromName(kBudgets[i].value);
            const char* lbl = claude::budgetPresetLabel(p, subNow);
            if (i) ui::sameLineIfFits(ImGui::CalcTextSize(lbl).x + ImGui::GetFrameHeight() * 1.5f);
            if (ImGui::RadioButton(lbl, ds.budget == kBudgets[i].value)) ds.budget = kBudgets[i].value, designerDirty_ = true;
        }
        dimWrapped(claude::budgetPresetSummary(claude::budgetPresetFromName(ds.budget), subNow));
        dirtyIf(ImGui::Checkbox("Vérification visuelle (captures d'écran)", &ds.visualChecks));
        ImGui::EndChild();
        ImGui::Spacing();

        // Affiner avec Claude
        beginCard("##dsgrefine");
        cardTitle("Demander un changement à Claude", fontBold);
        dirtyIf(multiline("##feedback", d.feedback, 3,
                          "Ex. : fusionne 03 et 04 ; ajoute un agent pour la documentation ; passe le moteur en Rust ; "
                          "un jalon de démo jouable plus tôt…"));
        bool canRefine = !util::trim(d.feedback).empty() && !busy;
        ImGui::BeginDisabled(!canRefine);
        bool refine = ImGui::Button("✦  Affiner");
        ImGui::EndDisabled();
        if (canRefine && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)) refine = true;
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Claude garde vos modifications et ne change que ce qui est demandé.");
        if (refine && canRefine) designerRun(true);
        ImGui::EndChild();
        ImGui::EndChild();

        // Équipe
        if (wide) ImGui::SameLine();
        ImGui::BeginChild("##dsgteam", ImVec2(0, 0));
        ImGui::PushFont(fontBold, 0.0f);
        ImGui::Text("Équipe — %d agents", (int)ds.agents.size());
        ImGui::PopFont();
        ImGui::SameLine();
        ui::helpMarker("Chaque agent deviendra un thème du projet ; sa fiche (dans les notes du thème) est relue à chaque\n"
                       "tâche. Le modèle conseillé guide l'orchestrateur, qui choisit le modèle de chaque tâche dans les\n"
                       "limites du budget. Navigateur : l'agent peut ouvrir et vérifier des pages web dans Chrome.");
        ImGui::SameLine();
        if (ImGui::SmallButton("+ Ajouter un agent")) {
            claude::DesignAgent a;
            char num[8];
            std::snprintf(num, sizeof num, "%02d", (int)ds.agents.size() + 1);
            a.name = std::string(num) + " Nouvel agent";
            a.role = "## Mission\n\n## Possède\n\n## Frontières\n\n## Vérifications\n";
            ds.agents.push_back(a);
            designerOpenRole_ = (int)ds.agents.size() - 1;
            designerDirty_ = true;
        }
        const float tw = ImGui::GetContentRegionAvail().x;
        const int cols = tw > fs * 44 ? 2 : 1;
        int del = -1, moveUp = -1;
        if (ImGui::BeginTable("##agents", cols, ImGuiTableFlags_SizingStretchSame)) {
            for (size_t i = 0; i < ds.agents.size(); ++i) {
                auto& a = ds.agents[i];
                ImGui::TableNextColumn();
                ImGui::PushID((int)i);
                beginCard("##agent");
                float bw = ui::buttonWidth("↑") + ui::buttonWidth("✕") + ImGui::GetStyle().ItemSpacing.x * 2;
                ImGui::PushFont(fontBold, 0.0f);
                ImGui::SetNextItemWidth(std::max(fs * 6, ImGui::GetContentRegionAvail().x - bw));
                dirtyIf(ImGui::InputTextWithHint("##n", "01 Nom de l'agent", &a.name));
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::BeginDisabled(i == 0);
                if (ImGui::Button("↑")) moveUp = (int)i;
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Monter (l'ordre suit celui du démarrage du travail)");
                ImGui::SameLine();
                if (ImGui::Button("✕")) del = (int)i;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Retirer cet agent");
                ImGui::SetNextItemWidth(-1);
                dirtyIf(ImGui::InputTextWithHint("##mi", "Mission en une phrase", &a.mission));
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !ImGui::IsItemActive() &&
                    ImGui::CalcTextSize(a.mission.c_str()).x > ImGui::GetItemRectSize().x - fs)
                    ImGui::SetTooltip("%s", a.mission.c_str());
                float cw = fs * 5.5f;
                dirtyIf(optCombo("##mo", a.model, kAgentModels, std::size(kAgentModels), cw));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Modèle conseillé pour ses tâches");
                ImGui::SameLine();
                dirtyIf(optCombo("##ef", a.effort, kAgentEfforts, std::size(kAgentEfforts), cw));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Effort conseillé");
                ui::sameLineIfFits(ImGui::CalcTextSize("Navigateur").x + ImGui::GetFrameHeight() * 1.5f);
                dirtyIf(ImGui::Checkbox("Navigateur", &a.browser));
                if (designerOpenRole_ == (int)i) ImGui::SetNextItemOpen(true), designerOpenRole_ = -1;
                std::string roleHead = std::string("Fiche de rôle") + (util::trim(a.role).empty() ? "  (vide)" : "") + "###role";
                if (ImGui::TreeNode(roleHead.c_str())) {
                    dirtyIf(multiline("##role", a.role, 12, "## Mission\n## Possède\n## Frontières\n## Conventions\n## Vérifications"));
                    ImGui::TreePop();
                }
                ImGui::EndChild();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (moveUp > 0) std::swap(ds.agents[(size_t)moveUp], ds.agents[(size_t)moveUp - 1]), designerDirty_ = true;
        if (del >= 0) ds.agents.erase(ds.agents.begin() + del), designerDirty_ = true;
        ImGui::EndChild();
    }

    // ======================= 4. Créer =======================
    else if (d.step == 3) {
        auto& ds = d.design;
        if (!d.createdProject.empty()) {
            float w = std::min(avail, fs * 40);
            ImGui::BeginChild("##dsgdone", ImVec2(w, 0));
            beginCard("##done");
            ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.2f);
            ImGui::TextColored(pal.ok, "✓ Projet « %s » créé", ds.name.c_str());
            ImGui::PopFont();
            dimWrapped("Ses agents sont les thèmes du projet dans l'espace de travail, et son orchestrateur est réglé. "
                       "Relisez l'objectif dans la page Orchestrateur, puis lancez l'orchestration.");
            ImGui::Spacing();
            if (ImGui::Button("Ouvrir l'orchestrateur")) {
                if (ws::Folder* p = workspace_.findFolderByPath(util::pathFromUtf8(d.createdProject)))
                    if (!(orch_ && orch_->running())) loadOrchestrator(p);
                page_ = 2;
            }
            ImGui::SameLine();
            if (ImGui::Button("Voir dans le cockpit")) {
                selectedFolder_ = d.createdProject;
                page_ = 0;
            }
            ImGui::SameLine();
            if (ImGui::Button("Nouveau design")) designerReset();
            ImGui::EndChild();
            ImGui::EndChild();
        } else {
            claude::DesignCheck check = claude::checkDesign(ds);
            float leftW = wide ? std::max(fs * 28, avail * 0.48f) : avail;
            ImGui::BeginChild("##dsgcreate", ImVec2(leftW, 0), wide ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY);
            beginCard("##where");
            cardTitle("Dossier de travail", fontBold);
            float browseW = ui::buttonWidth("Parcourir…");
            ImGui::SetNextItemWidth(std::max(fs * 8, ImGui::GetContentRegionAvail().x - browseW - ImGui::GetStyle().ItemSpacing.x));
            dirtyIf(ImGui::InputTextWithHint("##cwd", "Dossier du dépôt (créé s'il n'existe pas)", &d.cwd));
            ImGui::SameLine();
            if (ImGui::Button("Parcourir…##cwd"))
                openFolderDialog([this](const std::string& p) {
                    designer_.cwd = p;
                    designerDirty_ = true;
                });
            int cnt = 0;
            switch (folderState(d.cwd, &cnt)) {
                case 0: ImGui::TextColored(pal.err, "Choisissez où vivra le code du projet."); break;
                case 1: ImGui::TextColored(pal.ok, "✚ Sera créé."); break;
                case 2: ImGui::TextDisabled("Dossier existant, vide."); break;
                case 3: ImGui::TextDisabled("Dossier existant (%d éléments) : rien n'y sera écrasé.", cnt); break;
                default: ImGui::TextColored(pal.err, "Ce chemin n'est pas un dossier."); break;
            }
            ImGui::EndChild();
            ImGui::Spacing();

            beginCard("##what");
            cardTitle("Ce qui sera créé", fontBold);
            ImGui::Bullet();
            ImGui::TextWrapped("Projet « %s » dans l'espace de travail, avec %d thèmes : un par agent, sa fiche en notes.",
                               ds.name.c_str(), (int)ds.agents.size());
            ImGui::Bullet();
            std::string browsers;
            for (auto& a : ds.agents)
                if (a.browser) browsers += (browsers.empty() ? "" : ", ") + a.name;
            ImGui::TextWrapped("Orchestrateur : %s, objectif + %d jalons%s%s.",
                               claude::budgetPresetLabel(claude::budgetPresetFromName(ds.budget), subNow),
                               (int)ds.milestones.size(), ds.visualChecks ? ", vérification visuelle" : "",
                               browsers.empty() ? "" : (", navigateur pour " + browsers).c_str());
            ImGui::Spacing();
            ImGui::TextUnformatted("Fichiers de départ dans le dépôt");
            fs::path root = util::pathFromUtf8(util::trim(d.cwd));
            std::error_code ec;
            auto exists = [&](const std::string& rel) { return !util::trim(d.cwd).empty() && fs::exists(root / util::pathFromUtf8(rel), ec); };
            auto fileBox = [&](const char* label, bool& v, const std::string& rel) {
                dirtyIf(ImGui::Checkbox(label, &v));
                if (exists(rel)) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(existe déjà : conservé)");
                }
            };
            fileBox("CLAUDE.md — consignes du dépôt et carte de l'équipe", d.writeClaudeMd, "CLAUDE.md");
            fileBox("docs/PLAN.md — objectif et jalons", d.writePlan, "docs/PLAN.md");
            std::string agentsLbl = "docs/agents/ — une fiche par agent (" + std::to_string(ds.agents.size()) + ")";
            dirtyIf(ImGui::Checkbox(agentsLbl.c_str(), &d.writeAgentDocs));
            ImGui::Spacing();
            dirtyIf(ImGui::Checkbox("Ouvrir la page Orchestrateur ensuite", &d.openOrchestrator));
            dirtyIf(ImGui::Checkbox("Démarrer l'orchestration tout de suite", &d.startNow));
            ImGui::SameLine();
            ui::helpMarker("Décoché (conseillé) : relisez l'objectif et les réglages dans la page Orchestrateur, puis\n"
                           "lancez l'orchestration quand vous êtes prêt. Coché : elle démarre aussitôt le projet créé.");
            ImGui::EndChild();
            ImGui::Spacing();

            for (auto& e : check.errors) ImGui::TextColored(pal.err, "✕ %s", e.c_str());
            for (auto& p : workspace_.projects())
                if (lowerStr(p->name) == lowerStr(util::trim(ds.name)))
                    ImGui::TextColored(pal.err, "✕ Un projet « %s » existe déjà : renommez celui-ci à l'étape 3.", p->name.c_str());
            int st = folderState(d.cwd);
            bool canCreate = check.errors.empty() && st != 0 && st != 4 && !busy;
            ImGui::BeginDisabled(!canCreate);
            ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            if (ImGui::Button("✓  Créer le projet", ImVec2(std::min(fs * 14, ImGui::GetContentRegionAvail().x), fs * 2.2f))) designerCreate();
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("‹ Ajuster", ImVec2(0, fs * 2.2f))) d.step = 2;
            ImGui::EndChild();

            // Aperçu des fichiers
            if (wide) ImGui::SameLine();
            ImGui::BeginChild("##dsgfiles", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            cardTitle("Aperçu", fontBold);
            auto files = claude::scaffoldFiles(ds, true, true, true);
            files.insert(files.begin(), claude::ScaffoldFile{"Notes du projet (espace de travail)", claude::projectNotes(ds)});
            designerFilePreview_ = std::clamp(designerFilePreview_, 0, (int)files.size() - 1);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##file", files[(size_t)designerFilePreview_].path.c_str())) {
                for (size_t i = 0; i < files.size(); ++i)
                    if (ImGui::Selectable(files[i].path.c_str(), (int)i == designerFilePreview_)) designerFilePreview_ = (int)i;
                ImGui::EndCombo();
            }
            std::string content = files[(size_t)designerFilePreview_].content;
            ImGui::PushFont(fontMono, ImGui::GetStyle().FontSizeBase * 0.84f);
            ImGui::InputTextMultiline("##preview", &content, ImVec2(-1, -1), ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
            ImGui::PopFont();
            ImGui::EndChild();
        }
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
}

}  // namespace app
