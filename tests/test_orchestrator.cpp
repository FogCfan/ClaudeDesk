#include <doctest/doctest.h>

#include <map>

#include "claude/Orchestrator.h"

using namespace claude;

// ---------------------------------------------------------------------------
// Analyse des directives
// ---------------------------------------------------------------------------

TEST_CASE("orchestrateur : directives @@DELEGUER / @@HUMAIN / @@TERMINE") {
    std::string txt =
        "J'ai regardé le dépôt.\n\n"
        "@@DELEGUER 02 Toolchain & Build\n"
        "Installe la toolchain.\nCritère : `e7twin doctor` vert.\n"
        "@@FIN\n"
        "@@DÉLÉGUER **04 Simulateur Renode**\r\n"
        "Spike Renode.\r\n"
        "@@FIN\r\n";
    Directives d = parseDirectives(txt);
    REQUIRE(d.delegations.size() == 2);
    CHECK(d.delegations[0].agent == "02 Toolchain & Build");
    CHECK(d.delegations[0].task == "Installe la toolchain.\nCritère : `e7twin doctor` vert.");
    CHECK(d.delegations[1].agent == "04 Simulateur Renode");
    CHECK(d.delegations[1].task == "Spike Renode.");
    CHECK_FALSE(d.finished);
    CHECK_FALSE(d.needHuman);

    Directives h = parseDirectives("Bilan\n@@HUMAIN\nQuel UART pour le printf ?\n@@FIN");
    CHECK(h.needHuman);
    CHECK(h.question == "Quel UART pour le printf ?");

    Directives f = parseDirectives("@@TERMINE\nTout est livré et testé.");  // bloc non refermé accepté
    CHECK(f.finished);
    CHECK(f.summary == "Tout est livré et testé.");

    CHECK(parseDirectives("Aucune directive ici. @@DELEGUER en milieu de ligne").empty());
}

TEST_CASE("orchestrateur : correspondance des noms d'agents") {
    std::vector<std::string> a = {"00 Coordination", "01 Modèle de carte", "02 Toolchain & Build", "04 Simulateur Renode"};
    CHECK(matchAgent("02 Toolchain & Build", a) == 2);
    CHECK(matchAgent("02 toolchain & build", a) == 2);
    CHECK(matchAgent("02", a) == 2);
    CHECK(matchAgent("2", a) == 2);
    CHECK(matchAgent("Simulateur", a) == 3);
    CHECK(matchAgent("`01 Modèle de carte`", a) == 1);
    CHECK(matchAgent("Inconnu", a) == -1);
    CHECK(matchAgent("o", a) == -1);  // ambigu
    CHECK(matchAgent("", a) == -1);
}

TEST_CASE("orchestrateur : texte du dernier tour") {
    std::vector<Entry> e(6);
    e[0].kind = EntryKind::User, e[0].text = "ancien";
    e[1].kind = EntryKind::Assistant, e[1].text = "ancienne réponse";
    e[2].kind = EntryKind::User, e[2].text = "nouveau";
    e[3].kind = EntryKind::Assistant, e[3].text = "Je lis.";
    e[4].kind = EntryKind::ToolUse, e[4].text = "Read x";
    e[5].kind = EntryKind::Assistant, e[5].text = "Fini.";
    CHECK(lastTurnText(e) == "Je lis.\n\nFini.");
    Entry sub;
    sub.kind = EntryKind::Assistant, sub.text = "sous-agent", sub.parentToolUseId = "t1";
    e.push_back(sub);
    CHECK(lastTurnText(e) == "Je lis.\n\nFini.");
}

// ---------------------------------------------------------------------------
// Moteur, avec un hôte simulé
// ---------------------------------------------------------------------------

namespace {

struct FakeConv {
    ParticipantStatus st;
    std::vector<std::string> received;
    SendOptions last;
};

struct FakeHost : OrchestratorHost {
    std::vector<AgentInfo> agentList = {{"01 Modèle", "faits matériels"}, {"02 Build", "toolchain"}, {"03 Check", "barrière"}};
    std::map<std::string, FakeConv> convs;
    double usage = 0.1, weekly = 0.1;
    bool sub = false;
    int64_t t = 1000;
    std::string brainPrompt;

    std::vector<AgentInfo> agents() override { return agentList; }
    std::vector<std::string> brainTools;
    std::vector<std::string> interrupted;
    std::string openBrain(const std::string& hint, const std::string& sp, const std::vector<std::string>& tools) override {
        brainPrompt = sp;
        brainTools = tools;
        std::string id = hint.empty() ? "brain" : hint;
        convs[id].st.exists = true;
        return id;
    }
    std::string openAgent(const std::string& agent, const std::string& hint) override {
        std::string id = hint.empty() ? "conv:" + agent : hint;
        convs[id].st.exists = true;
        return id;
    }
    bool ensureOpen(const std::string& id) override { return convs.count(id) > 0; }
    bool send(const std::string& id, const std::string& text, const SendOptions& opt) override {
        auto& c = convs[id];
        c.received.push_back(text);
        c.last = opt;
        c.st.busy = true;
        return true;
    }
    ParticipantStatus status(const std::string& id) override {
        auto it = convs.find(id);
        return it == convs.end() ? ParticipantStatus{} : it->second.st;
    }
    double utilization(const std::string& w) override { return w == "five_hour" ? usage : weekly; }
    int64_t resetsAt(const std::string& w) override { return w == "five_hour" ? 5000 : 900000; }
    bool isSubscription() override { return sub; }
    // Rotation : un premier appel lance le « récap » (renvoie ""), le suivant crée la nouvelle conversation.
    std::map<std::string, int> rotateCalls;
    int rotations = 0;
    std::string rotate(const std::string& agent, const std::string& id) override {
        if (rotateCalls[id]++ == 0) return {};
        ++rotations;
        std::string nid = id + "#" + std::to_string(rotations);
        convs[nid].st.exists = true;
        (void)agent;
        return nid;
    }
    int64_t now() override { return t; }
    void interrupt(const std::string& id) override { interrupted.push_back(id); }

    // Simule la fin d'un tour.
    void answer(const std::string& id, const std::string& text, double cost = 0.5) {
        auto& c = convs[id];
        c.st.busy = false;
        c.st.turns++;
        c.st.costUsd += cost;
        c.st.lastText = text;
        c.st.lastTurnAt = t;
    }
};

}  // namespace

TEST_CASE("orchestrateur : cycle complet jusqu'à @@TERMINE") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "Construire E7 Twin";
    o.settings.maxParallel = 1;
    REQUIRE(o.start(h));
    CHECK(o.state == OrchState::Thinking);
    CHECK(h.brainPrompt.find("@@DELEGUER") != std::string::npos);
    REQUIRE(h.convs["brain"].received.size() == 1);
    CHECK(h.convs["brain"].received[0].find("02 Build") != std::string::npos);
    CHECK(h.convs["brain"].last.permissionMode == "auto");

    o.tick(h);  // le cerveau réfléchit encore
    CHECK(o.state == OrchState::Thinking);

    h.answer("brain", "Plan.\n@@DELEGUER 02\nInstalle.\n@@FIN\n@@DELEGUER 01 Modèle\nÉcris le YAML.\n@@FIN");
    o.tick(h);
    CHECK(o.state == OrchState::Working);
    CHECK(o.cycle == 1);
    REQUIRE(o.tasks.size() == 2);
    // Un seul agent à la fois (maxParallel = 1)
    CHECK(h.convs["conv:02 Build"].received.size() == 1);
    CHECK(h.convs["conv:02 Build"].received[0].find("Installe.") != std::string::npos);
    CHECK(h.convs.count("conv:01 Modèle") == 0);

    h.answer("conv:02 Build", "Fait : toolchain installée.");
    o.tick(h);
    CHECK(h.convs["conv:01 Modèle"].received.size() == 1);
    CHECK(o.state == OrchState::Working);

    h.answer("conv:01 Modèle", "Fait : YAML écrit.");
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    REQUIRE(h.convs["brain"].received.size() == 2);
    const std::string& rep = h.convs["brain"].received[1];
    CHECK(rep.find("toolchain installée") != std::string::npos);
    CHECK(rep.find("YAML écrit") != std::string::npos);
    CHECK(o.spentUsd == doctest::Approx(1.5));  // cerveau 0,5 + 2 agents

    h.answer("brain", "Tout est vérifié.\n@@TERMINE\nLivré.\n@@FIN");
    o.tick(h);
    CHECK(o.state == OrchState::Finished);
    CHECK(o.finalSummary == "Livré.");
    CHECK_FALSE(o.running());
}

TEST_CASE("orchestrateur : discussion après la fin, une question relance l'orchestration (cas réel)") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "@@TERMINE\nLivré.\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::Finished);
    o.tick(h);  // la réponse déjà traitée n'est pas relue
    CHECK(o.state == OrchState::Finished);

    // Simple discussion (pas de directive) : l'orchestration reste terminée.
    h.answer("brain", "Oui : le dashboard web fait office d'interface.");
    o.tick(h);
    CHECK(o.state == OrchState::Finished);

    // Le cerveau propose du travail en posant une question : elle devient cliquable (WaitingHuman).
    h.answer("brain", "Explications.\n@@HUMAIN\nVoulez-vous cet exemple ?\n- [ ] Oui, HP et HE (recommandé)\n- [ ] Non\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::WaitingHuman);
    REQUIRE(o.questions.size() == 1);
    CHECK(o.questions[0].options.size() == 2);
    CHECK(o.finalSummary == "Livré.");  // le bilan précédent reste lisible

    REQUIRE(o.answerHuman(h, "Oui, HP et HE"));
    CHECK(o.state == OrchState::Thinking);
    h.answer("brain", "@@DELEGUER 02 | modele=sonnet\nÉcris l'exemple.\n@@FIN");
    o.tick(h);
    CHECK(o.state == OrchState::Working);
    CHECK(h.convs["conv:02 Build"].received.size() == 1);
}

TEST_CASE("orchestrateur : discussion directe sans directive, ni relance ni bilan forcé") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.maxParallel = 2;
    REQUIRE(o.start(h));
    CHECK(h.brainPrompt.find("Quand l'humain t'écrit directement") != std::string::npos);

    // Question en attente : vous posez une question à côté, il répond simplement → la question reste posée.
    h.answer("brain", "@@HUMAIN\nQuel UART ?\n- [ ] UART2\n- [ ] UART4\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::WaitingHuman);
    size_t sent = h.convs["brain"].received.size();
    h.answer("brain", "L'UART2 est relié au connecteur J4, l'UART4 au débogueur.");
    o.tick(h);
    CHECK(o.state == OrchState::WaitingHuman);
    CHECK(o.questions.size() == 1);
    CHECK(h.convs["brain"].received.size() == sent);  // pas de « Ta réponse ne contient aucune directive »

    // Réponse à la question → délégations → l'équipe travaille.
    REQUIRE(o.answerHuman(h, "UART2"));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::Working);
    o.tick(h);
    CHECK(o.tasks.size() == 2);

    // Pendant le travail : simple discussion → l'équipe continue, rien n'est relancé.
    sent = h.convs["brain"].received.size();
    h.answer("brain", "Le build se lance avec `cmake --build`.");
    o.tick(h);
    CHECK(o.state == OrchState::Working);
    CHECK(o.tasks.size() == 2);
    CHECK(h.convs["brain"].received.size() == sent);

    // Pendant le travail : la discussion aboutit à une délégation → appliquée.
    h.answer("brain", "Bonne idée.\n@@DELEGUER 03\nVérifie le README.\n@@FIN");
    o.tick(h);
    CHECK(o.tasks.size() == 3);
    CHECK(h.convs.count("conv:03 Check") == 0);  // en file : deux agents travaillent déjà (maxParallel = 2)
    o.tick(h);  // déjà lue : pas de doublon
    CHECK(o.tasks.size() == 3);
    h.answer("conv:01 Modèle", "ok");
    o.tick(h);
    CHECK(h.convs["conv:03 Check"].received.size() == 1);
}

TEST_CASE("orchestrateur : sauvegarde ancienne en plein travail, la dernière réponse n'est pas rejouée") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::Working);
    auto j = o.toJson();
    j.erase("brainSeenTurns");  // fichier d'une version précédente
    Orchestrator o2;
    o2.fromJson(j);
    o2.resume(h);
    o2.tick(h);
    o2.tick(h);
    CHECK(o2.tasks.size() == 1);
}

TEST_CASE("orchestrateur : bilan et question dans la même réponse : la question l'emporte") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "@@TERMINE\nFait.\n@@FIN\n@@HUMAIN\nJe continue avec la suite ?\n- [ ] Oui\n- [ ] Non\n@@FIN");
    o.tick(h);
    CHECK(o.state == OrchState::WaitingHuman);
    CHECK(o.finalSummary == "Fait.");
}

TEST_CASE("orchestrateur : agent inconnu, relance sans directive, question humaine") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));

    h.answer("brain", "Je réfléchis…");  // pas de directive → relance
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    REQUIRE(h.convs["brain"].received.size() == 2);
    CHECK(h.convs["brain"].received[1].find("aucune directive") != std::string::npos);

    h.answer("brain", "@@DELEGUER 99 Fantôme\nx\n@@FIN");  // agent inconnu → compte rendu d'échec immédiat
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    CHECK(h.convs["brain"].received.back().find("Agent inconnu") != std::string::npos);

    h.answer("brain", "@@HUMAIN\nQuel UART ?\n@@FIN");
    o.tick(h);
    CHECK(o.state == OrchState::WaitingHuman);
    CHECK(o.humanQuestion == "Quel UART ?");
    o.tick(h);
    CHECK(o.state == OrchState::WaitingHuman);

    // L'utilisateur répond dans la conversation ; le cerveau délègue ensuite.
    h.answer("brain", "Merci.\n@@DELEGUER 03\nVérifie.\n@@FIN");
    o.tick(h);
    CHECK(o.state == OrchState::Working);
    CHECK(h.convs["conv:03 Check"].received.size() == 1);
}

TEST_CASE("orchestrateur : garde-fous budget, cycles, usage et plantage") {
    SUBCASE("budget") {
        FakeHost h;
        Orchestrator o;
        o.settings.objective = "x";
        o.settings.maxCostUsd = 1.0;
        o.settings.strictBudget = false;
        o.settings.budgetTolerance = 0;
        REQUIRE(o.start(h));
        h.answer("brain", "@@DELEGUER 01\na\n@@FIN", 1.2);  // le cerveau seul dépasse le budget
        o.tick(h);
        CHECK(o.state == OrchState::Paused);
        CHECK(o.pauseReason.find("budget") != std::string::npos);
        CHECK(h.convs.count("conv:01 Modèle") == 0);
        o.settings.maxCostUsd = 10;
        o.resume(h);
        o.tick(h);
        CHECK(h.convs["conv:01 Modèle"].received.size() == 1);
    }
    SUBCASE("cycles") {
        FakeHost h;
        Orchestrator o;
        o.settings.objective = "x";
        o.settings.maxCycles = 1;
        REQUIRE(o.start(h));
        h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
        o.tick(h);
        h.answer("conv:01 Modèle", "ok");
        o.tick(h);
        CHECK(o.state == OrchState::Paused);
        CHECK(o.pauseReason.find("cycles") != std::string::npos);
        o.settings.maxCycles = 5;
        o.resume(h);
        o.tick(h);
        CHECK(o.state == OrchState::Thinking);
        CHECK(h.convs["brain"].received.size() == 2);
    }
    SUBCASE("usage") {
        FakeHost h;
        Orchestrator o;
        o.settings.objective = "x";
        REQUIRE(o.start(h));
        h.usage = 0.95;
        h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
        o.tick(h);
        CHECK(o.state == OrchState::UsagePause);
        o.tick(h);
        CHECK(o.state == OrchState::UsagePause);
        h.t = 6000;  // après la réinitialisation de la fenêtre
        o.tick(h);
        CHECK(o.state == OrchState::Working);
        o.tick(h);
        CHECK(h.convs["conv:01 Modèle"].received.size() == 1);
    }
    SUBCASE("agent planté") {
        FakeHost h;
        Orchestrator o;
        o.settings.objective = "x";
        REQUIRE(o.start(h));
        h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
        o.tick(h);
        auto& c = h.convs["conv:01 Modèle"];
        c.st.busy = false;
        c.st.stopped = true;  // processus mort, aucun tour terminé
        o.tick(h);
        CHECK(o.state == OrchState::Thinking);
        CHECK(h.convs["brain"].received.back().find("ÉCHEC") != std::string::npos);
    }
}

TEST_CASE("orchestrateur : sauvegarde et reprise après fermeture") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.maxParallel = 2;
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::Working);

    Orchestrator r;
    r.fromJson(o.toJson());
    CHECK(r.state == OrchState::Paused);
    CHECK(r.resumeState() == OrchState::Working);
    CHECK(r.settings.maxParallel == 2);
    CHECK(r.tasks.size() == 2);
    CHECK(r.agentConv.size() == 2);

    // Après redémarrage : processus disparus, aucun tour terminé → tâches renvoyées.
    for (auto& [id, c] : h.convs) c.st.busy = false;
    r.resume(h);
    CHECK(r.state == OrchState::Working);
    r.tick(h);
    CHECK(h.convs["conv:01 Modèle"].received.size() == 2);
    CHECK(h.convs["conv:02 Build"].received.size() == 2);
}

// ---------------------------------------------------------------------------
// Modèles, questions, budgets
// ---------------------------------------------------------------------------

TEST_CASE("orchestrateur : modèle et effort par délégation") {
    Directives d = parseDirectives("@@DELEGUER 04 Simulateur | modele=Opus effort=high\nSpike.\n@@FIN\n"
                                   "@@DELEGUER 02 Build | modèle=haiku, effort=low\nDoc.\n@@FIN\n"
                                   "@@DELEGUER 01 Modèle\nYAML.\n@@FIN");
    REQUIRE(d.delegations.size() == 3);
    CHECK(d.delegations[0].agent == "04 Simulateur");
    CHECK(d.delegations[0].model == "opus");
    CHECK(d.delegations[0].effort == "high");
    CHECK(d.delegations[1].model == "haiku");
    CHECK(d.delegations[1].effort == "low");
    CHECK(d.delegations[2].model.empty());

    std::vector<std::string> all = {"haiku", "sonnet", "opus", "fable"};
    std::vector<std::string> eco = {"haiku", "sonnet"};
    CHECK(clampModel("opus", all) == "opus");
    CHECK(clampModel("", all).empty());
    CHECK(clampModel("opus", eco) == "sonnet");  // plafonné au plus capable autorisé
    CHECK(clampModel("claude-opus-5-5", eco) == "sonnet");
    CHECK(clampModel("", eco) == "sonnet");  // liste restreinte : jamais le défaut du CLI
    CHECK(clampModel("haiku", {"sonnet", "opus"}) == "sonnet");  // rien en dessous : le plus modeste
    CHECK(clampEffort("max", "high") == "high");
    CHECK(clampEffort("low", "high") == "low");
    CHECK(clampEffort("xhigh", "") == "xhigh");
    CHECK(clampEffort("bizarre", "high").empty());

    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.allowedModels = eco;
    o.settings.maxEffort = "medium";
    o.settings.defaultWorkerModel = "haiku";
    REQUIRE(o.start(h));
    CHECK(h.convs["brain"].last.model == "sonnet");  // orchestrateur : le plus capable autorisé
    h.answer("brain", "@@DELEGUER 01 | modele=opus effort=max\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN");
    o.tick(h);
    CHECK(h.convs["conv:01 Modèle"].last.model == "sonnet");
    CHECK(h.convs["conv:01 Modèle"].last.effort == "medium");
    CHECK(h.convs["conv:02 Build"].last.model == "haiku");  // défaut des agents
    CHECK(o.tasks[0].model == "sonnet");
}

TEST_CASE("orchestrateur : questions à options") {
    Directives d = parseDirectives("@@HUMAIN\nQuel UART pour le printf ?\n- UART2 (recommandé)\n- UART4\n- LPUART\n@@FIN\n"
                                   "@@HUMAIN\nOn publie sur GitHub ?\n@@FIN");
    REQUIRE(d.questions.size() == 2);
    CHECK(d.questions[0].text == "Quel UART pour le printf ?");
    REQUIRE(d.questions[0].options.size() == 3);
    CHECK(d.questions[0].options[0] == "UART2 (recommandé)");
    CHECK(d.questions[1].options.empty());
    CHECK(splitQuestion("Choix :\n1. Renode\n2) QEMU").options.size() == 2);
    CHECK(splitQuestion("Choix :\nA) Renode\nB) QEMU").options.size() == 2);
    CHECK(splitQuestion("Une seule ligne :\n- seule option").options.empty());

    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "@@HUMAIN\nQuel UART ?\n- UART2\n- UART4\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::WaitingHuman);
    CHECK(o.questions.size() == 1);
    int seq = o.questionSeq;
    CHECK(o.answerHuman(h, "Quel UART ? → UART2"));
    CHECK(o.state == OrchState::Thinking);
    CHECK(h.convs["brain"].received.back().find("UART2") != std::string::npos);
    CHECK(o.questions.empty());
    REQUIRE(o.qaHistory.size() == 1);
    CHECK(o.qaHistory[0].answer == "Quel UART ? → UART2");
    CHECK(o.questionSeq == seq);
}

TEST_CASE("orchestrateur : budget strict, phase finale puis conclusion") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.maxCostUsd = 2.0;
    o.settings.budgetTolerance = 0;  // sans marge : arrêt pile à la cible
    o.settings.maxParallel = 1;
    REQUIRE(o.start(h));
    CHECK_FALSE(o.subscription);
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN", 0.5);
    o.tick(h);
    h.answer("conv:01 Modèle", "ok", 1.6);  // 2,1 $ : budget dépassé, la tâche 02 ne part pas
    o.tick(h);
    CHECK(h.convs.count("conv:02 Build") == 0);
    CHECK(o.tasks[1].failed);
    CHECK(o.finalPhase());
    CHECK(o.state == OrchState::Thinking);
    CHECK(h.convs["brain"].received.back().find("BUDGET ÉPUISÉ") != std::string::npos);

    // L'orchestrateur tente encore de déléguer : pause, rien n'est envoyé.
    h.answer("brain", "@@DELEGUER 02\nb\n@@FIN", 0.1);
    o.tick(h);
    CHECK(o.state == OrchState::Paused);
    CHECK(h.convs.count("conv:02 Build") == 0);
}

TEST_CASE("orchestrateur : phase finale annoncée à 75 % du budget") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.maxCostUsd = 2.0;
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN", 0.5);
    o.tick(h);
    h.answer("conv:01 Modèle", "ok", 1.0);  // 1,5 / 2 $
    o.tick(h);
    CHECK(h.convs["brain"].received.back().find("Phase finale") != std::string::npos);
    CHECK_FALSE(o.finalPhase());
}

TEST_CASE("orchestrateur : budget en part de ce qui reste dans la session") {
    FakeHost h;
    h.sub = true;
    h.usage = 0.30;  // 60 points restent avant le plafond de 90 %
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.sessionShare = 0.5;  // la moitié de ce qui reste : 30 points
    o.settings.strictBudget = false;
    REQUIRE(o.start(h));
    CHECK(o.subscription);
    CHECK(o.sessionAllowance() == doctest::Approx(0.30));
    CHECK(o.budgetFraction() == doctest::Approx(0.0));
    CHECK(h.convs["brain"].received[0].find("ce qui restait") != std::string::npos);
    h.usage = 0.38;
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    o.tick(h);
    CHECK(o.windowSpent == doctest::Approx(0.08));
    CHECK(o.budgetFraction() == doctest::Approx(0.08 / 0.30));
    CHECK(o.budgetLabel().find("session de 5 h") != std::string::npos);
    h.usage = 0.62;  // 32 points sur 30 : cible dépassée, mais dans la tolérance (+15 %) — on continue
    h.answer("conv:01 Modèle", "ok");
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    CHECK(h.convs["brain"].received.back().find("Cible atteinte") != std::string::npos);
    h.answer("brain", "@@DELEGUER 01\nfinir\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::Working);
    h.usage = 0.66;  // 36 points sur 30 : au-delà de la tolérance
    h.answer("conv:01 Modèle", "ok");
    o.tick(h);
    CHECK(o.state == OrchState::UsagePause);  // abonnement : on attend la session suivante
    CHECK(o.pauseReason.find("prochaine session") != std::string::npos);

    // Nouvelle session de 5 h : budget renouvelé sur ce qui reste, l'orchestration repart seule.
    h.usage = 0.04;
    o.tick(h);
    CHECK(o.sessionAllowance() == doctest::Approx(0.43));  // moitié des 86 points qui restent
    CHECK(o.windowSpent == doctest::Approx(0.0));           // nouvelle référence : l'usage actuel de la session
    CHECK(o.usageSpent == doctest::Approx(0.36));           // total toutes sessions
    CHECK(o.state == OrchState::Working);

    // Forcer le mode API malgré l'abonnement, ou l'inverse
    o.settings.budgetMode = "api";
    CHECK_FALSE(Orchestrator::resolveSubscription(o.settings, true));
    o.settings.budgetMode = "subscription";
    CHECK(Orchestrator::resolveSubscription(o.settings, false));
}

TEST_CASE("orchestrateur : niveau bas en fin de session, sans fausse alerte") {
    // Cas réel : 13 % utilisés dans une session neuve, niveau « bas » (25 % du restant).
    FakeHost h;
    h.sub = true;
    h.usage = 0.13;
    Orchestrator o;
    o.settings.objective = "x";
    applyBudgetPreset(o.settings, BudgetPreset::Low);
    REQUIRE(o.start(h));
    CHECK(o.sessionAllowance() == doctest::Approx(0.25 * 0.77));
    CHECK(o.budgetFraction() < 0.01);  // rien de consommé : pas d'alerte
}

TEST_CASE("orchestrateur : abonnement détecté après le départ") {
    FakeHost h;  // pas encore de mesure : l'abonnement est inconnu au départ
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    CHECK_FALSE(o.subscription);
    h.sub = true;
    h.usage = 0.40;
    o.tick(h);
    CHECK(o.subscription);
    h.usage = 0.45;
    o.tick(h);
    CHECK(o.usageSpent == doctest::Approx(0.05));

    Orchestrator forced;  // mode API imposé : pas de bascule
    forced.settings.objective = "x";
    forced.settings.budgetMode = "api";
    FakeHost h2;
    REQUIRE(forced.start(h2));
    h2.sub = true;
    forced.tick(h2);
    CHECK_FALSE(forced.subscription);
}

TEST_CASE("orchestrateur : plafond hebdomadaire") {
    FakeHost h;
    h.weekly = 0.97;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    CHECK(o.state == OrchState::UsagePause);
    CHECK(o.pauseReason.find("hebdomadaire") != std::string::npos);
    h.weekly = 0.2;  // nouvelle semaine
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    CHECK(h.convs["brain"].received.size() == 1);
}

// ---------------------------------------------------------------------------
// Questions réelles, niveaux de budget, messages au cerveau
// ---------------------------------------------------------------------------

TEST_CASE("orchestrateur : deux questions numérotées avec sous-puces (cas réel)") {
    std::string body =
        "Deux décisions sont toujours en attente. Elles bloquent la compilation des vrais exemples du SDK Alif.\n"
        "1. **Licences des packs CMSIS.** `cpackget` attend une acceptation pour trois packs :\n"
        "   - ARM::CMSIS 6.1.0 et ARM::CMSIS-Compiler 2.1.0, sous licence Apache-2.0 ;\n"
        "   - AlifSemiconductor::Ensemble 2.2.1, sous licence propriétaire.\n"
        "Les textes sont dans `tools\\packs`. Soit tu lances toi-même le script, soit tu me réponds explicitement "
        "« j'accepte les licences des packs » et je le ferai lancer par l'agent 02.\n"
        "2. **Version de GCC** : 14.3.1, celle que nous avons fixée, ou 13.x, celle du template Alif ? "
        "Si personne ne compile encore pour la carte, je recommande de garder 14.3.1.";
    Directives d = parseDirectives("Point d'étape.\n@@HUMAIN\n" + body + "\n@@FIN");
    REQUIRE(d.questions.size() == 2);
    CHECK(d.questions[0].options.empty());  // les sous-puces sont du contexte, pas des choix
    CHECK(d.questions[0].title.find("Licences des packs CMSIS.") == 0);
    CHECK(d.questions[0].text.find("Deux décisions") == 0);  // contexte commun en tête de la 1re question
    CHECK(d.questions[0].text.find("ARM::CMSIS 6.1.0") != std::string::npos);
    REQUIRE(d.questions[0].suggestions.size() == 1);
    CHECK(d.questions[0].suggestions[0] == "j'accepte les licences des packs");
    CHECK(d.questions[1].title.find("Version de GCC") == 0);
    CHECK(d.questions[1].options.empty());

    // Courtes lignes numérotées sans question : ce sont des choix
    CHECK(splitQuestions("Quel simulateur ?\n1. Renode\n2. QEMU").size() == 1);
    CHECK(splitQuestions("Quel simulateur ?\n1. Renode\n2. QEMU")[0].options.size() == 2);
    // Puces de contexte suivies de texte : pas des choix
    CHECK(splitQuestion("État :\n- build OK\n- tests KO\nOn continue ?").options.empty());
    // Cases explicites : des choix, même suivies de texte
    HumanQuestion q = splitQuestion("Quel UART ?\n- [ ] UART2 (recommandé)\n- [ ] UART4\nPrécise si autre.");
    REQUIRE(q.options.size() == 2);
    CHECK(q.options[0] == "UART2 (recommandé)");
    CHECK(q.title == "Quel UART ?");
}

TEST_CASE("orchestrateur : niveaux de budget") {
    OrchestratorSettings s;
    s.objective = "garder";
    s.workerMode = "acceptEdits";
    for (BudgetPreset p : kBudgetPresets) {
        applyBudgetPreset(s, p);
        CHECK(budgetPresetMatches(s, p));
        CHECK(s.objective == "garder");
        CHECK(s.workerMode == "acceptEdits");
        for (BudgetPreset other : kBudgetPresets)
            if (other != p) CHECK_FALSE(budgetPresetMatches(s, other));
        CHECK_FALSE(budgetPresetSummary(p, true).empty());
    }
    applyBudgetPreset(s, BudgetPreset::Low);
    CHECK(s.sessionShare == doctest::Approx(0.25));
    CHECK(clampModel("opus", s.allowedModels) == "sonnet");
    s.maxParallel = 5;  // réglage avancé modifié : plus aucun niveau ne correspond (« personnalisé »)
    for (BudgetPreset p : kBudgetPresets) CHECK_FALSE(budgetPresetMatches(s, p));
    applyBudgetPreset(s, BudgetPreset::Max);
    CHECK(s.sessionShare == 0.0);  // sans limite, en enchaînant les sessions
    CHECK(std::string(budgetPresetLabel(BudgetPreset::Max, true)) == "Max abonnement");
    CHECK(budgetPresetSummary(BudgetPreset::Low, false).find("$") != std::string::npos);
}

TEST_CASE("orchestrateur : le cerveau connaît le budget et le quota réels") {
    FakeHost h;
    h.sub = true;
    h.usage = 0.87;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.sessionShare = 0;  // sans limite
    REQUIRE(o.start(h));
    const std::string& first = h.convs["brain"].received[0];
    CHECK(first.find("aucune limite fixée") != std::string::npos);
    CHECK(first.find("Budget : 0 % de la fenêtre") == std::string::npos);  // l'ancien libellé trompeur
    CHECK(first.find("fenêtre de 5 h à 87 %") != std::string::npos);
    CHECK(first.find("Marge faible") != std::string::npos);
    CHECK(first.find("doit tenir dans ce budget") == std::string::npos);  // pas de budget : pas de contrainte annoncée
    CHECK(first.find("optimise dans tous les cas") != std::string::npos);  // même sans limite, on économise
    CHECK(Orchestrator::systemPrompt().find("PLAFOND, pas un objectif") != std::string::npos);

    // Même au niveau maximal, les agents travaillent par défaut sur un modèle économique.
    OrchestratorSettings mx;
    applyBudgetPreset(mx, BudgetPreset::Max);
    CHECK(mx.defaultWorkerModel == "sonnet");
}

TEST_CASE("orchestrateur : relance et reprise signalées au cerveau") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "@@TERMINE\nfini\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::Finished);
    REQUIRE(o.start(h));  // relance dans la même conversation
    CHECK(h.convs["brain"].received.back().find("Relance de l'orchestration") == 2);

    // Fermeture de ClaudeDesk pendant la réflexion, puis reprise : message renvoyé une seule fois annoté.
    Orchestrator r;
    r.fromJson(o.toJson());
    h.convs["brain"].st.busy = false;
    r.resume(h);
    CHECK(h.convs["brain"].received.back().find("Reprise après interruption") == 2);
    Orchestrator r2;
    r2.fromJson(r.toJson());
    h.convs["brain"].st.busy = false;
    r2.resume(h);
    const std::string& again = h.convs["brain"].received.back();
    CHECK(again.find("Reprise après interruption") == 2);
    CHECK(again.find("Reprise après interruption", 10) == std::string::npos);
}

TEST_CASE("orchestrateur : segments d'affichage et messages de ClaudeDesk") {
    auto segs = splitDirectiveSegments("Analyse.\n\n@@DELEGUER 02 Build | modele=sonnet\nInstalle.\n@@FIN\n"
                                       "Puis :\n@@HUMAIN\nQuel UART ?\n- [ ] UART2\n@@FIN\n@@TERMINE\nFini.");
    REQUIRE(segs.size() == 5);
    CHECK(segs[0].kind == TextSegment::Text);
    CHECK(segs[0].body == "Analyse.");
    CHECK(segs[1].kind == TextSegment::Delegate);
    CHECK(segs[1].header == "02 Build | modele=sonnet");
    CHECK(segs[1].body == "Installe.");
    CHECK(segs[2].body == "Puis :");
    CHECK(segs[3].kind == TextSegment::Human);
    CHECK(segs[4].kind == TextSegment::Done);
    CHECK(segs[4].body == "Fini.");
    CHECK(splitDirectiveSegments("Pas de directive").size() == 1);

    CHECK(orchestratorMessageTitle("# Objectif\nx") == "Mission : objectif, équipe et moyens");
    CHECK(orchestratorMessageTitle("# Comptes rendus — cycle 3\n\n## 02") == "Comptes rendus de l'équipe — cycle 3");
    CHECK(orchestratorMessageTitle("_(Reprise après interruption : …)_\n\n# Objectif") == "Reprise après interruption");
    CHECK(orchestratorMessageTitle("Peux-tu accélérer ?").empty());
}

TEST_CASE("orchestrateur : tolérance de dépassement pour finir sans bâcler") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.maxCostUsd = 2.0;  // tolérance par défaut : +15 % → arrêt ferme à 2,30 $
    o.settings.maxParallel = 1;
    REQUIRE(o.start(h));
    CHECK(h.convs["brain"].received[0].find("Tolérance") != std::string::npos);
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN", 0.5);
    o.tick(h);
    h.answer("conv:01 Modèle", "ok", 1.6);  // 2,10 $ : cible dépassée, dans la tolérance → la tâche 02 part quand même
    o.tick(h);
    CHECK(h.convs["conv:02 Build"].received.size() == 1);
    CHECK_FALSE(o.finalPhase());
    h.answer("conv:02 Build", "ok", 0.3);  // 2,40 $ : au-delà de la tolérance
    o.tick(h);
    CHECK(o.finalPhase());
    CHECK(h.convs["brain"].received.back().find("BUDGET ÉPUISÉ") != std::string::npos);

    // Le message « Phase finale » rappelle la marge et demande de réduire le périmètre, pas la qualité.
    Orchestrator p;
    p.settings.objective = "x";
    p.settings.maxCostUsd = 2.0;
    FakeHost h2;
    REQUIRE(p.start(h2));
    h2.answer("brain", "@@DELEGUER 01\na\n@@FIN", 0.5);
    p.tick(h2);
    h2.answer("conv:01 Modèle", "ok", 1.0);
    p.tick(h2);
    const std::string& m = h2.convs["brain"].received.back();
    CHECK(m.find("Phase finale") != std::string::npos);
    CHECK(m.find("plutôt que la qualité") != std::string::npos);
    CHECK(m.find("sans te précipiter") != std::string::npos);
}

TEST_CASE("orchestrateur : la consommation des autres n'est pas comptée, la part suit le restant") {
    // Cas réel : l'orchestration attend votre réponse ; la mesure passe de 13 % (périmée) à 25 %.
    FakeHost h;
    h.sub = true;
    h.usage = 0.13;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.sessionShare = 0.5;
    REQUIRE(o.start(h));
    h.answer("brain", "@@HUMAIN\nQuel UART ?\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::WaitingHuman);
    h.t += 120;  // l'équipe est au repos depuis 2 min
    h.usage = 0.25;
    o.tick(h);
    CHECK(o.windowSpent == doctest::Approx(0.0));                // rien n'est attribué à l'orchestration
    CHECK(o.sessionAllowance() == doctest::Approx(0.5 * 0.65));  // la part suit ce qui reste réellement
    CHECK(o.budgetFraction() == doctest::Approx(0.0));
    // Changer de niveau met la part à jour immédiatement.
    applyBudgetPreset(o.settings, BudgetPreset::Low);
    CHECK(o.sessionAllowance() == doctest::Approx(0.25 * 0.65));
}

TEST_CASE("orchestrateur : relancer le budget garde la limite, plafonnée par ce qui reste") {
    // Exemple de l'utilisateur : 25 consommés sur une limite de 50 ; relance → 0 / 50 ;
    // si l'usage total de la session atteint ensuite 80 %, la limite tombe à 20 (ce qui reste).
    FakeHost h;
    h.sub = true;
    h.usage = 0.0;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.usagePause = 0;  // pas de plafond : la session entière
    o.settings.weeklyPause = 0;
    o.settings.sessionShare = 0.5;
    REQUIRE(o.start(h));
    CHECK(o.sessionAllowance() == doctest::Approx(0.50));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    h.usage = 0.25;
    o.tick(h);
    CHECK(o.windowSpent == doctest::Approx(0.25));
    o.resetBudget(h);
    CHECK(o.windowSpent == 0.0);
    CHECK(o.sessionAllowance() == doctest::Approx(0.50));  // la limite reste 50
    CHECK(o.budgetFraction() == doctest::Approx(0.0));
    // Consommation ailleurs pendant que l'équipe est au repos : la session n'a plus que 20 % → limite 20
    h.convs["conv:01 Modèle"].st.busy = false;
    h.t += 120;
    h.usage = 0.80;
    o.tick(h);
    CHECK(o.sessionAllowance() == doctest::Approx(0.20));
    // Changer de niveau annule la limite figée
    o.settings.sessionShare = 0.25;
    CHECK(o.sessionAllowance() == doctest::Approx(0.25 * 0.20));
}

TEST_CASE("orchestrateur : relancer le budget en API") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.maxCostUsd = 2.0;
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN", 1.5);
    o.tick(h);
    CHECK(o.budgetFraction() == doctest::Approx(0.75));
    o.resetBudget(h);
    CHECK(o.budgetFraction() == doctest::Approx(0.0));
    CHECK(o.spentUsd == doctest::Approx(1.5));  // le total reste affiché
    h.answer("conv:01 Modèle", "ok", 0.5);
    o.tick(h);
    CHECK(o.budgetSpentUsd() == doctest::Approx(0.5));
}

TEST_CASE("orchestrateur : une petite baisse de la mesure n'est pas une nouvelle session") {
    // Cas réel : en plein cycle, une mesure légèrement plus basse faisait repartir le budget de zéro et
    // attribuait toute la session (43 %) à l'orchestration.
    FakeHost h;
    h.sub = true;
    h.usage = 0.30;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.sessionShare = 0.5;
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN");
    h.usage = 0.36;
    o.tick(h);
    CHECK(o.windowSpent == doctest::Approx(0.06));
    h.usage = 0.35;  // mesure reçue dans le désordre
    o.tick(h);
    CHECK(o.windowSpent == doctest::Approx(0.06));
    CHECK(o.windowBase == doctest::Approx(0.30));  // la référence de départ est conservée
    h.usage = 0.40;
    o.tick(h);
    CHECK(o.windowSpent == doctest::Approx(0.10));  // écart depuis le départ, sans double comptage
    // Vraie nouvelle session : forte chute de la mesure (ou date de réinitialisation qui change)
    h.usage = 0.02;
    o.tick(h);
    CHECK(o.windowBase == doctest::Approx(0.02));
    CHECK(o.windowSpent == doctest::Approx(0.0));
}

TEST_CASE("orchestrateur : rappel quand le modèle n'est pas choisi, niveau moyen") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    applyBudgetPreset(o.settings, BudgetPreset::Medium);
    CHECK(o.settings.brainModel == "opus");  // l'orchestrateur décide : modèle capable
    CHECK(o.settings.defaultWorkerModel == "sonnet");
    REQUIRE(o.start(h));
    CHECK(h.convs["brain"].last.model == "opus");
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02 | modele=haiku effort=low\nb\n@@FIN");
    o.tick(h);
    CHECK(h.convs["conv:02 Build"].last.model == "haiku");
    h.answer("conv:01 Modèle", "ok");
    h.answer("conv:02 Build", "ok");
    o.tick(h);
    const std::string& rep = h.convs["brain"].received.back();
    CHECK(rep.find("tu n'as pas choisi de modèle pour 01 Modèle") != std::string::npos);
    CHECK(rep.find("pour 01 Modèle — le défaut") != std::string::npos);  // 02 avait un modèle choisi : absent du rappel
}

TEST_CASE("orchestrateur : contexte allégé par récap + nouvelle conversation") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "Construire E7 Twin";
    o.settings.rotateAtTokens = 120000;
    o.settings.rotateAfterTasks = 2;
    REQUIRE(o.start(h));
    // Cycle 1 : une tâche pour 01
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    o.tick(h);
    REQUIRE(h.convs["conv:01 Modèle"].received.size() == 1);
    h.convs["conv:01 Modèle"].st.contextTokens = 180000;  // la conversation de l'agent a beaucoup grossi
    h.answer("conv:01 Modèle", "ok");
    o.tick(h);
    // Cycle 2 : nouvelle tâche pour 01 → récap d'abord (en cours), puis nouvelle conversation
    h.answer("brain", "@@DELEGUER 01\nb\n@@FIN");
    o.tick(h);
    CHECK(h.convs["conv:01 Modèle"].received.size() == 1);  // rien n'est envoyé à l'ancienne conversation
    CHECK(o.state == OrchState::Working);
    o.tick(h);  // le récap est prêt
    REQUIRE(h.convs.count("conv:01 Modèle#1"));
    CHECK(h.convs["conv:01 Modèle#1"].received.size() == 1);
    CHECK(o.agentConv["01 Modèle"] == "conv:01 Modèle#1");
    bool logged = false;
    for (auto& l : o.log) logged |= l.text.find("contexte allégé") != std::string::npos;
    CHECK(logged);

    // L'orchestrateur aussi : son contexte chargé → nouvelle conversation, avec un rappel de l'objectif.
    h.convs["brain"].st.contextTokens = 150000;
    h.answer("conv:01 Modèle#1", "ok");
    o.tick(h);  // récap de l'orchestrateur en cours
    o.tick(h);
    CHECK(o.brainConvId == "brain#2");
    REQUIRE(h.convs["brain#2"].received.size() == 1);
    CHECK(h.convs["brain#2"].received[0].find("Contexte allégé") != std::string::npos);
    CHECK(h.convs["brain#2"].received[0].find("Construire E7 Twin") != std::string::npos);
    CHECK(h.convs["brain#2"].received[0].find("Comptes rendus") != std::string::npos);
}

TEST_CASE("orchestrateur : « Repartir du récap » — le cerveau et l'agent suivent la nouvelle conversation") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "Construire E7 Twin";
    o.settings.contextRotation = false;
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    o.tick(h);
    REQUIRE(o.agentConv["01 Modèle"] == "conv:01 Modèle");
    // L'utilisateur remet à zéro la conversation de l'agent, puis celle de l'orchestrateur.
    o.conversationReplaced("conv:01 Modèle", "neuve-01");
    CHECK(o.agentConv["01 Modèle"] == "neuve-01");
    o.conversationReplaced("brain", "brain-neuf");
    CHECK(o.brainConvId == "brain-neuf");
    // Le rappel attendu survit à un rechargement.
    Orchestrator o2;
    o2.fromJson(o.toJson());
    CHECK(o2.toJson().value("brainRefresh", false));
    // La tâche en vol (ancienne conversation) se termine : le compte rendu part au nouveau cerveau, avec le rappel.
    h.answer("conv:01 Modèle", "ok");
    o.tick(h);
    REQUIRE(h.convs["brain-neuf"].received.size() == 1);
    const std::string& first = h.convs["brain-neuf"].received[0];
    CHECK(first.find("Contexte allégé") != std::string::npos);
    CHECK(first.find("Construire E7 Twin") != std::string::npos);
    CHECK_FALSE(o.toJson().value("brainRefresh", true));  // une seule fois
}

TEST_CASE("orchestrateur : rotation après N tâches, et désactivable") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.rotateAfterTasks = 1;
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    o.tick(h);
    h.answer("conv:01 Modèle", "ok");
    o.tick(h);
    h.answer("brain", "@@DELEGUER 01\nb\n@@FIN");
    o.tick(h);
    o.tick(h);
    CHECK(o.agentConv["01 Modèle"] == "conv:01 Modèle#1");

    FakeHost h2;
    Orchestrator off;
    off.settings.objective = "x";
    off.settings.contextRotation = false;
    REQUIRE(off.start(h2));
    h2.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    off.tick(h2);
    h2.convs["conv:01 Modèle"].st.contextTokens = 900000;
    h2.answer("conv:01 Modèle", "ok");
    off.tick(h2);
    h2.answer("brain", "@@DELEGUER 01\nb\n@@FIN");
    off.tick(h2);
    CHECK(h2.convs["conv:01 Modèle"].received.size() == 2);  // même conversation
}

TEST_CASE("orchestrateur : le plafond du compte suspend, il ne fait pas conclure") {
    // Cas réel : compte à 97 % (plafond 90 %) → la part restante tombait à 0 et l'orchestrateur devait conclure
    // « objectif non atteint ». Il doit au contraire attendre la prochaine session de 5 h.
    FakeHost h;
    h.sub = true;
    h.usage = 0.30;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.sessionShare = 0.5;
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    o.tick(h);
    h.answer("conv:01 Modèle", "ok");
    h.usage = 0.97;
    o.tick(h);
    CHECK(o.state == OrchState::UsagePause);
    CHECK_FALSE(o.finalPhase());
    CHECK(h.convs["brain"].received.size() == 1);  // aucun « BUDGET ÉPUISÉ » envoyé
    // Nouvelle session : reprise, budget renouvelé, comptes rendus envoyés normalement.
    h.usage = 0.03;
    o.tick(h);  // reprise (état Working) …
    o.tick(h);  // … puis envoi des comptes rendus
    CHECK(o.state == OrchState::Thinking);
    CHECK(h.convs["brain"].received.back().find("Comptes rendus") != std::string::npos);
    CHECK(h.convs["brain"].received.back().find("BUDGET ÉPUISÉ") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Comptes rendus au fil de l'eau
// ---------------------------------------------------------------------------

TEST_CASE("orchestrateur : un agent termine, l'orchestrateur relance pendant que les autres travaillent") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.maxParallel = 3;
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::Working);
    CHECK(o.tasksInFlight() == 2);

    // 01 termine, 02 travaille encore : compte rendu intermédiaire immédiat.
    h.answer("conv:01 Modèle", "Fait : A.");
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    REQUIRE(h.convs["brain"].received.size() == 2);
    const std::string rep = h.convs["brain"].received[1];
    CHECK(rep.find("# Compte rendu intermédiaire") == 0);
    CHECK(rep.find("Fait : A.") != std::string::npos);
    CHECK(rep.find("Toujours en cours") != std::string::npos);
    CHECK(rep.find("02 Build") != std::string::npos);
    CHECK(orchestratorMessageTitle(rep).find("Compte rendu intermédiaire") == 0);

    // Pendant que le cerveau réfléchit, 02 termine : relevé, rapporté plus tard.
    h.answer("conv:02 Build", "Fait : B.");
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    CHECK(o.tasksInFlight() == 0);

    // Le cerveau relance 03 : la tâche part, 01 (déjà rapportée) disparaît, 02 attend son compte rendu.
    h.answer("brain", "@@DELEGUER 03 | modele=haiku\nc\n@@FIN");
    o.tick(h);
    CHECK(o.state == OrchState::Working);
    CHECK(o.cycle == 2);
    CHECK(h.convs["conv:03 Check"].received.size() == 1);
    REQUIRE(o.tasks.size() == 2);
    CHECK(o.tasks[0].agent == "02 Build");
    CHECK_FALSE(o.tasks[0].reported);

    // 02 non rapporté + 03 au travail : nouveau compte rendu intermédiaire avec B.
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    CHECK(h.convs["brain"].received.back().find("Fait : B.") != std::string::npos);
    CHECK(h.convs["brain"].received.back().find("03 Check") != std::string::npos);

    // @@ATTENDRE : on attend 03, sans relance « aucune directive ».
    h.answer("brain", "Rien d'indépendant à lancer.\n@@ATTENDRE\n03 doit finir.\n@@FIN");
    o.tick(h);
    CHECK(o.state == OrchState::Working);
    size_t n = h.convs["brain"].received.size();
    o.tick(h);
    CHECK(h.convs["brain"].received.size() == n);

    // 03 termine : plus personne au travail → comptes rendus complets (seulement le nouveau).
    h.answer("conv:03 Check", "Fait : C.");
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    const std::string full = h.convs["brain"].received.back();
    CHECK(full.find("# Comptes rendus") == 0);
    CHECK(full.find("Fait : C.") != std::string::npos);
    CHECK(full.find("Fait : B.") == std::string::npos);

    // @@ATTENDRE alors que plus rien ne tourne : relance.
    h.answer("brain", "@@ATTENDRE\n@@FIN");
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    CHECK(h.convs["brain"].received.back().find("aucune directive") != std::string::npos);
}

TEST_CASE("orchestrateur : pas de compte rendu intermédiaire inutile") {
    SUBCASE("option désactivée : on attend toute l'équipe") {
        FakeHost h;
        Orchestrator o;
        o.settings.objective = "x";
        o.settings.rollingReports = false;
        REQUIRE(o.start(h));
        h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN");
        o.tick(h);
        h.answer("conv:01 Modèle", "A");
        o.tick(h);
        CHECK(o.state == OrchState::Working);
        CHECK(h.convs["brain"].received.size() == 1);
        h.answer("conv:02 Build", "B");
        o.tick(h);
        CHECK(o.state == OrchState::Thinking);
        CHECK(h.convs["brain"].received.back().find("# Comptes rendus") == 0);
    }
    SUBCASE("aucune place libre (parallélisme atteint par la file)") {
        FakeHost h;
        Orchestrator o;
        o.settings.objective = "x";
        o.settings.maxParallel = 1;
        REQUIRE(o.start(h));
        h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN");
        o.tick(h);
        h.answer("conv:01 Modèle", "A");
        o.tick(h);  // 02 part aussitôt : la seule place est prise
        CHECK(o.state == OrchState::Working);
        CHECK(h.convs["brain"].received.size() == 1);
    }
    SUBCASE("tous les agents occupés") {
        FakeHost h;
        h.agentList = {{"01 Modèle", ""}, {"02 Build", ""}};
        Orchestrator o;
        o.settings.objective = "x";
        o.settings.maxParallel = 3;
        REQUIRE(o.start(h));
        // Deux tâches pour 01 (la seconde en file) et une pour 02.
        h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN\n@@DELEGUER 01\na2\n@@FIN");
        o.tick(h);
        h.answer("conv:01 Modèle", "A");
        o.tick(h);  // a2 part sur 01, 02 travaille : plus d'agent libre
        CHECK(h.convs["conv:01 Modèle"].received.size() == 2);
        CHECK(o.state == OrchState::Working);
        CHECK(h.convs["brain"].received.size() == 1);
    }
}

TEST_CASE("orchestrateur : l'équipe continue pendant une question, la réponse joint l'avancement") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN\n@@DELEGUER 02\nb\n@@FIN");
    o.tick(h);
    h.answer("conv:01 Modèle", "A");
    o.tick(h);  // compte rendu intermédiaire
    h.answer("brain", "@@HUMAIN\nOn garde UART2 ?\n- [ ] Oui\n- [ ] Non\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::WaitingHuman);
    h.answer("conv:02 Build", "Fait : B pendant la question.");
    o.tick(h);
    CHECK(o.state == OrchState::WaitingHuman);
    CHECK(o.tasksInFlight() == 0);

    // Sauvegarde / rechargement : le compte rendu non transmis est conservé.
    Orchestrator r;
    r.fromJson(o.toJson());
    CHECK(r.settings.rollingReports);
    REQUIRE(r.tasks.size() == 2);
    CHECK(r.tasks[0].reported);
    CHECK_FALSE(r.tasks[1].reported);

    REQUIRE(o.answerHuman(h, "Oui"));
    const std::string msg = h.convs["brain"].received.back();
    CHECK(msg.find("Oui") != std::string::npos);
    CHECK(msg.find("Fait : B pendant la question.") != std::string::npos);
    CHECK(o.tasks[1].reported);
}

TEST_CASE("orchestrateur : @@ATTENDRE analysé et affiché") {
    Directives d = parseDirectives("Je patiente.\n@@ATTENDRE\n02 doit finir.\n@@FIN");
    CHECK(d.wait);
    CHECK(d.empty());
    auto segs = splitDirectiveSegments("Texte\n@@ATTENDRE\nraison\n@@FIN");
    REQUIRE(segs.size() == 2);
    CHECK(segs[1].kind == TextSegment::Wait);
    CHECK(segs[1].body == "raison");
}

// ---------------------------------------------------------------------------
// Pause décidée par l'orchestrateur
// ---------------------------------------------------------------------------

TEST_CASE("orchestrateur : annonce de pause reconnue, vraie question conservée") {
    const std::string notice =
        "Orchestration en pause : le quota de la fenêtre de 5 h est presque épuisé.\n"
        "Relance-moi après la réinitialisation du quota (dans environ 4 h 20). Le prochain cycle est prévu :\n"
        "- **04** : terminer le stub MHU ;\n- **02** : terminer `build/package`.";
    CHECK(looksLikePauseNotice(notice));
    CHECK_FALSE(looksLikePauseNotice("On met en pause le dashboard ?"));
    CHECK_FALSE(looksLikePauseNotice("Quel UART ?\n- [ ] UART2\n- [ ] UART4"));
    CHECK_FALSE(looksLikePauseNotice("Installe GCC 14 puis préviens-moi."));

    Directives d = parseDirectives("Bilan.\n@@HUMAIN\n" + notice + "\n@@FIN");
    CHECK(d.pause);
    CHECK_FALSE(d.needHuman);
    CHECK(d.pauseNote.find("Relance-moi") != std::string::npos);
    Directives p = parseDirectives("@@PAUSE\nQuota épuisé, reprise : 04 puis 02.\n@@FIN");
    CHECK(p.pause);
    CHECK(p.pauseNote == "Quota épuisé, reprise : 04 puis 02.");
    CHECK_FALSE(p.empty());

    auto segs = splitDirectiveSegments("x\n@@HUMAIN\n" + notice + "\n@@FIN\n@@PAUSE\ny\n@@FIN");
    REQUIRE(segs.size() == 3);
    CHECK(segs[1].kind == TextSegment::Pause);
    CHECK(segs[2].kind == TextSegment::Pause);
}

TEST_CASE("orchestrateur : @@PAUSE suspend sans question puis reprend seul à la session suivante") {
    FakeHost h;  // fenêtre de 5 h réinitialisée à t = 5000
    h.sub = true;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "Le quota est presque épuisé.\n@@PAUSE\nReprise : 04 puis 02.\n@@FIN");
    o.tick(h);
    CHECK(o.state == OrchState::UsagePause);
    CHECK(o.brainPaused());
    CHECK(o.questions.empty());
    CHECK(o.humanQuestion.empty());
    CHECK(o.pauseUntil == 5000);
    CHECK(o.running());

    // Sauvegarde / rechargement : la pause reste automatique.
    Orchestrator r;
    r.fromJson(o.toJson());
    CHECK(r.state == OrchState::UsagePause);
    CHECK(r.pauseNote == "Reprise : 04 puis 02.");

    size_t n = h.convs["brain"].received.size();
    h.t = 3000;
    o.tick(h);
    CHECK(o.state == OrchState::UsagePause);
    CHECK(h.convs["brain"].received.size() == n);

    h.t = 6000;  // nouvelle session
    o.tick(h);
    CHECK(o.state == OrchState::Thinking);
    CHECK_FALSE(o.brainPaused());
    REQUIRE(h.convs["brain"].received.size() == n + 1);
    const std::string msg = h.convs["brain"].received.back();
    CHECK(msg.find("# Reprise après ta pause") == 0);
    CHECK(msg.find("> Reprise : 04 puis 02.") != std::string::npos);
    CHECK(msg.find("Reprise après interruption") == std::string::npos);
    CHECK(orchestratorMessageTitle(msg).find("Reprise après sa pause") == 0);
}

TEST_CASE("orchestrateur : une annonce de pause enregistrée comme question est convertie au rechargement") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "(réponse affichée comme une question)");
    // Simule l'état laissé par une version précédente : question en attente qui n'est qu'une annonce de pause.
    nlohmann::json j = o.toJson();
    j["state"] = "waitingHuman";
    j["humanQuestion"] = "Orchestration en pause : quota presque épuisé. Relance-moi après la réinitialisation du quota.";
    j["windowResetsAt"] = 5000;
    Orchestrator r;
    r.fromJson(j);
    CHECK(r.state == OrchState::UsagePause);
    CHECK(r.brainPaused());
    CHECK(r.questions.empty());
    CHECK(r.pauseUntil == 5000);
    h.t = 6000;
    r.tick(h);
    CHECK(r.state == OrchState::Thinking);
    CHECK(h.convs["brain"].received.back().find("# Reprise après ta pause") == 0);
}

// ---------------------------------------------------------------------------
// Économie de contexte et de cache (diagnostic du 2026-11-21 : contextes à 470 k, cache réécrit après les pauses
// et les changements de modèle, cerveau Opus qui lançait lui-même les tests)
// ---------------------------------------------------------------------------

TEST_CASE("économie : raisons de repartir d'une conversation neuve") {
    OrchestratorSettings s;
    ParticipantStatus st;
    st.contextTokens = 50000;
    st.lastTurnAt = 1000;
    st.model = "claude-sonnet-5-5";
    st.effort = "medium";
    const int64_t ttl = 3300;
    CHECK(rotationReason(s, st, 0, false, 1100, ttl, "sonnet", "medium").empty());
    // Seuil en jetons, quelle que soit la fenêtre du modèle
    st.contextTokens = 130000;
    CHECK(rotationReason(s, st, 0, false, 1100, ttl, "", "").find("contexte de 130 k") == 0);
    // Cache froid : seulement au-delà de la taille où réécrire coûte plus que repartir du récap
    st.contextTokens = 90000;
    CHECK(rotationReason(s, st, 0, false, 1000 + 3300, ttl, "", "").find("cache expiré") == 0);
    CHECK(rotationReason(s, st, 0, false, 1000 + 3200, ttl, "", "").empty());
    st.contextTokens = 60000;
    CHECK(rotationReason(s, st, 0, false, 1000 + 9999, ttl, "", "").empty());
    // Changement de modèle (cache propre au modèle) ou d'effort
    st.contextTokens = 70000;
    CHECK(rotationReason(s, st, 0, false, 1100, ttl, "haiku", "").find("changement de modèle") == 0);
    CHECK(rotationReason(s, st, 0, false, 1100, ttl, "sonnet", "").empty());  // alias = même modèle
    CHECK(rotationReason(s, st, 0, false, 1100, ttl, "", "high").find("changement d'effort") == 0);
    st.contextTokens = 30000;
    CHECK(rotationReason(s, st, 0, false, 1100, ttl, "haiku", "high").empty());  // petite conversation : on garde
    // Nombre de tâches : agents seulement
    CHECK(!rotationReason(s, st, 6, false, 1100, ttl, "", "").empty());
    CHECK(rotationReason(s, st, 6, true, 1100, ttl, "", "").empty());
    // Désactivations
    st.contextTokens = 90000;
    s.rotateWhenCold = false;
    CHECK(rotationReason(s, st, 0, false, 1000 + 9999, ttl, "", "").empty());
    s.rotateOnSwitch = false;
    CHECK(rotationReason(s, st, 0, false, 1100, ttl, "haiku", "").empty());
    s.contextRotation = false;
    st.contextTokens = 900000;
    CHECK(rotationReason(s, st, 0, false, 1100, ttl, "", "").empty());
    CHECK(kTokens(1234567) == "1,2 M");
    CHECK(kTokens(120000) == "120 k");
}

TEST_CASE("économie : un agent froid ou qui change de modèle repart du récap") {
    FakeHost h;
    h.sub = true;  // cache d'1 h
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01 | modele=sonnet\na\n@@FIN");
    o.tick(h);
    auto& a = h.convs["conv:01 Modèle"];
    a.st.contextTokens = 100000;
    a.st.model = "sonnet";
    h.answer("conv:01 Modèle", "ok");
    o.tick(h);
    // Deux heures plus tard (le cerveau, lui, est léger) : le cache de l'agent a expiré
    h.t += 2 * 3600;
    h.answer("brain", "@@DELEGUER 01 | modele=sonnet\nb\n@@FIN");
    o.tick(h);
    o.tick(h);
    CHECK(o.agentConv["01 Modèle"] == "conv:01 Modèle#1");
    bool cold = false;
    for (auto& l : o.log) cold |= l.text.find("cache expiré") != std::string::npos;
    CHECK(cold);

    // Changement de modèle sur une conversation chargée (cache encore chaud)
    auto& b = h.convs["conv:01 Modèle#1"];
    b.st.contextTokens = 90000;
    b.st.model = "sonnet";
    h.answer("conv:01 Modèle#1", "ok");
    o.tick(h);
    h.answer("brain", "@@DELEGUER 01 | modele=haiku effort=low\nc\n@@FIN");
    o.tick(h);
    o.tick(h);
    CHECK(o.agentConv["01 Modèle"] == "conv:01 Modèle#1#2");
    CHECK(h.convs["conv:01 Modèle#1#2"].last.model == "haiku");
}

TEST_CASE("économie : orchestrateur en lecture seule, et son contexte froid repart du récap") {
    FakeHost h;
    h.sub = true;
    Orchestrator o;
    o.settings.objective = "Construire E7 Twin";
    REQUIRE(o.start(h));
    CHECK(h.brainTools == std::vector<std::string>{"Read", "Glob", "Grep"});
    CHECK(h.brainPrompt.find("LECTURE SEULE") != std::string::npos);
    CHECK(h.brainPrompt.find("@@ROLE@@") == std::string::npos);
    CHECK(h.brainPrompt.find("@@VERIFY@@") == std::string::npos);

    h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    h.convs["brain"].st.contextTokens = 100000;  // sous le seuil de 120 k, mais au-dessus de celui du cache froid
    o.tick(h);
    // L'agent travaille longtemps : le cache du cerveau expire
    h.t += 3 * 3600;
    h.answer("conv:01 Modèle", "fini");
    o.tick(h);  // récap du cerveau en cours : le compte rendu attend
    CHECK(o.brainConvId == "brain");
    CHECK(h.convs["brain"].received.size() == 1);
    o.tick(h);
    CHECK(o.brainConvId == "brain#1");
    REQUIRE(h.convs["brain#1"].received.size() == 1);
    CHECK(h.convs["brain#1"].received[0].find("Contexte allégé") != std::string::npos);
    CHECK(h.convs["brain#1"].received[0].find("fini") != std::string::npos);

    FakeHost h2;
    Orchestrator full;
    full.settings.objective = "x";
    full.settings.brainReadOnly = false;
    REQUIRE(full.start(h2));
    CHECK(h2.brainTools.empty());
    CHECK(h2.brainPrompt.find("LECTURE SEULE") == std::string::npos);
}

TEST_CASE("économie : garde-fou d'une tâche qui s'emballe, mesures et compte rendu court") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.maxCallsPerTask = 100;
    o.settings.reportMaxChars = 1000;
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    o.tick(h);
    auto& a = h.convs["conv:01 Modèle"];
    REQUIRE(a.st.busy);
    a.st.apiCalls = 60;
    o.tick(h);
    CHECK(h.interrupted.empty());
    a.st.apiCalls = 120;
    a.st.contextTokens = 210000;
    o.tick(h);
    REQUIRE(h.interrupted.size() == 1);
    o.tick(h);
    CHECK(h.interrupted.size() == 1);  // une seule fois
    a.st.cacheReadTokens = 5000000;
    h.answer("conv:01 Modèle", "Long travail " + std::string(5000, 'x') + " **Fait** : FIN-DU-COMPTE-RENDU");
    o.tick(h);
    const std::string& rep = h.convs["brain"].received.back();
    CHECK(rep.find("INTERROMPUE") != std::string::npos);
    CHECK(rep.find("120 appels") != std::string::npos);
    CHECK(rep.find("contexte final 210 k") != std::string::npos);
    CHECK(rep.find("FIN-DU-COMPTE-RENDU") != std::string::npos);  // la fin (le compte rendu) est gardée
    CHECK(rep.find("début omis") != std::string::npos);
    CHECK(rep.size() < 3000);
}

TEST_CASE("économie : réglages persistés, ancien seuil en pourcentage ignoré") {
    Orchestrator o;
    o.settings.rotateAtTokens = 90000;
    o.settings.rotateWhenCold = false;
    o.settings.brainReadOnly = false;
    o.settings.maxCallsPerTask = 42;
    o.settings.taskContextLimit = 300000;
    o.settings.reportMaxChars = 2000;
    Orchestrator r;
    r.fromJson(o.toJson());
    CHECK(r.settings.rotateAtTokens == 90000);
    CHECK_FALSE(r.settings.rotateWhenCold);
    CHECK_FALSE(r.settings.brainReadOnly);
    CHECK(r.settings.maxCallsPerTask == 42);
    CHECK(r.settings.taskContextLimit == 300000);
    CHECK(r.settings.reportMaxChars == 2000);
    nlohmann::json old = {{"settings", {{"rotateAtContext", 0.45}}}};
    Orchestrator legacy;
    legacy.fromJson(old);
    CHECK(legacy.settings.rotateAtTokens == 120000);
}

TEST_CASE("économie : conversations allégées demandées pour les agents et le cerveau") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    CHECK(h.convs["brain"].last.lean);
    h.answer("brain", "@@DELEGUER 01\na\n@@FIN");
    o.tick(h);
    CHECK(h.convs["conv:01 Modèle"].last.lean);
    Orchestrator r;
    o.settings.leanAgents = false;
    r.fromJson(o.toJson());
    CHECK_FALSE(r.settings.leanAgents);
}

// ---------------------------------------------------------------------------
// Équipe évolutive : nouveaux agents proposés par le cerveau, toujours soumis à l'humain
// ---------------------------------------------------------------------------

namespace {
struct TeamHost : FakeHost {
    std::vector<std::pair<std::string, std::string>> created;  // nom, notes
    std::string createAgent(const std::string& name, const std::string& notes, std::string& error) override {
        for (auto& a : agentList)
            if (a.name == name) {
                error = "Un dossier porte déjà ce nom.";
                return {};
            }
        created.push_back({name, notes});
        agentList.push_back({name, notes});
        return name;
    }
};
}  // namespace

TEST_CASE("orchestrateur : directive @@NOUVEL_AGENT (variantes d'écriture)") {
    Directives d = parseDirectives(
        "Il faut une interface.\n"
        "@@NOUVEL_AGENT **09 Interface**\nPourquoi : une appli de bureau.\nMission : …\n@@FIN\n"
        "@@NOUVEL AGENT 10 Docs | inutile\nPourquoi : doc.\n@@FIN\n"
        "@@CRÉER_AGENT 11 Perf/Bench\nPourquoi : mesures.\n@@FIN\n");
    REQUIRE(d.newAgents.size() == 3);
    CHECK(d.newAgents[0].name == "09 Interface");
    CHECK(d.newAgents[0].notes == "Pourquoi : une appli de bureau.\nMission : …");
    CHECK(d.newAgents[1].name == "10 Docs");
    CHECK(d.newAgents[2].name == "11 Perf Bench");  // « / » interdit dans un nom de dossier
    CHECK_FALSE(d.empty());
    auto segs = splitDirectiveSegments("@@NOUVEL_AGENT 09 Interface\nPourquoi : x\n@@FIN");
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].kind == TextSegment::NewAgent);
    CHECK(cleanAgentName("  `a:b`.  ") == "a b");
}

TEST_CASE("orchestrateur : un nouvel agent n'est créé qu'avec l'accord de l'humain") {
    TeamHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    CHECK(h.brainPrompt.find("@@NOUVEL_AGENT") != std::string::npos);
    CHECK(h.convs["brain"].received[0].find("proposer un nouvel agent") != std::string::npos);

    // Proposition + tâche pour un agent existant + tâche (prématurée) pour l'agent proposé.
    h.answer("brain",
             "@@NOUVEL_AGENT 09 Interface\nPourquoi : une appli de bureau distincte.\nMission : l'UI.\n@@FIN\n"
             "@@DELEGUER 02 Build | modele=haiku\nCompile.\n@@FIN\n"
             "@@DELEGUER 09 Interface\nFais l'UI.\n@@FIN");
    o.tick(h);
    CHECK(h.created.empty());  // rien de créé sans réponse
    CHECK(o.state == OrchState::WaitingHuman);
    REQUIRE(o.pendingAgents.size() == 1);
    REQUIRE(o.questions.size() == 1);
    CHECK(o.questions[0].text.find("une appli de bureau distincte") != std::string::npos);
    REQUIRE(o.questions[0].options.size() == 2);
    CHECK(o.questions[0].options[0] == Orchestrator::agentAcceptLabel("09 Interface"));
    // L'agent existant travaille pendant que l'humain décide ; la tâche de l'agent proposé est écartée.
    CHECK(h.convs["conv:02 Build"].received.size() == 1);
    CHECK(h.convs.count("conv:09 Interface") == 0);

    // Rechargement pendant l'attente : la question est reconstruite.
    Orchestrator o2;
    o2.fromJson(o.toJson());
    REQUIRE(o2.pendingAgents.size() == 1);
    REQUIRE(o2.questions.size() == 1);
    CHECK(o2.questions[0].options[1] == Orchestrator::agentRefuseLabel("09 Interface"));

    REQUIRE(o.answerHuman(h, "**" + o.questions[0].title + "**\n" + Orchestrator::agentAcceptLabel("09 Interface") +
                                 "\n\nPrécisions : en Python/Qt."));
    REQUIRE(h.created.size() == 1);
    CHECK(h.created[0].first == "09 Interface");
    CHECK(h.created[0].second.find("Mission : l'UI.") != std::string::npos);
    CHECK(h.created[0].second.find("en Python/Qt.") != std::string::npos);  // précisions gardées dans la fiche
    CHECK(o.pendingAgents.empty());
    const std::string& msg = h.convs["brain"].received.back();
    CHECK(msg.find("accepté et créé") != std::string::npos);
    CHECK(msg.find("n'existe pas encore") != std::string::npos);  // la tâche écartée est rapportée

    // L'agent créé est aussitôt délégable.
    h.answer("brain", "@@DELEGUER 09 Interface | modele=sonnet\nFais l'UI.\n@@FIN");
    o.tick(h);
    CHECK(h.convs["conv:09 Interface"].received.size() == 1);
}

TEST_CASE("orchestrateur : nouvel agent refusé, ou précisions à intégrer") {
    TeamHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "@@NOUVEL_AGENT 09 Interface\nPourquoi : UI.\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::WaitingHuman);
    REQUIRE(o.answerHuman(h, Orchestrator::agentRefuseLabel("09 Interface")));
    CHECK(h.created.empty());
    CHECK(h.convs["brain"].received.back().find("refusé par l'humain") != std::string::npos);

    // Réponse libre (« Autre ») : pas de création, le cerveau révise et soumet de nouveau.
    h.answer("brain", "@@NOUVEL_AGENT 09 Interface\nPourquoi : UI.\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::WaitingHuman);
    REQUIRE(o.answerHuman(h, "Plutôt une page web servie par 05, pas une appli de bureau."));
    CHECK(h.created.empty());
    CHECK(h.convs["brain"].received.back().find("révise ta proposition") != std::string::npos);
    CHECK(o.pendingAgents.empty());
}

TEST_CASE("orchestrateur : propositions écartées d'office (désactivé, plafond, nom pris)") {
    TeamHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.maxAgents = 4;  // 3 agents au départ
    REQUIRE(o.start(h));
    h.answer("brain",
             "@@NOUVEL_AGENT 02 Build\nPourquoi : doublon.\n@@FIN\n"
             "@@NOUVEL_AGENT 09 Interface\nPourquoi : UI.\n@@FIN\n"
             "@@NOUVEL_AGENT 10 Docs\nPourquoi : doc.\n@@FIN");
    o.tick(h);
    // Seule « 09 Interface » est soumise : « 02 Build » existe, « 10 Docs » dépasserait le plafond.
    REQUIRE(o.pendingAgents.size() == 1);
    CHECK(o.pendingAgents[0].name == "09 Interface");
    REQUIRE(o.answerHuman(h, Orchestrator::agentAcceptLabel("09 Interface")));
    const std::string& msg = h.convs["brain"].received.back();
    CHECK(msg.find("un agent porte déjà ce nom") != std::string::npos);
    CHECK(msg.find("plafond de 4 agents") != std::string::npos);

    // Désactivé : rien n'est soumis, le motif part aussitôt au cerveau (personne ne travaille).
    o.settings.allowNewAgents = false;
    size_t before = h.convs["brain"].received.size();
    h.answer("brain", "@@NOUVEL_AGENT 11 Perf\nPourquoi : mesures.\n@@FIN");
    o.tick(h);
    CHECK(o.state != OrchState::WaitingHuman);
    CHECK(o.pendingAgents.empty());
    REQUIRE(h.convs["brain"].received.size() == before + 1);
    CHECK(h.convs["brain"].received.back().find("désactivée par l'humain") != std::string::npos);
    CHECK(Orchestrator::systemPrompt(true, false).find("tu ne peux pas créer d'agent") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Fin de phase : récaps de l'équipe (sans fermer les conversations)
// ---------------------------------------------------------------------------

namespace {
struct RecapHost : FakeHost {
    std::vector<std::vector<std::string>> recaps;
    void recapConversations(const std::vector<std::string>& ids) override { recaps.push_back(ids); }
};
}  // namespace

TEST_CASE("orchestrateur : récaps de l'équipe à @@TERMINE, agents encore au travail exclus") {
    RecapHost h;
    Orchestrator o;
    o.settings.objective = "x";
    REQUIRE(o.start(h));
    h.answer("brain", "@@DELEGUER 01\nA.\n@@FIN\n@@DELEGUER 02\nB.\n@@FIN");
    o.tick(h);
    h.answer("conv:01 Modèle", "Fait.");
    o.tick(h);  // compte rendu intermédiaire
    h.answer("brain", "@@TERMINE\nLivré (02 finit sa tâche).\n@@FIN");
    o.tick(h);
    REQUIRE(o.state == OrchState::Finished);
    REQUIRE(h.recaps.size() == 1);
    const auto& ids = h.recaps[0];
    CHECK(std::find(ids.begin(), ids.end(), "brain") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "conv:01 Modèle") != ids.end());
    CHECK(std::find(ids.begin(), ids.end(), "conv:02 Build") == ids.end());  // encore au travail
    CHECK(o.toJson()["settings"]["recapOnPhaseEnd"] == true);
}

TEST_CASE("orchestrateur : récaps à la pause de l'orchestrateur, pas sur le plafond du compte ni si désactivé") {
    {
        RecapHost h;
        h.sub = true;
        Orchestrator o;
        o.settings.objective = "x";
        REQUIRE(o.start(h));
        h.answer("brain", "@@PAUSE\nQuota bas : reprise demain.\n@@FIN");
        o.tick(h);
        REQUIRE(o.state == OrchState::UsagePause);
        CHECK(h.recaps.size() == 1);
    }
    {
        RecapHost h;
        h.sub = true;
        Orchestrator o;
        o.settings.objective = "x";
        o.settings.usagePause = 0.9;
        REQUIRE(o.start(h));
        h.answer("brain", "@@DELEGUER 01\nA.\n@@FIN");
        h.usage = 0.95;  // plafond du compte atteint : pause, sans récap (quota à préserver)
        o.tick(h);
        o.tick(h);
        CHECK(o.state == OrchState::UsagePause);
        CHECK(h.recaps.empty());
    }
    {
        RecapHost h;
        Orchestrator o;
        o.settings.objective = "x";
        o.settings.recapOnPhaseEnd = false;
        REQUIRE(o.start(h));
        h.answer("brain", "@@TERMINE\nLivré.\n@@FIN");
        o.tick(h);
        REQUIRE(o.state == OrchState::Finished);
        CHECK(h.recaps.empty());
    }
}

TEST_CASE("vérification visuelle : captures et agents équipés du navigateur") {
    FakeHost h;
    Orchestrator o;
    o.settings.objective = "x";
    o.settings.browserAgents = {"02 Build"};
    REQUIRE(o.start(h));
    // Le cerveau sait qu'il peut regarder les captures et qui a le navigateur
    CHECK(h.brainPrompt.find("Vérification VISUELLE") != std::string::npos);
    CHECK(h.brainPrompt.find("Agents équipés du navigateur") != std::string::npos);
    CHECK(h.convs["brain"].received[0].find("**02 Build** 🌐 navigateur") != std::string::npos);
    CHECK(h.convs["brain"].received[0].find("**01 Modèle** —") != std::string::npos);
    h.answer("brain", "@@DELEGUER 01 | modele=sonnet\na\n@@FIN\n@@DELEGUER 02 | modele=sonnet\nb\n@@FIN");
    o.tick(h);
    auto& a1 = h.convs["conv:01 Modèle"];
    auto& a2 = h.convs["conv:02 Build"];
    REQUIRE(!a1.received.empty());
    REQUIRE(!a2.received.empty());
    CHECK_FALSE(a1.last.chrome);
    CHECK(a2.last.chrome);
    CHECK(a2.last.lean);  // le navigateur reste compatible avec les conversations allégées
    CHECK(a1.received.back().find("_captures/") != std::string::npos);
    CHECK(a1.received.back().find("Claude in Chrome") == std::string::npos);
    CHECK(a2.received.back().find("Claude in Chrome") != std::string::npos);

    // Désactivée : ni consigne de capture, ni section dans le prompt du cerveau
    o.settings.visualChecks = false;
    o.settings.browserAgents.clear();
    CHECK(o.brainPrompt().find("VISUELLE") == std::string::npos);
    CHECK(o.brainPrompt().find("navigateur (🌐)") == std::string::npos);

    // Réglages sauvegardés
    OrchestratorSettings s;
    s.visualChecks = false;
    s.browserAgents = {"03 Check"};
    Orchestrator p;
    p.settings = s;
    Orchestrator q;
    q.fromJson(p.toJson());
    CHECK_FALSE(q.settings.visualChecks);
    REQUIRE(q.settings.browserAgents.size() == 1);
    CHECK(q.settings.browserAgents[0] == "03 Check");
}
