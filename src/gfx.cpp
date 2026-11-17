// Direct3D 9 proxy: borderless/windowed presentation, scaling, frame pacing and the F1 overlay.
#include "common.h"
#include <d3d9.h>
#include <mmsystem.h>
#include <math.h>
#include <algorithm>
#include <string>
#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
void ApplyWindowPlacement(HWND hwnd);
void DrawOverlayUI();
void EnsureMenuCursor();
static void LoadFonts(float scale);
float g_uiScale = 1.0f;

// Menu look: dark teal panels with Dream Raiders green accents.
static void ApplyMenuStyle(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowPadding = ImVec2(14, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.WindowRounding = 8;
    s.FrameRounding = 5;
    s.TabRounding = 5;
    s.GrabRounding = 5;
    s.PopupRounding = 6;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBorderSize = 0;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    s.GrabMinSize = 14;
    s.ScaleAllSizes(scale);
    auto C = [](int r, int g, int b, int a = 255) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a / 255.f); };
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = C(222, 236, 230);
    c[ImGuiCol_TextDisabled] = C(120, 160, 150);
    c[ImGuiCol_WindowBg] = C(6, 20, 24, 240);
    c[ImGuiCol_PopupBg] = C(8, 26, 30, 250);
    c[ImGuiCol_Border] = C(40, 140, 110, 160);
    c[ImGuiCol_BorderShadow] = C(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = C(14, 44, 46);
    c[ImGuiCol_FrameBgHovered] = C(22, 74, 66);
    c[ImGuiCol_FrameBgActive] = C(28, 96, 78);
    c[ImGuiCol_TitleBg] = C(8, 40, 38);
    c[ImGuiCol_TitleBgActive] = C(10, 62, 52);
    c[ImGuiCol_TitleBgCollapsed] = C(8, 40, 38);
    c[ImGuiCol_ScrollbarBg] = C(6, 20, 24);
    c[ImGuiCol_ScrollbarGrab] = C(30, 90, 76);
    c[ImGuiCol_ScrollbarGrabHovered] = C(40, 120, 96);
    c[ImGuiCol_ScrollbarGrabActive] = C(60, 170, 120);
    c[ImGuiCol_CheckMark] = C(90, 255, 150);
    c[ImGuiCol_SliderGrab] = C(70, 220, 140);
    c[ImGuiCol_SliderGrabActive] = C(110, 255, 170);
    c[ImGuiCol_Button] = C(18, 70, 58);
    c[ImGuiCol_ButtonHovered] = C(30, 112, 84);
    c[ImGuiCol_ButtonActive] = C(46, 156, 106);
    c[ImGuiCol_Header] = C(18, 70, 58);
    c[ImGuiCol_HeaderHovered] = C(30, 112, 84);
    c[ImGuiCol_HeaderActive] = C(46, 156, 106);
    c[ImGuiCol_Separator] = C(36, 96, 84, 180);
    c[ImGuiCol_SeparatorHovered] = C(60, 170, 120);
    c[ImGuiCol_SeparatorActive] = C(90, 255, 150);
    c[ImGuiCol_ResizeGrip] = C(30, 112, 84, 60);
    c[ImGuiCol_ResizeGripHovered] = C(46, 156, 106, 160);
    c[ImGuiCol_ResizeGripActive] = C(90, 255, 150, 220);
    c[ImGuiCol_Tab] = C(12, 46, 44);
    c[ImGuiCol_TabHovered] = C(34, 120, 90);
    c[ImGuiCol_TabSelected] = C(24, 92, 70);
    c[ImGuiCol_TabSelectedOverline] = C(90, 255, 150);
    c[ImGuiCol_TabDimmed] = C(10, 36, 36);
    c[ImGuiCol_TabDimmedSelected] = C(18, 70, 58);
    c[ImGuiCol_NavHighlight] = C(90, 255, 150);
    c[ImGuiCol_TextSelectedBg] = C(46, 156, 106, 120);
}
void AudioPause(bool pause);

Layout g_layout;
bool g_overlayOpen = false;

// ================================================================ layout
static BOOL CALLBACK EnumMon(HMONITOR m, HDC, LPRECT, LPARAM p) {
    auto v = (std::pair<int, HMONITOR>*)p;
    if (v->first-- == 0) v->second = m;
    return TRUE;
}

void ComputeLayout() {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (g_cfg.monitor > 0) {
        std::pair<int, HMONITOR> pick{g_cfg.monitor - 1, nullptr};
        EnumDisplayMonitors(nullptr, nullptr, EnumMon, (LPARAM)&pick);
        if (pick.second) mon = pick.second;
    }
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoA(mon, &mi);
    g_layout.monitorRect = mi.rcMonitor;
    DEVMODEA dm{};
    dm.dmSize = sizeof(dm);
    g_layout.refreshHz = EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) ? (int)dm.dmDisplayFrequency : 60;

    if (g_cfg.displayMode == DisplayMode::Windowed) {
        g_layout.clientW = g_cfg.windowW;
        g_layout.clientH = g_cfg.windowH;
    } else {
        g_layout.clientW = mi.rcMonitor.right - mi.rcMonitor.left;
        g_layout.clientH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    }
    Viewport vp = GetViewport();
    if (g_cfg.renderW > 0 && g_cfg.renderH > 0) {
        g_layout.renderW = g_cfg.renderW;
        g_layout.renderH = g_cfg.renderH;
    } else {
        // Native 16:9 (the game's own defaults are 1280x720 screen, 16:9 aspect).
        g_layout.renderW = vp.w & ~1;
        g_layout.renderH = vp.h & ~1;
    }
    Log("layout: client %dx%d, render %dx%d, monitor %ld,%ld %ldx%ld @%dHz", g_layout.clientW, g_layout.clientH,
        g_layout.renderW, g_layout.renderH, mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, g_layout.refreshHz);
}

Viewport GetViewport() {
    int cw = g_layout.clientW, ch = g_layout.clientH;
    if (g_hwnd) {
        RECT r;
        if (GetClientRect(g_hwnd, &r) && r.right > 0 && r.bottom > 0) {
            cw = r.right;
            ch = r.bottom;
        }
    }
    if (g_cfg.aspect == AspectMode::Stretch || cw <= 0 || ch <= 0) return Viewport{0, 0, cw, ch};
    int w = cw, h = (int)lround(cw * 9.0 / 16.0);
    if (h > ch) {
        h = ch;
        w = (int)lround(ch * 16.0 / 9.0);
    }
    return Viewport{(cw - w) / 2, (ch - h) / 2, w, h};
}

// ================================================================ icon
#include <shlobj.h>
#include <objbase.h>
void ApplyWindowIcon(HWND hwnd) {
    if (!hwnd || !g_module) return;
    HICON big = (HICON)LoadImageA(g_module, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                  GetSystemMetrics(SM_CYICON), 0);
    HICON small = (HICON)LoadImageA(g_module, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), 0);
    if (big) {
        SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
        SetClassLongPtrA(hwnd, GCLP_HICON, (LONG_PTR)big);
    }
    if (small) {
        SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);
        SetClassLongPtrA(hwnd, GCLP_HICONSM, (LONG_PTR)small);
    }
}

// Writes port\DreamRaiders.ico and a "Dream Raiders" shortcut (with that icon) next to Game.exe, once.
static DWORD WINAPI DesktopIconThread(LPVOID) {
    std::string ico = g_portDir + "DreamRaiders.ico", lnk = g_gameDir + "Dream Raiders.lnk";
    if (GetFileAttributesA(ico.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Rebuild an .ico file from the icon resource group embedded in this DLL.
        HRSRC grp = FindResourceA(g_module, MAKEINTRESOURCEA(1), (LPCSTR)RT_GROUP_ICON);
        if (grp) {
            #pragma pack(push, 2)
            struct GrpEntry { BYTE w, h, colors, res; WORD planes, bpp; DWORD size; WORD id; };
            struct FileEntry { BYTE w, h, colors, res; WORD planes, bpp; DWORD size; DWORD offset; };
            #pragma pack(pop)
            const BYTE* g = (const BYTE*)LockResource(LoadResource(g_module, grp));
            WORD count = *(const WORD*)(g + 4);
            const GrpEntry* ge = (const GrpEntry*)(g + 6);
            std::string out(6 + count * sizeof(FileEntry), '\0');
            memcpy(&out[0], g, 6);
            for (WORD i = 0; i < count; i++) {
                HRSRC r = FindResourceA(g_module, MAKEINTRESOURCEA(ge[i].id), (LPCSTR)RT_ICON);
                DWORD sz = r ? SizeofResource(g_module, r) : 0;
                FileEntry fe{ge[i].w, ge[i].h, ge[i].colors, ge[i].res, ge[i].planes, ge[i].bpp, sz, (DWORD)out.size()};
                memcpy(&out[6 + i * sizeof(FileEntry)], &fe, sizeof(fe));
                if (r) out.append((const char*)LockResource(LoadResource(g_module, r)), sz);
            }
            HANDLE h = CreateFileA(ico.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD w;
                WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
                CloseHandle(h);
            }
        }
    }
    // Only ever on the first run: if the shortcut is later moved or deleted, leave it that way.
    if (!g_cfg.shortcutCreated && SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        IShellLinkA* sl = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkA, (void**)&sl))) {
            std::string exe = g_gameDir + "Game.exe";
            sl->SetPath(exe.c_str());
            sl->SetWorkingDirectory(g_gameDir.c_str());
            sl->SetIconLocation(ico.c_str(), 0);
            sl->SetDescription("Dream Raiders (PC port)");
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
                wchar_t wl[MAX_PATH];
                MultiByteToWideChar(CP_ACP, 0, lnk.c_str(), -1, wl, MAX_PATH);
                if (SUCCEEDED(pf->Save(wl, TRUE))) Log("created shortcut %s", lnk.c_str());
                g_cfg.shortcutCreated = true;
                WritePrivateProfileStringA("Game", "ShortcutCreated", "1", (g_portDir + "DreamRaidersPort.ini").c_str());
                pf->Release();
            }
            sl->Release();
        }
        CoUninitialize();
    }
    return 0;
}
void InstallDesktopIcon() {
    HANDLE t = CreateThread(nullptr, 0, DesktopIconThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// ================================================================ window procedure
static WNDPROC s_origProc = nullptr;
static bool s_imguiReady = false;
static DWORD s_hintUntil = 0;

static LRESULT CALLBACK PortWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    // Esc opens/closes the pause menu (the game freezes while it is open).
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && !(lp & 0x40000000)) {
        g_overlayOpen = !g_overlayOpen;
        if (!g_overlayOpen) ConfigSave();
        else EnsureMenuCursor();
        InputStopRumble();
        return 0;
    }
    if (msg == WM_CLOSE) {
        Log("window closed");
        InputStopRumble();
        TerminateProcess(GetCurrentProcess(), 0);
    }
    if (s_imguiReady && g_overlayOpen) {
        if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return 0;
        if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR) return 0;
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(g_overlayOpen ? LoadCursor(nullptr, IDC_ARROW) : nullptr);
        return TRUE;
    }
    if (msg == WM_ERASEBKGND) {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) {
        InputStopRumble();
        ClipCursor(nullptr);
    }
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_KEYMENU) return 0; // no Alt menu beep
    return CallWindowProcA(s_origProc, h, msg, wp, lp);
}

// ================================================================ focus after a test/game mode relaunch
bool g_wantForeground = false;
static void TakeForeground() {
    if (!g_hwnd || GetForegroundWindow() == g_hwnd) return;
    // A tap of Alt lets a background process claim the foreground (the menu it would open is suppressed).
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(g_hwnd);
    BringWindowToTop(g_hwnd);
    SetFocus(g_hwnd);
}

// ================================================================ frame pacing
static LARGE_INTEGER s_qpf, s_nextFrame;
static bool s_vsyncEffective = false;
static HANDLE s_timer = nullptr;

static void FrameLimit() {
    if (s_vsyncEffective || g_cfg.fpsLimit <= 0) return;
    if (!s_qpf.QuadPart) {
        QueryPerformanceFrequency(&s_qpf);
        timeBeginPeriod(1);
        s_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!s_timer) s_timer = CreateWaitableTimerA(nullptr, TRUE, nullptr);
    }
    LONGLONG period = s_qpf.QuadPart / g_cfg.fpsLimit;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!s_nextFrame.QuadPart || now.QuadPart - s_nextFrame.QuadPart > period * 4) s_nextFrame.QuadPart = now.QuadPart;
    LONGLONG wait = s_nextFrame.QuadPart - now.QuadPart;
    if (wait > 0) {
        LONGLONG us = wait * 1000000 / s_qpf.QuadPart;
        if (us > 1500 && s_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(us - 1000) * 10;
            SetWaitableTimer(s_timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(s_timer, 50);
        }
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < s_nextFrame.QuadPart);
    }
    s_nextFrame.QuadPart += period;
}

// ================================================================ D3D9 hooks
typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
static CreateDevice_t o_CreateDevice;
static Reset_t o_Reset;
static Present_t o_Present;
static IDirect3DDevice9* s_device;

static void PatchVtbl(void* obj, int idx, void* hook, void** orig) {
    void** vt = *(void***)obj;
    if (vt[idx] == hook) return;
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    *orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}

static void FixParams(D3DPRESENT_PARAMETERS* pp, HWND hwnd) {
    s_vsyncEffective = g_cfg.vsync == VSyncMode::On ||
                       (g_cfg.vsync == VSyncMode::Auto && abs(g_layout.refreshHz - 60) <= 1);
    Log("game asked: %ux%u windowed=%d fmt=%d interval=%08X", pp->BackBufferWidth, pp->BackBufferHeight,
        pp->Windowed, pp->BackBufferFormat, pp->PresentationInterval);
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->BackBufferWidth = g_layout.renderW;
    pp->BackBufferHeight = g_layout.renderH;
    if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->hDeviceWindow = hwnd;
    pp->PresentationInterval = s_vsyncEffective ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
}

static void InitImGui(IDirect3DDevice9* dev) {
    if (s_imguiReady) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    float scale = std::max(1.0f, g_layout.clientH / 1080.0f);
    g_uiScale = scale;
    ApplyMenuStyle(scale);
    LoadFonts(scale);
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX9_Init(dev);
    s_imguiReady = true;
    s_hintUntil = GetTickCount() + 8000;
}

// ================================================================ loading screen
static ImFont* s_fontTitle = nullptr;
static ImFont* s_fontSmall = nullptr;
static float s_loadAlpha = 1.0f;
static float s_shownProgress = 0.0f;

static void LoadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    std::string dir = std::string(win) + "\\Fonts\\";
    const char* bold[] = {"bahnschrift.ttf", "segoeuib.ttf", "arialbd.ttf"};
    for (const char* f : bold) {
        std::string p = dir + f;
        if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        io.Fonts->AddFontFromFileTTF(p.c_str(), 19.0f * scale); // menu font (first = default)
        s_fontTitle = io.Fonts->AddFontFromFileTTF(p.c_str(), 92.0f * scale);
        s_fontSmall = io.Fonts->AddFontFromFileTTF(p.c_str(), 26.0f * scale);
        Log("menu/loading font: %s", f);
        break;
    }
    if (!s_fontTitle) { // no Windows fonts: built-in pixel font, scaled
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault(&cfg);
    }
}

static uint32_t Hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

// An original "Dream Raiders"-flavoured boot card: dark teal, glowing green circuit traces, silver title.
static void DrawLoadingScreen(float alpha, float progress) {
    Viewport vp = GetViewport();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    auto A = [&](int a) { return (int)(a * alpha); };
    ImVec2 p0((float)vp.x, (float)vp.y), p1((float)(vp.x + vp.w), (float)(vp.y + vp.h));
    ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    float u = vp.h / 1080.0f;
    float t = (float)GetTickCount() / 1000.0f;

    dl->AddRectFilledMultiColor(p0, p1, IM_COL32(3, 12, 22, A(255)), IM_COL32(3, 12, 22, A(255)),
                                IM_COL32(4, 30, 34, A(255)), IM_COL32(4, 30, 34, A(255)));
    for (int i = 16; i >= 1; i--) // soft green glow behind the title
        dl->AddCircleFilled(c, 42.0f * u * i, IM_COL32(20, 140, 70, A(5)), 64);

    // Circuit traces: right-angled paths running out from around the centre, with glowing end nodes.
    for (int i = 0; i < 46; i++) {
        uint32_t h = Hash(i * 7919 + 17);
        float ang = (h % 3600) / 3600.0f * 6.2831853f;
        float r0 = (220 + (h >> 12) % 160) * u;
        ImVec2 a(c.x + cosf(ang) * r0 * 1.7f, c.y + sinf(ang) * r0);
        ImVec2 pts[4];
        pts[0] = a;
        float len1 = (60 + (h >> 4) % 220) * u, len2 = (40 + (h >> 20) % 260) * u;
        bool horizFirst = h & 1;
        float sx = cosf(ang) >= 0 ? 1.0f : -1.0f, sy = sinf(ang) >= 0 ? 1.0f : -1.0f;
        pts[1] = horizFirst ? ImVec2(a.x + sx * len1, a.y) : ImVec2(a.x, a.y + sy * len1);
        pts[2] = ImVec2(pts[1].x + sx * len2 * 0.7f, pts[1].y + sy * len2 * 0.7f);
        pts[3] = horizFirst ? ImVec2(pts[2].x + sx * len1 * 0.6f, pts[2].y) : ImVec2(pts[2].x, pts[2].y + sy * len1 * 0.6f);
        dl->AddPolyline(pts, 4, IM_COL32(40, 150, 170, A(70)), 0, 2.0f * u);
        float pulse = 0.5f + 0.5f * sinf(t * 2.4f + i * 1.7f);
        dl->AddCircleFilled(pts[3], 9.0f * u, IM_COL32(60, 255, 140, A((int)(40 * pulse))), 16);
        dl->AddCircleFilled(pts[3], 4.0f * u, IM_COL32(150, 255, 190, A((int)(120 + 120 * pulse))), 12);
        dl->AddCircle(a, 3.0f * u, IM_COL32(40, 150, 170, A(110)), 10, 1.5f * u);
    }

    // Title
    const char* title = "DREAM RAIDERS";
    ImFont* ft = s_fontTitle ? s_fontTitle : ImGui::GetFont();
    float fs = s_fontTitle ? ft->FontSize : 64.0f * u;
    ImVec2 ts = ft->CalcTextSizeA(fs, FLT_MAX, 0, title);
    ImVec2 tp(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f - 30 * u);
    for (int r = 6; r >= 2; r -= 2) // green glow
        for (int k = 0; k < 8; k++) {
            float a2 = k * 0.785398f;
            dl->AddText(ft, fs, ImVec2(tp.x + cosf(a2) * r * u, tp.y + sinf(a2) * r * u), IM_COL32(40, 255, 120, A(22)), title);
        }
    dl->AddText(ft, fs, ImVec2(tp.x + 3 * u, tp.y + 4 * u), IM_COL32(0, 0, 0, A(160)), title);
    dl->AddText(ft, fs, tp, IM_COL32(222, 228, 234, A(255)), title);

    // Loading bar
    float bw = 520 * u, bh = 6 * u;
    ImVec2 b0(c.x - bw * 0.5f, tp.y + ts.y + 70 * u), b1(b0.x + bw, b0.y + bh);
    dl->AddRectFilled(b0, b1, IM_COL32(20, 60, 60, A(200)), 3 * u);
    dl->AddRectFilled(b0, ImVec2(b0.x + bw * progress, b1.y), IM_COL32(70, 255, 140, A(255)), 3 * u);
    dl->AddRectFilled(ImVec2(b0.x, b0.y - 3 * u), ImVec2(b0.x + bw * progress, b1.y + 3 * u), IM_COL32(70, 255, 140, A(40)), 5 * u);

    ImFont* fsm = s_fontSmall ? s_fontSmall : ImGui::GetFont();
    float ss = s_fontSmall ? fsm->FontSize : 20.0f * u;
    int dots = (int)(t * 3) % 4;
    char label[32];
    snprintf(label, sizeof(label), "LOADING%.*s", dots, "...");
    ImVec2 ls = fsm->CalcTextSizeA(ss, FLT_MAX, 0, "LOADING...");
    dl->AddText(fsm, ss, ImVec2(c.x - ls.x * 0.5f, b1.y + 18 * u), IM_COL32(150, 220, 200, A(230)), label);
}

static void UpdateLoadingScreen() {
    float p = BootProgress();
    static DWORD last = GetTickCount();
    DWORD now = GetTickCount();
    float dt = (now - last) / 1000.0f;
    last = now;
    if (p >= 0.0f) {
        s_loadAlpha = 1.0f;
        s_shownProgress = std::max(s_shownProgress, std::min(p, s_shownProgress + dt * 1.5f));
    } else if (s_loadAlpha > 0.0f) {
        s_shownProgress = std::min(1.0f, s_shownProgress + dt * 3.0f);
        if (s_shownProgress >= 1.0f) {
            s_loadAlpha = std::max(0.0f, s_loadAlpha - dt * 2.5f); // fade out
            if (s_loadAlpha <= 0.0f) s_hintUntil = GetTickCount() + 6000; // show the Esc hint once in
        }
    }
    if (s_loadAlpha > 0.0f) DrawLoadingScreen(s_loadAlpha, s_shownProgress);
}

static void RenderOverlay(IDirect3DDevice9* dev) {
    if (!s_imguiReady) return;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    UpdateLoadingScreen();
    if (g_cfg.showHint && s_loadAlpha <= 0.0f && GetTickCount() < s_hintUntil && !g_overlayOpen) {
        Viewport vp = GetViewport();
        ImGui::SetNextWindowPos(ImVec2(vp.x + 20.0f, vp.y + 20.0f));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("Esc: menu");
        ImGui::End();
    }
    if (g_overlayOpen) DrawOverlayUI();
    ImGui::EndFrame();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd && dd->CmdListsCount > 0) {
        // Draw straight onto the window-sized output: ImGui works in client pixels, so map to the backbuffer.
        IDirect3DStateBlock9* sb = nullptr;
        if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb->Capture();
        // ImGui works in window client pixels; the backbuffer only covers the game picture (viewport),
        // possibly at a different resolution, so move everything into backbuffer space.
        Viewport vp = GetViewport();
        float sx = (float)g_layout.renderW / std::max(1, vp.w), sy = (float)g_layout.renderH / std::max(1, vp.h);
        for (int n = 0; n < dd->CmdListsCount; n++) {
            ImDrawList* cl = dd->CmdLists[n];
            for (ImDrawVert& v : cl->VtxBuffer) {
                v.pos.x = (v.pos.x - vp.x) * sx;
                v.pos.y = (v.pos.y - vp.y) * sy;
            }
            for (ImDrawCmd& c : cl->CmdBuffer) {
                c.ClipRect.x = (c.ClipRect.x - vp.x) * sx;
                c.ClipRect.z = (c.ClipRect.z - vp.x) * sx;
                c.ClipRect.y = (c.ClipRect.y - vp.y) * sy;
                c.ClipRect.w = (c.ClipRect.w - vp.y) * sy;
            }
        }
        dd->DisplayPos = ImVec2(0, 0);
        dd->DisplaySize = ImVec2((float)g_layout.renderW, (float)g_layout.renderH);
        dd->FramebufferScale = ImVec2(1, 1);
        IDirect3DSurface9* oldRt = nullptr;
        dev->GetRenderTarget(0, &oldRt);
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            dev->SetRenderTarget(0, bb);
            bb->Release();
        }
        D3DVIEWPORT9 v = {0, 0, (DWORD)g_layout.renderW, (DWORD)g_layout.renderH, 0.0f, 1.0f};
        dev->SetViewport(&v);
        if (SUCCEEDED(dev->BeginScene())) {
            ImGui_ImplDX9_RenderDrawData(dd);
            dev->EndScene();
        }
        if (oldRt) {
            dev->SetRenderTarget(0, oldRt);
            oldRt->Release();
        }
        if (sb) {
            sb->Apply();
            sb->Release();
        }
    }
}

// ================================================================ pause
static IDirect3DTexture9* s_pauseTex = nullptr;
static void ReleasePauseTex() {
    if (s_pauseTex) {
        s_pauseTex->Release();
        s_pauseTex = nullptr;
    }
}

// Runs inside the game's Present while the menu is open: the game thread (and with it the game) is held
// here, the last frame is redrawn under the menu, sound is stopped, and window messages keep flowing.
static void PauseLoop(IDirect3DDevice9* dev, HWND wnd) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    if (!s_pauseTex)
        dev->CreateTexture(g_layout.renderW, g_layout.renderH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                           D3DPOOL_DEFAULT, &s_pauseTex, nullptr);
    IDirect3DSurface9* frozen = nullptr;
    if (s_pauseTex) s_pauseTex->GetSurfaceLevel(0, &frozen);
    if (frozen) dev->StretchRect(bb, nullptr, frozen, nullptr, D3DTEXF_NONE);
    AudioPause(true);
    InputStopRumble();
    Log("paused");
    while (g_overlayOpen) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                PostQuitMessage((int)m.wParam);
                g_overlayOpen = false;
                break;
            }
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        if (!g_overlayOpen) break;
        if (frozen) dev->StretchRect(frozen, nullptr, bb, nullptr, D3DTEXF_NONE);
        EnsureMenuCursor();
        RenderOverlay(dev);
        FrameLimit();
        Viewport vp = GetViewport();
        RECT d = {vp.x, vp.y, vp.x + vp.w, vp.y + vp.h};
        if (o_Present(dev, nullptr, &d, wnd, nullptr) == D3DERR_DEVICELOST) {
            g_overlayOpen = false;
            break;
        }
        if (s_vsyncEffective == false && g_cfg.fpsLimit <= 0) Sleep(1);
    }
    if (frozen) {
        dev->StretchRect(frozen, nullptr, bb, nullptr, D3DTEXF_NONE); // hand the game's frame back
        frozen->Release();
    }
    bb->Release();
    AudioPause(false);
    ConfigSave();
    Log("resumed");
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                                               const RGNDATA* dirty) {
    if (g_overlayOpen) PauseLoop(dev, wnd);
    static bool clipped = false;
    if (g_cfg.displayMode == DisplayMode::Windowed && g_cfg.confineMouse && InputHasFocus() && !g_overlayOpen) {
        RECT r;
        GetClientRect(g_hwnd, &r);
        MapWindowPoints(g_hwnd, nullptr, (POINT*)&r, 2);
        ClipCursor(&r);
        clipped = true;
    } else if (clipped) {
        ClipCursor(nullptr);
        clipped = false;
    }
    static int frames = 0;
    if (g_wantForeground && ++frames <= 120 && (frames == 1 || frames % 30 == 0)) {
        TakeForeground();
        if (GetForegroundWindow() == g_hwnd) g_wantForeground = false;
    }
    GameThreadFrame();
    InputFrame();
    EnsureMenuCursor();
    RenderOverlay(dev);
    FrameLimit();
    Viewport vp = GetViewport();
    RECT d = {vp.x, vp.y, vp.x + vp.w, vp.y + vp.h};
    HRESULT hr = o_Present(dev, nullptr, &d, wnd, dirty);
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    ReleasePauseTex();
    if (s_imguiReady) ImGui_ImplDX9_InvalidateDeviceObjects();
    if (pp) FixParams(pp, g_hwnd);
    HRESULT hr = o_Reset(dev, pp);
    Log("device reset -> %08lX", hr);
    if (s_imguiReady) ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus,
                                                   DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HWND hwnd = pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    if (g_hwnd != hwnd) Log("device window %p (window hook saw %p)", hwnd, g_hwnd);
    g_hwnd = hwnd;
    ApplyWindowPlacement(g_hwnd);
    if (!s_origProc) s_origProc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)PortWndProc);
    ApplyWindowIcon(g_hwnd);
    static bool iconFiles = false;
    if (!iconFiles) {
        iconFiles = true;
        InstallDesktopIcon();
    }
    InvalidateRect(g_hwnd, nullptr, TRUE);
    if (pp) FixParams(pp, g_hwnd);
    HRESULT hr = o_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    Log("CreateDevice(%ux%u) -> %08lX", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, hr);
    if (SUCCEEDED(hr) && out && *out) {
        s_device = *out;
        PatchVtbl(s_device, 16, (void*)Hook_Reset, (void**)&o_Reset);
        PatchVtbl(s_device, 17, (void*)Hook_Present, (void**)&o_Present);
        InitImGui(s_device);
    }
    return hr;
}

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    static Direct3DCreate9_t real = nullptr;
    if (!real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\d3d9.dll");
        HMODULE m = LoadLibraryA(path);
        real = m ? (Direct3DCreate9_t)GetProcAddress(m, "Direct3DCreate9") : nullptr;
        Log("system d3d9: %s %s", path, real ? "loaded" : "MISSING");
        if (!real) return nullptr;
    }
    IDirect3D9* d3d = real(sdk);
    if (d3d) PatchVtbl(d3d, 16, (void*)Hook_CreateDevice, (void**)&o_CreateDevice);
    return d3d;
}
