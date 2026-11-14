# Changelog

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
