#include "common.h"
#include <stdio.h>
#include <stdarg.h>

static CRITICAL_SECTION s_cs;
static HANDLE s_file = INVALID_HANDLE_VALUE;
static DWORD s_start;

void LogInit(const std::string& path) {
    InitializeCriticalSection(&s_cs);
    s_start = GetTickCount();
    // Append (the game relaunches itself to enter/leave test mode); start over once it gets large.
    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool big = GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 4u * 1024 * 1024);
    s_file = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         big ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s_file != INVALID_HANDLE_VALUE) {
        DWORD w;
        const char sep[] = "\r\n==============================================================\r\n";
        WriteFile(s_file, sep, sizeof(sep) - 1, &w, nullptr);
    }
}

static void WriteRaw(const char* s, size_t n) {
    if (s_file == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(s_file, s, (DWORD)n, &w, nullptr);
}

void Log(const char* fmt, ...) {
    char buf[2048];
    DWORD t = GetTickCount() - s_start;
    int n = snprintf(buf, sizeof(buf), "[%6lu.%03lu] ", t / 1000, t % 1000);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&s_cs);
    WriteRaw(buf, n);
    LeaveCriticalSection(&s_cs);
}

void LogHex(const char* tag, const void* data, size_t len) {
    std::string s;
    const uint8_t* p = (const uint8_t*)data;
    char b[4];
    for (size_t i = 0; i < len && i < 256; i++) {
        snprintf(b, sizeof(b), "%02X ", p[i]);
        s += b;
    }
    Log("%s (%u): %s", tag, (unsigned)len, s.c_str());
}
