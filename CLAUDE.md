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
- `src/app` — `App` (boucle, onglets, fenêtres), `AppMenus`, `Settings` (settings.json)
- `src/claude` — `ClaudeSession` (protocole stream-json), `Process`, `CommandLine`,
  `SessionConfig`, `History`, `Transcript`, `UsageTracker` (rate_limit_event + rapport de `/usage` : `claude -p "/usage"` est une commande locale, gratuite,
  qui répond même limite atteinte → `usage_report.rate_limits`), `OneShot` (récaps),
  `Designer` (conception projet + équipe d'agents à partir d'une description : prompts, JSON, validation, fichiers de départ),
  `Orchestrator` (moteur de l'orchestrateur, sans UI : directives `@@DELEGUER/@@HUMAIN/@@ATTENDRE/@@TERMINE/@@NOUVEL_AGENT`, comptes rendus au fil de l'eau, garde-fous ;
  un nouvel agent n'est JAMAIS créé sans l'accord de l'humain : `pendingAgents` → question → `OrchestratorHost::createAgent`)
- `src/ui` — panneaux ImGui : `ConversationView`, `PromptEditor`, `ConfigPanel`,
  `WorkspacePanel`, `Dashboard`, `UsagePanel`, `PermissionPanel`, `Markdown`, `Theme`, `Anim`, `Tutorial`,
  `OrchestratorPanel` (page 2 « Orchestrateur », Ctrl+3, + `AppOrchHost`, l'adaptateur moteur ↔ conversations ; état dans
  `<projet>/_orchestrateur.json`). La discussion du cerveau est exclue des onglets (`isOrchBrain`) et rendue en cartes
  (`drawBrainEntry`) ; le tableau de bord l'affiche en tête avec son équipe (`drawOrchestrationHero`).
  `DesignerPanel` (page 3 « Designer », Ctrl+4 : Décrire → Clarifier → Ajuster → Créer ; brouillon `designer.json`).
- `src/workspace` — `Workspace` : vrais dossiers + `_dossier.json` + `*.conv.json`, corbeille, `buildContext`
- `src/util` — chemins, chaînes, fichiers
- `tests/` — doctest (`desk_tests`) : protocole, workspace, chaînes
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
Designer : `--panel designer[:questions|:demo|:creer|:creer!]` (`creer!` crée le projet d'exemple dans `<ws>/../depot-test`),
`--design "<description>"` lance une vraie conception (mettre `{"model":"haiku"}` dans `<tmp>\data\designer.json`).
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

## Pièges connus
- ImGui : une fenêtre `NoBringToFrontOnFocus` passe derrière l'hôte du dockspace.
- Écrire les scripts (Python, etc.) avec l'outil Write, pas en heredoc bash : les `"\n"` cassent
  les chaînes C++ générées.

## Git
Dépôt public `FogCfan/ClaudeDesk`, branche `main`. `gh.exe` : `"/c/Program Files/GitHub CLI/gh.exe"`
(absent du PATH de bash).
- **Aucun commit, push ou PR sans demande explicite** de l'utilisateur.
- Quand il le demande : il est **seul auteur** — pas de ligne `Co-Authored-By`, pas de mention
  « Generated with Claude Code ». Sinon, lui donner les commandes git à enchaîner.
