// Types de projets : table, prompts adaptés du Designer et de l'orchestrateur, consignes des agents.
#include <doctest/doctest.h>

#include "claude/Designer.h"
#include "claude/Orchestrator.h"
#include "claude/ProjectKind.h"
#include "claude/Transcript.h"

using namespace claude;

TEST_CASE("types de projets : table et identifiants") {
    CHECK(allProjectKinds().size() == 7);
    for (ProjectKind k : allProjectKinds()) {
        const auto& i = projectKindInfo(k);
        CHECK(i.kind == k);
        CHECK(projectKindFromId(i.id) == k);
        CHECK(std::string(i.label).size() > 3);
    }
    CHECK(projectKindFromId("") == ProjectKind::Auto);
    CHECK(projectKindFromId("n'importe quoi") == ProjectKind::Auto);
    CHECK(std::string(projectFolderWord(ProjectKind::Software)) == "dépôt");
    CHECK(std::string(projectFolderWord(ProjectKind::Document)) == "dossier du projet");
    CHECK(projectKindInfo(ProjectKind::Data).themes.size() == 4);
    CHECK(projectKindInfo(ProjectKind::Question).themes.empty());
}

TEST_CASE("types de projets : le Designer adapte sa demande") {
    DesignRequest r;
    r.description = "Analyser nos ventes";
    r.kind = "data";
    std::string p = designPrompt(r);
    CHECK(p.find("Type de projet choisi par l'humain : Analyse de données") != std::string::npos);
    CHECK(p.find("ne les modifie JAMAIS") != std::string::npos);
    CHECK(p.find("\"kind\"") != std::string::npos);
    r.kind = "auto";
    p = designPrompt(r);
    CHECK(p.find("Type de projet choisi") == std::string::npos);
    CHECK(p.find("Détermine sa nature") != std::string::npos);
}

TEST_CASE("types de projets : lecture de la conception et fichiers de départ") {
    DesignResponse r = parseDesignResponse(R"({"design": {"kind": "document", "name": "Audit", "objective": "x",
        "agents": [{"name": "01 Rédaction", "role": "r"}]}})");
    REQUIRE(r.ok);
    CHECK(r.design.kind == "document");
    // Anciennes conceptions (sans « kind ») : logiciel, comme avant
    r = parseDesignResponse(R"({"design": {"name": "A", "objective": "x", "agents": [{"name": "01 A"}]}})");
    CHECK(r.design.kind == "software");

    ProjectDesign d = demoDesign(ProjectKind::Document);
    CHECK(d.kind == "document");
    CHECK(checkDesign(d).errors.empty());
    auto files = scaffoldFiles(d, true, true, false);
    REQUIRE(!files.empty());
    CHECK(files[0].content.find("## Méthode") != std::string::npos);
    CHECK(files[0].content.find("dossier du projet") != std::string::npos);
    CHECK(files[0].content.find("_captures") == std::string::npos);  // pas de vérification visuelle
    CHECK(projectNotes(d).find("Type : Rapport, audit ou étude") != std::string::npos);

    OrchestratorSettings s;
    applyDesign(d, s);
    CHECK(s.projectKind == "document");
    CHECK_FALSE(s.visualChecks);
    // Aller-retour du brouillon
    DesignerDraft dr;
    dr.req.kind = "concept";
    dr.design = d;
    DesignerDraft back;
    back.fromJson(dr.toJson());
    CHECK(back.req.kind == "concept");
    CHECK(back.design.kind == "document");
}

TEST_CASE("types de projets : l'orchestrateur et les agents suivent le type") {
    std::string code = Orchestrator::systemPrompt(true, false, false, "software");
    std::string doc = Orchestrator::systemPrompt(true, false, false, "document");
    CHECK(code.find("dans le même dépôt") != std::string::npos);
    CHECK(doc.find("dans le même dossier du projet") != std::string::npos);
    CHECK(doc.find("Fais valider le PLAN") != std::string::npos);
    CHECK(doc.find("@@KIND@@") == std::string::npos);
    CHECK(doc.find("@@FOLDER@@") == std::string::npos);
    std::string data = Orchestrator::systemPrompt(true, false, true, "data");
    CHECK(data.find("JAMAIS modifiés") != std::string::npos);
    CHECK(data.find("`_captures/` à la racine du dossier du projet") != std::string::npos);
    // Réglages enregistrés
    Orchestrator o;
    o.settings.projectKind = "concept";
    Orchestrator b;
    b.fromJson(o.toJson());
    CHECK(b.settings.projectKind == "concept");
    CHECK(taskVerifyHint(ProjectKind::Document).find("sources") != std::string::npos);
    CHECK(conversationKindHint(ProjectKind::Software).empty());
}

TEST_CASE("outils en clair pour tous les publics") {
    CHECK(toolLabel("Bash") == "Commande");
    CHECK(toolLabel("Grep") == "Recherche dans les fichiers");
    CHECK(toolLabel("mcp__claude-in-chrome__navigate") == "Navigateur");
    CHECK(toolLabel("mcp__notion__search") == "notion : search");
    CHECK(toolLabel("OutilInconnu") == "OutilInconnu");
    CHECK(toolAction("Edit") == "modifier un fichier");
    CHECK(toolSummary("Read", nlohmann::json{{"file_path", "rapport.md"}}) == "Lecture · rapport.md");
}
