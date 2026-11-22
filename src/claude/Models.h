#pragma once
// Les modèles de Claude, présentés par famille (les alias de Claude Code : haiku, sonnet, opus, fable), et leur
// accès selon l'abonnement. La CLI liste toutes les familles pour tous les comptes, sans dire lesquelles le compte
// peut utiliser : ClaudeDesk combine ce qui est connu (vérifié : avec Pro, Fable demande des crédits d'usage achetés
// à part — « You're out of usage credits ») et ce qu'il observe (refus ou réponses reçues).
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace claude {

struct ModelFamily {
    const char* alias;    // alias Claude Code
    const char* name;     // « Opus »
    const char* version;  // version actuelle de l'alias, « 5.5 »
    const char* fullId;   // identifiant figé de cette version
    const char* tagline;  // une ligne : à quoi il sert
    const char* uses;     // exemples d'usages
    int power;            // capacité, 1 à 4
    int cost;             // consommation de quota, 1 à 4
};
const std::vector<ModelFamily>& modelFamilies();  // du plus léger au plus capable
// Famille d'un modèle (alias ou identifiant complet, « claude-opus-5 »…) ; nullptr si inconnu ou vide (défaut).
const ModelFamily* findModelFamily(const std::string& model);

enum class ModelAccess {
    Unknown,       // pas d'information pour cet abonnement
    Included,      // inclus dans l'abonnement
    NeedsCredits,  // hors abonnement : consomme des crédits d'usage achetés à part
    Unavailable,   // refusé (« model_not_found » : inexistant ou sans accès)
};
const char* modelAccessLabel(ModelAccess a);
// Accès attendu d'après le type d'abonnement (« pro », « max »… de `claude auth status`).
ModelAccess expectedModelAccess(const std::string& familyAlias, const std::string& plan);
// Refus lié au modèle dans une réponse : champ « error » du message synthétique de la CLI + son texte.
// Unknown si l'erreur n'a rien à voir avec le modèle.
ModelAccess modelAccessFromError(const std::string& error, const std::string& text);
// Explication en français d'un refus, pour la conversation.
std::string modelRefusalText(const std::string& model, ModelAccess a);
bool isModelRefusalText(const std::string& text);  // note produite par modelRefusalText
// Nom lisible : « Opus 5.5 » pour « opus » ou « claude-opus-5-5 », « Par défaut » pour "".
std::string modelDisplayName(const std::string& model);

struct ModelAccessInfo {
    ModelAccess access = ModelAccess::Unknown;
    std::string plan;  // abonnement au moment de l'observation
    int64_t when = 0;
};

// Observations par famille, enregistrées dans settings.json. Une observation ne vaut que pour l'abonnement où elle
// a été faite (un passage de Pro à Max la rend caduque).
class ModelAccessBook {
public:
    std::map<std::string, ModelAccessInfo> seen;  // alias de famille → observation

    ModelAccess access(const std::string& familyAlias, const std::string& plan) const;
    bool observed(const std::string& familyAlias, const std::string& plan) const;
    // Le modèle peut-il servir sans frais supplémentaires ni refus ? (vrai si inconnu : on laisse essayer)
    bool usable(const std::string& model, const std::string& plan) const;
    void record(const std::string& familyAlias, const std::string& plan, ModelAccess a, int64_t now);
    // Réponse reçue d'un modèle : « inclus » s'il n'y avait pas mieux à savoir.
    void recordSuccess(const std::string& model, const std::string& plan, int64_t now);
    // Familles à ne pas proposer à l'orchestrateur (alias).
    std::vector<std::string> blocked(const std::string& plan) const;

    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
};

}  // namespace claude
