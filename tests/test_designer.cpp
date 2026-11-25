#include <doctest/doctest.h>

#include "claude/Designer.h"

using namespace claude;

TEST_CASE("designer : prompt") {
    DesignRequest r;
    r.description = "Un jeu de Pong en C++";
    r.folder = "C:\\Dev\\Pong";
    r.analyzeFolder = true;
    std::string p = designPrompt(r);
    CHECK(p.find("Un jeu de Pong en C++") != std::string::npos);
    CHECK(p.find("Read, Glob et Grep") != std::string::npos);
    CHECK(p.find("\"questions\"") != std::string::npos);  // questions permises au premier passage
    // Après des réponses : plus de questions
    DesignQuestion q;
    q.question = "Plateforme ?";
    q.options = {"Windows", "Web"};
    q.choice = 1;
    r.answered.push_back(q);
    p = designPrompt(r);
    CHECK(p.find("Plateforme ? → Web") != std::string::npos);
    CHECK(p.find("Ne pose pas de question") != std::string::npos);
    // Affiner : conception actuelle + demande
    std::string rp = refinePrompt(r, demoDesign(), "Fusionne 01 et 02");
    CHECK(rp.find("Carnet de recettes") != std::string::npos);
    CHECK(rp.find("Fusionne 01 et 02") != std::string::npos);
}

TEST_CASE("designer : analyse de la réponse") {
    auto r = parseDesignResponse(R"(Voici :
```json
{"design":{"name":"Pong / C++","summary":"s","stack":"C++20, SDL3","objective":"o","milestones":["a","b"],
 "budget":"HIGH","agents":[{"name":"01 Moteur","role":"## Mission\nmoteur","model":"Sonnet"},
 {"name":"01 Moteur","mission":"double"},{"name":"  "}]}}
```)");
    REQUIRE(r.ok);
    REQUIRE(r.hasDesign);
    CHECK(r.design.name == "Pong C++");  // caractère interdit retiré
    REQUIRE(r.design.stack.size() == 2);
    CHECK(r.design.stack[1] == "SDL3");
    CHECK(r.design.budget == "high");
    REQUIRE(r.design.agents.size() == 2);  // agent sans nom ignoré
    CHECK(r.design.agents[0].model == "sonnet");
    CHECK(r.design.agents[0].mission == "## Mission");  // repli : première ligne de la fiche
    CHECK(r.design.agents[1].name == "01 Moteur 2");    // doublon renommé

    auto q = parseDesignResponse(R"({"questions":[{"question":"Langage ?","options":["C++","Rust"],"recommended":1},{"question":""}]})");
    REQUIRE(q.ok);
    REQUIRE(q.questions.size() == 1);
    CHECK(q.questions[0].choice == 1);  // l'option recommandée est présélectionnée
    CHECK(q.questions[0].answer() == "Rust");

    CHECK_FALSE(parseDesignResponse("pas de json").ok);
    CHECK_FALSE(parseDesignResponse("{\"autre\":1}").ok);
}

TEST_CASE("designer : validation") {
    ProjectDesign d = demoDesign();
    auto c = checkDesign(d);
    CHECK(c.errors.empty());
    d.agents[1].name = "01 fondations";  // même nom à la casse près
    d.agents[2].name = "a\\b";
    d.objective.clear();
    c = checkDesign(d);
    CHECK(c.errors.size() == 3);
    ProjectDesign e;
    CHECK(checkDesign(e).errors.size() >= 3);
}

TEST_CASE("designer : fichiers de départ, notes et orchestrateur") {
    ProjectDesign d = demoDesign();
    auto files = scaffoldFiles(d, true, true, true);
    REQUIRE(files.size() == 2 + d.agents.size());
    CHECK(files[0].path == "CLAUDE.md");
    CHECK(files[0].content.find("| 02 API |") != std::string::npos);
    CHECK(files[1].path == "docs/PLAN.md");
    CHECK(files[2].path == "docs/agents/01-Fondations.md");
    CHECK(scaffoldFiles(d, false, false, true).size() == d.agents.size());

    std::string n = agentNotes(d.agents[3]);
    CHECK(n.rfind("Tests de bout en bout et vérification visuelle dans le navigateur. Modèle conseillé : haiku", 0) == 0);
    CHECK(projectNotes(d).find("## Jalons") != std::string::npos);

    OrchestratorSettings s;
    d.budget = "low";
    applyDesign(d, s);
    CHECK(budgetPresetMatches(s, BudgetPreset::Low));
    CHECK(s.objective.find("Jalons, dans l'ordre") != std::string::npos);
    REQUIRE(s.browserAgents.size() == 1);
    CHECK(s.browserAgents[0] == "04 Recette");
}

TEST_CASE("designer : brouillon sauvegardé") {
    DesignerDraft a;
    a.step = 2;
    a.req.description = "x";
    a.req.teamSize = 2;
    a.design = demoDesign();
    a.history.push_back(demoDesign());
    a.permissionMode = "default";
    a.startNow = true;
    DesignQuestion q;
    q.question = "?";
    q.options = {"a"};
    q.choice = -2;
    q.other = "libre";
    a.req.answered.push_back(q);
    DesignerDraft b;
    b.fromJson(a.toJson());
    CHECK(b.step == 2);
    CHECK(b.req.teamSize == 2);
    CHECK(b.design.agents.size() == 4);
    CHECK(b.design.agents[3].browser);
    CHECK(b.history.size() == 1);
    CHECK(b.permissionMode == "default");
    CHECK(b.startNow);
    REQUIRE(b.req.answered.size() == 1);
    CHECK(b.req.answered[0].answer() == "libre");
    // Étape incohérente avec le contenu : retour au début
    DesignerDraft c;
    c.step = 2;
    DesignerDraft d;
    d.fromJson(c.toJson());
    CHECK(d.step == 0);
}

TEST_CASE("designer : ligne d'activité tirée de la réflexion") {
    int para = 0;
    CHECK(thinkingHeadline("", &para).empty());
    CHECK(para == -1);
    // Début trop court : rien à dire encore.
    CHECK(thinkingHeadline("I'm", &para).empty());
    // Première phrase du paragraphe.
    CHECK(thinkingHeadline("I'm weighing the agents. Then the rest", &para) == "I'm weighing the agents.");
    CHECK(para == 0);
    // Titre en gras, et le dernier paragraphe l'emporte.
    CHECK(thinkingHeadline("First idea here.\n\n**Planning the team**\nDetails", &para) == "Planning the team");
    CHECK(para == 1);
    // Dernier paragraphe encore trop court : on garde le précédent.
    CHECK(thinkingHeadline("Je compare les moteurs de calcul.\n\nJe", &para) == "Je compare les moteurs de calcul.");
    CHECK(para == 0);
    // Coupé à la longueur demandée.
    std::string longLine(400, 'a');
    CHECK(thinkingHeadline(longLine, nullptr, 50).size() <= 53);
}

TEST_CASE("designer : précisions dans le prompt et le brouillon") {
    DesignRequest r;
    r.description = "Un playground d'IA";
    r.clarifications = "Pas de service en ligne.";
    CHECK(designPrompt(r).find("Précisions supplémentaires de l'humain\nPas de service en ligne.") != std::string::npos);
    CHECK(designPrompt(r).find("Réfléchis en français") != std::string::npos);
    DesignerDraft d;
    d.req = r;
    DesignerDraft e;
    e.fromJson(d.toJson());
    CHECK(e.req.clarifications == "Pas de service en ligne.");
}
