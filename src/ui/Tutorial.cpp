// Prise en main : écran de bienvenue (premier lancement), visite guidée qui
// surligne chaque zone de l'interface, guide de démarrage illustré d'actions,
// et projet exemple (mini calculatrice C++ + conversations pré-enregistrées).
#include <algorithm>
#include <cfloat>
#include <cmath>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "app/App.h"
#include "ui/Anim.h"
#include "ui/Theme.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace app {

using claude::Entry;
using claude::EntryKind;
using json = nlohmann::json;

// ===========================================================================
// Visite guidée
// ===========================================================================

namespace {

struct TourStep {
    const char* title;
    const char* body;     // markdown
    const char* target;   // nom de fenêtre ImGui, ou #editor #menubar #statusbar #pageswitch #none
    int page;             // 0 cockpit, 1 tableau de bord
};

const std::vector<TourStep>& tourSteps() {
    static const std::vector<TourStep> steps = {
        {"Bienvenue dans ClaudeDesk",
         "ClaudeDesk est une interface de bureau pour **Claude Code**. Elle pilote le vrai programme `claude` "
         "installé sur votre PC : vos réglages, vos fichiers `CLAUDE.md`, vos serveurs MCP et votre abonnement "
         "s'appliquent exactement comme dans le terminal.\n\n"
         "Cette visite présente chaque zone de l'écran en une douzaine d'étapes (2 minutes). "
         "Utilisez **Suivant** ou la flèche **→** du clavier.",
         "#none", 0},
        {"La barre de menus",
         "- **Fichier** : nouvelle conversation, nouveau projet, import des sessions du terminal, export.\n"
         "- **Session** : interrompre Claude, compacter le contexte, générer un récap.\n"
         "- **Configuration** : modèle, effort, mode de permission, outils, prompt système.\n"
         "- **Affichage** : panneaux, thème, taille du texte, pages.\n"
         "- **Aide** : ce guide, la visite, le projet exemple, les raccourcis (F1).\n\n"
         "À droite : les **pages** (Cockpit / Tableau de bord) et votre usage de la fenêtre de 5 h.",
         "#menubar", 0},
        {"Espace de travail : projets › thèmes › conversations",
         "Toutes vos conversations sont **enregistrées et rangées** ici, comme des fichiers dans des dossiers :\n\n"
         "- un **projet** (◆) correspond à un dossier de code sur lequel Claude travaille ;\n"
         "- il se découpe en **thèmes** (▪ Code, Infra, Docs…) et sous-thèmes ;\n"
         "- chaque niveau contient des **conversations**.\n\n"
         "Double-clic pour rouvrir, glisser-déposer pour ranger, clic droit pour tout le reste "
         "(renommer, statut, récap, déplacer…).",
         "Espace de travail", 0},
        {"L'inspecteur",
         "Il affiche les détails de ce que vous sélectionnez dans l'arbre :\n\n"
         "- pour un **dossier** : ses **notes** (que Claude recevra automatiquement) et sa **synthèse** ;\n"
         "- pour une **conversation** : son **récap** et les boutons **Reprendre** (même session Claude, "
         "historique complet) ou **Repartir de ce contexte** (nouvelle session légère qui reçoit les récaps).",
         "Inspecteur", 0},
        {"Le fil de la conversation",
         "Chaque conversation ouverte a son onglet (plusieurs peuvent tourner **en parallèle**).\n\n"
         "- les réponses s'affichent **en direct**, en Markdown ;\n"
         "- les **actions de Claude** (lire, modifier un fichier, lancer une commande) sont des lignes "
         "dépliables : un clic montre le détail, avec un **diff coloré** pour les modifications ;\n"
         "- survolez un message pour **Copier** ou **Sélectionner** son texte ; clic droit pour plus d'options ;\n"
         "- **Ctrl+F** filtre les messages.",
         "Conversations", 0},
        {"L'éditeur et la barre rapide",
         "Écrivez ici comme dans n'importe quel éditeur : souris, Ctrl+C / V / Z, Ctrl+flèches…\n\n"
         "- **Entrée** envoie, **Maj+Entrée** passe à la ligne ;\n"
         "- tapez **/** pour les commandes (`/compact`, `/context`, vos skills…) ;\n"
         "- **Ctrl+↑** rappelle vos messages précédents ;\n"
         "- **glissez une image** dans la fenêtre pour la joindre.\n\n"
         "Au-dessus : le **modèle**, l'**effort** de réflexion et le **mode** de permission, modifiables à tout "
         "moment, ainsi que le remplissage du **contexte**.",
         "#editor", 0},
        {"Les autorisations",
         "Quand Claude veut modifier un fichier ou lancer une commande, un **panneau orange** apparaît juste "
         "au-dessus de l'éditeur avec le détail exact (commande, diff…) :\n\n"
         "- **Autoriser** (Alt+A) : cette fois-ci ;\n"
         "- **Toujours autoriser** (Alt+T) : pour le reste de la session ;\n"
         "- **Refuser** (Alt+R) : avec, si vous voulez, une consigne pour Claude.\n\n"
         "Le **mode** change ce comportement : *Plan* (lecture seule), *Accepter les modifs*, *Auto*…",
         "Conversations", 0},
        {"Usage en direct",
         "L'équivalent de la commande `/usage`, mis à jour à chaque réponse :\n\n"
         "- la **fenêtre de 5 h** et la **semaine** de votre forfait, avec l'heure de réinitialisation ;\n"
         "- pour la conversation active : jetons, coût équivalent API, **remplissage du contexte**, "
         "et la **liste de tâches** de Claude quand il en tient une.",
         "Usage", 0},
        {"La configuration",
         "Six onglets : **Modèle** (et effort), **Mode**, **Outils** (autoriser / interdire, règles fines comme "
         "`Bash(git *)`), **Prompt système** (avec modèles réutilisables), **Avancé** et **Application**.\n\n"
         "Les réglages s'appliquent à la conversation active, ou deviennent les défauts avec "
         "« Enregistrer comme défaut ». Le modèle et le mode changent à chaud ; le reste relance la session "
         "en arrière-plan **sans perdre l'historique**.",
         "Configuration", 0},
        {"Page 2 : le tableau de bord des conversations",
         "Quand plusieurs conversations tournent en même temps, cette page les montre **toutes d'un coup** :\n\n"
         "- une **carte** par conversation : état, durée, tâches de Claude, dernières actions, dernière réponse ;\n"
         "- les **autorisations en un clic** directement sur la carte ;\n"
         "- une **réponse rapide** sans quitter la page ;\n"
         "- à droite, le **fil d'activité** de toutes les conversations.\n\n"
         "**Ctrl+1** / **Ctrl+2** pour changer de page, **Ctrl+Maj+D** pour l'ouvrir dans une **fenêtre séparée** "
         "(idéal sur un deuxième écran).",
         "#none", 1},
        {"Le mode cowork : la mémoire du projet",
         "Claude ne se souvient de rien d'une conversation à l'autre. Sur un vrai projet, on finit soit par garder "
         "**une conversation géante** (lente, chère, confuse), soit par **tout réexpliquer** à chaque fois.\n\n"
         "Le mode cowork règle ça comme une équipe le fait avec des comptes rendus :\n\n"
         "1. **Ranger** chaque conversation dans un projet et un thème ;\n"
         "2. **Résumer** en fin de conversation (*Récap*, Ctrl+Maj+R) : objectif, décisions, fichiers, reste à faire ;\n"
         "3. **Réinjecter** : chaque nouvelle conversation d'un thème démarre en connaissant les notes, synthèses et "
         "récaps de ce thème.\n\n"
         "Utile pour un projet qui **dure** et qui a **plusieurs facettes** ; inutile pour une question ponctuelle "
         "(rangez-la dans *Divers*). Le guide (section *Comprendre et bien utiliser*) détaille quand et comment "
         "s'en servir.",
         "Espace de travail", 0},
        {"La barre d'état",
         "En permanence : l'état de la conversation active (prête, en cours, durée du tour), le modèle, l'effort, "
         "le mode, le contexte utilisé et le coût. Les tâches de fond (récaps…) y apparaissent aussi.",
         "#statusbar", 0},
        {"À vous de jouer !",
         "Le meilleur moyen d'apprendre : le **projet exemple**. Il crée une petite calculatrice C++ volontairement "
         "imparfaite, rangée en thèmes, avec des conversations déjà enregistrées (récaps, outils, tâches…).\n\n"
         "Suggestions :\n"
         "- ouvrez *Code › Parseur › Division par zéro* et répondez « Applique le correctif » ;\n"
         "- testez le **mode Plan** sur l'idée de l'opérateur puissance ;\n"
         "- lancez deux conversations et suivez-les depuis le **tableau de bord**.\n\n"
         "Le **guide** (Maj+F1) détaille chaque fonctionnalité.",
         "#none", 0},
    };
    return steps;
}

}  // namespace

void App::startTour() {
    tourStep_ = 0;
    tourEnteredStep_ = -1;
    showGuide_ = false;
    settings_.tutorialSeen = true;
}

void App::drawTour() {
    const auto& steps = tourSteps();
    if (tourStep_ < 0) return;
    if (tourStep_ >= (int)steps.size()) {
        tourStep_ = -1;
        return;
    }
    const TourStep& st = steps[tourStep_];
    const auto& pal = ui::palette();
    bool entered = tourEnteredStep_ != tourStep_;
    if (entered) {
        // Prépare l'écran pour l'étape.
        if (!dash_) page_ = st.page;
        showWorkspace_ = showInspector_ = showUsage_ = showConfig_ = true;
        if (std::string(st.target) == "#editor" && open_.empty()) newConversation(nullptr);
        if (st.page == 1 && dash_) SDL_RaiseWindow(dash_->window);
        tourEnteredStep_ = tourStep_;
    }

    // Zone à mettre en évidence
    ImVec2 rmin(0, 0), rmax(0, 0);
    std::string target = st.target;
    auto fromArr = [&](const float* r) {
        rmin = ImVec2(r[0], r[1]);
        rmax = ImVec2(r[2], r[3]);
    };
    if (target == "#editor" && ImGui::GetFrameCount() - editorRectFrame_ <= 2) fromArr(editorRect_);
    else if (target == "#menubar") fromArr(menuBarRect_);
    else if (target == "#statusbar") fromArr(statusBarRect_);
    else if (target[0] != '#') {
        if (ImGuiWindow* w = ImGui::FindWindowByName(target.c_str()); w && w->WasActive) {
            rmin = w->Pos;
            rmax = ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
        }
    }
    bool hasRect = rmax.x > rmin.x + 4 && rmax.y > rmin.y + 4;
    if (hasRect) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        float pulse = settings_.animations ? ui::anim::pulse(0.7f) : 1.0f;
        fg->AddRect(ImVec2(rmin.x + 2, rmin.y + 2), ImVec2(rmax.x - 2, rmax.y - 2), ui::col(pal.accent, 0.55f + 0.45f * pulse), 8.0f, 0, 3.0f);
        if (settings_.glowEffects)
            for (int k = 1; k <= 6; ++k) {
                float e = k * 2.5f;
                fg->AddRect(ImVec2(rmin.x + 2 - e, rmin.y + 2 - e), ImVec2(rmax.x - 2 + e, rmax.y - 2 + e),
                            ui::col(pal.accent, (0.20f - k * 0.03f) * pulse), 8.0f + e, 0, 2.5f);
            }
    }

    // Bulle d'explication : à côté de la zone si possible, sinon au centre.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float w = ImGui::GetFontSize() * 27;
    static ImVec2 lastSize(w, 300);
    ImVec2 pos;
    if (!hasRect) {
        pos = ImVec2(vp->WorkPos.x + (vp->WorkSize.x - lastSize.x) * 0.5f, vp->WorkPos.y + (vp->WorkSize.y - lastSize.y) * 0.4f);
    } else if (rmax.x + 16 + lastSize.x < vp->WorkPos.x + vp->WorkSize.x) {
        pos = ImVec2(rmax.x + 16, rmin.y + 20);
    } else if (rmin.x - 16 - lastSize.x > vp->WorkPos.x) {
        pos = ImVec2(rmin.x - 16 - lastSize.x, rmin.y + 20);
    } else if (rmax.y + 16 + lastSize.y < vp->WorkPos.y + vp->WorkSize.y) {
        pos = ImVec2(rmin.x + 20, rmax.y + 16);
    } else {
        pos = ImVec2(rmin.x + 20, std::max(vp->WorkPos.y + 10, rmin.y - lastSize.y - 16));
    }
    pos.x = std::clamp(pos.x, vp->WorkPos.x + 8, vp->WorkPos.x + vp->WorkSize.x - lastSize.x - 8);
    pos.y = std::clamp(pos.y, vp->WorkPos.y + 8, vp->WorkPos.y + vp->WorkSize.y - lastSize.y - 8);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(w, FLT_MAX));
    if (entered) ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_Border, pal.accent);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::Begin("Visite guidée", nullptr,
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    // Toujours au premier plan (sans prendre le focus clavier) : la page affichée peut changer.
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::TextDisabled("Visite guidée — étape %d / %d", tourStep_ + 1, (int)steps.size());
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, pal.accent);
    ImGui::ProgressBar((tourStep_ + 1) / (float)steps.size(), ImVec2(-1, 3), "");
    ImGui::PopStyleColor();
    ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::TextColored(pal.accent, "%s", st.title);
    ImGui::PopFont();
    ui::MdStyle md{fontUi, fontBold, fontMono, true, fontItalic};
    ui::renderMarkdown(ui::parseMarkdown(st.body), md, 55000 + tourStep_);
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Separator();
    bool last = tourStep_ == (int)steps.size() - 1;
    ImGuiIO& io = ImGui::GetIO();
    bool keyNext = !io.WantTextInput && (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_Enter));
    bool keyPrev = !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_LeftArrow);
    ImGui::BeginDisabled(tourStep_ == 0);
    if (ImGui::Button("← Précédent") || (keyPrev && tourStep_ > 0)) --tourStep_;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!last) {
        ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (ImGui::Button("Suivant →") || keyNext) ++tourStep_;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Quitter la visite") || (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape))) tourStep_ = -1;
    } else {
        if (ImGui::Button("Créer le projet exemple")) {
            tourStep_ = -1;
            createExampleProject();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ouvrir le guide")) {
            tourStep_ = -1;
            showGuide_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Terminer") || keyNext) tourStep_ = -1;
    }
    lastSize = ImGui::GetWindowSize();
    ImGui::End();
    if (tourStep_ < 0) {
        page_ = 0;
        settings_.save(settingsPath_);
    }
}

// ===========================================================================
// Écran de bienvenue (premier lancement)
// ===========================================================================

void App::drawWelcomeModal() {
    if (showWelcomeModal_) {
        ImGui::OpenPopup("Bienvenue dans ClaudeDesk");
        showWelcomeModal_ = false;
    }
    const auto& pal = ui::palette();
    float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fs * 30, 0));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Bienvenue dans ClaudeDesk", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
        return;
    ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextColored(pal.accent, "Bonjour !");
    ImGui::PopFont();
    ImGui::TextWrapped("ClaudeDesk vous permet de travailler avec Claude Code dans une vraie fenêtre : éditeur confortable, "
                       "suivi de l'usage, configuration en un clic, et toutes vos conversations rangées par projet "
                       "et par thème pour reprendre le travail facilement.");
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("Comment voulez-vous commencer ?");
    ImVec2 bs(-1, ImGui::GetFrameHeight() * 1.6f);
    auto done = [&] {
        settings_.tutorialSeen = true;
        settings_.save(settingsPath_);
        ImGui::CloseCurrentPopup();
    };
    ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("Visite guidée de l'interface  (2 min)", bs)) {
        done();
        startTour();
    }
    ImGui::PopStyleColor(2);
    if (ImGui::Button("Explorer le projet exemple  (une mini calculatrice C++)", bs)) {
        done();
        createExampleProject();
    }
    if (ImGui::Button("Lire le guide de démarrage", bs)) {
        done();
        showGuide_ = true;
    }
    if (ImGui::Button("Je connais, plus tard", bs)) done();
    ImGui::TextDisabled("Tout reste accessible dans le menu Aide.");
    ImGui::EndPopup();
}

// ===========================================================================
// Guide de démarrage
// ===========================================================================
//
// Deux parties : « Mode d'emploi » (comment faire) et « Comprendre et bien
// utiliser » (pourquoi et quand : les fonctionnalités non triviales, utiles dans
// certains cas seulement — il faut savoir les reconnaître pour en tirer parti).

namespace {

struct GuideSection {
    const char* group;  // nom du groupe (affiché dans le sommaire au premier élément du groupe)
    const char* title;
    const char* body;   // markdown ; nullptr = contenu spécial (raccourcis)
};

const std::vector<GuideSection>& guideSections() {
    static const std::vector<GuideSection> s = {
        // ------------------------------------------------------------------ Bienvenue
        {"Accueil", "Bienvenue",
         "## Bienvenue dans ClaudeDesk\n\n"
         "ClaudeDesk est une interface de bureau pour **Claude Code**. Elle pilote le vrai programme `claude` installé "
         "sur votre PC : vos réglages, vos fichiers `CLAUDE.md`, vos serveurs MCP et votre abonnement s'appliquent "
         "exactement comme dans le terminal. Rien n'est envoyé ailleurs.\n\n"
         "### Ce que ClaudeDesk ajoute au terminal\n"
         "- un **vrai éditeur** (souris, copier-coller, historique) et un rendu lisible des réponses et des actions ;\n"
         "- l'**usage en direct** de votre forfait et du contexte ;\n"
         "- la **configuration en un clic** (modèle, effort, mode, outils, prompt système) ;\n"
         "- une **mémoire de travail organisée** : le *mode cowork* ;\n"
         "- le **suivi de plusieurs conversations en parallèle** : le *tableau de bord*.\n\n"
         "### Comment lire ce guide\n"
         "La partie **Mode d'emploi** explique *comment* faire : elle se lit en dix minutes.\n\n"
         "La partie **Comprendre et bien utiliser** explique *pourquoi* et *quand* se servir des fonctionnalités qui "
         "ne sont pas évidentes : le mode cowork, les récaps, le travail en parallèle, le mode Plan… Aucune n'est "
         "obligatoire : on peut très bien utiliser ClaudeDesk comme un terminal confortable. Mais chacune répond à un "
         "**problème précis** qui finit toujours par arriver sur un vrai projet ; savoir le reconnaître, c'est ce qui "
         "permet de travailler avec Claude sur des semaines plutôt que sur une seule conversation.\n\n"
         "> Le guide reste accessible à tout moment : **Aide › Guide de démarrage** (Maj+F1), ou depuis l'écran "
         "d'accueil quand aucune conversation n'est ouverte."},

        // ------------------------------------------------------------------ Mode d'emploi
        {"Mode d'emploi", "Démarrer en 2 minutes",
         "## Démarrer en 2 minutes\n\n"
         "1. **Créez un projet** : *+ Projet* dans l'espace de travail. Donnez-lui un nom et choisissez le dossier "
         "de votre code : c'est là que Claude lira et modifiera les fichiers. Des thèmes (Code, Infra, Docs, Idées) "
         "sont créés d'office.\n"
         "2. **Sélectionnez un thème** dans l'arbre, puis **Ctrl+N** : une conversation s'ouvre, rangée dans ce thème.\n"
         "3. **Écrivez** votre demande et appuyez sur **Entrée**. Claude répond en direct.\n"
         "4. Quand Claude veut **modifier un fichier** ou **lancer une commande**, autorisez-le (Alt+A) ou refusez (Alt+R).\n"
         "5. À la fin, cliquez **Récap** : la conversation est résumée et servira de mémoire aux suivantes "
         "(voir *Le mode cowork : pourquoi ?*).\n\n"
         "Tout est enregistré automatiquement : fermez l'application quand vous voulez, vos onglets seront rouverts.\n\n"
         "> Pas encore de code sous la main ? Le **projet exemple** est fait pour ça."},
        {"Mode d'emploi", "L'écran en un coup d'œil",
         "## L'écran en un coup d'œil\n\n"
         "ClaudeDesk a **deux pages** (en haut à droite de la barre de menus) :\n\n"
         "### Page 1 — Cockpit (Ctrl+1)\n"
         "| Zone | Rôle |\n|---|---|\n"
         "| Espace de travail (gauche) | vos projets, thèmes et conversations enregistrées |\n"
         "| Inspecteur (gauche, bas) | notes, synthèse, récap de la sélection |\n"
         "| Conversations (centre) | le fil, l'éditeur, les autorisations ; un onglet par conversation |\n"
         "| Usage (droite) | limites du forfait, contexte, coût, tâches de Claude |\n"
         "| Configuration (droite, bas) | modèle, effort, mode, outils, prompt système, préférences, graphismes |\n"
         "| Barre d'état (bas) | état de la conversation active |\n\n"
         "Tous les panneaux se **déplacent et s'ancrent** à la souris (glissez leur onglet). "
         "*Affichage › Réinitialiser la disposition* remet tout en place.\n\n"
         "### Page 2 — Tableau de bord (Ctrl+2)\n"
         "Toutes les conversations ouvertes en cartes, pour suivre plusieurs travaux en parallèle "
         "(voir *Travailler en parallèle*)."},
        {"Mode d'emploi", "Écrire à Claude",
         "## Écrire à Claude\n\n"
         "L'éditeur se comporte comme un éditeur de texte classique :\n\n"
         "- souris : clic, glisser pour sélectionner, **double-clic** sur un mot ;\n"
         "- **Ctrl+C / X / V**, **Ctrl+Z / Y**, **Ctrl+A**, **Ctrl+← / →** (mot par mot), **Maj+flèches** ;\n"
         "- **Entrée** envoie, **Maj+Entrée** ou **Ctrl+Entrée** passe à la ligne (inversable dans *Configuration › Application*).\n\n"
         "### Astuces\n"
         "- **/** ouvre la liste des commandes : `/compact` (résumer pour libérer du contexte), `/context`, `/init` "
         "(crée un `CLAUDE.md`), `/review`, et vos skills. **Tab** ou **Entrée** pour compléter.\n"
         "- **Ctrl+↑ / Ctrl+↓** : vos messages précédents.\n"
         "- **Glissez-déposez** une image (capture d'écran, maquette…) : elle est jointe au message. Un autre fichier "
         "est inséré comme référence `@\"chemin\"`.\n"
         "- Vous pouvez écrire **pendant que Claude travaille** : le message est mis en file.\n"
         "- **Échap** interrompt Claude à tout moment."},
        {"Mode d'emploi", "Suivre le travail de Claude",
         "## Suivre le travail de Claude\n\n"
         "Dans le fil, chaque **action** de Claude est une ligne repliable :\n\n"
         "- ◌ en cours, ✓ réussie, ✗ en erreur ;\n"
         "- un clic déplie le détail : **diff coloré** pour une modification (rouge = retiré, vert = ajouté), "
         "contenu pour une création de fichier, commande et sortie pour un terminal ;\n"
         "- les **sous-agents** (outil Task) affichent leur propre activité à l'intérieur.\n\n"
         "Sous le fil, un indicateur montre ce que fait Claude (requête, réflexion, outil) et depuis combien de temps.\n\n"
         "### Lire et copier\n"
         "- survolez un message : **Copier** ou **Sélectionner** (texte sélectionnable à la souris) ;\n"
         "- les blocs de code ont leur propre bouton **Copier** ;\n"
         "- **clic droit** : réutiliser un de vos messages, copier toute la conversation en Markdown ;\n"
         "- **Ctrl+F** filtre les messages, **Ctrl+Maj+C** copie la dernière réponse."},
        {"Mode d'emploi", "Autorisations et modes",
         "## Autorisations et modes\n\n"
         "Par défaut, Claude **demande** avant toute action sensible. Le panneau d'autorisation montre exactement ce "
         "qui va se passer, puis :\n\n"
         "- **Autoriser** (Alt+A) — cette fois ;\n"
         "- **Toujours autoriser** (Alt+T) — ce type d'action pour toute la session ;\n"
         "- **Refuser** (Alt+R) — avec éventuellement une consigne (« utilise plutôt… »).\n\n"
         "### Les modes (barre rapide ou menu Configuration)\n"
         "| Mode | Comportement |\n|---|---|\n"
         "| Normal | demande pour chaque action sensible |\n"
         "| Accepter les modifs | les modifications de fichiers passent sans demander |\n"
         "| Auto | Claude juge seul ce qui est sûr |\n"
         "| **Plan** | lecture seule : Claude explore et propose un plan, que vous validez avant toute modification |\n"
         "| Ne pas demander | tout ce qui demanderait est refusé |\n"
         "| Sans permissions | **dangereux** : tout est permis |\n\n"
         "Claude peut aussi vous **poser des questions à choix** : répondez directement dans le panneau.\n\n"
         "> Quel mode choisir, et quand ? Voir *Doser l'autonomie et la réflexion*."},
        {"Mode d'emploi", "Modèle, effort et configuration",
         "## Modèle, effort et configuration\n\n"
         "- **Modèle** : Opus (le plus capable), Sonnet (équilibré), Haiku (rapide et économe)… "
         "Il change **à chaud**, même au milieu d'une conversation.\n"
         "- **Effort** : de *Faible* à *Maximum*, la quantité de réflexion avant d'agir.\n"
         "- **Outils** : rendez un outil indisponible, toujours autorisé ou interdit ; ajoutez des règles fines "
         "comme `Bash(git *)` (toutes les commandes git) ou `Edit(src/**)`.\n"
         "- **Prompt système** : ajoutez des consignes permanentes ; enregistrez-les comme **modèles** réutilisables.\n"
         "- **Avancé** : dossiers supplémentaires accessibles, budget maximal, options libres.\n"
         "- **Graphismes** : fluidité, animations, qualité du texte, moteur GPU.\n\n"
         "Les réglages s'appliquent à la **conversation active**. *Enregistrer comme défaut* les utilise pour les "
         "suivantes. Changer l'effort, les outils ou le prompt **relance la session en arrière-plan** : l'historique "
         "est conservé."},
        {"Mode d'emploi", "Usage et limites",
         "## Usage et limites\n\n"
         "Le panneau **Usage** est l'équivalent de `/usage`, en direct :\n\n"
         "- **Session (fenêtre 5 h)** et **Semaine** : pourcentage de votre forfait consommé et heure de "
         "réinitialisation. Vert < 50 %, orange < 80 %, rouge au-delà.\n"
         "- La mesure arrive avec chaque réponse de Claude ; *Actualiser* envoie une requête minuscule à Haiku.\n"
         "- **Fenêtre de contexte** : la mémoire de travail de la conversation. Quand elle se remplit, tapez "
         "`/compact` ou repartez d'un récap.\n"
         "- Le **coût** affiché est l'équivalent au tarif de l'API : avec un abonnement, c'est le quota qui est "
         "consommé, pas de l'argent.\n\n"
         "> Pourquoi le contexte compte autant : voir *Le mode cowork : pourquoi ?*"},
        {"Mode d'emploi", "Reprendre et importer",
         "## Reprendre et importer\n\n"
         "- Vos onglets sont **rouverts** au démarrage, et chaque conversation garde l'identifiant de sa session "
         "Claude Code : votre prochain message **reprend exactement** là où vous en étiez.\n"
         "- **Importer depuis Claude Code** (bouton *Importer* de l'espace de travail) liste les sessions faites "
         "dans le **terminal** (`claude`), par dossier. *Importer* les range dans le dossier sélectionné : elles se "
         "reprennent telles quelles.\n"
         "- À l'inverse, *Session › Reprendre dans le terminal* copie la commande `claude --resume …`.\n"
         "- *Exporter* produit un fichier Markdown de la conversation (avec son récap)."},
        {"Mode d'emploi", "Raccourcis clavier", nullptr},

        // ------------------------------------------------------------------ Comprendre et bien utiliser
        {"Comprendre et bien utiliser", "Le mode cowork : pourquoi ?",
         "## Le mode cowork : pourquoi ?\n\n"
         "### Le problème\n"
         "Claude ne se souvient de rien d'une conversation à l'autre. Tout ce qu'il « sait » de votre projet tient "
         "dans sa **fenêtre de contexte** : les fichiers qu'il a lus et les messages échangés *dans cette "
         "conversation*. Sur un petit sujet, c'est parfait. Sur un vrai projet, au bout de quelques jours, trois "
         "situations reviennent sans cesse :\n\n"
         "1. **La conversation géante** : on garde tout dans la même session pour « qu'il se souvienne ». Le contexte "
         "se remplit, chaque message coûte plus de quota, Claude devient plus lent et finit par mélanger d'anciennes "
         "décisions avec les nouvelles, puis le contexte est compacté et des détails disparaissent.\n"
         "2. **Le réexplicage permanent** : on repart d'une conversation neuve, et l'on perd dix minutes à redire "
         "l'objectif, les conventions, ce qui a été tenté et pourquoi ça a été abandonné.\n"
         "3. **Le grenier** : des dizaines de sessions sans titre, impossibles à retrouver. On ne sait plus où une "
         "décision a été prise.\n\n"
         "### Ce que fait le mode cowork\n"
         "Il traite vos échanges avec Claude comme on traite le code : **rangés, résumés et réutilisables**.\n\n"
         "- **Ranger** : chaque conversation vit dans un projet et un thème, comme un fichier dans un dossier.\n"
         "- **Résumer** : un **récap** condense une conversation finie en quelques lignes (objectif, ce qui a été fait, "
         "décisions, fichiers, reste à faire) ; une **synthèse** condense les récaps d'un thème.\n"
         "- **Réinjecter** : une nouvelle conversation d'un thème reçoit automatiquement, en arrière-plan, les notes "
         "du projet, les synthèses et les récaps récents de ce thème. Claude démarre **informé**, avec quelques "
         "milliers de jetons au lieu de centaines de milliers.\n\n"
         "C'est la même idée qu'un bon carnet de bord dans une équipe : on ne relit pas toutes les réunions, on lit "
         "les comptes rendus.\n\n"
         "### Quand c'est utile\n"
         "- un projet qui **dure** (plusieurs jours, semaines) et sur lequel vous revenez souvent ;\n"
         "- un projet avec **plusieurs facettes** (code, infra, docs, recherche) que vous alternez ;\n"
         "- quand vous voulez des **conversations courtes et ciblées**, plus rapides et moins coûteuses, sans perdre "
         "le fil ;\n"
         "- quand vous voulez pouvoir **retrouver** pourquoi une décision a été prise ;\n"
         "- quand vous reprenez un sujet **après une pause** et voulez que Claude (et vous) sachiez où on en était.\n\n"
         "### Quand ce n'est pas nécessaire\n"
         "- une question ponctuelle (« comment on fait X en Python ? ») : utilisez *Divers*, sans récap ;\n"
         "- une tâche courte qui tient en une conversation ;\n"
         "- un sujet où vous voulez justement un regard neuf : décochez *Contexte cowork* dans la barre rapide.\n\n"
         "### Ce que ça coûte\n"
         "Un récap est généré par Haiku : quelques secondes et une très petite part du quota. Le contexte injecté "
         "est plafonné (environ 10 000 jetons au maximum) et n'est envoyé qu'au **démarrage** d'une session. C'est en "
         "général bien moins cher que de prolonger une conversation géante.\n\n"
         "> En résumé : le mode cowork transforme une suite de conversations isolées en **un projet qui a une "
         "mémoire**. Les sections suivantes expliquent comment l'organiser et l'entretenir."},
        {"Comprendre et bien utiliser", "Bien organiser un projet",
         "## Bien organiser un projet\n\n"
         "### Un projet = un dossier de code\n"
         "Le **dossier de travail** d'un projet est l'endroit où Claude lit et modifie les fichiers. Un dépôt git = "
         "un projet, en général.\n\n"
         "### Des thèmes = des préoccupations\n"
         "Découpez le projet selon **ce à quoi vous pensez quand vous travaillez**, pas selon l'arborescence des "
         "fichiers. Bons découpages :\n\n"
         "| Projet | Thèmes possibles |\n|---|---|\n"
         "| Jeu vidéo | Moteur › Physique, Moteur › Rendu, Gameplay, Outils, Idées |\n"
         "| Site web | Front, API, Base de données, Déploiement, Bugs |\n"
         "| Mémoire de fin d'études | Recherche biblio, Expériences, Rédaction, Relecture |\n\n"
         "Règle pratique : un thème doit regrouper des conversations dont les **récaps sont utiles les uns aux "
         "autres**. Si vous ne voudriez pas que Claude lise le récap A en travaillant sur B, ils ne vont pas dans le "
         "même thème.\n\n"
         "### Où ranger une conversation ?\n"
         "- un sujet précis → dans son **thème** (ou sous-thème) ;\n"
         "- une discussion sur **le projet entier** (architecture, priorités, planning) → à la **racine du projet** ;\n"
         "- une question sans lien avec un projet → **Divers**.\n\n"
         "Une conversation mal rangée se déplace d'un **glisser-déposer**.\n\n"
         "### Les notes : votre CLAUDE.md par dossier\n"
         "Les **notes** d'un projet ou d'un thème (Inspecteur) sont injectées dans chaque nouvelle conversation du "
         "dossier et de ses sous-dossiers. Elles sont courtes et stables :\n\n"
         "- objectif du projet ou du thème en deux phrases ;\n"
         "- conventions (« code en français », « pas de dépendance externe », « tests avec doctest ») ;\n"
         "- pièges connus (« ne jamais toucher à legacy/ », « l'API de paiement est en bac à sable »).\n\n"
         "Ce qui évolue (l'état d'avancement) n'a pas sa place dans les notes : c'est le rôle des récaps et des "
         "synthèses.\n\n"
         "### Les statuts\n"
         "*Idée*, *En cours*, *Bloqué*, *Terminé* (clic droit sur une conversation) : un coup d'œil à l'arbre suffit "
         "pour savoir quoi reprendre. Épinglez (★) les conversations de référence."},
        {"Comprendre et bien utiliser", "Récaps, synthèses et contexte injecté",
         "## Récaps, synthèses et contexte injecté\n\n"
         "### Ce qui est injecté, exactement\n"
         "Au démarrage d'une **nouvelle** session dans un dossier, Claude reçoit (dans son prompt système, invisible "
         "dans le fil) :\n\n"
         "1. le nom du projet, son dossier de travail et l'emplacement de la conversation ;\n"
         "2. les **notes** et **synthèses** du projet et de chaque dossier parent, du plus général au plus précis ;\n"
         "3. les **récaps** des conversations précédentes du même dossier, les plus récentes d'abord, jusqu'à un "
         "plafond d'environ 40 000 caractères ;\n"
         "4. à la racine d'un projet, la liste des thèmes.\n\n"
         "Vous pouvez le lire avant d'envoyer : *Configuration › Prompt système › Contexte cowork injecté*.\n\n"
         "### Quand générer un récap\n"
         "- à la **fin** d'une conversation qui a produit quelque chose (une décision, du code, un diagnostic) ;\n"
         "- avant une **pause** longue ;\n"
         "- avant de **repartir** d'un contexte neuf.\n\n"
         "Inutile pour une conversation qui n'a rien décidé. Le récap est régénérable à tout moment (*Régénérer*), "
         "et l'option *Récap automatique à la fermeture* (Configuration › Application) le fait pour vous.\n\n"
         "### Quand générer une synthèse de dossier\n"
         "Quand un thème accumule **plus de 4 ou 5 récaps** : la synthèse les condense en un état actuel, des "
         "décisions structurantes et une liste de problèmes ouverts. Les nouvelles conversations la reçoivent en "
         "premier ; c'est la mémoire « long terme » du thème.\n\n"
         "### Vérifier et corriger\n"
         "Un récap est généré par un modèle : relisez-le dans l'Inspecteur. S'il est faux ou incomplet, "
         "régénérez-le ou précisez-le dans les **notes** du dossier. Une mémoire fausse est pire qu'une absence de "
         "mémoire.\n\n"
         "### Limites à connaître\n"
         "- Le contexte n'est injecté qu'au **démarrage** d'une session. Une conversation reprise (*Reprendre*) "
         "garde le contexte de son démarrage.\n"
         "- Un récap ne remplace pas le code : Claude relira les fichiers dont il a besoin. Le récap lui dit "
         "**où regarder** et **ce qui a déjà été décidé**."},
        {"Comprendre et bien utiliser", "Reprendre ou repartir ?",
         "## Reprendre ou repartir ?\n\n"
         "Deux façons de continuer un travail, pour deux situations différentes.\n\n"
         "| | **Reprendre** | **Repartir de ce contexte** |\n|---|---|---|\n"
         "| Ce qui se passe | la même session Claude Code continue | une nouvelle conversation du même dossier démarre |\n"
         "| Ce que Claude sait | tout l'historique, mot pour mot | les notes, synthèses et récaps du dossier |\n"
         "| Contexte utilisé | élevé, et il grandit | faible |\n"
         "| Idéal pour | finir une tâche en cours, détails frais | une nouvelle étape, un nouveau sous-sujet |\n\n"
         "### Reprendre quand…\n"
         "- la tâche n'est **pas finie** et les détails de la discussion comptent (un bug en cours d'analyse, "
         "une modification à moitié faite) ;\n"
         "- la conversation est encore **courte** (contexte sous 50 %).\n\n"
         "### Repartir quand…\n"
         "- la tâche est **finie** et vous passez à la suivante ;\n"
         "- le contexte dépasse **60–70 %** (barre *Contexte* de la barre rapide) : Claude devient moins précis et "
         "chaque message coûte plus ;\n"
         "- la discussion a **dérivé** et vous voulez repartir sur des bases propres.\n\n"
         "### La méthode recommandée\n"
         "1. **Récap** de la conversation qui se termine.\n"
         "2. Dans l'Inspecteur : **Repartir de ce contexte**.\n"
         "3. Premier message : l'étape suivante, sans réexpliquer. Claude a déjà le récap.\n\n"
         "> Alternative intermédiaire : `/compact` résume la conversation **sur place**. Pratique, mais le résumé "
         "n'est ni relisible ni réutilisable par les autres conversations du thème, contrairement à un récap."},
        {"Comprendre et bien utiliser", "Travailler en parallèle",
         "## Travailler en parallèle : le tableau de bord\n\n"
         "### Pourquoi\n"
         "Claude travaille souvent plusieurs minutes d'affilée (lire le code, modifier, compiler, relancer les "
         "tests). Pendant ce temps, vous attendez. Le tableau de bord permet de **lancer plusieurs tâches "
         "indépendantes** et de les **superviser** d'un seul regard, comme un chef d'équipe qui passe de poste en "
         "poste : vous n'intervenez que quand une carte vous le demande (⚠).\n\n"
         "### Quand c'est utile\n"
         "- des tâches **indépendantes** et longues : écrire des tests pendant qu'une autre conversation documente, "
         "explorer deux pistes de correction, auditer deux modules ;\n"
         "- une tâche longue en **mode Accepter les modifs** que vous voulez surveiller sans y rester ;\n"
         "- un deuxième écran : *Ctrl+Maj+D* ouvre le tableau de bord dans sa propre fenêtre.\n\n"
         "### Quand ce n'est pas une bonne idée\n"
         "- deux conversations qui modifient **les mêmes fichiers** dans le même dossier : elles vont se marcher "
         "dessus (chacune écrase ou casse le travail de l'autre). Répartissez les zones (« toi tu ne touches qu'à "
         "tests/ »), ou mettez l'une en **mode Plan** (lecture seule) ;\n"
         "- des tâches qui **dépendent** l'une de l'autre : faites-les en séquence ;\n"
         "- quand votre **quota** est serré : deux conversations consomment deux fois plus vite. Gardez un œil sur "
         "la jauge 5 h en haut du tableau de bord.\n\n"
         "### Bien s'en servir\n"
         "- donnez à chaque conversation une consigne **autonome et complète** (objectif, périmètre, critère de fin) ;\n"
         "- demandez à Claude de tenir une **liste de tâches** : sa progression s'affiche sur la carte ;\n"
         "- les cartes qui vous attendent passent **en premier** : autorisez ou refusez directement sur la carte, ou "
         "ajoutez une consigne avec la réponse rapide ;\n"
         "- le **fil d'activité** à droite montre tout ce qui se passe, toutes conversations confondues."},
        {"Comprendre et bien utiliser", "Doser l'autonomie et la réflexion",
         "## Doser l'autonomie et la réflexion\n\n"
         "Trois réglages déterminent **combien Claude réfléchit** et **combien il agit seul**. Les bons réglages "
         "dépendent de la tâche, pas de vos goûts : les changer en cours de route est normal.\n\n"
         "### Le mode Plan : réfléchir avant d'agir\n"
         "En mode **Plan**, Claude ne peut que lire. Il explore le code, puis présente un plan que vous approuvez (ou "
         "faites corriger) avant qu'il ne touche à quoi que ce soit.\n\n"
         "- **Utile** pour les changements qui touchent **plusieurs fichiers**, une refonte, une fonctionnalité dont "
         "le design n'est pas évident, ou un code que vous connaissez mal : un mauvais plan se corrige en une "
         "phrase, une mauvaise implémentation de 400 lignes se défait difficilement.\n"
         "- **Inutile** pour une correction d'une ligne ou une question.\n\n"
         "### Le niveau d'autonomie : une question de confiance\n"
         "| Situation | Mode conseillé |\n|---|---|\n"
         "| Découverte d'un code, tâche risquée | Normal (vous validez chaque action) |\n"
         "| Plan approuvé, périmètre clair, git propre | Accepter les modifs |\n"
         "| Tâche longue que vous ne voulez pas surveiller | Auto (Claude juge ce qui est sûr) |\n"
         "| Audit, revue, exploration | Plan, ou Ne pas demander |\n\n"
         "Plus vous donnez d'autonomie, plus il est important d'avoir un **filet** : un dépôt git propre (pour "
         "annuler), des tests, et des règles qui interdisent le dangereux (Configuration › Outils, ex. "
         "interdire `Bash(rm *)`).\n\n"
         "### L'effort : la profondeur de réflexion\n"
         "- **Faible / Moyen** : questions simples, modifications mécaniques, renommages. Rapide et économe.\n"
         "- **Élevé** : la plupart du développement.\n"
         "- **Très élevé / Maximum** : bugs subtils, architecture, algorithmes ; plus lent et plus coûteux en quota.\n\n"
         "### Le modèle\n"
         "Haiku pour les petites tâches et les récaps, Sonnet pour l'équilibre, Opus pour le difficile. Changer de "
         "modèle **au milieu** d'une conversation est possible : par exemple Opus pour concevoir, puis Sonnet pour "
         "dérouler l'implémentation.\n\n"
         "### Les modèles de prompts\n"
         "Le prompt système (Configuration › Prompt système) fixe un **rôle** pour toute la conversation : relecteur "
         "exigeant, pédagogue, rédacteur concis… Enregistrez ceux qui vous servent souvent comme **modèles** : un "
         "clic suffit ensuite. Pour des consignes liées à un projet plutôt qu'à un rôle, préférez les **notes** du "
         "dossier."},
        {"Comprendre et bien utiliser", "Une journée type",
         "## Une journée type avec ClaudeDesk\n\n"
         "Pour voir comment tout s'articule, voici une séance de travail réaliste sur un projet de jeu.\n\n"
         "1. **Matin, reprise.** Écran d'accueil → *Reprendre* la dernière conversation de *Moteur › Physique*, "
         "statut *En cours*. Le récap dans l'Inspecteur rappelle où on en était : « collisions sur les pentes : "
         "correctif à tester ».\n"
         "2. **Finir la tâche.** La conversation est courte (contexte à 30 %) : on la **reprend** telle quelle. "
         "Claude relance les tests, ça passe. **Récap**, statut *Terminé*.\n"
         "3. **Nouvelle étape.** Sélection de *Gameplay*, Ctrl+N, mode **Plan** : « ajoute un double saut ». "
         "Claude reçoit automatiquement les notes du projet et les récaps de *Gameplay* ; il propose un plan qui "
         "respecte les conventions déjà décidées. On ajuste, on approuve, passage en **Accepter les modifs**.\n"
         "4. **En parallèle.** Pendant que le double saut s'implémente, une deuxième conversation dans *Outils* "
         "écrit un script d'export des niveaux (autre zone du code : pas de conflit). **Ctrl+2** : les deux cartes "
         "avancent ; une ⚠ demande l'autorisation de lancer la compilation, on autorise depuis la carte.\n"
         "5. **Fin de journée.** Récap des deux conversations. *Moteur* a accumulé six récaps : clic droit → "
         "**Synthèse du dossier**. Demain, n'importe quelle nouvelle conversation de *Moteur* démarrera avec cet "
         "état des lieux.\n\n"
         "Rien de tout cela n'est obligatoire. Mais c'est la différence entre « discuter avec Claude » et "
         "« **travailler avec Claude sur la durée** »."},
        {"Comprendre et bien utiliser", "Questions fréquentes",
         "## Questions fréquentes\n\n"
         "### « Claude Code introuvable »\n"
         "Installez Claude Code puis vérifiez *Configuration › Application* : le chemin de `claude.exe` est détecté "
         "automatiquement, ou indiquez-le à la main.\n\n"
         "### Claude ne voit pas mes fichiers\n"
         "Vérifiez le **dossier de travail** du projet (Inspecteur, sélection du projet) : Claude travaille dans ce "
         "dossier. Pour d'autres dossiers : *Configuration › Avancé › Dossiers supplémentaires*.\n\n"
         "### Claude ignore une décision prise dans une autre conversation\n"
         "Le contexte cowork ne contient que ce qui est dans les **récaps** et les **notes**. Générez le récap de la "
         "conversation où la décision a été prise, vérifiez qu'elle y figure, ou ajoutez-la aux notes du dossier.\n\n"
         "### Le contexte est presque plein\n"
         "Générez un **récap**, puis *Repartir de ce contexte* (voir *Reprendre ou repartir ?*), ou tapez `/compact`.\n\n"
         "### Mon changement de réglage ne s'applique pas\n"
         "Cliquez *Appliquer à la conversation*. Pour l'effort, les outils et le prompt, la session redémarre au "
         "prochain message (historique conservé).\n\n"
         "### Où sont mes données ?\n"
         "Conversations : le dossier de l'espace de travail (*Fichier › Ouvrir le dossier de l'espace de travail*). "
         "Réglages : `%APPDATA%\\ClaudeDesk`. Les éléments supprimés vont dans `.corbeille`.\n\n"
         "### Revoir l'accueil des nouveaux utilisateurs\n"
         "*Configuration › Application › Accueil* : le guide s'ouvrira de nouveau au prochain lancement. "
         "*Réinitialiser les préférences* remet tout ClaudeDesk à zéro (vos conversations sont conservées).\n\n"
         "### Quelque chose ne marche pas\n"
         "*Aide › Journal du CLI* affiche les messages d'erreur de Claude Code."},
    };
    return s;
}

}  // namespace

void App::drawGuideWindow() {
    const auto& pal = ui::palette();
    float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fs * 56, fs * 36), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("Guide de démarrage", &showGuide_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    const auto& secs = guideSections();
    guideSection_ = std::clamp(guideSection_, 0, (int)secs.size() - 1);

    // --- Sommaire, par groupes ---
    ImGui::BeginChild("##toc", ImVec2(fs * 16, 0), ImGuiChildFlags_Borders);
    const char* lastGroup = "";
    for (int i = 0; i < (int)secs.size(); ++i) {
        if (std::string(secs[i].group) != lastGroup) {
            lastGroup = secs[i].group;
            if (i > 0) ImGui::Dummy(ImVec2(0, 4));
            ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 0.85f);
            ImGui::TextColored(pal.accent, "%s", util::toLower(lastGroup) == "accueil" ? "ACCUEIL"
                                                 : std::string(lastGroup) == "Mode d'emploi" ? "MODE D'EMPLOI"
                                                                                          : "COMPRENDRE ET BIEN UTILISER");
            ImGui::PopFont();
        }
        bool isConcept = std::string(secs[i].group) == "Comprendre et bien utiliser";
        std::string label = std::string(isConcept ? "◆ " : "   ") + secs[i].title;
        if (ImGui::Selectable(label.c_str(), guideSection_ == i)) guideSection_ = i;
        if (isConcept && ImGui::IsItemHovered())
            ImGui::SetTooltip("Pourquoi et quand utiliser cette fonctionnalité");
    }
    ImGui::Separator();
    ImGui::TextDisabled("Essayer :");
    if (ImGui::Button("Visite guidée", ImVec2(-1, 0))) startTour();
    if (ImGui::Button("Projet exemple", ImVec2(-1, 0))) createExampleProject();
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Contenu ---
    ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_None);
    const GuideSection& s = secs[guideSection_];
    ImGui::BeginChild("##text", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.3f));
    static int scrolledFor = -1;
    if (scrolledFor != guideSection_) {
        ImGui::SetScrollY(0);
        scrolledFor = guideSection_;
    }
    if (s.body) {
        static std::vector<ui::MdBlock> blocks;
        static int parsedFor = -1;
        if (parsedFor != guideSection_) {
            blocks = ui::parseMarkdown(s.body);
            parsedFor = guideSection_;
        }
        ui::MdStyle md{fontUi, fontBold, fontMono, false, fontItalic};
        ui::renderMarkdown(blocks, md, 66000 + guideSection_);
    } else {
        ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.35f);
        ImGui::TextColored(pal.accent, "Raccourcis clavier");
        ImGui::PopFont();
        drawShortcutTables();
    }
    // Actions contextuelles : passer de l'explication à la pratique
    ImGui::Dummy(ImVec2(0, 8));
    std::string t = s.title;
    auto action = [&](const char* label, auto fn) {
        if (ImGui::Button(label)) fn();
        ImGui::SameLine();
    };
    auto openConfig = [&](int tab) {
        showConfig_ = true;
        configTab_ = tab;
        page_ = 0;
    };
    if (t == "Bienvenue") {
        action("Visite guidée (2 min)", [&] { startTour(); });
        action("Projet exemple", [&] { createExampleProject(); });
        action("Lire la suite →", [&] { guideSection_ = 1; });
    } else if (t == "Démarrer en 2 minutes") {
        action("Nouveau projet…", [&] { openNewProject_ = true; });
        action("Projet exemple", [&] { createExampleProject(); });
    } else if (t == "L'écran en un coup d'œil") {
        action("Visite guidée", [&] { startTour(); });
        action("Réinitialiser la disposition", [&] {
            resetLayout_ = true;
            page_ = 0;
        });
    } else if (t == "Autorisations et modes") {
        action("Ouvrir la configuration (Mode)", [&] { openConfig(1); });
    } else if (t == "Modèle, effort et configuration") {
        action("Ouvrir la configuration", [&] { openConfig(0); });
    } else if (t == "Usage et limites") {
        action("Actualiser l'usage", [&] { refreshUsage(); });
    } else if (t == "Reprendre et importer") {
        action("Importer depuis Claude Code…", [&] {
            showHistory_ = true;
            histProjects_ = claude::listHistoryProjects();
        });
    } else if (t == "Le mode cowork : pourquoi ?" || t == "Une journée type") {
        action("Voir un projet organisé (exemple)", [&] { createExampleProject(); });
        action("Créer mon projet…", [&] { openNewProject_ = true; });
    } else if (t == "Bien organiser un projet") {
        action("Créer mon projet…", [&] { openNewProject_ = true; });
        action("Ouvrir le dossier de l'espace de travail", [&] { util::openInShell(util::pathToUtf8(workspace_.root())); });
    } else if (t == "Récaps, synthèses et contexte injecté") {
        action("Voir le contexte injecté", [&] { openConfig(3); });
        action("Récap automatique à la fermeture…", [&] { openConfig(5); });
    } else if (t == "Reprendre ou repartir ?") {
        action("Projet exemple (Inspecteur › Repartir)", [&] { createExampleProject(); });
    } else if (t == "Travailler en parallèle") {
        action("Afficher le tableau de bord", [&] {
            if (!dash_) page_ = 1;
        });
        action("Dans une fenêtre séparée", [&] { openDashboardWindow(); });
    } else if (t == "Doser l'autonomie et la réflexion") {
        action("Modes de permission", [&] { openConfig(1); });
        action("Outils et règles", [&] { openConfig(2); });
        action("Modèles de prompts", [&] { openConfig(3); });
    } else if (t == "Questions fréquentes") {
        action("Journal du CLI", [&] { showLog_ = true; });
        action("Accueil et réinitialisation", [&] { openConfig(5); });
    }
    ImGui::NewLine();
    ImGui::EndChild();
    // Navigation
    ImGui::BeginDisabled(guideSection_ == 0);
    if (ImGui::Button("← Précédent")) --guideSection_;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(guideSection_ == (int)secs.size() - 1);
    if (ImGui::Button("Suivant →")) ++guideSection_;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d", guideSection_ + 1, (int)secs.size());
    ImGui::EndChild();
    ImGui::End();
}

// ===========================================================================
// Rappel « le guide est disponible » (lancements 2 à 11)
// ===========================================================================

void App::drawGuideReminder() {
    // Le guide est déjà ouvert : le rappel n'a plus de raison d'être.
    if (showGuide_ && showGuideReminder_) {
        showGuideReminder_ = false;
        reminderFadeOut_ = now_;
    }
    if (!showGuideReminder_ && reminderFadeOut_ < 0) return;
    const auto& pal = ui::palette();
    float fs = ImGui::GetFontSize();
    if (reminderStart_ <= 0) reminderStart_ = now_;
    double age = now_ - reminderStart_;

    // Opacité : apparition puis disparition en fondu une fois congédié
    float alpha = settings_.animations ? ui::anim::easeOutCubic((float)(age / 0.45)) : 1.0f;
    if (reminderFadeOut_ >= 0) {
        float k = settings_.animations ? (float)((now_ - reminderFadeOut_) / 0.3) : 1.0f;
        if (k >= 1.0f) {
            reminderFadeOut_ = -1;
            return;
        }
        alpha *= 1.0f - k;
    }
    ui::anim::markBusy();

    // Ancre : le menu « Aide » (là où le guide se trouve)
    ImVec2 amin(aideMenuRect_[0], aideMenuRect_[1]), amax(aideMenuRect_[2], aideMenuRect_[3]);
    if (amax.x <= amin.x) {
        amin = ImVec2(fs * 12, 0);
        amax = ImVec2(fs * 15, ImGui::GetFrameHeight());
    }
    float bob = settings_.animations ? 4.0f * std::sin((float)ui::anim::time() * 3.2f) : 0.0f;  // petite oscillation
    float slide = (1.0f - alpha) * 14.0f;
    ImVec2 tip((amin.x + amax.x) * 0.5f, amax.y + 4 + bob + slide);

    // Halo autour du menu Aide + flèche
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    float pulse = settings_.animations ? ui::anim::pulse(1.0f) : 1.0f;
    for (int k = 0; k < 4; ++k) {
        float e = 1.0f + k * 2.0f;
        fg->AddRect(ImVec2(amin.x - e, amin.y + 1 - e * 0.3f), ImVec2(amax.x + e, amax.y - 1 + e * 0.3f),
                    ui::col(pal.accent, alpha * (0.55f - k * 0.12f) * (0.4f + 0.6f * pulse)), 6.0f + e, 0, 2.0f);
    }
    float aw = fs * 0.55f;
    fg->AddTriangleFilled(ImVec2(tip.x, tip.y), ImVec2(tip.x - aw, tip.y + aw * 1.1f), ImVec2(tip.x + aw, tip.y + aw * 1.1f),
                          ui::col(pal.accent, alpha));

    // Bulle
    ImGui::SetNextWindowPos(ImVec2(std::max(8.0f, tip.x - fs * 3), tip.y + aw * 1.1f - 1));
    ImGui::SetNextWindowBgAlpha(0.97f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.02f, alpha));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs * 0.9f, fs * 0.7f));
    ImGui::PushStyleColor(ImGuiCol_Border, pal.accent);
    ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 22, 0), ImVec2(fs * 22, FLT_MAX));
    ImGui::Begin("##guidereminder", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImGui::PushFont(fontBold, 0.0f);
    ImGui::TextColored(pal.accent, "Le guide est toujours là");
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted("Menu Aide › Guide de démarrage (Maj+F1), ou l'écran d'accueil quand aucune conversation "
                           "n'est ouverte. Il explique aussi pourquoi et quand utiliser le mode cowork, les récaps et "
                           "le tableau de bord.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 2));
    bool dismiss = false;
    ImGui::PushStyleColor(ImGuiCol_Button, pal.accent);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button("Ouvrir le guide") && reminderFadeOut_ < 0) {
        showGuide_ = true;
        guideSection_ = 0;
        dismiss = true;
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Button("Non merci")) dismiss = true;
    int remaining = std::max(0, 11 - settings_.launchCount);
    if (remaining > 0) ImGui::TextDisabled("Ce rappel s'affichera encore %d fois.", remaining);
    else ImGui::TextDisabled("Dernier rappel.");
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);

    // Un clic ailleurs (après une courte période de grâce) ferme aussi le rappel.
    if (!hovered && age > 0.4 && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1))) dismiss = true;
    if (dismiss && reminderFadeOut_ < 0) {
        showGuideReminder_ = false;
        reminderFadeOut_ = now_;
    }
}

// ===========================================================================
// Projet exemple
// ===========================================================================

namespace {

const char* kExampleName = "Exemple - Mini calculatrice";

const char* kReadme = R"(# Mini calculatrice — projet d'exemple ClaudeDesk

Petite calculatrice en ligne de commande (C++17), **volontairement imparfaite** pour s'entraîner avec Claude.

## Fichiers
- `src/calc.h`, `src/calc.cpp` : évaluation d'une expression `a op b`
- `src/main.cpp` : boucle interactive
- `tests/test_calc.cpp` : tests minimalistes (assert)
- `CMakeLists.txt` : `cmake -B build && cmake --build build`

## Défauts connus (exprès !)
1. `1 / 0` renvoie `inf` au lieu d'être refusé.
2. `2 + 3 * 4` renvoie `5` : tout ce qui suit le premier calcul est ignoré.
3. Pas d'opérateur puissance `^`.

## Exercices dans ClaudeDesk
- Conversation *Code › Parseur › Division par zéro* : répondez « Ajoute le test puis applique le correctif ».
- Passez en **mode Plan** et demandez : « Propose un plan pour gérer la priorité des opérateurs ».
- Ouvrez deux conversations en parallèle et suivez-les dans le **tableau de bord** (Ctrl+2).
- Générez un **récap**, puis « Repartir de ce contexte » depuis l'Inspecteur.
)";

const char* kCalcH = R"(#pragma once
#include <optional>
#include <string>

// Évalue une expression de la forme "a op b" (op : + - * /).
// Retourne std::nullopt si l'expression est invalide.
std::optional<double> evaluate(const std::string& expr);
)";

const char* kCalcCpp = R"(#include "calc.h"

#include <sstream>

std::optional<double> evaluate(const std::string& expr) {
    std::istringstream in(expr);
    double a = 0, b = 0;
    char op = 0;
    if (!(in >> a >> op >> b)) return std::nullopt;
    // BUG : la suite de l'expression est ignorée ("2 + 3 * 4" donne 5).
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return a / b;  // BUG : pas de vérification de la division par zéro
    }
    return std::nullopt;
}
)";

const char* kMainCpp = R"(#include <iostream>

#include "calc.h"

int main() {
    std::cout << "Mini calculatrice - tapez une expression (ex. 2 + 3), ou q pour quitter.\n";
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        if (line == "q") break;
        if (auto r = evaluate(line)) std::cout << *r << "\n";
        else std::cout << "Expression invalide\n";
    }
}
)";

const char* kTestCpp = R"(#include <cassert>

#include "../src/calc.h"

int main() {
    assert(*evaluate("2 + 3") == 5);
    assert(*evaluate("10 / 4") == 2.5);
    assert(!evaluate("bonjour"));
    // À ajouter : division par zéro, expressions à plusieurs opérateurs...
    return 0;
}
)";

const char* kCMake = R"(cmake_minimum_required(VERSION 3.16)
project(MiniCalc CXX)
set(CMAKE_CXX_STANDARD 17)
add_executable(minicalc src/main.cpp src/calc.cpp)
add_executable(test_calc tests/test_calc.cpp src/calc.cpp)
)";

struct Builder {
    claude::ClaudeSession s;
    int64_t t;
    int toolCounter = 0;
    Builder(const std::string& title, const std::string& cwd, int64_t start) : t(start) {
        s.title = title;
        s.cwd = cwd;
        Entry e;
        e.kind = EntryKind::System;
        e.time = t;
        e.text = "Conversation d'exemple pré-enregistrée (aucun quota consommé). Écrivez un message pour continuer "
                 "avec le vrai Claude : il recevra ce fil et le contexte du dossier.";
        s.entries.push_back(e);
    }
    void user(const std::string& text) { push(EntryKind::User, text); }
    void claude(const std::string& text) { push(EntryKind::Assistant, text); }
    void tool(const std::string& name, const json& input, const std::string& result, bool error = false) {
        Entry e;
        e.kind = EntryKind::ToolUse;
        e.time = (t += 20);
        e.toolName = name;
        e.toolUseId = "toolu_exemple_" + std::to_string(++toolCounter) + "_" + s.localId.substr(0, 6);
        e.toolInput = input.dump(2);
        e.text = claude::toolSummary(name, input);
        e.toolResult = result;
        e.toolError = error;
        e.toolDone = true;
        s.entries.push_back(e);
    }
    void push(EntryKind k, const std::string& text) {
        Entry e;
        e.kind = k;
        e.text = text;
        e.time = (t += 45);
        s.entries.push_back(e);
    }
};

}  // namespace

void App::createExampleProject() {
    // Déjà créé ? On l'ouvre simplement.
    for (auto& p : workspace_.projects())
        if (p->name == kExampleName) {
            selectedFolder_ = util::pathToUtf8(p->path);
            selectedConv_.clear();
            page_ = 0;
            toast("Le projet exemple existe déjà : il est sélectionné dans l'espace de travail.");
            return;
        }

    // 1. Le code de la calculatrice, à côté des réglages de l'application.
    fs::path code = util::pathFromUtf8(settingsPath_).parent_path() / "exemple" / "mini-calculatrice";
    util::writeFileAtomic(code / "README.md", kReadme);
    util::writeFileAtomic(code / "CMakeLists.txt", kCMake);
    util::writeFileAtomic(code / "src" / "calc.h", kCalcH);
    util::writeFileAtomic(code / "src" / "calc.cpp", kCalcCpp);
    util::writeFileAtomic(code / "src" / "main.cpp", kMainCpp);
    util::writeFileAtomic(code / "tests" / "test_calc.cpp", kTestCpp);
    std::string cwd = util::pathToUtf8(code);
    std::string calcPath = util::pathToUtf8(code / "src" / "calc.cpp");
    std::string testPath = util::pathToUtf8(code / "tests" / "test_calc.cpp");

    // 2. Le projet et ses thèmes.
    std::string err;
    ws::Folder* proj = workspace_.createProject(kExampleName, cwd, &err);
    if (!proj) {
        toast("Création du projet exemple impossible : " + err, true);
        return;
    }
    proj->notes = "Projet d'exemple pour découvrir ClaudeDesk : calculatrice en ligne de commande, C++17.\n"
                  "Conventions : code et commentaires en français, pas de dépendance externe, "
                  "tests avec assert dans tests/.";
    workspace_.saveFolderMeta(proj);
    std::string projPath = util::pathToUtf8(proj->path);
    ws::Folder* codeF = workspace_.createFolder(proj, "Code");
    std::string codePath = util::pathToUtf8(codeF->path);
    ws::Folder* parser = workspace_.createFolder(codeF, "Parseur");
    ws::Folder* tests = workspace_.createFolder(proj, "Tests");
    ws::Folder* ideas = workspace_.createFolder(proj, "Idées");
    parser->notes = "Le parseur actuel lit exactement « a op b » avec un istringstream.";
    workspace_.saveFolderMeta(parser);

    int64_t base = util::nowUnix() - 6 * 3600;
    auto save = [&](ws::Folder* f, Builder& b, const std::string& status, const std::string& recap) -> std::string {
        ws::ConversationMeta m;
        m.id = b.s.localId;
        m.status = status;
        m.recap = recap;
        m.recapTime = b.t + 60;
        m.created = b.s.entries.front().time;
        json j = b.s.toJson();
        j["injectContext"] = true;
        workspace_.saveConversation(f, j, &m);
        return b.s.localId;
    };

    // --- Projet : présentation
    {
        Builder b("Présentation du projet", cwd, base);
        b.user("Explique-moi rapidement l'architecture de ce projet.");
        b.tool("Glob", {{"pattern", "**/*"}},
               "CMakeLists.txt\nREADME.md\nsrc/calc.cpp\nsrc/calc.h\nsrc/main.cpp\ntests/test_calc.cpp");
        b.tool("Read", {{"file_path", calcPath}}, kCalcCpp);
        b.claude("## Architecture\n\nLe projet est minuscule et bien découpé :\n\n"
                 "| Fichier | Rôle |\n|---|---|\n"
                 "| `src/calc.h` / `src/calc.cpp` | évalue une expression `a op b` |\n"
                 "| `src/main.cpp` | boucle interactive : lecture → calcul → affichage |\n"
                 "| `tests/test_calc.cpp` | tests avec `assert` |\n\n"
                 "**Deux défauts** repérés en lisant `calc.cpp` :\n"
                 "- `evaluate` ne lit que **trois jetons** : `2 + 3 * 4` renvoie `5` sans erreur ;\n"
                 "- la **division par zéro** renvoie `inf` au lieu d'être signalée.\n\n"
                 "Je propose de les traiter dans le thème **Code › Parseur**, et d'ajouter les tests "
                 "correspondants dans **Tests**.");
        save(proj, b, "done",
             "### Objectif\nComprendre l'architecture de la mini calculatrice.\n"
             "### Ce qui a été fait\nLecture de l'arborescence et de `src/calc.cpp`.\n"
             "### Décisions et points importants\n- Le parseur ne lit que `a op b` : la suite est ignorée.\n"
             "- La division par zéro n'est pas signalée.\n"
             "### Fichiers concernés\n`src/calc.h`, `src/calc.cpp`, `src/main.cpp`, `tests/test_calc.cpp`\n"
             "### Reste à faire\nCorriger les deux défauts (Code › Parseur), compléter les tests (Tests).");
    }
    // --- Code › Parseur : division par zéro (en cours, correctif refusé pour écrire d'abord un test)
    std::string mainConv;
    {
        Builder b("Division par zéro", cwd, base + 3600);
        b.user("`1 / 0` renvoie inf. Propose un correctif.");
        b.tool("Read", {{"file_path", calcPath}}, kCalcCpp);
        b.claude("`evaluate` renvoie déjà `std::nullopt` pour une expression invalide, et `main` affiche alors "
                 "« Expression invalide ». Le plus simple est donc de **refuser un diviseur nul** :");
        b.tool("Edit",
               {{"file_path", calcPath},
                {"old_string", "        case '/': return a / b;  // BUG : pas de vérification de la division par zéro"},
                {"new_string", "        case '/':\n            if (b == 0) return std::nullopt;  // division par zéro refusée\n"
                               "            return a / b;"}},
               "L'utilisateur a refusé cette action. Consigne : je veux d'abord un test qui échoue.", true);
        b.user("Attends : je veux d'abord un test qui échoue.");
        b.claude("Bonne approche (développement piloté par les tests). Voici le test à ajouter dans "
                 "`tests/test_calc.cpp` :\n\n```cpp\nassert(!evaluate(\"1 / 0\"));  // doit être refusé\n```\n\n"
                 "Il échoue aujourd'hui (le résultat vaut `inf`). Dites-moi quand l'ajouter et appliquer le correctif "
                 "ci-dessus.");
        mainConv = save(parser, b, "active",
                        "### Objectif\nRefuser la division par zéro dans `evaluate`.\n"
                        "### Ce qui a été fait\nCorrectif proposé (`if (b == 0) return std::nullopt;`) mais **refusé** : "
                        "l'utilisateur veut d'abord un test qui échoue.\n"
                        "### Décisions et points importants\nApproche TDD : test d'abord, correctif ensuite.\n"
                        "### Fichiers concernés\n`src/calc.cpp`, `tests/test_calc.cpp`\n"
                        "### Reste à faire\n1. Ajouter `assert(!evaluate(\"1 / 0\"));`\n2. Appliquer le correctif.");
    }
    // --- Tests : plan de tests avec liste de tâches
    {
        Builder b("Plan de tests", cwd, base + 2 * 3600);
        b.user("Quels tests faudrait-il ajouter ?");
        b.tool("Read", {{"file_path", testPath}}, kTestCpp);
        b.tool("TodoWrite",
               {{"todos", json::array({{{"content", "Tester la division par zéro"}, {"status", "completed"},
                                        {"activeForm", "Teste la division par zéro"}},
                                       {{"content", "Tester 2 + 3 * 4 (priorité des opérateurs)"}, {"status", "in_progress"},
                                        {"activeForm", "Teste la priorité des opérateurs"}},
                                       {{"content", "Tester les nombres négatifs et décimaux"}, {"status", "pending"},
                                        {"activeForm", "Teste les nombres négatifs"}},
                                       {{"content", "Tester les entrées invalides (texte, ligne vide)"}, {"status", "pending"},
                                        {"activeForm", "Teste les entrées invalides"}}})}},
               "Liste de tâches mise à jour.");
        b.claude("Quatre familles de tests, par ordre d'importance :\n\n"
                 "1. **Division par zéro** — `assert(!evaluate(\"1 / 0\"));`\n"
                 "2. **Priorité des opérateurs** — `2 + 3 * 4` doit valoir `14` (échoue aujourd'hui).\n"
                 "3. **Nombres négatifs et décimaux** — `-2.5 * 2` → `-5`.\n"
                 "4. **Entrées invalides** — texte, ligne vide, opérateur inconnu `2 % 3`.\n\n"
                 "Les tests 1 et 2 **échoueront** tant que le parseur n'est pas corrigé : c'est voulu.");
        save(tests, b, "idea",
             "### Objectif\nDéfinir les tests manquants.\n### Ce qui a été fait\nPlan en 4 familles (liste de tâches).\n"
             "### Décisions et points importants\nLes tests de division par zéro et de priorité doivent échouer avant correction.\n"
             "### Fichiers concernés\n`tests/test_calc.cpp`\n### Reste à faire\nÉcrire les tests 2 à 4.");
    }
    // --- Idées : opérateur puissance
    {
        Builder b("Opérateur puissance ^", cwd, base + 3 * 3600);
        b.user("Ce serait bien d'avoir ^ pour les puissances. Comment tu ferais ?");
        b.claude("Deux options :\n\n"
                 "### A. Le minimum\nAjouter `case '^': return std::pow(a, b);` — 2 lignes, mais le parseur reste "
                 "limité à `a op b`.\n\n"
                 "### B. Un vrai parseur (recommandé)\nUn **parseur récursif descendant** avec trois niveaux de "
                 "priorité :\n\n```\nexpression := terme (('+' | '-') terme)*\nterme      := facteur (('*' | '/') facteur)*\n"
                 "facteur    := nombre ('^' facteur)?   // ^ associatif à droite\n```\n\n"
                 "Il règle **en même temps** le bug `2 + 3 * 4` et permettrait d'ajouter les parenthèses.\n\n"
                 "> Pour une évolution de cette taille, passez en **mode Plan** : je proposerai les étapes avant de "
                 "toucher au code.");
        save(ideas, b, "idea",
             "### Objectif\nAjouter l'opérateur `^`.\n### Ce qui a été fait\nDeux options comparées.\n"
             "### Décisions et points importants\nOption B recommandée : parseur récursif descendant (corrige aussi la priorité).\n"
             "### Fichiers concernés\n`src/calc.cpp`\n### Reste à faire\nPlanifier en mode Plan, puis implémenter.");
    }

    // Synthèse du thème Code (comme si elle avait été générée)
    if (ws::Folder* cf = workspace_.findFolderByPath(util::pathFromUtf8(codePath))) {
        cf->recap = "### État actuel\nLe parseur ne gère que `a op b`. Correctif de la division par zéro prêt, en attente "
                    "d'un test.\n### Décisions structurantes\nTDD ; viser un parseur récursif descendant.\n"
                    "### Problèmes ouverts / reste à faire\n- Division par zéro (test + correctif)\n"
                    "- Priorité des opérateurs";
        cf->recapTime = util::nowUnix();
        workspace_.saveFolderMeta(cf);
    }

    selectedFolder_ = projPath;
    selectedConv_ = mainConv;
    page_ = 0;
    showWorkspace_ = showInspector_ = true;
    openConversation(mainConv);
    toast("Projet exemple créé : explorez l'arbre à gauche, puis répondez à Claude dans « Division par zéro ».");
}

}  // namespace app
