#include "claude/Auth.h"

#include <nlohmann/json.hpp>

#include "util/Strings.h"

using json = nlohmann::json;

namespace claude {

bool isAuthErrorText(const std::string& text) {
    static const char* const patterns[] = {
        "failed to authenticate",
        "authentication_failed",
        "authentication_error",
        "invalid api key",
        "invalid x-api-key",
        "oauth token has expired",
        "oauth token revoked",
        "reached its time limit",
        "claude auth login",
        "please run /login",
        "run /login",
        "not logged in",
    };
    for (const char* p : patterns)
        if (util::containsInsensitive(text, p)) return true;
    return false;
}

std::string AuthStatus::planLabel() const {
    if (plan.empty()) return {};
    std::string p = plan;
    if (!p.empty()) p[0] = (char)std::toupper((unsigned char)p[0]);
    return "Claude " + p;
}

AuthStatus parseAuthStatus(const std::string& output) {
    AuthStatus st;
    size_t a = output.find('{'), b = output.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a) {
        st.error = util::trim(output).empty() ? "aucune réponse de « claude auth status »" : util::trim(output);
        return st;
    }
    json j = json::parse(output.substr(a, b - a + 1), nullptr, false);
    if (!j.is_object()) {
        st.error = "réponse illisible de « claude auth status »";
        return st;
    }
    auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    st.known = true;
    st.loggedIn = j.value("loggedIn", false);
    st.method = str("authMethod");
    st.email = str("email");
    st.orgName = str("orgName");
    st.plan = str("subscriptionType");
    if (st.plan.empty()) st.plan = str("planTier");
    st.expiresAt = util::unixFromIso(str("loginExpiresAt"));
    return st;
}

bool AuthChecker::start(const std::string& exe) {
    finished_ = false;
    out_.clear();
    err_.clear();
    status_ = {};
    if (exe.empty()) {
        status_.error = "Claude Code introuvable";
        finished_ = true;
        return false;
    }
    process_ = std::make_unique<Process>();
    if (!process_->start(exe, {"auth", "status", "--json"}, "")) {
        status_.error = process_->error();
        process_.reset();
        finished_ = true;
        return false;
    }
    return true;
}

bool AuthChecker::update() {
    if (finished_) return true;
    if (!process_) return finished_ = true;
    auto take = [&] {
        for (auto& l : process_->poll()) (l.isStderr ? err_ : out_) += l.text + "\n";
    };
    take();
    if (process_->running()) return false;
    take();
    process_->shutdown(0);
    process_.reset();
    status_ = parseAuthStatus(out_);
    if (!status_.known && !util::trim(err_).empty()) status_.error = util::trim(err_);
    finished_ = true;
    return true;
}

}  // namespace claude
