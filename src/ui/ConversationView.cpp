// Zone centrale : onglets de conversations, fil des messages, outils.
#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "app/App.h"
#include "ui/Anim.h"
#include "ui/Theme.h"
#include "util/Paths.h"
#include "util/Strings.h"

namespace app {

using claude::Entry;
using claude::EntryKind;
using json = nlohmann::json;

namespace {

// Réduit temporairement la zone de contenu à droite (marge interne des cartes).
struct RightMargin {
    ImGuiWindow* w;
    float savedWork, savedContent;
    explicit RightMargin(float m) : w(ImGui::GetCurrentWindow()) {
        savedWork = w->WorkRect.Max.x;
        savedContent = w->ContentRegionRect.Max.x;
        w->WorkRect.Max.x -= m;
        w->ContentRegionRect.Max.x -= m;
    }
    ~RightMargin() {
        w->WorkRect.Max.x = savedWork;
        w->ContentRegionRect.Max.x = savedContent;
    }
};

// Carte avec fond arrondi dessiné derrière le contenu.
struct Card {
    ImDrawListSplitter splitter;
    ImDrawList* dl;
    ImVec2 start;
    float width;
    ImVec4 color;
    float pad;
    bool accentBar;
    Card(float w, const ImVec4& c, bool bar = false) : dl(ImGui::GetWindowDrawList()), width(w), color(c), accentBar(bar) {
        pad = ImGui::GetStyle().FramePadding.x + 2;
        splitter.Split(dl, 2);
        splitter.SetCurrentChannel(dl, 1);
        start = ImGui::GetCursorScreenPos();
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, pad * 0.5f));
        ImGui::Indent(pad);
    }
    void end() {
        ImGui::Unindent(pad);
        ImGui::Dummy(ImVec2(0, pad * 0.3f));
        ImGui::EndGroup();
        ImVec2 endP(start.x + width, ImGui::GetItemRectMax().y);
        splitter.SetCurrentChannel(dl, 0);
        dl->AddRectFilled(start, endP, ui::col(color), 8.0f);
        if (accentBar) dl->AddRectFilled(start, ImVec2(start.x + 3, endP.y), ui::col(ui::palette().accent), 8.0f, ImDrawFlags_RoundCornersLeft);
        splitter.Merge(dl);
    }
};

// Raccourcit un chemin par la gauche pour tenir dans `maxW` pixels.
std::string ellipsizeLeft(const std::string& text, float maxW) {
    if (maxW <= 20) return "…";
    if (ImGui::CalcTextSize(text.c_str()).x <= maxW) return text;
    size_t start = 0;
    while (start < text.size()) {
        ++start;
        while (start < text.size() && ((unsigned char)text[start] & 0xC0) == 0x80) ++start;
        std::string t = "…" + text.substr(start);
        if (ImGui::CalcTextSize(t.c_str()).x <= maxW) return t;
    }
    return "…";
}

std::string clockTime(int64_t t) {
    if (!t) return {};
    std::string iso = util::isoFromUnix(t);
    return iso.size() >= 16 ? iso.substr(11, 5) : iso;
}

// Affiche un texte monospace limité à `maxLines` lignes.
void monoText(ImFont* mono, const std::string& text, int maxLines, bool* showAll, const ImVec4* color = nullptr) {
    ImGui::PushFont(mono, ImGui::GetStyle().FontSizeBase * 0.84f);
    if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
    size_t cut = std::string::npos;
    int lines = 0;
    if (!showAll || !*showAll) {
        for (size_t i = 0; i < text.size(); ++i)
            if (text[i] == '\n' && ++lines >= maxLines) {
                cut = i;
                break;
            }
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str(), cut == std::string::npos ? nullptr : text.c_str() + cut);
    ImGui::PopTextWrapPos();
    if (color) ImGui::PopStyleColor();
    ImGui::PopFont();
    if (cut != std::string::npos && showAll) {
        size_t total = std::count(text.begin(), text.end(), '\n') + 1;
        if (ImGui::SmallButton(("Tout afficher (" + std::to_string(total) + " lignes)").c_str())) *showAll = true;
    }
}

void diffView(ImFont* mono, const std::string& oldS, const std::string& newS) {
    const auto& pal = ui::palette();
    ImGui::PushFont(mono, ImGui::GetStyle().FontSizeBase * 0.84f);
    ImGui::PushTextWrapPos(0.0f);
    int n = 0;
    for (auto& l : util::splitLines(oldS)) {
        if (++n > 40) break;
        ImGui::TextColored(pal.diffDel, "- %s", l.c_str());
    }
    n = 0;
    for (auto& l : util::splitLines(newS)) {
        if (++n > 40) break;
        ImGui::TextColored(pal.diffAdd, "+ %s", l.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
}

}  // namespace

// ---------------------------------------------------------------------------

void App::drawConversationArea() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 4));
    ImGui::Begin("Conversations", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    if (open_.empty()) {
        drawWelcome();
        ImGui::End();
        return;
    }
    int closeIndex = -1;
    if (ImGui::BeginTabBar("##convtabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyShrink |
                                             ImGuiTabBarFlags_TabListPopupButton)) {
        if (ImGui::TabItemButton(" + ", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) newConversation(nullptr);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nouvelle conversation (Ctrl+N)");
        for (int i = 0; i < (int)open_.size(); ++i) {
            auto& oc = *open_[i];
            auto& s = *oc.session;
            std::string label = util::truncateUtf8(s.title.empty() ? "Sans titre" : s.title, 32);
            if (s.state == claude::SessionState::Busy) label += "  ●";
            if (!s.permissions.empty()) label += "  ⚠";
            label += "###tab" + s.localId;
            bool openFlag = true;
            ImGuiTabItemFlags flags = 0;
            if (selectTab_ == i) flags |= ImGuiTabItemFlags_SetSelected;
            bool selected = ImGui::BeginTabItem(label.c_str(), &openFlag, flags);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(s.title.c_str());
                if (auto* f = folderOf(oc)) ImGui::TextDisabled("%s", f->displayPath().c_str());
                ImGui::TextDisabled("%s", s.cwd.c_str());
                ImGui::EndTooltip();
            }
            bool closeOthers = false;
            if (ImGui::BeginPopupContextItem(("##tabctx" + s.localId).c_str())) {
                if (ImGui::MenuItem("Générer le récap")) requestRecap(s.localId);
                if (ImGui::MenuItem("Exporter en Markdown")) exportMarkdown(oc);
                if (ImGui::MenuItem("Redémarrer la session")) s.restart();
                ImGui::Separator();
                if (ImGui::MenuItem("Fermer", "Ctrl+W")) closeIndex = i;
                if (ImGui::MenuItem("Fermer les autres")) closeOthers = true;
                ImGui::EndPopup();
            }
            if (selected) {
                active_ = i;
                ImGui::PushID(s.localId.c_str());
                drawConversation(oc);
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            if (closeOthers) {
                std::string keep = s.localId;
                for (int k = (int)open_.size() - 1; k >= 0; --k)
                    if (open_[k]->session->localId != keep) closeConversation(k);
                active_ = 0;
                break;
            }
            if (!openFlag) closeIndex = i;
        }
        selectTab_ = -1;
        ImGui::EndTabBar();
    }
    if (closeIndex >= 0) closeConversation(closeIndex);
    ImGui::End();
}

void App::drawWelcome() {
    const auto& pal = ui::palette();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 2));
    float colW = std::min(w - 40, ImGui::GetFontSize() * 34);
    ImGui::SetCursorPosX((w - colW) * 0.5f);
    ImGui::BeginGroup();
    ImGui::PushFont(fontBold, ImGui::GetStyle().FontSizeBase * 1.8f);
    ImGui::TextColored(pal.accent, "ClaudeDesk");
    ImGui::PopFont();
    ImGui::TextDisabled("Une interface de bureau pour Claude Code, organisée en projets et thèmes.");
    ImGui::Dummy(ImVec2(0, 10));
    ImVec2 bs(colW, ImGui::GetFrameHeight() * 1.5f);
    if (ImGui::Button("Nouvelle conversation   (Ctrl+N)", bs)) newConversation(nullptr);
    if (ImGui::Button("Nouveau projet…", bs)) openNewProject_ = true;
    if (ImGui::Button("Importer une session depuis Claude Code (terminal)…", bs)) showHistory_ = true;
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextDisabled("Première fois ?");
    float third = (colW - ImGui::GetStyle().ItemSpacing.x * 2) / 3;
    if (ImGui::Button("Visite guidée", ImVec2(third, 0))) startTour();
    ImGui::SameLine();
    if (ImGui::Button("Guide de démarrage", ImVec2(third, 0))) showGuide_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Projet exemple", ImVec2(third, 0))) createExampleProject();
    ImGui::Dummy(ImVec2(0, 12));

    // Conversations récentes, tous projets confondus.
    std::vector<std::pair<ws::Folder*, ws::ConversationMeta*>> recent;
    std::function<void(ws::Folder*)> collect = [&](ws::Folder* f) {
        for (auto& c : f->conversations) recent.push_back({f, &c});
        for (auto& ch : f->children) collect(ch.get());
    };
    for (auto& p : workspace_.projects()) collect(p.get());
    std::sort(recent.begin(), recent.end(), [](auto& a, auto& b) { return a.second->updated > b.second->updated; });
    if (!recent.empty()) {
        ImGui::SeparatorText("Reprendre");
        for (size_t i = 0; i < recent.size() && i < 10; ++i) {
            auto [f, m] = recent[i];
            ImGui::PushID((int)i);
            if (ImGui::Selectable(("##r" + m->id).c_str(), false, 0, ImVec2(colW, ImGui::GetTextLineHeight() * 2.1f))) {
                openConversation(m->id);
                ImGui::PopID();
                break;
            }
            ImGui::SameLine(ImGui::GetStyle().ItemSpacing.x);
            ImGui::BeginGroup();
            ImGui::TextUnformatted(m->title.c_str());
            ImGui::TextDisabled("%s  ·  %s  ·  %d messages", f->displayPath().c_str(), util::humanDate(m->updated).c_str(),
                                m->messages);
            ImGui::EndGroup();
            ImGui::PopID();
        }
    }
    ImGui::Dummy(ImVec2(0, 12));
    ImGui::TextDisabled("Astuce : F1 affiche tous les raccourcis clavier.");
    ImGui::EndGroup();
}

void App::drawConversation(OpenConversation& oc) {
    auto& s = *oc.session;
    const auto& pal = ui::palette();

    // --- En-tête ---
    {
        ws::Folder* f = folderOf(oc);
        float startX = ImGui::GetCursorPosX();
        float avail = ImGui::GetContentRegionAvail().x;
        float bw = ImGui::CalcTextSize("Récap").x + ImGui::CalcTextSize("Rechercher").x + ImGui::CalcTextSize("Exporter").x +
                   ImGui::GetStyle().FramePadding.x * 6 + ImGui::GetStyle().ItemSpacing.x * 3;
        ImGui::AlignTextToFramePadding();
        std::string where = f ? f->displayPath() : "Non classée";
        ImGui::TextColored(pal.dim, "%s", where.c_str());
        ImGui::SameLine();
        float room = avail - bw - ImGui::CalcTextSize(where.c_str()).x - 30;
        ImGui::TextDisabled("·  %s", ellipsizeLeft(s.cwd, room).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nDossier de travail de Claude (clic : ouvrir dans l'Explorateur)", s.cwd.c_str());
        if (ImGui::IsItemClicked()) util::openInShell(s.cwd);
        ImGui::SameLine(startX + std::max(0.0f, avail - bw));
        if (ImGui::SmallButton("Rechercher")) oc.focusSearch = oc.showSearch = !oc.showSearch;
        ImGui::SameLine();
        if (ImGui::SmallButton("Récap")) requestRecap(s.localId);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Génère un récapitulatif de cette conversation (utilisé pour reprendre le travail et\n"
                              "injecté dans les nouvelles conversations du même dossier). Ctrl+Maj+R");
        ImGui::SameLine();
        if (ImGui::SmallButton("Exporter")) exportMarkdown(oc);
    }
    if (oc.showSearch) {
        ImGui::SetNextItemWidth(-1);
        if (oc.focusSearch) ImGui::SetKeyboardFocusHere();
        oc.focusSearch = false;
        ImGui::InputTextWithHint("##search", "Filtrer les messages… (Échap pour fermer)", &oc.search);
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            oc.showSearch = false;
            oc.search.clear();
        }
    }

    // --- Hauteur réservée en bas : permissions + barre rapide + éditeur ---
    float lineH = ImGui::GetFrameHeightWithSpacing();
    float bottom = oc.editorHeight + lineH * 1.4f + 10;
    if (!s.permissions.empty())
        bottom += oc.permPanelHeight > 0 ? oc.permPanelHeight
                                         : std::min(ImGui::GetContentRegionAvail().y * 0.55f, ImGui::GetFontSize() * 20);
    else
        oc.permPanelHeight = 0;
    if (!oc.attachments.empty()) bottom += lineH;

    // Défilement fluide : la molette fixe une cible, la vue y glisse en douceur.
    bool smoothScroll = settings_.smoothScroll && settings_.animations;
    ImGui::BeginChild("##messages", ImVec2(0, -bottom), ImGuiChildFlags_None,
                      smoothScroll ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None);
    float scrollMax = ImGui::GetScrollMaxY();
    if (smoothScroll) {
        ImGuiIO& io = ImGui::GetIO();
        float cur = ImGui::GetScrollY();
        // La barre de défilement a été déplacée à la souris : la cible suit.
        if (oc.scrollSetLast >= 0 && std::fabs(cur - oc.scrollSetLast) > 1.5f) oc.scrollTarget = cur;
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel != 0 && !io.KeyCtrl) {
            oc.scrollTarget = std::clamp(oc.scrollTarget, 0.0f, scrollMax) - io.MouseWheel * ImGui::GetTextLineHeightWithSpacing() * 4.5f;
            oc.scrollTarget = std::clamp(oc.scrollTarget, 0.0f, scrollMax);
        }
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput) {
            float page = ImGui::GetWindowHeight() * 0.85f;
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) oc.scrollTarget = std::min(scrollMax, std::min(oc.scrollTarget, scrollMax) + page);
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) oc.scrollTarget = std::max(0.0f, std::min(oc.scrollTarget, scrollMax) - page);
            if (ImGui::IsKeyPressed(ImGuiKey_Home)) oc.scrollTarget = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_End)) oc.scrollTarget = 1e9f;
        }
    }
    bool atBottom = smoothScroll ? oc.scrollTarget >= scrollMax - 30 : ImGui::GetScrollY() >= scrollMax - 30;
    float width = ImGui::GetContentRegionAvail().x;
    if (oc.cache.size() != s.entries.size()) {
        size_t old = oc.cache.size();
        oc.cache.resize(s.entries.size());
        // Les messages qui arrivent pendant la session apparaissent en fondu (pas ceux chargés du disque).
        if (old > 0)
            for (size_t k = old; k < oc.cache.size(); ++k) oc.cache[k].appear = 0;
    }
    ImGui::Dummy(ImVec2(0, 4));
    int shown = 0;
    int hoveredNow = -1;
    for (int i = 0; i < (int)s.entries.size(); ++i) {
        const Entry& e = s.entries[i];
        if (!e.parentToolUseId.empty()) continue;  // affichées sous l'outil Task correspondant
        if (e.kind == EntryKind::Thinking && (!settings_.showThinking || (e.text.empty() && !e.streaming))) continue;
        if (!oc.search.empty() && !util::containsInsensitive(e.text, oc.search) &&
            !util::containsInsensitive(e.toolResult, oc.search))
            continue;
        ++shown;
        // En-tête « Claude » au début de chaque réponse (après un message de l'utilisateur).
        if (e.kind != EntryKind::User && e.kind != EntryKind::System && e.kind != EntryKind::Error && oc.search.empty()) {
            int prev = i - 1;
            while (prev >= 0 && (!s.entries[prev].parentToolUseId.empty() ||
                                 (s.entries[prev].kind == EntryKind::Thinking && s.entries[prev].text.empty())))
                --prev;
            if (prev >= 0 && s.entries[prev].kind == EntryKind::User) {
                ImGui::PushFont(fontBold, 0.0f);
                ImGui::TextColored(pal.accent, "Claude");
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", clockTime(e.time).c_str());
            }
        }
        EntryCache& c = oc.cache[i];
        // Saut des entrées hors écran (hauteur connue) : la liste reste fluide même très longue.
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (c.height > 0 && c.width == width && !e.streaming && c.textLen == e.text.size() &&
            !ImGui::IsRectVisible(p, ImVec2(p.x + width, p.y + c.height))) {
            ImGui::Dummy(ImVec2(width, c.height));
            continue;
        }
        ImGui::PushID(i);
        // Fondu + léger glissement à l'apparition
        float appearT = 1.0f;
        if (settings_.animations && c.appear >= 0) {
            if (c.appear == 0) c.appear = now_;
            appearT = ui::anim::easeOutCubic((float)((now_ - c.appear) / (0.35 / settings_.animSpeed)));
            if (appearT >= 1.0f) c.appear = -1;
            else ui::anim::markBusy();
        }
        float slide = (1.0f - appearT) * ImGui::GetFontSize() * 0.8f;
        if (appearT < 1.0f) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::max(0.02f, appearT));
        if (slide > 0.5f) ImGui::Indent(slide);
        ImGui::BeginGroup();
        drawEntry(oc, i, width - slide);
        ImGui::EndGroup();
        if (slide > 0.5f) ImGui::Unindent(slide);
        if (appearT < 1.0f) ImGui::PopStyleVar();
        c.height = ImGui::GetItemRectSize().y;
        c.width = width;
        if (e.kind == EntryKind::User || e.kind == EntryKind::Assistant) {
            ImVec2 gMin = ImGui::GetItemRectMin(), gMax = ImGui::GetItemRectMax();
            bool hov = ImGui::IsMouseHoveringRect(gMin, gMax) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            if (hov) hoveredNow = i;
            if (hov || oc.selectable.count(i)) {
                // Boutons en surimpression en haut à droite du message
                ImVec2 saved = ImGui::GetCursorScreenPos();
                bool sel = oc.selectable.count(i) > 0;
                const char* selLbl = sel ? "Rendu" : "Sélectionner";
                float bw2 = ImGui::CalcTextSize("Copier").x + ImGui::CalcTextSize(selLbl).x +
                            ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x + 10;
                ImGui::SetCursorScreenPos(ImVec2(gMax.x - bw2, gMin.y + 4));
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::SmallButton("Copier")) {
                    ImGui::SetClipboardText(e.text.c_str());
                    toast("Copié dans le presse-papiers.");
                }
                ImGui::SameLine();
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::SmallButton(selLbl)) {
                    if (sel) oc.selectable.erase(i);
                    else oc.selectable.insert(i);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Texte sélectionnable à la souris (Ctrl+A, Ctrl+C…)");
                ImGui::SetCursorScreenPos(saved);
            }
            // Clic droit n'importe où sur le message
            if (ImGui::BeginPopupContextItem("##entryctx")) {
                if (ImGui::MenuItem("Copier le message")) ImGui::SetClipboardText(e.text.c_str());
                if (ImGui::MenuItem("Sélectionner le texte", nullptr, oc.selectable.count(i) > 0)) {
                    if (oc.selectable.count(i)) oc.selectable.erase(i);
                    else oc.selectable.insert(i);
                }
                if (e.kind == EntryKind::User && ImGui::MenuItem("Réutiliser dans l'éditeur")) {
                    oc.draft = e.text;
                    oc.replaceDraft = e.text;
                    oc.hasReplace = true;
                    oc.focusEditor = true;
                    oc.cursorToEnd = true;
                }
                if (ImGui::MenuItem("Copier toute la conversation (Markdown)"))
                    ImGui::SetClipboardText(claude::transcriptToMarkdown(s.entries, 0).c_str());
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }
    oc.hoveredEntry = hoveredNow;
    if (!oc.search.empty() && shown == 0) ImGui::TextDisabled("Aucun message ne contient « %s ».", oc.search.c_str());
    if (s.entries.empty()) {
        ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 2));
        ImGui::TextColored(pal.accent, "Nouvelle conversation");
        ImGui::TextDisabled("Dossier de travail : %s", s.cwd.c_str());
        if (auto* f = folderOf(oc)) {
            ImGui::TextDisabled("Rangée dans : %s", f->displayPath().c_str());
            if (oc.injectContext) {
                std::string ctx = workspace_.buildContext(f, s.localId);
                ImGui::TextDisabled("Contexte cowork injecté : %s caractères (notes, synthèses et récaps du dossier).",
                                    util::humanTokens((int64_t)ctx.size()).c_str());
            }
        }
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::TextDisabled("Écrivez votre message en bas. Glissez-déposez des fichiers ou des images dans la fenêtre pour les joindre.");
    }

    // Indicateur d'activité
    if (s.state == claude::SessionState::Busy || s.state == claude::SessionState::Starting) {
        ImGui::Dummy(ImVec2(0, 2));
        ui::spinner((float)now_, pal.accent);
        ImGui::SameLine();
        double el = now_ - s.turnStartTime;
        std::string st = s.statusText.empty() ? "Claude travaille…" : s.statusText;
        if (s.thinkingTokens > 0) st += "  (" + util::humanTokens(s.thinkingTokens) + " jetons de réflexion)";
        ImGui::TextColored(pal.dim, "%s  ·  %s", st.c_str(), util::humanDuration((int64_t)el).c_str());
        if (s.queuedMessages > 0) {
            ImGui::SameLine();
            ImGui::TextColored(pal.warn, "· %d message(s) en file", s.queuedMessages);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Interrompre (Échap)")) s.interrupt();
    }
    ImGui::Dummy(ImVec2(0, 6));

    bool follow = oc.scrollToBottom || (atBottom && s.state == claude::SessionState::Busy);
    if (smoothScroll) {
        if (!oc.scrollInit) {  // ouverture : on se place directement en bas, sans animation
            ImGui::SetScrollHereY(1.0f);
            oc.scrollTarget = 1e9f;
            oc.scrollInit = true;
            oc.scrollSetLast = -1;
        } else {
            if (follow) oc.scrollTarget = 1e9f;  // suit le bas pendant que Claude écrit
            float tgt = std::clamp(oc.scrollTarget, 0.0f, scrollMax);
            float cur = ImGui::GetScrollY();
            float k = 1.0f - std::exp(-16.0f * ui::anim::dt());
            float nv = cur + (tgt - cur) * k;
            if (std::fabs(tgt - nv) < 0.6f) nv = tgt;
            else ui::anim::markBusy();
            if (nv != cur) ImGui::SetScrollY(nv);
            oc.scrollSetLast = nv;
        }
    } else if (follow) {
        ImGui::SetScrollHereY(1.0f);
    }
    oc.scrollToBottom = false;
    ImVec2 msgMin = ImGui::GetWindowPos(), msgSize = ImGui::GetWindowSize();
    ImGui::EndChild();
    // Bouton flottant « revenir en bas », dessiné par-dessus le fil.
    if (!atBottom) {
        ImVec2 saved = ImGui::GetCursorPos();
        const char* lbl = "  ↓  ";
        ImVec2 sz = ImGui::CalcTextSize(lbl);
        ImGui::SetCursorScreenPos(ImVec2(msgMin.x + msgSize.x - sz.x - 40, msgMin.y + msgSize.y - sz.y - 24));
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::Button(lbl)) oc.scrollToBottom = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Aller en bas");
        ImGui::SetCursorPos(saved);
    }

    // Séparateur redimensionnable entre le fil et l'éditeur
    {
        ImGui::InvisibleButton("##split", ImVec2(-1, 6));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive())
            oc.editorHeight = std::clamp(oc.editorHeight - ImGui::GetIO().MouseDelta.y, ImGui::GetFontSize() * 3,
                                         ImGui::GetMainViewport()->Size.y * 0.6f);
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        float mid = (a.y + b.y) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + (b.x - a.x) * 0.45f, mid), ImVec2(a.x + (b.x - a.x) * 0.55f, mid),
                                            ui::col(pal.dim, 0.6f), 2);
    }
    drawPermissionPanel(oc);
    drawPromptEditor(oc);
}

void App::drawEntry(OpenConversation& oc, int i, float width) {
    auto& s = *oc.session;
    const Entry& e = s.entries[i];
    const auto& pal = ui::palette();
    EntryCache& cache = oc.cache[i];
    ui::MdStyle md{fontUi, fontBold, fontMono, settings_.wrapCode, fontItalic};

    switch (e.kind) {
        case EntryKind::User: {
            ImGui::Dummy(ImVec2(0, 2));
            Card card(width, pal.userBg, true);
            {
                RightMargin rm(card.pad);
                ImGui::PushFont(fontBold, 0.0f);
                ImGui::TextColored(pal.accent, "Vous");
                ImGui::PopFont();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", clockTime(e.time).c_str());
                if (e.attachments > 0) {
                    ImGui::SameLine();
                    ui::badge((std::to_string(e.attachments) + " image(s)").c_str(), pal.link);
                }
                if (oc.selectable.count(i)) {
                    ui::selectableText("##sel", e.text, ImGui::GetContentRegionAvail().x);
                } else {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(e.text.c_str());
                    ImGui::PopTextWrapPos();
                }
            }
            card.end();
            ImGui::Dummy(ImVec2(0, 2));
            break;
        }
        case EntryKind::Assistant: {
            if (cache.textLen != e.text.size() || cache.streaming != e.streaming) {
                cache.blocks = ui::parseMarkdown(e.text);
                cache.textLen = e.text.size();
                cache.streaming = e.streaming;
            }
            ImGui::Indent(4);
            {
                RightMargin rm(8);
                if (oc.selectable.count(i)) {
                    ui::selectableText("##sel", e.text, ImGui::GetContentRegionAvail().x);
                } else {
                    ui::renderMarkdown(cache.blocks, md, i);
                    if (e.streaming) {
                        ImGui::SameLine(0, 4);
                        ImVec4 cc = pal.accent;
                        cc.w = 0.3f + 0.7f * ui::anim::pulse(1.4f);
                        ImGui::TextColored(cc, "▍");
                    }
                }
            }
            ImGui::Unindent(4);
            break;
        }
        case EntryKind::Thinking: {
            bool open = oc.expanded.count(i) > 0;
            ImGui::PushStyleColor(ImGuiCol_Text, pal.dim);
            std::string label = std::string(open ? "▾ " : "▸ ") + "Réflexion" +
                                (e.text.empty() ? (e.streaming ? "…" : " (contenu non transmis)")
                                                : "  · " + util::humanTokens((int64_t)e.text.size() / 4) + " jetons");
            if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0, 0))) {
                if (open) oc.expanded.erase(i);
                else oc.expanded.insert(i);
            }
            if (open && !e.text.empty()) {
                ImGui::Indent(14);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(e.text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Unindent(14);
            }
            ImGui::PopStyleColor();
            break;
        }
        case EntryKind::ToolUse:
            drawToolEntry(oc, i, width);
            break;
        case EntryKind::System:
        case EntryKind::Error: {
            ImGui::PushStyleColor(ImGuiCol_Text, e.kind == EntryKind::Error ? pal.err : pal.dim);
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(((e.kind == EntryKind::Error ? "⚠ " : "• ") + e.text).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            break;
        }
    }
}

void App::drawToolEntry(OpenConversation& oc, int i, float width) {
    auto& s = *oc.session;
    const Entry& e = s.entries[i];
    const auto& pal = ui::palette();
    bool running = !e.toolDone && (s.state == claude::SessionState::Busy);
    bool open = settings_.expandTools ? !oc.collapsedOverride.count(i) : oc.expanded.count(i) > 0;

    ImVec4 stateCol = running ? pal.warn : e.toolError ? pal.err : e.toolDone ? pal.ok : pal.dim;
    const char* stateIcon = running ? "◌" : e.toolError ? "✗" : e.toolDone ? "✓" : "·";
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    std::string label = "##tool";
    bool clicked = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowOverlap, ImVec2(width - 8, 0));
    ImGui::PopStyleColor();
    ImGui::SameLine(ImGui::GetStyle().ItemSpacing.x + 2);
    ImGui::TextColored(pal.dim, "%s", open ? "▾" : "▸");
    ImGui::SameLine();
    ImGui::TextColored(stateCol, "%s", stateIcon);
    ImGui::SameLine();
    ImGui::PushFont(fontBold, 0.0f);
    ImGui::TextUnformatted(e.toolName.c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    std::string detail = e.text.size() > e.toolName.size() + 3 ? e.text.substr(e.toolName.size() + 3) : "";
    ImGui::TextColored(pal.dim, "%s", detail.c_str());
    // Nombre d'actions d'un sous-agent
    if (e.toolName == "Task" || e.toolName == "Agent") {
        int sub = 0;
        for (auto& x : s.entries)
            if (x.parentToolUseId == e.toolUseId && x.kind == EntryKind::ToolUse) ++sub;
        if (sub) {
            ImGui::SameLine();
            ui::badge((std::to_string(sub) + " actions").c_str(), pal.link);
        }
    }
    if (clicked) {
        if (settings_.expandTools) {
            if (open) oc.collapsedOverride.insert(i);
            else oc.collapsedOverride.erase(i);
        } else {
            if (open) oc.expanded.erase(i);
            else oc.expanded.insert(i);
        }
    }
    if (!open) return;

    json input = json::parse(e.toolInput, nullptr, false);
    auto str = [&](const char* k) { return input.is_object() && input.contains(k) && input[k].is_string() ? input[k].get<std::string>() : std::string(); };
    Card card(width - 8, pal.cardBg);
    {
        RightMargin rm(card.pad);
        bool showAll = oc.selectable.count(-i - 1) > 0;  // clé négative : « tout afficher » des outils
        if (e.toolName == "Edit" || e.toolName == "MultiEdit") {
            ImGui::TextDisabled("%s", str("file_path").c_str());
            if (input.contains("edits") && input["edits"].is_array()) {
                for (auto& ed : input["edits"])
                    diffView(fontMono, ed.value("old_string", ""), ed.value("new_string", ""));
            } else {
                diffView(fontMono, str("old_string"), str("new_string"));
            }
        } else if (e.toolName == "Write") {
            ImGui::TextDisabled("%s", str("file_path").c_str());
            monoText(fontMono, str("content"), 25, &showAll, &pal.diffAdd);
        } else if (e.toolName == "Bash" || e.toolName == "PowerShell") {
            if (!str("description").empty()) ImGui::TextDisabled("%s", str("description").c_str());
            monoText(fontMono, (e.toolName == "Bash" ? "$ " : "PS> ") + str("command"), 30, nullptr, &pal.codeText);
        } else if (e.toolName == "TodoWrite" && input.contains("todos")) {
            for (auto& t : input["todos"]) {
                std::string st = t.value("status", "");
                ImVec4 c = st == "completed" ? pal.ok : st == "in_progress" ? pal.warn : pal.dim;
                ImGui::TextColored(c, "%s", st == "completed" ? "☑" : st == "in_progress" ? "◐" : "☐");
                ImGui::SameLine();
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(t.value("content", "").c_str());
                ImGui::PopTextWrapPos();
            }
        } else if (e.toolName == "Task" || e.toolName == "Agent") {
            ImGui::TextDisabled("Sous-agent : %s", str("subagent_type").empty() ? "général" : str("subagent_type").c_str());
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(util::truncateUtf8(str("prompt"), 1500).c_str());
            ImGui::PopTextWrapPos();
            ImGui::SeparatorText("Activité");
            for (auto& x : s.entries) {
                if (x.parentToolUseId != e.toolUseId) continue;
                if (x.kind == EntryKind::ToolUse)
                    ImGui::TextColored(x.toolError ? pal.err : pal.dim, "%s %s", x.toolError ? "✗" : "✓", x.text.c_str());
                else if (x.kind == EntryKind::Assistant && !x.text.empty()) {
                    ImGui::PushTextWrapPos(0.0f);
                    ImGui::TextUnformatted(util::truncateUtf8(x.text, 600).c_str());
                    ImGui::PopTextWrapPos();
                }
            }
        } else {
            monoText(fontMono, e.toolInput, 20, &showAll);
        }
        if (!e.toolResult.empty()) {
            ImGui::Dummy(ImVec2(0, 2));
            ImGui::TextColored(e.toolError ? pal.err : pal.dim, e.toolError ? "Erreur" : "Résultat");
            ImGui::SameLine();
            if (ImGui::SmallButton("Copier##res")) ImGui::SetClipboardText(e.toolResult.c_str());
            monoText(fontMono, e.toolResult, 14, &showAll, e.toolError ? &pal.err : nullptr);
        } else if (running) {
            ImGui::TextColored(pal.warn, "En cours…");
        }
        if (showAll) oc.selectable.insert(-i - 1);
    }
    card.end();
}

}  // namespace app
