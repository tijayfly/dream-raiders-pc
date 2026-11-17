#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

Config g_cfg;

static std::string IniPath() { return g_portDir + "DreamRaidersPort.ini"; }

static std::string GetStr(const char* sec, const char* key, const std::string& def) {
    char buf[256];
    GetPrivateProfileStringA(sec, key, def.c_str(), buf, sizeof(buf), IniPath().c_str());
    // strip inline comments
    std::string s = buf;
    size_t c = s.find(';');
    if (c != std::string::npos) s = s.substr(0, c);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}
static int GetInt(const char* sec, const char* key, int def) {
    std::string s = GetStr(sec, key, std::to_string(def));
    return (int)strtol(s.c_str(), nullptr, 0);
}
static float GetFloat(const char* sec, const char* key, float def) {
    char d[32];
    snprintf(d, sizeof(d), "%.3f", def);
    return (float)atof(GetStr(sec, key, d).c_str());
}
static bool GetBool(const char* sec, const char* key, bool def) { return GetInt(sec, key, def ? 1 : 0) != 0; }
static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

static int ParseSrc(const std::string& v) {
    std::string s = Lower(v);
    int r = 0;
    if (s.find("mouse") != std::string::npos) r |= SRC_MOUSE;
    if (s.find("pad1") != std::string::npos) r |= SRC_PAD1;
    if (s.find("pad2") != std::string::npos) r |= SRC_PAD2;
    if (s.find("pad3") != std::string::npos) r |= SRC_PAD3;
    if (s.find("pad4") != std::string::npos) r |= SRC_PAD4;
    return r;
}
static std::string SrcToStr(int v) {
    std::string s;
    auto add = [&](const char* n) { if (!s.empty()) s += "+"; s += n; };
    if (v & SRC_MOUSE) add("mouse");
    if (v & SRC_PAD1) add("pad1");
    if (v & SRC_PAD2) add("pad2");
    if (v & SRC_PAD3) add("pad3");
    if (v & SRC_PAD4) add("pad4");
    if (s.empty()) s = "none";
    return s;
}

void ConfigLoad() {
    Config& c = g_cfg;
    std::string m = Lower(GetStr("Display", "Mode", "borderless"));
    c.displayMode = (m == "windowed") ? DisplayMode::Windowed : DisplayMode::Borderless;
    c.windowW = GetInt("Display", "WindowWidth", c.windowW);
    c.windowH = GetInt("Display", "WindowHeight", c.windowH);
    c.renderW = GetInt("Display", "RenderWidth", 0);
    c.renderH = GetInt("Display", "RenderHeight", 0);
    c.aspect = Lower(GetStr("Display", "Aspect", "letterbox")) == "stretch" ? AspectMode::Stretch : AspectMode::Letterbox;
    std::string vs = Lower(GetStr("Display", "VSync", "auto"));
    c.vsync = vs == "on" ? VSyncMode::On : vs == "off" ? VSyncMode::Off : VSyncMode::Auto;
    c.fpsLimit = std::max(0, GetInt("Display", "FpsLimit", 60));
    c.monitor = GetInt("Display", "Monitor", 0);

    c.p1Src = ParseSrc(GetStr("Input", "Player1", "mouse+pad1"));
    c.p2Src = ParseSrc(GetStr("Input", "Player2", "pad2"));
    c.padSpeed = GetFloat("Input", "PadSpeed", 1.0f);
    c.padDeadzone = GetFloat("Input", "PadDeadzone", 0.20f);
    c.invertX = GetBool("Input", "InvertX", true);
    c.invertY = GetBool("Input", "InvertY", true);
    c.anP1X = GetInt("Input", "AnalogP1X", 1);
    c.anP1Y = GetInt("Input", "AnalogP1Y", 0);
    c.anP2X = GetInt("Input", "AnalogP2X", 3);
    c.anP2Y = GetInt("Input", "AnalogP2Y", 2);
    c.triggerButton = GetInt("Input", "TriggerButton", 1);
    c.confineMouse = GetBool("Input", "ConfineMouse", true);

    c.rumble = GetBool("Rumble", "Enabled", true);
    c.rumbleStrength = GetFloat("Rumble", "Strength", 0.6f);

    c.freePlay = GetBool("Game", "FreePlay", true);
    c.showHint = GetBool("Game", "ShowHint", true);
    c.resetSaveOnNextLaunch = GetBool("Game", "ResetSaveOnNextLaunch", false);
    c.seedCalibration = GetBool("Input", "SeedCalibration", true);
    c.shortcutCreated = GetBool("Game", "ShortcutCreated", false);
    c.optDifficulty = GetInt("GameOptions", "Difficulty", -1);
    c.optLanguage = GetInt("GameOptions", "Language", -1);
    c.optSubtitles = GetInt("GameOptions", "Subtitles", -1);
    c.optContinue = GetInt("GameOptions", "Continue", -1);
    c.optContinueTime = GetInt("GameOptions", "ContinueTime", -1);
    c.optSelectTime = GetInt("GameOptions", "SelectTime", -1);
    c.optAttractSound = GetInt("GameOptions", "AttractSound", -1);

    c.jvsLogPackets = GetInt("Debug", "JvsLogPackets", 40);
}

static void Put(const char* sec, const char* key, const std::string& v) {
    WritePrivateProfileStringA(sec, key, v.c_str(), IniPath().c_str());
}
static void PutF(const char* sec, const char* key, float v) {
    char b[32];
    snprintf(b, sizeof(b), "%.2f", v);
    Put(sec, key, b);
}

void ConfigSave() {
    const Config& c = g_cfg;
    Put("Display", "Mode", c.displayMode == DisplayMode::Windowed ? "windowed" : "borderless");
    Put("Display", "WindowWidth", std::to_string(c.windowW));
    Put("Display", "WindowHeight", std::to_string(c.windowH));
    Put("Display", "RenderWidth", std::to_string(c.renderW));
    Put("Display", "RenderHeight", std::to_string(c.renderH));
    Put("Display", "Aspect", c.aspect == AspectMode::Stretch ? "stretch" : "letterbox");
    Put("Display", "VSync", c.vsync == VSyncMode::On ? "on" : c.vsync == VSyncMode::Off ? "off" : "auto");
    Put("Display", "FpsLimit", std::to_string(c.fpsLimit));
    Put("Display", "Monitor", std::to_string(c.monitor));

    Put("Input", "Player1", SrcToStr(c.p1Src));
    Put("Input", "Player2", SrcToStr(c.p2Src));
    PutF("Input", "PadSpeed", c.padSpeed);
    PutF("Input", "PadDeadzone", c.padDeadzone);
    Put("Input", "InvertX", c.invertX ? "1" : "0");
    Put("Input", "InvertY", c.invertY ? "1" : "0");
    Put("Input", "AnalogP1X", std::to_string(c.anP1X));
    Put("Input", "AnalogP1Y", std::to_string(c.anP1Y));
    Put("Input", "AnalogP2X", std::to_string(c.anP2X));
    Put("Input", "AnalogP2Y", std::to_string(c.anP2Y));
    Put("Input", "TriggerButton", std::to_string(c.triggerButton));
    Put("Input", "ConfineMouse", c.confineMouse ? "1" : "0");

    Put("Rumble", "Enabled", c.rumble ? "1" : "0");
    PutF("Rumble", "Strength", c.rumbleStrength);

    Put("Game", "FreePlay", c.freePlay ? "1" : "0");
    Put("Game", "ShowHint", c.showHint ? "1" : "0");
    Put("Game", "ResetSaveOnNextLaunch", c.resetSaveOnNextLaunch ? "1" : "0");
    Put("Input", "SeedCalibration", c.seedCalibration ? "1" : "0");
    Put("Game", "ShortcutCreated", c.shortcutCreated ? "1" : "0");
    Put("GameOptions", "Difficulty", std::to_string(c.optDifficulty));
    Put("GameOptions", "Language", std::to_string(c.optLanguage));
    Put("GameOptions", "Subtitles", std::to_string(c.optSubtitles));
    Put("GameOptions", "Continue", std::to_string(c.optContinue));
    Put("GameOptions", "ContinueTime", std::to_string(c.optContinueTime));
    Put("GameOptions", "SelectTime", std::to_string(c.optSelectTime));
    Put("GameOptions", "AttractSound", std::to_string(c.optAttractSound));

    Put("Debug", "JvsLogPackets", std::to_string(c.jvsLogPackets));
}
