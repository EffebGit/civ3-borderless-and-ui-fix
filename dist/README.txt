Civ3 Borderless
===============

Borderless fullscreen + resolution-independent UI for Sid Meier's
Civilization III: Complete (Conquests, v1.22).

This does NOT modify Civ3Conquests.exe. It is an app-local opengl32.dll proxy:
Civ3 statically imports OPENGL32, so Windows loads our DLL before any engine
code runs. From there it:

  1. makes the process per-monitor DPI aware, so Windows stops bitmap-stretching
     the whole game at 150%/200% scaling (that is what makes the stock game
     blurry on a 4K panel);

  2. presents a "virtual display" to the engine: the game is told the screen is
     1920x1080 (configurable). It lays out its whole UI - panels, fonts, unit
     boxes, advisor screens - for that size, which is why the UI ends up twice
     as large on a 3840x2160 monitor instead of 1:1 hairline text;

  3. takes the engine's single full-frame GDI present (it BitBlits its 8/16-bit
     back buffer onto the window DC) and stretches it to fill the real window
     with a hard-edged integer scale, so map and UI scale together and stay
     crisp;

  4. turns the window into a borderless popup covering one monitor, refuses
     display-mode changes (alt-tab never rearranges your desktop) and translates
     mouse coordinates back into the engine's coordinate space so clicking is
     pixel-accurate.

Everything the engine asks about size is answered consistently: GetSystemMetrics,
GetClientRect, GetWindowRect, GetDeviceCaps, EnumDisplaySettings, plus the
WM_SIZE / WM_WINDOWPOSCHANGED payloads. Engines of this vintage allocate their
back buffer and compute row strides from several of those at once, so a single
inconsistent answer overflows a buffer and crashes.


WHAT YOU GET
------------
  * true borderless fullscreen, no mode switch, no flicker on alt-tab
  * UI and map both scale - text is readable at 4K
  * exact 2x pixels at 3840x2160 from a 1920x1080 logical frame, no blur
  * costs about 2% of one core


INSTALL
-------
  1. Close the game.
  2. Copy opengl32.dll, opengl32_real.dll and civ3bl.ini into
        <Steam>\steamapps\common\Sid Meier's Civilization III Complete\Conquests
     (install.bat does this for you and patches conquests.ini).

Two DLLs end up in that folder, and they are a pair:

  opengl32.dll        this patch. It is shipped in the download, built from the
                      project's source. It carries a system DLL's name on purpose:
                      Windows resolves an app-local DLL first, and opengl32 is not
                      a KnownDLL, so the game loads it before its own engine runs.

  opengl32_real.dll   your own C:\Windows\SysWOW64\opengl32.dll, copied in by the
                      installer. It is Microsoft's file and is NOT redistributed
                      with this patch. The proxy forwards every OpenGL call to it.

  Do not substitute a 64-bit opengl32.dll, and do not delete only one of the two:
  the game will fail to start with 0xC0000139 (STATUS_ENTRYPOINT_NOT_FOUND).

  If a Steam "verify integrity of game files" removes opengl32.dll, run install.bat
  again.

CONFIGURATION
-------------
civ3bl.ini, in the same folder. The two that matter:

  LogicalWidth / LogicalHeight
      The resolution the engine renders at. Lower = bigger UI.
      1920x1080 on a 4K screen = 2x. 1280x720 = 3x (very large, chunky).
      0 / 0 (default) = automatic: a 16:9 frame (the aspect the game's interface
      was designed for), integer-scaled and centred, with black pillar/letterbox
      bars whenever the monitor is not 16:9. Nothing is ever stretched.

          1920x1080 (16:9)  -> 1920x1080  1x  fills exactly
          2560x1440 (16:9)  -> 1280x720   2x  fills exactly
          3840x2160 (16:9)  -> 1920x1080  2x  fills exactly
          3440x1440 (21:9)  -> 1280x720   2x  440px pillar bars
          5120x1440 (32:9)  -> 1280x720   2x  1280px pillar bars
          2560x1600 (16:10) -> 1280x720   2x  80px letterbox
          1024x768  (4:3)   -> 1024x576   1x  96px letterbox

      If you would rather use the whole ultrawide panel, pin a pair with that
      aspect (e.g. 1720x720 on 21:9). The engine then lays out its HUD for the
      wider frame and the bars disappear -- by choice, not by accident.

  Fit
      integer (default) = crisp integer scaling, black bars if aspect differs
      fit               = fractional scaling to fill, slightly soft
      stretch           = fill the monitor and ignore aspect ratio (distorts)

  LockCursor
      1 (default) = keep the mouse on the game's monitor while the game has
                    focus, released the moment anything else is focused. A
                    borderless window otherwise loses edge-scrolling to a
                    monitor sitting beside it.
      0           = leave the cursor alone

Set LogLevel=1 if you need to debug; it writes civ3bl.log next to the exe.


UNINSTALL
---------
Delete opengl32.dll and opengl32_real.dll from the Conquests folder (and
civ3bl.ini if you like). That is the entire patch. uninstall.bat does it and
restores conquests.ini from the backup taken at install time.

If a Steam "verify integrity of game files" ever removes opengl32.dll, just run
install.bat again.


NOTES / LIMITS
--------------
  * Tested on the Steam 1.22 build of Civ3Conquests.exe. The base game and PTW
    exes in the same install are untouched.
  * The intro movie is disabled by the installer (PlayIntro=0). The Bink video
    player draws outside the engine's frame, so it cannot be scaled by this
    method and would appear in a corner.
  * The title screen artwork is a fixed 1024x768 bitmap; the engine itself
    letterboxes it, so it will always have black margins on a widescreen
    monitor. That is stock behaviour, not a bug in this patch.
  * Some full-screen edge cases (very old saved games that recorded a different
    resolution) may need one restart after changing LogicalWidth/Height.
