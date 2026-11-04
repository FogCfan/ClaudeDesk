// Demandes de Claude : autorisation d'un outil, questions à choix (AskUserQuestion)
// et validation de plan (ExitPlanMode).
#include <algorithm>
#include <cfloat>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "app/App.h"
#include "ui/Theme.h"
#include "util/Strings.h"

namespace app {

using json = nlohmann::json;

namespace {

std::string jstr(const json& j, const char* k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}

void mono(ImFont* f, const std::string& text, const ImVec4* color = nullptr, int maxLines = 30) {
    ImGui::PushFont(f, ImGui::GetStyle().FontSizeBase * 0.84f);
    if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
    ImGui::PushTextWrapPos(0.0f);
    int n = 0;
    for (auto& l : util::splitLines(text)) {
        if (++n > maxLines) {
            ImGui::TextDisabled("… (%d lignes au total)", (int)util::splitLines(text).size());
            break;
        }
        ImGui::TextUnformatted(l.c_str());
    }
    ImGui::PopTextWrapPos();
    if (color) ImGui::PopStyleColor();
    ImGui::PopFont();
}

std::string describeSuggestion(const json& s) {
    std::string type = jstr(s, "type");
    if (type == "setMode") return "passer en mode « " + jstr(s, "mode") + " » pour la session";
    if (type == "addRules" || type == "replaceRules") {
        std::string r;
        for (auto& rule : s.value("rules", json::array())) {
            if (!r.empty()) r += ", ";
            r += jstr(rule, "toolName");
            if (!jstr(rule, "ruleContent").empty()) r += "(" + jstr(rule, "ruleContent") + ")";
        }
        return "toujours autoriser " + r + (jstr(s, "destination") == "session" ? " (session)" : " (" + jstr(s, "destination") + ")");
    }
    if (type == "addDirectories") return "autoriser l'accès au dossier";
    return type;
}

}  // namespace

void App::drawPermissionPanel(OpenConversation& oc) {
    auto& s = *oc.session;
    if (s.permissions.empty()) return;
    const auto& pal = ui::palette();
    claude::PermissionRequest& p = s.permissions.front();
    float maxH = std::min(ImGui::GetContentRegionAvail().y * 0.55f, ImGui::GetFontSize() * 20);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, pal.cardBg);
    ImGui::PushStyleColor(ImGuiCol_Border, p.isQuestion ? pal.link : pal.warn);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxH));
    ImGui::BeginChild(("##perm" + p.requestId).c_str(), ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    if (p.isQuestion) {
        // --- Questions à choix ---
        ImGui::PushFont(fontBold, 0.0f);
        ImGui::TextColored(pal.link, "Claude vous pose %s", p.input.value("questions", json::array()).size() > 1 ? "des questions" : "une question");
        ImGui::PopFont();
        auto questions = p.input.value("questions", json::array());
        int qi = 0;
        for (auto& q : questions) {
            std::string question = jstr(q, "question");
            bool multi = q.value("multiSelect", false);
            ImGui::PushID(qi++);
            ImGui::SeparatorText(jstr(q, "header").c_str());
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(question.c_str());
            ImGui::PopTextWrapPos();
            std::string& ans = oc.answers[question];
            auto selected = util::split(ans, '\x1f');
            auto isSel = [&](const std::string& l) { return std::find(selected.begin(), selected.end(), l) != selected.end(); };
            int oi = 0;
            for (auto& opt : q.value("options", json::array())) {
                std::string label = jstr(opt, "label");
                ImGui::PushID(oi++);
                bool sel = isSel(label);
                bool clicked = multi ? ImGui::Checkbox(label.c_str(), &sel) : ImGui::RadioButton(label.c_str(), sel);
                if (clicked) {
                    if (multi) {
                        std::vector<std::string> v;
                        for (auto& x : selected)
                            if (!x.empty() && x != label) v.push_back(x);
                        if (sel) v.push_back(label);
                        ans.clear();
                        for (auto& x : v) ans += (ans.empty() ? "" : "\x1f") + x;
                    } else {
                        ans = label;
                        oc.otherAnswers[question].clear();
                    }
                }
                if (!jstr(opt, "description").empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("— %s", jstr(opt, "description").c_str());
                }
                ImGui::PopID();
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextWithHint("##other", "Autre réponse (texte libre)…", &oc.otherAnswers[question]) && !multi)
                ans.clear();
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, 4));
        if (ImGui::Button("Répondre")) {
            json answers = json::object();
            for (auto& q : questions) {
                std::string question = jstr(q, "question");
                std::string a = oc.answers[question];
                util::replaceAll(a, "\x1f", ", ");
                std::string other = util::trim(oc.otherAnswers[question]);
                if (!other.empty()) a = a.empty() ? other : a + ", " + other;
                answers[question] = a;
            }
            json updated = p.input;
            updated["answers"] = answers;
            oc.answers.clear();
            oc.otherAnswers.clear();
            s.respondPermission(p.requestId, true, {}, updated);
        }
        ImGui::SameLine();
        if (ImGui::Button("Ignorer")) s.respondPermission(p.requestId, false, "L'utilisateur a préféré ne pas répondre.");
    } else if (p.toolName == "ExitPlanMode") {
        // --- Validation d'un plan ---
        ImGui::PushFont(fontBold, 0.0f);
        ImGui::TextColored(pal.accent, "Claude a terminé son plan");
        ImGui::PopFont();
        ui::MdStyle md{fontUi, fontBold, fontMono, settings_.wrapCode, fontItalic};
        ui::renderMarkdown(ui::parseMarkdown(jstr(p.input, "plan")), md, 99991);
        ImGui::Separator();
        if (ImGui::Button("Approuver et passer à l'action")) {
            for (auto& sg : p.suggestions)
                if (jstr(sg, "type") == "setMode") s.config.permissionMode = jstr(sg, "mode");
            if (s.config.permissionMode == "plan") s.config.permissionMode = "default";
            s.respondPermission(p.requestId, true, {}, nullptr, true);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Claude sort du mode plan et commence les modifications.");
        ImGui::SameLine();
        if (ImGui::Button("Continuer à planifier")) s.respondPermission(p.requestId, false, oc.denyReason.empty() ? "Continue à affiner le plan." : oc.denyReason);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##why", "Remarques sur le plan (envoyées si vous continuez à planifier)…", &oc.denyReason);
    } else {
        // --- Autorisation d'un outil ---
        ImGui::PushFont(fontBold, 0.0f);
        ImGui::TextColored(pal.warn, "Claude demande l'autorisation : %s", p.toolName.c_str());
        ImGui::PopFont();
        if (!p.description.empty() && p.description != p.toolName) {
            ImGui::SameLine();
            ImGui::TextDisabled("— %s", p.description.c_str());
        }
        const json& in = p.input;
        if (p.toolName == "Bash" || p.toolName == "PowerShell") {
            if (!jstr(in, "description").empty()) ImGui::TextDisabled("%s", jstr(in, "description").c_str());
            mono(fontMono, jstr(in, "command"), &pal.codeText);
        } else if (p.toolName == "Edit" || p.toolName == "MultiEdit") {
            ImGui::TextUnformatted(jstr(in, "file_path").c_str());
            auto diff = [&](const json& e) {
                mono(fontMono, jstr(e, "old_string"), &pal.diffDel, 15);
                mono(fontMono, jstr(e, "new_string"), &pal.diffAdd, 15);
            };
            if (in.contains("edits")) for (auto& e : in["edits"]) diff(e);
            else diff(in);
        } else if (p.toolName == "Write") {
            ImGui::TextUnformatted(jstr(in, "file_path").c_str());
            mono(fontMono, jstr(in, "content"), &pal.diffAdd, 20);
        } else if (p.toolName == "WebFetch") {
            ImGui::TextUnformatted(jstr(in, "url").c_str());
            ImGui::TextDisabled("%s", jstr(in, "prompt").c_str());
        } else {
            mono(fontMono, in.dump(2, ' ', false, json::error_handler_t::replace), nullptr, 20);
        }
        ImGui::Dummy(ImVec2(0, 4));
        bool allow = ImGui::Button("Autoriser  (Alt+A)") || ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_A, ImGuiInputFlags_RouteGlobal);
        bool always = false;
        if (p.suggestions.is_array() && !p.suggestions.empty()) {
            ImGui::SameLine();
            always = ImGui::Button("Toujours autoriser  (Alt+T)") || ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_T, ImGuiInputFlags_RouteGlobal);
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                for (auto& sg : p.suggestions) ImGui::BulletText("%s", describeSuggestion(sg).c_str());
                ImGui::EndTooltip();
            }
        }
        ImGui::SameLine();
        bool deny = ImGui::Button("Refuser  (Alt+R)") || ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_R, ImGuiInputFlags_RouteGlobal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##why", "Raison du refus / consigne pour Claude (optionnel)", &oc.denyReason);
        if (allow || always) {
            if (always)
                for (auto& sg : p.suggestions)
                    if (jstr(sg, "type") == "setMode") s.config.permissionMode = jstr(sg, "mode");
            s.respondPermission(p.requestId, true, {}, nullptr, always);
            oc.denyReason.clear();
        } else if (deny) {
            s.respondPermission(p.requestId, false, oc.denyReason);
            oc.denyReason.clear();
        }
    }
    if (s.permissions.size() > 1) ImGui::TextDisabled("%d autre(s) demande(s) en attente", (int)s.permissions.size() - 1);
    ImGui::EndChild();
    oc.permPanelHeight = ImGui::GetItemRectSize().y + ImGui::GetStyle().ItemSpacing.y;
}

}  // namespace app
