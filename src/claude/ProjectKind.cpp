#include "claude/ProjectKind.h"

namespace claude {

namespace {

const std::vector<ProjectKindInfo>& table() {
    static const std::vector<ProjectKindInfo> t = {
        {ProjectKind::Auto, "auto", "Laisser Claude choisir",
         "Décrivez simplement ce que vous voulez : Claude détermine la nature du projet et l'organisation qui convient.",
         "Ex. : préparer le lancement de notre nouvelle offre (étude de marché, argumentaire, page web) ; comprendre "
         "pourquoi nos ventes baissent depuis mars à partir de nos exports Excel ; écrire un jeu de société et ses "
         "règles…",
         "Selon le projet : documents, analyses, logiciel, ou un mélange.",
         {"Idées", "Travail", "Livrables", "Divers"}, true, false},
        {ProjectKind::Software, "software", "Logiciel ou application",
         "Une application, un site, un script, un outil : du code qui fonctionne, testé.",
         "Ex. : une application web de recettes avec planning de la semaine et liste de courses ; un jeu de Pong en "
         "C++ avec SDL3 ; un outil en ligne de commande qui renomme des photos d'après leur date… Précisez le public, "
         "la plateforme et les fonctionnalités clés.",
         "Un programme qui fonctionne, ses tests, et la documentation pour le lancer.",
         {"Code", "Infra", "Docs", "Idées"}, true, true},
        {ProjectKind::Concept, "concept", "Idées et concept",
         "Organiser des idées, construire une stratégie, concevoir un produit, un service ou un univers.",
         "Ex. : concevoir l'offre d'une association (publics, services, financement) ; structurer les idées d'un "
         "roman et son univers ; rédiger le cahier des charges d'une application avant de la faire développer ; "
         "préparer un plan stratégique sur trois ans…",
         "Des documents clairs et structurés : synthèses, options comparées, décisions, plan d'action.",
         {"Idées", "Recherche", "Décisions", "Rédaction"}, false, false},
        {ProjectKind::Data, "data", "Analyse de données",
         "Explorer des fichiers Excel ou CSV, calculer, comparer, faire des graphiques et en tirer des conclusions.",
         "Ex. : analyser nos ventes 2023–2025 (exports Excel dans le dossier) par produit et par région, trouver ce "
         "qui explique la baisse du printemps, et produire un rapport avec graphiques ; nettoyer et fusionner trois "
         "fichiers clients…",
         "Des chiffres vérifiés, des graphiques, et un rapport de conclusions — avec des scripts qui refont le calcul.",
         {"Données", "Analyses", "Graphiques", "Rapport"}, true, true},
        {ProjectKind::Document, "document", "Rapport, audit ou étude",
         "Produire un document de référence : audit, rapport, étude, note de synthèse, veille, mémoire.",
         "Ex. : un audit de la sécurité informatique d'une PME de 20 personnes, avec constats et recommandations "
         "chiffrées ; une étude comparative de cinq logiciels de CRM ; une note de synthèse sur la nouvelle "
         "réglementation… Indiquez le public, la longueur visée et le format (Word, PDF…).",
         "Un document structuré et sourcé (Markdown, exportable en Word ou PDF), relu et cohérent.",
         {"Sources", "Analyse", "Rédaction", "Relecture"}, false, false},
        {ProjectKind::Hybrid, "hybrid", "Projet hybride",
         "Une part de réflexion ou de documents, une part de code ou de données : un prototype et sa conception, "
         "une étude et son outil…",
         "Ex. : concevoir un outil de suivi des stocks pour notre atelier (besoins, maquette, puis prototype) ; une "
         "étude d'impact accompagnée d'un tableau de bord interactif ; un jeu avec son document de conception…",
         "Des documents de conception ET un prototype ou des analyses qui fonctionnent.",
         {"Concept", "Réalisation", "Données", "Docs"}, true, true},
        {ProjectKind::Question, "question", "Juste une question",
         "Pas besoin d'équipe : une conversation avec Claude suffit. Vous pourrez la ranger dans un projet ensuite.",
         "Ex. : comment calculer une marge sur coût variable ? Que dit cette clause de mon contrat ? Quelle "
         "différence entre un CDD et un contrat de mission ?",
         "Une réponse, dans une conversation que vous pouvez poursuivre.",
         {}, false, false},
    };
    return t;
}

}  // namespace

const ProjectKindInfo& projectKindInfo(ProjectKind k) {
    for (auto& i : table())
        if (i.kind == k) return i;
    return table().front();
}

const std::vector<ProjectKind>& allProjectKinds() {
    static const std::vector<ProjectKind> v = {ProjectKind::Auto,     ProjectKind::Software, ProjectKind::Concept,
                                               ProjectKind::Data,     ProjectKind::Document, ProjectKind::Hybrid,
                                               ProjectKind::Question};
    return v;
}

ProjectKind projectKindFromId(const std::string& id) {
    for (auto& i : table())
        if (id == i.id) return i.kind;
    return ProjectKind::Auto;
}

const char* projectKindId(ProjectKind k) { return projectKindInfo(k).id; }

const char* projectFolderWord(ProjectKind k) {
    return k == ProjectKind::Software || k == ProjectKind::Hybrid ? "dépôt" : "dossier du projet";
}

std::string designerKindGuidance(ProjectKind k) {
    switch (k) {
        case ProjectKind::Software:
            return "## Nature du projet : logiciel\n"
                   "Un programme qui fonctionne. Les agents se partagent le dépôt (dossiers, modules) ; chacun vérifie "
                   "son travail par le build et les tests, et par une capture pour le visuel. stack = langages, "
                   "bibliothèques, outils de build et de tests ; conventions = build, tests, style.";
        case ProjectKind::Concept:
            return "## Nature du projet : idées et concept (pas de code)\n"
                   "Le livrable est un ensemble de DOCUMENTS Markdown bien structurés dans le dossier du projet "
                   "(ex. `docs/` : synthèse, options comparées, décisions argumentées, plan d'action). L'humain n'est "
                   "pas forcément technicien : écris pour lui, simplement.\n"
                   "- Agents typiques : recherche (faits, exemples, références), conception (idées, options, "
                   "arbitrages), rédaction (mise en forme claire), et un RELECTEUR critique qui vérifie la cohérence, "
                   "les angles morts et la clarté. 2 à 4 agents suffisent en général.\n"
                   "- Chaque agent possède des DOCUMENTS ou des sections (pas des dossiers de code).\n"
                   "- Vérifications : relecture croisée, cohérence entre documents, sources citées pour les faits, "
                   "questions ouvertes listées. Pas de build ni de tests.\n"
                   "- stack = outils et formats (Markdown, tableaux, schémas Mermaid, export Word/PDF si demandé) ; "
                   "conventions = ton, public, structure des documents, langue. visualChecks = false sauf schémas.";
        case ProjectKind::Data:
            return "## Nature du projet : analyse de données\n"
                   "Les données de l'humain (Excel, CSV…) sont la matière première : on ne les modifie JAMAIS, on "
                   "travaille sur des copies ou par scripts. Le livrable : des chiffres vérifiés, des graphiques et un "
                   "rapport de conclusions compréhensible par un non-spécialiste.\n"
                   "- Méthode : scripts Python reproductibles (pandas, matplotlib ; openpyxl pour Excel) rangés par "
                   "étape (ex. `analyses/01_nettoyage.py`), résultats dans `resultats/`, graphiques PNG dans "
                   "`graphiques/`, rapport `rapport.md`. Le premier jalon vérifie l'environnement (Python et "
                   "bibliothèques) et décrit les données (colonnes, volumes, qualité, valeurs manquantes).\n"
                   "- Agents typiques : préparation des données (nettoyage, fusion, contrôle qualité), analyse "
                   "(statistiques, segments, tendances), visualisation, rédaction du rapport. 2 à 4 agents.\n"
                   "- Vérifications : chaque chiffre du rapport vient d'un script relancé ; totaux recoupés avec les "
                   "fichiers sources ; graphiques examinés (captures). visualChecks = true.\n"
                   "- Si Python n'est pas installé, le premier jalon le constate et l'orchestrateur le demande à "
                   "l'humain (avec la marche à suivre) au lieu d'improviser.";
        case ProjectKind::Document:
            return "## Nature du projet : rapport, audit ou étude (pas de code)\n"
                   "Le livrable est un DOCUMENT de référence, structuré et sourcé, écrit pour son public (direction, "
                   "client, jury…). Rédaction en Markdown dans le dossier du projet ; export Word ou PDF en fin de "
                   "projet si demandé (pandoc s'il est installé, sinon instructions pour l'humain).\n"
                   "- Méthode : plan validé d'abord (jalon 1), puis collecte des sources et constats, analyse, "
                   "rédaction par parties, relecture, synthèse (résumé pour décideurs en tête).\n"
                   "- Agents typiques : collecte et sources (documents de l'humain, recherche web si utile), analyse et "
                   "constats, rédaction, RELECTEUR exigeant (exactitude, sources, cohérence, ton). Pour un audit : "
                   "constats → risques → recommandations priorisées.\n"
                   "- Chaque agent possède des parties du document (fichiers `sections/…`).\n"
                   "- Vérifications : toute affirmation factuelle est sourcée ; chiffres recoupés ; le plan est "
                   "respecté ; relecture croisée. Pas de build ni de tests. visualChecks = false.";
        case ProjectKind::Hybrid:
            return "## Nature du projet : hybride\n"
                   "Une part de réflexion ou de documents, une part de code ou de données. Commence par la conception "
                   "(besoins, choix, maquette) avant la réalisation, et garde les deux cohérents.\n"
                   "- Sépare nettement les agents « documents » (conception, rédaction, relecture : ils possèdent "
                   "`docs/`) et les agents « réalisation » (code ou analyses : ils possèdent leurs dossiers).\n"
                   "- Vérifications adaptées à chaque part : relecture et cohérence pour les documents ; build, tests, "
                   "captures ou scripts relancés pour la réalisation.\n"
                   "- stack = outils de la partie réalisation + formats des documents.";
        case ProjectKind::Question:
            return "## Nature : une simple question\nRéponds directement ; aucune équipe n'est nécessaire.";
        case ProjectKind::Auto:
        default:
            return "## Nature du projet : à déterminer\n"
                   "Le projet n'est pas forcément du logiciel : ce peut être des idées et un concept, une analyse de "
                   "données, un rapport ou un audit, ou un mélange. Détermine sa nature d'après la demande, indique-la "
                   "dans \"kind\" (software | concept | data | document | hybrid), et conçois l'équipe en "
                   "conséquence :\n"
                   "- logiciel : agents qui se partagent le dépôt, vérification par build et tests ;\n"
                   "- concept ou document : livrables Markdown structurés, agents recherche / rédaction / relecteur "
                   "critique, vérification par relecture et sources, pas de build ;\n"
                   "- données : scripts Python reproductibles, graphiques, rapport ; les données sources ne sont jamais "
                   "modifiées ; chaque chiffre vient d'un script relancé ;\n"
                   "- hybride : conception d'abord, puis réalisation, agents documents et agents réalisation séparés.";
    }
}

std::string orchestratorKindGuidance(ProjectKind k) {
    switch (k) {
        case ProjectKind::Software:
            return "## Nature du projet : logiciel\nLe livrable est un programme qui fonctionne. Vérifie par le build et "
                   "les tests (délégués), et par des captures pour ce qui est visible.";
        case ProjectKind::Concept:
            return "## Nature du projet : idées et concept\n"
                   "Pas de code : le travail produit des DOCUMENTS Markdown structurés dans le dossier du projet "
                   "(synthèses, options comparées, décisions, plan d'action). L'humain n'est pas forcément technicien.\n"
                   "- Découpe par question à traiter ou par document ; une tâche = un document ou une section aboutie.\n"
                   "- Vérifie par une RELECTURE CRITIQUE (confiée à un autre agent que l'auteur) : cohérence, angles "
                   "morts, clarté, faits sourcés. Pas de build ni de tests.\n"
                   "- Les choix qui engagent l'humain (orientation, public, priorités) lui reviennent : propose des "
                   "options argumentées (@@HUMAIN) plutôt que de trancher seul.\n"
                   "- Le bilan final présente les documents produits et comment les lire, en langage simple.";
        case ProjectKind::Data:
            return "## Nature du projet : analyse de données\n"
                   "- Les fichiers de données de l'humain ne sont JAMAIS modifiés : copies ou scripts seulement.\n"
                   "- Commence par vérifier l'environnement (Python, pandas, matplotlib, openpyxl) et décrire les "
                   "données (colonnes, volumes, qualité). Si Python manque, demande à l'humain (@@HUMAIN) avec la marche "
                   "à suivre plutôt que d'improviser.\n"
                   "- Chaque résultat vient d'un script reproductible (rangé par étape) ; un chiffre cité dans le rapport "
                   "doit pouvoir être recalculé. Recoupe les totaux avec les fichiers sources.\n"
                   "- Graphiques en PNG, examinés (captures) ; rapport final compréhensible par un non-spécialiste : "
                   "conclusions d'abord, méthode et limites ensuite.";
        case ProjectKind::Document:
            return "## Nature du projet : rapport, audit ou étude\n"
                   "Pas de code : le livrable est un document de référence, structuré et sourcé, en Markdown (export "
                   "Word/PDF en fin de projet si demandé : pandoc s'il est installé).\n"
                   "- Fais valider le PLAN par l'humain avant la rédaction (@@HUMAIN avec le plan proposé).\n"
                   "- Découpe par partie du document ; une tâche = une section rédigée et vérifiée.\n"
                   "- Vérifie par une relecture croisée exigeante (un autre agent que l'auteur) : exactitude, sources "
                   "citées pour chaque fait, chiffres recoupés, cohérence et ton adaptés au public. Pas de build ni de "
                   "tests.\n"
                   "- Pour un audit : constats → risques (gravité, probabilité) → recommandations priorisées et "
                   "chiffrées quand c'est possible.";
        case ProjectKind::Hybrid:
            return "## Nature du projet : hybride\n"
                   "Une part de documents (conception, rédaction), une part de réalisation (code ou données). Conception "
                   "d'abord, réalisation ensuite ; garde les deux cohérents (mets à jour les documents quand la "
                   "réalisation s'en écarte). Vérifie chaque part à sa façon : relecture et sources pour les documents ; "
                   "build, tests, scripts relancés ou captures pour la réalisation.";
        case ProjectKind::Question:
            return {};
        case ProjectKind::Auto:
        default:
            return "## Nature du projet\n"
                   "Ce n'est pas forcément du logiciel. Détermine-la d'après l'objectif et le contenu du dossier "
                   "(documents, données, code) et adapte ta méthode : documents → relecture critique et sources ; "
                   "données → scripts reproductibles, données sources jamais modifiées, graphiques vérifiés ; code → "
                   "build et tests. L'humain n'est pas forcément technicien : explique simplement.";
    }
}

std::string taskVerifyHint(ProjectKind k) {
    switch (k) {
        case ProjectKind::Concept:
        case ProjectKind::Document:
            return "relu : cohérence, sources citées, ce qui reste incertain";
        case ProjectKind::Data:
            return "script relancé + résultat clé en une ligne, totaux recoupés";
        case ProjectKind::Hybrid:
        case ProjectKind::Auto:
            return "commande lancée + résultat, ou relecture faite";
        default:
            return "commande + résultat en une ligne";
    }
}

std::string conversationKindHint(ProjectKind k) {
    switch (k) {
        case ProjectKind::Concept:
            return "Nature du projet : idées et concept. Produis des documents Markdown clairs et structurés dans le "
                   "dossier du projet ; présente les options avec leurs avantages et inconvénients ; écris simplement, "
                   "l'utilisateur n'est pas forcément technicien.";
        case ProjectKind::Data:
            return "Nature du projet : analyse de données. Ne modifie jamais les fichiers de données d'origine ; "
                   "travaille par scripts Python reproductibles (pandas, matplotlib, openpyxl pour Excel) ; vérifie "
                   "les chiffres ; explique les résultats simplement, conclusions d'abord. Si Python manque, dis-le "
                   "et explique comment l'installer.";
        case ProjectKind::Document:
            return "Nature du projet : rapport, audit ou étude. Rédige en Markdown structuré dans le dossier du "
                   "projet ; cite tes sources pour chaque fait ; adapte le ton au public visé ; export Word/PDF par "
                   "pandoc si l'utilisateur le demande.";
        case ProjectKind::Hybrid:
            return "Nature du projet : hybride (documents + réalisation). Garde la conception et la réalisation "
                   "cohérentes ; explique simplement les choix techniques.";
        default:
            return {};
    }
}

}  // namespace claude
