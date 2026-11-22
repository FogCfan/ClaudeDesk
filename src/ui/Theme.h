#pragma once
#include <string>
#include <imgui.h>

namespace ui {

struct Palette {
    ImVec4 accent;       // couleur principale (orange Claude)
    ImVec4 accentSoft;
    ImVec4 userBg;       // fond des messages utilisateur
    ImVec4 cardBg;       // fond des cartes (outils, permissions)
    ImVec4 codeBg;
    ImVec4 codeText;
    ImVec4 dim;          // texte secondaire
    ImVec4 ok, warn, err;
    ImVec4 link;
    ImVec4 diffAdd, diffDel;
};

enum class ThemeId { Dark = 0, Light = 1, Dusk = 2 };

// `compact` : marges et espacements réduits (densité « compacte »).
void applyTheme(ThemeId id, float scale, bool compact = false);
const Palette& palette();

inline ImU32 col(const ImVec4& c, float alphaMul = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul));
}

// Petit indicateur d'activité animé (trois points).
void spinner(float t, const ImVec4& color);
// Étiquette arrondie colorée (statut, modèle…).
void badge(const char* text, const ImVec4& color);
// Bouton d'aide « (?) » avec infobulle.
void helpMarker(const char* text);
// --- Mise en page qui s'adapte aux panneaux étroits ---
// Largeur d'un bouton portant ce libellé (texte visible + marges du cadre).
float buttonWidth(const char* label);
// À appeler avant un élément de largeur `w` : il reste sur la ligne courante s'il y tient, sinon il passe dessous.
void sameLineIfFits(float w);
// Largeur d'un champ à étiquette : `desired`, réduite pour que champ + étiquette tiennent dans la place restante.
float fitItemWidth(float desired, const char* label = nullptr);
// Note grisée à la suite de l'élément précédent, avec retour à la ligne (ou dessous si la place manque).
void sideNote(const char* text);
// Barre de progression colorée selon le seuil.
// `animId` non nul : le remplissage glisse en douceur vers la valeur.
// Style épuré : barre fine, valeur écrite À CÔTÉ (jamais par-dessus le remplissage).
void usageBar(float fraction, const char* overlay, float height = 0, float width = -1, const char* animId = nullptr);
// Couleur d'un niveau d'usage : vert → orange → rouge.
ImVec4 levelColor(float fraction);
// Jauge circulaire : anneau coloré selon le niveau, gros chiffre au centre (`bigFont` : police grasse, ou nullptr),
// petite légende dessous (facultative). Occupe un carré de `diameter`.
void ringGauge(float fraction, float diameter, const char* bigText, const char* caption = nullptr, ImFont* bigFont = nullptr,
               const char* animId = nullptr);
// Mini anneau (barre de menu, listes) aligné sur la ligne de texte.
void miniRing(float fraction, float size = 0, const char* animId = nullptr);
// Puce plate, à la manière de la barre d'état de VS Code : texte seul, cadre au survol. Renvoie true au clic.
bool chip(const char* label, const ImVec4* color = nullptr);
// Case à cocher dont le libellé passe à la ligne au lieu d'être coupé (panneaux étroits). `help` : infobulle.
bool checkboxWrapped(const char* label, bool* v, const char* help = nullptr);
// Rangée de boutons exclusifs (« Sombre | Clair | Crépuscule ») ; passe à la ligne si la place manque.
bool segmented(const char* id, int* value, const char* const* labels, int count);
// Chemin raccourci pour l'affichage (« …\Documents\ClaudeDesk ») : les `keep` derniers éléments.
std::string shortPath(const std::string& path, int keep = 2);
// Ligne de mesure : libellé à gauche, valeur à droite (en gras), barre fine dessous, note grisée facultative.
void meterRow(const char* label, float fraction, const char* value, const char* note = nullptr, ImFont* valueFont = nullptr,
              const char* animId = nullptr);

}  // namespace ui
