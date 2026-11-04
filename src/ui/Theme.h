#pragma once
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

void applyTheme(ThemeId id, float scale);
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
// Barre de progression colorée selon le seuil.
// `animId` non nul : le remplissage glisse en douceur vers la valeur.
void usageBar(float fraction, const char* overlay, float height = 0, float width = -1, const char* animId = nullptr);

}  // namespace ui
