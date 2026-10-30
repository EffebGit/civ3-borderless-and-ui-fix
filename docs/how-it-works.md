# How it works

Notes for anyone maintaining or porting this to another fixed-resolution game.

## 1. The injection point

Civ3 statically imports `OPENGL32.dll` (17 functions — and, as it turns out, never calls a
single one of them at runtime; presentation is GDI). That import is still useful: `opengl32` is
**not** in the KnownDLLs list, so a copy of `opengl32.dll` in the game folder wins import
resolution and our code runs before the engine's entry point.

```
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs" | findstr /i opengl
```

`gdi32`, `user32` and `kernel32` *are* KnownDLLs and cannot be proxied. Everything we need from
them is reached by patching each loaded module's IAT from inside our DLL — see §5.

The proxy exports all 368 `opengl32` entry points. 361 are forwarders to `opengl32_real.dll`
(the user's own `C:\Windows\SysWOW64\opengl32.dll`, which the installer copies in), and 7 are
ours. Two build details that cost real time:

- stdcall decoration: a `__declspec(dllexport) BOOL WINAPI wglMakeCurrent(HDC, HGLRC)` is
  exported as `_wglMakeCurrent@8`. The game imports `wglMakeCurrent`. Without a `.def` alias
  (`wglMakeCurrent=_wglMakeCurrent@8`) the process dies at load with `0xC0000139`
  (STATUS_ENTRYPOINT_NOT_FOUND) before a single line of game code runs.
- MSVC will not emit forwarders to a non-system DLL without an import library for the target:
  `lib /def:opengl32_real.def /out:opengl32_real.lib /machine:x86`. Without it you get LNK2001
  for all 361 forwarded symbols plus "forwarded statement not supported for the target platform".

## 2. The virtual display

The engine is told the screen is `LogicalWidth × LogicalHeight` (1920×1080 by default). It lays
out its entire UI for that size — panels, bitmap fonts, advisor screens — and the finished frame
is then upscaled to the monitor. That is the whole UI-scaling trick: there is no per-element
scaling, because the game's interface is fixed-size artwork positioned by code.

A lower logical size therefore means a bigger UI (1280×720 ≈ 3× on a 4K panel), and the aspect
of the logical frame is the aspect of the game you get.

## 3. What must agree

This is the part that crashes engines. A 2005-era engine allocates its back buffer, computes row
strides, and computes UI anchors from several APIs at once. If any one of them answers
differently from the others, you get a write past the end of a surface.

| API | Must answer |
|---|---|
| `GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)` | logical size |
| `GetClientRect(game hwnd)` | logical size |
| `GetWindowRect(game hwnd)` | logical size — a borderless popup's client == window |
| `GetDeviceCaps(HORZRES/VERTRES)` | logical size, **for the game window DC and for screen DCs** |
| `EnumDisplaySettings(ENUM_CURRENT_SETTINGS)` | logical size |
| `ChangeDisplaySettings(A/W)` | swallow, return `DISP_CHANGE_SUCCESSFUL` |
| `WM_SIZE` lParam | logical size |
| `WM_WINDOWPOSCHANGING` / `WM_WINDOWPOSCHANGED` | logical size |
| `SetWindowPos(game hwnd, …)` | force the monitor rect |

`GetDeviceCaps` needs care: the engine asks its **own bitmap** DCs for dimensions, and lying
about those corrupts surface allocation. `WindowFromDC` returns NULL for both screen and memory
DCs, so track the HDCs you handed out from `GetDC(NULL)` / `GetDC(GetDesktopWindow())` and lie
only for those plus the game window's DC.

Do **not** blanket-override `EnumDisplaySettings`: rewriting the whole enumerated mode list to
one entry makes the engine pick a mode it can't use and exit during startup. Rewrite only
`ENUM_CURRENT_SETTINGS` and `ENUM_REGISTRY_SETTINGS`.

If some other path still leaks the real size, the symptom is specific and obvious in the log:
`engine frame is now 3840x2160`. The frame-size tracker (below) then degrades to 1:1 instead of
stretching 4K into 4K, so it stays playable while you hunt the leak — for Civ3 it was the
`WM_WINDOWPOSCHANGED` payload.

## 4. The present

The engine's screen update is a single GDI `BitBlt` of its back buffer onto the window DC at
`(0,0,frameW,frameH)`. Intercept it, map the destination rect into the client area with an
integer scale, and call the real `StretchBlt` with `SetStretchBltMode(COLORONCOLOR)` so pixel art
stays hard instead of being HALFTONE-blurred. Partial blits into the same DC are sprites and
small updates — map them through the same transform, with the same scale.

Scale = `min(clientW / frameW, clientH / frameH)`, floored to an integer in `Fit=integer`, and
the remainder is centred with the bars painted `BLACKNESS` before each full frame so nothing
stale shows through.

Frame size is adopted from the largest blit seen, not hard-coded, so an unexpected engine
resolution degrades to "no scaling" rather than to garbage.

## 5. Hooking non-proxyable DLLs

`patch_module()` walks `IMAGE_IMPORT_DESCRIPTOR`s of every module and rewrites the thunks for the
names we want. Two things matter:

- Re-run the scan periodically. Engine sub-DLLs (`jgl.dll` here) and DRM/audio proxies load
  late; a module loaded after the first pass stays unhooked.
- Skip your own module, or you will hook your own forwarding calls.

## 6. Input

Two separate breaks, both of which look like a hit-test bug:

- A `WS_POPUP` window answers `MA_NOACTIVATE` to `WM_MOUSEACTIVATE`, so clicking it never
  activates it and the engine — which checks `GetForegroundWindow` — silently drops every click,
  *while `WM_LBUTTONDOWN` still arrives*. Subclass the WndProc and return `MA_ACTIVATE`.
- The engine hit-tests clicks with the **client coordinates carried by the message**, not the
  polled cursor. Remap `WM_MOUSEMOVE` / `WM_*BUTTON*` (client coords) and `WM_MOUSEWHEEL`
  (screen coords) back into engine space, and hook `ScreenToClient` / `ClientToScreen` to match.
  The engine will happily poll `GetCursorPos` for hover and use the message for the click.

Cursor confinement: once the window is borderless, edge-scrolling walks the pointer onto the
neighbouring monitor and the map stops panning. A 25 ms thread clips the cursor to the monitor
while `GetForegroundWindow()` is the game and releases it otherwise — re-asserting every tick,
because the engine releases the clip itself on menus and advisor screens. Never leave the clip
set when focus moves; that is what makes alt-tab pleasant instead of hostage-taking.

## 7. Debugging

- A vectored exception handler with module-relative addresses (`module+0x…`) is enough to locate
  a crash without a debugger. Filter `0x40010006` (DBG_PRINTEXCEPTION_C) — that is
  `OutputDebugString`, not a fault, and it will drown the log.
- Disassemble the faulting offset with capstone straight out of the file's memory-mapped image.
  For Civ3 the crash was `mov word ptr [edi], ax` in a palette-conversion loop: a destination
  pointer walking off the end of a surface whose stride came from `GetWindowRect` while its
  allocation came from `GetClientRect`.
- Capture the **window**, not the desktop: `GetDC(hwnd)` + `BitBlt` into a DIB. Desktop grabs
  (`ImageGrab.grab(all_screens=True)`) use the virtual-desktop minimum as origin, so crop maths
  is wrong on multi-monitor setups, and the composite may include the cursor and DWM-scaled
  surfaces.

## 8. Why the HUD can't be pulled into a 16:9 safe area

Map and HUD are composited into one surface in one pass and presented as one blit, so the pixels
under the HUD panels never exist in the buffer to move inward — and the map viewport is
`screen − HUD`, so it was never rendered anywhere else either. Shifting HUD panels at present
time would leave holes. "Wide world + 16:9 HUD" requires the engine's own layout code, which is
a different (much larger) project than a presentation shim.
