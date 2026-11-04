#pragma once
// Petit système d'animation pour ImGui (mode immédiat) : des valeurs qui
// convergent en douceur vers une cible, indépendamment de la fréquence d'images.
#include <imgui.h>

namespace ui::anim {

// À appeler en début d'image, pour chaque contexte ImGui.
// `primary` : seule la fenêtre principale fait avancer l'horloge et l'état « occupé ».
void newFrame(bool enabled, float speed, bool primary = true);
bool enabled();
float dt();          // durée de l'image (s), multipliée par la vitesse des animations
double time();       // horloge des animations (s)

// Approche exponentielle de `target` (rate ~ 1/s : 12 = rapide, 6 = doux).
// La première valeur vaut `initial` (ou `target` si initial < 0).
float smooth(ImGuiID id, float target, float rate = 12.0f, float initial = -1.0f);
// Variante pour une position (défilement fluide…).
ImVec2 smooth2(ImGuiID id, ImVec2 target, float rate = 12.0f);
// Force une valeur (ex. saut immédiat).
void set(ImGuiID id, float value);

// Vrai si au moins une animation n'a pas fini de converger lors de l'image
// précédente : la boucle principale continue alors de dessiner.
bool busy();
void markBusy();

float easeOutCubic(float t);
float smoothstep(float t);
// Pulsation 0..1 (sinus), `hz` battements par seconde.
float pulse(float hz, float phase = 0.0f);

}  // namespace ui::anim
