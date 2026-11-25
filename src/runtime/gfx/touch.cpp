// On-screen touch controls, for phones without a controller (Android; R2_TOUCH=1 shows them
// anywhere, for screenshots): a stick that starts where the left thumb lands, a C-stick that
// starts where the right thumb lands, the GameCube's A / B / X / Y / Z / START, L and R as bars
// (the higher the finger, the more of the analog trigger; the top is the click), the d-pad
// (wingman commands) and a menu button, drawn over the picture with ImGui inside the screen's
// safe area. Touches become port 1's controller (input.cpp). They show while no controller is
// connected; while the settings menu is open only the menu button stays (the menu takes touches
// as mouse clicks).
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_video.h>
#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "../runtime.h"
#include "gx_bridge.h"

namespace rt::gfx {
namespace {

enum Ctl { Stick, CStick, A, B, X, Y, Z, L, R, Start, DUp, DDown, DLeft, DRight, MenuBtn, kCtlCount };
constexpr u16 kBits[kCtlCount] = {0, 0, 0x0100, 0x0200, 0x0400, 0x0800, 0x0010, 0x0040, 0x0020, 0x1000,
                                  0x0008, 0x0004, 0x0001, 0x0002, 0};
const char* const kLabels[kCtlCount] = {"", "", "A", "B", "X", "Y", "Z", "L", "R", "START", "", "", "", "", ""};

struct Circle {
    float x, y, r;
};
struct Bar {
    float x, y, hw, hh;  // middle, half width, half height
};
struct Area {
    float x0, y0, x1, y1;
};
struct Layout {
    Circle c[kCtlCount];
    Bar barL, barR;
    float stickR, knobR;
};

// Positions in units of the safe area's shorter side (landscape), from its corners.
Layout layout(const Area& s) {
    Layout l{};
    const float u = std::min(s.x1 - s.x0, s.y1 - s.y0), m = 0.04f * u;
    l.stickR = 0.17f * u;
    l.knobR = 0.075f * u;
    l.c[Stick] = {s.x0 + m + 0.21f * u, s.y1 - m - 0.21f * u, l.stickR};
    const Circle a{s.x1 - m - 0.28f * u, s.y1 - m - 0.22f * u, 0.105f * u};
    l.c[A] = a;
    l.c[B] = {a.x - 0.19f * u, a.y + 0.11f * u, 0.07f * u};
    l.c[X] = {a.x + 0.20f * u, a.y - 0.06f * u, 0.07f * u};
    l.c[Y] = {a.x - 0.05f * u, a.y - 0.20f * u, 0.07f * u};
    l.c[Z] = {a.x + 0.20f * u, a.y - 0.23f * u, 0.055f * u};
    l.c[CStick] = {a.x - 0.36f * u, a.y - 0.12f * u, l.stickR * 0.7f};
    l.barL = {s.x0 + m + 0.09f * u, s.y0 + m + 0.17f * u, 0.075f * u, 0.15f * u};
    l.barR = {s.x1 - m - 0.09f * u, s.y0 + m + 0.17f * u, 0.075f * u, 0.15f * u};
    l.c[L] = {l.barL.x, l.barL.y, l.barL.hw};
    l.c[R] = {l.barR.x, l.barR.y, l.barR.hw};
    const float mid = (s.x0 + s.x1) * 0.5f;
    l.c[Start] = {mid + 0.09f * u, s.y0 + m + 0.06f * u, 0.06f * u};
    l.c[MenuBtn] = {mid - 0.09f * u, s.y0 + m + 0.06f * u, 0.045f * u};
    const float dx = s.x0 + m + 0.36f * u, dy = s.y0 + m + 0.17f * u, ds = 0.075f * u, dr = 0.042f * u;
    l.c[DUp] = {dx, dy - ds, dr};
    l.c[DDown] = {dx, dy + ds, dr};
    l.c[DLeft] = {dx - ds, dy, dr};
    l.c[DRight] = {dx + ds, dy, dr};
    return l;
}

struct Finger {
    SDL_FingerID id;
    int ctl;         // a stick, a button, or -1 (a touch on nothing)
    float x, y;      // where it is (screen points)
    float cx, cy;    // a stick: where its middle is (where the thumb landed)
};

#ifdef __ANDROID__
constexpr bool kPlatform = true;
#else
constexpr bool kPlatform = false;
#endif
bool s_enabled = false, s_forced = false, s_visible = false;
float s_w = 0.f, s_h = 0.f;  // screen size in ImGui's points (the last frame's)
std::vector<Finger> s_fingers;
bool s_closedMenu = false;

bool inBar(const Bar& b, float x, float y) {
    return std::abs(x - b.x) <= b.hw * 1.2f && std::abs(y - b.y) <= b.hh * 1.1f;
}

int hit(const Layout& l, float x, float y) {
    if (inBar(l.barL, x, y)) return L;
    if (inBar(l.barR, x, y)) return R;
    int best = -1;
    float bestD = 1e9f;
    for (int i = A; i < kCtlCount; i++) {
        if (i == L || i == R) continue;
        const Circle& c = l.c[i];
        const float d = std::hypot(x - c.x, y - c.y);
        if (d <= c.r * 1.3f && d / c.r < bestD) best = i, bestD = d / c.r;
    }
    return best;
}

// how far up its bar a finger is: 0 at the bottom, 1 at the top
float barLevel(const Bar& b, float y) { return std::clamp((b.y + b.hh - y) / (2.f * b.hh), 0.f, 1.f); }
constexpr float kBarClick = 0.85f;  // from here up: the trigger's click

void screenSize() {
    if (s_w > 0.f) return;
    int w = 0, h = 0;
    if (SDL_Window* win = aurora_get_window()) SDL_GetWindowSize(win, &w, &h);
    s_w = float(w), s_h = float(h);
}

// the part of the screen clear of the notch, the rounded corners and the home indicator
Area safeArea() {
    Area s{0.f, 0.f, s_w, s_h};
    SDL_Rect r{};
    int w = 0, h = 0;
    SDL_Window* win = aurora_get_window();
    if (win && SDL_GetWindowSize(win, &w, &h) && w > 0 && h > 0 && SDL_GetWindowSafeArea(win, &r) && r.w > 0 && r.h > 0) {
        const float kx = s_w / float(w), ky = s_h / float(h);
        s = {float(r.x) * kx, float(r.y) * ky, float(r.x + r.w) * kx, float(r.y + r.h) * ky};
    }
    return s;
}

// the controls show while no controller is connected
void updateVisible() {
    s_visible = s_forced || !SDL_HasGamepad();
    if (!s_visible) s_fingers.clear();
}

void openMenu() {
    // the menu, as the back button: menuEvent's own path
    SDL_Event k{};
    k.type = SDL_EVENT_KEY_DOWN;
    k.key.key = SDLK_AC_BACK;
    menuEvent(&k);
}

}  // namespace

bool touchMenuButton(float nx, float ny) {
    if (!s_enabled || !s_visible || launcherShowing()) return false;
    screenSize();
    const Layout l = layout(safeArea());
    s_closedMenu = hit(l, nx * s_w, ny * s_h) == MenuBtn;
    return s_closedMenu;
}

void touchEvent(const void* p) {
    static bool init = false;
    if (!init) {
        init = true;
        s_forced = envFlag("R2_TOUCH");
        s_enabled = kPlatform || s_forced;
    }
    if (!s_enabled) return;
    const SDL_Event& e = *static_cast<const SDL_Event*>(p);
    updateVisible();
    if (!s_visible) return;
    switch (e.type) {
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        break;
    default:
        return;
    }
    if (s_closedMenu) {  // this touch closed the menu (menuEvent, just before)
        s_closedMenu = false;
        return;
    }
    if (menuOpen() || launcherShowing()) {
        s_fingers.clear();
        return;
    }
    screenSize();
    const SDL_TouchFingerEvent& t = e.tfinger;
    const float x = t.x * s_w, y = t.y * s_h;
    auto it = std::find_if(s_fingers.begin(), s_fingers.end(), [&](const Finger& f) { return f.id == t.fingerID; });
    if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
        if (it != s_fingers.end()) s_fingers.erase(it);
        return;
    }
    const Area safe = safeArea();
    const Layout l = layout(safe);
    if (e.type == SDL_EVENT_FINGER_DOWN) {
        Finger f{t.fingerID, hit(l, x, y), x, y, 0.f, 0.f};
        if (f.ctl == MenuBtn) {
            openMenu();
            s_fingers.clear();
            return;
        }
        // off the buttons, below the top row: the left half is the stick, the right half the
        // C-stick, each centered where the thumb lands
        const float mid = (safe.x0 + safe.x1) * 0.5f, top = safe.y0 + (safe.y1 - safe.y0) * 0.45f;
        if (f.ctl < 0 && y > top) {
            f.ctl = x < mid ? Stick : CStick;
            const float r = l.c[f.ctl].r * 0.6f;
            f.cx = x < mid ? std::clamp(x, safe.x0 + r, mid) : std::clamp(x, mid, safe.x1 - r);
            f.cy = std::clamp(y, top, safe.y1 - r);
        }
        if (it != s_fingers.end()) *it = f;
        else s_fingers.push_back(f);
        return;
    }
    if (it == s_fingers.end()) return;  // motion
    it->x = x, it->y = y;
    if (it->ctl != Stick && it->ctl != CStick && it->ctl != L && it->ctl != R) {
        const int now = hit(l, x, y);  // a thumb slides from one button to the next
        if (now != MenuBtn && now != L && now != R) it->ctl = now;
    }
}

bool touchApply(void* padStatus) {
    if (!s_enabled || !s_visible || menuOpen()) return false;
    auto& st = *static_cast<PADStatus*>(padStatus);
    st.err = PAD_ERR_NONE;  // (the game asks for a controller on port 1 otherwise)
    screenSize();
    const Layout l = layout(safeArea());
    for (const Finger& f : s_fingers) {
        if (f.ctl == Stick || f.ctl == CStick) {
            const float r = l.c[f.ctl].r;
            float dx = (f.x - f.cx) / r, dy = (f.y - f.cy) / r;
            const float len = std::hypot(dx, dy);
            if (len > 1.f) dx /= len, dy /= len;
            (f.ctl == Stick ? st.stickX : st.substickX) = s8(std::lround(dx * 127.f));
            (f.ctl == Stick ? st.stickY : st.substickY) = s8(std::lround(-dy * 127.f));
        } else if (f.ctl == L || f.ctl == R) {
            // the game reads a trigger from 30 (nothing) to 170 (full); the click adds the boost / full brake
            const float level = barLevel(f.ctl == L ? l.barL : l.barR, f.y);
            const bool click = level >= kBarClick;
            const u8 v = click ? 255 : u8(std::lround(45.f + level / kBarClick * 125.f));
            (f.ctl == L ? st.triggerLeft : st.triggerRight) = v;
            if (click) st.button |= kBits[f.ctl];
        } else if (f.ctl > CStick && f.ctl < kCtlCount) {
            st.button |= kBits[f.ctl];
        }
    }
    return true;
}

void touchDraw() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !ctx->WithinFrameScope) return;
    const ImGuiIO& io = ImGui::GetIO();
    s_w = io.DisplaySize.x, s_h = io.DisplaySize.y;
    if (s_enabled) updateVisible();
    if (!s_enabled || !s_visible || s_w <= 0.f || launcherShowing()) return;
    const Layout l = layout(safeArea());
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    auto menuButton = [&] {
        const Circle& c = l.c[MenuBtn];
        dl->AddCircleFilled(ImVec2(c.x, c.y), c.r, IM_COL32(255, 255, 255, 34), 40);
        dl->AddCircle(ImVec2(c.x, c.y), c.r, IM_COL32(255, 255, 255, 110), 40, std::max(1.5f, c.r * 0.05f));
        for (int k = -1; k <= 1; k++)
            dl->AddLine(ImVec2(c.x - c.r * 0.45f, c.y + k * c.r * 0.35f), ImVec2(c.x + c.r * 0.45f, c.y + k * c.r * 0.35f),
                        IM_COL32(255, 255, 255, 210), std::max(1.5f, c.r * 0.1f));
    };
    if (menuOpen()) {
        menuButton();
        return;
    }
    bool down[kCtlCount] = {};
    const Finger* held[kCtlCount] = {};
    for (const Finger& f : s_fingers)
        if (f.ctl >= 0 && f.ctl < kCtlCount) down[f.ctl] = true, held[f.ctl] = &f;
    ImFont* font = ImGui::GetFont();
    auto label = [&](const Circle& c, const char* s, float size, ImU32 col) {
        const ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0.f, s);
        dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, s);
    };
    auto button = [&](int i, ImU32 tint) {
        const Circle& c = l.c[i];
        const int fa = down[i] ? 110 : 34;
        dl->AddCircleFilled(ImVec2(c.x, c.y), c.r, (tint & 0x00FFFFFF) | (ImU32(fa) << 24), 40);
        dl->AddCircle(ImVec2(c.x, c.y), c.r, IM_COL32(255, 255, 255, down[i] ? 200 : 110), 40, std::max(1.5f, c.r * 0.05f));
    };
    // a stick: its ring where it is held (the C-stick's only then), the knob under the thumb
    auto stick = [&](int i, bool atRest) {
        const Finger* f = held[i];
        if (!f && !atRest) return;
        const float r = l.c[i].r, knob = l.knobR * r / l.stickR;
        const float cx = f ? f->cx : l.c[i].x, cy = f ? f->cy : l.c[i].y;
        dl->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(255, 255, 255, 22), 48);
        dl->AddCircle(ImVec2(cx, cy), r, IM_COL32(255, 255, 255, 90), 48, std::max(1.5f, r * 0.03f));
        float kx = cx, ky = cy;
        if (f) {
            float dx = f->x - cx, dy = f->y - cy;
            const float len = std::hypot(dx, dy);
            if (len > r) dx *= r / len, dy *= r / len;
            kx += dx, ky += dy;
        }
        dl->AddCircleFilled(ImVec2(kx, ky), knob, IM_COL32(255, 255, 255, f ? 120 : 60), 32);
    };
    stick(Stick, true);
    stick(CStick, false);
    // a trigger's bar: filled up to the finger, the click's part marked at the top
    auto bar = [&](int i, const Bar& b, ImU32 tint) {
        const ImVec2 p0(b.x - b.hw, b.y - b.hh), p1(b.x + b.hw, b.y + b.hh);
        const float round = b.hw * 0.5f, clickY = p1.y - kBarClick * 2.f * b.hh;
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 26), round);
        if (held[i]) {
            const float level = barLevel(b, held[i]->y);
            dl->AddRectFilled(ImVec2(p0.x, p1.y - level * 2.f * b.hh), p1, (tint & 0x00FFFFFF) | (ImU32(level >= kBarClick ? 170 : 110) << 24),
                              round);
        }
        dl->AddRect(p0, p1, IM_COL32(255, 255, 255, held[i] ? 200 : 110), round, 0, std::max(1.5f, b.hw * 0.05f));
        dl->AddLine(ImVec2(p0.x + b.hw * 0.3f, clickY), ImVec2(p1.x - b.hw * 0.3f, clickY), IM_COL32(255, 255, 255, 110),
                    std::max(1.f, b.hw * 0.04f));
        label({b.x, b.y + b.hh * 0.45f, b.hw}, kLabels[i], b.hw * 0.95f, IM_COL32(255, 255, 255, 200));
    };
    const ImU32 white = IM_COL32(255, 255, 255, 255), green = IM_COL32(40, 200, 140, 255), red = IM_COL32(230, 70, 70, 255),
                amber = IM_COL32(255, 180, 60, 255), blue = IM_COL32(90, 160, 255, 255);
    bar(L, l.barL, blue);
    bar(R, l.barR, amber);
    button(A, green);
    button(B, red);
    for (int i : {X, Y, Z, Start, DUp, DDown, DLeft, DRight}) button(i, white);
    for (int i : {A, B, X, Y, Z}) label(l.c[i], kLabels[i], l.c[i].r * 0.95f, IM_COL32(255, 255, 255, 200));
    label(l.c[Start], "START", l.c[Start].r * 0.55f, IM_COL32(255, 255, 255, 200));
    // d-pad arrows
    for (int i : {DUp, DDown, DLeft, DRight}) {
        const Circle& c = l.c[i];
        const float s = c.r * 0.45f;
        ImVec2 p[3];
        switch (i) {
        case DUp: p[0] = {c.x, c.y - s}, p[1] = {c.x + s, c.y + s * 0.6f}, p[2] = {c.x - s, c.y + s * 0.6f}; break;
        case DDown: p[0] = {c.x, c.y + s}, p[1] = {c.x - s, c.y - s * 0.6f}, p[2] = {c.x + s, c.y - s * 0.6f}; break;
        case DLeft: p[0] = {c.x - s, c.y}, p[1] = {c.x + s * 0.6f, c.y - s}, p[2] = {c.x + s * 0.6f, c.y + s}; break;
        default: p[0] = {c.x + s, c.y}, p[1] = {c.x - s * 0.6f, c.y + s}, p[2] = {c.x - s * 0.6f, c.y - s}; break;
        }
        dl->AddTriangleFilled(p[0], p[1], p[2], IM_COL32(255, 255, 255, 200));
    }
    menuButton();
}

}  // namespace rt::gfx
