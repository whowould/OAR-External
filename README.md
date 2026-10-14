# OAR external

An external overlay for **One-Armed Robber** (Unreal Engine 4.27). It reads the game's memory from a separate
process, draws an ESP on a transparent click-through window over the game, and uses the
[Nebula ImGui menu](../menu) as its control panel.

| Feature | What it does |
|---|---|
| **Camera ESP** | Security cameras: box, `CAM #n` label, state (working / EMP / destroyed / sees you / in use), distance and a view-direction line. Destroyed cameras are hidden by default (*ESP > Cameras > Hide destroyed*). |
| **Guard ESP** | Guards only (`NPC_Guard`; police, SWAT and other NPCs are never drawn): box, health bar, state (patrolling, investigating, alert, escorting, tied up, EMP), distance, head marker, snaplines. |
| **Force guard check-in** | A dead guard drops a phone that raises the alarm unless it reaches a check-in point before its timer (15 s) ends. *Check in now* (or *Auto*, at a chosen time left) clears the phone's `Active?` flag, which is exactly what the check-in point does. Dropped phones and check-in points can be marked on the ESP too. |

## Build

Needs CMake 3.20+, Ninja and MinGW-w64 g++ 13 (the same toolchain as the menu project), and the `menu` project
next to this folder (it provides Dear ImGui, FreeType and the theme/widgets, which are compiled in unmodified).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\oar_external.exe
```

`-DNEBULA_DIR=<path>` points at the menu project if it lives elsewhere.

## Use

1. Start the game, then start `oar_external.exe`. It asks for administrator rights because the game runs elevated
   and a process can only open an elevated process if it is elevated too.
2. The ESP follows the game window while the game (or the menu) is in front. The game must be windowed or borderless
   (the Windows default for UE4 *Fullscreen* is fine); exclusive fullscreen hides the overlay.
3. The menu is a normal window. The sidebar bottom shows the connection state. Hotkeys work while the game has focus:

| Key | Action |
|---|---|
| F6 | Toggle ESP |
| F7 | Check in now |
| F8 | Toggle auto check-in |

All of them can be rebound in *Settings > Keybinds*. Settings autosave; named configs (*Configs* page) are plain
text files in `%LOCALAPPDATA%\OAR External\configs` and can be exported to / imported from the clipboard.

Check-in takes effect where the phone logic runs: **while you host or play solo**. The phone's beeper and highlight
belong to `StopAlertMulti()`, which cannot be called from outside, so it keeps beeping until it is picked up.

## How it works

```
 scanner thread (view 250 Hz, entities 100 Hz)  UI thread
 ┌──────────────────────────────┐   Snapshot   ┌──────────────────────────────────────────┐
 │ Process  ->  FNamePool/GWorld │ ───────────> │ menu window (Nebula, D3D9Ex FLIPEX)       │
 │ Reflection (UClass/FProperty) │  (immutable) │ overlay window (D3D9 + DWM glass) -> ESP  │
 │ actor discovery, bulk reads   │ <─────────── │ global hotkeys, settings, configs         │
 │ check-in write                │   Settings   └──────────────────────────────────────────┘
 └──────────────────────────────┘
```

* **No hardcoded game offsets.** Only two code signatures are used (`FNamePool`, `GWorld`). Everything else is
  resolved *by name* through Unreal's reflection data (`NPC_Guard_C.Health`, `CameraBP_C.Destroyed?`,
  `GuardPhone_C.Active?`, `PlayerCameraManager.CameraCachePrivate.POV.FOV`, ...), starting from `GWorld`.
  `ULevel::Actors` and `USceneComponent::ComponentToWorld` are not reflected; they are found by checking live
  memory (a `TArray` of actors; a transform equal to the relative location of parentless roots).
* **Caching.** Resolved addresses and offsets are persisted per game build in
  `%LOCALAPPDATA%\OAR External\cache\offsets-<exe stamp>-<pak stamp>.txt`. The key covers the `.pak` files too,
  because Blueprint layouts (guards, cameras, phones) are cooked into them: any game or content update starts from
  an empty cache. Cache values are validated on load and the file is replaced atomically. FName strings, class
  names and class -> entity kind are cached in memory; the actor list is walked every 0.5 s and only unseen
  actors cost a lookup; each tracked entity is refreshed with **one bulk read** (two for guards). The camera view
  is read on every 4 ms tick, entities every 10 ms; a tick costs about 0.1 ms in a heist.
* **Safe writes.** The only write the tool ever does is clearing `GuardPhone_C.Active?`. It is skipped unless the
  property resolved as a Blueprint `BoolProperty`, and it re-checks that the phone is still the same live object
  right before writing. Property types are verified during reflection, so a changed variable type fails safe.
* **Pixel-exact ESP.** The overlay is the game's client area at 1:1, so everything is snapped to whole pixels
  (`src/overlay/pixel.h`): every rectangle corner goes through `std::round`; boxes are filled integer strips
  (black outside / colour / black inside, no strokes), lines are centred on pixel centres, font sizes and line
  widths are whole pixels, anti-aliasing is off, and text is 1-bit Tahoma with a uniform one-pixel outline. The only
  transparency left in a frame is the colours' own alpha (checked on the raw pixels with `--esp-shot`).
* **Projection** mirrors Unreal: camera POV (location, rotation, FOV) with `ULocalPlayer::AspectRatioAxisConstraint`
  deciding whether FOV is horizontal or vertical, plus letterboxed views.
* **Checks.** The engine layout the reflection code assumes is verified against the live `UWorld` at attach; if it
  does not match (a different engine version) the menu says *Unsupported* instead of reading garbage.

## Tools

| Command | Purpose |
|---|---|
| `build\oar_external.exe --demo` | Synthetic scene (guards, cameras, phone, check-in point) to try the menu and ESP without a mission. |
| `build\oar_external.exe --shots <dir>` | Renders every menu page to `<dir>\view_N.bmp`. |
| `build\oar_external.exe --esp-shot <file>` | Renders the demo ESP offscreen (default settings, 32-bit BMP with alpha) so the exact pixels can be inspected. |
| `build\oar_probe.exe [seconds] [debug]` | Console view of what the scanner sees (link state, camera, entities, timings). |
| `build\oar_selftest.exe` | Checks that need no game: pattern matching, settings round-trip, config store, projection math. |
| *ESP > Style > Mark every actor* | Draws a dot and class name for each nearby actor: find what something is called, check the projection. |

## Layout

```
src/game/      process access, Unreal memory model (names, reflection), scanner thread, snapshot types
src/features/  check-in
src/overlay/   overlay window, ESP painter, projection
src/menu/      Nebula shell and pages for this app, config store
src/settings.* every user setting, in one visited list (defaults, save, load, export)
tools/         probe and self-test
```
