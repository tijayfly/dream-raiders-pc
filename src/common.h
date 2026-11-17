// Dream Raiders PC port - shared declarations
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdint.h>
#include <string>

// ---------------------------------------------------------------- logging
void LogInit(const std::string& path);
void Log(const char* fmt, ...);
void LogHex(const char* tag, const void* data, size_t len);

// ---------------------------------------------------------------- paths
extern std::string g_gameDir;   // folder containing Game.exe (with trailing '\')
extern std::string g_portDir;   // g_gameDir + "port\"

// ---------------------------------------------------------------- config
enum class DisplayMode { Borderless, Windowed };
enum class AspectMode { Letterbox, Stretch };
enum class VSyncMode { Auto, On, Off };

// Which physical devices drive a player's gun
enum InputSrc : int { SRC_NONE = 0, SRC_MOUSE = 1, SRC_PAD1 = 2, SRC_PAD2 = 4, SRC_PAD3 = 8, SRC_PAD4 = 16 };

struct Config {
    // Display
    DisplayMode displayMode = DisplayMode::Borderless;
    int windowW = 1600, windowH = 900;
    int renderW = 0, renderH = 0;           // 0 = match viewport
    AspectMode aspect = AspectMode::Letterbox;
    VSyncMode vsync = VSyncMode::Auto;
    int fpsLimit = 60;
    int monitor = 0;                        // 0 = primary
    // Input
    int p1Src = SRC_MOUSE | SRC_PAD1;
    int p2Src = SRC_PAD2;
    float padSpeed = 1.0f;
    float padDeadzone = 0.20f;
    bool invertX = true, invertY = true; // the guns read both axes reversed
    int anP1X = 1, anP1Y = 0, anP2X = 3, anP2Y = 2; // the game reads Y on the even channels
    int triggerButton = 1;                  // JVS push-button number
    bool confineMouse = true;
    // Rumble
    bool rumble = true;
    float rumbleStrength = 0.6f;
    // Game
    bool freePlay = true;
    std::string region = "japan";
    std::string keychipId = "AAVE-01A12345678";
    bool showHint = true;
    bool resetSaveOnNextLaunch = false;
    bool shortcutCreated = false;           // the "Dream Raiders" shortcut is only made once
    bool seedCalibration = true;            // write exact corner calibration for both guns
    // Game options (-1 = leave the game's own setting alone)
    int optDifficulty = -1, optLanguage = -1, optSubtitles = -1, optContinue = -1, optContinueTime = -1,
        optSelectTime = -1, optAttractSound = -1;
    // Debug
    int jvsLogPackets = 40;                 // how many JVS packets to dump to the log
};
extern Config g_cfg;
void ConfigLoad();
void ConfigSave();

// ---------------------------------------------------------------- game layout
// Viewport (in window client coordinates) where the game picture lands
struct Viewport { int x, y, w, h; };
Viewport GetViewport();
void ComputeLayout();           // fills g_layout from config + monitor
struct Layout {
    RECT monitorRect;           // screen coords of the target monitor
    int clientW, clientH;       // window client size
    int renderW, renderH;       // backbuffer size
    int refreshHz;
};
extern Layout g_layout;
extern HWND g_hwnd;
extern HMODULE g_module;
void ApplyWindowIcon(HWND hwnd);
void InstallDesktopIcon();

// ---------------------------------------------------------------- modules
bool VerifyGameExe();
bool IsTestModeProcess();
void ApplyCodePatches();
void PokeGameState();           // free play (any thread)
void GameThreadFrame();         // per-frame upkeep on the game thread (calibration, options)
bool GameOptGet(const char* key, int& value);
bool GameOptSet(const char* key, int value);
int GameRegionIndex();
float BootProgress();
void InstallImportHooks();
void PrepareSaveFiles();
std::string RuntimeGameIniPath();
void BuildRuntimeGameIni();

// JVS
size_t JvsProcess(const uint8_t* req, size_t reqLen, uint8_t* out, size_t outCap);
bool JvsAddressed();
const uint8_t* JvsOutputs();    // last general purpose output bytes (4)

// Input
struct PlayerState {
    float x = 0.5f, y = 0.5f;   // 0..1 inside the game picture
    bool trigger = false, start = false, service = false;
    bool coinEdge = false;
    bool usingPad = false;      // last moved by a gamepad
    int padIndex = -1;
};
struct InputSnapshot {
    PlayerState p[2];
    bool test = false;
};
void InputPoll(InputSnapshot& out);
void InputFrame();              // per-frame upkeep (rumble timers)
void InputStopRumble();
bool InputHasFocus();

// Overlay
extern bool g_overlayOpen;
