# ClaudeDesk

Interface de bureau C++ (SDL3 + Dear ImGui) pour **Claude Code**. Elle pilote le
vrai CLI `claude` en mode `-p --input-format stream-json --output-format stream-json` :
vos réglages, CLAUDE.md, MCP, skills, hooks et votre abonnement s'appliquent
exactement comme dans le terminal.

En plus d'un vrai éditeur et du suivi en direct de l'usage, ClaudeDesk range le travail en
**projets › thèmes › conversations** (panneau **Projets**) et peut faire travailler une **équipe d'agents**
sur un objectif grâce à l'**orchestrateur** (page 3), dans un budget que vous fixez.

Windows (MSYS2 UCRT64) et macOS (Homebrew) ; ClaudeDesk est lui-même développé depuis ClaudeDesk.

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
Réglages : `~/Library/Application Support/ClaudeDesk` ; projets : `~/Documents/ClaudeDesk`.

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
- Repère discret « ── hh:mm ── » dans le fil à chaque changement de minute.
- La barre des tâches clignote quand Claude a fini ou attend une décision.

### Quatre pages : Cockpit, Tableau de bord, Orchestrateur, Designer
- **Cockpit** (Ctrl+1) : la page principale décrite ci-dessus.
- **Tableau de bord des conversations** (Ctrl+2) : toutes les conversations ouvertes en cartes, triées
  par urgence (⚠ attend votre décision, en cours, prête, en veille). Chaque carte montre l'état et la durée,
  la progression de la liste de tâches de Claude, les dernières actions, la dernière réponse (en direct),
  les boutons **Autoriser / Toujours / Refuser** et une **réponse rapide**. À droite, un **fil d'activité**
  commun à toutes les conversations. Filtres « actives » / « à traiter », « Tout interrompre ».
- **Fenêtre séparée** (Ctrl+Maj+D ou ⧉) : le tableau de bord dans sa propre fenêtre Windows (second
  contexte ImGui), à poser sur un deuxième écran. Son état est mémorisé au redémarrage.
- **Orchestrateur** (Ctrl+3) : le chef de projet automatique et son équipe (voir plus bas). Quand une
  orchestration existe, le tableau de bord l'affiche en tête (carte « Orchestration » + équipe).
- **Designer** (Ctrl+4) : conçoit un projet et son équipe d'agents à partir d'une description (voir plus bas).

### Usage en direct (équivalent de `/usage`)
- Limites du forfait : fenêtre de 5 h et semaine, % utilisé, heure de réinitialisation,
  historique sur 24 h (heures locales, session en cours surlignée, détail au survol). Mis à jour à chaque
  réponse (événements `rate_limit_event` du CLI) ; bouton **Actualiser** = commande locale `/usage` du CLI
  (`claude -p "/usage"` → `usage_report.rate_limits`) : aucun appel au modèle, donc gratuite, et elle répond
  même quand la limite est atteinte. Une date de réinitialisation dépassée s'affiche « réinitialisée » (0 %).
- **Actualisation automatique** (Configuration › Application) : au lancement si la mesure date de plus de 1 min,
  puis si elle date de plus de 5 min (fenêtre regardée, aucune session au travail), et dès qu'une limite s'est
  réinitialisée — au plus une fois par minute.
- Conversation active : coût équivalent API, jetons (dont cache), tours, temps API,
  **remplissage de la fenêtre de contexte**, coût par tour, tâches de Claude, serveurs MCP.

### Configuration (menu Configuration ou Ctrl+,)
- **Modèle** (liste fournie par le CLI), modèle de secours, **effort** (low → max), style de sortie.
- **Mode** de permission (Normal, Accepter les modifs, Auto, Plan, Ne pas demander, Sans permissions).
- **Outils** : disponibles / toujours autorisés / interdits, règles fines (`Bash(git *)`…).
- **Prompt système** : ajouter ou remplacer, modèles réutilisables, aperçu de la mémoire du projet injectée.
- **Avancé** : dossiers supplémentaires (`--add-dir`), budget max, options libres.
- **Application** : chemin de Claude Code, dossier des projets, thème, taille du texte…

Le modèle et le mode changent **à chaud**. Les autres réglages relancent la session en
arrière-plan avec `--resume` : l'historique est conservé.

### Graphismes et fluidité (Configuration › Graphismes)
- **Mode d'énergie** (aussi dans *Affichage › Mode d'énergie*), en un clic :
  - **Automatique** (défaut) : Performance sur secteur, Économie dès que le portable passe sur batterie
    (les deux réglables) ; une notification signale chaque bascule.
  - **Performance** : rendu continu à la fréquence de l'écran, toutes les animations.
  - **Équilibré** : ne redessine que s'il se passe quelque chose, 30 i/s en arrière-plan.
  - **Économie de batterie** : 30 i/s maximum, 10 i/s en arrière-plan ou fenêtre réduite, sans animations ni
    halos, rendu au repos (≈ 6 i/s quand rien ne bouge).
  Modifier un réglage de détail fait passer en **Personnalisé**. La barre d'état rappelle le mode économe.
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

### Pour tous les projets, pas seulement le code
ClaudeDesk sert aussi bien à un développeur qu'à quelqu'un qui organise des idées, analyse des données ou rédige un
audit. L'accueil propose deux entrées : **« Une question ? »** (Claude répond aussitôt, dans une conversation) et
**« Démarrer un projet »**, par type :

| Type | Livrable | Méthode de Claude |
|---|---|---|
| Idées et concept | documents structurés : options, décisions, plan | recherche, rédaction, relecture critique |
| Analyse de données | chiffres vérifiés, graphiques, rapport | scripts Python reproductibles, données d'origine jamais modifiées |
| Rapport, audit ou étude | document sourcé (Word/PDF) | plan validé d'abord, sources pour chaque fait, relecture croisée |
| Logiciel ou application | programme testé | build, tests, captures |
| Projet hybride | documents + réalisation | conception d'abord, puis réalisation |
| Laisser Claude choisir | selon le projet | Claude détermine le type |

Le type règle le **Designer** (équipe adaptée), l'**orchestrateur** (découpage, vérifications, livrables), les consignes
des agents, la mémoire injectée dans les conversations du projet et le vocabulaire de l'interface (« dossier du
projet » plutôt que « dépôt »…). Il se change dans l'Inspecteur du projet. Les actions de Claude s'affichent en clair
(« Lecture », « Modification », « Commande », « Recherche web »…), et les autorisations aussi (« Claude demande
l'autorisation de modifier un fichier »).

### Projets : projets › thèmes › conversations
Le panneau **Projets** montre une **vraie arborescence de dossiers** (par défaut `Documents\ClaudeDesk`) :

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

Chaque élément a son **icône**, comme dans l'explorateur de VS Code (légende : bouton **?** du panneau) : projet
(dossier orange), thème (dossier jaune), **agent** (robot : thème d'un projet orchestré), **orchestrateur**,
conversation (bulle ; un point orange quand Claude travaille), fichiers (couleur selon le type : code, données,
image, document).
- **Dossier de travail** : sous chaque projet, l'explorateur du code (dossiers lourds comme `.git` ou
  `node_modules` masqués) ; double-clic pour ouvrir un fichier, clic droit pour le **joindre à la conversation**,
  aperçu des fichiers texte dans l'inspecteur.
- **Fichiers de référence** : clic droit sur un dossier › *Ajouter des fichiers…*, ou glisser-déposer depuis
  l'Explorateur sur un projet ou un thème. Ils sont copiés dans le dossier, et leur chemin est donné à Claude dans
  les nouvelles conversations du dossier (« Documents de référence »).

- Chaque conversation enregistre son transcript, ses stats, son **récap** et l'identifiant
  de session Claude Code. **Reprendre** = votre prochain message continue la session (`--resume`).
- **Récap** (bouton, Ctrl+Maj+R, ou automatique à la fermeture) : objectif, ce qui a été fait,
  décisions, fichiers, reste à faire — généré par Haiku.
- **Synthèse de dossier** : condense les récaps d'un thème ; les récaps manquants ou périmés sont
  d'abord générés (3 en parallèle), l'inspecteur affiche « récaps x / n ».
- **Contexte injecté** : une nouvelle conversation reçoit dans son prompt système les notes et
  synthèses du projet et des thèmes parents, plus les récaps des échanges précédents du dossier.
  « ↻ Repartir du récap » (en-tête de conversation ou Inspecteur) génère le récap s'il manque, ouvre une
  conversation neuve, légère mais informée, avec les mêmes réglages, et range l'ancienne en « Terminée ».
  Sur la discussion de l'orchestrateur, le cerveau suit la nouvelle conversation (rappel de l'objectif inclus).
- Glisser-déposer pour ranger une conversation, statuts (Idée, En cours, Bloqué, Terminé),
  épingles, recherche dans les titres et récaps.
- **Importer depuis Claude Code** : les sessions faites dans le terminal
  (`~/.claude/projects`) s'importent dans un dossier et se reprennent telles quelles.

### Orchestrateur : une équipe d'agents sur un objectif
Une conversation **« Orchestrateur »** (le cerveau), à la racine d'un projet, pilote les conversations
de ses thèmes : **chaque thème est un agent**, les notes du dossier décrivent son rôle. Ouverture : Ctrl+3,
le bouton de page *Orchestrateur*, ou **Orchestrer…** dans l'inspecteur d'un projet.

```
 cerveau ──@@DELEGUER agent | modele=… effort=…──▶ agents (en parallèle, limité)
    ▲                                                  │
    └───── comptes rendus (au fil de l'eau) ◀──────────┘   … jusqu'à @@TERMINE
```

- **Directives** en fin de réponse du cerveau : `@@DELEGUER <agent>` (une tâche, avec modèle et effort),
  `@@HUMAIN` (une question pour vous), `@@ATTENDRE` (attendre les agents encore au travail), `@@TERMINE` (bilan).
- **Au fil de l'eau** : le cerveau n'attend pas toute l'équipe. Dès qu'un agent termine alors que d'autres
  travaillent encore (et qu'une place est libre), il reçoit un **compte rendu intermédiaire** et peut relancer
  aussitôt les agents libres ; une tâche confiée à un agent occupé part après la sienne. L'équipe continue
  aussi pendant qu'il réfléchit ou qu'une question vous attend (la réponse lui transmet ce qui a avancé).
  Désactivable : *Réglages avancés › Exécution › Comptes rendus au fil de l'eau*.
- **Équipe évolutive** : quand un besoin durable n'est couvert par aucun agent, le cerveau peut proposer un
  nouvel agent (`@@NOUVEL_AGENT <nom>`, avec « Pourquoi : … » et sa fiche de rôle). **Rien n'est créé sans votre
  accord** : la proposition vous est soumise comme une question (*Créer* / *Ne pas créer* / *Autre* : vos
  précisions, qu'il intègre avant de la soumettre de nouveau). Une fois accepté, l'agent devient un nouveau thème
  du projet, avec sa fiche dans les notes (et vos précisions éventuelles). Pendant que vous décidez, les autres
  agents continuent. Réglages : *Exécution › L'orchestrateur peut proposer de nouveaux agents*, plafond 12 par défaut.
- **Budget** — la contrainte principale, toujours un **plafond** (le cerveau optimise : Haiku pour la
  mécanique, Sonnet pour le courant, Opus pour le difficile). Quatre niveaux simples :

  | Niveau | Abonnement (part de ce qui reste dans la session de 5 h) | API | Modèles permis | Agents |
  |---|---|---|---|---|
  | Bas | 25 % | 5 $ | Haiku, Sonnet | 2 |
  | Moyen (défaut) | 50 % | 20 $ | + Opus | 3 |
  | Élevé | 100 % | 60 $ | + Fable | 3 |
  | Max abonnement | sans limite (sessions enchaînées) | 200 $ | tous | 4 |

  La part se renouvelle à chaque session de 5 h ; seule la consommation faite pendant que l'équipe travaille
  est comptée. Option **résultat dans le budget** : phase finale à 75 % (« réduis le périmètre plutôt que la
  qualité »), tolérance de +15 % pour finir proprement, puis conclusion obligatoire. Plafonds du compte (5 h et
  hebdomadaire) : pause puis reprise automatique à la réinitialisation. « ↺ Relancer le budget » remet le
  compteur à zéro. *Réglages avancés* : budget détaillé, modèles et efforts autorisés, défauts des agents et
  du cerveau, parallélisme, cycles maximum, modes de permission.
- **Questions** : un bloc `@@HUMAIN` devient un formulaire (questions numérotées séparées, options
  cliquables `- [ ]`, la recommandée présélectionnée, réponses suggérées, « Laisse-le décider », Ctrl+Entrée).
  La barre des tâches clignote.
- **Contexte et cache maîtrisés** (chaque appel d'outil relit tout le contexte ; c'est là que part le quota) :
  avant d'envoyer un message, une conversation est récapitulée puis remplacée par une conversation neuve du
  même thème (qui reçoit la mémoire du projet) si son contexte dépasse **120 k jetons** (seuil en jetons, pas en
  % d'une fenêtre de 1 M), après 6 tâches, si son **cache a expiré** (≈ 1 h sous abonnement : reprendre
  froid réécrirait tout le contexte au prix fort), ou si l'on s'apprête à **changer son modèle ou son effort**
  (le cache est propre au modèle). Idem pour le cerveau, dont le message attend la fin du récap.
  **En fin de phase** (`@@TERMINE`, pause de l'orchestrateur ou du budget jusqu'à la session suivante), les
  conversations de l'équipe sont récapitulées sans être fermées : la mémoire du projet suit le travail fait, et à
  la reprise (cache expiré), une rotation repart aussitôt du récap déjà prêt. Pas lors d'une pause sur le plafond
  du compte. Réglage : *Exécution › Récaps de l'équipe en fin de phase*.
- **Économie** : conversations **allégées** (`--disable-slash-commands`, sans sous-agents ni serveurs MCP :
  contexte fixe d'un agent de 29,7 k à 17,6 k jetons sur E7 Twin, appliqué aux conversations neuves) ;
  le cerveau est en **lecture seule** (Read, Glob, Grep) et délègue tests et builds ; les
  agents reçoivent des consignes d'économie et rendent un compte rendu de 15 lignes, transmis avec ses
  mesures (appels, contexte final, jetons relus/écrits) et tronqué par le début au-delà de 3 000 caractères ;
  **garde-fous** par tâche (150 appels, 250 k de contexte) : l'agent est interrompu et le cerveau redécoupe.
- **Vérification visuelle** (*Exécution*, activée par défaut) : tout changement visible est livré avec une capture
  PNG dans `_captures/` à la racine du dépôt (dossier auto-ignoré par git), que l'agent examine ; le cerveau, même en
  lecture seule, peut l'ouvrir avec Read (~1,5 k jetons par image). **Agents équipés du navigateur** : cochés dans
  *Exécution*, ils reçoivent `--chrome` (outils Claude in Chrome : ouvrir une page, cliquer, console, capture),
  compatible avec les conversations allégées ; marqués « 🌐 navigateur » pour le cerveau.
- **La page** : cartes Avancement et Budget (jauge, repère 75 %, cible, tolérance), l'équipe (statut, modèle,
  contexte de chaque agent), réglages, journal, et la discussion du cerveau à droite, rendue en cartes
  (tâches confiées, questions, bilan) — vous pouvez lui écrire directement. Onglet « Bilan ✓ » à la fin.
- **Persistance** : `<projet>/_orchestrateur.json`, rechargé au démarrage ; une orchestration interrompue par
  la fermeture reste en pause jusqu'à *Reprendre* (les tâches interrompues sont renvoyées).

### Designer : de l'idée à l'équipe d'agents
Page 4 (Ctrl+4, ou *Fichier › Concevoir un projet avec le Designer…*), en quatre étapes :
1. **Décrire** : ce qu'on veut construire ; facultatif : un dossier (existant → Claude lit le code, en lecture
   seule, pour bâtir l'équipe sur l'existant), technologies imposées, contraintes, taille d'équipe, niveau de budget.
2. **Clarifier** (seulement si besoin) : jusqu'à 4 questions de Claude, réponses à cliquer, la recommandée
   présélectionnée ; « Laisser Claude trancher ».
3. **Ajuster** : projet (nom, résumé, pile, conventions), orchestrateur (objectif, jalons ordonnés, budget,
   vérification visuelle) et une carte par agent (nom, mission, modèle/effort conseillés, navigateur, fiche de
   rôle). Tout s'édite à la main ; **Demander un changement** fait affiner par Claude (il garde vos
   modifications) ; **Version précédente** annule une génération. Points bloquants en rouge, conseils en orange.
4. **Créer** : projet + un thème par agent (fiche en notes, mission + modèle conseillé en tête pour le
   résumé vu par l'orchestrateur), fichiers de départ dans le dépôt (`CLAUDE.md`, `docs/PLAN.md`,
   `docs/agents/*.md`, jamais d'écrasement), `_orchestrateur.json` réglé (niveau de budget, objectif + jalons,
   visuel, agents navigateur). Ouvre ensuite la page Orchestrateur ; démarrage immédiat en option.

Le designer est un appel `claude -p` (stream-json, fil d'activité en direct, annulable) en **mode Plan par
défaut** ; modèle (Opus par défaut), effort et mode réglables. Réponse attendue : un bloc JSON (`questions` ou
`design`) ; en mode plan, un plan rendu par ExitPlanMode est aussi accepté. Brouillon persistant :
`designer.json` dans le dossier de données. Moteur testé : `src/claude/Designer` (`tests/test_designer.cpp`).
Test réel : `--design "<description>"` (réglages lus dans le `designer.json` du dossier de données de test) ;
captures : `--panel designer[:questions|:demo|:creer]`, `designer:creer!` crée le projet d'exemple dans
`<espace de test>/../depot-test`.

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
    résolvent, quand elles servent et quand elles sont inutiles, avec exemples et pièges — les projets
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
| Ctrl+1 / Ctrl+2 / Ctrl+3 / Ctrl+4 | Cockpit / tableau de bord / orchestrateur / designer |
| Ctrl+Maj+D | Tableau de bord dans une fenêtre séparée |
| Maj+F1 | Guide de démarrage |
| Ctrl+S | Tout enregistrer |
| Ctrl++ / Ctrl+- / Ctrl+molette | Taille du texte |

## Architecture

```
src/
  util/        Strings (UTF-8, base64, dates), Paths (fichiers, %APPDATA%)
  claude/      Process        processus Win32 (Process_posix : macOS) + threads de lecture
               CommandLine    échappement des arguments, détection de claude.exe
               SessionConfig  réglages → options du CLI
               ClaudeSession  protocole stream-json : messages, streaming, outils,
                              permissions (control_request can_use_tool), set_model,
                              set_permission_mode, interrupt, --resume
               Transcript     entrées affichées / sauvegardées, export Markdown
               UsageTracker   limites du forfait, stats par session
               OneShot        appels ponctuels (récaps, actualisation de l'usage)
               History        lecture des sessions de ~/.claude/projects
               Orchestrator   moteur de l'orchestrateur, sans UI : directives, file de
                              tâches, comptes rendus au fil de l'eau, budget, rotation
               Designer       conception projet + équipe : prompts, analyse JSON,
                              validation, fichiers de départ, réglages de l'orchestrateur
  workspace/   Workspace      arborescence des projets sur disque + contexte injecté
  app/         App (boucle SDL, actions), AppMenus, Settings
  ui/          ConversationView, PromptEditor, PermissionPanel, UsagePanel,
               ConfigPanel, WorkspacePanel, Dashboard (page 2 + fil d'activité),
               OrchestratorPanel (page 3 + AppOrchHost, l'adaptateur moteur ↔ conversations),
               Tutorial (bienvenue, visite, guide, projet exemple), Markdown, Theme, Anim
tests/         doctest : protocole rejoué, arborescence, échappement, utilitaires, orchestrateur
```

Données : `%APPDATA%\ClaudeDesk\` (settings.json, usage.json, layout.ini, prompts\).

Options de lancement (tests, instance isolée) : `--data-dir`, `--workspace`, `--cwd`, `--send "texte"`,
`--panel history|shortcuts|newproject|light|dashboard|detached|guide<N>|welcome|example|gfx|reminder|tour<N>|synthesis`,
`--panel orchestrator[:reglages|:budget|:modeles|:execution|:discussion]`, `--orchestrate "objectif"` (lance
l'orchestrateur sur le premier projet à thèmes), `--screenshot capture.bmp --after 3 [--nowait]` (la fenêtre
détachée est capturée dans `capture_dash.bmp`).

Tests unitaires : `cmake --build --preset debug` puis `build/debug/desk_tests.exe` (ou `ctest --preset debug`).
