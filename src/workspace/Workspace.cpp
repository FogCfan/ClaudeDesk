#include "workspace/Workspace.h"

#include <algorithm>

#include "util/Paths.h"
#include "util/Strings.h"

namespace ws {

using json = nlohmann::json;
static const char* kMetaFile = "_dossier.json";
static const char* kConvExt = ".conv.json";

static bool isConversationFile(const fs::path& p) { return util::endsWith(util::pathToUtf8(p.filename()), kConvExt); }

std::string conversationFileName(const std::string& title, const std::string& id) {
    std::string t = util::sanitizeFileName(util::truncateUtf8(title, 60));
    util::replaceAll(t, "…", "");
    return util::trim(t) + " [" + id.substr(0, 8) + "]" + kConvExt;
}

const char* statusLabel(const std::string& s) {
    if (s == "active") return "En cours";
    if (s == "done") return "Terminé";
    if (s == "blocked") return "Bloqué";
    if (s == "idea") return "Idée";
    return "";
}

// --- Folder -----------------------------------------------------------------

std::string Folder::displayPath() const {
    std::string s = name;
    for (const Folder* p = parent; p; p = p->parent) s = p->name + " › " + s;
    return s;
}

const Folder* Folder::project() const {
    const Folder* f = this;
    while (f->parent) f = f->parent;
    return f;
}
Folder* Folder::project() {
    Folder* f = this;
    while (f->parent) f = f->parent;
    return f;
}

std::string Folder::effectiveCwd() const {
    for (const Folder* f = this; f; f = f->parent)
        if (!f->cwd.empty()) return f->cwd;
    return {};
}

int Folder::totalConversations() const {
    int n = (int)conversations.size();
    for (auto& c : children) n += c->totalConversations();
    return n;
}

int64_t Folder::lastActivity() const {
    int64_t t = 0;
    for (auto& c : conversations) t = std::max(t, c.updated);
    for (auto& c : children) t = std::max(t, c->lastActivity());
    return t;
}

// --- Workspace ----------------------------------------------------------------

bool Workspace::open(const fs::path& root) {
    root_ = root;
    std::error_code ec;
    fs::create_directories(root_, ec);
    if (!fs::is_directory(root_, ec)) return false;
    rescan();
    return true;
}

void Workspace::rescan() {
    projects_.clear();
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (auto& d : fs::directory_iterator(root_, ec)) {
        auto name = util::pathToUtf8(d.path().filename());
        if (d.is_directory() && !name.empty() && name[0] != '.') dirs.push_back(d.path());
    }
    std::sort(dirs.begin(), dirs.end());
    for (auto& d : dirs) projects_.push_back(scanFolder(d, nullptr, true));
}

void Workspace::loadFolderMeta(Folder& f) {
    std::string txt;
    if (!util::readFile(f.path / kMetaFile, txt)) return;
    json j = json::parse(txt, nullptr, false);
    if (!j.is_object()) return;
    f.cwd = j.value("cwd", "");
    f.notes = j.value("notes", "");
    f.recap = j.value("recap", "");
    f.recapTime = j.value("recapTime", (int64_t)0);
    f.icon = j.value("icon", "");
}

bool Workspace::saveFolderMeta(Folder* f) {
    json j = {{"kind", f->isProject ? "project" : "folder"},
              {"cwd", f->cwd},
              {"notes", f->notes},
              {"recap", f->recap},
              {"recapTime", f->recapTime},
              {"icon", f->icon}};
    return util::writeFileAtomic(f->path / kMetaFile, j.dump(2));
}

bool Workspace::readConversationMeta(const fs::path& file, ConversationMeta& m) {
    std::string txt;
    if (!util::readFile(file, txt)) return false;
    json j = json::parse(txt, nullptr, false);
    if (!j.is_object()) return false;
    m.file = file;
    m.id = j.value("id", "");
    m.title = j.value("title", "");
    m.claudeSessionId = j.value("claudeSessionId", "");
    m.recap = j.value("recap", "");
    m.recapTime = j.value("recapTime", (int64_t)0);
    m.created = j.value("created", (int64_t)0);
    m.updated = j.value("updated", (int64_t)0);
    m.pinned = j.value("pinned", false);
    m.status = j.value("status", "");
    m.messages = 0;
    for (auto& e : j.value("entries", json::array()))
        if (e.value("kind", "") == "user") m.messages++;
    if (j.contains("stats")) {
        m.costUsd = j["stats"].value("costUsd", 0.0);
        m.model = j["stats"].value("lastModel", "");
    }
    return !m.id.empty();
}

std::unique_ptr<Folder> Workspace::scanFolder(const fs::path& dir, Folder* parent, bool isProject) {
    auto f = std::make_unique<Folder>();
    f->path = dir;
    f->name = util::pathToUtf8(dir.filename());
    f->parent = parent;
    f->isProject = isProject;
    loadFolderMeta(*f);
    std::error_code ec;
    std::vector<fs::path> subdirs;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_directory()) {
            auto n = util::pathToUtf8(e.path().filename());
            if (!n.empty() && n[0] != '.') subdirs.push_back(e.path());
        } else if (isConversationFile(e.path())) {
            ConversationMeta m;
            if (readConversationMeta(e.path(), m)) f->conversations.push_back(std::move(m));
        }
    }
    std::sort(subdirs.begin(), subdirs.end());
    for (auto& s : subdirs) f->children.push_back(scanFolder(s, f.get(), false));
    std::sort(f->conversations.begin(), f->conversations.end(), [](auto& a, auto& b) {
        if (a.pinned != b.pinned) return a.pinned;
        return a.updated > b.updated;
    });
    return f;
}

Folder* Workspace::createProject(const std::string& name, const std::string& cwd, std::string* error) {
    std::string clean = util::sanitizeFileName(name);
    fs::path p = root_ / util::pathFromUtf8(clean);
    std::error_code ec;
    if (fs::exists(p, ec)) {
        if (error) *error = "Un projet porte déjà ce nom.";
        return nullptr;
    }
    if (!fs::create_directories(p, ec)) {
        if (error) *error = "Impossible de créer le dossier : " + ec.message();
        return nullptr;
    }
    auto f = std::make_unique<Folder>();
    f->name = clean;
    f->path = p;
    f->isProject = true;
    f->cwd = cwd;
    saveFolderMeta(f.get());
    projects_.push_back(std::move(f));
    std::sort(projects_.begin(), projects_.end(), [](auto& a, auto& b) { return a->name < b->name; });
    for (auto& pr : projects_)
        if (pr->path == p) return pr.get();
    return nullptr;
}

Folder* Workspace::createFolder(Folder* parent, const std::string& name, std::string* error) {
    if (!parent) return nullptr;
    std::string clean = util::sanitizeFileName(name);
    fs::path p = parent->path / util::pathFromUtf8(clean);
    std::error_code ec;
    if (fs::exists(p, ec)) {
        if (error) *error = "Un dossier porte déjà ce nom.";
        return nullptr;
    }
    if (!fs::create_directories(p, ec)) {
        if (error) *error = "Impossible de créer le dossier : " + ec.message();
        return nullptr;
    }
    auto f = std::make_unique<Folder>();
    f->name = clean;
    f->path = p;
    f->parent = parent;
    saveFolderMeta(f.get());
    Folder* raw = f.get();
    parent->children.push_back(std::move(f));
    std::sort(parent->children.begin(), parent->children.end(), [](auto& a, auto& b) { return a->name < b->name; });
    return raw;
}

static void repath(Folder* f, const fs::path& newPath) {
    f->path = newPath;
    for (auto& c : f->conversations) c.file = newPath / c.file.filename();
    for (auto& ch : f->children) repath(ch.get(), newPath / ch->path.filename());
}

bool Workspace::renameFolder(Folder* f, const std::string& newName, std::string* error) {
    std::string clean = util::sanitizeFileName(newName);
    fs::path np = f->path.parent_path() / util::pathFromUtf8(clean);
    std::error_code ec;
    if (fs::exists(np, ec)) {
        if (error) *error = "Ce nom est déjà utilisé.";
        return false;
    }
    fs::rename(f->path, np, ec);
    if (ec) {
        if (error) *error = "Renommage impossible : " + ec.message() + " (une conversation de ce dossier est peut-être ouverte ailleurs)";
        return false;
    }
    f->name = clean;
    repath(f, np);
    return true;
}

bool Workspace::removeFolder(Folder* f) {
    fs::path trash = root_ / ".corbeille";
    std::error_code ec;
    fs::create_directories(trash, ec);
    fs::path dest = trash / util::pathFromUtf8(f->name + " (" + std::to_string(util::nowUnix()) + ")");
    fs::rename(f->path, dest, ec);
    if (ec) return false;
    auto& siblings = f->parent ? f->parent->children : projects_;
    siblings.erase(std::remove_if(siblings.begin(), siblings.end(), [&](auto& p) { return p.get() == f; }), siblings.end());
    return true;
}

ConversationMeta* Workspace::saveConversation(Folder* folder, const json& sessionJson, const ConversationMeta* metaIn) {
    if (!folder) return nullptr;
    std::string id = sessionJson.value("id", "");
    if (id.empty()) return nullptr;
    ConversationMeta* m = nullptr;
    for (auto& c : folder->conversations)
        if (c.id == id) m = &c;
    if (!m) {
        folder->conversations.push_back({});
        m = &folder->conversations.back();
        m->id = id;
        m->created = util::nowUnix();
    }
    if (metaIn && metaIn != m) {
        m->recap = metaIn->recap;
        m->recapTime = metaIn->recapTime;
        m->pinned = metaIn->pinned;
        m->status = metaIn->status;
        if (metaIn->created) m->created = metaIn->created;
    }
    m->title = sessionJson.value("title", m->title);
    m->claudeSessionId = sessionJson.value("claudeSessionId", "");
    m->updated = util::nowUnix();
    m->messages = 0;
    for (auto& e : sessionJson.value("entries", json::array()))
        if (e.value("kind", "") == "user") m->messages++;
    if (sessionJson.contains("stats")) {
        m->costUsd = sessionJson["stats"].value("costUsd", 0.0);
        m->model = sessionJson["stats"].value("lastModel", "");
    }

    json j = sessionJson;
    j["recap"] = m->recap;
    j["recapTime"] = m->recapTime;
    j["created"] = m->created;
    j["updated"] = m->updated;
    j["pinned"] = m->pinned;
    j["status"] = m->status;

    fs::path desired = folder->path / util::pathFromUtf8(conversationFileName(m->title, id));
    if (!m->file.empty() && m->file != desired) {
        std::error_code ec;
        fs::rename(m->file, desired, ec);
        if (ec) desired = m->file;  // on garde l'ancien nom si le renommage échoue
    }
    m->file = desired;
    util::writeFileAtomic(m->file, j.dump(1, ' ', false, json::error_handler_t::replace));
    return m;
}

bool Workspace::loadConversation(const ConversationMeta& meta, json& out) const {
    std::string txt;
    if (!util::readFile(meta.file, txt)) return false;
    out = json::parse(txt, nullptr, false);
    return out.is_object();
}

bool Workspace::updateConversationMeta(const ConversationMeta& meta) {
    auto [folder, m] = findConversation(meta.id);
    if (!m) return false;
    json j;
    if (!loadConversation(*m, j)) return false;
    m->recap = meta.recap;
    m->recapTime = meta.recapTime;
    m->pinned = meta.pinned;
    m->status = meta.status;
    j["title"] = meta.title;
    m->title = meta.title;
    j["recap"] = m->recap;
    j["recapTime"] = m->recapTime;
    j["pinned"] = m->pinned;
    j["status"] = m->status;
    fs::path desired = folder->path / util::pathFromUtf8(conversationFileName(m->title, m->id));
    std::error_code ec;
    if (desired != m->file) {
        fs::rename(m->file, desired, ec);
        if (!ec) m->file = desired;
    }
    return util::writeFileAtomic(m->file, j.dump(1, ' ', false, json::error_handler_t::replace));
}

bool Workspace::moveConversation(const std::string& id, Folder* dest) {
    auto [src, m] = findConversation(id);
    if (!m || !dest || src == dest) return false;
    fs::path np = dest->path / m->file.filename();
    std::error_code ec;
    fs::rename(m->file, np, ec);
    if (ec) return false;
    ConversationMeta copy = *m;
    copy.file = np;
    src->conversations.erase(std::remove_if(src->conversations.begin(), src->conversations.end(),
                                            [&](auto& c) { return c.id == id; }),
                             src->conversations.end());
    dest->conversations.insert(dest->conversations.begin(), copy);
    return true;
}

bool Workspace::removeConversation(const std::string& id) {
    auto [folder, m] = findConversation(id);
    if (!m) return false;
    fs::path trash = root_ / ".corbeille";
    std::error_code ec;
    fs::create_directories(trash, ec);
    fs::rename(m->file, trash / m->file.filename(), ec);
    if (ec) return false;
    folder->conversations.erase(std::remove_if(folder->conversations.begin(), folder->conversations.end(),
                                               [&](auto& c) { return c.id == id; }),
                                folder->conversations.end());
    return true;
}

Folder* Workspace::findFolderRec(Folder* f, const fs::path& p) {
    if (f->path == p) return f;
    for (auto& c : f->children)
        if (Folder* r = findFolderRec(c.get(), p)) return r;
    return nullptr;
}

Folder* Workspace::findFolderByPath(const fs::path& p) {
    for (auto& pr : projects_)
        if (Folder* r = findFolderRec(pr.get(), p)) return r;
    return nullptr;
}

static std::pair<Folder*, ConversationMeta*> findConvRec(Folder* f, const std::string& id) {
    for (auto& c : f->conversations)
        if (c.id == id) return {f, &c};
    for (auto& ch : f->children)
        if (auto r = findConvRec(ch.get(), id); r.second) return r;
    return {nullptr, nullptr};
}

std::pair<Folder*, ConversationMeta*> Workspace::findConversation(const std::string& id) {
    for (auto& pr : projects_)
        if (auto r = findConvRec(pr.get(), id); r.second) return r;
    return {nullptr, nullptr};
}

std::string Workspace::buildContext(const Folder* folder, const std::string& excludeId, size_t maxChars) const {
    if (!folder) return {};
    const Folder* project = folder->project();
    std::string ctx;
    ctx += "# Contexte de travail (ClaudeDesk — mode cowork)\n";
    ctx += "Projet : " + project->name;
    if (!folder->effectiveCwd().empty()) ctx += " (dossier de travail : " + folder->effectiveCwd() + ")";
    ctx += "\nCette conversation est rangée dans : " + folder->displayPath() + "\n";
    ctx += "L'utilisateur organise ses échanges avec toi par projet puis par thème. Les éléments ci-dessous "
           "résument le travail déjà fait : appuie-toi dessus pour assurer la continuité, sans les répéter.\n";

    // Du projet vers le dossier courant : notes et synthèses.
    std::vector<const Folder*> chain;
    for (const Folder* f = folder; f; f = f->parent) chain.insert(chain.begin(), f);
    for (const Folder* f : chain) {
        std::string label = f->isProject ? "du projet" : "du dossier « " + f->name + " »";
        if (!util::trim(f->notes).empty()) ctx += "\n## Notes " + label + "\n" + f->notes + "\n";
        if (!util::trim(f->recap).empty()) ctx += "\n## Synthèse " + label + "\n" + f->recap + "\n";
    }

    // Récaps des conversations précédentes du dossier (les plus récentes d'abord).
    std::vector<const ConversationMeta*> convs;
    for (auto& c : folder->conversations)
        if (c.id != excludeId && !util::trim(c.recap).empty()) convs.push_back(&c);
    std::sort(convs.begin(), convs.end(), [](auto a, auto b) { return a->updated > b->updated; });
    if (!convs.empty()) ctx += "\n## Échanges précédents dans « " + folder->name + " »\n";
    for (auto* c : convs) {
        std::string block = "\n### " + c->title + " (" + util::humanDate(c->updated) + ")\n" + c->recap + "\n";
        if (ctx.size() + block.size() > maxChars) {
            ctx += "\n(… autres échanges omis)\n";
            break;
        }
        ctx += block;
    }
    // Liste des sous-thèmes, pour donner la carte du projet.
    if (folder->isProject && !folder->children.empty()) {
        ctx += "\n## Thèmes du projet\n";
        for (auto& ch : folder->children)
            ctx += "- " + ch->name + " (" + std::to_string(ch->totalConversations()) + " échanges)\n";
    }
    return util::truncateUtf8(ctx, maxChars);
}

}  // namespace ws
