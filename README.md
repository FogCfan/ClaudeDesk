# ClaudeDesk

Interface de bureau C++ (SDL3 + Dear ImGui) pour **Claude Code**. Elle pilote le
vrai CLI `claude` en mode `-p --input-format stream-json --output-format stream-json` :
vos réglages, CLAUDE.md, MCP, skills, hooks et votre abonnement s'appliquent
exactement comme dans le terminal.

## Lancer

```
release.bat          compile (Release, sans console) puis lance
release.bat build    compile seulement → build\release\ClaudeDesk.exe
debug.bat            compile en Debug, lance les tests, puis l'application (avec console)
```

Prérequis : MSYS2 UCRT64 avec `gcc cmake ninja sdl3 nlohmann-json freetype`
(`pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,sdl3,nlohmann-json,freetype}`) ; toutes les DLL
nécessaires sont copiées à côté de l'exécutable à la compilation, et Claude Code
installé (`claude.exe` est détecté automatiquement, sinon : Configuration › Application).

### macOS

```
brew install cmake ninja sdl3 nlohmann-json freetype
./mac.sh                  compile en Debug, lance les tests, puis l'application
./mac.sh build            compile + tests seulement
./mac.sh release          compile en Release puis lance → build/mac-release/ClaudeDesk
```

`claude` est cherché dans le PATH du shell de connexion (récupéré au démarrage, même lancé
depuis le Finder), puis dans `~/.local/bin`, `/opt/homebrew/bin` et `/usr/local/bin`.
Réglages : `~/Library/Application Support/ClaudeDesk` ; espace de travail : `~/Documents/ClaudeDesk`.

## Fonctionnalités

### Éditeur
- Texte multi-ligne avec retour à la ligne, souris (clic, glisser, double-clic = mot),
  et les raccourcis habituels : Ctrl+C/X/V, Ctrl+Z/Y, Ctrl+A, Ctrl+←/→, Maj+flèches,
  Ctrl+Retour arrière, Début/Fin…
- **Entrée** envoie, **Maj+Entrée** fait une nouvelle ligne (inversable dans les préférences).
- **Ctrl+↑ / Ctrl+↓** : rappel des messages précédents.
- **/** : autocomplétion des commandes (/compact, /context, skills, commandes des plugins…).
- Glisser-déposer : une image est jointe au message, un autre fichier est référencé (`@"chemin"`).
- Messages envoyés pendant que Claude travaille : mis en file.

### Conversation
- Réponses diffusées en direct, rendu Markdown (titres, listes, tableaux, `code`, liens,
  blocs de code avec bouton **Copier**).
- Appels d'outils repliables avec état (en cours / ok / erreur), **diff coloré** pour Edit,
  aperçu pour Write, commande pour Bash, liste de tâches pour TodoWrite, activité des sous-agents.
- Bouton **Sélectionner** sur chaque message (texte sélectionnable à la souris), clic droit :
  copier, réutiliser, copier toute la conversation en Markdown.
- Recherche dans la conversation (Ctrl+F), export Markdown.
- **Permissions** : panneau Autoriser / Toujours autoriser / Refuser (+ consigne), Alt+A/T/R.
  Les questions de Claude (AskUserQuestion) et la validation de plan (mode Plan) ont leur propre UI.
- Plusieurs conversations en parallèle (onglets), rouvertes au redémarrage.
- La barre des tâches clignote quand Claude a fini ou attend une décision.

### Deux pages : Cockpit et Tableau de bord
- **Cockpit** (Ctrl+1) : la page principale décrite ci-dessus.
- **Tableau de bord des conversations** (Ctrl+2) : toutes les conversations ouvertes en cartes, triées
  par urgence (⚠ attend votre décision, en cours, prête, en veille). Chaque carte montre l'état et la durée,
  la progression de la liste de tâches de Claude, les dernières actions, la dernière réponse (en direct),
  les boutons **Autoriser / Toujours / Refuser** et une **réponse rapide**. À droite, un **fil d'activité**
  commun à toutes les conversations. Filtres « actives » / « à traiter », « Tout interrompre ».
- **Fenêtre séparée** (Ctrl+Maj+D ou ⧉) : le tableau de bord dans sa propre fenêtre Windows (second
  contexte ImGui), à poser sur un deuxième écran. Son état est mémorisé au redémarrage.

### Usage en direct (équivalent de `/usage`)
- Limites du forfait : fenêtre de 5 h et semaine, % utilisé, heure de réinitialisation,
  historique sur 24 h. Mis à jour à chaque réponse (événements `rate_limit_event` du CLI) ;
  bouton **Actualiser** (requête minuscule à Haiku).
- Conversation active : coût équivalent API, jetons (dont cache), tours, temps API,
  **remplissage de la fenêtre de contexte**, coût par tour, tâches de Claude, serveurs MCP.

### Configuration (menu Configuration ou Ctrl+,)
- **Modèle** (liste fournie par le CLI), modèle de secours, **effort** (low → max), style de sortie.
- **Mode** de permission (Normal, Accepter les modifs, Auto, Plan, Ne pas demander, Sans permissions).
- **Outils** : disponibles / toujours autorisés / interdits, règles fines (`Bash(git *)`…).
- **Prompt système** : ajouter ou remplacer, modèles réutilisables, aperçu du contexte cowork.
- **Avancé** : dossiers supplémentaires (`--add-dir`), budget max, options libres.
- **Application** : chemin de Claude Code, espace de travail, thème, taille du texte…

Le modèle et le mode changent **à chaud**. Les autres réglages relancent la session en
arrière-plan avec `--resume` : l'historique est conservé.

### Graphismes et fluidité (Configuration › Graphismes)
- **Cadence** : synchronisée sur l'écran (VSync, défaut), limite personnalisée (30 → 480 i/s) ou illimitée.
  L'interface est redessinée en continu : les animations restent fluides même au repos (mode économie
  d'énergie optionnel, et 30 i/s en arrière-plan si souhaité). Une image coûte ~0,5 ms (≈ 2 000 i/s en illimité).
- **Animations** : défilement fluide (molette, Page préc./suiv., Début/Fin), apparition des messages en fondu,
  fondu entre les pages, barres d'usage qui se remplissent en douceur, indicateur d'activité en arc,
  curseur de rédaction pulsé, notifications qui glissent, halos lumineux sur les cartes actives et pendant la
  visite guidée. Vitesse réglable, tout désactivable.
- **Qualité** : texte rendu par **FreeType** (net, emoji en couleur, vraie police italique), courbes et coins
  arrondis haute définition.
- **Processeur graphique** : moteur de rendu au choix (Direct3D 12 / 11, Vulkan, OpenGL, SDL GPU, logiciel),
  relance automatique ; la carte graphique dédiée est demandée sur les portables hybrides. Compteur
  d'images/seconde optionnel dans la barre d'état.

### Mode cowork : projets › thèmes › conversations
L'espace de travail est une **vraie arborescence de dossiers** (par défaut `Documents\ClaudeDesk`) :

```
ClaudeDesk\
  Platformer2D\                  ← projet (dossier de travail : C:\Dev\Platformer2D)
    _dossier.json                ← notes + synthèse du projet
    Architecture du moteur [1a2b3c4d].conv.json   ← échange sur le projet en général
    Code\                        ← thème
      _dossier.json
      Physique\                  ← sous-thème
        Collisions pentes [5e6f7a8b].conv.json
    Infra\
    Docs\
  Divers\                        ← conversations rapides hors projet
  .corbeille\                    ← éléments supprimés
```

- Chaque conversation enregistre son transcript, ses stats, son **récap** et l'identifiant
  de session Claude Code. **Reprendre** = votre prochain message continue la session (`--resume`).
- **Récap** (bouton, Ctrl+Maj+R, ou automatique à la fermeture) : objectif, ce qui a été fait,
  décisions, fichiers, reste à faire — généré par Haiku.
- **Synthèse de dossier** : condense les récaps d'un thème.
- **Contexte injecté** : une nouvelle conversation reçoit dans son prompt système les notes et
  synthèses du projet et des thèmes parents, plus les récaps des échanges précédents du dossier.
  « Repartir de ce contexte » ouvre une conversation neuve, légère, mais informée.
- Glisser-déposer pour ranger une conversation, statuts (Idée, En cours, Bloqué, Terminé),
  épingles, recherche dans les titres et récaps.
- **Importer depuis Claude Code** : les sessions faites dans le terminal
  (`~/.claude/projects`) s'importent dans un dossier et se reprennent telles quelles.

### Prise en main
- **Premier lancement** : le guide s'ouvre sur sa page *Bienvenue*. **Lancements 2 à 11** : une bulle animée,
  accrochée au menu *Aide*, rappelle où trouver le guide ; elle disparaît au premier clic ailleurs ou sur
  « Non merci ». Ensuite plus rien — sauf *Configuration › Application › Accueil* (revoir l'accueil) ou
  *Réinitialiser les préférences* (conversations conservées).
- **Visite guidée** (Aide › Visite guidée) : 13 étapes qui surlignent chaque zone de l'interface,
  navigation au clavier (← →).
- **Guide de démarrage** (Maj+F1), en deux parties :
  - *Mode d'emploi* : premiers pas, écran, éditeur, suivi de Claude, autorisations, configuration, usage,
    reprise/import, raccourcis ;
  - *Comprendre et bien utiliser* : les fonctionnalités non triviales, expliquées par le problème qu'elles
    résolvent, quand elles servent et quand elles sont inutiles, avec exemples et pièges — le mode cowork
    (pourquoi), organiser un projet, récaps/synthèses/contexte injecté, reprendre ou repartir, travailler en
    parallèle, doser l'autonomie et la réflexion (mode Plan, modes, effort, modèles, prompts), une journée type,
    FAQ. Chaque section a des boutons pour passer à la pratique.
- **Projet exemple** (Aide › Projet exemple) : une mini calculatrice C++ volontairement boguée
  (`%APPDATA%\ClaudeDesk\exemple\mini-calculatrice`) rangée en thèmes (Code › Parseur, Tests, Idées),
  avec 4 conversations pré-enregistrées (outils, diff refusé, liste de tâches, récaps, statuts) et une
  synthèse de dossier. Écrire dans une de ces conversations la poursuit avec le vrai Claude, qui reçoit le
  fil et le contexte du dossier.

## Raccourcis
F1 dans l'application affiche la liste complète.

| Touche | Action |
|---|---|
| Ctrl+N / Ctrl+W | Nouvelle conversation / fermer l'onglet |
| Ctrl+PgSuiv / PgPréc | Onglet suivant / précédent |
| Échap | Interrompre Claude |
| Ctrl+F | Rechercher dans la conversation |
| Ctrl+Maj+C | Copier la dernière réponse |
| Ctrl+Maj+R | Générer le récap |
| Ctrl+1 / Ctrl+2 | Cockpit / tableau de bord |
| Ctrl+Maj+D | Tableau de bord dans une fenêtre séparée |
| Maj+F1 | Guide de démarrage |
| Ctrl+S | Tout enregistrer |
| Ctrl++ / Ctrl+- / Ctrl+molette | Taille du texte |

## Architecture

```
src/
  util/        Strings (UTF-8, base64, dates), Paths (fichiers, %APPDATA%)
  claude/      Process        processus Win32 + threads de lecture des tubes
               CommandLine    échappement des arguments, détection de claude.exe
               SessionConfig  réglages → options du CLI
               ClaudeSession  protocole stream-json : messages, streaming, outils,
                              permissions (control_request can_use_tool), set_model,
                              set_permission_mode, interrupt, --resume
               Transcript     entrées affichées / sauvegardées, export Markdown
               UsageTracker   limites du forfait, stats par session
               OneShot        appels ponctuels (récaps, actualisation de l'usage)
               History        lecture des sessions de ~/.claude/projects
  workspace/   Workspace      arborescence cowork sur disque + contexte injecté
  app/         App (boucle SDL, actions), AppMenus, Settings
  ui/          ConversationView, PromptEditor, PermissionPanel, UsagePanel,
               ConfigPanel, WorkspacePanel, Dashboard (page 2 + fil d'activité),
               Tutorial (bienvenue, visite, guide, projet exemple), Markdown, Theme
tests/         doctest : protocole rejoué, arborescence, échappement, utilitaires
```

Données : `%APPDATA%\ClaudeDesk\` (settings.json, usage.json, layout.ini, prompts\).

Options de lancement (tests) : `--data-dir`, `--workspace`, `--cwd`, `--send "texte"`,
`--panel history|shortcuts|newproject|light|dashboard|detached|guide<N>|welcome|example|gfx|reminder|tour<N>`,
`--screenshot capture.bmp --after 3 [--nowait]` (la fenêtre détachée est capturée dans `capture_dash.bmp`).
