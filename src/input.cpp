// Mouse / keyboard / XInput -> two light-gun players, plus gamepad rumble.
#include "common.h"
#include <xinput.h>
#include <math.h>
#include <algorithm>
#include <mutex>

typedef DWORD(WINAPI* XInputGetState_t)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* XInputSetState_t)(DWORD, XINPUT_VIBRATION*);
static XInputGetState_t s_xget;
static XInputSetState_t s_xset;
static bool s_xtried = false;

static void LoadXInput() {
    if (s_xtried) return;
    s_xtried = true;
    const char* dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (auto d : dlls) {
        HMODULE m = LoadLibraryA(d);
        if (!m) continue;
        s_xget = (XInputGetState_t)GetProcAddress(m, "XInputGetState");
        s_xset = (XInputSetState_t)GetProcAddress(m, "XInputSetState");
        if (s_xget) {
            Log("using %s for gamepads", d);
            return;
        }
    }
    Log("XInput not available, gamepads disabled");
}

struct PadState {
    bool connected = false;
    XINPUT_STATE st{};
    WORD prevButtons = 0;
    uint8_t prevLT = 0, prevRT = 0;
    DWORD lastCheck = 0;
    // rumble
    float motorL = -1, motorR = -1;
};
static PadState s_pads[4];
static std::mutex s_lock;

struct PlayerCursor {
    float x = 0.5f, y = 0.5f;
    bool usingPad = false;
    int lastMouseX = INT_MIN, lastMouseY = INT_MIN;
    bool prevCoin = false;
    bool triggerHeld = false;
    int padForRumble = -1;
};
static PlayerCursor s_cur[2];
static LARGE_INTEGER s_freq, s_last;

bool InputHasFocus() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

static bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static float Deadzone(float v, float dz) {
    float a = fabsf(v);
    if (a < dz) return 0.0f;
    float n = (a - dz) / (1.0f - dz);
    n = n * n; // finer control near the centre
    return v < 0 ? -n : n;
}

static void PollPads() {
    LoadXInput();
    if (!s_xget) return;
    DWORD now = GetTickCount();
    for (int i = 0; i < 4; i++) {
        PadState& p = s_pads[i];
        // Probing a disconnected pad is slow; only retry every 2 s.
        if (!p.connected && now - p.lastCheck < 2000 && p.lastCheck) continue;
        p.lastCheck = now;
        p.prevButtons = p.st.Gamepad.wButtons;
        p.prevLT = p.st.Gamepad.bLeftTrigger;
        p.prevRT = p.st.Gamepad.bRightTrigger;
        XINPUT_STATE st{};
        bool was = p.connected;
        p.connected = s_xget(i, &st) == ERROR_SUCCESS;
        if (p.connected) p.st = st;
        else memset(&p.st, 0, sizeof(p.st));
        if (p.connected != was) Log("gamepad %d %s", i + 1, p.connected ? "connected" : "disconnected");
    }
}

static void SetMotor(int pad, float l, float r) {
    if (!s_xset || pad < 0 || pad > 3 || !s_pads[pad].connected) return;
    PadState& p = s_pads[pad];
    if (fabsf(p.motorL - l) < 0.01f && fabsf(p.motorR - r) < 0.01f) return;
    p.motorL = l;
    p.motorR = r;
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = (WORD)(std::min(1.0f, l) * 65535);
    v.wRightMotorSpeed = (WORD)(std::min(1.0f, r) * 65535);
    s_xset(pad, &v);
}

void InputStopRumble() {
    for (int i = 0; i < 4; i++) SetMotor(i, 0, 0);
}

static int PadIndexOf(int src) {
    for (int i = 0; i < 4; i++)
        if (src & (SRC_PAD1 << i)) return i;
    return -1;
}

void InputPoll(InputSnapshot& o) {
    std::lock_guard<std::mutex> l(s_lock);
    if (!s_freq.QuadPart) {
        QueryPerformanceFrequency(&s_freq);
        QueryPerformanceCounter(&s_last);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)(now.QuadPart - s_last.QuadPart) / (float)s_freq.QuadPart;
    s_last = now;
    dt = std::min(dt, 0.1f);

    PollPads();
    bool focus = InputHasFocus() && !g_overlayOpen;
    Viewport vp = GetViewport();

    POINT mp{0, 0};
    bool mouseOk = g_hwnd && GetCursorPos(&mp) && ScreenToClient(g_hwnd, &mp);

    o = InputSnapshot();
    o.test = false; // the arcade test menu is not used on PC

    for (int pl = 0; pl < 2; pl++) {
        int src = pl == 0 ? g_cfg.p1Src : g_cfg.p2Src;
        PlayerCursor& c = s_cur[pl];
        PlayerState& ps = o.p[pl];
        bool trig = false, start = false, coin = false;

        if ((src & SRC_MOUSE) && mouseOk && vp.w > 0 && vp.h > 0) {
            if (mp.x != c.lastMouseX || mp.y != c.lastMouseY) {
                if (c.lastMouseX != INT_MIN) c.usingPad = false;
                c.lastMouseX = mp.x;
                c.lastMouseY = mp.y;
                if (!c.usingPad && focus) {
                    c.x = std::min(1.0f, std::max(0.0f, (mp.x - vp.x) / (float)vp.w));
                    c.y = std::min(1.0f, std::max(0.0f, (mp.y - vp.y) / (float)vp.h));
                }
            }
            if (focus) {
                trig |= Key(VK_LBUTTON);
                start |= Key(VK_RETURN);
                coin |= Key(pl == 0 ? '5' : '6');
            }
        }
        int pad = PadIndexOf(src);
        c.padForRumble = pad;
        if (pad >= 0 && s_pads[pad].connected) {
            const XINPUT_GAMEPAD& g = s_pads[pad].st.Gamepad;
            float rx = Deadzone(g.sThumbRX / 32767.0f, g_cfg.padDeadzone);
            float ry = Deadzone(g.sThumbRY / 32767.0f, g_cfg.padDeadzone);
            float lx = Deadzone(g.sThumbLX / 32767.0f, g_cfg.padDeadzone);
            float ly = Deadzone(g.sThumbLY / 32767.0f, g_cfg.padDeadzone);
            float sx = fabsf(rx) > fabsf(lx) ? rx : lx;
            float sy = fabsf(ry) > fabsf(ly) ? ry : ly;
            if (sx != 0 || sy != 0) {
                c.usingPad = true;
                float speed = 0.9f * g_cfg.padSpeed; // picture widths per second at full deflection
                c.x = std::min(1.0f, std::max(0.0f, c.x + sx * speed * dt * (9.0f / 16.0f)));
                c.y = std::min(1.0f, std::max(0.0f, c.y - sy * speed * dt));
            }
            if (!g_overlayOpen) {
                trig |= g.bRightTrigger > 60 || (g.wButtons & XINPUT_GAMEPAD_A);
                start |= (g.wButtons & XINPUT_GAMEPAD_START) != 0;
                coin |= (g.wButtons & XINPUT_GAMEPAD_BACK) != 0;
                if (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) { c.x = 0.5f; c.y = 0.5f; } // recentre
            }
        }
        ps.x = c.x;
        ps.y = c.y;
        ps.trigger = trig;
        ps.start = start;
        ps.coinEdge = coin && !c.prevCoin;
        c.prevCoin = coin;
        ps.usingPad = c.usingPad;
        ps.padIndex = pad;
        c.triggerHeld = trig;
    }
}

// Rumble: a recoil-like pulse train while the trigger is held (guns are automatic).
void InputFrame() {
    std::lock_guard<std::mutex> l(s_lock);
    bool active = g_cfg.rumble && InputHasFocus() && !g_overlayOpen;
    DWORD t = GetTickCount();
    bool want[4] = {false, false, false, false};
    float str[4] = {0, 0, 0, 0};
    for (int pl = 0; pl < 2; pl++) {
        int pad = s_cur[pl].padForRumble;
        if (pad < 0 || pad > 3) continue;
        if (active && s_cur[pl].triggerHeld) {
            bool on = (t % 110) < 55;
            want[pad] = true;
            str[pad] = std::max(str[pad], on ? g_cfg.rumbleStrength : g_cfg.rumbleStrength * 0.15f);
        }
    }
    for (int i = 0; i < 4; i++) {
        if (want[i]) SetMotor(i, str[i] * 0.5f, str[i]);
        else SetMotor(i, 0, 0);
    }
}
