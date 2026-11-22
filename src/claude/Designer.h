#pragma once
// Designer : conçoit un projet et l'équipe qui va le réaliser à partir d'une description.
//
// Claude (par défaut en mode plan : il peut lire un dossier existant, rien modifier) renvoie une conception
// structurée en JSON : projet, objectif et jalons de l'orchestrateur, agents avec leur fiche de rôle. L'humain
// l'ajuste (à la main ou en demandant à Claude d'affiner), puis ClaudeDesk crée le projet cowork, un thème par
// agent (fiche en notes), les fichiers de départ du dépôt et l'orchestrateur réglé.
// Ce module n'a pas d'interface (voir ui/DesignerPanel.cpp) : prompts, analyse de la réponse, validation,
// fichiers de départ, réglages de l'orchestrateur.
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "claude/Orchestrator.h"
#include "claude/ProjectKind.h"

namespace claude {

struct DesignAgent {
    std::string name;     // nom du thème (« 01 Fondations »)
    std::string mission;  // une phrase
    std::string role;     // fiche de rôle Markdown (Mission, Possède, Frontières, Conventions, Vérifications)
    std::string model = "sonnet";
    std::string effort = "medium";
    bool browser = false;  // équipé du navigateur (Claude in Chrome)
};

struct ProjectDesign {
    std::string kind = "software";  // type de projet (claude/ProjectKind) : software, concept, data, document, hybrid
    std::string name;
    std::string summary;      // 2–4 phrases, choix faits par le designer compris
    std::vector<std::string> stack;  // technologies (logiciel) ou outils et formats (autres types)
    std::string objective;    // objectif de l'orchestrateur : livrable, critères d'acceptation, priorités
    std::vector<std::string> milestones;
    std::string conventions;  // règles de travail : build, tests, style (code) ; ton, public, structure (documents)
    std::vector<DesignAgent> agents;
    std::string budget = "medium";  // low | medium | high | max
    bool visualChecks = true;

    bool empty() const { return name.empty() && agents.empty() && objective.empty(); }
};

struct DesignQuestion {
    std::string question;
    std::string why;
    std::vector<std::string> options;
    int recommended = 0;
    int choice = -1;    // option choisie ; -1 aucune ; -2 « Autre » (texte libre)
    std::string other;
    std::string answer() const;  // réponse retenue (vide = au choix du designer)
};

struct DesignRequest {
    std::string kind = "auto";  // type choisi par l'humain (« auto » : le designer détermine)
    std::string description;
    std::string folder;        // dossier de travail visé (existant ou à créer) ; vide = à décider
    bool analyzeFolder = false;
    std::string tech;          // technologies imposées
    std::string constraints;
    int teamSize = 0;          // 0 auto, 1 petite (2–3), 2 moyenne (4–6), 3 grande (7–10)
    std::string budget = "medium";
    bool allowQuestions = true;
    std::vector<DesignQuestion> answered;  // questions de clarification déjà répondues
};

// Ce que Claude a renvoyé : des questions, ou une conception.
struct DesignResponse {
    bool ok = false;
    std::string error;
    std::vector<DesignQuestion> questions;
    bool hasDesign = false;
    ProjectDesign design;
};

std::string designPrompt(const DesignRequest& req);
std::string refinePrompt(const DesignRequest& req, const ProjectDesign& current, const std::string& feedback);
DesignResponse parseDesignResponse(const std::string& text);

// Problèmes de la conception : `errors` bloquent la création, `warnings` sont des conseils.
struct DesignCheck {
    std::vector<std::string> errors, warnings;
};
DesignCheck checkDesign(const ProjectDesign& d);
// (noms nettoyés par cleanAgentName, de l'orchestrateur)

// Fichiers de départ du dépôt (chemins relatifs) : CLAUDE.md, docs/PLAN.md, docs/agents/<agent>.md.
struct ScaffoldFile {
    std::string path;
    std::string content;
};
std::vector<ScaffoldFile> scaffoldFiles(const ProjectDesign& d, bool claudeMd, bool plan, bool agentDocs);
std::string projectNotes(const ProjectDesign& d);  // notes du projet cowork
std::string agentNotes(const DesignAgent& a);      // notes du thème = fiche relue par l'agent à chaque tâche

BudgetPreset budgetPresetFromName(const std::string& name);
const char* budgetName(BudgetPreset p);
// Réglages de l'orchestrateur du projet : niveau de budget, objectif + jalons, vérification visuelle, navigateur.
void applyDesign(const ProjectDesign& d, OrchestratorSettings& s);

nlohmann::json toJson(const ProjectDesign& d);
ProjectDesign designFromJson(const nlohmann::json& j);
nlohmann::json toJson(const DesignQuestion& q);
DesignQuestion questionFromJson(const nlohmann::json& j);

// Brouillon du Designer (persisté entre les lancements).
struct DesignerDraft {
    int step = 0;  // 0 Décrire, 1 Clarifier, 2 Ajuster, 3 Créer
    DesignRequest req;
    std::vector<DesignQuestion> questions;  // en attente de réponse
    ProjectDesign design;
    std::vector<ProjectDesign> history;     // versions précédentes (annuler une génération)
    std::string feedback;                   // « Affiner : que changer ? »
    // Réglages du designer
    std::string model = "opus";
    std::string effort;
    std::string permissionMode = "plan";
    // Création
    std::string cwd;                        // dossier de travail retenu à la création
    bool writeClaudeMd = true, writePlan = true, writeAgentDocs = true;
    bool openOrchestrator = true, startNow = false;
    std::string createdProject;             // chemin du projet créé (UTF-8) ; vide = pas encore
    double lastCostUsd = 0;

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
};

// Conception d'exemple (captures de test) : logiciel, ou rapport d'audit pour ProjectKind::Document.
ProjectDesign demoDesign(ProjectKind kind = ProjectKind::Software);

}  // namespace claude
