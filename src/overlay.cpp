// Esc pause menu.
#include "common.h"
#include <algorithm>
#include <vector>
#include "imgui.h"

extern float g_uiScale;

static bool s_restartNote = false;

static bool SrcCombo(const char* label, int& src) {
    static const char* names[] = {"None", "Mouse", "Pad 1", "Pad 2", "Pad 3", "Pad 4", "Mouse + Pad 1"};
    static const int vals[] = {SRC_NONE, SRC_MOUSE, SRC_PAD1, SRC_PAD2, SRC_PAD3, SRC_PAD4, SRC_MOUSE | SRC_PAD1};
    int cur = 0;
    for (int i = 0; i < 7; i++)
        if (vals[i] == src) cur = i;
    if (ImGui::Combo(label, &cur, names, 7)) {
        src = vals[cur];
        return true;
    }
    return false;
}

// A game option backed by the game's own settings store. Changes apply from the next game started.
static void OptionCombo(const char* label, const char* key, int& cfgValue, const std::vector<std::pair<int, const char*>>& choices) {
    int v;
    if (!GameOptGet(key, v)) {
        ImGui::BeginDisabled();
        int dummy = 0;
        ImGui::Combo(label, &dummy, "(not available yet)\0");
        ImGui::EndDisabled();
        return;
    }
    int cur = -1;
    for (size_t i = 0; i < choices.size(); i++)
        if (choices[i].first == v) cur = (int)i;
    const char* preview = cur >= 0 ? choices[cur].second : "?";
    if (ImGui::BeginCombo(label, preview)) {
        for (size_t i = 0; i < choices.size(); i++) {
            bool sel = (int)i == cur;
            if (ImGui::Selectable(choices[i].second, sel)) {
                GameOptSet(key, choices[i].first);
                cfgValue = choices[i].first;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

static std::vector<std::pair<int, const char*>> Seconds(int from, int to, int step) {
    static char buf[16][8];
    std::vector<std::pair<int, const char*>> r;
    int i = 0;
    for (int s = from; s <= to && i < 16; s += step, i++) {
        snprintf(buf[i], sizeof(buf[i]), "%d s", s);
        r.push_back({s, buf[i]});
    }
    return r;
}

void DrawOverlayUI() {
    Viewport vp = GetViewport();
    // Fixed size for every tab so the window doesn't jump around when switching.
    float w = std::min(vp.w * 0.9f, 680.0f * g_uiScale);
    // Height = the tallest tab seen so far (starts at the Game tab's height), so all tabs match.
    static float s_contentH = 0.0f;
    if (s_contentH <= 0.0f) { // estimate the Game tab, the tallest: 9 controls, 3 text lines, 2 separators
        const ImGuiStyle& st = ImGui::GetStyle();
        s_contentH = st.WindowPadding.y * 2 + ImGui::GetFrameHeight() /*title*/ + ImGui::GetFrameHeightWithSpacing() /*tabs*/ +
                     9 * ImGui::GetFrameHeightWithSpacing() + 4 * ImGui::GetTextLineHeightWithSpacing() +
                     2 * st.ItemSpacing.y * 2;
    }
    float h = std::min(vp.h * 0.9f, s_contentH);
    ImGui::SetNextWindowPos(ImVec2(vp.x + vp.w * 0.5f, vp.y + vp.h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (!ImGui::Begin("Dream Raiders  -  Paused", &g_overlayOpen,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        ImGui::End();
        return;
    }
    Config& c = g_cfg;
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Pause Menu")) {
            ImGui::Spacing();
            float bw = ImGui::GetContentRegionAvail().x;
            if (ImGui::Button("Resume", ImVec2(bw, 0))) g_overlayOpen = false;
            ImGui::Spacing();
            if (ImGui::Button("Quit to desktop", ImVec2(bw, 0))) {
                ConfigSave();
                InputStopRumble();
                Log("quit from menu");
                TerminateProcess(GetCurrentProcess(), 0);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            SrcCombo("Player 1", c.p1Src);
            SrcCombo("Player 2", c.p2Src);
            ImGui::SliderFloat("Gamepad aim speed", &c.padSpeed, 0.3f, 3.0f, "%.2f");
            ImGui::SliderFloat("Stick deadzone", &c.padDeadzone, 0.05f, 0.5f, "%.2f");
            ImGui::Checkbox("Invert X", &c.invertX);
            ImGui::SameLine();
            ImGui::Checkbox("Invert Y", &c.invertY);
            if (c.displayMode == DisplayMode::Windowed) ImGui::Checkbox("Keep mouse inside window", &c.confineMouse);
            ImGui::Separator();
            ImGui::TextDisabled("Mouse: left = fire, Enter = start, 5/6 = coin");
            ImGui::TextDisabled("Pad: RT/A = fire, Start = start, View/Back = coin, R3 = recentre");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            int mode = (int)c.displayMode;
            if (ImGui::Combo("Mode", &mode, "Borderless fullscreen\0Window\0")) {
                c.displayMode = (DisplayMode)mode;
                s_restartNote = true;
            }
            if (c.displayMode == DisplayMode::Windowed) {
                if (ImGui::InputInt("Window width", &c.windowW, 16, 160)) s_restartNote = true;
                if (ImGui::InputInt("Window height", &c.windowH, 9, 90)) s_restartNote = true;
                c.windowW = std::max(320, c.windowW);
                c.windowH = std::max(180, c.windowH);
            }
            static const char* resNames[] = {"Match screen", "3840 x 2160", "2560 x 1440", "1920 x 1080", "1280 x 720 (original)"};
            static const int resW[] = {0, 3840, 2560, 1920, 1280}, resH[] = {0, 2160, 1440, 1080, 720};
            int cur = 0;
            for (int i = 0; i < 5; i++)
                if (c.renderW == resW[i] && c.renderH == resH[i]) cur = i;
            if (ImGui::Combo("Render resolution", &cur, resNames, 5)) {
                c.renderW = resW[cur];
                c.renderH = resH[cur];
                s_restartNote = true;
            }
            int asp = (int)c.aspect;
            if (ImGui::Combo("Aspect", &asp, "16:9 with black bars\0Stretch to fill\0")) {
                c.aspect = (AspectMode)asp;
                s_restartNote = true;
            }
            int vs = (int)c.vsync;
            if (ImGui::Combo("V-sync", &vs, "Auto (on for 60 Hz screens)\0On\0Off\0")) {
                c.vsync = (VSyncMode)vs;
                s_restartNote = true;
            }
            ImGui::TextDisabled("Screen: %d Hz. The game runs at a fixed 60 fps.", g_layout.refreshHz);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rumble")) {
            ImGui::Checkbox("Rumble while firing", &c.rumble);
            ImGui::SliderFloat("Strength", &c.rumbleStrength, 0.1f, 1.0f, "%.2f");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
            OptionCombo("Difficulty", "gdiff", c.optDifficulty,
                        {{0, "Easiest"}, {1, "Very easy"}, {2, "Easy"}, {3, "Normal"}, {4, "Hard"}, {5, "Very hard"}, {6, "Hardest"}});
            {
                // Only the languages the game allows for its region
                static const char* langNames[] = {"English", "Japanese", "Simplified Chinese", "Spanish",
                                                  "Russian", "Italian",  "French",             "Turkish"};
                std::vector<std::pair<int, const char*>> langs;
                int region = GameRegionIndex();
                if (region >= 0 && region < 6) {
                    int* list = *(int**)(0x805988 + region * 8);
                    int n = *(int*)(0x80598C + region * 8);
                    for (int i = 0; i < n && i < 8; i++)
                        if (list[i] >= 0 && list[i] < 8) langs.push_back({list[i], langNames[list[i]]});
                }
                OptionCombo("Language", "glng", c.optLanguage, langs);
            }
            OptionCombo("Subtitles", "sbtl", c.optSubtitles, {{0, "Off"}, {1, "On"}});
            OptionCombo("Continues", "gct", c.optContinue, {{0, "Off"}, {1, "On"}});
            OptionCombo("Continue timer", "gctt", c.optContinueTime, Seconds(10, 30, 5));
            OptionCombo("Dream select timer", "gsst", c.optSelectTime, Seconds(10, 60, 5));
            OptionCombo("Attract mode sound", "gss", c.optAttractSound, {{0, "Off"}, {1, "On"}});
            ImGui::TextDisabled("Takes effect from the next game you start.");
            ImGui::Separator();
            ImGui::Checkbox("Free play", &c.freePlay);
            ImGui::Separator();
            if (ImGui::Checkbox("Reset scores and settings on next launch", &c.resetSaveOnNextLaunch)) s_restartNote = true;
            ImGui::TextDisabled("Old save files are kept as .bak in the port folder.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (s_restartNote) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Some changes apply next time you start the game.");
    ImGui::TextDisabled("Esc resumes. Settings save automatically.");
    float needed = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
    if (needed > s_contentH) s_contentH = needed;
    ImGui::End();
}
