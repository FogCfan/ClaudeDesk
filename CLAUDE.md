# ClaudeDesk — consignes pour Claude Code

Interface de bureau pour Claude Code : pilote `claude -p` en stream-json, usage en direct,
panneaux de configuration, mode cowork (projets › thèmes › conversations).
**Ce logiciel est développé depuis lui-même** : la session en cours tourne très probablement
dans `build\release\ClaudeDesk.exe`.

## Pile
C++20 · SDL3 · Dear ImGui (docking 1.92.9b, récupéré par FetchContent) · nlohmann_json ·
doctest · FreeType. Chaîne MSYS2 UCRT64 (gcc, cmake, ninja), presets dans `CMakePresets.json`.
Code, commentaires et textes d'interface en **français**.

## Carte du code
- `src/app` — `App` (boucle, onglets, fenêtres), `AppMenus`, `Settings` (settings.json ; préréglages d'énergie
  `applyEnergyPreset` / `energyPresetMatches`, mode automatique selon `SDL_GetPowerInfo` dans `App::tickEnergy`)
- `src/claude` — `ClaudeSession` (protocole stream-json), `Process`, `CommandLine`,
  `SessionConfig`, `Auth` (connexion : `isAuthErrorText`, `claude auth status --json` → `AuthChecker`),
  `History`, `Transcript`, `UsageTracker` (rate_limit_event + rapport de `/usage` : `claude -p "/usage"` est une commande locale, gratuite,
  qui répond même limite atteinte → `usage_report.rate_limits`), `OneShot` (récaps),
  `Designer` (conception projet + équipe d'agents à partir d'une description : prompts, JSON, validation, fichiers de départ),
  `ProjectKind` (types de projets — logiciel, idées et concept, analyse de données, rapport/audit, hybride, auto, question :
  ClaudeDesk n'est PAS réservé aux développeurs. Le type règle le Designer, le prompt de l'orchestrateur (« Nature du
  projet »), les consignes et le « Vérifié » des tâches d'agents, la mémoire injectée et les libellés. Enregistré dans
  `_dossier.json` (`projectKind`) et `OrchestratorSettings::projectKind` ; défaut « software » pour l'existant),
  `Models` (familles haiku/sonnet/opus/fable, accès selon l'abonnement : la CLI liste Fable pour tous, mais avec Pro
  il demande des crédits d'usage — refus « out of usage credits », vérifié ; refus et réponses retenus dans
  `Settings::modelAccess`, modèles bloqués jamais confiés par l'orchestrateur : `usableModels`, `blockedModels`),
  `Orchestrator` (moteur de l'orchestrateur, sans UI : directives `@@DELEGUER/@@HUMAIN/@@ATTENDRE/@@TERMINE/@@NOUVEL_AGENT`, comptes rendus au fil de l'eau, garde-fous ;
  un nouvel agent n'est JAMAIS créé sans l'accord de l'humain : `pendingAgents` → question → `OrchestratorHost::createAgent`)
- `src/ui` — panneaux ImGui : `ConversationView`, `PromptEditor`, `ConfigPanel`,
  `WorkspacePanel` (panneau « Projets » — l'ancien « Espace de travail » / « mode cowork », renommé : ne plus écrire
  « cowork » dans l'interface ; arbre à icônes, explorateur du dossier de travail, fichiers de référence
  `Folder::files`, dépôt de fichiers `dropOnProjects` ; projet orchestré : nœud « Orchestrateur » › agents › discussions ;
  une discussion = suite de sessions reliées par `ConversationMeta::continues` (rotation de contexte, « Repartir du
  récap »), regroupées par `ws::conversationChains` — liens déduits des dates pour les anciennes conversations), `Icons` (icônes vectorielles dessinées à la main : les emoji
  ne s'affichent pas dans les libellés ; `ui::iconTreeNode`, `ui::icon`, `ui::fileIcon`), `Dashboard`, `UsagePanel`, `PermissionPanel`, `Markdown`, `Theme`, `Anim`, `Tutorial`,
  `OrchestratorPanel` (page 2 « Orchestrateur », Ctrl+3, + `AppOrchHost`, l'adaptateur moteur ↔ conversations ; état dans
  `<projet>/_orchestrateur.json`). La discussion du cerveau est exclue des onglets (`isOrchBrain`) et rendue en cartes
  (`drawBrainEntry`) ; le tableau de bord l'affiche en tête avec son équipe (`drawOrchestrationHero`).
  `LoginPanel` (fenêtre « Connexion à Claude » : `claude auth login` lancé dans une fenêtre de terminal via
  `TerminalProcess`, connexion détectée par `auth status` toutes les 2 s, puis renvoi des messages refusés
  (`ClaudeSession::resendLast`) ; l'orchestrateur est mis en pause à l'échec et repris après ; un tour refusé
  faute de connexion ne compte pas dans `stats.turns`). Carte d'erreur : `drawAuthErrorCard`.
  `DesignerPanel` (page 3 « Designer », Ctrl+4 : Décrire → Clarifier → Ajuster → Créer ; brouillon `designer.json`).
- `src/workspace` — `Workspace` : vrais dossiers + `_dossier.json` + `*.conv.json`, corbeille, `buildContext`
- `src/util` — chemins, chaînes, fichiers
- `tests/` — doctest (`desk_tests`) : protocole, workspace, chaînes, modèles
- `assets/fonts` — polices fournies (IBM Plex Sans par défaut, Noto Sans, Inter, JetBrains Mono ; licence SIL OFL), copiées
  dans `fonts/` à côté de l'exe ; `Settings::uiFont` ("plex"/"noto"/"inter"/"system"), `monoFont`, `fontWeight`
  (RasterizerMultiply, 1,3). Rendu par défaut : `fontRenderer` 3 = hinting de la police. Le rendu « sans grille » (1)
  rend le texte FLOU (traits à cheval sur deux pixels) : l'utilisateur l'a rejeté. Migrations : `fontsVersion`
- `cmake/CopyRuntimeDeps.cmake` — copie des DLL à côté des exécutables

## Compiler et tester
Mettre `C:\msys64\ucrt64\bin` dans le PATH, puis :
```
cmake --build --preset debug      # (cmake --preset debug la première fois)
ctest --preset debug              # ou build/debug/desk_tests.exe
```
`debug.bat build` fait les deux. Toujours compiler + tester après une modification.

**macOS** : presets `mac-debug` / `mac-release`, script `./mac.sh [release] [build]` (Homebrew).
Code spécifique isolé : `Process.cpp` (Win32) / `Process_posix.cpp`, `#ifdef _WIN32` dans
`util/Paths.cpp`, `util/Strings.cpp`, `CommandLine.cpp` et les listes de polices de `App.cpp`.
Le workflow `.github/workflows/macos.yml` compile et teste sur macOS à chaque push.

## ⚠ Auto-développement : ne pas casser l'outil en cours d'utilisation
- **Ne jamais compiler la Release** (`release.bat`, `cmake --build --preset release`) pendant une
  session : l'exécutable est verrouillé et c'est lui qui fait tourner la conversation.
  Travailler en preset `debug` ; l'utilisateur recompile la Release lui-même après avoir fermé l'appli.
- Ne jamais tuer le processus `ClaudeDesk.exe` en cours.
- Ne jamais lancer une instance de test sur les vraies données (`%APPDATA%\ClaudeDesk`,
  `Documents\ClaudeDesk`).

## Vérification visuelle (instance isolée)
```
build\debug\ClaudeDesk.exe --data-dir <tmp>\data --workspace <tmp>\ws ^
    [--cwd <dossier>] [--panel history|light|dashboard|detached|guide|welcome|example|tour<N>|config[:<onglet>]] ^
    [--size 900x600] [--send "message"] --nowait --screenshot <tmp>\capture.bmp --after 3
```
`--size` réduit la fenêtre pour vérifier les panneaux étroits (helpers `ui::sameLineIfFits`, `ui::fitItemWidth`,
`ui::sideNote` dans `Theme.h` : les boutons passent à la ligne au lieu de sortir du panneau).
Projets : `--panel projets:legende|projets:fichiers|projets:sessions` (légende des icônes, explorateur du dossier de travail déplié, agents à plusieurs sessions dépliés).
Polices : `--panel police:systeme` (bascule à chaud vers les polices du système). Modèles : `--panel config:0`.
Connexion : `--panel login|login:attente|login:ok|login:conv` (fenêtre ou carte de conversation, sans rien lancer).
Designer : `--panel designer[:questions|:demo|:doc|:type-<id>|:creer|:creer!]` (`doc` = conception d'audit ; `type-data`… =
étape 1, type présélectionné ; `creer!` crée le projet d'exemple dans `<ws>/../depot-test`),
`--design "<description>"` lance une vraie conception (mettre `{"model":"haiku","request":{"kind":"data"}}` dans
`<tmp>\data\designer.json`) — SANS `--nowait`, sinon la capture n'attend pas la fin de la conception.
Orchestrateur : `--panel orchestrator[:budget|:modeles|:execution|:discussion]` ouvre la page ; `--orchestrate "<objectif>"` la lance sur le premier
projet à thèmes de l'espace de test (la capture attend la fin de l'orchestration, 2 min max après `--after`).
La capture ne contient que la fenêtre ; la lire ensuite pour vérifier. Pour tester un vrai
échange à moindre coût : `{"defaultConfig":{"model":"haiku"}}` dans `<tmp>\data\settings.json`.

## Protocole CLI (vérifié)
`claude -p --input-format stream-json --output-format stream-json --verbose
--include-partial-messages --permission-prompt-tool stdio`. Contrôles disponibles : `initialize`,
`set_model`, `set_permission_mode`, `interrupt` ; permissions via `control_request can_use_tool`.
**`set_effort` n'existe pas** → changer l'effort = relancer avec `--resume`.
`--append-system-prompt-file` fonctionne.

## Style de l'interface (épuré, à la VS Code)
- Peu de texte à l'écran, le détail dans les infobulles ou replié (`TreeNode` « Détails »). Pas de compteur à 0.
- Survols neutres ; l'orange (`pal.accent`) est réservé à la sélection et à l'action principale.
- Composants communs (`ui/Theme.h`) : `usageBar` (barre fine, valeur À CÔTÉ — jamais par-dessus le remplissage),
  `ringGauge`, `miniRing`, `meterRow`, `levelColor`, `chip` (bouton plat, cadre au survol : actions secondaires),
  `segmented` (choix exclusifs en boutons, à la place des boutons radio), `checkboxWrapped` (libellé qui passe à la
  ligne au lieu d'être coupé), `shortPath`, `helpMarker` (petit « ? » cerclé) ; cartes de choix
  `SelectCard`/`drawSelectCard` (ConfigPanel.cpp, modèles et modes). Densité « compacte » : `Settings::compactUi`.
- Rien ne doit déborder d'un panneau étroit : mesurer la place (`ui::sameLineIfFits`, `fitItemWidth`), raccourcir
  (`truncateUtf8` ajoute déjà « … »), ou passer à la ligne. Hauteurs variables mesurées (ex. `oc.quickBarHeight`).
- Configuration : sections (`configTab_`) 0 Modèle · 1 Mode · 2 Outils · 3 Instructions · 4 Avancé · 5 Général ·
  6 Performances · 7 Apparence ; liste à gauche si le panneau est large, sélecteur déroulant sinon. Les préférences
  s'enregistrent seules (`App::autosaveSettings`, comparaison de `Settings::dump()` toutes les 3 s).
- Panneaux : un seul × (sur l'onglet) — drapeaux `NoWindowMenuButton | NoCloseButton` sur le dockspace.

## Pièges connus
- ImGui : une fenêtre `NoBringToFrontOnFocus` passe derrière l'hôte du dockspace.
- Écrire les scripts (Python, etc.) avec l'outil Write, pas en heredoc bash : les `"\n"` cassent
  les chaînes C++ générées. Dans un script Python qui génère du C++, doubler les antislashs (`'\\\\'`) ou utiliser
  une chaîne brute `r'''…'''`.
- `Process_posix.cpp` et les branches `#else` de `_WIN32` ne sont PAS compilés sous Windows : une erreur n'y apparaît
  qu'au build macOS de la CI. Les relire avec soin après modification.

## Git
Dépôt public `FogCfan/ClaudeDesk`, branche `main`. `gh.exe` : `"/c/Program Files/GitHub CLI/gh.exe"`
(absent du PATH de bash).
- **Aucun commit, push ou PR sans demande explicite** de l'utilisateur.
- Quand il le demande : il est **seul auteur** — pas de ligne `Co-Authored-By`, pas de mention
  « Generated with Claude Code ». Sinon, lui donner les commandes git à enchaîner.
