#include "claude/Orchestrator.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "util/Strings.h"

namespace claude {

using json = nlohmann::json;

namespace {

// Clé de comparaison : minuscules, accents de « DÉLÉGUER » ramenés, espaces retirés.
std::string normKey(std::string s) {
    util::replaceAll(s, "É", "E");
    util::replaceAll(s, "é", "e");
    util::replaceAll(s, "È", "E");
    util::replaceAll(s, "è", "e");
    return util::toLower(util::trim(s));
}

// Retire les décorations Markdown autour d'un nom d'agent (« **02 Build** », `x`, « … »).
std::string cleanName(std::string s) {
    static const std::vector<std::string> deco = {"**", "*", "`", "\"", "'", ":", "«", "»", "<", ">", " ", "\t"};
    bool again = true;
    while (again && !s.empty()) {
        again = false;
        for (auto& d : deco) {
            if (util::startsWith(s, d)) s.erase(0, d.size()), again = true;
            if (util::endsWith(s, d)) s.erase(s.size() - d.size()), again = true;
        }
    }
    return s;
}

// Partie d'une ligne après le mot-clé (« @@DELEGUER 02 Build » -> « 02 Build »).
std::string afterKeyword(const std::string& line) {
    size_t p = line.find_first_of(" \t:");
    return p == std::string::npos ? std::string() : util::trim(line.substr(p));
}

int leadingNumber(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
    return i == 0 ? -1 : std::atoi(s.substr(0, i).c_str());
}

const char* stateKey(OrchState s) {
    switch (s) {
        case OrchState::Idle: return "idle";
        case OrchState::Thinking: return "thinking";
        case OrchState::Working: return "working";
        case OrchState::WaitingHuman: return "waitingHuman";
        case OrchState::Paused: return "paused";
        case OrchState::UsagePause: return "usagePause";
        case OrchState::Finished: return "finished";
    }
    return "idle";
}

OrchState stateFromKey(const std::string& k) {
    for (OrchState s : {OrchState::Idle, OrchState::Thinking, OrchState::Working, OrchState::WaitingHuman, OrchState::Paused,
                        OrchState::UsagePause, OrchState::Finished})
        if (k == stateKey(s)) return s;
    return OrchState::Idle;
}

}  // namespace

// ---------------------------------------------------------------------------
// Directives
// ---------------------------------------------------------------------------

Directives parseDirectives(const std::string& text) {
    Directives d;
    enum class Mode { None, Delegate, Human, Done } mode = Mode::None;
    std::string agent, body;
    auto flush = [&] {
        std::string b = util::trim(body);
        switch (mode) {
            case Mode::Delegate:
                if (!agent.empty()) d.delegations.push_back({agent, b});
                break;
            case Mode::Human:
                d.needHuman = true;
                d.question += (d.question.empty() ? "" : "\n\n") + b;
                break;
            case Mode::Done:
                d.finished = true;
                d.summary += (d.summary.empty() ? "" : "\n\n") + b;
                break;
            case Mode::None: break;
        }
        mode = Mode::None;
        agent.clear();
        body.clear();
    };
    for (const std::string& raw : util::splitLines(text)) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = util::trim(line);
        std::string k = normKey(t);
        if (util::startsWith(k, "@@")) {
            if (util::startsWith(k, "@@fin")) {
                flush();
                continue;
            }
            if (util::startsWith(k, "@@deleguer")) {
                flush();
                mode = Mode::Delegate;
                agent = cleanName(afterKeyword(t));
                continue;
            }
            if (util::startsWith(k, "@@humain")) {
                flush();
                mode = Mode::Human;
                body = afterKeyword(t) + "\n";
                continue;
            }
            if (util::startsWith(k, "@@termine")) {
                flush();
                mode = Mode::Done;
                body = afterKeyword(t) + "\n";
                continue;
            }
        }
        if (mode != Mode::None) body += line + "\n";
    }
    flush();  // bloc non refermé : accepté
    return d;
}

int matchAgent(const std::string& name, const std::vector<std::string>& agents) {
    std::string n = normKey(cleanName(name));
    if (n.empty()) return -1;
    for (size_t i = 0; i < agents.size(); ++i)
        if (normKey(agents[i]) == n) return (int)i;
    // Numéro en tête (« 02 », « 2 », « 02 - Build »)
    if (int num = leadingNumber(n); num >= 0) {
        int found = -1, count = 0;
        for (size_t i = 0; i < agents.size(); ++i)
            if (leadingNumber(normKey(agents[i])) == num) found = (int)i, ++count;
        if (count == 1) return found;
    }
    // Fragment unique
    int found = -1, count = 0;
    for (size_t i = 0; i < agents.size(); ++i) {
        std::string a = normKey(agents[i]);
        if (a.find(n) != std::string::npos || n.find(a) != std::string::npos) found = (int)i, ++count;
    }
    return count == 1 ? found : -1;
}

std::string lastTurnText(const std::vector<Entry>& entries) {
    size_t start = 0;
    for (size_t i = entries.size(); i-- > 0;)
        if (entries[i].kind == EntryKind::User && entries[i].parentToolUseId.empty()) {
            start = i + 1;
            break;
        }
    std::string out;
    for (size_t i = start; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        if (!e.parentToolUseId.empty()) continue;
        std::string piece;
        if (e.kind == EntryKind::Assistant) piece = e.text;
        else if (e.kind == EntryKind::Error) piece = "[Erreur] " + e.text;
        else continue;
        if (util::trim(piece).empty()) continue;
        if (!out.empty()) out += "\n\n";
        out += piece;
    }
    return out;
}

const char* orchStateLabel(OrchState s) {
    switch (s) {
        case OrchState::Idle: return "Prêt";
        case OrchState::Thinking: return "L'orchestrateur réfléchit";
        case OrchState::Working: return "Les agents travaillent";
        case OrchState::WaitingHuman: return "Attend votre réponse";
        case OrchState::Paused: return "En pause";
        case OrchState::UsagePause: return "Pause : limite d'usage";
        case OrchState::Finished: return "Terminé";
    }
    return "";
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

std::string Orchestrator::systemPrompt() {
    return R"(# Rôle : ORCHESTRATEUR (piloté par ClaudeDesk)
Tu coordonnes des agents spécialisés. Chaque agent est une conversation Claude Code distincte, dans le même dépôt, avec son propre rôle (décrit dans les notes de son dossier). Tu ne développes pas toi-même : tu planifies, tu délègues, tu vérifies et tu décides de la suite. Tu peux lire le dépôt et lancer des commandes de vérification (tests, build) pour contrôler le travail rapporté.

## Protocole (obligatoire)
Termine CHAQUE réponse par un ou plusieurs blocs de directives, chaque marqueur seul sur sa ligne :

@@DELEGUER <nom exact de l'agent>
<tâche précise, autonome et vérifiable : contexte utile, fichiers concernés, critère de fin>
@@FIN

@@HUMAIN
<question ou décision qui exige l'humain>
@@FIN

@@TERMINE
<bilan final : ce qui est livré, comment c'est vérifié, limites connues>
@@FIN

## Règles
- Plusieurs @@DELEGUER vers des agents différents s'exécutent en parallèle : ne parallélise que des tâches indépendantes. Un agent ne reçoit qu'une tâche à la fois.
- L'agent ne voit PAS cette conversation : donne-lui tout le contexte nécessaire dans la tâche.
- Après chaque cycle, ClaudeDesk te transmet les comptes rendus des agents. Vérifie (lis les fichiers, lance les tests) avant de considérer une étape comme faite ; renvoie une correction si besoin.
- Découpe en étapes de taille raisonnable : une tâche ≈ un livrable vérifiable.
- @@HUMAIN pour : décision majeure non tranchée, information manquante, action irréversible ou externe (commit, push, publication, achat, matériel physique). Ne demande jamais de commit ou de push aux agents.
- @@TERMINE seulement quand l'objectif est atteint et vérifié.
- Sans directive, l'orchestration ne peut pas avancer.)";
}

std::string Orchestrator::firstMessage(const std::vector<AgentInfo>& agents) const {
    std::string m = "# Objectif\n" + util::trim(settings.objective) + "\n\n# Agents disponibles\n";
    for (auto& a : agents) m += "- **" + a.name + "**" + (a.brief.empty() ? "" : " — " + a.brief) + "\n";
    m += "\nUtilise exactement ces noms dans @@DELEGUER. Jusqu'à " + std::to_string(std::max(1, settings.maxParallel)) +
         " agents travaillent en parallèle. Commence par examiner l'état du dépôt (plan, documentation, code existant), "
         "puis donne tes premières directives.";
    return m;
}

static std::string taskMessage(const OrchTask& t) {
    return "[Tâche de l'orchestrateur — cycle " + std::to_string(t.cycle) + "]\n\n" + t.task +
           "\n\n---\nTravaille en autonomie jusqu'au bout de cette tâche (pas de question à l'humain sauf décision "
           "irréversible ; ni commit ni push). Termine ta réponse par un compte rendu court : **Fait**, **Vérifié** "
           "(comment), **Reste / bloquant**.";
}

std::string Orchestrator::buildReport() const {
    std::string r = "# Comptes rendus — cycle " + std::to_string(cycle) + "\n\n";
    for (auto& t : tasks) {
        r += "## " + t.agent + (t.failed ? " — ÉCHEC" : " — terminé") + "\n";
        r += "**Tâche :** " + util::firstLine(t.task, 200) + "\n\n";
        std::string rep = util::trim(t.report).empty() ? "(aucun texte rendu)" : t.report;
        if (rep.size() > 8000) rep = util::truncateUtf8(rep, 8000) + "\n(… compte rendu tronqué)";
        r += rep + "\n\n";
    }
    r += "---\nDépense de l'orchestration : " + util::formatUsd(spentUsd);
    if (settings.maxCostUsd > 0) r += " / " + util::formatUsd(settings.maxCostUsd);
    r += " · cycle " + std::to_string(cycle);
    if (settings.maxCycles > 0) r += " / " + std::to_string(settings.maxCycles);
    r += ".\nVérifie ce qui doit l'être, puis donne tes directives (@@DELEGUER / @@HUMAIN / @@TERMINE).";
    return r;
}

// ---------------------------------------------------------------------------
// Moteur
// ---------------------------------------------------------------------------

bool Orchestrator::running() const {
    return state == OrchState::Thinking || state == OrchState::Working || state == OrchState::WaitingHuman ||
           state == OrchState::UsagePause;
}

void Orchestrator::addLog(OrchestratorHost* h, const std::string& text, bool important) {
    log.push_back({h ? h->now() : util::nowUnix(), text, important});
    while (log.size() > 500) log.pop_front();
    changed = true;
    if (h) h->onEvent(text, important);
}

void Orchestrator::pauseFrom(OrchState from, const std::string& reason, OrchestratorHost* h) {
    resumeState_ = from;
    state = OrchState::Paused;
    pauseReason = reason;
    changed = true;
    addLog(h, "Pause : " + reason, true);
}

bool Orchestrator::start(OrchestratorHost& h) {
    if (util::trim(settings.objective).empty()) {
        addLog(&h, "Indiquez d'abord l'objectif de l'orchestration.", true);
        return false;
    }
    std::vector<AgentInfo> ag = h.agents();
    if (ag.empty()) {
        addLog(&h, "Ce projet n'a aucun thème : créez un dossier par agent.", true);
        return false;
    }
    cycle = 0;
    spentUsd = 0;
    tasks.clear();
    log.clear();
    finalSummary.clear();
    humanQuestion.clear();
    pauseReason.clear();
    nudges_ = 0;
    std::string id = h.openBrain(brainConvId, systemPrompt());
    if (id.empty()) {
        addLog(&h, "Impossible d'ouvrir la conversation de l'orchestrateur.", true);
        return false;
    }
    brainConvId = id;
    addLog(&h, "Démarrage — " + util::firstLine(settings.objective, 120) + " (" + std::to_string(ag.size()) + " agents)", true);
    if (!usageOk(h)) {
        // Reprise automatique : le premier message partira à la levée de la pause.
        lastBrainMessage_ = firstMessage(ag);
        brainTurnsAtSend_ = h.status(brainConvId).turns;
        resumeState_ = OrchState::Thinking;
        return true;
    }
    return sendBrain(h, firstMessage(ag));
}

bool Orchestrator::sendBrain(OrchestratorHost& h, const std::string& text) {
    ParticipantStatus st = h.status(brainConvId);
    brainTurnsAtSend_ = st.turns;
    brainCostAtSend_ = st.costUsd;
    lastBrainMessage_ = text;
    brainHumanNotified_ = false;
    if (!h.send(brainConvId, text, settings.brainMode)) {
        pauseFrom(OrchState::Thinking, "envoi impossible à l'orchestrateur (Claude Code introuvable ?)", &h);
        return false;
    }
    state = OrchState::Thinking;
    changed = true;
    return true;
}

void Orchestrator::pause(const std::string& reason) {
    if (!running()) return;
    if (state != OrchState::UsagePause) resumeState_ = state;
    state = OrchState::Paused;
    pauseReason = reason;
    addLog(nullptr, "Pause : " + reason, true);
}

void Orchestrator::stop() {
    if (state == OrchState::Idle) return;
    state = OrchState::Idle;
    pauseReason.clear();
    addLog(nullptr, "Orchestration arrêtée.", true);
}

void Orchestrator::resume(OrchestratorHost& h) {
    if (state != OrchState::Paused && state != OrchState::UsagePause) return;
    OrchState to = resumeState_;
    if (to != OrchState::Working && to != OrchState::WaitingHuman) to = OrchState::Thinking;
    pauseReason.clear();
    if (!brainConvId.empty()) h.ensureOpen(brainConvId);
    for (auto& t : tasks)
        if (t.sent && !t.done) h.ensureOpen(t.convId);
    state = to;
    changed = true;
    addLog(&h, "Reprise.", true);
    if (to == OrchState::Thinking) {
        ParticipantStatus st = h.status(brainConvId);
        if (!st.exists) {
            pauseFrom(OrchState::Thinking, "conversation de l'orchestrateur introuvable", &h);
            return;
        }
        // Le dernier message n'a pas obtenu de réponse (fermeture, plantage…) : on le renvoie.
        if (!st.busy && st.turns <= brainTurnsAtSend_)
            sendBrain(h, lastBrainMessage_.empty() ? "Reprends là où tu en étais et donne tes directives." : lastBrainMessage_);
    } else if (to == OrchState::Working) {
        for (auto& t : tasks) {
            if (!t.sent || t.done) continue;
            ParticipantStatus st = h.status(t.convId);
            if (!st.busy && st.turns <= t.turnsAtSend) t.sent = false;  // renvoyée par dispatch
        }
    }
}

bool Orchestrator::budgetOk(OrchestratorHost& h) {
    if (settings.maxCostUsd > 0 && spentUsd >= settings.maxCostUsd) {
        pauseFrom(state, "budget atteint (" + util::formatUsd(spentUsd) + " / " + util::formatUsd(settings.maxCostUsd) +
                             ") — augmentez-le puis reprenez",
                  &h);
        return false;
    }
    return true;
}

bool Orchestrator::usageOk(OrchestratorHost& h) {
    double u = h.fiveHourUtilization();
    int64_t resetsAt = h.fiveHourResetsAt();
    if (resetsAt > 0 && h.now() > resetsAt) return true;  // mesure périmée : la fenêtre a été réinitialisée
    if (settings.usagePause > 0 && u >= settings.usagePause) {
        resumeState_ = state;
        state = OrchState::UsagePause;
        pauseReason = "fenêtre de 5 h à " + std::to_string((int)(u * 100 + 0.5)) + " % : reprise automatique à sa réinitialisation";
        changed = true;
        addLog(&h, "Pause : " + pauseReason, true);
        return false;
    }
    return true;
}

void Orchestrator::tick(OrchestratorHost& h) {
    switch (state) {
        case OrchState::Thinking: tickThinking(h); break;
        case OrchState::Working: tickWorking(h); break;
        case OrchState::WaitingHuman: {
            ParticipantStatus st = h.status(brainConvId);
            if (st.exists && !st.busy && st.turns > brainTurnsAtSend_) {
                spentUsd += std::max(0.0, st.costUsd - brainCostAtSend_);
                humanQuestion.clear();
                addLog(&h, "Vous avez répondu : l'orchestrateur reprend.");
                handleBrainAnswer(h, st.lastText);
            }
            break;
        }
        case OrchState::UsagePause: {
            double u = h.fiveHourUtilization();
            int64_t r = h.fiveHourResetsAt();
            if (settings.usagePause <= 0 || u < settings.usagePause || (r > 0 && h.now() > r + 30)) {
                OrchState to = resumeState_;
                state = OrchState::Paused;  // passe par resume() : renvoie ce qui doit l'être
                resumeState_ = to;
                addLog(&h, "Limite d'usage levée.");
                resume(h);
            }
            break;
        }
        default: break;
    }
}

void Orchestrator::tickThinking(OrchestratorHost& h) {
    ParticipantStatus st = h.status(brainConvId);
    if (!st.exists) {
        pauseFrom(OrchState::Thinking, "la conversation de l'orchestrateur a été fermée", &h);
        return;
    }
    if (st.needsHuman && !brainHumanNotified_) {
        brainHumanNotified_ = true;
        addLog(&h, "L'orchestrateur attend votre autorisation (voir sa conversation).", true);
    }
    if (st.busy) return;
    if (st.turns > brainTurnsAtSend_) {
        spentUsd += std::max(0.0, st.costUsd - brainCostAtSend_);
        handleBrainAnswer(h, st.lastText);
        return;
    }
    pauseFrom(OrchState::Thinking, "le processus de l'orchestrateur s'est arrêté avant de répondre", &h);
}

void Orchestrator::handleBrainAnswer(OrchestratorHost& h, const std::string& text) {
    Directives d = parseDirectives(text);
    changed = true;
    if (d.finished) {
        state = OrchState::Finished;
        finalSummary = d.summary;
        addLog(&h, "Objectif atteint : " + util::firstLine(d.summary, 150), true);
        return;
    }
    if (d.needHuman) {
        state = OrchState::WaitingHuman;
        humanQuestion = d.question;
        ParticipantStatus st = h.status(brainConvId);
        brainTurnsAtSend_ = st.turns;
        brainCostAtSend_ = st.costUsd;
        std::string extra = d.delegations.empty() ? "" : " (ses délégations attendront votre réponse)";
        addLog(&h, "Question pour vous : " + util::firstLine(d.question, 150) + " — répondez dans la conversation « Orchestrateur »" + extra,
               true);
        return;
    }
    if (d.delegations.empty()) {
        if (++nudges_ > 2) {
            state = OrchState::WaitingHuman;
            humanQuestion = "L'orchestrateur ne donne plus de directives : écrivez-lui dans sa conversation pour le relancer.";
            ParticipantStatus st = h.status(brainConvId);
            brainTurnsAtSend_ = st.turns;
            brainCostAtSend_ = st.costUsd;
            addLog(&h, humanQuestion, true);
            return;
        }
        addLog(&h, "Réponse sans directive : relance de l'orchestrateur.");
        sendBrain(h, "Ta réponse ne contient aucune directive. Termine par un bloc @@DELEGUER <agent> … @@FIN, "
                     "@@HUMAIN … @@FIN ou @@TERMINE … @@FIN (voir le protocole).");
        return;
    }
    nudges_ = 0;
    std::vector<AgentInfo> ag = h.agents();
    std::vector<std::string> names;
    for (auto& a : ag) names.push_back(a.name);
    std::string available;
    for (auto& n : names) available += (available.empty() ? "" : ", ") + n;

    ++cycle;
    tasks.clear();
    int valid = 0;
    for (auto& del : d.delegations) {
        OrchTask t;
        t.cycle = cycle;
        t.task = del.task;
        int idx = matchAgent(del.agent, names);
        if (idx < 0) {
            t.agent = del.agent;
            t.done = t.failed = true;
            t.report = "Agent inconnu « " + del.agent + " ». Agents disponibles : " + available + ".";
            addLog(&h, "Agent inconnu : « " + del.agent + " »", true);
        } else {
            t.agent = names[idx];
            ++valid;
        }
        tasks.push_back(std::move(t));
    }
    if (valid == 0) {
        sendBrain(h, buildReport());
        return;
    }
    state = OrchState::Working;
    addLog(&h, "Cycle " + std::to_string(cycle) + " : " + std::to_string(valid) + " tâche(s) déléguée(s).", true);
    dispatch(h);
}

void Orchestrator::dispatch(OrchestratorHost& h) {
    int active = 0;
    for (auto& t : tasks)
        if (t.sent && !t.done) ++active;
    for (auto& t : tasks) {
        if (state != OrchState::Working) return;
        if (t.sent || t.done) continue;
        if (active >= std::max(1, settings.maxParallel)) break;
        bool agentBusy = false;
        for (auto& o : tasks)
            if (&o != &t && o.agent == t.agent && o.sent && !o.done) agentBusy = true;
        if (agentBusy) continue;
        if (!budgetOk(h) || !usageOk(h)) return;
        std::string id = h.openAgent(t.agent, agentConv.count(t.agent) ? agentConv[t.agent] : std::string());
        if (id.empty()) {
            t.done = t.failed = true;
            t.report = "Impossible d'ouvrir la conversation de l'agent.";
            addLog(&h, t.agent + " : conversation introuvable.", true);
            continue;
        }
        agentConv[t.agent] = id;
        ParticipantStatus st = h.status(id);
        if (st.busy) continue;  // l'utilisateur s'en sert : la tâche partira au prochain passage
        t.convId = id;
        t.turnsAtSend = st.turns;
        t.costAtSend = st.costUsd;
        if (!h.send(id, taskMessage(t), settings.workerMode)) {
            t.done = t.failed = true;
            t.report = "Envoi impossible (Claude Code introuvable ?).";
            addLog(&h, t.agent + " : envoi impossible.", true);
            continue;
        }
        t.sent = true;
        t.sentAt = h.now();
        ++active;
        changed = true;
        addLog(&h, "→ " + t.agent + " : " + util::firstLine(t.task, 100));
    }
}

void Orchestrator::tickWorking(OrchestratorHost& h) {
    for (auto& t : tasks) {
        if (!t.sent || t.done) continue;
        ParticipantStatus st = h.status(t.convId);
        if (!st.exists) {
            t.done = t.failed = true;
            t.doneAt = h.now();
            t.report = "(conversation fermée avant la fin de la tâche)";
            addLog(&h, t.agent + " : conversation fermée.", true);
            continue;
        }
        if (st.needsHuman && !t.notifiedHuman) {
            t.notifiedHuman = true;
            addLog(&h, t.agent + " attend votre autorisation.", true);
        }
        if (st.busy) continue;
        t.done = true;
        t.doneAt = h.now();
        changed = true;
        if (st.turns > t.turnsAtSend) {
            t.report = st.lastText;
            spentUsd += std::max(0.0, st.costUsd - t.costAtSend);
            addLog(&h, "✓ " + t.agent + " a terminé.");
        } else {
            t.failed = true;
            t.report = "(le processus de l'agent s'est arrêté avant la fin du tour)";
            addLog(&h, t.agent + " : processus arrêté.", true);
        }
    }
    dispatch(h);
    if (state != OrchState::Working) return;
    for (auto& t : tasks)
        if (!t.done) return;
    if (h.status(brainConvId).busy) return;  // l'utilisateur discute avec l'orchestrateur
    if (settings.maxCycles > 0 && cycle >= settings.maxCycles) {
        pauseFrom(OrchState::Working,
                  "nombre maximal de cycles atteint (" + std::to_string(settings.maxCycles) + ") — augmentez-le puis reprenez", &h);
        return;
    }
    if (!budgetOk(h) || !usageOk(h)) return;
    addLog(&h, "Comptes rendus du cycle " + std::to_string(cycle) + " envoyés à l'orchestrateur.");
    sendBrain(h, buildReport());
}

// ---------------------------------------------------------------------------
// Persistance
// ---------------------------------------------------------------------------

json Orchestrator::toJson() const {
    json jt = json::array();
    for (auto& t : tasks)
        jt.push_back({{"agent", t.agent}, {"task", t.task}, {"convId", t.convId}, {"cycle", t.cycle},
                      {"turnsAtSend", t.turnsAtSend}, {"costAtSend", t.costAtSend}, {"sent", t.sent}, {"done", t.done},
                      {"failed", t.failed}, {"report", t.report}, {"sentAt", t.sentAt}, {"doneAt", t.doneAt}});
    json jl = json::array();
    for (auto& l : log) jl.push_back({{"t", l.time}, {"text", l.text}, {"important", l.important}});
    return {{"version", 1},
            {"settings",
             {{"objective", settings.objective},
              {"maxCycles", settings.maxCycles},
              {"maxCostUsd", settings.maxCostUsd},
              {"maxParallel", settings.maxParallel},
              {"workerMode", settings.workerMode},
              {"brainMode", settings.brainMode},
              {"usagePause", settings.usagePause}}},
            {"state", stateKey(state)},
            {"resumeState", stateKey(resumeState_)},
            {"pauseReason", pauseReason},
            {"cycle", cycle},
            {"spentUsd", spentUsd},
            {"brainConvId", brainConvId},
            {"agentConv", agentConv},
            {"tasks", jt},
            {"log", jl},
            {"finalSummary", finalSummary},
            {"humanQuestion", humanQuestion},
            {"brainTurnsAtSend", brainTurnsAtSend_},
            {"brainCostAtSend", brainCostAtSend_},
            {"lastBrainMessage", lastBrainMessage_},
            {"nudges", nudges_}};
}

void Orchestrator::fromJson(const json& j) {
    json s = j.value("settings", json::object());
    settings.objective = s.value("objective", std::string());
    settings.maxCycles = s.value("maxCycles", settings.maxCycles);
    settings.maxCostUsd = s.value("maxCostUsd", settings.maxCostUsd);
    settings.maxParallel = s.value("maxParallel", settings.maxParallel);
    settings.workerMode = s.value("workerMode", settings.workerMode);
    settings.brainMode = s.value("brainMode", settings.brainMode);
    settings.usagePause = s.value("usagePause", settings.usagePause);
    state = stateFromKey(j.value("state", std::string("idle")));
    resumeState_ = stateFromKey(j.value("resumeState", std::string("idle")));
    pauseReason = j.value("pauseReason", std::string());
    cycle = j.value("cycle", 0);
    spentUsd = j.value("spentUsd", 0.0);
    brainConvId = j.value("brainConvId", std::string());
    agentConv.clear();
    if (j.contains("agentConv") && j["agentConv"].is_object())
        for (auto& [k, v] : j["agentConv"].items())
            if (v.is_string()) agentConv[k] = v.get<std::string>();
    tasks.clear();
    for (auto& jt : j.value("tasks", json::array())) {
        OrchTask t;
        t.agent = jt.value("agent", std::string());
        t.task = jt.value("task", std::string());
        t.convId = jt.value("convId", std::string());
        t.cycle = jt.value("cycle", 0);
        t.turnsAtSend = jt.value("turnsAtSend", 0);
        t.costAtSend = jt.value("costAtSend", 0.0);
        t.sent = jt.value("sent", false);
        t.done = jt.value("done", false);
        t.failed = jt.value("failed", false);
        t.report = jt.value("report", std::string());
        t.sentAt = jt.value("sentAt", (int64_t)0);
        t.doneAt = jt.value("doneAt", (int64_t)0);
        tasks.push_back(std::move(t));
    }
    log.clear();
    for (auto& jl : j.value("log", json::array()))
        log.push_back({jl.value("t", (int64_t)0), jl.value("text", std::string()), jl.value("important", false)});
    finalSummary = j.value("finalSummary", std::string());
    humanQuestion = j.value("humanQuestion", std::string());
    brainTurnsAtSend_ = j.value("brainTurnsAtSend", 0);
    brainCostAtSend_ = j.value("brainCostAtSend", 0.0);
    lastBrainMessage_ = j.value("lastBrainMessage", std::string());
    nudges_ = j.value("nudges", 0);
    // Rechargée en cours de route (ClaudeDesk fermé) : les processus ont disparu,
    // on attend une reprise explicite.
    if (state == OrchState::Thinking || state == OrchState::Working || state == OrchState::WaitingHuman) {
        resumeState_ = state;
        state = OrchState::Paused;
        pauseReason = "ClaudeDesk a été fermé pendant l'orchestration — cliquez sur Reprendre";
    } else if (state == OrchState::UsagePause) {
        state = OrchState::Paused;
        pauseReason = "pause d'usage en cours à la fermeture — cliquez sur Reprendre";
    }
    changed = false;
}

}  // namespace claude
