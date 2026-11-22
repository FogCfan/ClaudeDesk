#include <doctest/doctest.h>

#include "claude/ClaudeSession.h"
#include "util/Paths.h"
#include "util/Strings.h"
#include "workspace/Workspace.h"

TEST_CASE("arborescence des projets sur disque") {
    fs::path root = fs::temp_directory_path() / ("claudedesk_test_" + util::newUuid().substr(0, 8));
    {
        ws::Workspace w;
        REQUIRE(w.open(root));
        auto* p = w.createProject("Mon Projet", "C:\\Dev\\X");
        REQUIRE(p);
        auto* code = w.createFolder(p, "Code");
        auto* rendu = w.createFolder(code, "Rendu");
        REQUIRE(rendu);
        CHECK(rendu->displayPath() == "Mon Projet › Code › Rendu");
        CHECK(rendu->effectiveCwd() == "C:\\Dev\\X");

        claude::ClaudeSession s;
        s.title = "Refonte du shader";
        s.addSystemNote("x");
        auto* m = w.saveConversation(rendu, s.toJson(), nullptr);
        REQUIRE(m);
        CHECK(fs::exists(m->file));
        ws::ConversationMeta upd = *m;
        upd.recap = "### Objectif\nShader plus rapide";
        CHECK(w.updateConversationMeta(upd));

        p->notes = "Moteur 2D en C++";
        w.saveFolderMeta(p);
        std::string ctx = w.buildContext(rendu, "autre-id");
        CHECK(ctx.find("Moteur 2D en C++") != std::string::npos);
        CHECK(ctx.find("Shader plus rapide") != std::string::npos);
        CHECK(w.buildContext(rendu, s.localId).find("Shader plus rapide") == std::string::npos);

        CHECK(w.moveConversation(s.localId, code));
        CHECK(code->conversations.size() == 1);
        CHECK(rendu->conversations.empty());
        CHECK(w.renameFolder(code, "Moteur"));
        CHECK(fs::exists(code->conversations[0].file));
    }
    {
        ws::Workspace w;  // relecture depuis le disque
        REQUIRE(w.open(root));
        REQUIRE(w.projects().size() == 1);
        auto& p = *w.projects()[0];
        CHECK(p.isProject);
        CHECK(p.cwd == "C:\\Dev\\X");
        CHECK(p.notes == "Moteur 2D en C++");
        REQUIRE(p.children.size() == 1);
        CHECK(p.children[0]->name == "Moteur");
        REQUIRE(p.children[0]->conversations.size() == 1);
        CHECK(p.children[0]->conversations[0].recap.find("Shader") != std::string::npos);
        CHECK(p.totalConversations() == 1);
        CHECK(w.removeConversation(p.children[0]->conversations[0].id));
        CHECK(p.totalConversations() == 0);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST_CASE("fichiers de référence d'un dossier") {
    fs::path root = fs::temp_directory_path() / ("claudedesk_test_" + util::newUuid().substr(0, 8));
    fs::path src = root.parent_path() / ("claudedesk_src_" + util::newUuid().substr(0, 8) + ".md");
    util::writeFileAtomic(src, "# Cahier des charges");
    {
        ws::Workspace w;
        REQUIRE(w.open(root));
        auto* p = w.createProject("Projet", "");
        auto* code = w.createFolder(p, "Code");
        REQUIRE(code);
        std::string err;
        CHECK(w.addFiles(code, {src, src}, &err) == 2);  // le second reçoit un suffixe « (2) »
        CHECK(code->files.size() == 2);
        CHECK(ws::Workspace::isInternalFile(code->path / "_dossier.json"));
        CHECK(ws::Workspace::isInternalFile(code->path / "x [12345678].conv.json"));
        CHECK_FALSE(ws::Workspace::isInternalFile(code->path / "schema.png"));
        // Le contexte d'une conversation du thème cite leur chemin.
        std::string ctx = w.buildContext(code);
        CHECK(ctx.find("Documents de référence") != std::string::npos);
        CHECK(ctx.find(util::pathToUtf8(code->files[0].filename())) != std::string::npos);
        // Relus au scan, sans les fichiers internes.
        fs::path codePath = code->path;
        w.rescan();  // `code` n'est plus valide
        auto* code2 = w.findFolderByPath(codePath);
        REQUIRE(code2);
        CHECK(code2->files.size() == 2);
        CHECK(w.removeFile(code2, code2->files[0]));
        CHECK(code2->files.size() == 1);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove(src, ec);
}

namespace {
ws::ConversationMeta conv(const std::string& id, const std::string& title, int64_t created, int64_t updated,
                          const std::string& status = {}, int64_t recapTime = 0, const std::string& continues = {}) {
    ws::ConversationMeta m;
    m.id = id;
    m.title = title;
    m.created = created;
    m.updated = updated;
    m.status = status;
    m.recapTime = recapTime;
    m.continues = continues;
    return m;
}
std::vector<std::string> ids(const ws::ConversationChain& c) {
    std::vector<std::string> v;
    for (auto* m : c.sessions) v.push_back(m->id);
    return v;
}
}  // namespace

TEST_CASE("discussions : sessions reliées par la rotation de contexte") {
    SUBCASE("lien enregistré") {
        ws::Folder f;
        f.conversations.push_back(conv("b", "Tâches de l'orchestrateur", 2000, 3000, "active", 0, "a"));
        f.conversations.push_back(conv("a", "Tâches de l'orchestrateur", 1000, 1990, "done"));
        f.conversations.push_back(conv("x", "Autre sujet", 1500, 2500));
        auto chains = ws::conversationChains(f);
        REQUIRE(chains.size() == 2);
        CHECK(ids(chains[0]) == std::vector<std::string>{"a", "b"});  // la plus récente d'abord
        CHECK(chains[0].latest()->id == "b");
        CHECK(chains[0].indexOf("a") == 0);
        CHECK(ids(chains[1]) == std::vector<std::string>{"x"});
        CHECK(ws::countDiscussions(f) == 2);
    }
    SUBCASE("anciennes conversations : déduit des dates (comme les données réelles)") {
        ws::Folder f;
        // Mission → Tâches (créée à la mise à jour de Mission) → Tâches (créée 77 s après le récap de la précédente,
        // qui a été réenregistrée plus tard)
        f.conversations.push_back(conv("m", "Mission", 100, 5000, "done", 5000));
        f.conversations.push_back(conv("t1", "Tâches de l'orchestrateur", 5000, 9000, "done", 8000));
        f.conversations.push_back(conv("t2", "Tâches de l'orchestrateur", 8077, 9500, "active", 9400));
        auto chains = ws::conversationChains(f);
        REQUIRE(chains.size() == 1);
        CHECK(ids(chains[0]) == std::vector<std::string>{"m", "t1", "t2"});
    }
    SUBCASE("pas de regroupement abusif") {
        ws::Folder f;
        // Pas terminée, trop éloignée, ou titre différent (hors titres de l'orchestrateur) : discussions séparées
        f.conversations.push_back(conv("a", "Shader", 100, 1000, "active"));
        f.conversations.push_back(conv("b", "Shader", 1001, 2000));
        f.conversations.push_back(conv("c", "Rendu", 3000, 4000, "done"));
        f.conversations.push_back(conv("d", "Rendu", 9000, 9500));
        f.conversations.push_back(conv("e", "Notes", 10000, 11000, "done"));
        f.conversations.push_back(conv("g", "Idées", 11001, 12000));
        CHECK(ws::conversationChains(f).size() == 6);
    }
    SUBCASE("liens corrompus (boucle) : chaque conversation reste affichée une fois") {
        ws::Folder f;
        f.conversations.push_back(conv("a", "X", 100, 200, "done", 0, "b"));
        f.conversations.push_back(conv("b", "X", 300, 400, "done", 0, "a"));
        auto chains = ws::conversationChains(f);
        size_t total = 0;
        for (auto& c : chains) total += c.sessions.size();
        CHECK(total == 2);
    }
}

TEST_CASE("discussions : le lien est enregistré sur disque") {
    fs::path root = fs::temp_directory_path() / ("claudedesk_test_" + util::newUuid().substr(0, 8));
    {
        ws::Workspace w;
        REQUIRE(w.open(root));
        auto* p = w.createProject("P", "");
        claude::ClaudeSession s1, s2;
        s1.title = s2.title = "Tâches de l'orchestrateur";
        s1.addSystemNote("x");
        s2.addSystemNote("y");
        REQUIRE(w.saveConversation(p, s1.toJson(), nullptr));
        ws::ConversationMeta m2;
        m2.continues = s1.localId;
        REQUIRE(w.saveConversation(p, s2.toJson(), &m2));
        w.rescan();
        auto [f, m] = w.findConversation(s2.localId);
        REQUIRE(m);
        CHECK(m->continues == s1.localId);
        // Une mise à jour (titre, statut…) garde le lien
        ws::ConversationMeta upd = *m;
        upd.status = "done";
        CHECK(w.updateConversationMeta(upd));
        w.rescan();
        CHECK(w.findConversation(s2.localId).second->continues == s1.localId);
        CHECK(ws::conversationChains(*w.findConversation(s2.localId).first).size() == 1);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}
