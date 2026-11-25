#include "claude/Models.h"

#include "util/Strings.h"

namespace claude {

using json = nlohmann::json;

const std::vector<ModelFamily>& modelFamilies() {
    static const std::vector<ModelFamily> v = {
        {"haiku", "Haiku", "5.5", "claude-haiku-5-5", "Rapide et économe",
         "questions simples, résumés, tri, petites corrections", 1, 1},
        {"sonnet", "Sonnet", "5.5", "claude-sonnet-5-5", "L'équilibre au quotidien",
         "la plupart des tâches : rédaction, analyse, code courant", 2, 2},
        {"opus", "Opus", "5.5", "claude-opus-5-5", "Le plus capable de l'abonnement",
         "travail complexe : architecture, raisonnement long, gros documents", 3, 3},
        {"fable", "Fable", "5.2", "claude-fable-5-2", "Pour les défis les plus durs",
         "problèmes très difficiles et tâches très longues", 4, 4},
    };
    return v;
}

const ModelFamily* findModelFamily(const std::string& model) {
    std::string m = util::toLower(model);
    if (m.empty() || m == "default") return nullptr;
    for (auto& f : modelFamilies())
        if (m.find(f.alias) != std::string::npos) return &f;
    return nullptr;
}

const char* modelAccessLabel(ModelAccess a) {
    switch (a) {
        case ModelAccess::Included: return "Inclus dans l'abonnement";
        case ModelAccess::NeedsCredits: return "Crédits d'usage requis";
        case ModelAccess::Unavailable: return "Indisponible pour ce compte";
        case ModelAccess::Unknown: break;
    }
    return "Accès non vérifié";
}

ModelAccess expectedModelAccess(const std::string& familyAlias, const std::string& plan) {
    std::string p = util::toLower(plan);
    bool fable = familyAlias == "fable";
    if (p.find("pro") != std::string::npos)  // « pro », « Claude Pro » (vérifié le 22/11/2026)
        return fable ? ModelAccess::NeedsCredits : ModelAccess::Included;
    if (p.find("max") != std::string::npos || p.find("team") != std::string::npos ||
        p.find("enterprise") != std::string::npos)
        return fable ? ModelAccess::Unknown : ModelAccess::Included;
    return ModelAccess::Unknown;
}

bool isUsageLimitText(const std::string& text) {
    std::string t = util::toLower(text);
    // « You've hit your 5-hour limit · resets 8:30am (Europe/Paris) · out of usage credits » (vérifié le 25/11/2026)
    return t.find("hit your") != std::string::npos || t.find("usage limit") != std::string::npos ||
           t.find("limit reached") != std::string::npos || t.find("limit · resets") != std::string::npos;
}

std::string usageLimitWindow(const std::string& text) {
    std::string t = util::toLower(text);
    return t.find("week") != std::string::npos ? "seven_day" : "five_hour";
}

ModelAccess modelAccessFromError(const std::string& error, const std::string& text) {
    std::string t = util::toLower(text);
    // La limite de la session (5 h, semaine) dit aussi « out of usage credits » : ce n'est pas le modèle qui est refusé.
    if (isUsageLimitText(text)) return ModelAccess::Unknown;
    if (error == "model_not_found" || t.find("issue with the selected model") != std::string::npos)
        return ModelAccess::Unavailable;
    if (t.find("out of usage credits") != std::string::npos || t.find("usage credits") != std::string::npos)
        return ModelAccess::NeedsCredits;
    return ModelAccess::Unknown;
}

std::string modelDisplayName(const std::string& model) {
    if (model.empty() || model == "default") return "Par défaut";
    const ModelFamily* f = findModelFamily(model);
    if (!f) return model;
    std::string m = util::toLower(model);
    if (m == f->alias || m == f->fullId) return std::string(f->name) + " " + f->version;
    return model;  // version figée plus ancienne : l'identifiant parle de lui-même
}

std::string modelRefusalText(const std::string& model, ModelAccess a) {
    std::string name = modelDisplayName(model);
    if (a == ModelAccess::NeedsCredits)
        return "« " + name + " » n'est pas inclus dans votre abonnement : ce modèle consomme des crédits d'usage achetés "
               "à part (claude.ai › Réglages › Usage). Choisissez un autre modèle (Opus, Sonnet ou Haiku), puis renvoyez "
               "votre message.";
    return "Le modèle « " + name + " » n'existe pas ou n'est pas accessible avec ce compte. Choisissez un autre modèle, "
           "puis renvoyez votre message.";
}

bool isModelRefusalText(const std::string& text) {
    return text.find("n'est pas inclus dans votre abonnement : ce modèle consomme des crédits") != std::string::npos ||
           text.find("n'existe pas ou n'est pas accessible avec ce compte") != std::string::npos;
}

ModelAccess ModelAccessBook::access(const std::string& familyAlias, const std::string& plan) const {
    auto it = seen.find(familyAlias);
    if (it != seen.end() && it->second.access != ModelAccess::Unknown && util::toLower(it->second.plan) == util::toLower(plan))
        return it->second.access;
    return expectedModelAccess(familyAlias, plan);
}

bool ModelAccessBook::observed(const std::string& familyAlias, const std::string& plan) const {
    auto it = seen.find(familyAlias);
    return it != seen.end() && it->second.access != ModelAccess::Unknown &&
           util::toLower(it->second.plan) == util::toLower(plan);
}

bool ModelAccessBook::usable(const std::string& model, const std::string& plan) const {
    const ModelFamily* f = findModelFamily(model);
    if (!f) return true;
    ModelAccess a = access(f->alias, plan);
    return a == ModelAccess::Included || a == ModelAccess::Unknown;
}

void ModelAccessBook::record(const std::string& familyAlias, const std::string& plan, ModelAccess a, int64_t now) {
    if (familyAlias.empty() || a == ModelAccess::Unknown) return;
    // Un modèle inclus dans l'abonnement ne « demande » pas de crédits : c'est la limite d'usage qui parle.
    if (a == ModelAccess::NeedsCredits && expectedModelAccess(familyAlias, plan) == ModelAccess::Included) return;
    seen[familyAlias] = {a, plan, now};
}

void ModelAccessBook::recordSuccess(const std::string& model, const std::string& plan, int64_t now) {
    const ModelFamily* f = findModelFamily(model);
    if (!f) return;
    // Hors abonnement mais des crédits ont été achetés : ça répond, et ça reste payant. On garde l'information.
    if (access(f->alias, plan) == ModelAccess::NeedsCredits) return;
    if (!observed(f->alias, plan) || seen[f->alias].access == ModelAccess::Unavailable)
        record(f->alias, plan, ModelAccess::Included, now);
}

std::vector<std::string> ModelAccessBook::blocked(const std::string& plan) const {
    std::vector<std::string> v;
    for (auto& f : modelFamilies())
        if (!usable(f.alias, plan)) v.push_back(f.alias);
    return v;
}

json ModelAccessBook::toJson() const {
    json j = json::object();
    for (auto& [k, v] : seen) j[k] = {{"access", (int)v.access}, {"plan", v.plan}, {"when", v.when}};
    return j;
}

void ModelAccessBook::fromJson(const json& j) {
    seen.clear();
    if (!j.is_object()) return;
    for (auto& [k, v] : j.items()) {
        if (!v.is_object()) continue;
        int a = v.value("access", 0);
        if (a < 0 || a > 3) continue;
        ModelAccessInfo info{(ModelAccess)a, v.value("plan", ""), v.value("when", (int64_t)0)};
        // Observation erronée des versions précédentes : la limite de 5 h (« … out of usage credits ») était prise
        // pour un refus du modèle, et bloquait Opus/Sonnet/Haiku pourtant inclus dans l'abonnement.
        if (info.access == ModelAccess::NeedsCredits && expectedModelAccess(k, info.plan) == ModelAccess::Included) continue;
        seen[k] = info;
    }
}

}  // namespace claude
