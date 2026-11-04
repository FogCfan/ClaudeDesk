#include "ui/Anim.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace ui::anim {

namespace {
struct State {
    bool enabled = true;
    float speed = 1.0f;
    float dt = 1.0f / 60.0f;
    double time = 0;
    bool busyThisFrame = false;
    bool busyLastFrame = false;
};
State g;
std::unordered_map<uint64_t, float> g_values;

uint64_t key(ImGuiID id, int lane = 0) {
    // Les ID ImGui sont propres à chaque contexte (deux fenêtres) : on les combine.
    return ((uint64_t)(uintptr_t)ImGui::GetCurrentContext() << 20) ^ ((uint64_t)id << 2) ^ (uint64_t)lane;
}
}  // namespace

void newFrame(bool enabled, float speed, bool primary) {
    if (primary) {
        g.busyLastFrame = g.busyThisFrame;
        g.busyThisFrame = false;
    }
    g.enabled = enabled;
    g.speed = speed <= 0 ? 1.0f : speed;
    float raw = ImGui::GetIO().DeltaTime;
    if (raw <= 0 || raw > 0.25f) raw = 1.0f / 60.0f;
    g.dt = raw * g.speed;
    if (primary) g.time += raw * g.speed;
    if (g_values.size() > 20000) g_values.clear();  // garde-fou
}

bool enabled() { return g.enabled; }
float dt() { return g.dt; }
double time() { return g.time; }
bool busy() { return g.busyLastFrame || g.busyThisFrame; }
void markBusy() { g.busyThisFrame = true; }

static float approach(float& v, float target, float rate) {
    if (!g.enabled) return v = target;
    float k = 1.0f - std::exp(-rate * g.dt);
    v += (target - v) * k;
    if (std::fabs(target - v) < 0.0015f * (1.0f + std::fabs(target))) v = target;
    else g.busyThisFrame = true;
    return v;
}

float smooth(ImGuiID id, float target, float rate, float initial) {
    auto [it, inserted] = g_values.try_emplace(key(id), initial < 0 ? target : initial);
    return approach(it->second, target, rate);
}

ImVec2 smooth2(ImGuiID id, ImVec2 target, float rate) {
    auto [ix, newX] = g_values.try_emplace(key(id, 1), target.x);
    auto [iy, newY] = g_values.try_emplace(key(id, 2), target.y);
    return ImVec2(approach(ix->second, target.x, rate), approach(iy->second, target.y, rate));
}

void set(ImGuiID id, float value) { g_values[key(id)] = value; }

float easeOutCubic(float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float u = 1 - t;
    return 1 - u * u * u;
}

float smoothstep(float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

float pulse(float hz, float phase) {
    if (!g.enabled) return 0.75f;
    g.busyThisFrame = true;  // une pulsation est une animation continue
    return 0.5f + 0.5f * std::sin((float)(g.time * hz * 6.2831853) + phase);
}

}  // namespace ui::anim
