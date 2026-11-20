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
    std::string mode;
};

struct FakeHost : OrchestratorHost {
    std::vector<AgentInfo> agentList = {{"01 Modèle", "faits matériels"}, {"02 Build", "toolchain"}, {"03 Check", "barrière"}};
    std::map<std::string, FakeConv> convs;
    double usage = 0.1;
    int64_t t = 1000;
    std::string brainPrompt;

    std::vector<AgentInfo> agents() override { return agentList; }
    std::string openBrain(const std::string& hint, const std::string& sp) override {
        brainPrompt = sp;
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
    bool send(const std::string& id, const std::string& text, const std::string& mode) override {
        auto& c = convs[id];
        c.received.push_back(text);
        c.mode = mode;
        c.st.busy = true;
        return true;
    }
    ParticipantStatus status(const std::string& id) override {
        auto it = convs.find(id);
        return it == convs.end() ? ParticipantStatus{} : it->second.st;
    }
    double fiveHourUtilization() override { return usage; }
    int64_t fiveHourResetsAt() override { return 5000; }
    int64_t now() override { return t; }

    // Simule la fin d'un tour.
    void answer(const std::string& id, const std::string& text, double cost = 0.5) {
        auto& c = convs[id];
        c.st.busy = false;
        c.st.turns++;
        c.st.costUsd += cost;
        c.st.lastText = text;
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
    CHECK(h.convs["brain"].mode == "auto");

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
