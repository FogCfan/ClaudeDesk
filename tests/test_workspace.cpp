#include <doctest/doctest.h>

#include "claude/ClaudeSession.h"
#include "util/Paths.h"
#include "util/Strings.h"
#include "workspace/Workspace.h"

TEST_CASE("arborescence cowork sur disque") {
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
