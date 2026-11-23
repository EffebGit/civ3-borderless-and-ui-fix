// civ3bl_common.h -- shared logging + IAT hook plumbing for the Civ3 borderless layer
#pragma once

#define CIV3BL_VERSION "1.0.4"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <tlhelp32.h>

typedef struct {
    int  enabled;        // 0 = pure passthrough
    int  logLevel;       // 0 none, 1 info, 2 verbose
    int  dpiAware;       // set per-monitor-v2 before windows exist
    int  borderless;     // restyle + reposition the game window
    int  scalePresent;   // scale GDI presents to the real window
    int  logicalW;       // virtual display width reported to the game
    int  logicalH;
    char fit[16];        // "integer" | "fit" | "stretch"
    int  monitorOverride; // -1 = primary
    int  verboseBlit;    // log every present call
    int  forceRedraw;    // >0 = invalidate the client N times per second
    int  repaintOnClick; // full repaint after a click (engine damage tracking drops regions)
    int  repaintOnKey;   // full repaint after keyboard input (text/number fields never redraw)
    int  lockCursor;     // confine the cursor to the monitor while foreground
    int  lieScreen;      // report LogicalWidth/Height via GetSystemMetrics
    int  lieClientRect;  // report LogicalWidth/Height via GetClientRect
    int  lieDeviceCaps;  // report LogicalWidth/Height via GetDeviceCaps
} Civ3Cfg;

extern Civ3Cfg g_cfg;
void logmsg(const char *fmt, ...);
void cfg_load(void);
