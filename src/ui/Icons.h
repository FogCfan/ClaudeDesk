#pragma once
// Petites icônes vectorielles (dessinées à la main, indépendantes de la police) pour distinguer d'un coup d'œil,
// comme dans l'explorateur de VS Code : projet, thème (dossier), agent, orchestrateur, conversation, fichiers.
#include <string>

#include <imgui.h>

#include "claude/ProjectKind.h"

namespace ui {

enum class Icon {
    Project,       // projet (dossier orange)
    Folder,        // thème / dossier fermé
    FolderOpen,    // thème / dossier ouvert
    Agent,         // thème d'un projet orchestré = agent
    Orchestrator,  // conversation « Orchestrateur » (le cerveau)
    Chat,          // conversation
    WorkDir,       // dossier de travail du projet (le code)
    File,          // fichier quelconque
    FileCode,      // code source
    FileText,      // texte, Markdown
    FileData,      // JSON, YAML, TOML, CSV, config
    FileImage,     // image
    FilePdf,       // PDF / document
    Trash,
    History,       // sessions précédentes d'une discussion (horloge + flèche de retour)
    Send,          // envoyer (avion en papier)
    Stop,          // arrêter (carré)
    // Types de projets (Designer, accueil, création de projet)
    KindAuto,      // étincelle : Claude choisit
    KindSoftware,  // fenêtre de code
    KindConcept,   // ampoule
    KindData,      // graphique en barres
    KindDocument,  // rapport
    KindHybrid,    // ampoule + code
    KindQuestion,  // bulle « ? »
};

// Couleur propre à l'icône (variante légèrement adaptée au thème clair / sombre).
ImVec4 iconColor(Icon icon);
// Icône d'un type de projet.
Icon projectKindIcon(claude::ProjectKind k);
// Icône d'un fichier d'après son extension.
Icon fileIcon(const std::string& fileName);
// Dessine l'icône dans le carré [pos, pos + size]. `col` = 0 : couleur propre à l'icône.
// `activity` > 0 : petit point animé (conversation ou agent au travail).
void drawIcon(ImDrawList* dl, Icon icon, ImVec2 pos, float size, ImU32 col = 0, float activity = 0);
// Icône posée comme un élément ImGui (taille de la police), à enchaîner avec SameLine().
void icon(Icon icon, ImU32 col = 0, float activity = 0);
// Nœud d'arbre précédé d'une icône (même signature que TreeNodeEx). Le libellé est décalé pour lui laisser la place.
bool iconTreeNode(const char* strId, ImGuiTreeNodeFlags flags, Icon icon, const char* label, ImU32 col = 0,
                  float activity = 0);

}  // namespace ui
