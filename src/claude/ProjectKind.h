#pragma once
// Types de projets : ClaudeDesk ne sert pas qu'à écrire du code. Organiser des idées, analyser des données, produire
// un audit ou un rapport, mener un projet hybride (une part de concept, une part de code) : chaque type a son
// vocabulaire, ses thèmes de départ, sa façon de travailler et de vérifier, et ses livrables.
//
// Le type d'un projet est choisi dans le Designer (ou à la création d'un projet), enregistré dans ses notes de
// dossier (`projectKind`) et dans les réglages de son orchestrateur. Il guide :
//   - le Designer (conception de l'équipe adaptée),
//   - l'orchestrateur (comment découper, vérifier, livrer) et les consignes de chaque tâche d'agent,
//   - la mémoire injectée dans les conversations du projet (conseils de méthode),
//   - l'interface (libellés, thèmes proposés, exemples).
// Module sans interface (testé dans tests/test_project_kind.cpp).
#include <string>
#include <vector>

namespace claude {

enum class ProjectKind {
    Auto,      // « Laisser Claude choisir » : le Designer (ou l'orchestrateur) détermine la nature du projet
    Software,  // logiciel, application, script, site
    Concept,   // idées, stratégie, conception produit, cahier des charges, univers créatif
    Data,      // analyse de données : tableaux, statistiques, graphiques, tableaux de bord
    Document,  // rapport, audit, étude, rédaction structurée, veille
    Hybrid,    // une part de concept / documents, une part de code ou de données
    Question,  // pas un projet : une question, une conversation suffit
};

struct ProjectKindInfo {
    ProjectKind kind;
    const char* id;          // identifiant enregistré (« software », « data »…)
    const char* label;       // « Analyse de données »
    const char* tagline;     // une ligne pour les cartes de choix
    const char* examples;    // texte indicatif de la description
    const char* deliverable; // ce que l'on obtient à la fin, en clair
    std::vector<const char*> themes;  // thèmes proposés à la création manuelle d'un projet
    bool visualChecks;       // captures des livrables visibles par défaut
    bool code;               // le projet contient du code (vocabulaire « dépôt », build, tests)
};

const ProjectKindInfo& projectKindInfo(ProjectKind k);
const std::vector<ProjectKind>& allProjectKinds();  // ordre d'affichage
ProjectKind projectKindFromId(const std::string& id);  // inconnu ou vide → Auto
const char* projectKindId(ProjectKind k);

// Nom du dossier de fichiers du projet, selon le type : « dépôt » pour du code, « dossier du projet » sinon.
const char* projectFolderWord(ProjectKind k);

// --- Textes pour Claude -----------------------------------------------------------------------------------------
// Designer : comment concevoir l'équipe et les livrables pour ce type (section « ## Nature du projet »).
std::string designerKindGuidance(ProjectKind k);
// Orchestrateur : comment mener ce type de projet (découpage, vérifications, livrables) — section du prompt.
std::string orchestratorKindGuidance(ProjectKind k);
// Agent : ce qui compte comme « vérifié » dans un compte rendu de tâche pour ce type.
std::string taskVerifyHint(ProjectKind k);
// Conversation ordinaire d'un projet de ce type : conseils de méthode courts, injectés avec la mémoire du projet.
std::string conversationKindHint(ProjectKind k);

}  // namespace claude
