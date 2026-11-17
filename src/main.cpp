#include "version.h"
// Dream Raiders PC port - loaded as d3d9.dll next to Game.exe.
#include "common.h"
#include <stdlib.h>

std::string g_gameDir, g_portDir;
HMODULE g_module = nullptr;

static void Init() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_gameDir = exe;
    g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of("\\/") + 1);
    g_portDir = g_gameDir + "port\\";
    CreateDirectoryA(g_portDir.c_str(), nullptr);
    LogInit(g_portDir + "port.log");
    Log("Dream Raiders PC port v" PORT_VERSION_STR ", built " __DATE__ " " __TIME__);
    Log("game folder: %s", g_gameDir.c_str());
    Log("command line: %s", GetCommandLineA());

    // Relaunched for a test/game mode switch: wait until the previous copy has fully exited.
    char pid[16];
    if (GetEnvironmentVariableA("DRPORT_WAIT_PID", pid, sizeof(pid))) {
        SetEnvironmentVariableA("DRPORT_WAIT_PID", nullptr);
        extern bool g_wantForeground;
        g_wantForeground = true;
        HANDLE prev = OpenProcess(SYNCHRONIZE, FALSE, strtoul(pid, nullptr, 10));
        if (prev) {
            DWORD r = WaitForSingleObject(prev, 15000);
            CloseHandle(prev);
            Log("waited for previous copy (pid %s): %s", pid, r == WAIT_OBJECT_0 ? "exited" : "timed out");
        }
    }

    // The game loads Data\... relative to the working directory: make it the game folder.
    SetCurrentDirectoryA(g_gameDir.c_str());

    ConfigLoad();
    ConfigSave(); // writes a complete, commented-free ini with every option on first run
    ComputeLayout();
    if (!VerifyGameExe()) {
        MessageBoxA(nullptr,
                    "This Game.exe is not the build the Dream Raiders PC port was made for.\n"
                    "Hardware emulation is disabled; the game will probably not start.",
                    "Dream Raiders PC port", MB_ICONWARNING);
    }
    PrepareSaveFiles();
    BuildRuntimeGameIni();
    ApplyCodePatches();
    InstallImportHooks();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_module = inst;
        // Only act inside the game, not if some other program loads this d3d9.dll.
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        const char* base = strrchr(exe, '\\');
        base = base ? base + 1 : exe;
        if (_stricmp(base, "Game.exe") == 0) Init();
    }
    return TRUE;
}
