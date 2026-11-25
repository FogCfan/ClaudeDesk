#include "claude/Orchestrator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "claude/ProjectKind.h"
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

// Une réponse du cerveau qui change la suite (sinon : simple discussion avec l'humain).
bool hasDirectives(const Directives& d) {
    return !d.delegations.empty() || !d.newAgents.empty() || d.needHuman || d.finished || d.pause;
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

namespace {
// « @@NOUVEL_AGENT », « @@NOUVEL AGENT », « @@CRÉER_AGENT »…
bool newAgentKey(const std::string& k) { return util::startsWith(k, "@@nouvel") || util::startsWith(k, "@@creer"); }
// Nom après le mot-clé ; « @@NOUVEL AGENT 09 Interface » laisse « AGENT 09 Interface » : on retire « agent ».
std::string newAgentName(const std::string& line) {
    std::string n = afterKeyword(line);
    if (util::startsWith(normKey(n), "agent ") || util::startsWith(normKey(n), "agent:")) n = util::trim(n.substr(6));
    size_t bar = n.find('|');
    if (bar != std::string::npos) n = n.substr(0, bar);
    return cleanAgentName(n);
}
}  // namespace

std::string cleanAgentName(const std::string& name) {
    std::string s = util::trim(name);
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();  // « `Nom`. » : ponctuation de phrase
    s = cleanName(s);
    std::string out;
    for (char c : s) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            (unsigned char)c < 0x20)
            c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += c;
    }
    out = util::trim(out);
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();  // interdit en fin de nom (Windows)
    if (out.size() > 60) out = util::trim(util::truncateUtf8(out, 60));
    return out;
}

Directives parseDirectives(const std::string& text) {
    Directives d;
    enum class Mode { None, Delegate, Human, Done, Wait, Pause, NewAgent } mode = Mode::None;
    Delegation cur;
    std::string newName;
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
            case Mode::NewAgent:
                if (!newName.empty()) d.newAgents.push_back({newName, b});
                break;
            case Mode::Pause:
                d.pause = true;
                d.pauseNote += (d.pauseNote.empty() ? "" : "\n\n") + b;
                break;
            case Mode::Human:
                if (looksLikePauseNotice(b)) {  // annonce de pause écrite comme une question : traitée comme @@PAUSE
                    d.pause = true;
                    d.pauseNote += (d.pauseNote.empty() ? "" : "\n\n") + b;
                } else if (!b.empty()) {
                    d.needHuman = true;
                    d.question += (d.question.empty() ? "" : "\n\n") + b;
                    for (auto& q : splitQuestions(b)) d.questions.push_back(std::move(q));
                }
                break;
            case Mode::Done:
                d.finished = true;
                d.summary += (d.summary.empty() ? "" : "\n\n") + b;
                break;
            case Mode::Wait: d.wait = true; break;
            case Mode::None: break;
        }
        mode = Mode::None;
        cur = Delegation{};
        newName.clear();
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
            if (newAgentKey(k)) {
                flush();
                mode = Mode::NewAgent;
                newName = newAgentName(t);
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
            if (util::startsWith(k, "@@attendre")) {
                flush();
                mode = Mode::Wait;
                continue;
            }
            if (util::startsWith(k, "@@pause")) {
                flush();
                mode = Mode::Pause;
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

bool looksLikePauseNotice(const std::string& body) {
    std::string b = util::trim(body);
    if (b.empty() || b.find('?') != std::string::npos) return false;
    // De vrais choix (« - [ ] … ») ou une réponse suggérée : c'est une question. Une simple liste (plan de reprise) non.
    if (b.find("[ ]") != std::string::npos) return false;
    for (auto& q : splitQuestions(b))
        if (!q.suggestions.empty()) return false;
    std::string l = util::toLower(b);
    for (const char* w : {"en pause", "mettre en pause", "mets en pause", "relance-moi", "relancez-moi", "relance moi",
                          "relancez moi", "réinitialisation du quota", "reinitialisation du quota"})
        if (l.find(w) != std::string::npos) return true;
    return false;
}

std::vector<TextSegment> splitDirectiveSegments(const std::string& text) {
    std::vector<TextSegment> out;
    TextSegment cur;
    auto flush = [&] {
        cur.body = util::trim(cur.body);
        if (cur.kind == TextSegment::Human && looksLikePauseNotice(cur.body)) cur.kind = TextSegment::Pause;
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
            else if (newAgentKey(k)) kind = TextSegment::NewAgent;
            else if (util::startsWith(k, "@@humain")) kind = TextSegment::Human;
            else if (util::startsWith(k, "@@termine")) kind = TextSegment::Done;
            else if (util::startsWith(k, "@@attendre")) kind = TextSegment::Wait;
            else if (util::startsWith(k, "@@pause")) kind = TextSegment::Pause;
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
    if (util::startsWith(t, "# Reprise après ta pause")) return "Reprise après sa pause (nouvelle session de 5 h)";
    if (util::startsWith(t, "# Compte rendu intermédiaire")) {
        std::string first = util::firstLine(t, 120);
        size_t p = first.find("cycle");
        return "Compte rendu intermédiaire" + (p == std::string::npos ? std::string() : " — " + first.substr(p));
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

std::vector<std::string> usableModels(const std::vector<std::string>& allowed, const std::vector<std::string>& blocked) {
    if (blocked.empty()) return allowed;
    std::vector<std::string> base = allowed.empty() ? std::vector<std::string>{"haiku", "sonnet", "opus", "fable"} : allowed;
    std::vector<std::string> out;
    for (auto& m : base) {
        bool bad = false;
        for (auto& b : blocked)
            if (modelTier(m) == modelTier(b)) bad = true;
        if (!bad) out.push_back(m);
    }
    return out.empty() ? allowed : out;
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

// ---------------------------------------------------------------------------
// Économie de contexte et de cache
// ---------------------------------------------------------------------------

std::string kTokens(int64_t n) {
    char buf[32];
    if (n < 1000) std::snprintf(buf, sizeof buf, "%lld", (long long)n);
    else if (n < 1000000) std::snprintf(buf, sizeof buf, "%lld k", (long long)((n + 500) / 1000));
    else {
        std::snprintf(buf, sizeof buf, "%.1f M", n / 1e6);
        for (char* c = buf; *c; ++c)
            if (*c == '.') *c = ',';
    }
    return buf;
}

std::string rotationReason(const OrchestratorSettings& s, const ParticipantStatus& st, int tasksInConv, bool isBrain,
                           int64_t now, int64_t ttlSeconds, const std::string& nextModel, const std::string& nextEffort) {
    if (!s.contextRotation) return {};
    const int64_t ctx = st.contextTokens;
    if (s.rotateAtTokens > 0 && ctx >= s.rotateAtTokens)
        return "contexte de " + kTokens(ctx) + " (seuil " + kTokens(s.rotateAtTokens) + ")";
    if (!isBrain && s.rotateAfterTasks > 0 && tasksInConv >= s.rotateAfterTasks)
        return std::to_string(tasksInConv) + " tâches dans la même conversation";
    // Conversation froide : son cache a expiré, le prochain appel réécrirait tout le contexte.
    if (s.rotateWhenCold && ttlSeconds > 0 && st.lastTurnAt > 0 && ctx >= s.coldRotateTokens &&
        now - st.lastTurnAt >= ttlSeconds)
        return "cache expiré (inactive depuis " + util::humanDuration(now - st.lastTurnAt) + ", " + kTokens(ctx) +
               " à réécrire)";
    // Changement de modèle ou d'effort : le cache ne sert plus.
    if (s.rotateOnSwitch && ctx >= s.switchRotateTokens) {
        int from = modelTier(st.model), to = modelTier(nextModel);
        if (!nextModel.empty() && from > 0 && to > 0 && from != to)
            return "changement de modèle (" + st.model + " → " + nextModel + ") sur " + kTokens(ctx) +
                   " de contexte : le cache est propre au modèle";
        if (!nextEffort.empty() && nextEffort != st.effort && !st.effort.empty())
            return "changement d'effort (" + st.effort + " → " + nextEffort + ") sur " + kTokens(ctx) + " de contexte";
    }
    return {};
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

void Orchestrator::absorbUntracked(OrchestratorHost& h) {
    double u = h.utilization("five_hour");
    if (u < 0 || windowBase < 0) return;
    if (lastFiveHour_ >= 0 && u > lastFiveHour_ + 1e-9) {
        windowBase += u - lastFiveHour_;  // consommé hors orchestration (elle ne travaillait pas)
        changed = true;
    }
    if (lastFiveHour_ < 0 || u > lastFiveHour_) lastFiveHour_ = u;
    // (une baisse est laissée à trackUsage : nouvelle session de 5 h ou bruit de mesure)
}

void Orchestrator::resetBudget(OrchestratorHost& h) {
    if (subscription && windowSpent <= 1e-9 && budgetOverridden()) {
        // Rien de consommé depuis la dernière relance : on revient à la part normale du niveau.
        absorbUntracked(h);
        allowanceOverride_ = -1;
        finalRequested_ = false;
        changed = true;
        addLog(&h, "Budget recalculé : " + pct(sessionAllowance()) + " de la fenêtre (part du niveau sur ce qui reste).", true);
        return;
    }
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
                     "l'orchestration reprendra seule après la réinitialisation de la fenêtre. Si rien d'utile ne tient "
                     "dans cette marge, réponds @@PAUSE (état + plan de reprise) plutôt qu'@@HUMAIN.";
        }
    } else {
        n = settings.maxCostUsd > 0 ? "Budget : " + util::formatUsd(budgetSpentUsd()) + " dépensés sur " + util::formatUsd(settings.maxCostUsd) + "."
                                    : "Budget : aucune limite fixée (dépensé : " + util::formatUsd(budgetSpentUsd()) + ").";
    }
    n += "\nLe budget est un maximum : optimise dans tous les cas (le modèle le plus économique qui suffit à chaque tâche).";
    std::string models;
    for (auto& m : usableModels(settings.allowedModels, blockedModels)) models += (models.empty() ? "" : ", ") + m;
    n += "\nModèles autorisés : " + (models.empty() ? std::string("tous") : models);
    if (!blockedModels.empty()) {
        std::string b;
        for (auto& m : blockedModels) b += (b.empty() ? "" : ", ") + m;
        n += " (hors abonnement de l'utilisateur, ne pas demander : " + b + ")";
    }
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

std::vector<std::string> Orchestrator::brainTools() const {
    if (settings.brainReadOnly) return {"Read", "Glob", "Grep"};
    return {};
}

int64_t Orchestrator::cacheTtlSeconds() const {
    if (settings.cacheTtlMinutes > 0) return (int64_t)settings.cacheTtlMinutes * 60;
    return subscription ? 55 * 60 : 4 * 60;
}

std::string Orchestrator::brainPrompt() const {
    std::string p = systemPrompt(settings.brainReadOnly, settings.allowNewAgents, settings.visualChecks, settings.projectKind);
    if (!settings.browserAgents.empty()) {
        p += "\n\n## Agents équipés du navigateur (🌐)\n";
        for (auto& a : settings.browserAgents) p += "- " + a + "\n";
        p += "Eux seuls peuvent ouvrir une page web dans Chrome (rendu, clics, console, captures).";
    }
    return p;
}

bool Orchestrator::isBrowserAgent(const std::string& agent) const {
    return std::find(settings.browserAgents.begin(), settings.browserAgents.end(), agent) != settings.browserAgents.end();
}

std::string Orchestrator::systemPrompt(bool brainReadOnly, bool canCreateAgents, bool visualChecks,
                                      const std::string& projectKind) {
    const ProjectKind kind = projectKindFromId(projectKind);
    const std::string folder = projectFolderWord(kind);
    std::string role = brainReadOnly
        ? "Tu ne réalises pas le travail toi-même : tu planifies, tu délègues, tu contrôles et tu décides de la suite. Tes outils sont en "
          "LECTURE SEULE (Read, Glob, Grep) : tu ne lances ni tests, ni build, ni commandes. Pour exécuter une vérification, "
          "confie-la à un agent (souvent haiku, effort low : « lance X, rends le résultat en 5 lignes »), ou exige dans la "
          "tâche elle-même la preuve (commande lancée + résultat résumé)."
        : "Tu ne réalises pas le travail toi-même : tu planifies, tu délègues, tu vérifies et tu décides de la suite. Tu peux lire le "
          "dossier et lancer des commandes de vérification (tests, build) pour contrôler le travail rapporté — avec parcimonie : "
          "tu es le modèle le plus coûteux et chaque sortie reste dans ton contexte.";
    std::string verify = brainReadOnly
        ? "- ClaudeDesk te transmet les comptes rendus des agents (au fil de l'eau, puis quand toute l'équipe a fini). Contrôle-les "
          "(lis au besoin les fichiers clés, partiellement) avant de considérer une étape comme faite ; s'il manque une preuve, "
          "délègue une vérification courte ou renvoie une correction."
        : "- ClaudeDesk te transmet les comptes rendus des agents (au fil de l'eau, puis quand toute l'équipe a fini). Vérifie "
          "l'essentiel avant de considérer une étape comme faite ; renvoie une correction si besoin.";
    std::string p = R"(# Rôle : ORCHESTRATEUR (piloté par ClaudeDesk)
Tu coordonnes des agents spécialisés. Chaque agent est une conversation Claude Code distincte, dans le même @@FOLDER@@, avec son propre rôle (décrit dans les notes de son dossier). @@ROLE@@

## Protocole (obligatoire)
Termine CHAQUE réponse à un message de ClaudeDesk par un ou plusieurs blocs de directives, chaque marqueur seul sur sa ligne :

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

@@ATTENDRE
<facultatif : ce que tu attends>
@@FIN
(seulement en réponse à un compte rendu intermédiaire, quand rien d'utile ne peut partir avant les autres comptes rendus)

@@PAUSE
<état actuel (ce qui est fait, ce qui a été interrompu) et plan de reprise>
@@FIN
(quand le quota du compte ou le budget ne permet plus d'avancer proprement : ClaudeDesk suspend l'orchestration et te relance
seul à la prochaine session de 5 h, avec ce message. N'utilise JAMAIS @@HUMAIN pour annoncer une pause : @@HUMAIN est
réservé aux vraies questions.)

## Quand l'humain t'écrit directement
Les messages de ClaudeDesk commencent par un titre « # … » (Objectif, Comptes rendus, Compte rendu intermédiaire,
Réponse de l'humain, Reprise…) ou par une note en italique « _(…)_ ». Tout autre message vient de l'humain, qui discute
avec toi (une question, une explication, comment utiliser ce qui a été livré…). Réponds-lui alors comme dans une
conversation normale : directement, sans section de bilan, sans récapitulatif de l'équipe, sans liste d'« actions
prévues » ni « aucune action ». N'ajoute une directive que si ta réponse change vraiment la suite : confier un travail
(@@DELEGUER), lui poser une décision à trancher (@@HUMAIN), conclure (@@TERMINE). Sans directive, l'orchestration
reste dans son état : l'équipe continue, une question en attente reste posée, une orchestration terminée le reste.
Si son message tranche une question en attente, donne en revanche tes directives pour la suite.

## Travail au fil de l'eau
Tu n'attends pas la fin de toute l'équipe : dès qu'un agent termine alors que d'autres travaillent encore, ClaudeDesk
t'envoie un **compte rendu intermédiaire** (ce qui vient de finir + qui travaille encore). Vérifie si besoin, puis :
- confie aussitôt une nouvelle tâche aux agents libres (@@DELEGUER) si un travail indépendant est prêt — c'est le
  moyen de garder l'équipe occupée ;
- sinon réponds @@ATTENDRE : ClaudeDesk te renverra la suite dès le prochain agent terminé.
Une tâche confiée à un agent encore occupé part après la sienne (file d'attente).

## Optimiser, toujours
Le budget est un PLAFOND, pas un objectif : quel que soit le niveau (même élevé ou sans limite), cherche la façon la plus
efficace de mener le projet. Un travail bien découpé et confié au bon modèle va plus vite et coûte moins cher ; garde la
marge pour les imprévus et les vérifications.

## Choisir le modèle et l'effort de chaque tâche
Indique-les pour CHAQUE délégation (« | modele=… effort=… ») et choisis le plus économique qui suffit — la majorité des
tâches d'un projet ne demandent pas le modèle le plus puissant :
- haiku, effort low/medium : tâches mécaniques (renommage, documentation, mise en forme, vérification simple) ;
- sonnet, effort medium : travail courant (développement, rédaction, analyse, tests, scripts d'installation) ;
  effort high seulement pour un raisonnement difficile (algorithme délicat, bogue retors) — pas pour écrire beaucoup ;
- opus, effort high : conception, architecture, raisonnement délicat, débogage difficile, sujets mal connus ;
- fable ou effort xhigh/max : seulement si une tentative plus économique a échoué.
L'effort se paie surtout en réflexion (jetons de sortie, les plus chers) : il ne rend pas la rédaction meilleure. Une
tâche qui écrit de longs documents ou beaucoup de fichiers simples coûte déjà cher en sortie : effort medium.
Les modèles autorisés et le budget rappelés par ClaudeDesk sont des limites, pas des consignes d'utilisation ; plus la
fin du budget approche, plus tu économises.

## Économie de contexte (ce qui coûte vraiment)
Chaque appel d'outil d'un agent relit TOUT son contexte : une tâche de 100 appels sur 200 k jetons coûte bien plus que
trois tâches nettes de 30 appels. ClaudeDesk indique sous chaque compte rendu les appels et le contexte consommés.
- Des tâches ciblées : donne les chemins de fichiers et le critère de fin, pour que l'agent n'explore pas tout le @@FOLDER@@.
  Une tâche qui dépasse ~60 appels est trop grosse : redécoupe la suite.
- Garde le même modèle pour un agent quand c'est possible : changer le modèle (ou l'effort) d'une conversation déjà
  chargée oblige ClaudeDesk à repartir d'une conversation neuve (récap), le cache étant propre au modèle. Regroupe les
  petites tâches mécaniques sur un agent déjà réglé sur haiku plutôt que de basculer un agent sonnet très chargé.
- Les comptes rendus doivent rester courts ; ne demande jamais de recopier des journaux ou des fichiers entiers.

## Questions à l'humain
Une seule question par bloc @@HUMAIN ; pour plusieurs questions, écris plusieurs blocs. ClaudeDesk les affiche dans un formulaire :
- si la réponse est un choix, termine le bloc par la liste des options, une par ligne au format « - [ ] option » : l'humain pourra cliquer ; mets en premier celle que tu recommandes, suivie de « (recommandé) » ;
- tout le contexte se place AVANT les options, en phrases ou en sous-puces indentées (les puces de premier niveau en fin de bloc sont lues comme des choix).

@@TEAM@@

@@KIND@@

## Règles
- Plusieurs @@DELEGUER vers des agents différents s'exécutent en parallèle : ne parallélise que des tâches indépendantes. Un agent ne reçoit qu'une tâche à la fois.
- L'agent ne voit PAS cette conversation : donne-lui tout le contexte nécessaire dans la tâche.
@@VERIFY@@
- Découpe en étapes de taille raisonnable : une tâche ≈ un livrable vérifiable.
- @@HUMAIN pour : décision majeure non tranchée, information manquante, action irréversible ou externe (envoi, publication, commit, push, achat, matériel physique). Ne demande jamais de commit, de push ni d'envoi aux agents.
- @@TERMINE seulement quand l'objectif est atteint et vérifié, ou quand ClaudeDesk annonce le budget épuisé.
- En réponse à ClaudeDesk, sans directive, l'orchestration ne peut pas avancer.)";
    std::string team = canCreateAgents
        ? R"(## Faire évoluer l'équipe
Si un besoin DURABLE apparaît qu'aucun agent ne couvre (un nouveau sous-système, une application distincte comme une
interface graphique…), propose un nouvel agent :

@@NOUVEL_AGENT <nom du thème, selon la convention de l'équipe (ex. « 09 Interface »)>
Pourquoi : <le besoin, pourquoi aucun agent actuel ne le couvre, ce que ce nouvel agent apportera au projet>
<fiche de rôle : mission, fichiers et dossiers possédés, frontières avec les autres agents, conventions, vérifications attendues>
@@FIN

- ClaudeDesk soumet TOUJOURS la proposition à l'humain, avec ta fiche : il accepte, refuse ou apporte des précisions.
  Rien n'est créé sans son accord ; inutile d'ajouter un @@HUMAIN pour demander la permission.
- Commence par « Pourquoi : » : c'est ce que l'humain lira pour décider. La fiche devient ensuite les notes du thème,
  relues par l'agent à chaque tâche : écris-la pour lui, autonome et précise.
- Ne confie aucune tâche au nouvel agent avant la décision : ClaudeDesk te la transmettra (créé, refusé, ou précisions :
  révise alors ta proposition et soumets-la de nouveau). Les autres agents peuvent continuer pendant ce temps.
- Une tâche ponctuelle ne justifie pas un agent : confie-la à l'agent le plus proche. Chaque agent ajoute un contexte
  fixe et de la coordination.
- Si le dossier tient des fiches d'agents (ex. docs/agents/), fais-y ajouter la sienne une fois l'agent créé.)"
        : "## Équipe\nL'équipe est fixée par l'humain : tu ne peux pas créer d'agent. Si un besoin durable n'est couvert par "
          "aucun agent, signale-le (@@HUMAIN) en proposant le rôle à créer.";
    if (visualChecks)
        verify += "\n- Vérification VISUELLE : pour tout livrable visible (interface, page web, graphique, rendu), exige dans la "
                  "tâche une capture PNG enregistrée dans `_captures/` à la racine du @@FOLDER@@, que l'agent examine lui-même et "
                  "dont il donne le chemin. Tu peux ouvrir une capture avec Read (les images s'affichent) pour juger "
                  "toi-même : ~1,5 k jetons par image, réserve-le aux livrables visuels importants ou douteux. Pour une page "
                  "web, confie la vérification à un agent équipé du navigateur (repéré « 🌐 navigateur » dans la liste des "
                  "agents) : il peut l'ouvrir, cliquer, lire la console et faire la capture.";
    util::replaceAll(p, "@@ROLE@@", role);
    util::replaceAll(p, "@@VERIFY@@", verify);
    util::replaceAll(p, "@@TEAM@@", team);
    util::replaceAll(p, "@@KIND@@", orchestratorKindGuidance(kind));
    util::replaceAll(p, "@@FOLDER@@", folder);
    return p;
}

std::string Orchestrator::firstMessage(const std::vector<AgentInfo>& agents, const std::string& planName) const {
    std::string m = "# Objectif\n" + util::trim(settings.objective) + "\n\n# Agents disponibles\n";
    for (auto& a : agents)
        m += "- **" + a.name + "**" + (isBrowserAgent(a.name) ? " 🌐 navigateur" : "") + (a.brief.empty() ? "" : " — " + a.brief) + "\n";
    m += "\n# Moyens\n";
    if (subscription)
        m += "Compte avec abonnement" + (planName.empty() ? std::string() : " (" + planName + ")") +
             " : le budget est une part du quota de la fenêtre de 5 h.\n";
    else
        m += "Facturation à l'usage (API) : le budget est en dollars.\n";
    m += budgetNotes() + "\n";
    if (settings.strictBudget && budgetFraction() >= 0)
        m += "Le résultat doit tenir dans ce budget : planifie en conséquence (l'essentiel d'abord).\n";
    m += "Jusqu'à " + std::to_string(std::max(1, settings.maxParallel)) + " agents travaillent en parallèle.\n";
    if (settings.allowNewAgents)
        m += "Équipe de " + std::to_string(agents.size()) + " agents" +
             (settings.maxAgents > 0 ? " (au plus " + std::to_string(settings.maxAgents) + ")" : std::string()) +
             " : tu peux proposer un nouvel agent (@@NOUVEL_AGENT), soumis à l'accord de l'humain.\n";
    const ProjectKind kind = projectKindFromId(settings.projectKind);
    m += std::string("\nUtilise exactement les noms ci-dessus dans @@DELEGUER. Commence par examiner l'état du ") +
         projectFolderWord(kind) +
         (kind == ProjectKind::Software ? " (plan, documentation, code existant)"
                                        : " (plan, documents, données ou code déjà présents)") +
         ", puis donne tes premières directives.";
    return m;
}

static std::string taskMessage(const OrchTask& t, bool visualChecks, bool browser, ProjectKind kind) {
    const std::string folder = projectFolderWord(kind);
    const bool code = kind == ProjectKind::Software || kind == ProjectKind::Hybrid || kind == ProjectKind::Data;
    std::string visual;
    if (visualChecks)
        visual = "Si la tâche change quelque chose de visible (interface, page, graphique), fais-en une capture PNG dans "
                 "`_captures/` à la racine du " + folder + " (crée le dossier au besoin, avec un `.gitignore` contenant `*`), "
                 "nommée d'après la tâche ; examine-la toi-même avec Read et donne son chemin dans **Vérifié**. " +
                 std::string(browser ? "Tu disposes du navigateur (outils Claude in Chrome) : pour une page web, ouvre-la, "
                                       "vérifie le rendu et la console, puis capture-la.\n"
                                     : "Utilise l'option de capture du projet s'il en a une.\n");
    return "[Tâche de l'orchestrateur — cycle " + std::to_string(t.cycle) + "]\n\n" + t.task +
           "\n\n---\nTravaille en autonomie jusqu'au bout de cette tâche (pas de question à l'humain sauf décision "
           "irréversible ; ni commit, ni push, ni envoi).\n" +
           std::string(code ? "Économise ton contexte (chaque appel d'outil relit toute la conversation) : limite les sorties de "
                              "commandes (`| tail -n 30`, `grep`, options silencieuses), lis les fichiers par parties utiles, "
                              "évite les boucles d'essais à l'aveugle."
                            : "Économise ton contexte (chaque appel d'outil relit toute la conversation) : lis les documents "
                              "par parties utiles, n'ouvre que ce qui sert la tâche.") +
           " Si la tâche s'avère bien plus grosse que prévu, livre une étape propre et arrête-toi : "
           "l'orchestrateur redécoupera la suite.\n" + visual +
           "Termine par un compte rendu de 15 lignes au plus, sans journaux ni contenu recopiés : **Fait**, **Vérifié** "
           "(" + taskVerifyHint(kind) + "), **Fichiers**, **Reste / bloquant**.";
}

int Orchestrator::tasksInFlight() const {
    int n = 0;
    for (auto& t : tasks)
        if (!t.done) ++n;
    return n;
}

void Orchestrator::markReported() {
    for (auto& t : tasks)
        if (t.done) t.reported = true;
    teamNotes_.clear();
    changed = true;
}

// Comptes rendus pas encore transmis, puis les agents toujours au travail (ou en file).
std::string Orchestrator::progressSection(int64_t now) const {
    std::string r;
    bool any = false;
    for (auto& t : tasks) {
        if (!t.done || t.reported) continue;
        any = true;
        r += "## " + t.agent + (t.failed ? " — ÉCHEC" : !t.guardNote.empty() ? " — INTERROMPUE" : " — terminé");
        if (!t.model.empty() || !t.effort.empty())
            r += " (" + (t.model.empty() ? std::string("modèle par défaut") : t.model) +
                 (t.effort.empty() ? "" : ", effort " + t.effort) + ")";
        r += "\n**Tâche :** " + util::firstLine(t.task, 200) + "\n";
        if (t.calls > 0)
            r += "_Coût : " + std::to_string(t.calls) + " appels · contexte final " + kTokens(t.contextEnd) + " · " +
                 kTokens(t.cacheRead) + " relus en cache · " + kTokens(t.cacheWrite) + " écrits_\n";
        if (!t.guardNote.empty())
            r += "**⚠ Interrompue par ClaudeDesk : " + t.guardNote +
                 ".** Le travail est partiel : vérifie l'état, puis confie la suite en tâches plus petites et plus ciblées.\n";
        r += "\n";
        std::string rep = util::trim(t.report).empty() ? "(aucun texte rendu)" : t.report;
        size_t cap = (size_t)std::max(500, settings.reportMaxChars);
        if (rep.size() > cap) {
            // Le compte rendu structuré est à la fin du tour : on garde la fin (UTF-8 respecté).
            size_t cut = rep.size() - cap;
            while (cut < rep.size() && ((unsigned char)rep[cut] & 0xC0) == 0x80) ++cut;
            rep = "(… début omis — texte complet dans la conversation de l'agent)\n" + rep.substr(cut);
        }
        r += rep + "\n\n";
    }
    if (!teamNotes_.empty()) {
        any = true;
        r += "## Équipe\n";
        for (auto& n : teamNotes_) r += "- " + n + "\n";
        r += "\n";
    }
    if (!any) r += "_(Aucun nouveau compte rendu depuis le précédent.)_\n\n";
    // Délégations sans modèle : le défaut a servi partout. Rappel pour que le cerveau optimise.
    std::string unchosen;
    for (auto& t : tasks)
        if (t.done && !t.reported && t.sent && !t.modelChosen) unchosen += (unchosen.empty() ? "" : ", ") + t.agent;
    if (!unchosen.empty())
        r += "**Rappel** : tu n'as pas choisi de modèle pour " + unchosen + " — le défaut des agents (" +
             (settings.defaultWorkerModel.empty() ? std::string("celui de leur conversation") : settings.defaultWorkerModel) +
             ") a été appliqué. Indique « | modele=… effort=… » à CHAQUE délégation : haiku pour la mécanique, sonnet pour "
             "le courant, opus pour le difficile (conception, débogage, sujets mal connus).\n\n";
    std::string running;
    for (auto& t : tasks) {
        if (t.done) continue;
        running += "- **" + t.agent + "** — ";
        if (t.sent) {
            int64_t mins = t.sentAt > 0 && now > t.sentAt ? (now - t.sentAt) / 60 : 0;
            running += "au travail depuis " + std::to_string(mins) + " min";
        } else {
            running += "en file d'attente";
        }
        running += " : " + util::firstLine(t.task, 160) + "\n";
    }
    if (!running.empty()) r += "## Toujours en cours\n" + running + "\n";
    return r;
}

std::string Orchestrator::buildReport(bool partial, int64_t now) const {
    std::string r = (partial ? "# Compte rendu intermédiaire — cycle " : "# Comptes rendus — cycle ") + std::to_string(cycle) +
                    "\n\n" + progressSection(now);
    r += "---\n" + budgetNotes() + "\nCycle " + std::to_string(cycle);
    if (settings.maxCycles > 0) r += " / " + std::to_string(settings.maxCycles);
    if (partial)
        r += ".\nLes agents « toujours en cours » continuent : ne leur confie rien qui dépende de leur résultat. Vérifie ce "
             "qui doit l'être, puis relance dès maintenant les agents libres (@@DELEGUER), pose une question (@@HUMAIN) ou "
             "attends les comptes rendus suivants (@@ATTENDRE … @@FIN).";
    else
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
        recapTeam(h, "pause du budget");
        return;
    }
    pauseFrom(state, "budget épuisé (" + budgetLabel() + ") — augmentez-le puis reprenez", &h);
}

// L'orchestrateur suspend lui-même (@@PAUSE) : pas de question pour l'humain, reprise automatique à la prochaine
// session de 5 h, avec son plan de reprise.
void Orchestrator::brainPause(OrchestratorHost& h, const std::string& note) {
    ParticipantStatus st = h.status(brainConvId);
    brainTurnsAtSend_ = st.turns;  // reprise : le message de reprise partira (aucune réponse en attente)
    brainCostAtSend_ = st.costUsd;
    pauseNote = util::trim(note).empty() ? std::string("(pause sans précision)") : util::trim(note);
    int64_t r = h.resetsAt("five_hour");
    pauseUntil = r > h.now() ? r : (windowResetsAt_ > h.now() ? windowResetsAt_ : 0);
    resumeState_ = OrchState::Thinking;
    state = OrchState::UsagePause;
    usageWindow_ = "brain";
    pauseReason = pauseUntil > h.now()
                      ? "l'orchestrateur s'est mis en pause : reprise automatique à la prochaine session de 5 h (dans " +
                            util::humanDuration(pauseUntil - h.now()) + ")"
                      : "l'orchestrateur s'est mis en pause : reprise automatique dès la prochaine session de 5 h (ou "
                        "cliquez sur Reprendre)";
    changed = true;
    addLog(&h, "⏸ L'orchestrateur se met en pause : " + util::firstLine(pauseNote, 150), true);
    recapTeam(h, "pause");
}

void Orchestrator::recapTeam(OrchestratorHost& h, const std::string& why) {
    if (!settings.recapOnPhaseEnd) return;
    std::vector<std::string> ids;
    auto add = [&](const std::string& id) {
        if (id.empty() || std::find(ids.begin(), ids.end(), id) != ids.end()) return;
        ParticipantStatus st = h.status(id);
        if (st.busy) return;  // encore au travail : son récap se fera à sa prochaine rotation
        ids.push_back(id);
    };
    add(brainConvId);
    for (auto& [agent, id] : agentConv) add(id);
    if (ids.empty()) return;
    addLog(&h, "Fin de phase (" + why + ") : récaps de l'équipe pour la mémoire du projet (" + std::to_string(ids.size()) +
                   " conversation(s), celles déjà à jour sont ignorées).");
    h.recapConversations(ids);
}

std::string Orchestrator::resumeAfterPauseMessage() const {
    std::string quoted;
    for (auto& l : util::splitLines(pauseNote)) quoted += "> " + l + "\n";
    return "# Reprise après ta pause\n\nTu avais suspendu l'orchestration avec ce message :\n\n" + quoted + "\n" +
           budgetNotes() +
           "\n\nDes agents ont pu être interrompus pendant la pause : vérifie l'état du dépôt, puis reprends ton plan "
           "(@@DELEGUER / @@HUMAIN / @@PAUSE / @@TERMINE).";
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
    pendingAgents.clear();
    teamNotes_.clear();
    pauseReason.clear();
    pauseNote.clear();
    pauseUntil = 0;
    nudges_ = 0;
    finalRequested_ = false;
    brainRotating_ = false;
    pendingBrain_.clear();
    subscription = resolveSubscription(settings, h.isSubscription());
    lastFiveHour_ = h.utilization("five_hour");
    if (lastFiveHour_ >= 0) beginWindow(lastFiveHour_);
    std::string id = h.openBrain(brainConvId, brainPrompt(), brainTools());
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

bool Orchestrator::sendBrain(OrchestratorHost& h, const std::string& msg) {
    SendOptions opt{settings.brainMode, clampModel(settings.brainModel, usableModels(settings.allowedModels, blockedModels)),
                    clampEffort(settings.brainEffort, settings.maxEffort), settings.leanAgents};
    // Avant d'écrire au cerveau : s'il est trop chargé, ou froid (cache expiré pendant que l'équipe travaillait ou
    // pendant une pause), il repart d'une conversation neuve — le message attend la fin du récap (voir tick).
    // Réglages du cerveau (prompt, outils en lecture seule) tenus à jour : sans effet s'ils n'ont pas changé.
    if (!brainConvId.empty() && h.ensureOpen(brainConvId))
        h.openBrain(brainConvId, brainPrompt(), brainTools());
    ParticipantStatus st = h.status(brainConvId);
    if (!st.busy) {
        std::string why = rotationReasonFor(st, brainConvId, h.now(), opt.model, opt.effort);
        if (!why.empty()) {
            std::string nid = h.rotate("", brainConvId);
            if (nid.empty()) {
                if (!brainRotating_) addLog(&h, "↻ Orchestrateur : récap avant de repartir à neuf — " + why + ".", true);
                brainRotating_ = true;
                pendingBrain_ = msg;
                lastBrainMessage_ = msg;  // rechargé en pause : resume() le renverra
                brainTurnsAtSend_ = st.turns;
                state = OrchState::Thinking;
                changed = true;
                return true;
            }
            addLog(&h, "↻ Orchestrateur : contexte allégé (récap + nouvelle conversation) — " + why + ".", true);
            conversationReplaced(brainConvId, nid);
            st = h.status(brainConvId);
        }
    }
    brainRotating_ = false;
    pendingBrain_.clear();
    std::string text = brainRefresh_ ? brainRefresher() + msg : msg;
    brainRefresh_ = false;
    brainTurnsAtSend_ = st.turns;
    brainCostAtSend_ = st.costUsd;
    lastBrainMessage_ = text;
    brainHumanNotified_ = false;
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
    std::string asked = humanQuestion;
    for (auto& a : pendingAgents) asked += (asked.empty() ? "" : "\n\n") + agentQuestion(a).title;
    qaHistory.push_back({h.now(), asked, answer});
    while (qaHistory.size() > 50) qaHistory.pop_front();
    addLog(&h, "Vous : " + util::firstLine(answer, 150), true);
    std::string decisions = resolveAgentProposals(h, answer);  // avant d'effacer la question (précisions : à qui ?)
    questions.clear();
    humanQuestion.clear();
    std::string msg = "# Réponse de l'humain\n\n" + answer + "\n\n" + decisions;
    // Pendant la question, l'équipe a continué : on joint ce qui a avancé.
    bool news = tasksInFlight() > 0 || !teamNotes_.empty();
    for (auto& t : tasks)
        if (t.done && !t.reported) news = true;
    if (news) {
        msg += "# Avancement de l'équipe pendant la question\n\n" + progressSection(h.now());
        markReported();
    }
    return sendBrain(h, msg + "Poursuis et donne tes directives.");
}

// ---------------------------------------------------------------------------
// Équipe évolutive : propositions de nouveaux agents, toujours soumises à l'humain
// ---------------------------------------------------------------------------

std::string Orchestrator::agentAcceptLabel(const std::string& name) { return "Créer l'agent « " + name + " »"; }
std::string Orchestrator::agentRefuseLabel(const std::string& name) { return "Ne pas créer « " + name + " »"; }

HumanQuestion Orchestrator::agentQuestion(const NewAgent& a) {
    HumanQuestion q;
    q.title = "Ajouter l'agent « " + a.name + " » à l'équipe ?";
    q.text = "**L'orchestrateur propose un nouvel agent : « " + a.name + " »**\n\n" + a.notes +
             "\n\n_Acceptez, refusez, ou choisissez « Autre » pour préciser ce que vous attendez de cet agent : "
             "l'orchestrateur révisera sa proposition et vous la soumettra de nouveau._";
    q.options = {agentAcceptLabel(a.name), agentRefuseLabel(a.name)};
    return q;
}

std::vector<HumanQuestion> Orchestrator::proposeAgents(OrchestratorHost& h, const std::vector<NewAgent>& list) {
    std::vector<HumanQuestion> qs;
    if (list.empty()) return qs;
    std::vector<std::string> names;
    for (auto& a : h.agents()) names.push_back(a.name);
    size_t count = names.size();
    for (const NewAgent& a : list) {
        std::string why;
        bool taken = false;
        for (auto& n : names) taken = taken || normKey(n) == normKey(a.name);
        for (auto& p : pendingAgents) taken = taken || normKey(p.name) == normKey(a.name);
        if (!settings.allowNewAgents)
            why = "la création d'agents est désactivée par l'humain (réglages › Exécution) : répartis ce travail sur "
                  "l'équipe actuelle, ou signale le besoin par @@HUMAIN";
        else if (a.name.empty())
            why = "nom manquant (« @@NOUVEL_AGENT <nom> »)";
        else if (taken)
            why = "un agent porte déjà ce nom : utilise-le, ou propose un autre nom";
        else if (util::trim(a.notes).empty())
            why = "fiche de rôle vide : explique « Pourquoi : … » puis décris sa mission et ses frontières";
        else if (settings.maxAgents > 0 && count >= (size_t)settings.maxAgents)
            why = "l'équipe a atteint son plafond de " + std::to_string(settings.maxAgents) +
                  " agents (réglable par l'humain) : répartis ce travail sur l'équipe actuelle";
        if (!why.empty()) {
            teamNotes_.push_back("Proposition d'agent « " + (a.name.empty() ? std::string("?") : a.name) +
                                 " » non soumise : " + why + ".");
            addLog(&h, "Proposition d'agent écartée (" + (a.name.empty() ? std::string("sans nom") : a.name) + ") : " +
                           util::firstLine(why, 120));
            continue;
        }
        ++count;
        pendingAgents.push_back(a);
        qs.push_back(agentQuestion(a));
        addLog(&h, "L'orchestrateur propose un nouvel agent : « " + a.name + " » (en attente de votre accord).", true);
    }
    changed = true;
    return qs;
}

std::string Orchestrator::resolveAgentProposals(OrchestratorHost& h, const std::string& answer) {
    if (pendingAgents.empty()) return {};
    // Précisions libres jointes à la réponse : gardées dans la fiche de l'agent créé (s'il est seul en jeu).
    std::string extra;
    for (const std::string& l : util::splitLines(answer))
        if (util::startsWith(util::trim(l), "Précisions :")) extra = util::trim(util::trim(l).substr(std::string("Précisions :").size()));
    std::string r = "# Décision sur l'équipe\n";
    for (const NewAgent& a : pendingAgents) {
        if (answer.find(agentAcceptLabel(a.name)) != std::string::npos) {
            std::string notes = a.notes;
            if (!extra.empty() && pendingAgents.size() == 1 && util::trim(humanQuestion).empty())
                notes += "\n\n## Précisions de l'humain\n" + extra;
            std::string error;
            std::string created = h.createAgent(a.name, notes, error);
            if (!created.empty()) {
                r += "- **" + created + "** : accepté et créé (sa fiche est dans les notes de son thème). Tu peux lui "
                     "confier des tâches avec @@DELEGUER " + created + ".\n";
                addLog(&h, "Nouvel agent créé : « " + created + " ».", true);
                h.onEvent("Nouvel agent dans l'équipe : « " + created + " »", true);
            } else {
                r += "- **" + a.name + "** : accepté, mais la création a échoué (" + error +
                     "). Poursuis avec l'équipe actuelle ou propose un autre nom.\n";
                addLog(&h, "Création de l'agent « " + a.name + " » impossible : " + error, true);
            }
        } else if (answer.find(agentRefuseLabel(a.name)) != std::string::npos) {
            r += "- **" + a.name + "** : refusé par l'humain. Répartis ce travail sur les agents existants ; ne repropose "
                 "pas cet agent sans élément nouveau.\n";
            addLog(&h, "Agent « " + a.name + " » refusé.");
        } else {
            r += "- **" + a.name + "** : ni accepté ni refusé — l'agent n'est PAS créé. Si l'humain a apporté des "
                 "précisions, révise ta proposition en conséquence et soumets-la de nouveau (@@NOUVEL_AGENT) ; sinon "
                 "poursuis avec l'équipe actuelle. La création d'un agent exige toujours son accord explicite.\n";
            addLog(&h, "Agent « " + a.name + " » : précisions demandées, proposition à réviser.");
        }
    }
    pendingAgents.clear();
    changed = true;
    return r + "\n";
}

void Orchestrator::askHuman(OrchestratorHost& h, const std::string& rawQuestion, const std::vector<HumanQuestion>& qs,
                            const std::string& logExtra) {
    state = OrchState::WaitingHuman;
    humanQuestion = rawQuestion;
    questions = qs;
    ++questionSeq;
    ParticipantStatus st = h.status(brainConvId);
    brainTurnsAtSend_ = st.turns;
    brainCostAtSend_ = st.costUsd;
    if (!rawQuestion.empty() || pendingAgents.empty())
        addLog(&h, "Question pour vous : " + util::firstLine(qs.empty() ? rawQuestion : qs[0].text, 150) + logExtra, true);
    changed = true;
}

void Orchestrator::pause(const std::string& reason) {
    if (!running()) return;
    if (state != OrchState::UsagePause) resumeState_ = state;
    state = OrchState::Paused;
    pauseReason = reason;
    addLog(nullptr, "Pause : " + reason, true);
}

void Orchestrator::limitReached(OrchestratorHost& h, const std::string& win) {
    if (!running() && state != OrchState::Paused) return;
    const int64_t now = h.now();
    if (state == OrchState::UsagePause && usageWindow_ == win && limitUntil_ > now) return;  // déjà en pause pour ça
    int64_t r = h.resetsAt(win);
    limitUntil_ = r > now ? r + 30 : now + 600;  // réinitialisation inconnue : nouvel essai dans 10 min
    if (state == OrchState::Paused) return;      // pause manuelle : elle reste manuelle
    if (state != OrchState::UsagePause) resumeState_ = state;
    state = OrchState::UsagePause;
    usageWindow_ = win;
    std::string w = win == "seven_day" ? "quota hebdomadaire" : "session de 5 h";
    pauseReason = "limite du compte atteinte (" + w + ") : reprise automatique " +
                  (r > now ? "dans " + util::humanDuration(limitUntil_ - now) : std::string("dans 10 min"));
    changed = true;
    addLog(&h, "Pause : " + pauseReason, true);
}

void Orchestrator::stop() {
    if (state == OrchState::Idle) return;
    state = OrchState::Idle;
    pauseReason.clear();
    pauseNote.clear();
    pauseUntil = 0;
    limitUntil_ = 0;
    questions.clear();
    pendingAgents.clear();
    addLog(nullptr, "Orchestration arrêtée.", true);
}

void Orchestrator::resume(OrchestratorHost& h) {
    if (state != OrchState::Paused && state != OrchState::UsagePause) return;
    OrchState to = resumeState_;
    limitUntil_ = 0;  // reprise (automatique ou à la main) : la limite sera de nouveau signalée si elle tient
    if (to != OrchState::Working && to != OrchState::WaitingHuman) to = OrchState::Thinking;
    pauseReason.clear();
    if (!exhausted()) finalRequested_ = false;  // budget relevé : on repart normalement
    if (!brainConvId.empty()) h.ensureOpen(brainConvId);
    reviveTasks(h);
    absorbUntracked(h);  // usage consommé ailleurs pendant la pause : il ne revient pas à l'orchestration
    state = to;
    changed = true;
    addLog(&h, "Reprise.", true);
    if (to == OrchState::Thinking) {
        ParticipantStatus st = h.status(brainConvId);
        if (!st.exists) {
            pauseFrom(OrchState::Thinking, "conversation de l'orchestrateur introuvable", &h);
            return;
        }
        if (!pauseNote.empty()) {  // fin d'une pause décidée par l'orchestrateur : il reprend son plan
            snapshotQuota(h);
            std::string msg = resumeAfterPauseMessage();
            pauseNote.clear();
            pauseUntil = 0;
            if (!st.busy) sendBrain(h, msg);
        } else if (!st.busy && st.turns <= brainTurnsAtSend_) {
            // Le dernier message n'a pas obtenu de réponse (fermeture, plantage…) : on le renvoie.
            std::string msg = lastBrainMessage_.empty() ? "Reprends là où tu en étais et donne tes directives." : lastBrainMessage_;
            const std::string tag = "_(Reprise après interruption";
            if (!util::startsWith(msg, tag))
                msg = tag + " : ce message a peut-être déjà été traité en partie. Vérifie l'état du dépôt avant d'agir.)_\n\n" + msg;
            snapshotQuota(h);
            sendBrain(h, msg);
        }
    }
}

// Tâches en vol (les agents travaillent aussi pendant que le cerveau réfléchit ou attend une réponse) : conversations
// rouvertes ; une tâche dont le tour n'a pas abouti (processus disparu) repart via dispatch.
void Orchestrator::reviveTasks(OrchestratorHost& h) {
    for (auto& t : tasks) {
        if (!t.sent || t.done) continue;
        h.ensureOpen(t.convId);
        ParticipantStatus st = h.status(t.convId);
        if (!st.busy && st.turns <= t.turnsAtSend) t.sent = false;
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
    else if (state == OrchState::Finished && subscription) absorbUntracked(h);  // affichage et reprise à jour
    // L'équipe avance indépendamment du cerveau : on relève les tâches terminées et on envoie celles en file,
    // qu'il réfléchisse, attende une réponse humaine ou attende les agents.
    if (canDispatch()) {
        if (reviveOnTick_) {
            reviveOnTick_ = false;
            reviveTasks(h);
        }
        collectTasks(h);
        dispatch(h);
    }
    // Message au cerveau en attente de son récap (rotation) : il part dès que la nouvelle conversation est prête.
    if (state == OrchState::Thinking && !pendingBrain_.empty()) {
        std::string m = pendingBrain_;
        sendBrain(h, m);
        return;
    }
    switch (state) {
        case OrchState::Thinking: tickThinking(h); break;
        case OrchState::Working: tickWorking(h); break;
        case OrchState::Finished: tickFinished(h); break;
        case OrchState::WaitingHuman: {
            // Réponse donnée directement dans la conversation de l'orchestrateur.
            ParticipantStatus st = h.status(brainConvId);
            if (st.exists && !st.busy && st.turns > brainTurnsAtSend_) {
                spentUsd += std::max(0.0, st.costUsd - brainCostAtSend_);
                if (!hasDirectives(parseDirectives(st.lastText))) {
                    // Simple discussion (une précision, une question de votre part) : la question reste posée.
                    brainTurnsAtSend_ = brainSeenTurns_ = st.turns;
                    brainCostAtSend_ = st.costUsd;
                    changed = true;
                    break;
                }
                if (!humanQuestion.empty()) qaHistory.push_back({h.now(), humanQuestion, "(répondu dans la conversation)"});
                humanQuestion.clear();
                questions.clear();
                addLog(&h, "Vous avez répondu : l'orchestrateur reprend.");
                handleBrainAnswer(h, st.lastText);
            }
            break;
        }
        case OrchState::UsagePause: {
            if (usageWindow_ == "brain") {  // pause décidée par l'orchestrateur : reprise à la session suivante
                int64_t r = h.resetsAt("five_hour");
                if (pauseUntil == 0 && r > h.now()) pauseUntil = r, changed = true;
                bool reset = pauseUntil > 0 && (h.now() > pauseUntil + 30 || r > pauseUntil + 60 ||
                                                windowResetsAt_ > pauseUntil + 60);
                if (reset) {
                    state = OrchState::Paused;
                    addLog(&h, "Nouvelle session de 5 h : l'orchestrateur reprend après sa pause.", true);
                    resume(h);
                }
                break;
            }
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
            if (limitUntil_ > 0) {  // refus « limite atteinte » reçu : la mesure peut être périmée, on attend la date
                if (h.now() < limitUntil_) break;
                limitUntil_ = 0;
                state = OrchState::Paused;
                addLog(&h, "Limite du compte réinitialisée : l'orchestration reprend.");
                resume(h);
                break;
            }
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

bool Orchestrator::brainChatReply(OrchestratorHost& h) {
    ParticipantStatus st = h.status(brainConvId);
    if (!st.exists || st.busy) return false;
    if (brainSeenTurns_ < 0 || st.turns < brainSeenTurns_) {  // ancienne sauvegarde ou conversation remplacée
        brainSeenTurns_ = st.turns;
        return false;
    }
    if (st.turns == brainSeenTurns_) return false;
    spentUsd += std::max(0.0, st.costUsd - brainCostAtSend_);
    brainCostAtSend_ = st.costUsd;
    brainSeenTurns_ = st.turns;
    changed = true;
    if (!hasDirectives(parseDirectives(st.lastText))) return false;  // simple discussion : l'équipe continue
    addLog(&h, "Directives données pendant votre discussion avec l'orchestrateur.");
    brainTurnsAtSend_ = st.turns;
    handleBrainAnswer(h, st.lastText);
    return true;
}

// Orchestration terminée, mais vous continuez à discuter avec l'orchestrateur. S'il répond par une question
// (@@HUMAIN) ou confie du travail (@@DELEGUER), l'orchestration reprend du service ; sinon ce n'est qu'une discussion
// (un nouveau @@TERMINE met simplement le bilan à jour).
void Orchestrator::tickFinished(OrchestratorHost& h) {
    if (brainConvId.empty()) return;
    ParticipantStatus st = h.status(brainConvId);
    if (!st.exists || st.busy || st.turns <= brainTurnsAtSend_) return;
    spentUsd += std::max(0.0, st.costUsd - brainCostAtSend_);
    Directives d = parseDirectives(st.lastText);
    if (!d.needHuman && d.delegations.empty()) {
        brainTurnsAtSend_ = brainSeenTurns_ = st.turns;
        brainCostAtSend_ = st.costUsd;
        if (d.finished && !util::trim(d.summary).empty()) finalSummary = d.summary;
        changed = true;
        return;
    }
    addLog(&h, "Vous avez repris la discussion avec l'orchestrateur : l'orchestration reprend.", true);
    nudges_ = 0;
    state = OrchState::Thinking;
    handleBrainAnswer(h, st.lastText);
}

void Orchestrator::handleBrainAnswer(OrchestratorHost& h, const std::string& text) {
    Directives d = parseDirectives(text);
    changed = true;
    brainSeenTurns_ = h.status(brainConvId).turns;
    // Propositions restées sans décision par le formulaire (l'humain a répondu dans la conversation) : caduques.
    for (auto& a : pendingAgents)
        teamNotes_.push_back("Proposition d'agent « " + a.name + " » restée sans décision (l'humain a répondu dans la "
                             "conversation) : rien n'a été créé ; soumets-la de nouveau si elle reste utile.");
    pendingAgents.clear();
    // Nouveaux agents : jamais créés d'office. Chaque proposition devient une question pour l'humain.
    std::vector<HumanQuestion> agentQs = proposeAgents(h, d.newAgents);
    if (!agentQs.empty() && !d.needHuman) {
        // Rien d'autre à trancher : les tâches confiées aux agents existants partent, l'humain décide pendant ce temps.
        if (d.finished && !util::trim(d.summary).empty()) finalSummary = d.summary;
        nudges_ = 0;
        if (!d.delegations.empty()) delegate(h, d.delegations, false);
        askHuman(h, "", agentQs, "");
        return;
    }
    if (!agentQs.empty()) d.questions.insert(d.questions.end(), agentQs.begin(), agentQs.end());
    // « J'ai fini… voulez-vous en plus … ? » : la question l'emporte (le bilan est gardé), sinon elle serait perdue.
    if (d.finished && d.needHuman && !util::trim(d.summary).empty()) finalSummary = d.summary;
    if (d.finished && !d.needHuman) {
        state = OrchState::Finished;
        finalSummary = d.summary;
        {  // la réponse est traitée : tickFinished ne doit pas la relire
            ParticipantStatus st = h.status(brainConvId);
            brainTurnsAtSend_ = st.turns;
            brainCostAtSend_ = st.costUsd;
        }
        addLog(&h, "Objectif atteint : " + util::firstLine(d.summary, 150), true);
        if (int n = tasksInFlight(); n > 0)
            addLog(&h, std::to_string(n) + " tâche(s) encore en cours ou en file : celles déjà lancées se terminent dans "
                       "leur conversation.");
        recapTeam(h, "objectif atteint");
        return;
    }
    if (d.needHuman) {
        askHuman(h, d.question, d.questions, d.delegations.empty() ? "" : " (ses délégations attendront votre réponse)");
        return;
    }
    if (d.pause) {
        nudges_ = 0;
        brainPause(h, d.pauseNote);
        return;
    }
    if (d.delegations.empty() && !d.newAgents.empty()) {
        // Propositions toutes écartées d'office (plafond, nom pris…) : le motif part au prochain compte rendu.
        nudges_ = 0;
        state = OrchState::Working;
        if (tasksInFlight() == 0) sendReport(h, false);
        return;
    }
    if (d.delegations.empty() && tasksInFlight() > 0) {
        // @@ATTENDRE (ou rien) alors que des agents travaillent encore : on attend leurs comptes rendus.
        nudges_ = 0;
        state = OrchState::Working;
        addLog(&h, "L'orchestrateur attend les comptes rendus des agents encore au travail.");
        return;
    }
    if (d.delegations.empty()) {
        if (++nudges_ > 2) {
            HumanQuestion q;
            q.title = q.text = "L'orchestrateur ne donne plus de directives. Que doit-il faire ?";
            askHuman(h, q.text, {q}, "");
            return;
        }
        addLog(&h, "Réponse sans directive : relance de l'orchestrateur.");
        sendBrain(h, std::string("Ta réponse ne contient aucune directive") +
                         (d.wait ? " exploitable : plus aucun agent ne travaille, @@ATTENDRE n'attendrait rien" : "") +
                         ". Termine par un bloc @@DELEGUER <agent> … @@FIN, @@HUMAIN … @@FIN ou @@TERMINE … @@FIN "
                         "(voir le protocole).");
        return;
    }
    nudges_ = 0;
    delegate(h, d.delegations, true);
}

void Orchestrator::delegate(OrchestratorHost& h, const std::vector<Delegation>& dels, bool reportFailures) {
    std::vector<AgentInfo> ag = h.agents();
    std::vector<std::string> names;
    for (auto& a : ag) names.push_back(a.name);
    std::string available;
    for (auto& n : names) available += (available.empty() ? "" : ", ") + n;
    std::vector<std::string> proposed;
    for (auto& a : pendingAgents) proposed.push_back(a.name);

    ++cycle;
    // On garde les tâches encore en cours / en file et les comptes rendus pas encore transmis.
    tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [](const OrchTask& t) { return t.done && t.reported; }),
                tasks.end());
    int valid = 0;
    for (auto& del : dels) {
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
            if (int p = matchAgent(del.agent, proposed); p >= 0) {
                t.report = "« " + proposed[p] + " » n'existe pas encore : sa création attend l'accord de l'humain. "
                           "Redélègue cette tâche une fois l'agent créé.";
                addLog(&h, "Tâche pour « " + proposed[p] + " » écartée : agent pas encore accepté.");
            } else {
                t.report = "Agent inconnu « " + del.agent + " ». Agents disponibles : " + available + ".";
                addLog(&h, "Agent inconnu : « " + del.agent + " »", true);
            }
        } else {
            t.agent = names[idx];
            ++valid;
        }
        tasks.push_back(std::move(t));
    }
    state = OrchState::Working;
    if (valid == 0) {  // échecs immédiats : rapportés tout de suite si personne ne travaille, sinon au prochain compte rendu
        if (reportFailures && tasksInFlight() == 0) sendReport(h, false);
        return;
    }
    addLog(&h, "Cycle " + std::to_string(cycle) + " : " + std::to_string(valid) + " tâche(s) déléguée(s).", true);
    dispatch(h);
}

void Orchestrator::dispatch(OrchestratorHost& h) {
    int active = 0;
    for (auto& t : tasks)
        if (t.sent && !t.done) ++active;
    for (auto& t : tasks) {
        if (!canDispatch()) return;
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
        // Modèle / effort : demande du cerveau, sinon défauts, dans les limites autorisées.
        std::string wantModel = !t.model.empty() ? t.model : settings.defaultWorkerModel;
        std::string wantEffort = !t.effort.empty() ? t.effort : settings.defaultWorkerEffort;
        std::string model = clampModel(wantModel, usableModels(settings.allowedModels, blockedModels));
        std::string effort = clampEffort(wantEffort, settings.maxEffort);
        // Conversation trop chargée, froide, ou qui changerait de modèle : on repart du récap.
        if (std::string why = rotationReasonFor(st, id, h.now(), model, effort); !why.empty()) {
            std::string nid = h.rotate(t.agent, id);
            if (nid.empty()) continue;  // récap en cours : la tâche partira au prochain passage
            addLog(&h, "↻ " + t.agent + " : contexte allégé (récap + nouvelle conversation) — " + why + ".");
            agentConv[t.agent] = id = nid;
            st = h.status(id);
        }
        if (!t.model.empty() && model != t.model)
            addLog(&h, t.agent + " : modèle « " + t.model + " » non autorisé, remplacé par « " + model + " ».");
        t.model = model;
        t.effort = effort;
        t.convId = id;
        t.turnsAtSend = st.turns;
        t.costAtSend = st.costUsd;
        t.callsAtSend = st.apiCalls;
        t.readAtSend = st.cacheReadTokens;
        t.writeAtSend = st.cacheWriteTokens;
        t.outAtSend = st.outputTokens;
        const bool browser = isBrowserAgent(t.agent);
        std::string msg = taskMessage(t, settings.visualChecks, browser, projectKindFromId(settings.projectKind));
        // Tâche déjà envoyée une fois (limite d'usage atteinte en plein travail, processus arrêté…) : l'agent
        // reprend là où il en était au lieu de tout refaire.
        if (t.sentAt > 0)
            msg = "_(Reprise après interruption : cette tâche a déjà été commencée. Vérifie ce qui est fait dans le " +
                  std::string(projectKindFromId(settings.projectKind) == ProjectKind::Software ? "dépôt" : "dossier") +
                  " et poursuis sans refaire.)_\n\n" + msg;
        if (!h.send(id, msg,
                    SendOptions{settings.workerMode, model, effort, settings.leanAgents, browser})) {
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

void Orchestrator::collectTasks(OrchestratorHost& h) {
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
        // Mesures en direct (affichées sur la carte de l'agent, transmises au cerveau)
        if (st.apiCalls >= t.callsAtSend) {
            t.calls = st.apiCalls - t.callsAtSend;
            t.cacheRead = std::max<int64_t>(0, st.cacheReadTokens - t.readAtSend);
            t.cacheWrite = std::max<int64_t>(0, st.cacheWriteTokens - t.writeAtSend);
            t.output = std::max<int64_t>(0, st.outputTokens - t.outAtSend);
            t.contextEnd = st.contextTokens;
        }
        if (st.busy) {
            // Garde-fous : une tâche qui s'emballe (boucle d'essais, contexte qui enfle) est interrompue.
            if (t.guardNote.empty()) {
                if (settings.maxCallsPerTask > 0 && t.calls >= settings.maxCallsPerTask)
                    t.guardNote = std::to_string(t.calls) + " appels API pour cette seule tâche (limite " +
                                  std::to_string(settings.maxCallsPerTask) + ")";
                else if (settings.taskContextLimit > 0 && st.contextTokens >= settings.taskContextLimit)
                    t.guardNote = "contexte de " + kTokens(st.contextTokens) + " (limite " + kTokens(settings.taskContextLimit) + ")";
                if (!t.guardNote.empty()) {
                    h.interrupt(t.convId);
                    addLog(&h, "⛔ " + t.agent + " interrompu : " + t.guardNote + ".", true);
                    changed = true;
                }
            }
            continue;
        }
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
}

// Compte rendu intermédiaire utile : quelque chose de neuf à rapporter, rien en file (une place libre serait
// aussitôt prise), une place libre et au moins un agent libre — sinon le cerveau ne pourrait rien lancer.
bool Orchestrator::partialReportUseful(OrchestratorHost& h) const {
    if (!settings.rollingReports || finalRequested_ || exhausted()) return false;
    if (settings.maxCycles > 0 && cycle >= settings.maxCycles) return false;
    bool fresh = false;
    int running = 0;
    std::vector<std::string> busyAgents;
    for (auto& t : tasks) {
        if (t.done && !t.reported) fresh = true;
        if (!t.done && !t.sent) return false;  // en file
        if (!t.done && t.sent) {
            ++running;
            if (std::find(busyAgents.begin(), busyAgents.end(), t.agent) == busyAgents.end()) busyAgents.push_back(t.agent);
        }
    }
    if (!fresh || running == 0 || running >= std::max(1, settings.maxParallel)) return false;
    return busyAgents.size() < h.agents().size();
}

bool Orchestrator::sendReport(OrchestratorHost& h, bool partial) {
    // La rotation éventuelle du cerveau (trop chargé, froid) est gérée par sendBrain.
    std::string report = buildReport(partial, h.now());
    std::string done;
    for (auto& t : tasks)
        if (t.done && !t.reported) done += (done.empty() ? "" : ", ") + t.agent;
    markReported();
    if (partial)
        addLog(&h, "Compte rendu intermédiaire (" + done + ") envoyé à l'orchestrateur ; " +
                       std::to_string(tasksInFlight()) + " agent(s) toujours au travail.");
    else
        addLog(&h, "Comptes rendus du cycle " + std::to_string(cycle) + " envoyés à l'orchestrateur.");
    return sendBrain(h, report);
}

void Orchestrator::tickWorking(OrchestratorHost& h) {
    if (state != OrchState::Working) return;
    if (h.status(brainConvId).busy) return;  // l'utilisateur discute avec l'orchestrateur
    if (brainChatReply(h)) return;
    if (tasksInFlight() > 0) {
        // Au fil de l'eau : un agent a fini, d'autres travaillent encore — le cerveau peut relancer les agents libres.
        if (partialReportUseful(h) && usageOk(h)) sendReport(h, true);
        return;
    }
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
    sendReport(h, false);
}

std::string Orchestrator::rotationReasonFor(const ParticipantStatus& st, const std::string& convId, int64_t now,
                                            const std::string& nextModel, const std::string& nextEffort) const {
    if (convId.empty()) return {};
    bool brain = convId == brainConvId;
    auto it = convTasks_.find(convId);
    int n = it == convTasks_.end() ? 0 : it->second;
    return rotationReason(settings, st, n, brain, now, cacheTtlSeconds(), nextModel, nextEffort);
}

void Orchestrator::conversationReplaced(const std::string& oldId, const std::string& newId) {
    if (oldId.empty() || newId.empty() || oldId == newId) return;
    if (brainConvId == oldId) {
        brainConvId = newId;
        brainSeenTurns_ = -1;
        brainRefresh_ = true;
        brainRotating_ = false;
    }
    for (auto& [agent, id] : agentConv)
        if (id == oldId) id = newId;
    convTasks_.erase(oldId);
    changed = true;
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
                      {"report", t.report}, {"reported", t.reported}, {"sentAt", t.sentAt}, {"doneAt", t.doneAt},
                      {"callsAtSend", t.callsAtSend}, {"readAtSend", t.readAtSend}, {"writeAtSend", t.writeAtSend},
                      {"outAtSend", t.outAtSend}, {"calls", t.calls}, {"cacheRead", t.cacheRead},
                      {"cacheWrite", t.cacheWrite}, {"output", t.output}, {"contextEnd", t.contextEnd},
                      {"guardNote", t.guardNote}});
    json jl = json::array();
    for (auto& l : log) jl.push_back({{"t", l.time}, {"text", l.text}, {"important", l.important}});
    json jq = json::array();
    for (auto& q : questions)
        jq.push_back({{"title", q.title}, {"text", q.text}, {"options", q.options}, {"suggestions", q.suggestions}});
    json ja = json::array();
    for (auto& a : pendingAgents) ja.push_back({{"name", a.name}, {"notes", a.notes}});
    json jh = json::array();
    for (auto& q : qaHistory) jh.push_back({{"t", q.time}, {"q", q.question}, {"a", q.answer}});
    const auto& s = settings;
    return {{"version", 2},
            {"settings",
             {{"objective", s.objective},
              {"maxCycles", s.maxCycles},
              {"maxParallel", s.maxParallel},
              {"rollingReports", s.rollingReports},
              {"allowNewAgents", s.allowNewAgents},
              {"maxAgents", s.maxAgents},
              {"visualChecks", s.visualChecks}, {"projectKind", s.projectKind},
              {"browserAgents", s.browserAgents},
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
              {"recapOnPhaseEnd", s.recapOnPhaseEnd},
              {"rotateAtTokens", s.rotateAtTokens},
              {"rotateAfterTasks", s.rotateAfterTasks},
              {"rotateWhenCold", s.rotateWhenCold},
              {"coldRotateTokens", s.coldRotateTokens},
              {"cacheTtlMinutes", s.cacheTtlMinutes},
              {"rotateOnSwitch", s.rotateOnSwitch},
              {"switchRotateTokens", s.switchRotateTokens},
              {"brainReadOnly", s.brainReadOnly},
              {"leanAgents", s.leanAgents},
              {"reportMaxChars", s.reportMaxChars},
              {"maxCallsPerTask", s.maxCallsPerTask},
              {"taskContextLimit", s.taskContextLimit},
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
            {"brainRefresh", brainRefresh_},
            {"agentConv", agentConv},
            {"tasks", jt},
            {"log", jl},
            {"finalSummary", finalSummary},
            {"humanQuestion", humanQuestion},
            {"questions", jq},
            {"questionSeq", questionSeq},
            {"qaHistory", jh},
            {"pendingAgents", ja},
            {"teamNotes", teamNotes_},
            {"brainTurnsAtSend", brainTurnsAtSend_},
            {"brainSeenTurns", brainSeenTurns_},
            {"brainCostAtSend", brainCostAtSend_},
            {"lastBrainMessage", lastBrainMessage_},
            {"nudges", nudges_},
            {"finalRequested", finalRequested_},
            {"usageWindow", usageWindow_},
            {"pauseNote", pauseNote},
            {"pauseUntil", pauseUntil}};
}

void Orchestrator::fromJson(const json& j) {
    json s = j.value("settings", json::object());
    auto& st = settings;
    st.objective = s.value("objective", std::string());
    st.maxCycles = s.value("maxCycles", st.maxCycles);
    st.maxParallel = s.value("maxParallel", st.maxParallel);
    st.rollingReports = s.value("rollingReports", st.rollingReports);
    st.allowNewAgents = s.value("allowNewAgents", st.allowNewAgents);
    st.maxAgents = std::max(0, s.value("maxAgents", st.maxAgents));
    st.visualChecks = s.value("visualChecks", st.visualChecks);
    st.projectKind = s.value("projectKind", st.projectKind);
    st.browserAgents.clear();
    if (s.contains("browserAgents") && s["browserAgents"].is_array())
        for (auto& a : s["browserAgents"])
            if (a.is_string()) st.browserAgents.push_back(a.get<std::string>());
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
    st.recapOnPhaseEnd = s.value("recapOnPhaseEnd", st.recapOnPhaseEnd);
    // Ancien réglage « rotateAtContext » (part d'une fenêtre de 1 M, soit ~450 k) : ignoré, on passe au seuil en jetons.
    st.rotateAtTokens = s.value("rotateAtTokens", st.rotateAtTokens);
    st.rotateAfterTasks = s.value("rotateAfterTasks", st.rotateAfterTasks);
    st.rotateWhenCold = s.value("rotateWhenCold", st.rotateWhenCold);
    st.coldRotateTokens = s.value("coldRotateTokens", st.coldRotateTokens);
    st.cacheTtlMinutes = s.value("cacheTtlMinutes", st.cacheTtlMinutes);
    st.rotateOnSwitch = s.value("rotateOnSwitch", st.rotateOnSwitch);
    st.switchRotateTokens = s.value("switchRotateTokens", st.switchRotateTokens);
    st.brainReadOnly = s.value("brainReadOnly", st.brainReadOnly);
    st.leanAgents = s.value("leanAgents", st.leanAgents);
    st.reportMaxChars = s.value("reportMaxChars", st.reportMaxChars);
    st.maxCallsPerTask = s.value("maxCallsPerTask", st.maxCallsPerTask);
    st.taskContextLimit = s.value("taskContextLimit", st.taskContextLimit);
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
    brainRefresh_ = j.value("brainRefresh", false);
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
        // Ancien format (cycles barrières) : hors phase « agents au travail », les tâches terminées étaient rapportées.
        t.reported = jt.value("reported", t.done && j.value("state", std::string()) != "working");
        t.sentAt = jt.value("sentAt", (int64_t)0);
        t.doneAt = jt.value("doneAt", (int64_t)0);
        t.callsAtSend = jt.value("callsAtSend", 0);
        t.readAtSend = jt.value("readAtSend", (int64_t)0);
        t.writeAtSend = jt.value("writeAtSend", (int64_t)0);
        t.outAtSend = jt.value("outAtSend", (int64_t)0);
        t.calls = jt.value("calls", 0);
        t.cacheRead = jt.value("cacheRead", (int64_t)0);
        t.cacheWrite = jt.value("cacheWrite", (int64_t)0);
        t.output = jt.value("output", (int64_t)0);
        t.contextEnd = jt.value("contextEnd", (int64_t)0);
        t.guardNote = jt.value("guardNote", std::string());
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
    pendingAgents.clear();
    for (auto& ja : j.value("pendingAgents", json::array()))
        if (ja.is_object()) pendingAgents.push_back({ja.value("name", std::string()), ja.value("notes", std::string())});
    teamNotes_.clear();
    for (auto& n : j.value("teamNotes", json::array()))
        if (n.is_string()) teamNotes_.push_back(n.get<std::string>());
    qaHistory.clear();
    for (auto& jh : j.value("qaHistory", json::array()))
        qaHistory.push_back({jh.value("t", (int64_t)0), jh.value("q", std::string()), jh.value("a", std::string())});
    brainTurnsAtSend_ = j.value("brainTurnsAtSend", 0);
    brainSeenTurns_ = j.value("brainSeenTurns", -1);
    brainCostAtSend_ = j.value("brainCostAtSend", 0.0);
    lastBrainMessage_ = j.value("lastBrainMessage", std::string());
    nudges_ = j.value("nudges", 0);
    finalRequested_ = j.value("finalRequested", false);
    usageWindow_ = j.value("usageWindow", std::string());
    pauseNote = j.value("pauseNote", std::string());
    pauseUntil = j.value("pauseUntil", (int64_t)0);
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
    if (state == OrchState::WaitingHuman && !humanQuestion.empty()) {
        Directives d = parseDirectives("@@HUMAIN\n" + humanQuestion + "\n@@FIN");
        questions = d.questions;
        if (d.pause && !d.needHuman) {
            // Annonce de pause enregistrée comme une question (versions précédentes) : c'est une pause de
            // l'orchestrateur, reprise à la fin de la session de 5 h où elle a été décidée.
            pauseNote = d.pauseNote;
            pauseUntil = windowResetsAt_;
            humanQuestion.clear();
            questions.clear();
            resumeState_ = OrchState::Thinking;
            state = OrchState::UsagePause;
            usageWindow_ = "brain";
            pauseReason = "l'orchestrateur s'est mis en pause : reprise automatique à la prochaine session de 5 h (ou "
                          "cliquez sur Reprendre)";
        }
    }
    // Propositions d'agents en attente : leurs questions sont reconstruites (après celles du cerveau).
    if (state == OrchState::WaitingHuman && !pendingAgents.empty()) {
        if (humanQuestion.empty()) questions.clear();
        for (auto& a : pendingAgents) questions.push_back(agentQuestion(a));
    }
    // Rechargée en cours de route (ClaudeDesk fermé) : les processus ont disparu,
    // on attend une reprise explicite. Une question en attente reste une question.
    reviveOnTick_ = false;
    if (state == OrchState::WaitingHuman)
        for (auto& t : tasks)
            if (t.sent && !t.done) reviveOnTick_ = true;  // processus disparus : rouvrir / renvoyer au prochain passage
    if (state == OrchState::Thinking || state == OrchState::Working) {
        resumeState_ = state;
        state = OrchState::Paused;
        pauseReason = "ClaudeDesk a été fermé pendant l'orchestration — cliquez sur Reprendre";
    } else if (state == OrchState::UsagePause && usageWindow_ != "brain") {  // pause de l'orchestrateur : reste automatique
        state = OrchState::Paused;
        pauseReason = "pause d'usage en cours à la fermeture — cliquez sur Reprendre";
    }
    changed = false;
}

}  // namespace claude
