# Changelog

## 1.0.4

### Fixed

- **Typed values in dialog fields never appeared.** The engine's damage tracking does not mark a
  text or number box dirty, so digits entered into the trade screen's amount field stayed invisible
  until something else forced a full paint (which, with `RepaintOnClick`, usually meant moving the
  mouse). The window procedure now also nudges one full repaint after keyboard input, throttled to
  one per 10 ms like the mouse path.

### Added

- **`RepaintOnKey=1`** (default on) — the new gate for the above. Re-read live from `civ3bl.ini`, so
  it can be A/B tested without restarting the game. Set to 0 to restore pre-1.0.4 behaviour.
## 1.0.3

### Fixed

- **Modal screens stopped responding to the mouse.** While the "what to build next" advisor was
  up, the build list did not highlight on hover and the production menu ignored the mouse wheel,
  because the repaint nudge only fired for button messages and that dialog never repaints
  incrementally. The nudge now covers mouse movement, buttons and the wheel, throttled to at most
  one forced repaint per 10 ms so a high-rate mouse cannot cause a redraw storm (worst case about
  one full redraw per frame while the pointer is moving).

## 1.0.2

### Added

- **`ForceRedraw=N`** — invalidate the client N times a second so the engine keeps producing
  frames while idle (useful for frame generation and VRR, which need a present stream). Off by
  default: the engine is event driven and idles at ~0 % CPU. Measured at the menu, a steady
  60 Hz costs roughly 13 % of one core (~0.2 % per frame), about half that at 30 Hz; a busy map
  costs more. It does not make scrolling smoother — scroll input already produces frames.

## 1.0.1

### Fixed

- **Startup race.** The IAT patches were applied by a worker thread roughly 120 ms after load,
  so a fast start could create the window and size the engine's surfaces from the real desktop
  resolution first. The game then rendered at 3840×2160 and the frame size never came back
  down — the UI came up at 1:1 and looked hairline. The executable is now patched synchronously
  in `DllMain`; late-loaded DLLs are still handled by the worker.
- **Regions that never repainted.** The engine's incremental damage tracking drops areas once
  its output is scaled: in the tech tree, techs below the second row did not change colour when
  selected, while a full repaint (alt-tab) fixed them. The shim now invalidates the window after
  each click (`RepaintOnClick=1`).
- `GetWindowRect` now agrees with `GetClientRect` about the virtual display size. When they
  disagreed the engine allocated a surface at one size and computed row strides from the other,
  producing a write access violation inside its own palette loop a few seconds into loading.

### Added

- **Live configuration reload.** `civ3bl.ini` is re-read when it changes on disk, so
  `RepaintOnClick` and `LogLevel` can be A/B tested without restarting the game.
- **Clip regions are scaled** with the same transform as the blits (`SelectClipRgn`), so
  engine-scoped repaints cover their full scaled area.
- Present-rate statistics (`LogLevel=1`, one line per 5 s) and a vectored exception handler that
  logs module-relative crash addresses.
- Automatic logical size keeps a 16:9 frame and chooses the largest integer scale that fits, so
  ultrawide and 16:10 panels get black bars instead of a stretched or re-proportioned image.

## 1.0.0

- First public release: borderless fullscreen via an app-local `opengl32` proxy, a virtual
  display so the fixed-size interface is laid out at a readable resolution, integer-scaled
  present, per-monitor DPI awareness, refused display-mode changes, and a cursor confined to the
  game's monitor while it has focus.
