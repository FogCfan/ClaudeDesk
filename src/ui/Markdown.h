#pragma once
// Rendu Markdown minimal pour ImGui : titres, listes, citations, blocs de code
// (avec bouton Copier), tableaux, et dans le texte : **gras**, *italique*,
// `code`, [liens](url). Le texte est découpé mot à mot pour le retour à la ligne.
#include <string>
#include <string_view>
#include <vector>

struct ImFont;

namespace ui {

struct MdBlock {
    enum Type { Paragraph, Heading, Bullet, Numbered, Code, Quote, Rule, Table } type = Paragraph;
    int level = 0;        // titre : 1..6 ; liste : niveau d'indentation
    std::string text;
    std::string lang;     // bloc de code
    std::string number;   // liste numérotée
};

std::vector<MdBlock> parseMarkdown(std::string_view src);

struct MdStyle {
    ImFont* body = nullptr;
    ImFont* bold = nullptr;
    ImFont* mono = nullptr;
    bool wrapCode = false;
    ImFont* italic = nullptr;
};

// Rend une liste de blocs. `idSeed` distingue les boutons de copie entre messages.
void renderMarkdown(const std::vector<MdBlock>& blocks, const MdStyle& style, int idSeed);
// Rend une ligne avec mise en forme en ligne, retour à la ligne à `wrapRight` (x écran).
void renderRichText(std::string_view text, const MdStyle& style, float wrapRight);

// Bloc de texte sélectionnable (InputTextMultiline en lecture seule, hauteur auto).
void selectableText(const char* id, const std::string& text, float width, ImFont* font = nullptr);

}  // namespace ui
