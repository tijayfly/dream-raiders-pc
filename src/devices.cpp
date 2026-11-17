// Import-table hooks on Game.exe: arcade device emulation (JVS, SMBus EEPROM/DIP switches, SRAM),
// Game.ini redirection and window management.
#include "common.h"
#include <winioctl.h>
#include <setupapi.h>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>

HWND g_hwnd = nullptr;

// ================================================================ IAT patching
static void* PatchImport(const char* dll, const char* func, void* hook) {
    auto base = (uint8_t*)GetModuleHandleA(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, func) != 0) continue;
            void* orig = (void*)addrs->u1.Function;
            DWORD old;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            addrs->u1.Function = (uintptr_t)hook;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    Log("import %s!%s not found", dll, func);
    return nullptr;
}
#define HOOK(dll, name) o_##name = (decltype(o_##name))PatchImport(dll, #name, (void*)Hook_##name)

// ================================================================ fake devices
enum class Dev { Jvs, SmBus, Sram };
static std::mutex s_devLock;
static std::map<HANDLE, Dev> s_devs;
static const char kSmBusPath[] = "\\\\?\\root#mxsmbus#0000#{port-emulated}";

static HANDLE NewFakeHandle(Dev d) {
    HANDLE h = CreateEventA(nullptr, TRUE, FALSE, nullptr); // a real, closable kernel handle
    std::lock_guard<std::mutex> l(s_devLock);
    s_devs[h] = d;
    return h;
}
static bool DevOf(HANDLE h, Dev& d) {
    std::lock_guard<std::mutex> l(s_devLock);
    auto it = s_devs.find(h);
    if (it == s_devs.end()) return false;
    d = it->second;
    return true;
}
static bool IsFake(HANDLE h) {
    Dev d;
    return DevOf(h, d) && d != Dev::Sram;
}

static BOOL Complete(LPOVERLAPPED ov, LPDWORD ret, DWORD n) {
    if (ret) *ret = n;
    if (ov) {
        ov->Internal = 0;
        ov->InternalHigh = n;
        if (ov->hEvent) SetEvent(ov->hEvent);
    }
    return TRUE;
}

// ---------------------------------------------------------------- SMBus (EEPROM + PCA9535 DIP switches)
static std::vector<uint8_t> s_eeprom; // 8 devices x 64 KB, indexed by (i2c addr & 7)
static std::mutex s_eepLock;
static void EepromLoad() {
    s_eeprom.assign(8 * 65536, 0xFF);
    HANDLE h = CreateFileA((g_portDir + "eeprom.bin").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD r;
        ReadFile(h, s_eeprom.data(), (DWORD)s_eeprom.size(), &r, nullptr);
        CloseHandle(h);
    }
}
static void EepromStore(size_t off, size_t n) {
    HANDLE h = CreateFileA((g_portDir + "eeprom.bin").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart < (LONGLONG)s_eeprom.size()) { // first write: whole image
        DWORD w;
        WriteFile(h, s_eeprom.data(), (DWORD)s_eeprom.size(), &w, nullptr);
    } else {
        SetFilePointer(h, (LONG)off, nullptr, FILE_BEGIN);
        DWORD w;
        WriteFile(h, &s_eeprom[off], (DWORD)n, &w, nullptr);
    }
    CloseHandle(h);
}

static uint8_t s_pcaRegs[8][8];
static int s_smbusLogged = 0;

static BOOL SmBusIoctl(DWORD code, void* in, DWORD inSz, void* out, DWORD outSz, LPDWORD ret, LPOVERLAPPED ov) {
    uint8_t* b = (uint8_t*)(out ? out : in);
    switch (code) {
    case 0x9C406008: // driver version
        if (out && outSz >= 4) *(DWORD*)out = 0x00010001;
        return Complete(ov, ret, 4);
    case 0x9C402004: { // byte transaction: [0]status [1]cmd [2]addr [3]reg [5]data
        if (!b || outSz < 6) return Complete(ov, ret, outSz);
        if (in && in != out) memcpy(b, in, std::min(inSz, outSz));
        uint8_t cmd = b[1], addr = b[2], reg = b[3];
        if (cmd == 5) b[5] = (reg < 2) ? 0xFF : s_pcaRegs[addr & 7][reg & 7]; // input ports: all switches off
        else if (cmd == 4) s_pcaRegs[addr & 7][reg & 7] = b[5];
        if (s_smbusLogged < 30) {
            s_smbusLogged++;
            Log("smbus byte cmd=%u addr=%02X reg=%02X data=%02X", cmd, addr, reg, b[5]);
        }
        b[0] = 0;
        return Complete(ov, ret, outSz);
    }
    case 0x9C40200C: { // block transaction: [0]status [1]cmd [2]addr(16) [4]reg(16) [6]len [7..]data
        if (!b || outSz < 7) return Complete(ov, ret, outSz);
        if (in && in != out) memcpy(b, in, std::min(inSz, outSz));
        uint8_t cmd = b[1];
        unsigned addr = *(uint16_t*)(b + 2), reg = *(uint16_t*)(b + 4), len = std::min<unsigned>(b[6], 0x20);
        size_t off = ((addr & 7) << 16) | (reg & 0xFFFF);
        std::lock_guard<std::mutex> l(s_eepLock);
        if (off + len <= s_eeprom.size()) {
            if (cmd == 9) memcpy(b + 7, &s_eeprom[off], len);
            else if (cmd == 8) {
                memcpy(&s_eeprom[off], b + 7, len);
                EepromStore(off, len);
            }
        }
        b[0] = 0;
        return Complete(ov, ret, outSz);
    }
    }
    Log("smbus: unhandled ioctl %08lX", code);
    if (out) memset(out, 0, outSz);
    return Complete(ov, ret, outSz);
}

// ---------------------------------------------------------------- JVS (mxjvs driver)
static BOOL JvsIoctl(DWORD code, void* in, DWORD inSz, void* out, DWORD outSz, LPDWORD ret, LPOVERLAPPED ov) {
    if (code == 0x9C402000) {
        PokeGameState();
        size_t n = JvsProcess((const uint8_t*)in, inSz, (uint8_t*)out, outSz);
        return Complete(ov, ret, (DWORD)n);
    }
    Log("jvs: unhandled ioctl %08lX in=%lu out=%lu", code, inSz, outSz);
    if (out && outSz) memset(out, 0, outSz);
    return Complete(ov, ret, outSz >= 4 ? 4 : outSz);
}

// ---------------------------------------------------------------- SRAM (real file)
static BOOL SramIoctl(DWORD code, void* out, DWORD outSz, LPDWORD ret, LPOVERLAPPED ov) {
    if (code == 0x9C406000 && out && outSz >= 4) { // driver version
        *(DWORD*)out = 0x00010001;
        return Complete(ov, ret, 4);
    }
    if (code == IOCTL_DISK_GET_DRIVE_GEOMETRY && out && outSz >= sizeof(DISK_GEOMETRY)) {
        auto g = (DISK_GEOMETRY*)out;
        g->Cylinders.QuadPart = 4096; // 4096 * 512 = 2 MB
        g->MediaType = FixedMedia;
        g->TracksPerCylinder = 1;
        g->SectorsPerTrack = 1;
        g->BytesPerSector = 512;
        return Complete(ov, ret, sizeof(DISK_GEOMETRY));
    }
    Log("sram: unhandled ioctl %08lX", code);
    return Complete(ov, ret, 0);
}

// ================================================================ kernel32 hooks
static HANDLE(WINAPI* o_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE(WINAPI* o_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static BOOL(WINAPI* o_DeviceIoControl)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL(WINAPI* o_CloseHandle)(HANDLE);
static BOOL(WINAPI* o_GetCommModemStatus)(HANDLE, LPDWORD);
static BOOL(WINAPI* o_GetCommState)(HANDLE, LPDCB);
static BOOL(WINAPI* o_SetCommState)(HANDLE, LPDCB);
static BOOL(WINAPI* o_SetCommTimeouts)(HANDLE, LPCOMMTIMEOUTS);
static BOOL(WINAPI* o_SetupComm)(HANDLE, DWORD, DWORD);
static BOOL(WINAPI* o_PurgeComm)(HANDLE, DWORD);
static BOOL(WINAPI* o_ClearCommError)(HANDLE, LPDWORD, LPCOMSTAT);
static BOOL(WINAPI* o_CancelIo)(HANDLE);
static BOOL(WINAPI* o_GetOverlappedResult)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
static BOOL(WINAPI* o_ReadFile)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL(WINAPI* o_WriteFile)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);

static bool EndsWithI(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && _stricmp(s.c_str() + s.size() - n, suf) == 0;
}

static HANDLE WINAPI Hook_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl) {
    std::string n = name ? name : "";
    if (_stricmp(n.c_str(), "\\\\.\\mxjvs") == 0 || _stricmp(n.c_str(), "COM4") == 0 ||
        _stricmp(n.c_str(), "\\\\.\\COM4") == 0) {
        Log("open %s -> emulated JVS I/O board", n.c_str());
        return NewFakeHandle(Dev::Jvs);
    }
    if (n == kSmBusPath) {
        Log("open SMBus -> emulated (EEPROM + DIP switches)");
        return NewFakeHandle(Dev::SmBus);
    }
    if (EndsWithI(n, "GAME.INI") && (n.size() == 8 || n[n.size() - 9] == '\\' || n[n.size() - 9] == '/')) {
        std::string p = RuntimeGameIniPath();
        Log("open %s -> %s", n.c_str(), p.c_str());
        return o_CreateFileA(p.c_str(), acc, share, sa, disp, flags, tmpl);
    }
    HANDLE h = o_CreateFileA(name, acc, share, sa, disp, flags, tmpl);
    if (h != INVALID_HANDLE_VALUE) {
        // Boot/attract timeline for tuning the fast boot: scenes, movies, stage scripts.
        std::string l = n;
        for (auto& ch : l) ch = (char)tolower((unsigned char)ch);
        if (l.find("scene") != std::string::npos || l.find(".sfd") != std::string::npos ||
            l.find(".avi") != std::string::npos || l.find("label") != std::string::npos)
            Log("load %s", n.c_str());
    }
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        static int logged = 0;
        if (logged++ < 200) Log("open %s failed (%lu)", n.c_str(), e);
        SetLastError(e);
    }
    return h;
}

static HANDLE WINAPI Hook_CreateFileW(LPCWSTR wname, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl) {
    std::wstring w = wname ? wname : L"";
    if (_wcsicmp(w.c_str(), L"\\\\.\\mxsram") == 0) {
        std::string p = g_portDir + "sram.bin";
        HANDLE h = o_CreateFileA(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Log("open \\\\.\\mxsram -> %s (%s)", p.c_str(), h != INVALID_HANDLE_VALUE ? "ok" : "FAILED");
        if (h != INVALID_HANDLE_VALUE) {
            std::lock_guard<std::mutex> l(s_devLock);
            s_devs[h] = Dev::Sram;
        }
        return h;
    }
    if (_wcsicmp(w.c_str(), L"\\\\.\\mxsuperio") == 0 || _wcsicmp(w.c_str(), L"\\\\.\\columba") == 0) {
        SetLastError(ERROR_FILE_NOT_FOUND); // hardware monitor / DMI: not needed, the game tolerates absence
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = o_CreateFileW(wname, acc, share, sa, disp, flags, tmpl);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        char a[MAX_PATH * 2];
        WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, a, sizeof(a), nullptr, nullptr);
        static int loggedW = 0;
        if (loggedW++ < 200) Log("openW %s failed (%lu)", a, e);
        SetLastError(e);
    }
    return h;
}

static BOOL WINAPI Hook_DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD inSz, LPVOID out, DWORD outSz,
                                        LPDWORD ret, LPOVERLAPPED ov) {
    Dev d;
    if (DevOf(h, d)) {
        switch (d) {
        case Dev::Jvs: return JvsIoctl(code, in, inSz, out, outSz, ret, ov);
        case Dev::SmBus: return SmBusIoctl(code, in, inSz, out, outSz, ret, ov);
        case Dev::Sram: return SramIoctl(code, out, outSz, ret, ov);
        }
    }
    return o_DeviceIoControl(h, code, in, inSz, out, outSz, ret, ov);
}

static BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    {
        std::lock_guard<std::mutex> l(s_devLock);
        s_devs.erase(h);
    }
    return o_CloseHandle(h);
}

static BOOL WINAPI Hook_GetCommModemStatus(HANDLE h, LPDWORD st) {
    Dev d;
    if (DevOf(h, d)) {
        if (st) *st = (d == Dev::Jvs && JvsAddressed()) ? 0x30 : 0x10; // JVS sense line
        return TRUE;
    }
    return o_GetCommModemStatus(h, st);
}
static BOOL WINAPI Hook_GetCommState(HANDLE h, LPDCB dcb) {
    if (IsFake(h)) {
        if (dcb) {
            DWORD len = dcb->DCBlength;
            memset(dcb, 0, sizeof(DCB));
            dcb->DCBlength = len ? len : sizeof(DCB);
            dcb->BaudRate = 115200;
            dcb->ByteSize = 8;
        }
        return TRUE;
    }
    return o_GetCommState(h, dcb);
}
static BOOL WINAPI Hook_SetCommState(HANDLE h, LPDCB dcb) { return IsFake(h) ? TRUE : o_SetCommState(h, dcb); }
static BOOL WINAPI Hook_SetCommTimeouts(HANDLE h, LPCOMMTIMEOUTS t) { return IsFake(h) ? TRUE : o_SetCommTimeouts(h, t); }
static BOOL WINAPI Hook_SetupComm(HANDLE h, DWORD a, DWORD b) { return IsFake(h) ? TRUE : o_SetupComm(h, a, b); }
static BOOL WINAPI Hook_PurgeComm(HANDLE h, DWORD f) { return IsFake(h) ? TRUE : o_PurgeComm(h, f); }
static BOOL WINAPI Hook_ClearCommError(HANDLE h, LPDWORD e, LPCOMSTAT s) {
    if (IsFake(h)) {
        if (e) *e = 0;
        if (s) memset(s, 0, sizeof(COMSTAT));
        return TRUE;
    }
    return o_ClearCommError(h, e, s);
}
static BOOL WINAPI Hook_CancelIo(HANDLE h) { return IsFake(h) ? TRUE : o_CancelIo(h); }
static BOOL WINAPI Hook_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD n, BOOL wait) {
    if (IsFake(h)) {
        if (n) *n = ov ? (DWORD)ov->InternalHigh : 0;
        return TRUE;
    }
    return o_GetOverlappedResult(h, ov, n, wait);
}
static int s_rwLogged = 0;
static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD r, LPOVERLAPPED ov) {
    if (IsFake(h)) {
        if (s_rwLogged++ < 10) Log("unexpected ReadFile on emulated device");
        return Complete(ov, r, 0);
    }
    return o_ReadFile(h, buf, n, r, ov);
}
static BOOL WINAPI Hook_WriteFile(HANDLE h, LPCVOID buf, DWORD n, LPDWORD w, LPOVERLAPPED ov) {
    if (IsFake(h)) {
        if (s_rwLogged++ < 10) Log("unexpected WriteFile on emulated device");
        return Complete(ov, w, n);
    }
    return o_WriteFile(h, buf, n, w, ov);
}

// ================================================================ setupapi hooks (SMBus discovery)
static const HDEVINFO kFakeDevInfo = (HDEVINFO)(uintptr_t)0x5EA6A000;
static HDEVINFO WINAPI Hook_SetupDiGetClassDevsA(const GUID*, PCSTR, HWND, DWORD) { return kFakeDevInfo; }
static BOOL WINAPI Hook_SetupDiEnumDeviceInterfaces(HDEVINFO, PSP_DEVINFO_DATA, const GUID* g, DWORD idx,
                                                    PSP_DEVICE_INTERFACE_DATA data) {
    if (idx != 0 || !data) {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    if (g) data->InterfaceClassGuid = *g;
    data->Flags = SPINT_ACTIVE;
    data->Reserved = 0;
    return TRUE;
}
static BOOL WINAPI Hook_SetupDiGetDeviceInterfaceDetailA(HDEVINFO, PSP_DEVICE_INTERFACE_DATA,
                                                         PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail, DWORD size,
                                                         PDWORD required, PSP_DEVINFO_DATA) {
    DWORD need = (DWORD)(offsetof(SP_DEVICE_INTERFACE_DETAIL_DATA_A, DevicePath) + sizeof(kSmBusPath));
    if (required) *required = need;
    if (!detail || size < need) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(detail->DevicePath, kSmBusPath, sizeof(kSmBusPath));
    return TRUE;
}
static BOOL WINAPI Hook_SetupDiDestroyDeviceInfoList(HDEVINFO) { return TRUE; }
static HDEVINFO(WINAPI* o_SetupDiGetClassDevsA)(const GUID*, PCSTR, HWND, DWORD);
static BOOL(WINAPI* o_SetupDiEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID*, DWORD, PSP_DEVICE_INTERFACE_DATA);
static BOOL(WINAPI* o_SetupDiGetDeviceInterfaceDetailA)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_A, DWORD, PDWORD, PSP_DEVINFO_DATA);
static BOOL(WINAPI* o_SetupDiDestroyDeviceInfoList)(HDEVINFO);

// ================================================================ user32 hooks (window)
static HWND(WINAPI* o_CreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static LONG(WINAPI* o_SetWindowLongA)(HWND, int, LONG);
static BOOL(WINAPI* o_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static BOOL(WINAPI* o_ShowWindow)(HWND, int);

static DWORD WindowStyle() {
    return g_cfg.displayMode == DisplayMode::Windowed ? (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE)
                                                      : (WS_POPUP | WS_VISIBLE);
}
static RECT WindowRect() {
    RECT m = g_layout.monitorRect;
    if (g_cfg.displayMode != DisplayMode::Windowed) return m;
    RECT r = {0, 0, g_layout.clientW, g_layout.clientH};
    AdjustWindowRectEx(&r, WindowStyle(), FALSE, 0);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = m.left + ((m.right - m.left) - w) / 2, y = m.top + ((m.bottom - m.top) - h) / 2;
    return RECT{x, y, x + w, y + h};
}

void ApplyWindowPlacement(HWND hwnd) {
    RECT r = WindowRect();
    o_SetWindowLongA(hwnd, GWL_STYLE, (LONG)WindowStyle());
    o_SetWindowLongA(hwnd, GWL_EXSTYLE, 0);
    o_SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static HWND WINAPI Hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                                        HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    if (!parent && !g_hwnd && style != 0) { // the game's main window (a second, hidden helper window has style 0)
        RECT r = WindowRect();
        HWND hw = o_CreateWindowExA(0, cls, "Dream Raiders", WindowStyle(), r.left, r.top, r.right - r.left,
                                    r.bottom - r.top, parent, menu, inst, param);
        g_hwnd = hw;
        Log("game window %p created: %ldx%ld at %ld,%ld (%s)", hw, r.right - r.left, r.bottom - r.top, r.left, r.top,
            g_cfg.displayMode == DisplayMode::Windowed ? "windowed" : "borderless");
        return hw;
    }
    return o_CreateWindowExA(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
}
static LONG WINAPI Hook_SetWindowLongA(HWND h, int idx, LONG v) {
    if (h == g_hwnd && (idx == GWL_STYLE || idx == GWL_EXSTYLE)) return GetWindowLongA(h, idx);
    return o_SetWindowLongA(h, idx, v);
}
static BOOL WINAPI Hook_SetWindowPos(HWND h, HWND after, int x, int y, int w, int hh, UINT f) {
    if (h == g_hwnd) return TRUE;
    return o_SetWindowPos(h, after, x, y, w, hh, f);
}
// The game reads the keyboard as a leftover debug input (e.g. Home = TEST). Players use the port's own
// controls, so give it an empty keyboard.
static BOOL(WINAPI* o_GetKeyboardState)(PBYTE);
static BOOL WINAPI Hook_GetKeyboardState(PBYTE keys) {
    if (keys) memset(keys, 0, 256);
    return TRUE;
}

// The game hides the cursor by calling SetCursor(NULL); keep the arrow while one of our menus is open.
static HCURSOR(WINAPI* o_SetCursor)(HCURSOR);
static HCURSOR WINAPI Hook_SetCursor(HCURSOR c) {
    if (g_overlayOpen) return o_SetCursor(LoadCursor(nullptr, IDC_ARROW));
    return o_SetCursor(c);
}
void EnsureMenuCursor() {
    if (!o_SetCursor) o_SetCursor = SetCursor;
    if (!g_overlayOpen || !g_hwnd || GetForegroundWindow() != g_hwnd) return;
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING)) {
        int guard = 0;
        while (ShowCursor(TRUE) < 0 && ++guard < 64) {}
    }
    POINT p;
    RECT r;
    if (GetCursorPos(&p) && GetWindowRect(g_hwnd, &r) && PtInRect(&r, p)) o_SetCursor(LoadCursor(nullptr, IDC_ARROW));
}

static BOOL WINAPI Hook_ShowWindow(HWND h, int cmd) {
    if (h == g_hwnd && cmd != SW_HIDE && cmd != SW_MINIMIZE) cmd = SW_SHOW;
    return o_ShowWindow(h, cmd);
}

// ================================================================ DirectSound (pause support)
#include <mmsystem.h>
#include <dsound.h>
#include <set>
static std::mutex s_sndLock;
static std::set<IDirectSoundBuffer*> s_buffers;
static std::map<IDirectSoundBuffer*, DWORD> s_pausedBuffers;
typedef HRESULT(STDMETHODCALLTYPE* CreateSoundBuffer_t)(IDirectSound8*, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN);
typedef HRESULT(STDMETHODCALLTYPE* Duplicate_t)(IDirectSound8*, LPDIRECTSOUNDBUFFER, LPDIRECTSOUNDBUFFER*);
typedef ULONG(STDMETHODCALLTYPE* Release_t)(IDirectSoundBuffer*);
static CreateSoundBuffer_t o_CreateSoundBuffer;
static Duplicate_t o_DuplicateSoundBuffer;
static Release_t o_BufRelease;
typedef HRESULT(STDMETHODCALLTYPE* Play_t)(IDirectSoundBuffer*, DWORD, DWORD, DWORD);
static Play_t o_BufPlay;
static bool s_audioPaused = false;
static HRESULT(WINAPI* o_DirectSoundCreate8)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);

static void PatchVt(void* obj, int idx, void* hook, void** orig) {
    void** vt = *(void***)obj;
    if (vt[idx] == hook) return;
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old);
    *orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
}
static ULONG STDMETHODCALLTYPE Hook_BufRelease(IDirectSoundBuffer* b) {
    ULONG r = o_BufRelease(b);
    if (r == 0) {
        std::lock_guard<std::mutex> l(s_sndLock);
        s_buffers.erase(b);
        s_pausedBuffers.erase(b);
    }
    return r;
}
// While paused, the audio library's own thread would just restart stopped buffers; hold those requests
// and replay them on resume.
static HRESULT STDMETHODCALLTYPE Hook_BufPlay(IDirectSoundBuffer* b, DWORD r, DWORD prio, DWORD flags) {
    {
        std::lock_guard<std::mutex> l(s_sndLock);
        if (s_audioPaused) {
            s_pausedBuffers[b] = DSBSTATUS_PLAYING | ((flags & DSBPLAY_LOOPING) ? DSBSTATUS_LOOPING : 0);
            return DS_OK;
        }
    }
    return o_BufPlay(b, r, prio, flags);
}
static void Track(IDirectSoundBuffer* b) {
    if (!b) return;
    PatchVt(b, 2, (void*)Hook_BufRelease, (void**)&o_BufRelease);
    PatchVt(b, 12, (void*)Hook_BufPlay, (void**)&o_BufPlay);
    std::lock_guard<std::mutex> l(s_sndLock);
    s_buffers.insert(b);
}
static HRESULT STDMETHODCALLTYPE Hook_CreateSoundBuffer(IDirectSound8* ds, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN u) {
    HRESULT hr = o_CreateSoundBuffer(ds, d, out, u);
    if (SUCCEEDED(hr) && out && *out && d && !(d->dwFlags & DSBCAPS_PRIMARYBUFFER)) Track(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_DuplicateSoundBuffer(IDirectSound8* ds, LPDIRECTSOUNDBUFFER src, LPDIRECTSOUNDBUFFER* out) {
    HRESULT hr = o_DuplicateSoundBuffer(ds, src, out);
    if (SUCCEEDED(hr) && out) Track(*out);
    return hr;
}
static HRESULT WINAPI Hook_DirectSoundCreate8(LPCGUID g, LPDIRECTSOUND8* out, LPUNKNOWN u) {
    HRESULT hr = o_DirectSoundCreate8(g, out, u);
    if (SUCCEEDED(hr) && out && *out) {
        PatchVt(*out, 3, (void*)Hook_CreateSoundBuffer, (void**)&o_CreateSoundBuffer);
        PatchVt(*out, 5, (void*)Hook_DuplicateSoundBuffer, (void**)&o_DuplicateSoundBuffer);
        Log("DirectSound created (pause can stop audio)");
    }
    return hr;
}

void AudioPause(bool pause) {
    std::lock_guard<std::mutex> l(s_sndLock);
    if (pause) {
        s_pausedBuffers.clear();
        for (IDirectSoundBuffer* b : s_buffers) {
            DWORD st = 0;
            if (SUCCEEDED(b->GetStatus(&st)) && (st & DSBSTATUS_PLAYING)) {
                s_pausedBuffers[b] = st;
                b->Stop();
            }
        }
        s_audioPaused = true;
        Log("audio paused: %u of %u buffers were playing", (unsigned)s_pausedBuffers.size(), (unsigned)s_buffers.size());
    } else {
        s_audioPaused = false;
        for (auto& kv : s_pausedBuffers)
            if (o_BufPlay) o_BufPlay(kv.first, 0, 0, (kv.second & DSBSTATUS_LOOPING) ? DSBPLAY_LOOPING : 0);
        s_pausedBuffers.clear();
    }
}

static void* PatchImportOrdinal(const char* dll, WORD ordinal, void* hook) {
    auto base = (uint8_t*)GetModuleHandleA(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) || IMAGE_ORDINAL(names->u1.Ordinal) != ordinal) continue;
            void* orig = (void*)addrs->u1.Function;
            DWORD old;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            addrs->u1.Function = (uintptr_t)hook;
            VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
            return orig;
        }
    }
    Log("import %s!#%u not found", dll, ordinal);
    return nullptr;
}

// ================================================================ install
void InstallImportHooks() {
    EepromLoad();
    memset(s_pcaRegs, 0, sizeof(s_pcaRegs));
    const char* k = "KERNEL32.dll";
    HOOK(k, CreateFileA);
    HOOK(k, CreateFileW);
    HOOK(k, DeviceIoControl);
    HOOK(k, CloseHandle);
    HOOK(k, GetCommModemStatus);
    HOOK(k, GetCommState);
    HOOK(k, SetCommState);
    HOOK(k, SetCommTimeouts);
    HOOK(k, SetupComm);
    HOOK(k, PurgeComm);
    HOOK(k, ClearCommError);
    HOOK(k, CancelIo);
    HOOK(k, GetOverlappedResult);
    HOOK(k, ReadFile);
    HOOK(k, WriteFile);
    const char* s = "SETUPAPI.dll";
    HOOK(s, SetupDiGetClassDevsA);
    HOOK(s, SetupDiEnumDeviceInterfaces);
    HOOK(s, SetupDiGetDeviceInterfaceDetailA);
    HOOK(s, SetupDiDestroyDeviceInfoList);
    const char* u = "USER32.dll";
    HOOK(u, CreateWindowExA);
    HOOK(u, SetWindowLongA);
    HOOK(u, SetWindowPos);
    HOOK(u, ShowWindow);
    HOOK(u, SetCursor);
    HOOK(u, GetKeyboardState);
    o_DirectSoundCreate8 = (decltype(o_DirectSoundCreate8))PatchImportOrdinal("DSOUND.dll", 11, (void*)Hook_DirectSoundCreate8);
    if (!o_DirectSoundCreate8) { // put it back if the lookup failed
        Log("DirectSound hook unavailable");
    }
    if (!o_SetCursor) o_SetCursor = SetCursor;
    // Fall back to the real functions if a lookup failed, so we never call through a null pointer.
    if (!o_SetWindowLongA) o_SetWindowLongA = SetWindowLongA;
    if (!o_SetWindowPos) o_SetWindowPos = SetWindowPos;
    if (!o_CreateFileA) o_CreateFileA = CreateFileA;
    Log("import hooks installed");
}
