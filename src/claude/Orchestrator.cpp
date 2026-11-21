#include "claude/Orchestrator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

// Retire les décorations Markdown autour d'un nom (« **02 Build** », `x`, « … »).
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

// « 02 Build | modele=opus effort=high » -> agent + paramètres.
void parseDelegationHeader(const std::string& header, Delegation& d) {
    size_t bar = header.find('|');
    d.agent = cleanName(bar == std::string::npos ? header : header.substr(0, bar));
    if (bar == std::string::npos) return;
    std::string params = header.substr(bar + 1);
    for (char& c : params)
        if (c == ',' || c == ';' || c == '|') c = ' ';
    for (const std::string& tok : util::split(params, ' ')) {
        size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        std::string k = normKey(tok.substr(0, eq));
        std::string v = util::toLower(cleanName(tok.substr(eq + 1)));
        if (k == "modele" || k == "model") d.model = v;
        else if (k == "effort") d.effort = v;
    }
}

// Ligne d'option : « - x », « * x », « • x », « 1. x », « 2) x », « A) x », « b. x ».
bool optionLine(const std::string& t, std::string& out) {
    auto rest = [&](size_t n) {
        out = util::trim(t.substr(n));
        return !out.empty();
    };
    if (util::startsWith(t, "- ") || util::startsWith(t, "* ")) return rest(2);
    if (util::startsWith(t, "• ")) return rest(std::string("• ").size());
    size_t i = 0;
    while (i < t.size() && std::isdigit((unsigned char)t[i])) ++i;
    if (i > 0 && i + 1 < t.size() && (t[i] == '.' || t[i] == ')') && t[i + 1] == ' ') return rest(i + 2);
    // « a) x », « B) x », « C. x » (lettre minuscule suivie d'un point exclue : trop ambigu)
    if (t.size() > 3 && std::isalpha((unsigned char)t[0]) && t[2] == ' ' &&
        (t[1] == ')' || (t[1] == '.' && std::isupper((unsigned char)t[0]))))
        return rest(3);
    return false;
}

int effortRank(const std::string& e) {
    static const char* order[] = {"low", "medium", "high", "xhigh", "max"};
    for (int i = 0; i < 5; ++i)
        if (e == order[i]) return i + 1;
    return 0;
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

std::string pct(double f) { return std::to_string((int)std::lround(f * 100)) + " %"; }

}  // namespace

// ---------------------------------------------------------------------------
// Directives
// ---------------------------------------------------------------------------

namespace {

int indentOf(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else if (c == '\t') n += 4;
        else break;
    }
    return n;
}

// « - [ ] x », « - [x] x », « [ ] x » : choix explicite.
bool checkboxLine(const std::string& t, std::string& out) {
    std::string u = t;
    if (util::startsWith(u, "- ") || util::startsWith(u, "* ")) u = util::trim(u.substr(2));
    for (const char* box : {"[ ] ", "[x] ", "[X] "})
        if (util::startsWith(u, box)) {
            out = util::trim(u.substr(4));
            return !out.empty();
        }
    return false;
}

// « 1. x » / « 2) x » (numéro seul, pour repérer des questions numérotées).
bool numberedLine(const std::string& t, std::string& out) {
    size_t i = 0;
    while (i < t.size() && std::isdigit((unsigned char)t[i])) ++i;
    if (i > 0 && i + 1 < t.size() && (t[i] == '.' || t[i] == ')') && t[i + 1] == ' ') {
        out = util::trim(t.substr(i + 2));
        return !out.empty();
    }
    return false;
}

// Intitulé court : première ligne non vide, sans décorations Markdown.
std::string questionTitle(const std::string& text) {
    for (const std::string& raw : util::splitLines(text)) {
        std::string t = util::trim(raw);
        while (!t.empty() && t[0] == '#') t.erase(0, 1);
        util::replaceAll(t, "**", "");
        util::replaceAll(t, "`", "");
        t = util::trim(t);
        if (!t.empty()) return util::truncateUtf8(t, 160);
    }
    return {};
}

// Réponses suggérées : « … » placés dans une phrase qui parle de répondre.
std::vector<std::string> suggestedReplies(const std::string& text) {
    std::vector<std::string> out;
    const std::string open = "«", close = "»";
    size_t p = 0;
    while ((p = text.find(open, p)) != std::string::npos) {
        size_t e = text.find(close, p);
        if (e == std::string::npos) break;
        size_t sentence = text.rfind('.', p);
        sentence = sentence == std::string::npos ? 0 : sentence;
        std::string before = util::toLower(text.substr(sentence, p - sentence));
        std::string q = util::trim(text.substr(p + open.size(), e - p - open.size()));
        if (before.find("répon") != std::string::npos && !q.empty() && q.size() < 120 &&
            std::find(out.begin(), out.end(), q) == out.end())
            out.push_back(q);
        p = e + close.size();
    }
    return out;
}

}  // namespace

HumanQuestion splitQuestion(const std::string& body) {
    HumanQuestion q;
    std::vector<std::string> lines = util::splitLines(body);
    for (auto& l : lines)
        if (!l.empty() && l.back() == '\r') l.pop_back();
    // 1) Cases à cocher explicites, n'importe où dans le bloc.
    std::vector<std::string> boxes;
    std::string boxText;
    for (auto& raw : lines) {
        std::string opt;
        if (indentOf(raw) < 2 && checkboxLine(util::trim(raw), opt)) boxes.push_back(opt);
        else boxText += raw + "\n";
    }
    if (boxes.size() >= 2) {
        q.options = boxes;
        q.text = util::trim(boxText);
    } else {
        // 2) Liste finale de premier niveau : rien d'autre que des options (ou leur suite indentée) après elle.
        size_t first = lines.size();
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string opt;
            if (indentOf(lines[i]) < 2 && optionLine(util::trim(lines[i]), opt)) {
                first = i;
                break;
            }
        }
        std::vector<std::string> opts;
        bool trailing = first < lines.size();
        for (size_t i = first; trailing && i < lines.size(); ++i) {
            std::string t = util::trim(lines[i]), opt;
            if (t.empty()) continue;
            if (indentOf(lines[i]) < 2 && optionLine(t, opt)) opts.push_back(opt);
            else if (indentOf(lines[i]) >= 2 && !opts.empty()) opts.back() += " " + t;  // suite de l'option
            else trailing = false;
        }
        if (trailing && opts.size() >= 2) {
            q.options = opts;
            std::string text;
            for (size_t i = 0; i < first; ++i) text += lines[i] + "\n";
            q.text = util::trim(text);
        } else {
            q.text = util::trim(body);
        }
    }
    q.title = questionTitle(q.text);
    if (q.options.empty()) q.suggestions = suggestedReplies(q.text);
    return q;
}

std::vector<HumanQuestion> splitQuestions(const std::string& body) {
    std::vector<std::string> lines = util::splitLines(body);
    for (auto& l : lines)
        if (!l.empty() && l.back() == '\r') l.pop_back();
    std::vector<size_t> items;
    std::vector<std::string> heads;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string head;
        if (indentOf(lines[i]) < 2 && numberedLine(util::trim(lines[i]), head)) {
            items.push_back(i);
            heads.push_back(head);
        }
    }
    if (items.size() >= 2) {
        // Des questions plutôt que des choix : longues, interrogatives, titrées en gras ou suivies de détails.
        bool questionList = false;
        for (size_t k = 0; k < items.size(); ++k) {
            size_t next = k + 1 < items.size() ? items[k + 1] : lines.size();
            bool hasBody = false;
            for (size_t i = items[k] + 1; i < next; ++i)
                if (!util::trim(lines[i]).empty()) hasBody = true;
            const std::string& h = heads[k];
            if (h.size() > 90 || h.find('?') != std::string::npos || util::startsWith(h, "**") || hasBody) questionList = true;
        }
        if (questionList) {
            std::vector<HumanQuestion> out;
            std::string pre;
            for (size_t i = 0; i < items[0]; ++i) pre += lines[i] + "\n";
            pre = util::trim(pre);
            for (size_t k = 0; k < items.size(); ++k) {
                size_t next = k + 1 < items.size() ? items[k + 1] : lines.size();
                std::string text = heads[k] + "\n";
                for (size_t i = items[k] + 1; i < next; ++i) text += lines[i] + "\n";
                HumanQuestion q = splitQuestion(text);
                q.title = questionTitle(heads[k]);
                if (k == 0 && !pre.empty()) q.text = pre + "\n\n" + q.text;
                out.push_back(std::move(q));
            }
            return out;
        }
    }
    return {splitQuestion(body)};
}

Directives parseDirectives(const std::string& text) {
    Directives d;
    enum class Mode { None, Delegate, Human, Done } mode = Mode::None;
    Delegation cur;
    std::string body;
    auto flush = [&] {
        std::string b = util::trim(body);
        switch (mode) {
            case Mode::Delegate:
                if (!cur.agent.empty()) {
                    cur.task = b;
                    d.delegations.push_back(cur);
                }
                break;
            case Mode::Human:
                if (!b.empty()) {
                    d.needHuman = true;
                    d.question += (d.question.empty() ? "" : "\n\n") + b;
                    for (auto& q : splitQuestions(b)) d.questions.push_back(std::move(q));
                }
                break;
            case Mode::Done:
                d.finished = true;
                d.summary += (d.summary.empty() ? "" : "\n\n") + b;
                break;
            case Mode::None: break;
        }
        mode = Mode::None;
        cur = Delegation{};
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
                parseDelegationHeader(afterKeyword(t), cur);
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

std::vector<TextSegment> splitDirectiveSegments(const std::string& text) {
    std::vector<TextSegment> out;
    TextSegment cur;
    auto flush = [&] {
        cur.body = util::trim(cur.body);
        if (cur.kind != TextSegment::Text || !cur.body.empty()) out.push_back(cur);
        cur = TextSegment{};
    };
    for (const std::string& raw : util::splitLines(text)) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = util::trim(line);
        std::string k = normKey(t);
        if (util::startsWith(k, "@@")) {
            TextSegment::Kind kind = TextSegment::Text;
            if (util::startsWith(k, "@@fin")) {
                flush();
                continue;
            }
            if (util::startsWith(k, "@@deleguer")) kind = TextSegment::Delegate;
            else if (util::startsWith(k, "@@humain")) kind = TextSegment::Human;
            else if (util::startsWith(k, "@@termine")) kind = TextSegment::Done;
            if (kind != TextSegment::Text) {
                flush();
                cur.kind = kind;
                cur.header = afterKeyword(t);
                continue;
            }
        }
        cur.body += line + "\n";
    }
    flush();
    return out;
}

std::string orchestratorMessageTitle(const std::string& userText) {
    std::string t = util::trim(userText);
    if (util::startsWith(t, "_(Relance de l'orchestration")) return "Relance de l'orchestration";
    if (util::startsWith(t, "_(Reprise après interruption")) return "Reprise après interruption";
    if (util::startsWith(t, "# Objectif")) return "Mission : objectif, équipe et moyens";
    if (util::startsWith(t, "# Comptes rendus")) {
        std::string first = util::firstLine(t, 120);
        size_t p = first.find("cycle");
        return "Comptes rendus de l'équipe" + (p == std::string::npos ? std::string() : " — " + first.substr(p));
    }
    if (util::startsWith(t, "Ta réponse ne contient aucune directive")) return "Relance : directive attendue";
    if (util::startsWith(t, "Reprends là où tu en étais")) return "Reprise";
    return {};
}

// ---------------------------------------------------------------------------
// Modèles et effort
// ---------------------------------------------------------------------------

int modelTier(const std::string& model) {
    std::string m = util::toLower(model);
    if (m.find("haiku") != std::string::npos) return 1;
    if (m.find("sonnet") != std::string::npos) return 2;
    if (m.find("opus") != std::string::npos) return 3;
    if (m.find("fable") != std::string::npos) return 4;
    return 0;
}

std::string clampModel(const std::string& requested, const std::vector<std::string>& allowed) {
    std::map<int, std::string> byTier;  // gamme -> modèle autorisé
    for (auto& a : allowed)
        if (int t = modelTier(a); t > 0 && !byTier.count(t)) byTier[t] = a;
    bool restricted = !byTier.empty() && byTier.size() < 4;
    int tier = modelTier(requested);
    if (requested.empty() || tier == 0) return restricted ? byTier.rbegin()->second : requested;
    if (!restricted || byTier.count(tier)) return requested;
    // Le plus capable des modèles autorisés qui ne dépasse pas la demande, sinon le plus modeste.
    std::string best;
    for (auto& [t, m] : byTier)
        if (t <= tier) best = m;
    return best.empty() ? byTier.begin()->second : best;
}

std::string clampEffort(const std::string& requested, const std::string& maxEffort) {
    int r = effortRank(requested);
    if (r == 0) return {};
    int m = effortRank(maxEffort);
    if (m > 0 && r > m) return maxEffort;
    return requested;
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
// Niveaux de budget
// ---------------------------------------------------------------------------

namespace {

struct PresetValues {
    double share, usd, pause5h, pauseWeek;
    std::vector<std::string> models;
    const char *maxEffort, *workerModel, *workerEffort, *brainModel, *brainEffort;
    int parallel, cycles;
};

const PresetValues& presetValues(BudgetPreset p) {
    static const PresetValues low{0.25, 5.0, 0.90, 0.90, {"haiku", "sonnet"}, "medium", "haiku", "low", "sonnet", "medium", 2, 10};
    static const PresetValues medium{0.50, 20.0, 0.90, 0.95, {"haiku", "sonnet", "opus"}, "high", "sonnet", "medium", "opus", "medium", 3, 20};
    static const PresetValues high{1.0, 60.0, 0.92, 0.95, {"haiku", "sonnet", "opus", "fable"}, "xhigh", "sonnet", "medium", "opus", "high", 3, 40};
    static const PresetValues max{0.0, 200.0, 0.98, 0.98, {"haiku", "sonnet", "opus", "fable"}, "", "sonnet", "medium", "opus", "high", 4, 0};
    switch (p) {
        case BudgetPreset::Low: return low;
        case BudgetPreset::Medium: return medium;
        case BudgetPreset::High: return high;
        case BudgetPreset::Max: return max;
    }
    return medium;
}

const char* effortWord(const std::string& e) {
    if (e == "low") return "faible";
    if (e == "medium") return "moyen";
    if (e == "high") return "élevé";
    if (e == "xhigh") return "très élevé";
    if (e == "max") return "maximal";
    return "libre";
}

std::string capitalized(const std::string& m) {
    if (m.empty()) return m;
    std::string r = m;
    r[0] = (char)std::toupper((unsigned char)r[0]);
    return r;
}

}  // namespace

const char* budgetPresetLabel(BudgetPreset p, bool subscription) {
    switch (p) {
        case BudgetPreset::Low: return "Bas budget";
        case BudgetPreset::Medium: return "Budget moyen";
        case BudgetPreset::High: return "Budget élevé";
        case BudgetPreset::Max: return subscription ? "Max abonnement" : "Budget maximal";
    }
    return "";
}

std::string budgetPresetSummary(BudgetPreset p, bool subscription, double remaining) {
    const PresetValues& v = presetValues(p);
    std::string first;
    if (subscription) {
        if (v.share <= 0) first = "Sans limite : enchaîne les sessions de 5 h (pause à " + pct(v.pause5h) + ")";
        else if (v.share >= 1.0) first = "Tout ce qui reste dans la session";
        else first = pct(v.share) + " de ce qui reste dans la session";
        if (v.share > 0 && remaining >= 0) first += " (≈ " + pct(v.share * remaining) + " de la fenêtre)";
    } else
        first = util::formatUsd(v.usd) + " maximum";
    std::string models;
    for (auto& m : v.models) models += (models.empty() ? "" : " · ") + capitalized(m);
    std::string second = "Permis : " + models + " · effort " + (std::string(v.maxEffort).empty() ? "libre" : std::string("≤ ") + effortWord(v.maxEffort)) +
                         " · " + std::to_string(v.parallel) + " agents";
    return first + "\n" + second;
}

void applyBudgetPreset(OrchestratorSettings& s, BudgetPreset p) {
    const PresetValues& v = presetValues(p);
    s.sessionShare = v.share;
    s.maxCostUsd = v.usd;
    s.usagePause = v.pause5h;
    s.weeklyPause = v.pauseWeek;
    s.strictBudget = true;
    s.allowedModels = v.models;
    s.maxEffort = v.maxEffort;
    s.defaultWorkerModel = v.workerModel;
    s.defaultWorkerEffort = v.workerEffort;
    s.brainModel = v.brainModel;
    s.brainEffort = v.brainEffort;
    s.maxParallel = v.parallel;
    s.maxCycles = v.cycles;
}

bool budgetPresetMatches(const OrchestratorSettings& s, BudgetPreset p) {
    OrchestratorSettings ref = s;
    applyBudgetPreset(ref, p);
    auto sameModels = [](std::vector<std::string> a, std::vector<std::string> b) {
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        return a == b;
    };
    auto near = [](double a, double b) { return std::abs(a - b) < 1e-6; };
    return near(s.sessionShare, ref.sessionShare) && near(s.maxCostUsd, ref.maxCostUsd) &&
           near(s.usagePause, ref.usagePause) && near(s.weeklyPause, ref.weeklyPause) && s.strictBudget == ref.strictBudget &&
           sameModels(s.allowedModels, ref.allowedModels) && s.maxEffort == ref.maxEffort &&
           s.defaultWorkerModel == ref.defaultWorkerModel && s.defaultWorkerEffort == ref.defaultWorkerEffort &&
           s.brainModel == ref.brainModel && s.brainEffort == ref.brainEffort && s.maxParallel == ref.maxParallel &&
           s.maxCycles == ref.maxCycles;
}

// ---------------------------------------------------------------------------
// Budget
// ---------------------------------------------------------------------------

bool Orchestrator::resolveSubscription(const OrchestratorSettings& s, bool hostSubscription) {
    if (s.budgetMode == "subscription") return true;
    if (s.budgetMode == "api") return false;
    return hostSubscription;
}

void Orchestrator::beginWindow(double usage) {
    windowBase = std::max(0.0, usage);
    windowSpent = 0;
    allowanceOverride_ = -1;  // nouvelle session : part recalculée normalement
    changed = true;
}

void Orchestrator::resetBudget(OrchestratorHost& h) {
    if (subscription) {
        double keep = sessionAllowance();  // la limite actuelle est conservée…
        double u = h.utilization("five_hour");
        if (u < 0) u = std::max(0.0, windowBase) + windowSpent;
        beginWindow(u);
        lastFiveHour_ = u;
        if (settings.sessionShare > 0) {
            allowanceOverride_ = keep;  // …dans la limite de ce qui reste (voir sessionAllowance)
            overrideShare_ = settings.sessionShare;
        }
        addLog(&h, "Budget relancé : compteur à 0, limite " + pct(sessionAllowance()) + " de la fenêtre.", true);
    } else {
        costBaseline_ = spentUsd;
        addLog(&h, "Budget relancé : compteur à 0 $, limite " + util::formatUsd(settings.maxCostUsd) + ".", true);
    }
    finalRequested_ = false;
    changed = true;
}

double Orchestrator::sessionAllowance() const {
    if (windowBase < 0) return 0;
    double room = std::max(0.0, ceiling() - windowBase);  // ce que la session laisse à l'orchestration
    if (allowanceOverride_ >= 0 && std::abs(overrideShare_ - settings.sessionShare) < 1e-9)
        return std::min(allowanceOverride_, room);
    return settings.sessionShare * room;
}

// L'orchestrateur ou un agent de l'équipe travaille (ou vient de travailler : la mesure d'usage
// arrive parfois juste après la fin du tour).
bool Orchestrator::teamActive(OrchestratorHost& h) {
    bool busy = !brainConvId.empty() && h.status(brainConvId).busy;
    for (auto& t : tasks)
        if (!busy && t.sent && !t.done && !t.convId.empty() && h.status(t.convId).busy) busy = true;
    if (busy) lastActiveAt_ = (double)h.now();
    return busy || (double)h.now() - lastActiveAt_ <= 30;
}

double Orchestrator::budgetFraction() const {
    if (subscription) {
        if (settings.sessionShare <= 0) return -1;
        if (windowBase < 0) return 0;  // pas encore de mesure
        double a = sessionAllowance();
        if (a <= 1e-6) return 1.0;
        return windowSpent / a;
    }
    return settings.maxCostUsd > 0 ? budgetSpentUsd() / settings.maxCostUsd : -1;
}

std::string Orchestrator::budgetLabel() const {
    if (subscription) {
        std::string s = pct(windowSpent);
        if (settings.sessionShare > 0) s += " / " + pct(sessionAllowance());
        return s + " de la session de 5 h";
    }
    std::string s = util::formatUsd(budgetSpentUsd());
    if (settings.maxCostUsd > 0) s += " / " + util::formatUsd(settings.maxCostUsd);
    return s;
}

void Orchestrator::snapshotQuota(OrchestratorHost& h) {
    quota5h_ = h.utilization("five_hour");
    quota7d_ = h.utilization("seven_day");
    reset5h_ = h.resetsAt("five_hour");
    snapNow_ = h.now();
}

std::string Orchestrator::budgetNotes() const {
    std::string n;
    double f = budgetFraction();
    if (subscription) {
        if (settings.sessionShare > 0)
            n = "Budget de cette session de 5 h : " + pct(windowSpent) + " consommés sur " + pct(sessionAllowance()) +
                " de la fenêtre alloués à l'orchestration (" + pct(settings.sessionShare) +
                " de ce qui restait). Le budget se renouvelle à chaque nouvelle session.";
        else
            n = "Budget : aucune limite fixée par l'utilisateur (consommé dans cette session : " + pct(windowSpent) +
                " de la fenêtre de 5 h).";
        n += " Un modèle plus puissant ou un effort plus élevé consomme le quota plus vite.";
        if (quota5h_ >= 0) {
            n += "\nQuota du compte : fenêtre de 5 h à " + pct(quota5h_);
            if (settings.usagePause > 0) n += " (pause automatique à " + pct(settings.usagePause) + ")";
            if (reset5h_ > snapNow_ && snapNow_ > 0) n += ", réinitialisée dans " + util::humanDuration(reset5h_ - snapNow_);
            if (quota7d_ >= 0) {
                n += " ; semaine à " + pct(quota7d_);
                if (settings.weeklyPause > 0) n += " (pause à " + pct(settings.weeklyPause) + ")";
            }
            n += ".";
            if (settings.usagePause > 0 && settings.usagePause - quota5h_ < 0.10)
                n += "\n**Marge faible** : il reste " + pct(std::max(0.0, settings.usagePause - quota5h_)) +
                     " avant la pause automatique. Préfère des tâches courtes et des modèles économiques ; "
                     "l'orchestration reprendra seule après la réinitialisation de la fenêtre.";
        }
    } else {
        n = settings.maxCostUsd > 0 ? "Budget : " + util::formatUsd(budgetSpentUsd()) + " dépensés sur " + util::formatUsd(settings.maxCostUsd) + "."
                                    : "Budget : aucune limite fixée (dépensé : " + util::formatUsd(budgetSpentUsd()) + ").";
    }
    n += "\nLe budget est un maximum : optimise dans tous les cas (le modèle le plus économique qui suffit à chaque tâche).";
    std::string models;
    for (auto& m : settings.allowedModels) models += (models.empty() ? "" : ", ") + m;
    n += "\nModèles autorisés : " + (models.empty() ? std::string("tous") : models);
    n += " · effort maximal : " + (settings.maxEffort.empty() ? std::string("aucun plafond") : settings.maxEffort) + ".";
    double tol = std::max(0.0, settings.budgetTolerance);
    if (f >= 0 && tol > 0)
        n += "\nTolérance : la cible peut être dépassée de " + pct(tol) + " du budget pour terminer proprement ; ne bâcle "
             "jamais le travail pour rester pile dans la cible.";
    if (settings.strictBudget && finalRequested_) {
        n += "\n\n**BUDGET ÉPUISÉ** (cible et tolérance) : aucune nouvelle délégation ne sera envoyée. Réponds par "
             "@@TERMINE avec un bilan honnête : ce qui est livré et vérifié, ce qui manque, et comment poursuivre.";
    } else if (f >= 1.0) {
        n += "\n\n**Cible atteinte** (" + pct(f) + " du budget) : la marge de tolérance sert à TERMINER PROPREMENT ce qui "
             "est en cours — corrections, vérifications, conclusion. Ne lance pas de nouveau chantier ; ne bâcle rien ; "
             "conclus (@@TERMINE) dès que l'essentiel est livré et vérifié. Arrêt ferme à " + pct(1.0 + tol) + " du budget.";
    } else if (f >= 0.75) {
        n += "\n\n**Phase finale** : environ " + pct(std::max(0.0, 1.0 - f)) + " du budget avant la cible. Réduis le "
             "périmètre plutôt que la qualité : termine proprement ce qui est commencé, ne délègue que l'essentiel, "
             "préfère les modèles économiques, et prévois de conclure (@@TERMINE)" +
             (tol > 0 ? std::string(" — une marge de ") + pct(tol) + " au-delà de la cible permet de finir sans te précipiter."
                      : std::string(" avant l'épuisement."));
    }
    return n;
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------

std::string Orchestrator::systemPrompt() {
    return R"(# Rôle : ORCHESTRATEUR (piloté par ClaudeDesk)
Tu coordonnes des agents spécialisés. Chaque agent est une conversation Claude Code distincte, dans le même dépôt, avec son propre rôle (décrit dans les notes de son dossier). Tu ne développes pas toi-même : tu planifies, tu délègues, tu vérifies et tu décides de la suite. Tu peux lire le dépôt et lancer des commandes de vérification (tests, build) pour contrôler le travail rapporté.

## Protocole (obligatoire)
Termine CHAQUE réponse par un ou plusieurs blocs de directives, chaque marqueur seul sur sa ligne :

@@DELEGUER <nom exact de l'agent> | modele=<haiku|sonnet|opus|fable> effort=<low|medium|high|xhigh|max>
<tâche précise, autonome et vérifiable : contexte utile, fichiers concernés, critère de fin>
@@FIN

@@HUMAIN
<une question courte et précise, avec le contexte nécessaire pour y répondre>
- [ ] <option recommandée> (recommandé)
- [ ] <autre option>
@@FIN

@@TERMINE
<bilan final : ce qui est livré, comment c'est vérifié, limites connues>
@@FIN

## Optimiser, toujours
Le budget est un PLAFOND, pas un objectif : quel que soit le niveau (même élevé ou sans limite), cherche la façon la plus
efficace de mener le projet. Un travail bien découpé et confié au bon modèle va plus vite et coûte moins cher ; garde la
marge pour les imprévus et les vérifications.

## Choisir le modèle et l'effort de chaque tâche
Indique-les pour CHAQUE délégation (« | modele=… effort=… ») et choisis le plus économique qui suffit — la majorité des
tâches d'un projet ne demandent pas le modèle le plus puissant :
- haiku, effort low/medium : tâches mécaniques (renommage, documentation, mise en forme, vérification simple) ;
- sonnet, effort medium/high : développement courant, tests, intégration ;
- opus, effort high : conception, architecture, débogage difficile, sujets mal connus ;
- fable ou effort xhigh/max : seulement si une tentative plus économique a échoué.
Les modèles autorisés et le budget rappelés par ClaudeDesk sont des limites, pas des consignes d'utilisation ; plus la
fin du budget approche, plus tu économises.

## Questions à l'humain
Une seule question par bloc @@HUMAIN ; pour plusieurs questions, écris plusieurs blocs. ClaudeDesk les affiche dans un formulaire :
- si la réponse est un choix, termine le bloc par la liste des options, une par ligne au format « - [ ] option » : l'humain pourra cliquer ; mets en premier celle que tu recommandes, suivie de « (recommandé) » ;
- tout le contexte se place AVANT les options, en phrases ou en sous-puces indentées (les puces de premier niveau en fin de bloc sont lues comme des choix).

## Règles
- Plusieurs @@DELEGUER vers des agents différents s'exécutent en parallèle : ne parallélise que des tâches indépendantes. Un agent ne reçoit qu'une tâche à la fois.
- L'agent ne voit PAS cette conversation : donne-lui tout le contexte nécessaire dans la tâche.
- Après chaque cycle, ClaudeDesk te transmet les comptes rendus des agents. Vérifie (lis les fichiers, lance les tests) avant de considérer une étape comme faite ; renvoie une correction si besoin.
- Découpe en étapes de taille raisonnable : une tâche ≈ un livrable vérifiable.
- @@HUMAIN pour : décision majeure non tranchée, information manquante, action irréversible ou externe (commit, push, publication, achat, matériel physique). Ne demande jamais de commit ou de push aux agents.
- @@TERMINE seulement quand l'objectif est atteint et vérifié, ou quand ClaudeDesk annonce le budget épuisé.
- Sans directive, l'orchestration ne peut pas avancer.)";
}

std::string Orchestrator::firstMessage(const std::vector<AgentInfo>& agents, const std::string& planName) const {
    std::string m = "# Objectif\n" + util::trim(settings.objective) + "\n\n# Agents disponibles\n";
    for (auto& a : agents) m += "- **" + a.name + "**" + (a.brief.empty() ? "" : " — " + a.brief) + "\n";
    m += "\n# Moyens\n";
    if (subscription)
        m += "Compte avec abonnement" + (planName.empty() ? std::string() : " (" + planName + ")") +
             " : le budget est une part du quota de la fenêtre de 5 h.\n";
    else
        m += "Facturation à l'usage (API) : le budget est en dollars.\n";
    m += budgetNotes() + "\n";
    if (settings.strictBudget && budgetFraction() >= 0)
        m += "Le résultat doit tenir dans ce budget : planifie en conséquence (l'essentiel d'abord).\n";
    m += "Jusqu'à " + std::to_string(std::max(1, settings.maxParallel)) + " agents travaillent en parallèle.\n\n";
    m += "Utilise exactement les noms ci-dessus dans @@DELEGUER. Commence par examiner l'état du dépôt (plan, "
         "documentation, code existant), puis donne tes premières directives.";
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
        r += "## " + t.agent + (t.failed ? " — ÉCHEC" : " — terminé");
        if (!t.model.empty() || !t.effort.empty())
            r += " (" + (t.model.empty() ? std::string("modèle par défaut") : t.model) +
                 (t.effort.empty() ? "" : ", effort " + t.effort) + ")";
        r += "\n**Tâche :** " + util::firstLine(t.task, 200) + "\n\n";
        std::string rep = util::trim(t.report).empty() ? "(aucun texte rendu)" : t.report;
        if (rep.size() > 8000) rep = util::truncateUtf8(rep, 8000) + "\n(… compte rendu tronqué)";
        r += rep + "\n\n";
    }
    // Délégations sans modèle : le défaut a servi partout. Rappel pour que le cerveau optimise.
    std::string unchosen;
    for (auto& t : tasks)
        if (t.sent && !t.modelChosen) unchosen += (unchosen.empty() ? "" : ", ") + t.agent;
    if (!unchosen.empty())
        r += "**Rappel** : tu n'as pas choisi de modèle pour " + unchosen + " — le défaut des agents (" +
             (settings.defaultWorkerModel.empty() ? std::string("celui de leur conversation") : settings.defaultWorkerModel) +
             ") a été appliqué. Indique « | modele=… effort=… » à CHAQUE délégation : haiku pour la mécanique, sonnet pour "
             "le courant, opus pour le difficile (conception, débogage, sujets mal connus).\n\n";
    r += "---\n" + budgetNotes() + "\nCycle " + std::to_string(cycle);
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

// Budget épuisé sans « résultat dans le budget » : sous abonnement, on attend la prochaine session de 5 h
// (le budget s'y renouvelle) ; en API, pause jusqu'à ce que l'utilisateur relève le budget.
void Orchestrator::budgetPause(OrchestratorHost& h) {
    if (subscription && settings.sessionShare > 0) {
        if (state != OrchState::UsagePause) resumeState_ = state;
        state = OrchState::UsagePause;
        usageWindow_ = "budget";
        pauseReason = "budget de cette session atteint (" + budgetLabel() + ") : reprise automatique à la prochaine session de 5 h";
        changed = true;
        addLog(&h, "Pause : " + pauseReason, true);
        return;
    }
    pauseFrom(state, "budget épuisé (" + budgetLabel() + ") — augmentez-le puis reprenez", &h);
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
    costBaseline_ = 0;
    usageSpent = 0;
    windowBase = -1;
    windowSpent = 0;
    tasks.clear();
    log.clear();
    finalSummary.clear();
    humanQuestion.clear();
    questions.clear();
    pauseReason.clear();
    nudges_ = 0;
    finalRequested_ = false;
    subscription = resolveSubscription(settings, h.isSubscription());
    lastFiveHour_ = h.utilization("five_hour");
    if (lastFiveHour_ >= 0) beginWindow(lastFiveHour_);
    std::string id = h.openBrain(brainConvId, systemPrompt());
    if (id.empty()) {
        addLog(&h, "Impossible d'ouvrir la conversation de l'orchestrateur.", true);
        return false;
    }
    brainConvId = id;
    addLog(&h, "Démarrage — " + util::firstLine(settings.objective, 120) + " (" + std::to_string(ag.size()) + " agents, budget " +
                   (subscription ? "abonnement" : "API") + ")",
           true);
    snapshotQuota(h);
    std::string first = firstMessage(ag, h.planName());
    if (h.status(brainConvId).turns > 0)
        first = "_(Relance de l'orchestration. Cette conversation contient déjà ton travail précédent : repars de l'état "
                "actuel du dépôt, sans refaire ce qui est fait.)_\n\n" + first;
    if (!usageOk(h)) {
        // Reprise automatique : le premier message partira à la levée de la pause.
        lastBrainMessage_ = first;
        brainTurnsAtSend_ = h.status(brainConvId).turns;
        resumeState_ = OrchState::Thinking;
        return true;
    }
    return sendBrain(h, first);
}

bool Orchestrator::sendBrain(OrchestratorHost& h, const std::string& text) {
    ParticipantStatus st = h.status(brainConvId);
    brainTurnsAtSend_ = st.turns;
    brainCostAtSend_ = st.costUsd;
    lastBrainMessage_ = text;
    brainHumanNotified_ = false;
    SendOptions opt{settings.brainMode, clampModel(settings.brainModel, settings.allowedModels),
                    clampEffort(settings.brainEffort, settings.maxEffort)};
    if (!h.send(brainConvId, text, opt)) {
        pauseFrom(OrchState::Thinking, "envoi impossible à l'orchestrateur (Claude Code introuvable ?)", &h);
        return false;
    }
    lastActiveAt_ = (double)h.now();
    state = OrchState::Thinking;
    changed = true;
    return true;
}

bool Orchestrator::answerHuman(OrchestratorHost& h, const std::string& answer) {
    if (state != OrchState::WaitingHuman || util::trim(answer).empty()) return false;
    h.ensureOpen(brainConvId);
    ParticipantStatus st = h.status(brainConvId);
    if (!st.exists || st.busy) return false;
    qaHistory.push_back({h.now(), humanQuestion, answer});
    while (qaHistory.size() > 50) qaHistory.pop_front();
    addLog(&h, "Vous : " + util::firstLine(answer, 150), true);
    questions.clear();
    humanQuestion.clear();
    return sendBrain(h, "# Réponse de l'humain\n\n" + answer + "\n\nPoursuis et donne tes directives.");
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
    questions.clear();
    addLog(nullptr, "Orchestration arrêtée.", true);
}

void Orchestrator::resume(OrchestratorHost& h) {
    if (state != OrchState::Paused && state != OrchState::UsagePause) return;
    OrchState to = resumeState_;
    if (to != OrchState::Working && to != OrchState::WaitingHuman) to = OrchState::Thinking;
    pauseReason.clear();
    if (!exhausted()) finalRequested_ = false;  // budget relevé : on repart normalement
    if (!brainConvId.empty()) h.ensureOpen(brainConvId);
    for (auto& t : tasks)
        if (t.sent && !t.done) h.ensureOpen(t.convId);
    lastFiveHour_ = h.utilization("five_hour");
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
        {
            std::string msg = lastBrainMessage_.empty() ? "Reprends là où tu en étais et donne tes directives." : lastBrainMessage_;
            const std::string tag = "_(Reprise après interruption";
            if (!util::startsWith(msg, tag))
                msg = tag + " : ce message a peut-être déjà été traité en partie. Vérifie l'état du dépôt avant d'agir.)_\n\n" + msg;
            snapshotQuota(h);
            sendBrain(h, msg);
        }
    } else if (to == OrchState::Working) {
        for (auto& t : tasks) {
            if (!t.sent || t.done) continue;
            ParticipantStatus st = h.status(t.convId);
            if (!st.busy && st.turns <= t.turnsAtSend) t.sent = false;  // renvoyée par dispatch
        }
    }
}

bool Orchestrator::usageOk(OrchestratorHost& h) {
    const std::pair<const char*, double> limits[] = {{"five_hour", settings.usagePause}, {"seven_day", settings.weeklyPause}};
    for (auto& [win, limit] : limits) {
        if (limit <= 0) continue;
        int64_t r = h.resetsAt(win);
        if (r > 0 && h.now() > r) continue;  // mesure périmée : la fenêtre a été réinitialisée
        double u = h.utilization(win);
        if (u >= limit) {
            if (state != OrchState::UsagePause) resumeState_ = state;
            state = OrchState::UsagePause;
            usageWindow_ = win;
            std::string w = std::string(win) == "five_hour" ? "fenêtre de 5 h" : "quota hebdomadaire";
            pauseReason = w + " à " + pct(u) + " (plafond " + pct(limit) + ") : reprise automatique à sa réinitialisation";
            changed = true;
            addLog(&h, "Pause : " + pauseReason, true);
            return false;
        }
    }
    return true;
}

void Orchestrator::trackUsage(OrchestratorHost& h) {
    snapshotQuota(h);
    double u = h.utilization("five_hour");
    if (u < 0) return;
    int64_t r = h.resetsAt("five_hour");
    if (windowBase < 0 || lastFiveHour_ < 0) {  // première mesure : l'usage de départ de la session est mémorisé
        if (windowBase < 0) beginWindow(u);
        lastFiveHour_ = u;
        if (r > 0) windowResetsAt_ = r;
        return;
    }
    if (windowResetsAt_ == 0 && r > 0) windowResetsAt_ = r;
    // Nouvelle session de 5 h : sa date de réinitialisation a changé, l'ancienne est passée (et l'usage a baissé),
    // ou forte chute. Une petite baisse n'est que du bruit (mesures reçues dans le désordre par plusieurs agents).
    bool newWindow = (windowResetsAt_ > 0 && r > windowResetsAt_ + 60) ||
                     (windowResetsAt_ > 0 && h.now() > windowResetsAt_ + 30 && u < lastFiveHour_ - 1e-9) ||
                     u < lastFiveHour_ - 0.20;
    if (newWindow) {
        beginWindow(u);  // nouveau départ : l'usage actuel de la session sert de référence
        windowResetsAt_ = r;
        lastFiveHour_ = u;
        addLog(&h, "Nouvelle session de 5 h : budget renouvelé (" + pct(sessionAllowance()) + " de la fenêtre).");
        return;
    }
    if (u > lastFiveHour_ + 1e-9) {
        // Seule la consommation faite pendant que l'équipe travaille revient à l'orchestration ; le reste (vos
        // autres conversations, une mesure périmée corrigée au lancement…) réduit simplement ce qui reste.
        double d = u - lastFiveHour_;
        if (teamActive(h)) {
            usageSpent += d;
            windowSpent += d;
        } else {
            windowBase += d;
        }
        lastFiveHour_ = u;
        changed = true;
    }
    // u <= lastFiveHour_ : bruit de mesure, ignoré (on garde le maximum observé).
}

void Orchestrator::tick(OrchestratorHost& h) {
    // Installation neuve : l'abonnement n'est connu qu'après la première réponse de Claude.
    if (settings.budgetMode == "auto" && !subscription && state != OrchState::Idle && state != OrchState::Finished &&
        h.isSubscription()) {
        subscription = true;
        usageSpent = 0;
        lastFiveHour_ = h.utilization("five_hour");
        windowBase = -1;
        if (lastFiveHour_ >= 0) beginWindow(lastFiveHour_);
        addLog(&h, "Abonnement détecté : le budget devient une part du quota de 5 h.", true);
    }
    if (state != OrchState::Idle && state != OrchState::Finished) trackUsage(h);
    switch (state) {
        case OrchState::Thinking: tickThinking(h); break;
        case OrchState::Working: tickWorking(h); break;
        case OrchState::WaitingHuman: {
            // Réponse donnée directement dans la conversation de l'orchestrateur.
            ParticipantStatus st = h.status(brainConvId);
            if (st.exists && !st.busy && st.turns > brainTurnsAtSend_) {
                spentUsd += std::max(0.0, st.costUsd - brainCostAtSend_);
                if (!humanQuestion.empty()) qaHistory.push_back({h.now(), humanQuestion, "(répondu dans la conversation)"});
                humanQuestion.clear();
                questions.clear();
                addLog(&h, "Vous avez répondu : l'orchestrateur reprend.");
                handleBrainAnswer(h, st.lastText);
            }
            break;
        }
        case OrchState::UsagePause: {
            if (usageWindow_ == "budget") {
                int64_t r = h.resetsAt("five_hour");
                if (!exhausted() || (r > 0 && h.now() > r + 30)) {
                    if (exhausted()) {  // session réinitialisée, mesure pas encore reçue
                        beginWindow(0);
                        lastFiveHour_ = 0;
                        windowResetsAt_ = 0;
                    }
                    state = OrchState::Paused;
                    addLog(&h, "Nouvelle session de 5 h : l'orchestration reprend.");
                    resume(h);
                }
                break;
            }
            std::string win = usageWindow_.empty() ? "five_hour" : usageWindow_;
            double limit = win == "seven_day" ? settings.weeklyPause : settings.usagePause;
            double u = h.utilization(win);
            int64_t r = h.resetsAt(win);
            if (limit <= 0 || u < limit || (r > 0 && h.now() > r + 30)) {
                state = OrchState::Paused;  // passe par resume() : renvoie ce qui doit l'être
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
        questions = d.questions;
        ++questionSeq;
        ParticipantStatus st = h.status(brainConvId);
        brainTurnsAtSend_ = st.turns;
        brainCostAtSend_ = st.costUsd;
        std::string extra = d.delegations.empty() ? "" : " (ses délégations attendront votre réponse)";
        addLog(&h, "Question pour vous : " + util::firstLine(questions.empty() ? d.question : questions[0].text, 150) + extra,
               true);
        return;
    }
    if (d.delegations.empty()) {
        if (++nudges_ > 2) {
            state = OrchState::WaitingHuman;
            humanQuestion = "L'orchestrateur ne donne plus de directives. Que doit-il faire ?";
            HumanQuestion q;
            q.title = q.text = humanQuestion;
            questions = {q};
            ++questionSeq;
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
        t.model = del.model;
        t.effort = del.effort;
        t.modelChosen = !del.model.empty();
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
        // Plafond du compte atteint : pause jusqu'à la prochaine session (et non « budget épuisé »).
        if (!finalRequested_ && !usageOk(h)) return;
        if (exhausted()) {
            if (settings.strictBudget && !finalRequested_) {
                // Résultat dans le budget : plus de nouvelle tâche, le cerveau devra conclure.
                t.done = t.failed = true;
                t.report = "Non envoyée : budget épuisé.";
                changed = true;
                continue;
            }
            budgetPause(h);
            return;
        }
        if (active >= std::max(1, settings.maxParallel)) break;
        bool agentBusy = false;
        for (auto& o : tasks)
            if (&o != &t && o.agent == t.agent && o.sent && !o.done) agentBusy = true;
        if (agentBusy) continue;
        if (!usageOk(h)) return;
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
        if (needsRotation(st, id)) {
            std::string nid = h.rotate(t.agent, id);
            if (nid.empty()) continue;  // récap en cours : la tâche partira au prochain passage
            addLog(&h, "↻ " + t.agent + " : contexte allégé (récap + nouvelle conversation, " +
                           std::to_string((int)std::lround(st.context * 100)) + " % de contexte avant).");
            agentConv[t.agent] = id = nid;
            st = h.status(id);
        }
        // Modèle / effort : demande du cerveau, sinon défauts, dans les limites autorisées.
        std::string wantModel = !t.model.empty() ? t.model : settings.defaultWorkerModel;
        std::string wantEffort = !t.effort.empty() ? t.effort : settings.defaultWorkerEffort;
        std::string model = clampModel(wantModel, settings.allowedModels);
        std::string effort = clampEffort(wantEffort, settings.maxEffort);
        if (!t.model.empty() && model != t.model)
            addLog(&h, t.agent + " : modèle « " + t.model + " » non autorisé, remplacé par « " + model + " ».");
        t.model = model;
        t.effort = effort;
        t.convId = id;
        t.turnsAtSend = st.turns;
        t.costAtSend = st.costUsd;
        if (!h.send(id, taskMessage(t), SendOptions{settings.workerMode, model, effort})) {
            t.done = t.failed = true;
            t.report = "Envoi impossible (Claude Code introuvable ?).";
            addLog(&h, t.agent + " : envoi impossible.", true);
            continue;
        }
        t.sent = true;
        t.sentAt = h.now();
        convTasks_[id]++;
        lastActiveAt_ = (double)h.now();
        ++active;
        changed = true;
        std::string tag;
        if (!model.empty() || !effort.empty())
            tag = " [" + (model.empty() ? std::string("défaut") : model) + (effort.empty() ? "" : " · " + effort) + "]";
        addLog(&h, "→ " + t.agent + tag + " : " + util::firstLine(t.task, 100));
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
    // Le plafond du compte passe avant le budget : il suspend (reprise à la prochaine session) sans faire conclure.
    if (!finalRequested_ && !usageOk(h)) return;
    if (exhausted()) {
        if (settings.strictBudget && !finalRequested_) {
            finalRequested_ = true;
            addLog(&h, "Budget épuisé : l'orchestrateur doit conclure avec un bilan.", true);
        } else {
            budgetPause(h);
            return;
        }
    } else if (settings.maxCycles > 0 && cycle >= settings.maxCycles) {
        pauseFrom(OrchState::Working,
                  "nombre maximal de cycles atteint (" + std::to_string(settings.maxCycles) + ") — augmentez-le puis reprenez", &h);
        return;
    }
    if (!finalRequested_ && !usageOk(h)) return;
    std::string prefix;
    ParticipantStatus bst = h.status(brainConvId);
    if (needsRotation(bst, brainConvId)) {
        std::string nid = h.rotate("", brainConvId);
        if (nid.empty()) return;  // récap de l'orchestrateur en cours
        addLog(&h, "↻ Orchestrateur : contexte allégé (récap + nouvelle conversation).", true);
        brainConvId = nid;
        prefix = brainRefresher();
    }
    addLog(&h, "Comptes rendus du cycle " + std::to_string(cycle) + " envoyés à l'orchestrateur.");
    sendBrain(h, prefix + buildReport());
}

bool Orchestrator::needsRotation(const ParticipantStatus& st, const std::string& convId) const {
    if (!settings.contextRotation || convId.empty()) return false;
    if (settings.rotateAtContext > 0 && st.context >= settings.rotateAtContext) return true;
    auto it = convTasks_.find(convId);
    int n = it == convTasks_.end() ? 0 : it->second;
    if (convId == brainConvId) n = 0;  // l'orchestrateur : seul le contexte compte
    return settings.rotateAfterTasks > 0 && n >= settings.rotateAfterTasks;
}

std::string Orchestrator::brainRefresher() const {
    return "_(Contexte allégé : ta conversation précédente a été résumée ; son récap figure dans ton contexte, avec les "
           "notes du projet. Le protocole @@DELEGUER / @@HUMAIN / @@TERMINE est inchangé.)_\n\n# Rappel de l'objectif\n" +
           util::trim(settings.objective) + "\n\nCycle " + std::to_string(cycle) + " ; utilise les noms d'agents exacts "
           "(liste dans ton contexte).\n\n";
}

// ---------------------------------------------------------------------------
// Persistance
// ---------------------------------------------------------------------------

json Orchestrator::toJson() const {
    json jt = json::array();
    for (auto& t : tasks)
        jt.push_back({{"agent", t.agent},   {"task", t.task},     {"convId", t.convId},       {"model", t.model}, {"modelChosen", t.modelChosen},
                      {"effort", t.effort}, {"cycle", t.cycle},   {"turnsAtSend", t.turnsAtSend},
                      {"costAtSend", t.costAtSend}, {"sent", t.sent}, {"done", t.done}, {"failed", t.failed},
                      {"report", t.report}, {"sentAt", t.sentAt}, {"doneAt", t.doneAt}});
    json jl = json::array();
    for (auto& l : log) jl.push_back({{"t", l.time}, {"text", l.text}, {"important", l.important}});
    json jq = json::array();
    for (auto& q : questions)
        jq.push_back({{"title", q.title}, {"text", q.text}, {"options", q.options}, {"suggestions", q.suggestions}});
    json jh = json::array();
    for (auto& q : qaHistory) jh.push_back({{"t", q.time}, {"q", q.question}, {"a", q.answer}});
    const auto& s = settings;
    return {{"version", 2},
            {"settings",
             {{"objective", s.objective},
              {"maxCycles", s.maxCycles},
              {"maxParallel", s.maxParallel},
              {"workerMode", s.workerMode},
              {"brainMode", s.brainMode},
              {"budgetMode", s.budgetMode},
              {"maxCostUsd", s.maxCostUsd},
              {"sessionShare", s.sessionShare},
              {"usagePause", s.usagePause},
              {"weeklyPause", s.weeklyPause},
              {"strictBudget", s.strictBudget},
              {"budgetTolerance", s.budgetTolerance},
              {"contextRotation", s.contextRotation},
              {"rotateAtContext", s.rotateAtContext},
              {"rotateAfterTasks", s.rotateAfterTasks},
              {"allowedModels", s.allowedModels},
              {"maxEffort", s.maxEffort},
              {"defaultWorkerModel", s.defaultWorkerModel},
              {"defaultWorkerEffort", s.defaultWorkerEffort},
              {"brainModel", s.brainModel},
              {"brainEffort", s.brainEffort}}},
            {"state", stateKey(state)},
            {"resumeState", stateKey(resumeState_)},
            {"pauseReason", pauseReason},
            {"cycle", cycle},
            {"spentUsd", spentUsd},
            {"usageSpent", usageSpent},
            {"windowBase", windowBase},
            {"windowSpent", windowSpent},
            {"lastFiveHour", lastFiveHour_},
            {"windowResetsAt", windowResetsAt_},
            {"allowanceOverride", allowanceOverride_},
            {"overrideShare", overrideShare_},
            {"costBaseline", costBaseline_},
            {"convTasks", convTasks_},
            {"subscription", subscription},
            {"brainConvId", brainConvId},
            {"agentConv", agentConv},
            {"tasks", jt},
            {"log", jl},
            {"finalSummary", finalSummary},
            {"humanQuestion", humanQuestion},
            {"questions", jq},
            {"questionSeq", questionSeq},
            {"qaHistory", jh},
            {"brainTurnsAtSend", brainTurnsAtSend_},
            {"brainCostAtSend", brainCostAtSend_},
            {"lastBrainMessage", lastBrainMessage_},
            {"nudges", nudges_},
            {"finalRequested", finalRequested_},
            {"usageWindow", usageWindow_}};
}

void Orchestrator::fromJson(const json& j) {
    json s = j.value("settings", json::object());
    auto& st = settings;
    st.objective = s.value("objective", std::string());
    st.maxCycles = s.value("maxCycles", st.maxCycles);
    st.maxParallel = s.value("maxParallel", st.maxParallel);
    st.workerMode = s.value("workerMode", st.workerMode);
    st.brainMode = s.value("brainMode", st.brainMode);
    st.budgetMode = s.value("budgetMode", st.budgetMode);
    st.maxCostUsd = s.value("maxCostUsd", st.maxCostUsd);
    st.sessionShare = s.value("sessionShare", st.sessionShare);
    st.usagePause = s.value("usagePause", st.usagePause);
    st.weeklyPause = s.value("weeklyPause", st.weeklyPause);
    st.strictBudget = s.value("strictBudget", st.strictBudget);
    st.budgetTolerance = s.value("budgetTolerance", st.budgetTolerance);
    st.contextRotation = s.value("contextRotation", st.contextRotation);
    st.rotateAtContext = s.value("rotateAtContext", st.rotateAtContext);
    st.rotateAfterTasks = s.value("rotateAfterTasks", st.rotateAfterTasks);
    if (s.contains("allowedModels") && s["allowedModels"].is_array()) {
        st.allowedModels.clear();
        for (auto& m : s["allowedModels"])
            if (m.is_string()) st.allowedModels.push_back(m.get<std::string>());
    }
    st.maxEffort = s.value("maxEffort", st.maxEffort);
    st.defaultWorkerModel = s.value("defaultWorkerModel", st.defaultWorkerModel);
    st.defaultWorkerEffort = s.value("defaultWorkerEffort", st.defaultWorkerEffort);
    st.brainModel = s.value("brainModel", st.brainModel);
    st.brainEffort = s.value("brainEffort", st.brainEffort);

    state = stateFromKey(j.value("state", std::string("idle")));
    resumeState_ = stateFromKey(j.value("resumeState", std::string("idle")));
    pauseReason = j.value("pauseReason", std::string());
    cycle = j.value("cycle", 0);
    spentUsd = j.value("spentUsd", 0.0);
    usageSpent = j.value("usageSpent", 0.0);
    windowBase = j.value("windowBase", -1.0);
    windowSpent = j.value("windowSpent", 0.0);
    subscription = j.value("subscription", false);
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
        t.model = jt.value("model", std::string());
        t.modelChosen = jt.value("modelChosen", false);
        t.effort = jt.value("effort", std::string());
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
    questions.clear();
    for (auto& jq : j.value("questions", json::array())) {
        HumanQuestion q;
        q.title = jq.value("title", std::string());
        q.text = jq.value("text", std::string());
        for (auto& o : jq.value("options", json::array()))
            if (o.is_string()) q.options.push_back(o.get<std::string>());
        for (auto& o : jq.value("suggestions", json::array()))
            if (o.is_string()) q.suggestions.push_back(o.get<std::string>());
        if (q.title.empty()) q.title = util::firstLine(q.text, 160);
        questions.push_back(std::move(q));
    }
    questionSeq = j.value("questionSeq", 0);
    qaHistory.clear();
    for (auto& jh : j.value("qaHistory", json::array()))
        qaHistory.push_back({jh.value("t", (int64_t)0), jh.value("q", std::string()), jh.value("a", std::string())});
    brainTurnsAtSend_ = j.value("brainTurnsAtSend", 0);
    brainCostAtSend_ = j.value("brainCostAtSend", 0.0);
    lastBrainMessage_ = j.value("lastBrainMessage", std::string());
    nudges_ = j.value("nudges", 0);
    finalRequested_ = j.value("finalRequested", false);
    usageWindow_ = j.value("usageWindow", std::string());
    lastFiveHour_ = j.value("lastFiveHour", -1.0);
    windowResetsAt_ = j.value("windowResetsAt", (int64_t)0);
    allowanceOverride_ = j.value("allowanceOverride", -1.0);
    overrideShare_ = j.value("overrideShare", -1.0);
    costBaseline_ = j.value("costBaseline", 0.0);
    convTasks_.clear();
    if (j.contains("convTasks") && j["convTasks"].is_object())
        for (auto& [k, v] : j["convTasks"].items())
            if (v.is_number_integer()) convTasks_[k] = v.get<int>();  // gardé : une session réinitialisée entre-temps sera détectée
    // Questions en attente : réanalysées avec le découpage actuel (questions multiples, options…).
    if (state == OrchState::WaitingHuman && !humanQuestion.empty())
        questions = parseDirectives("@@HUMAIN\n" + humanQuestion + "\n@@FIN").questions;
    // Rechargée en cours de route (ClaudeDesk fermé) : les processus ont disparu,
    // on attend une reprise explicite. Une question en attente reste une question.
    if (state == OrchState::Thinking || state == OrchState::Working) {
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
