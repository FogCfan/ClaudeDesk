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
