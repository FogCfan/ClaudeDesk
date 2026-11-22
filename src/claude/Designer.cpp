#include "claude/Designer.h"

#include <algorithm>
#include <cctype>

#include "util/Strings.h"

namespace claude {

using nlohmann::json;

namespace {

std::string str(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return {};
    const json& v = j[key];
    if (v.is_string()) return util::trim(v.get<std::string>());
    if (v.is_array()) {  // une liste là où on attend un texte : on la met en puces
        std::string s;
        for (auto& x : v)
            if (x.is_string()) s += "- " + x.get<std::string>() + "\n";
        return util::trim(s);
    }
    return {};
}

std::vector<std::string> strList(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.is_object() || !j.contains(key)) return out;
    const json& v = j[key];
    if (v.is_array()) {
        for (auto& x : v)
            if (x.is_string() && !util::trim(x.get<std::string>()).empty()) out.push_back(util::trim(x.get<std::string>()));
    } else if (v.is_string()) {
        for (auto& l : util::split(v.get<std::string>(), ','))
            if (!util::trim(l).empty()) out.push_back(util::trim(l));
    }
    return out;
}

std::string lower(std::string s) { return util::toLower(s); }

const char* teamSizeText(int t) {
    switch (t) {
        case 1: return "petite (2 à 3 agents)";
        case 2: return "moyenne (4 à 6 agents)";
        case 3: return "grande (7 à 10 agents)";
        default: return "à toi de juger (le plus souvent 3 à 6 agents)";
    }
}

const char* budgetText(const std::string& b) {
    if (b == "low") return "bas (économiser le quota : petits modèles, peu d'agents en parallèle)";
    if (b == "high") return "élevé";
    if (b == "max") return "maximal (abonnement)";
    return "moyen";
}

std::string requestSection(const DesignRequest& r) {
    std::string m = "## Demande de l'humain\n" + util::trim(r.description) + "\n\n";
    if (!util::trim(r.folder).empty())
        m += "Dossier de travail : `" + util::trim(r.folder) + "`" +
             (r.analyzeFolder ? " — dossier EXISTANT : la conception part de ce qui s'y trouve déjà.\n"
                              : " — dossier neuf (ou contenu à ignorer).\n");
    else
        m += "Dossier de travail : pas encore choisi (projet neuf).\n";
    if (!util::trim(r.tech).empty()) m += "Technologies imposées : " + util::trim(r.tech) + "\n";
    if (!util::trim(r.constraints).empty()) m += "Contraintes : " + util::trim(r.constraints) + "\n";
    m += std::string("Taille d'équipe : ") + teamSizeText(r.teamSize) + "\n";
    m += std::string("Budget : ") + budgetText(r.budget) + "\n";
    bool any = false;
    for (auto& q : r.answered) {
        if (!any) m += "\n## Réponses aux questions de clarification\n";
        any = true;
        std::string a = q.answer();
        m += "- " + q.question + " → " + (a.empty() ? "au choix du designer" : a) + "\n";
    }
    return m;
}

const char* kSchema = R"SCH(```json
{"design": {
  "name": "Nom du projet (court, sans / \\ : * ? \" < > |)",
  "summary": "2 à 4 phrases : ce qui sera construit, et les choix que tu as faits faute de précision",
  "stack": ["langage", "bibliothèques", "outils de build/tests"],
  "objective": "objectif complet pour l'orchestrateur : livrable final, critères d'acceptation vérifiables, priorités (l'essentiel d'abord)",
  "milestones": ["Jalon 1 — livrable vérifiable", "Jalon 2 — …"],
  "conventions": "build, tests, style, langue des textes — Markdown court",
  "budget": "low|medium|high|max",
  "visualChecks": true,
  "agents": [
    {"name": "01 Fondations",
     "mission": "une phrase (120 caractères max)",
     "role": "fiche Markdown adressée à l'agent : ## Mission, ## Possède (dossiers/fichiers), ## Frontières, ## Conventions, ## Vérifications",
     "model": "haiku|sonnet|opus",
     "effort": "low|medium|high",
     "browser": false}
  ]
}}
```)SCH";

const char* kTeamRules = R"RULES(## Règles de conception de l'équipe
- Fonctionnement : un ORCHESTRATEUR (le « cerveau », en lecture seule) découpe le travail et le délègue aux agents ;
  chaque agent est une conversation Claude Code distincte dans le même dépôt, qui relit sa fiche à chaque tâche,
  ne voit pas les autres conversations et rend un compte rendu court. Plusieurs agents travaillent en parallèle.
- Chaque agent POSSÈDE une partie disjointe du dépôt (dossiers, fichiers) : c'est ce qui évite les conflits quand
  ils travaillent en même temps. Les frontières doivent être nettes.
- Pas d'agent « chef de projet » : c'est le rôle de l'orchestrateur. Un agent tests/qualité seulement si le projet
  le justifie. Une tâche ponctuelle ne justifie pas un agent : chaque agent ajoute du contexte et de la coordination.
- Noms « NN Nom » (01, 02…), dans l'ordre où le travail démarre, courts, en français.
- La fiche (role) est écrite POUR l'agent, autonome et précise : sa mission, ce qu'il possède, ce qui appartient aux
  autres, les conventions, et comment il vérifie son travail (commandes de build/tests ; capture PNG pour le visuel).
- model/effort conseillés : haiku pour le mécanique (docs, mise en forme), sonnet pour le développement courant,
  opus seulement pour l'architecture délicate.
- browser: true seulement pour un agent qui doit ouvrir des pages web dans Chrome pour les vérifier.
- milestones : 3 à 6 jalons ordonnés, chacun vérifiable. objective : ce que l'orchestrateur doit livrer, et comment
  savoir que c'est fini.
- budget : garde celui demandé sauf raison forte.)RULES";

}  // namespace

std::string DesignQuestion::answer() const {
    if (choice == -2) return util::trim(other);
    if (choice >= 0 && choice < (int)options.size()) return options[(size_t)choice];
    return {};
}

std::string designPrompt(const DesignRequest& r) {
    std::string p = "# Rôle : DESIGNER DE PROJET (ClaudeDesk)\n"
                    "Tu conçois un projet logiciel ET l'équipe d'agents Claude Code qui va le réaliser, pilotée par "
                    "l'orchestrateur de ClaudeDesk. Tu ne développes rien toi-même : tu produis la conception.\n\n";
    p += requestSection(r) + "\n";
    p += "## Ce que tu dois faire\n";
    if (r.analyzeFolder && !util::trim(r.folder).empty())
        p += "- Examine d'abord le dossier de travail avec Read, Glob et Grep — lectures ciblées (structure, README, "
             "fichiers de build, code principal), une quinzaine au plus — et bâtis la conception sur l'existant : "
             "les agents se partagent le code qui existe déjà.\n";
    if (r.allowQuestions && r.answered.empty())
        p += "- Si une information ESSENTIELLE manque et change vraiment la conception (plateforme cible, langage, "
             "périmètre…), pose d'abord jusqu'à 4 questions, chacune avec 2 à 4 options et celle que tu recommandes. "
             "Sinon, conçois directement en faisant des choix raisonnables (cite-les dans summary). Ne pose pas de "
             "question de détail.\n"
             "  Format des questions :\n"
             "```json\n{\"questions\": [{\"question\": \"…\", \"why\": \"en quoi ça change la conception\", "
             "\"options\": [\"…\", \"…\"], \"recommended\": 0}]}\n```\n";
    else
        p += "- Ne pose pas de question : conçois directement, en faisant des choix raisonnables (cite-les dans summary).\n";
    p += "- Réponds UNIQUEMENT par un bloc json, sans texte autour.\n\nFormat de la conception :\n";
    p += kSchema;
    p += "\n\n";
    p += kTeamRules;
    return p;
}

std::string refinePrompt(const DesignRequest& r, const ProjectDesign& current, const std::string& feedback) {
    DesignRequest noQ = r;
    std::string p = "# Rôle : DESIGNER DE PROJET (ClaudeDesk)\n"
                    "Tu as conçu un projet et l'équipe d'agents qui va le réaliser. L'humain a pu modifier la conception "
                    "à la main, et te demande maintenant des changements.\n\n";
    p += requestSection(noQ) + "\n";
    p += "## Conception actuelle (modifications de l'humain comprises)\n```json\n" +
         json{{"design", toJson(current)}}.dump(1) + "\n```\n\n";
    p += "## Changements demandés\n" + util::trim(feedback) + "\n\n";
    p += "## Ce que tu dois faire\n"
         "- Applique les changements demandés, et seulement eux : garde le reste tel quel, y compris les modifications "
         "de l'humain. Si un changement en implique d'autres (un agent supprimé dont les fichiers passent à un autre), "
         "fais-les aussi.\n";
    if (r.analyzeFolder && !util::trim(r.folder).empty())
        p += "- Tu peux relire le dossier de travail (Read, Glob, Grep) si c'est nécessaire.\n";
    p += "- Réponds UNIQUEMENT par la conception COMPLÈTE mise à jour, dans un bloc json au même format :\n";
    p += kSchema;
    p += "\n\n";
    p += kTeamRules;
    return p;
}

// Le JSON de la réponse : le dernier bloc ```json, sinon du premier « { » au dernier « } ».
static std::string extractJson(const std::string& text) {
    size_t at = text.rfind("```json");
    if (at != std::string::npos) {
        size_t start = text.find('\n', at);
        size_t end = start == std::string::npos ? std::string::npos : text.find("```", start);
        if (start != std::string::npos && end != std::string::npos) return text.substr(start + 1, end - start - 1);
    }
    size_t a = text.find('{'), b = text.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a) return {};
    return text.substr(a, b - a + 1);
}

// Caractère interdit dans un nom de dossier (Windows) ?
static bool hasForbidden(const std::string& name) {
    for (char c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            (unsigned char)c < 32)
            return true;
    return false;
}

static DesignAgent agentFromJson(const json& a) {
    DesignAgent ag;
    ag.name = cleanAgentName(str(a, "name"));
    ag.mission = str(a, "mission");
    ag.role = str(a, "role");
    std::string m = lower(str(a, "model")), e = lower(str(a, "effort"));
    if (!m.empty()) ag.model = m;
    if (!e.empty()) ag.effort = e;
    ag.browser = a.is_object() && a.value("browser", false);
    if (ag.mission.empty() && !ag.role.empty()) ag.mission = util::firstLine(ag.role, 120);
    return ag;
}

ProjectDesign designFromJson(const json& d) {
    ProjectDesign out;
    if (!d.is_object()) return out;
    out.name = cleanAgentName(str(d, "name"));
    out.summary = str(d, "summary");
    out.stack = strList(d, "stack");
    out.objective = str(d, "objective");
    out.milestones = strList(d, "milestones");
    out.conventions = str(d, "conventions");
    std::string b = lower(str(d, "budget"));
    if (b == "low" || b == "medium" || b == "high" || b == "max") out.budget = b;
    out.visualChecks = d.value("visualChecks", true);
    if (d.contains("agents") && d["agents"].is_array())
        for (auto& a : d["agents"]) {
            DesignAgent ag = agentFromJson(a);
            if (ag.name.empty()) continue;
            // Noms en double : suffixe, pour ne pas fusionner deux thèmes
            std::string base = ag.name;
            for (int n = 2;; ++n) {
                bool taken = false;
                for (auto& x : out.agents)
                    if (lower(x.name) == lower(ag.name)) taken = true;
                if (!taken) break;
                ag.name = base + " " + std::to_string(n);
            }
            out.agents.push_back(std::move(ag));
        }
    return out;
}

DesignResponse parseDesignResponse(const std::string& text) {
    DesignResponse r;
    std::string js = extractJson(text);
    if (js.empty()) {
        r.error = "La réponse ne contient pas de JSON.";
        return r;
    }
    json j = json::parse(js, nullptr, false);
    if (!j.is_object()) {
        r.error = "JSON invalide dans la réponse.";
        return r;
    }
    if (j.contains("design") && j["design"].is_object()) {
        r.design = designFromJson(j["design"]);
        r.hasDesign = true;
        r.ok = !r.design.empty();
        if (!r.ok) r.error = "Conception vide.";
        return r;
    }
    if (j.contains("questions") && j["questions"].is_array()) {
        for (auto& q : j["questions"]) {
            DesignQuestion dq = questionFromJson(q);
            if (dq.question.empty()) continue;
            dq.choice = dq.options.empty() ? -2 : std::clamp(dq.recommended, 0, (int)dq.options.size() - 1);
            r.questions.push_back(std::move(dq));
        }
        r.ok = !r.questions.empty();
        if (!r.ok) r.error = "Questions vides.";
        return r;
    }
    // Conception sans enveloppe « design »
    if (j.contains("agents")) {
        r.design = designFromJson(j);
        r.hasDesign = r.ok = !r.design.empty();
        return r;
    }
    r.error = "Réponse inattendue (ni « design » ni « questions »).";
    return r;
}

DesignCheck checkDesign(const ProjectDesign& d) {
    DesignCheck c;
    if (util::trim(d.name).empty()) c.errors.push_back("Le projet n'a pas de nom.");
    if (hasForbidden(d.name))
        c.errors.push_back("Le nom du projet contient des caractères interdits (/ \\ : * ? \" < > |).");
    if (util::trim(d.objective).empty()) c.errors.push_back("L'objectif de l'orchestrateur est vide.");
    if (d.agents.empty()) c.errors.push_back("L'équipe n'a aucun agent.");
    for (size_t i = 0; i < d.agents.size(); ++i) {
        const auto& a = d.agents[i];
        if (util::trim(a.name).empty()) {
            c.errors.push_back("L'agent n° " + std::to_string(i + 1) + " n'a pas de nom.");
            continue;
        }
        if (hasForbidden(a.name))
            c.errors.push_back("« " + a.name + " » : caractères interdits dans le nom.");
        for (size_t k = 0; k < i; ++k)
            if (lower(util::trim(d.agents[k].name)) == lower(util::trim(a.name)))
                c.errors.push_back("Deux agents s'appellent « " + a.name + " ».");
        if (util::trim(a.role).empty()) c.warnings.push_back("« " + a.name + " » n'a pas de fiche de rôle : il ne saura pas ce qu'il possède.");
    }
    if (d.agents.size() == 1) c.warnings.push_back("Un seul agent : l'orchestrateur n'aura rien à paralléliser.");
    if (d.agents.size() > 10) c.warnings.push_back("Plus de 10 agents : beaucoup de coordination et de contexte fixe.");
    if (d.milestones.empty()) c.warnings.push_back("Aucun jalon : l'orchestrateur devra découper seul.");
    return c;
}

std::string projectNotes(const ProjectDesign& d) {
    std::string n = util::trim(d.summary) + "\n";
    if (!d.stack.empty()) {
        n += "\n## Pile\n";
        for (auto& s : d.stack) n += "- " + s + "\n";
    }
    if (!util::trim(d.conventions).empty()) n += "\n## Conventions\n" + util::trim(d.conventions) + "\n";
    if (!d.milestones.empty()) {
        n += "\n## Jalons\n";
        for (size_t i = 0; i < d.milestones.size(); ++i) n += std::to_string(i + 1) + ". " + d.milestones[i] + "\n";
    }
    n += "\n(Conçu avec le Designer de ClaudeDesk.)\n";
    return n;
}

std::string agentNotes(const DesignAgent& a) {
    // La première phrase sert de résumé à l'orchestrateur (liste des agents) : mission + modèle conseillé.
    std::string n = util::trim(a.mission);
    if (!n.empty() && n.back() != '.' && n.back() != '!' && n.back() != '?') n += ".";
    n += " Modèle conseillé : " + a.model + (a.effort.empty() ? "" : ", effort " + a.effort) + ".\n\n";
    n += util::trim(a.role) + "\n";
    return n;
}

static std::string slugFile(const std::string& name) {
    std::string s = util::sanitizeFileName(name);
    std::replace(s.begin(), s.end(), ' ', '-');
    return s.empty() ? "agent" : s;
}

std::vector<ScaffoldFile> scaffoldFiles(const ProjectDesign& d, bool claudeMd, bool plan, bool agentDocs) {
    std::vector<ScaffoldFile> v;
    if (claudeMd) {
        std::string c = "# " + d.name + "\n\n" + util::trim(d.summary) + "\n";
        if (!d.stack.empty()) {
            c += "\n## Pile\n";
            for (auto& s : d.stack) c += "- " + s + "\n";
        }
        if (!util::trim(d.conventions).empty()) c += "\n## Conventions\n" + util::trim(d.conventions) + "\n";
        c += "\n## Équipe (orchestrateur ClaudeDesk)\n"
             "Le travail est réparti entre des agents qui possèdent chacun une partie du dépôt ; ne modifie pas ce qui "
             "appartient à un autre agent sans que la tâche le demande. Fiches détaillées : `docs/agents/`.\n\n"
             "| Agent | Mission |\n|---|---|\n";
        for (auto& a : d.agents) c += "| " + a.name + " | " + a.mission + " |\n";
        if (plan) c += "\nPlan et jalons : `docs/PLAN.md`.\n";
        c += "\n## Captures\nLes captures de vérification visuelle vont dans `_captures/` (ignoré par git).\n";
        v.push_back({"CLAUDE.md", c});
    }
    if (plan) {
        std::string p = "# Plan — " + d.name + "\n\n## Objectif\n" + util::trim(d.objective) + "\n";
        if (!d.milestones.empty()) {
            p += "\n## Jalons\n";
            for (size_t i = 0; i < d.milestones.size(); ++i) p += std::to_string(i + 1) + ". " + d.milestones[i] + "\n";
        }
        v.push_back({"docs/PLAN.md", p});
    }
    if (agentDocs)
        for (auto& a : d.agents)
            v.push_back({"docs/agents/" + slugFile(a.name) + ".md", "# " + a.name + "\n\n" + agentNotes(a)});
    return v;
}

BudgetPreset budgetPresetFromName(const std::string& n) {
    if (n == "low") return BudgetPreset::Low;
    if (n == "high") return BudgetPreset::High;
    if (n == "max") return BudgetPreset::Max;
    return BudgetPreset::Medium;
}

const char* budgetName(BudgetPreset p) {
    switch (p) {
        case BudgetPreset::Low: return "low";
        case BudgetPreset::High: return "high";
        case BudgetPreset::Max: return "max";
        default: return "medium";
    }
}

void applyDesign(const ProjectDesign& d, OrchestratorSettings& s) {
    applyBudgetPreset(s, budgetPresetFromName(d.budget));
    std::string o = util::trim(d.objective);
    if (!d.milestones.empty()) {
        o += "\n\nJalons, dans l'ordre :\n";
        for (size_t i = 0; i < d.milestones.size(); ++i) o += std::to_string(i + 1) + ". " + d.milestones[i] + "\n";
    }
    s.objective = util::trim(o);
    s.visualChecks = d.visualChecks;
    s.browserAgents.clear();
    for (auto& a : d.agents)
        if (a.browser) s.browserAgents.push_back(a.name);
}

json toJson(const ProjectDesign& d) {
    json agents = json::array();
    for (auto& a : d.agents)
        agents.push_back({{"name", a.name},   {"mission", a.mission}, {"role", a.role},
                          {"model", a.model}, {"effort", a.effort},   {"browser", a.browser}});
    return {{"name", d.name},       {"summary", d.summary},         {"stack", d.stack},
            {"objective", d.objective}, {"milestones", d.milestones}, {"conventions", d.conventions},
            {"budget", d.budget},   {"visualChecks", d.visualChecks}, {"agents", agents}};
}

json toJson(const DesignQuestion& q) {
    return {{"question", q.question}, {"why", q.why},     {"options", q.options},
            {"recommended", q.recommended}, {"choice", q.choice}, {"other", q.other}};
}

DesignQuestion questionFromJson(const json& j) {
    DesignQuestion q;
    if (!j.is_object()) return q;
    q.question = str(j, "question");
    q.why = str(j, "why");
    q.options = strList(j, "options");
    q.recommended = j.contains("recommended") && j["recommended"].is_number_integer() ? j["recommended"].get<int>() : 0;
    q.choice = j.contains("choice") && j["choice"].is_number_integer() ? j["choice"].get<int>() : -1;
    q.other = j.value("other", "");
    return q;
}

json DesignerDraft::toJson() const {
    json qs = json::array(), ans = json::array(), hist = json::array();
    for (auto& q : questions) qs.push_back(claude::toJson(q));
    for (auto& q : req.answered) ans.push_back(claude::toJson(q));
    for (auto& h : history) hist.push_back(claude::toJson(h));
    return {{"step", step},
            {"request",
             {{"description", req.description}, {"folder", req.folder}, {"analyzeFolder", req.analyzeFolder},
              {"tech", req.tech}, {"constraints", req.constraints}, {"teamSize", req.teamSize}, {"budget", req.budget},
              {"allowQuestions", req.allowQuestions}, {"answered", ans}}},
            {"questions", qs},
            {"design", claude::toJson(design)},
            {"history", hist},
            {"feedback", feedback},
            {"model", model},
            {"effort", effort},
            {"permissionMode", permissionMode},
            {"cwd", cwd},
            {"writeClaudeMd", writeClaudeMd},
            {"writePlan", writePlan},
            {"writeAgentDocs", writeAgentDocs},
            {"openOrchestrator", openOrchestrator},
            {"startNow", startNow},
            {"createdProject", createdProject},
            {"lastCostUsd", lastCostUsd}};
}

void DesignerDraft::fromJson(const json& j) {
    if (!j.is_object()) return;
    step = std::clamp(j.value("step", 0), 0, 3);
    if (j.contains("request") && j["request"].is_object()) {
        const json& r = j["request"];
        req.description = r.value("description", "");
        req.folder = r.value("folder", "");
        req.analyzeFolder = r.value("analyzeFolder", false);
        req.tech = r.value("tech", "");
        req.constraints = r.value("constraints", "");
        req.teamSize = std::clamp(r.value("teamSize", 0), 0, 3);
        req.budget = r.value("budget", "medium");
        req.allowQuestions = r.value("allowQuestions", true);
        req.answered.clear();
        for (auto& q : r.value("answered", json::array())) req.answered.push_back(questionFromJson(q));
    }
    questions.clear();
    for (auto& q : j.value("questions", json::array())) questions.push_back(questionFromJson(q));
    design = designFromJson(j.value("design", json::object()));
    history.clear();
    for (auto& h : j.value("history", json::array())) history.push_back(designFromJson(h));
    feedback = j.value("feedback", "");
    model = j.value("model", model);
    effort = j.value("effort", effort);
    permissionMode = j.value("permissionMode", permissionMode);
    cwd = j.value("cwd", "");
    writeClaudeMd = j.value("writeClaudeMd", true);
    writePlan = j.value("writePlan", true);
    writeAgentDocs = j.value("writeAgentDocs", true);
    openOrchestrator = j.value("openOrchestrator", true);
    startNow = j.value("startNow", false);
    createdProject = j.value("createdProject", "");
    lastCostUsd = j.value("lastCostUsd", 0.0);
    if (step == 1 && questions.empty()) step = 0;
    if (step >= 2 && design.empty()) step = 0;
}

ProjectDesign demoDesign() {
    ProjectDesign d;
    d.name = "Carnet de recettes";
    d.summary = "Application web de gestion de recettes : saisie, recherche par ingrédient, liste de courses générée "
                "pour la semaine. Choix faits : front React + Vite, API Node/Express, base SQLite (un seul utilisateur).";
    d.stack = {"TypeScript", "React + Vite", "Node.js / Express", "SQLite", "Vitest", "Playwright"};
    d.objective = "Livrer une application utilisable localement (npm run dev) : créer, modifier et rechercher des "
                  "recettes, planifier la semaine et obtenir la liste de courses agrégée. Critères : tests unitaires et "
                  "de bout en bout au vert, captures des trois écrans principaux. L'essentiel d'abord : la saisie et la "
                  "recherche, puis la planification.";
    d.milestones = {"Squelette du dépôt, build et tests qui tournent", "API des recettes (CRUD + recherche) testée",
                    "Écrans Recettes et Recherche", "Planning de la semaine et liste de courses",
                    "Tests de bout en bout et finitions visuelles"};
    d.conventions = "- `npm run build` et `npm test` doivent passer avant chaque compte rendu\n- Textes de l'interface en français\n"
                    "- Un composant React par fichier, CSS modules";
    d.budget = "medium";
    d.agents = {
        {"01 Fondations", "Dépôt, outillage, build, tests et intégration continue.",
         "## Mission\nMettre en place et garder sain le socle du dépôt.\n\n## Possède\n- `package.json`, configs (Vite, "
         "TS, ESLint, Vitest, Playwright)\n- `.github/`\n\n## Frontières\nNe touche pas au code applicatif (`server/`, "
         "`web/`).\n\n## Vérifications\n`npm run build && npm test`",
         "sonnet", "medium", false},
        {"02 API", "API Express et base SQLite : modèles, routes, recherche par ingrédient.",
         "## Mission\nConcevoir et implémenter l'API.\n\n## Possède\n- `server/`\n\n## Frontières\nLe front consomme "
         "l'API via `web/src/api.ts` (appartient à 03 Interface).\n\n## Vérifications\n`npm test -- server`",
         "sonnet", "medium", false},
        {"03 Interface", "Écrans React : recettes, recherche, planning, liste de courses.",
         "## Mission\nConstruire l'interface.\n\n## Possède\n- `web/`\n\n## Frontières\nN'écrit pas dans `server/`.\n\n"
         "## Vérifications\n`npm test -- web` et une capture de chaque écran modifié dans `_captures/`.",
         "sonnet", "medium", false},
        {"04 Recette", "Tests de bout en bout et vérification visuelle dans le navigateur.",
         "## Mission\nVérifier l'application comme un utilisateur.\n\n## Possède\n- `e2e/`\n\n## Frontières\nSignale "
         "les défauts dans son compte rendu, ne corrige pas le code des autres.\n\n## Vérifications\n`npx playwright "
         "test`, captures dans `_captures/`.",
         "haiku", "medium", true},
    };
    return d;
}

}  // namespace claude
