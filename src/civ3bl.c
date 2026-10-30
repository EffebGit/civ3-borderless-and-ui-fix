// civ3bl.c -- Civ3 borderless + UI-scaling layer for Sid Meier's Civilization III: Complete
//
// Loaded as an app-local opengl32.dll proxy (every real export is forwarded to
// opengl32_real.dll by the generated .def; only the wgl/gl entry points we
// actually touch are implemented here).  Civ3 statically imports OPENGL32, so
// this DLL runs before any engine code, which buys us:
//
//   * per-monitor-v2 DPI awareness before Civ3 creates its window (kills the
//     blurry 150% bitmap stretch Windows applies to the stock game)
//   * a consistent "virtual display": the engine is told the screen is
//     LogicalWidth x LogicalHeight, so it renders its UI at a size that is
//     readable on a 4K panel instead of 1:1 physical pixels
//   * the engine's single full-frame GDI present (BitBlt of its DIB back
//     buffer onto the window DC) is re-pointed at the whole client area and
//     scaled up, so map + UI + everything drawn by the engine scales together
//   * the window is forced to a borderless popup covering one monitor
//   * desktop mode switches are refused, so alt-tab never drops your desktop
//
// No byte of Civ3Conquests.exe is modified.  Delete opengl32.dll,
// opengl32_real.dll and civ3bl.ini to get the stock game back.

#include "civ3bl_common.h"
#include <stdlib.h>

Civ3Cfg g_cfg;

static CRITICAL_SECTION g_cs;
static FILE *g_fp;
static HMODULE g_real;
static char g_exeDir[MAX_PATH];

// ------------------------------------------------------------------ logging --
void logmsg(const char *fmt, ...)
{
    va_list ap;
    if (g_cfg.logLevel <= 0) return;
    EnterCriticalSection(&g_cs);
    if (!g_fp) {
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%s\\civ3bl.log", g_exeDir);
        g_fp = fopen(path, "w");
    }
    if (g_fp) {
        va_start(ap, fmt); vfprintf(g_fp, fmt, ap); va_end(ap);
        fputc('\n', g_fp); fflush(g_fp);
    }
    LeaveCriticalSection(&g_cs);
}

static void *realproc(const char *name)
{
    if (!g_real) {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat(sys, "\\opengl32.dll");        /* 32-bit process -> SysWOW64 */
        g_real = LoadLibraryA(sys);
    }
    return g_real ? (void*)GetProcAddress(g_real, name) : NULL;
}

static void detach(void)
{
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
}

// -------------------------------------------------------------------- config --
static int ini_int(const char *file, const char *sec, const char *key, int def)
{
    return (int)GetPrivateProfileIntA(sec, key, def, file);
}

void cfg_load(void)
{
    char ini[MAX_PATH], fit[32];
    _snprintf(ini, sizeof(ini), "%s\\civ3bl.ini", g_exeDir);
    g_cfg.enabled        = ini_int(ini, "Civ3Borderless", "Enabled", 1);
    g_cfg.logLevel       = ini_int(ini, "Civ3Borderless", "LogLevel", 0);
    g_cfg.dpiAware       = ini_int(ini, "Civ3Borderless", "DpiAware", 1);
    g_cfg.borderless     = ini_int(ini, "Civ3Borderless", "Borderless", 1);
    g_cfg.scalePresent   = ini_int(ini, "Civ3Borderless", "ScalePresent", 1);
    g_cfg.logicalW       = ini_int(ini, "Civ3Borderless", "LogicalWidth", 1920);
    g_cfg.logicalH       = ini_int(ini, "Civ3Borderless", "LogicalHeight", 1080);
    g_cfg.monitorOverride = ini_int(ini, "Civ3Borderless", "Monitor", 0);
    g_cfg.verboseBlit    = ini_int(ini, "Civ3Borderless", "LogBlits", 0);
    g_cfg.lockCursor     = ini_int(ini, "Civ3Borderless", "LockCursor", 1);
    g_cfg.lieScreen      = ini_int(ini, "Civ3Borderless", "LieScreen", 1);
    g_cfg.lieClientRect  = ini_int(ini, "Civ3Borderless", "LieClientRect", 1);
    g_cfg.lieDeviceCaps  = ini_int(ini, "Civ3Borderless", "LieDeviceCaps", 1);
    GetPrivateProfileStringA("Civ3Borderless", "Fit", "integer", fit, sizeof(fit), ini);
    strncpy(g_cfg.fit, fit, sizeof(g_cfg.fit) - 1);
    g_cfg.fit[sizeof(g_cfg.fit) - 1] = 0;
}

// ------------------------------------------------------------------- state ----
static HWND  g_gameWnd;
static RECT  g_monitorRect;
static int   g_logicalW, g_logicalH;
static HDC   g_screenDc[8];

/* destination rectangle inside the real client area, recomputed per present */
static int    g_frameW, g_frameH;    /* size the engine actually renders */
static double g_scale = 1.0;
static int    g_dx, g_dy;
static int    g_cw, g_ch;
static int    g_nPresent;

// --------------------------------------------------------------- IAT patching --
typedef void (*HOOKFN)(void);

static HOOKFN hook_for(const char *dll, const char *name);

static void patch_module(HMODULE mod)
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    if (!mod) return;
    dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS*)((char*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) return;

    imp = (IMAGE_IMPORT_DESCRIPTOR*)((char*)mod +
          nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

    for (; imp->Name; imp++) {
        const char *dll = (char*)mod + imp->Name;
        IMAGE_THUNK_DATA *oft = (IMAGE_THUNK_DATA*)((char*)mod +
                                (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA*)((char*)mod + imp->FirstThunk);
        if (!oft || !ft) continue;
        for (; oft->u1.AddressOfData; oft++, ft++) {
            const char *name;
            HOOKFN h;
            DWORD old;
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            name = (char*)((IMAGE_IMPORT_BY_NAME*)((char*)mod + oft->u1.AddressOfData))->Name;
            h = hook_for(dll, name);
            if (!h) continue;
            if (ft->u1.Function == (DWORD_PTR)h) continue;
            if (VirtualProtect(ft, sizeof(void*), PAGE_READWRITE, &old)) {
                ft->u1.Function = (DWORD_PTR)h;
                VirtualProtect(ft, sizeof(void*), old, &old);
            }
        }
    }
}

static void patch_all(void)
{
    HANDLE snap;
    MODULEENTRY32 me;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, "opengl32.dll") == 0) continue;
            patch_module((HMODULE)me.modBaseAddr);
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
}

// ------------------------------------------------------------------- helpers --
static BOOL is_game_wnd(HWND h)
{
    DWORD pid; char cls[64];
    if (!h || !IsWindow(h)) return FALSE;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return FALSE;
    GetClassNameA(h, cls, sizeof(cls));
    return !strcmp(cls, "JackalClass");
}

static BOOL is_screen_dc(HDC hdc)
{
    int i;
    for (i = 0; i < 8; i++) if (g_screenDc[i] == hdc && hdc) return TRUE;
    return FALSE;
}

static BOOL is_game_dc(HDC hdc)
{
    if (!hdc) return FALSE;
    return is_game_wnd(WindowFromDC(hdc));
}

/* Recompute the destination rect for the engine's frame: preserve aspect ratio,
   optionally snap to an integer scale, centre inside the real client area. */
static void update_fit(void)
{
    RECT c;
    double s, sw, sh;
    if (!g_gameWnd || !GetClientRect(g_gameWnd, &c)) return;
    g_cw = c.right - c.left;
    g_ch = c.bottom - c.top;
    if (g_cw <= 0 || g_ch <= 0) return;

    sw = (double)g_cw / g_frameW;
    sh = (double)g_ch / g_frameH;
    s = sw < sh ? sw : sh;
    if (!strcmp(g_cfg.fit, "stretch")) {
        g_scale = 0.0;                     /* handled specially in map_frame */
        g_dx = 0; g_dy = 0;
        return;
    }
    if (!strcmp(g_cfg.fit, "integer")) {
        int i = (int)(s + 0.001);
        if (i >= 1) s = i;
    }
    g_scale = s;
    g_dx = (int)((g_cw - g_frameW * s) / 2.0 + 0.5);
    g_dy = (int)((g_ch - g_frameH * s) / 2.0 + 0.5);
}

/* Map a rectangle from engine (logical) coordinates into client pixels. */
static void map_rect(int *x, int *y, int *w, int *h)
{
    if (g_scale <= 0.0) {                    /* stretch */
        *x = (int)((long long)*x * g_cw / g_frameW);
        *y = (int)((long long)*y * g_ch / g_frameH);
        *w = (int)((long long)*w * g_cw / g_frameW);
        *h = (int)((long long)*h * g_ch / g_frameH);
        return;
    }
    *x = g_dx + (int)(*x * g_scale);
    *y = g_dy + (int)(*y * g_scale);
    *w = (int)(*w * g_scale + 0.5);
    *h = (int)(*h * g_scale + 0.5);
}

/* Inverse map for pointer coordinates. */
static void unmap_point(int *x, int *y)
{
    double s = g_scale > 0.0 ? g_scale : (double)g_cw / g_frameW;
    *x = (int)((*x - g_dx) / s);
    *y = (int)((*y - g_dy) / s);
}

// -------------------------------------------------------------------- hooks ---
typedef int  (WINAPI *PFN_GetSystemMetrics)(int);
typedef BOOL (WINAPI *PFN_GetClientRect)(HWND, LPRECT);
typedef int  (WINAPI *PFN_GetDeviceCaps)(HDC, int);
typedef BOOL (WINAPI *PFN_GetWindowRect)(HWND, LPRECT);
typedef HDC  (WINAPI *PFN_GetDC)(HWND);
typedef int  (WINAPI *PFN_ReleaseDC)(HWND, HDC);
typedef BOOL (WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef int  (WINAPI *PFN_SetStretchBltMode)(HDC, int);
typedef BOOL (WINAPI *PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL (WINAPI *PFN_StretchBlt)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
typedef int  (WINAPI *PFN_SetDIBitsToDevice)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const void*, const BITMAPINFO*, UINT);
typedef int  (WINAPI *PFN_StretchDIBits)(HDC, int, int, int, int, int, int, int, int, const void*, const BITMAPINFO*, UINT, DWORD);
typedef BOOL (WINAPI *PFN_GetCursorPos)(LPPOINT);
typedef BOOL (WINAPI *PFN_ScreenToClient)(HWND, LPPOINT);
typedef BOOL (WINAPI *PFN_ClientToScreen)(HWND, LPPOINT);
typedef BOOL (WINAPI *PFN_ClipCursor)(const RECT*);
typedef LONG (WINAPI *PFN_ChangeDisplaySettingsA)(LPDEVMODEA, DWORD);
typedef BOOL (WINAPI *PFN_EnumDisplaySettingsA)(LPCSTR, DWORD, LPDEVMODEA);

static PFN_GetSystemMetrics p_GetSystemMetrics;
static PFN_GetClientRect    p_GetClientRect;
static PFN_GetDeviceCaps    p_GetDeviceCaps;
static PFN_GetWindowRect    p_GetWindowRect;
static PFN_GetDC            p_GetDC;
static PFN_ReleaseDC        p_ReleaseDC;
static PFN_SetWindowPos     p_SetWindowPos;
static PFN_SetStretchBltMode p_SetStretchBltMode;
static PFN_BitBlt           p_BitBlt;
static PFN_StretchBlt       p_StretchBlt;
static PFN_SetDIBitsToDevice p_SetDIBitsToDevice;
static PFN_StretchDIBits    p_StretchDIBits;
static PFN_GetCursorPos     p_GetCursorPos;
static PFN_ScreenToClient   p_ScreenToClient;
static PFN_ClientToScreen   p_ClientToScreen;
static PFN_ClipCursor       p_ClipCursor;
static PFN_ChangeDisplaySettingsA p_ChangeDSA;
static PFN_EnumDisplaySettingsA p_EnumDSA;
static int g_nEnum, g_nWndRect, g_nSpw;

static int g_sizes[32][2], g_nSizes;
static int g_nPosMsg, g_nSizeMsg, g_nMsg, g_nStretch, g_nDib, g_nStretchDib, g_nGetDC, g_nRestyle, g_nCur, g_nStc, g_nCts;

static int WINAPI H_GetSystemMetrics(int idx)
{
    int v;
    if (!p_GetSystemMetrics) p_GetSystemMetrics = (PFN_GetSystemMetrics)GetProcAddress(GetModuleHandleA("user32.dll"), "GetSystemMetrics");
    v = p_GetSystemMetrics(idx);
    if (!g_cfg.scalePresent || !g_cfg.lieScreen) return v;
    if (idx == SM_CXSCREEN) return g_logicalW;
    if (idx == SM_CYSCREEN) return g_logicalH;
    return v;
}

static BOOL WINAPI H_GetClientRect(HWND hwnd, LPRECT r)
{
    BOOL ok;
    if (!p_GetClientRect) p_GetClientRect = (PFN_GetClientRect)GetProcAddress(GetModuleHandleA("user32.dll"), "GetClientRect");
    ok = p_GetClientRect(hwnd, r);
    if (ok && g_cfg.scalePresent && g_cfg.lieClientRect && g_gameWnd && hwnd == g_gameWnd) {
        if (g_nGetDC++ < 3)
            logmsg("GetClientRect(game): real %ldx%ld -> reporting %dx%d", r->right, r->bottom, g_logicalW, g_logicalH);
        r->right = g_logicalW;
        r->bottom = g_logicalH;
    }
    return ok;
}

static HDC WINAPI H_GetDC(HWND hwnd)
{
    HDC dc;
    int i;
    if (!p_GetDC) p_GetDC = (PFN_GetDC)GetProcAddress(GetModuleHandleA("user32.dll"), "GetDC");
    dc = p_GetDC(hwnd);
    if (dc && (!hwnd || hwnd == GetDesktopWindow())) {
        for (i = 0; i < 8; i++) if (!g_screenDc[i]) { g_screenDc[i] = dc; break; }
    }
    return dc;
}

static int WINAPI H_ReleaseDC(HWND hwnd, HDC dc)
{
    int i;
    if (!p_ReleaseDC) p_ReleaseDC = (PFN_ReleaseDC)GetProcAddress(GetModuleHandleA("user32.dll"), "ReleaseDC");
    for (i = 0; i < 8; i++) if (g_screenDc[i] == dc) g_screenDc[i] = NULL;
    return p_ReleaseDC(hwnd, dc);
}

static BOOL WINAPI H_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    if (!p_SetWindowPos) p_SetWindowPos = (PFN_SetWindowPos)GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowPos");
    if (g_cfg.borderless && is_game_wnd(hwnd)) {
        /* The window is always the full monitor; the engine's own resize requests
           are answered with the logical size through WM_SIZE instead. */
        if (g_cfg.logLevel > 0 && g_nSpw++ < 6)
            logmsg("SetWindowPos(game) %dx%d -> monitor", cx, cy);
        return p_SetWindowPos(hwnd, HWND_TOP, g_monitorRect.left, g_monitorRect.top,
                              g_monitorRect.right - g_monitorRect.left,
                              g_monitorRect.bottom - g_monitorRect.top,
                              SWP_NOOWNERZORDER | (flags & (SWP_SHOWWINDOW | SWP_NOACTIVATE)));
    }
    return p_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

static BOOL WINAPI H_GetWindowRect(HWND hwnd, LPRECT r)
{
    BOOL ok;
    if (!p_GetWindowRect) p_GetWindowRect = (PFN_GetWindowRect)GetProcAddress(GetModuleHandleA("user32.dll"), "GetWindowRect");
    ok = p_GetWindowRect(hwnd, r);
    if (ok && g_cfg.scalePresent && g_cfg.lieClientRect && is_game_wnd(hwnd)) {
        /* A borderless popup has client size == window size; report the same
           logical size from both APIs or the engine computes mismatched strides. */
        if (g_nWndRect++ < 3)
            logmsg("GetWindowRect(game): real %ldx%ld -> %dx%d", r->right - r->left, r->bottom - r->top, g_logicalW, g_logicalH);
        r->right = r->left + g_logicalW;
        r->bottom = r->top + g_logicalH;
    }
    return ok;
}

static int WINAPI H_GetDeviceCaps(HDC hdc, int idx)
{
    int v;
    if (!p_GetDeviceCaps) p_GetDeviceCaps = (PFN_GetDeviceCaps)GetProcAddress(GetModuleHandleA("gdi32.dll"), "GetDeviceCaps");
    v = p_GetDeviceCaps(hdc, idx);
    if (g_cfg.scalePresent && g_cfg.lieDeviceCaps && (is_game_dc(hdc) || is_screen_dc(hdc))) {
        if (idx == HORZRES) return g_logicalW;
        if (idx == VERTRES) return g_logicalH;
    }
    return v;
}

static void note_blit(int w, int h)
{
    int i;
    if (w > 0 && h > 0 && w * h > g_frameW * g_frameH) {
        g_frameW = w; g_frameH = h;
        g_scale = 1.0;
        if (g_cfg.logLevel > 0) logmsg("engine frame is now %dx%d", w, h);
    }
    if (g_cfg.logLevel < 1) return;
    for (i = 0; i < g_nSizes; i++)
        if (g_sizes[i][0] == w && g_sizes[i][1] == h) return;
    if (g_nSizes < 32) {
        g_sizes[g_nSizes][0] = w; g_sizes[g_nSizes][1] = h;
        logmsg("blit size #%d: %dx%d", g_nSizes, w, h);
        g_nSizes++;
    }
}

/* Fill the letterbox/pillarbox area so leftover pixels never show through. */
static void paint_bars(HDC dst, int W, int H)
{
    int right = g_dx + W, bottom = g_dy + H;
    if (g_dx > 0) {
        PatBlt(dst, 0, 0, g_dx, g_ch, BLACKNESS);
        PatBlt(dst, right, 0, g_cw - right, g_ch, BLACKNESS);
    }
    if (g_dy > 0) {
        PatBlt(dst, 0, 0, g_cw, g_dy, BLACKNESS);
        PatBlt(dst, 0, bottom, g_cw, g_ch - bottom, BLACKNESS);
    }
}

static BOOL WINAPI H_BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop)
{
    if (!p_BitBlt)   p_BitBlt   = (PFN_BitBlt)GetProcAddress(GetModuleHandleA("gdi32.dll"), "BitBlt");
    if (!p_StretchBlt) p_StretchBlt = (PFN_StretchBlt)GetProcAddress(GetModuleHandleA("gdi32.dll"), "StretchBlt");
    if (g_cfg.scalePresent && is_game_dc(dst)) note_blit(w, h);
    if (g_cfg.scalePresent && is_game_dc(dst)) {
        int X = x, Y = y, W = w, H = h;
        update_fit();
        map_rect(&X, &Y, &W, &H);
        if (!p_SetStretchBltMode) p_SetStretchBltMode = (PFN_SetStretchBltMode)GetProcAddress(GetModuleHandleA("gdi32.dll"), "SetStretchBltMode");
        if (p_SetStretchBltMode) p_SetStretchBltMode(dst, 3 /* COLORONCOLOR: hard pixels, no blur */);
        if (w == g_frameW && h == g_frameH) paint_bars(dst, W, H);
        if (w == g_logicalW && h == g_logicalH && (g_cfg.verboseBlit || g_nPresent++ < 4))
            logmsg("present: %dx%d -> client %dx%d at %d,%d (scale %.3f)", w, h, W, H, X, Y, g_scale);
        return p_StretchBlt(dst, X, Y, W, H, src, sx, sy, w, h, rop);
    }
    return p_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
}

static BOOL WINAPI H_StretchBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop)
{
    if (!p_StretchBlt) p_StretchBlt = (PFN_StretchBlt)GetProcAddress(GetModuleHandleA("gdi32.dll"), "StretchBlt");
    if (g_cfg.scalePresent && is_game_dc(dst)) {
        int X = x, Y = y, W = w, H = h;
        update_fit();
        map_rect(&X, &Y, &W, &H);
        if (!p_SetStretchBltMode) p_SetStretchBltMode = (PFN_SetStretchBltMode)GetProcAddress(GetModuleHandleA("gdi32.dll"), "SetStretchBltMode");
        if (p_SetStretchBltMode) p_SetStretchBltMode(dst, 3);
        if (w == g_frameW && h == g_frameH) paint_bars(dst, W, H);
        return p_StretchBlt(dst, X, Y, W, H, src, sx, sy, sw, sh, rop);
    }
    if (g_nStretch++ < 8 && g_cfg.verboseBlit)
        logmsg("StretchBlt(game=%d) %d,%d %dx%d", is_game_dc(dst), x, y, w, h);
    return p_StretchBlt(dst, x, y, w, h, src, sx, sy, sw, sh, rop);
}

static int WINAPI H_SetDIBitsToDevice(HDC hdc, int x, int y, DWORD w, DWORD h, int sx, int sy,
                                      UINT start, UINT lines, const void *bits, const BITMAPINFO *bi, UINT usage)
{
    if (!p_SetDIBitsToDevice) p_SetDIBitsToDevice = (PFN_SetDIBitsToDevice)GetProcAddress(GetModuleHandleA("gdi32.dll"), "SetDIBitsToDevice");
    if (g_nDib++ < 4 && g_cfg.verboseBlit) logmsg("SetDIBitsToDevice(game=%d) %d,%d %lux%lu", is_game_dc(hdc), x, y, w, h);
    return p_SetDIBitsToDevice(hdc, x, y, w, h, sx, sy, start, lines, bits, bi, usage);
}

static int WINAPI H_StretchDIBits(HDC hdc, int x, int y, int w, int h, int sx, int sy, int sw, int sh,
                                  const void *bits, const BITMAPINFO *bi, UINT usage, DWORD rop)
{
    if (!p_StretchDIBits) p_StretchDIBits = (PFN_StretchDIBits)GetProcAddress(GetModuleHandleA("gdi32.dll"), "StretchDIBits");
    if (g_cfg.scalePresent && is_game_dc(hdc)) {
        int X = x, Y = y, W = w, H = h;
        update_fit();
        map_rect(&X, &Y, &W, &H);
        return p_StretchDIBits(hdc, X, Y, W, H, sx, sy, sw, sh, bits, bi, usage, rop);
    }
    return p_StretchDIBits(hdc, x, y, w, h, sx, sy, sw, sh, bits, bi, usage, rop);
}

static BOOL WINAPI H_ScreenToClient(HWND hwnd, LPPOINT pt)
{
    BOOL ok;
    if (!p_ScreenToClient) p_ScreenToClient = (PFN_ScreenToClient)GetProcAddress(GetModuleHandleA("user32.dll"), "ScreenToClient");
    ok = p_ScreenToClient(hwnd, pt);
    if (ok && g_cfg.scalePresent && g_gameWnd && hwnd == g_gameWnd) {
        int ox = pt->x, oy = pt->y;
        unmap_point(&pt->x, &pt->y);
        if (g_cfg.verboseBlit && ++g_nStc < 60)
            logmsg("ScreenToClient %d,%d -> %d,%d (scale %.2f)", ox, oy, pt->x, pt->y, g_scale);
    }
    return ok;
}

static BOOL WINAPI H_ClientToScreen(HWND hwnd, LPPOINT pt)
{
    if (!p_ClientToScreen) p_ClientToScreen = (PFN_ClientToScreen)GetProcAddress(GetModuleHandleA("user32.dll"), "ClientToScreen");
    if (g_cfg.scalePresent && g_gameWnd && hwnd == g_gameWnd) {
        int x = pt->x, y = pt->y, w = 1, h = 1;
        int ox = pt->x, oy = pt->y;
        map_rect(&x, &y, &w, &h);
        pt->x = x; pt->y = y;
        if (g_cfg.verboseBlit && ++g_nCts < 60) logmsg("ClientToScreen %d,%d -> %d,%d", ox, oy, x, y);
    }
    return p_ClientToScreen(hwnd, pt);
}

static BOOL WINAPI H_GetCursorPos(LPPOINT pt)
{
    BOOL ok;
    if (!p_GetCursorPos) p_GetCursorPos = (PFN_GetCursorPos)GetProcAddress(GetModuleHandleA("user32.dll"), "GetCursorPos");
    ok = p_GetCursorPos(pt);
    if (ok && g_cfg.verboseBlit && ++g_nCur < 60) logmsg("GetCursorPos -> %ld,%ld", pt->x, pt->y);
    return ok;
}

static BOOL WINAPI H_ClipCursor(const RECT *r)
{
    if (!p_ClipCursor) p_ClipCursor = (PFN_ClipCursor)GetProcAddress(GetModuleHandleA("user32.dll"), "ClipCursor");
    if (g_cfg.scalePresent && r && g_gameWnd) return p_ClipCursor(&g_monitorRect);
    return p_ClipCursor(r);
}

static BOOL WINAPI H_EnumDisplaySettingsA(LPCSTR dev, DWORD mode, LPDEVMODEA dm)
{
    BOOL ok;
    if (!p_EnumDSA) p_EnumDSA = (PFN_EnumDisplaySettingsA)GetProcAddress(GetModuleHandleA("user32.dll"), "EnumDisplaySettingsA");
    ok = p_EnumDSA(dev, mode, dm);
    /* Only the *current* desktop mode is lied about; the enumerated mode list is
       left intact so the engine's own resolution table stays coherent. */
    if (ok && g_cfg.scalePresent && g_cfg.lieScreen && (mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS)) {
        if (g_nEnum++ < 4)
            logmsg("EnumDisplaySettingsA(%s, %lu) %lux%lu -> %dx%d", dev ? dev : "(null)", mode,
                   dm->dmPelsWidth, dm->dmPelsHeight, g_logicalW, g_logicalH);
        dm->dmPelsWidth = g_logicalW;
        dm->dmPelsHeight = g_logicalH;
    }
    return ok;
}

static LONG WINAPI H_ChangeDisplaySettingsA(LPDEVMODEA dm, DWORD flags)
{
    logmsg("ChangeDisplaySettingsA(%lux%lu flags=0x%lx) refused - keeping desktop mode",
           dm ? dm->dmPelsWidth : 0, dm ? dm->dmPelsHeight : 0, flags);
    return DISP_CHANGE_SUCCESSFUL;
}

static HOOKFN hook_for(const char *dll, const char *name)
{
    if (_stricmp(dll, "USER32.dll") == 0) {
        if (!strcmp(name, "GetSystemMetrics")) return (HOOKFN)H_GetSystemMetrics;
        if (!strcmp(name, "GetClientRect"))    return (HOOKFN)H_GetClientRect;
        if (!strcmp(name, "GetWindowRect"))    return (HOOKFN)H_GetWindowRect;
        if (!strcmp(name, "GetCursorPos"))     return (HOOKFN)H_GetCursorPos;
        if (!strcmp(name, "ScreenToClient"))   return (HOOKFN)H_ScreenToClient;
        if (!strcmp(name, "ClientToScreen"))   return (HOOKFN)H_ClientToScreen;
        if (!strcmp(name, "ClipCursor"))       return (HOOKFN)H_ClipCursor;
        if (!strcmp(name, "GetDC"))            return (HOOKFN)H_GetDC;
        if (!strcmp(name, "ReleaseDC"))        return (HOOKFN)H_ReleaseDC;
        if (!strcmp(name, "SetWindowPos"))     return (HOOKFN)H_SetWindowPos;
        if (!strcmp(name, "ChangeDisplaySettingsA")) return (HOOKFN)H_ChangeDisplaySettingsA;
        if (!strcmp(name, "EnumDisplaySettingsA"))   return (HOOKFN)H_EnumDisplaySettingsA;
    }
    if (_stricmp(dll, "GDI32.dll") == 0) {
        if (!strcmp(name, "GetDeviceCaps"))     return (HOOKFN)H_GetDeviceCaps;
        if (!strcmp(name, "BitBlt"))            return (HOOKFN)H_BitBlt;
        if (!strcmp(name, "StretchBlt"))        return (HOOKFN)H_StretchBlt;
        if (!strcmp(name, "SetDIBitsToDevice")) return (HOOKFN)H_SetDIBitsToDevice;
        if (!strcmp(name, "StretchDIBits"))     return (HOOKFN)H_StretchDIBits;
    }
    return NULL;
}

// -------------------------------------------------------------- window setup --
static WNDPROC g_origProc;

/* The engine hit-tests clicks with the client coordinates carried by the
   message, so mouse messages must be translated back into its own logical
   space before the original window procedure sees them. */
static LPARAM remap_mouse(LPARAM lp, int screen_coords)
{
    POINT p;
    unsigned short a, b;
    if (!g_cfg.scalePresent || g_scale == 1.0) return lp;
    p.x = (short)LOWORD(lp);
    p.y = (short)HIWORD(lp);
    if (screen_coords) {
        if (g_gameWnd) ScreenToClient(g_gameWnd, &p);
        unmap_point(&p.x, &p.y);
    } else {
        unmap_point(&p.x, &p.y);
    }
    a = (unsigned short)(p.x & 0xFFFF);
    b = (unsigned short)(p.y & 0xFFFF);
    return (LPARAM)((unsigned)(b) << 16 | a);
}
static LRESULT CALLBACK game_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        /* The engine allocates its frame from the resize payload, so feed it the
           logical size while the real window stays monitor-sized. */
        if (g_cfg.scalePresent) {
            if (g_cfg.logLevel > 1 && g_nSizeMsg++ < 8)
                logmsg("WM_SIZE %dx%d -> %dx%d", (int)LOWORD(lp), (int)HIWORD(lp), g_logicalW, g_logicalH);
            return CallWindowProcA(g_origProc, h, msg, wp,
                                   (LPARAM)((unsigned)g_logicalH << 16 | (unsigned short)g_logicalW));
        }
        break;
    case WM_WINDOWPOSCHANGING:
    case 0x0047: {                       /* WM_WINDOWPOSCHANGED */
        /* These carry the real (4K) size; the engine must only ever see the
           logical size, otherwise it rebuilds its frame at monitor resolution. */
        if (g_cfg.scalePresent && lp) {
            WINDOWPOS wp2 = *(WINDOWPOS*)lp;
            wp2.cx = g_logicalW;
            wp2.cy = g_logicalH;
            wp2.x = 0;
            wp2.y = 0;
            if (g_cfg.logLevel > 1 && g_nPosMsg++ < 6)
                logmsg("WM_WINDOWPOS(0x%04x) -> %dx%d", msg, g_logicalW, g_logicalH);
            return CallWindowProcA(g_origProc, h, msg, wp, (LPARAM)&wp2);
        }
        break;
    }
    case WM_MOUSEACTIVATE:
        /* A WS_POPUP window answers MA_NOACTIVATE by default, which makes the
           engine think it never got focus and ignore every click. */
        if (g_cfg.verboseBlit) logmsg("WM_MOUSEACTIVATE -> MA_ACTIVATE");
        return MA_ACTIVATE;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        if (g_cfg.verboseBlit && g_nMsg++ < 40)
            logmsg("wndproc msg 0x%04x client %d,%d -> %d,%d", msg,
                   (int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
                   (int)(short)LOWORD(remap_mouse(lp, 0)), (int)(short)HIWORD(remap_mouse(lp, 0)));
        return CallWindowProcA(g_origProc, h, msg, wp, remap_mouse(lp, 0));
    case WM_MOUSEWHEEL: case 0x02A3:                      /* wheel uses screen coords */
        return CallWindowProcA(g_origProc, h, msg, wp, remap_mouse(lp, 1));
    case WM_ACTIVATE:
        if (g_cfg.verboseBlit) logmsg("WM_ACTIVATE wp=0x%lx", (unsigned long)wp);
        break;
    case WM_SETFOCUS:
        if (g_cfg.verboseBlit) logmsg("WM_SETFOCUS");
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU || (wp & 0xFFF0) == SC_CLOSE) return 0;
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO*)lp;
        mm->ptMaxTrackSize.x = g_monitorRect.right - g_monitorRect.left;
        mm->ptMaxTrackSize.y = g_monitorRect.bottom - g_monitorRect.top;
        return 0;
    }
    }
    return CallWindowProcA(g_origProc, h, msg, wp, lp);
}

static void restyle_window(void)
{
    g_nRestyle++;
    if (g_cfg.logLevel > 0 && g_nRestyle < 20)
        logmsg("restyle #%d -> borderless %ld,%ld-%ld,%ld", g_nRestyle,
               g_monitorRect.left, g_monitorRect.top, g_monitorRect.right, g_monitorRect.bottom);
    SetWindowLongA(g_gameWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    SetWindowLongA(g_gameWnd, GWL_EXSTYLE, GetWindowLongA(g_gameWnd, GWL_EXSTYLE) & ~WS_EX_APPWINDOW);
    SetWindowPos(g_gameWnd, HWND_TOP,
                 g_monitorRect.left, g_monitorRect.top,
                 g_monitorRect.right - g_monitorRect.left,
                 g_monitorRect.bottom - g_monitorRect.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    if (!g_origProc) {
        g_origProc = (WNDPROC)SetWindowLongA(g_gameWnd, GWL_WNDPROC, (LONG)game_wndproc);
        if (g_cfg.logLevel > 0) logmsg("wndproc hooked (orig %p)", (void*)g_origProc);
    }
    if (GetForegroundWindow() != g_gameWnd && g_nRestyle < 4) {
        SetForegroundWindow(g_gameWnd);
    }
}

typedef struct { HWND w[8]; int n; } WndList;
static BOOL CALLBACK collect_game_wnd(HWND h, LPARAM lp)
{
    WndList *l = (WndList*)lp;
    if (is_game_wnd(h) && l->n < 8) l->w[l->n++] = h;
    return TRUE;
}

static void enforce_geometry(void)
{
    WndList l;
    int i;
    RECT r;
    DWORD st;
    if (!g_cfg.borderless) return;
    l.n = 0;
    EnumWindows(collect_game_wnd, (LPARAM)&l);
    if (!l.n) { g_gameWnd = NULL; return; }
    for (i = 0; i < l.n; i++) {
        HWND h = l.w[i];
        st = GetWindowLongA(h, GWL_STYLE);
        if (!GetWindowRect(h, &r)) continue;
        if (st != (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN) ||
            r.left != g_monitorRect.left || r.top != g_monitorRect.top ||
            r.right != g_monitorRect.right || r.bottom != g_monitorRect.bottom) {
            g_gameWnd = h;
            restyle_window();
        }
    }
    if (!is_game_wnd(g_gameWnd)) {
        for (i = 0; i < l.n; i++) if (IsWindowVisible(l.w[i])) { g_gameWnd = l.w[i]; break; }
    }
}

static BOOL CALLBACK find_game_wnd(HWND h, LPARAM lp)
{
    DWORD pid; char cls[64];
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    GetClassNameA(h, cls, sizeof(cls));
    if (!strcmp(cls, "JackalClass")) { *(HWND*)lp = h; return FALSE; }
    return TRUE;
}

static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep)
{
    void *addr = ep->ExceptionRecord->ExceptionAddress;
    if (ep->ExceptionRecord->ExceptionCode == 0x40010006)      /* OutputDebugString */
        return EXCEPTION_CONTINUE_SEARCH;
    HMODULE mod = NULL;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) && mod) {
        GetModuleFileNameA(mod, name, MAX_PATH);
        char *b = strrchr(name, 0x5C); if (b) memmove(name, b + 1, strlen(b));
    }
    logmsg("!! exception 0x%08lx at %p in %s (base %p, +0x%lx)",
           ep->ExceptionRecord->ExceptionCode, addr, name, (void*)mod,
           mod ? (unsigned long)((char*)addr - (char*)mod) : 0UL);
    {
        unsigned long *ebp = (unsigned long*)ep->ContextRecord->Ebp;
        int depth;
        for (depth = 0; depth < 12 && ebp && !IsBadReadPtr(ebp, 8); depth++) {
            unsigned long ret = ebp[1], *next = (unsigned long*)ebp[0];
            HMODULE m = NULL; char nm[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)ret, &m) && m) {
                GetModuleFileNameA(m, nm, MAX_PATH);
                { char *b = strrchr(nm, 0x5C); if (b) memmove(nm, b + 1, strlen(b)); }
            }
            logmsg("   frame %d: %s+0x%lx (ret %p)", depth, nm,
                   m ? (unsigned long)(ret - (unsigned long)m) : 0UL, (void*)ret);
            if (next <= ebp) break;
            ebp = next;
        }
    }
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
        logmsg("   access violation %s address %p",
               ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
               (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static POINT point_zero(void) { POINT p; p.x = 0; p.y = 0; return p; }

typedef struct { int idx; HMONITOR hm; } MonEnum;
static BOOL CALLBACK mon_cb(HMONITOR hm, HDC dc, LPRECT r, LPARAM lp)
{
    MonEnum *e = (MonEnum*)lp; (void)dc; (void)r;
    if (e->idx-- == 0) { e->hm = hm; return FALSE; }
    return TRUE;
}
static HMONITOR monitor_at(int idx)
{
    MonEnum e; e.idx = idx; e.hm = NULL;
    EnumDisplayMonitors(NULL, NULL, mon_cb, (LPARAM)&e);
    return e.hm;
}

/* Confine the cursor to the monitor while the game owns the foreground, and let
   it go the moment anything else takes focus. Without this a borderless window
   loses edge-scrolling to the neighbouring monitor. */
static int g_clipped;

static DWORD WINAPI cursor_thread(LPVOID p)
{
    (void)p;
    for (;;) {
        Sleep(25);
        if (!g_cfg.lockCursor || !g_gameWnd || !IsWindow(g_gameWnd)) {
            if (g_clipped) { ClipCursor(NULL); g_clipped = 0; }
            continue;
        }
        {
            int want = (GetForegroundWindow() == g_gameWnd) &&
                       IsWindowVisible(g_gameWnd) && !IsIconic(g_gameWnd);
            if (want) {
                RECT cur;
                /* The engine clips/releases the cursor itself at various points
                   (menus, advisor screens), so re-assert rather than trust it. */
                if (!GetClipCursor(&cur) || cur.left != g_monitorRect.left ||
                    cur.top != g_monitorRect.top || cur.right != g_monitorRect.right ||
                    cur.bottom != g_monitorRect.bottom) {
                    ClipCursor(&g_monitorRect);
                }
                g_clipped = 1;
            } else if (g_clipped) {
                ClipCursor(NULL);
                g_clipped = 0;
            }
        }
    }
    return 0;
}

static DWORD WINAPI worker(LPVOID p)
{
    int ticks = 0;
    (void)p;
    for (;;) {
        Sleep(120);
        ticks++;
        if (ticks < 150 || (ticks % 16) == 0) patch_all();

        if (!g_gameWnd) {
            HWND h = NULL;
            EnumWindows(find_game_wnd, (LPARAM)&h);
            if (h) {
                RECT r;
                g_gameWnd = h;
                GetWindowRect(h, &r);
                logmsg("game window found: %p rect %ld,%ld-%ld,%ld style=0x%08lx",
                       (void*)h, r.left, r.top, r.right, r.bottom, GetWindowLongA(h, GWL_STYLE));
            }
        }
        enforce_geometry();
        if (g_cfg.verboseBlit && g_gameWnd && (ticks % 8) == 0) {
            HWND fg = GetForegroundWindow();
            logmsg("foreground=%p game=%p active=%d", (void*)fg, (void*)g_gameWnd,
                   GetActiveWindow() == g_gameWnd);
        }
    }
    return 0;
}

// ----------------------------------------------------------------- DllMain ----
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char *s;
        HMONITOR hm;
        MONITORINFO mi;

        InitializeCriticalSection(&g_cs);
        DisableThreadLibraryCalls(hInst);
        GetModuleFileNameA(NULL, g_exeDir, MAX_PATH);
        s = strrchr(g_exeDir, '\\');
        if (s) *s = 0;
        cfg_load();
        if (!g_cfg.enabled) return TRUE;

        if (g_cfg.dpiAware) {
            typedef BOOL (WINAPI *PFN_SPDAC)(HANDLE);
            PFN_SPDAC pd = (PFN_SPDAC)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
            if (pd) pd((HANDLE)-4);        /* PER_MONITOR_AWARE_V2 */
        }

        hm = g_cfg.monitorOverride > 0 ? monitor_at(g_cfg.monitorOverride)
                                       : MonitorFromPoint(point_zero(), MONITOR_DEFAULTTOPRIMARY);
        mi.cbSize = sizeof(mi);
        if (hm && GetMonitorInfoA(hm, &mi)) g_monitorRect = mi.rcMonitor;
        else {
            g_monitorRect.left = 0; g_monitorRect.top = 0;
            g_monitorRect.right = GetSystemMetrics(SM_CXSCREEN);
            g_monitorRect.bottom = GetSystemMetrics(SM_CYSCREEN);
        }

        g_logicalW = g_cfg.logicalW;
        g_logicalH = g_cfg.logicalH;
        if (g_logicalW <= 0 || g_logicalH <= 0) {
            /* Auto: the engine always renders a 16:9 frame -- that is the aspect
               Civ3's UI was designed for -- and the present is centred with black
               bars on anything wider (21:9, 32:9) or taller (16:10, 4:3). Pick the
               candidate that fills the most of the monitor at a whole-number scale,
               preferring the larger logical frame on a tie so the UI does not get
               needlessly huge. */
            static const int cand[][2] = { {1920,1080}, {1600,900}, {1280,720}, {1024,576} };
            int mw = g_monitorRect.right - g_monitorRect.left;
            int mh = g_monitorRect.bottom - g_monitorRect.top;
            int bestScore = -1, bestK = 0, i;
            g_logicalW = 1920; g_logicalH = 1080;
            for (i = 0; i < (int)(sizeof(cand)/sizeof(cand[0])); i++) {
                int k = mw / cand[i][0];
                int kh = mh / cand[i][1];
                int score;
                if (kh < k) k = kh;
                if (k < 1) continue;
                score = cand[i][1] * k;             /* filled height in pixels */
                if (score > bestScore || (score == bestScore && k < bestK)) {
                    bestScore = score; bestK = k;
                    g_logicalW = cand[i][0];
                    g_logicalH = cand[i][1];
                }
            }
            if (bestScore < 0) { g_logicalW = 1024; g_logicalH = 576; bestK = 1; }
        }

        AddVectoredExceptionHandler(1, crash_handler);
        g_frameW = g_logicalW;
        g_frameH = g_logicalH;
        logmsg("=== Civ3 borderless layer attached ===");
        logmsg("monitor %ld,%ld-%ld,%ld | virtual display %dx%d | fit=%s | borderless=%d scale=%d dpi=%d lockcursor=%d",
               g_monitorRect.left, g_monitorRect.top, g_monitorRect.right, g_monitorRect.bottom,
               g_logicalW, g_logicalH, g_cfg.fit, g_cfg.borderless, g_cfg.scalePresent, g_cfg.dpiAware, g_cfg.lockCursor);

        CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (g_cfg.lockCursor) CreateThread(NULL, 0, cursor_thread, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_clipped) { ClipCursor(NULL); g_clipped = 0; }
        detach();
    }
    return TRUE;
}

// --- GL entry points Civ3 imports (presentation is GDI, so these only log) -----
typedef struct HGLRC__ *HGLRCx;
typedef HGLRCx (WINAPI *PFN_CREATE)(HDC);
typedef BOOL   (WINAPI *PFN_MAKE)(HDC, HGLRCx);
typedef BOOL   (WINAPI *PFN_SWAP)(HDC);

__declspec(dllexport) HGLRCx WINAPI wglCreateContext(HDC hdc)
{
    static PFN_CREATE p;
    if (!p) p = (PFN_CREATE)realproc("wglCreateContext");
    return p(hdc);
}
__declspec(dllexport) BOOL WINAPI wglMakeCurrent(HDC hdc, HGLRCx rc)
{
    static PFN_MAKE p;
    if (!p) p = (PFN_MAKE)realproc("wglMakeCurrent");
    return p(hdc, rc);
}
__declspec(dllexport) BOOL WINAPI wglSwapBuffers(HDC hdc)
{
    static PFN_SWAP p;
    if (!p) p = (PFN_SWAP)realproc("wglSwapBuffers");
    return p(hdc);
}
__declspec(dllexport) void WINAPI glViewport(int x, int y, int w, int h)
{ static void (WINAPI *p)(int,int,int,int); if (!p) p = realproc("glViewport"); p(x,y,w,h); }
__declspec(dllexport) void WINAPI glScissor(int x, int y, int w, int h)
{ static void (WINAPI *p)(int,int,int,int); if (!p) p = realproc("glScissor"); p(x,y,w,h); }
__declspec(dllexport) void WINAPI glOrtho(double l, double r, double b, double t, double n, double f)
{ static void (WINAPI *p)(double,double,double,double,double,double); if (!p) p = realproc("glOrtho"); p(l,r,b,t,n,f); }
__declspec(dllexport) void WINAPI glLineWidth(float w)
{ static void (WINAPI *p)(float); if (!p) p = realproc("glLineWidth"); p(w); }
