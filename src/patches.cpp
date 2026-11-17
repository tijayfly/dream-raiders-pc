// Game-specific knowledge for Game.exe (Dream Raiders, SBXM, build Aug 1 2012).
// Replaces the keychip / platform / process-master calls of SEGA amLib with local stand-ins,
// prepares backup memory images and generates the runtime Game.ini.
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <vector>
#include <algorithm>

static const DWORD kExpectedTimestamp = 0x50194FC2;
static const DWORD kExpectedImageSize = 53555200;
static bool s_verified = false;

bool VerifyGameExe() {
    auto base = (uint8_t*)GetModuleHandleA(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    s_verified = nt->FileHeader.TimeDateStamp == kExpectedTimestamp &&
                 nt->OptionalHeader.SizeOfImage == kExpectedImageSize &&
                 nt->OptionalHeader.ImageBase == 0x400000 && (uintptr_t)base == 0x400000;
    Log("Game.exe timestamp %08lX size %lu base %p -> %s", nt->FileHeader.TimeDateStamp,
        nt->OptionalHeader.SizeOfImage, base, s_verified ? "recognised" : "NOT RECOGNISED");
    return s_verified;
}

// ---------------------------------------------------------------- helpers
static bool WriteCode(uintptr_t addr, const void* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)addr, bytes, n);
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    return true;
}
static bool JmpTo(uintptr_t at, const void* target, const char* expect, size_t expLen, const char* name) {
    if (memcmp((void*)at, expect, expLen) != 0) {
        Log("patch %s @%08X: unexpected bytes, skipped", name, (unsigned)at);
        return false;
    }
    uint8_t j[5] = {0xE9};
    int32_t rel = (int32_t)((uintptr_t)target - (at + 5));
    memcpy(j + 1, &rel, 4);
    bool ok = WriteCode(at, j, 5);
    Log("patch %s @%08X -> %s", name, (unsigned)at, ok ? "ok" : "FAILED");
    return ok;
}

// ---------------------------------------------------------------- amLib log capture
// amLib's internal printer: void(const char* func, int line, const char* fmt, va_list)
static void __cdecl Hook_AmLibPrint(const char* func, int line, const char* fmt, va_list ap) {
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    Log("amLib %s:%d %s", func ? func : "?", line, msg);
}

// ---------------------------------------------------------------- platform
// amPlatformGetBoardType(int* out): RingWide = platform "AAL" / maker "NEC" = board type 0.
static int __stdcall Hook_GetBoardType(int* out) {
    if (!out) return -2;
    *out = 0;
    return 0;
}

// ---------------------------------------------------------------- keychip (amDongle)
static uint8_t* DongleState() { return *(uint8_t**)0x7BE2B8; }

static int __stdcall Hook_DongleLoadTable(const char*) { return 0; }

static int __cdecl Hook_DongleSetupKeychip() {
    uint8_t* d = DongleState();
    if (d) {
        *(int*)(d + 0x04) = 1;   // opened
        *(int*)(d + 0x08) = 1;   // keychip present
        *(int*)(d + 0x10) = 1;   // table loaded
        *(int*)(d + 0x44) = 0;   // last result ok
    }
    Log("keychip: emulated (id %s, region %s)", g_cfg.keychipId.c_str(), g_cfg.region.c_str());
    return 0;
}
static int __cdecl Hook_DongleUpdate() { return 0; }

static int RegionBits() {
    const std::string& r = g_cfg.region;
    if (r == "usa" || r == "us") return 2;
    if (r == "export" || r == "exp") return 4;
    if (r == "china" || r == "cn") return 8;
    return 1; // japan
}
static int __stdcall Hook_DongleGetRegion(int* out, int) {
    if (out) *out = RegionBits();
    return 0;
}
static int __stdcall Hook_DongleGetKeyId(char* out, int) {
    if (out) {
        memset(out, 0, 17);
        strncpy(out, g_cfg.keychipId.c_str(), 16);
    }
    return 0;
}

// ---------------------------------------------------------------- process master (amMaster)
bool IsTestModeProcess() {
    // Gametest.bat starts the game as "game.exe T"
    const char* cl = GetCommandLineA();
    const char* p = cl;
    if (*p == '"') { p = strchr(p + 1, '"'); p = p ? p + 1 : cl + strlen(cl); }
    else while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    return (p[0] == 'T' || p[0] == 't') && (p[1] == 0 || p[1] == ' ');
}
static int __stdcall Hook_MasterGetProcessCount(int* out, int, int) {
    if (out) *out = 1;
    return 0;
}
// The game calls this while shutting down. next == 3 means "switch mode": on the cabinet the process
// master then relaunches Game.exe in test mode ("Game.exe T") or back in game mode. Do that ourselves.
static bool s_relaunched = false;
static int __stdcall Hook_MasterExit(int next, int arg) {
    if (next == 3 && !s_relaunched) {
        s_relaunched = true;
        bool inTest = IsTestModeProcess();
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string cmd = std::string("\"") + exe + "\"" + (inTest ? "" : " T");
        // The game will not start while another copy is still running, so the new copy waits for us to exit.
        char pid[16];
        snprintf(pid, sizeof(pid), "%lu", GetCurrentProcessId());
        SetEnvironmentVariableA("DRPORT_WAIT_PID", pid);
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessA(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, g_gameDir.c_str(), &si, &pi);
        Log("mode switch: relaunching in %s mode -> %s", inTest ? "game" : "test", ok ? "ok" : "FAILED");
        if (ok) {
            AllowSetForegroundWindow(pi.dwProcessId); // let the new copy take focus
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    } else {
        Log("amMasterExitA(%d, %d)", next, arg);
    }
    return 0;
}

// ---------------------------------------------------------------- 720-line assumptions
// Some full-screen 2D art (1024x720 textures) is scaled by width/1024 horizontally but by a fixed 1.0
// vertically, which is only right on the cabinet's 720-line screen. Feed it height/720 instead.
static float s_scaleY720 = 1.0f;
static void PatchFixedYScale(uintptr_t at, const char* name) {
    // at: "fld1 ; cvtsi2ss xmm0, dword ptr [0xBD827C]"  (10 bytes)
    static const uint8_t expect[] = {0xD9, 0xE8, 0xF3, 0x0F, 0x2A, 0x05, 0x7C, 0x82, 0xBD, 0x00};
    if (memcmp((void*)at, expect, sizeof(expect)) != 0) {
        Log("patch %s @%08X: unexpected bytes, skipped", name, (unsigned)at);
        return;
    }
    uint8_t* stub = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return;
    size_t n = 0;
    stub[n++] = 0xD9; stub[n++] = 0x05; // fld dword ptr [s_scaleY720]
    uintptr_t a = (uintptr_t)&s_scaleY720;
    memcpy(stub + n, &a, 4); n += 4;
    memcpy(stub + n, expect + 2, 8); n += 8; // the original cvtsi2ss
    stub[n++] = 0xE9;                       // jmp back
    int32_t back = (int32_t)((at + 10) - ((uintptr_t)stub + n + 4));
    memcpy(stub + n, &back, 4); n += 4;
    uint8_t j[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
    int32_t rel = (int32_t)((uintptr_t)stub - (at + 5));
    memcpy(j + 1, &rel, 4);
    bool ok = WriteCode(at, j, sizeof(j));
    Log("patch %s @%08X -> %s (y scale %.3f)", name, (unsigned)at, ok ? "ok" : "FAILED", s_scaleY720);
}

// ---------------------------------------------------------------- sprite clip rectangles
// Sprites can carry a clip rectangle (e.g. the rolling rank counter on the result screen) written as
// hard-coded 1280x720 pixel values. Scale them by height/720 before they reach the scissor setter
// (0x404A90: eax = x, esi = y, two stack args = width/height; it clamps to the screen itself).
extern "C" {
float g_clipScale = 1.0f;
uintptr_t g_scissorOrig = 0x404A90;
void ScissorThunk();
}
__asm__(".globl _ScissorThunk\n"
        "_ScissorThunk:\n"
        "  pushl %eax\n"
        "  fildl (%esp)\n"
        "  fmuls _g_clipScale\n"
        "  fistpl (%esp)\n"
        "  popl %eax\n"
        "  pushl %esi\n"
        "  fildl (%esp)\n"
        "  fmuls _g_clipScale\n"
        "  fistpl (%esp)\n"
        "  popl %esi\n"
        "  fildl 4(%esp)\n"
        "  fmuls _g_clipScale\n"
        "  fistpl 4(%esp)\n"
        "  fildl 8(%esp)\n"
        "  fmuls _g_clipScale\n"
        "  fistpl 8(%esp)\n"
        "  jmp *_g_scissorOrig\n");

static void RedirectCall(uintptr_t at, uintptr_t expectTarget, void* to, const char* name) {
    uint8_t* p = (uint8_t*)at;
    if (p[0] != 0xE8 || at + 5 + *(int32_t*)(p + 1) != expectTarget) {
        Log("patch %s @%08X: unexpected bytes, skipped", name, (unsigned)at);
        return;
    }
    int32_t rel = (int32_t)((uintptr_t)to - (at + 5));
    WriteCode(at + 1, &rel, 4);
}

// ---------------------------------------------------------------- apply
void ApplyCodePatches() {
    if (!s_verified) {
        Log("code patches skipped: unknown Game.exe build");
        return;
    }
    JmpTo(0x709890, (void*)Hook_GetBoardType, "\x83\xEC\x28\xA1\x14\xE3\x7B\x00", 8, "amPlatformGetBoardType");
    JmpTo(0x7078D0, (void*)Hook_DongleLoadTable, "\x51\x55\x57\x8B\x3D\xB8\xE2\x7B", 8, "amDongleLoadTable");
    JmpTo(0x7073B0, (void*)Hook_DongleSetupKeychip, "\x83\xEC\x0C\xA1\xB8\xE2\x7B\x00", 8, "amDongleSetupKeychip");
    JmpTo(0x708D40, (void*)Hook_DongleUpdate, "\x8B\x0D\xB8\xE2\x7B\x00\x83\xEC", 8, "amDongleUpdate");
    JmpTo(0x7088A0, (void*)Hook_DongleGetRegion, "\xA1\xB8\xE2\x7B\x00\x83\x38\x00", 8, "amDongleGetRegion");
    JmpTo(0x708950, (void*)Hook_DongleGetKeyId, "\xA1\xB8\xE2\x7B\x00\x83\x38\x00", 8, "amDongleGetKeyId");
    JmpTo(0x6FE830, (void*)Hook_MasterGetProcessCount, "\x51\x33\xC0\x66\x89\x04\x24\xA1", 8, "amMasterGetProcessCount");
    JmpTo(0x6FE910, (void*)Hook_MasterExit, "\x81\xEC\xCC\x00\x00\x00\xA1\x14", 8, "amMasterExitA");
    // JVS bus reset: the game waits 1 s after each of two reset broadcasts for real boards to settle.
    {
        static const uint8_t expect[] = {0x68, 0xE8, 0x03, 0x00, 0x00};
        if (memcmp((void*)0x704BEF, expect, 5) == 0) {
            uint8_t p[5] = {0x68, 20, 0, 0, 0};
            WriteCode(0x704BEF, p, 5);
            Log("patch JVS reset wait 1000 ms -> 20 ms");
        }
    }
    // Boot hardware-init waits (seat/power-up checks): a 360-frame countdown, then a 60->240 frame settle.
    {
        static const uint8_t cd[] = {0xC7, 0x05, 0xF8, 0xDD, 0x57, 0x01, 0x68, 0x01, 0x00, 0x00};
        static const uint8_t st[] = {0x3D, 0xF0, 0x00, 0x00, 0x00};
        if (memcmp((void*)0x4D7FDB, cd, sizeof(cd)) == 0 && memcmp((void*)0x4D8128, st, sizeof(st)) == 0) {
            uint32_t one = 1, settle = 0x3D;
            WriteCode(0x4D7FDB + 6, &one, 4);
            WriteCode(0x4D8128 + 1, &settle, 4);
            Log("patch boot hardware waits 9 s -> ~2 frames");
        } else {
            Log("patch boot hardware waits: unexpected bytes, skipped");
        }
    }
    // Attract "Presented by SEGA" screen: skip drawing the KEYCHIP ID / MAIN ID lines.
    {
        static const uint8_t expect[] = {0xD9, 0x05, 0xA0, 0x70, 0x78, 0x00, 0x8B, 0x0D};
        if (memcmp((void*)0x46C0E6, expect, sizeof(expect)) == 0) {
            uint8_t p[8] = {0x83, 0xEC, 0x30, 0xE9}; // sub esp,0x30 (matches the add esp,0x30 we land before)
            int32_t rel = (int32_t)(0x46C29C - (0x46C0E6 + 3 + 5));
            memcpy(p + 4, &rel, 4);
            WriteCode(0x46C0E6, p, 8);
            Log("patch hide keychip/main ID text");
        }
    }
    s_scaleY720 = g_layout.renderH / 720.0f;
    PatchFixedYScale(0x46D30D, "title background height");
    g_clipScale = g_layout.renderH / 720.0f;
    for (uintptr_t site : {0x416207u, 0x416DBAu, 0x41703Fu, 0x418721u})
        RedirectCall(site, 0x404A90, (void*)ScissorThunk, "sprite clip");
    Log("sprite clip rectangles scaled by %.3f", g_clipScale);
    JmpTo(0x70B480, (void*)Hook_AmLibPrint, "\x81\xEC\x04\x04\x00\x00\xA1\x14", 8, "amLibPrint");

    // Turn on amLib's error reporting for every module (routed into port.log by the hook above)
    static const uintptr_t levels[] = {0xA07688, 0xA07F90, 0xA07F98, 0xA082B8, 0xA08548, 0xA08554, 0xA08558,
                                       0xA148A8, 0xA14CB8, 0xA14ED0, 0xA14ED8, 0xA18DA8, 0xA18E30, 0xA1CE10,
                                       0xA21058};
    for (uintptr_t a : levels) *(int*)a = 1;
}

// ---------------------------------------------------------------- settings store
// The game keeps operator settings in a linked list of named values (head at 0x15C3CF4), loaded from
// settings.tg. Node: name, type (0 = int), value pointer, ?, next.
struct StoreNode {
    const char* name;
    int type;
    int* value;
    int unused;
    StoreNode* next;
};
static StoreNode* FindKey(const char* key) {
    if (!s_verified) return nullptr;
    for (StoreNode* n = *(StoreNode**)0x15C3CF4; n; n = n->next)
        if (n->name && strcmp(n->name, key) == 0) return (n->type == 0 && n->value) ? n : nullptr;
    return nullptr;
}
bool GameOptGet(const char* key, int& v) {
    StoreNode* n = FindKey(key);
    if (!n) return false;
    v = *n->value;
    return true;
}
bool GameOptSet(const char* key, int v) {
    StoreNode* n = FindKey(key);
    if (!n) return false;
    *n->value = v;
    return true;
}
int GameRegionIndex() { return s_verified ? *(int*)0x348A0E4 : 0; }

// Boot progress for the loading screen: < 0 once the game has finished booting.
float BootProgress() {
    if (!s_verified) return -1.0f;
    int top = *(int*)0x15BA740;   // 0 init, 1 boot state machine, 2 mode init, 3 running
    if (top >= 3) return -1.0f;
    int st = *(int*)0x15BA788;    // boot state 0..11
    float f = top == 0 ? 0.0f : top == 1 ? 0.1f + 0.75f * (st / 11.0f) : 0.9f;
    return f;
}

// ---------------------------------------------------------------- gun calibration
// Guns map raw readings to the screen from four corner points per gun (live copy at 0x15B9B90 +
// gun*0x44: flag, TL, BR, BL, TR, centre; store keys c1f, c11x..c15y). Raw = our 16-bit value / 4,
// so the exact corners follow directly from how the port encodes the mouse/pad position.
static void SeedCalibration() {
    auto raw = [](float v, bool inv) { return (int)((inv ? 1.0f - v : v) * 65535.0f + 0.5f) >> 2; };
    int L = raw(0, g_cfg.invertX), R = raw(1, g_cfg.invertX), T = raw(0, g_cfg.invertY), B = raw(1, g_cfg.invertY);
    int C = raw(0.5f, false);
    const int cal[11] = {1, L, T, R, B, L, B, R, T, C, C};
    static const char* keys[2][11] = {
        {"c1f", "c11x", "c11y", "c12x", "c12y", "c13x", "c13y", "c14x", "c14y", "c15x", "c15y"},
        {"c2f", "c21x", "c21y", "c22x", "c22y", "c23x", "c23y", "c24x", "c24y", "c25x", "c25y"}};
    static bool logged = false;
    for (int g = 0; g < 2; g++) {
        int* live = (int*)(0x15B9B90 + g * 0x44);
        for (int i = 0; i < 11; i++) {
            if (live[i] != cal[i]) live[i] = cal[i];
            GameOptSet(keys[g][i], cal[i]);
        }
    }
    if (!logged) {
        logged = true;
        Log("gun calibration seeded: left %d right %d top %d bottom %d", L, R, T, B);
    }
}

static void ApplyGameOptions() {
    static bool done = false;
    if (done || !FindKey("gdiff")) return; // settings not loaded yet
    done = true;
    struct { const char* key; int v; } o[] = {{"gdiff", g_cfg.optDifficulty}, {"glng", g_cfg.optLanguage},
                                             {"sbtl", g_cfg.optSubtitles},   {"gct", g_cfg.optContinue},
                                             {"gctt", g_cfg.optContinueTime}, {"gsst", g_cfg.optSelectTime},
                                             {"gss", g_cfg.optAttractSound}};
    for (auto& x : o)
        if (x.v >= 0 && GameOptSet(x.key, x.v)) Log("game option %s = %d", x.key, x.v);
}

void GameThreadFrame() {
    if (!s_verified) return;
    PokeGameState();
    if (g_cfg.seedCalibration && *(StoreNode**)0x15C3CF4) SeedCalibration();
    ApplyGameOptions();
}

void PokeGameState() {
    if (!s_verified) return;
    // Debug: report movie playback geometry whenever it changes.
    static int last[6] = {-1, -1, -1, -1, -1, -1};
    int cur[6] = {*(int*)0x158FB28, *(int*)0x158FB2C, *(int*)0x158FB48, *(int*)0x158FB40, *(int*)0x158FB44,
                  *(int*)0x158FB78};
    if (memcmp(cur, last, sizeof(cur)) != 0) {
        memcpy(last, cur, sizeof(cur));
        float* f = (float*)0x158FB4C;
        Log("movie: %dx%d mode=%d psyuv=%d rgb=%d active=%d custom pos %.1f,%.1f scale %.3f,%.3f size %.1f,%.1f",
            cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], f[0], f[1], f[3], f[4], f[7], f[8]);
        Log("render globals: screen %dx%d render %dx%d 3d %dx%d", *(int*)0xBD8260, *(int*)0xBD8264,
            *(int*)0xBD827C, *(int*)0xBD8280, *(int*)0xBD8274, *(int*)0xBD8278);
    }
    // amCredit configuration lives at 0xA07FE0 once initialised (flag at 0xA07FD8).
    // byte +2 is the free play switch.
    if (g_cfg.freePlay && *(int*)0xA07FD8) *(uint8_t*)0xA07FE2 = 1;
}

// ---------------------------------------------------------------- save files
static uint32_t Crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0u - (c & 1)));
    }
    return ~c;
}

static void MakeStaticRecord(std::vector<uint8_t>& sram, size_t off) {
    // Backup record 7 ("static info"): 0x200 bytes, CRC32 of bytes 4..end stored at +0.
    // +8: amCredit configuration (16 bytes), +0x18: per-player credit table (zeros are valid).
    uint8_t* r = &sram[off];
    memset(r, 0, 0x200);
    static const uint8_t credit[16] = {0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x01, 0x01,
                                       0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00};
    memcpy(r + 8, credit, 16);
    if (g_cfg.freePlay) r[8 + 2] = 1;
    uint32_t crc = Crc32(r + 4, 0x200 - 4);
    memcpy(r, &crc, 4);
}

static bool FileExists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void PrepareSaveFiles() {
    std::string sram = g_portDir + "sram.bin", eeprom = g_portDir + "eeprom.bin";
    if (g_cfg.resetSaveOnNextLaunch) {
        char stamp[32];
        SYSTEMTIME st;
        GetLocalTime(&st);
        snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
        if (FileExists(sram)) MoveFileA(sram.c_str(), (sram + "." + stamp + ".bak").c_str());
        if (FileExists(eeprom)) MoveFileA(eeprom.c_str(), (eeprom + "." + stamp + ".bak").c_str());
        g_cfg.resetSaveOnNextLaunch = false;
        ConfigSave();
        Log("save data reset (old files kept as .bak)");
    }
    if (!FileExists(sram)) {
        std::vector<uint8_t> img(2 * 1024 * 1024, 0);
        MakeStaticRecord(img, 0x0000);
        MakeStaticRecord(img, 0x1000);
        HANDLE h = CreateFileA(sram.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(h, img.data(), (DWORD)img.size(), &w, nullptr);
            CloseHandle(h);
            Log("created fresh sram.bin (2 MB, free play %s)", g_cfg.freePlay ? "on" : "off");
        }
    }
}

// ---------------------------------------------------------------- Game.ini
std::string RuntimeGameIniPath() { return g_portDir + "Game.runtime.ini"; }

void BuildRuntimeGameIni() {
    std::string src = g_gameDir + "Game.ini";
    std::vector<std::string> lines;
    FILE* f = fopen(src.c_str(), "rb");
    if (f) {
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) {
            std::string s = buf;
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
            lines.push_back(s);
        }
        fclose(f);
    } else {
        Log("Game.ini not found next to Game.exe, using built-in defaults");
        lines = {"JVSENABLE\t1", "SEATENABLE\t0", "REGION\tE"};
    }
    bool vsync = g_cfg.vsync == VSyncMode::On ||
                 (g_cfg.vsync == VSyncMode::Auto && std::abs(g_layout.refreshHz - 60) <= 1);
    std::vector<std::pair<std::string, std::string>> overrides = {
        {"FULLSCREEN", "0"},
        {"SCREEN_WIDTH", std::to_string(g_layout.renderW)},
        {"SCREEN_HEIGHT", std::to_string(g_layout.renderH)},
        {"RENDER_WIDTH", std::to_string(g_layout.renderW)},
        {"RENDER_HEIGHT", std::to_string(g_layout.renderH)},
        // 3D scene resolution: defaults to 1024x600 when absent, which is then scaled up.
        {"RENDER_WIDTH3D", std::to_string(g_layout.renderW)},
        {"RENDER_HEIGHT3D", std::to_string(g_layout.renderH)},
        {"ASPECT_X", "16"},
        {"ASPECT_Y", "9"},
        {"ENABLE_VSYNC", vsync ? "1" : "0"},
        {"SEATENABLE", "0"},
        {"JVSENABLE", "1"},
    };
    auto keyOf = [](const std::string& l) {
        size_t e = l.find_first_of(" \t");
        return e == std::string::npos ? l : l.substr(0, e);
    };
    std::string out;
    std::vector<bool> used(overrides.size(), false);
    for (auto& l : lines) {
        std::string k = keyOf(l);
        bool replaced = false;
        for (size_t i = 0; i < overrides.size(); i++) {
            if (k == overrides[i].first) {
                out += overrides[i].first + " " + overrides[i].second + "\r\n";
                used[i] = replaced = true;
                break;
            }
        }
        if (!replaced && !l.empty()) out += l + "\r\n";
    }
    for (size_t i = 0; i < overrides.size(); i++)
        if (!used[i]) out += overrides[i].first + " " + overrides[i].second + "\r\n";
    FILE* o = fopen(RuntimeGameIniPath().c_str(), "wb");
    if (o) {
        fwrite(out.data(), 1, out.size(), o);
        fclose(o);
    }
    Log("runtime Game.ini: render %dx%d, vsync %d", g_layout.renderW, g_layout.renderH, vsync ? 1 : 0);
}
