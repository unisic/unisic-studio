# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with
code in this repository. It carries the same rules as `AGENTS.md` plus
Claude-specific deep notes; read both. Where they overlap they agree, and if they
ever disagree, **AGENTS.md and the actual code win.**

## Build & Run

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/unisic-studio
```

unisic-kit is a **git submodule** — clone with `--recurse-submodules` (or run
`git submodule update --init --recursive`). Requires `qt6-qtbase-devel
qt6-qtdeclarative-devel qt6-qtsvg-devel` (Fedora). Compile-time-guarded optionals:
`pipewire-devel` (kit capture path, `HAVE_PIPEWIRE`), `libinput`/`libudev`
(click capture, `HAVE_LIBINPUT`), `LayerShellQt` (HUD, `HAVE_LAYERSHELL`) — the
build succeeds without any of them. **Runtime deps are live now:** `ffmpeg` +
`ffprobe` are shelled out (record, decode, export, probe) and located via
`QStandardPaths::findExecutable`; the **QtMultimedia QML plugin is runtime-only**
and gated behind `Studio.capVideoPlayback` — when it is absent the editor
degrades to a `PosterExtractor` still frame rather than failing.

`ctest --test-dir build` runs the unit suite (11 tests).

## What Unisic Studio is (and is not)

Unisic Studio is a **Screen Studio-like post-production tool for screen
recordings** on Linux Wayland: import a raw capture, auto-generate zoom/pan from
the cursor and click tracks, style it, and export via ffmpeg. Sibling of Unisic;
shares the `unisic-kit` foundation. GPLv3. Zero telemetry.

**It is NOT** a live screen recorder for casual clips (that is Unisic's job), a
general video editor, a cloud service, a cross-platform app, or an X11 tool.
Studio *does* record — but only as the front door to post-production, through the
kit's portal/PipeWire stack, never as a general capture utility.

**The pipeline is live end-to-end:** record (portal + PipeWire + libinput clicks)
→ project sidecar → auto-zoom → editor preview → export (MP4/WebM/GIF). ~15.8k
lines across `src/` + `qml/`, 11 unit tests. **README's "Status & roadmap" is the
authoritative statement of what is done** (it calls this **v1**); in-code comments
label the subsystem generations M1–M4. Do not re-derive the milestone here — read
README and the code.

**Known outstanding:** webcam **export** compositing (`src/render/RenderPipeline.cpp`
~L222 — schema, capture and *preview* all carry the webcam; export does not yet
composite the second layer), and broader live-session testing across compositors.

**`docs/NEXT-screen-studio-feel.md` is the LIVE SPEC** for the current motion and
cursor-feel work (spring camera, cursor dynamics, tuning sliders) — read it before
touching `src/engine/`. The "no feature creep" rule below still binds: this is a
mature pipeline, so the bar for *new* surface is higher, not lower.

## Prime directives

Every change is judged against these, in order:

1. **Lightweight.** Small binary, small dependency set, fast startup, low idle
   RAM/CPU.
2. **Correct.** No regressions in the load-bearing subsystems — settings
   persistence, the recorder, the project sidecar, the zoom/motion engine, and
   the render/export path are **all live and all load-bearing now**. When in
   doubt, verify on a real Wayland session (see below).
3. **No leaks.** Every `new` needs an owner; every temp file, D-Bus handle, and
   ffmpeg/PipeWire resource needs a teardown path.
4. **No feature creep.** Adding code is a cost. The best change is often a
   smaller diff, or a deletion.

## Architecture map

Every subsystem below **exists and ships**. The headers are densely commented and
are the real documentation — skim the `.h` before changing a `.cpp`.

- `external/unisic-kit/` — **submodule.** The shared foundation: `Unisic.Kit` QML
  design system (`Theme` singleton, the `U*` components, `SidebarItem`, …), plus
  C++ `ThemeController`, `IconImageProvider`, `ConfigPath`, `SettingMacro.h`, and
  the portal/PipeWire capture stack. **Kit changes go through the unisic-kit
  repo, never edited in-place under `external/`.**
- `src/main.cpp` — entry point: `QApplication`, dev-vs-stable identity, kit
  `ConfigPath` naming (`unisic-studio` / `unisic-studio-dev`), `QQuickStyle=Basic`,
  Breeze icon-fallback pinning, single-instance socket (UID-keyed, second launch
  forwards a bare `raise`), `IconImageProvider` registration, translator install,
  QML engine load, and the **dev CLI flags** below.
- `src/StudioApp.{h,cpp}` — **the facade** exposed to QML as context property
  `Studio` (analogous to Unisic's `App`). Now substantial: import, open/save,
  recording control, zoom regenerate/edit, file pickers, recents, input
  permission. **It is at its size limit** — keep new behaviour in focused
  subsystem classes that `StudioApp` merely wires up, never piled onto the facade.
- `src/StudioSettings.{h,cpp}` — persisted settings as `Q_PROPERTY`s via the
  kit's `U_SETTING` macro, on the kit `ConfigPath` file, with the 800 ms
  debounce-sync + `aboutToQuit` flush. Bare top-level keys (see landmines).

**`src/capture/`** — recording:
- `StudioRecorder` — the session orchestrator: portal ScreenCast (Cursor
  **Metadata** mode, so no pointer is baked into the pixels) → kit
  `PipeWireGrabber` → fixed-FPS wall-clock sampler → ffmpeg rawvideo stdin →
  master `.mkv` (H.264/CRF + FLAC) in the XDG cache. Handles countdown, pause
  excision, cancel.
- `ClickCapture` — global pointer-button capture via libinput/udev on its own
  thread. **Plain open, no `EVIOCGRAB`** — it observes input, never steals it.
  Inert without `HAVE_LIBINPUT`. Gives button + `CLOCK_MONOTONIC` only (no
  coordinates); the recorder resolves position from the cursor track.
- `InputPermission` — on-demand probe of whether libinput can see pointer
  devices. Returns a raw enum + raw command string; `tr()` wrapping happens at
  the UI layer.
- `RecordingAssembler` — raw capture data → `StudioProject` sidecar. **Its own TU
  for a hard reason** (see the `CursorSample` landmine).
- `RecorderMath.h` — pure sampling/timing math (unit-tested).

**`src/project/`** — the `.unisicstudio` sidecar document:
- `StudioProject` — schemaVersion 1, compact JSON, written **atomically**; refuses
  a newer schema. Points at the master video by relative + absolute path; owns
  `ZoomTimeline`/`StyleModel` as child QObjects, cursor/click tracks as values;
  `dirty` tracking.
- `ZoomTimeline` — `QAbstractListModel` of keyframes, **always kept sorted by
  `tMs`**. `clearAuto()` spares Manual and locked rows — regenerating the
  automatic camera must never discard the user's work.
- `StyleModel` — background/padding/rounding/shadow/frame/aspect/cursor styling;
  coalesced `changed()` for dirty-tracking.
- `CursorTrack` / `ClickTrack` — recorded input tracks (both unit-tested).

**`src/engine/`** — motion (pure logic: Core+Gui only, no QML, no clocks, no
randomness; identical inputs → byte-identical output, and the tests pin it):
- `KeyframeEngine` — cursor path + clicks → `ZoomTimeline` of Auto keyframes.
- `SpringCameraEvaluator` (in `KeyframeEngine.h`) — the camera. Keyframes are
  **targets**; a stateful spring chases them, so `evaluate(t)` is a deterministic
  simulation with a forward cache. **One evaluator per preview/export stream** so
  state cannot leak between them.
- `CursorSmoother` — one-euro filter over a `CursorTrack`; non-mutating.
- `TrajectoryMetrics` — smoothness/settling/bounds metrics the motion self-tests
  assert on.
- `Easing.h` — header-only cubic-bezier, no Qt. Engine and renderer must agree.

**`src/render/`** — export:
- `RenderPipeline` — renders **the same `qml/composition/CompositionRoot.qml`**
  as the live preview, offscreen, at export resolution.
- `ExportController` — thin QML-facing façade; maps UI choices to
  `RenderPipeline::Settings`, relays progress/state.
- `FrameDecoder` — ffmpeg → raw BGRA frames on a worker thread, with a
  **credit-semaphore backpressure** (N=4 in flight) that bounds memory.
- `VideoFrameItem` — `QQuickItem` bridging CPU frames to a scene-graph texture;
  parented into `CompositionRoot`'s `videoSlot` exactly where preview parents its
  `PreviewVideo`.
- `CursorShapeProvider` — serves recorded cursor bitmaps via
  `image://cursorshape/<projectId>/<shapeId>`; process-wide, mutex-guarded,
  ref-counted per project.

**`src/media/`** — `VideoProbe` (async `ffprobe` → duration/fps/size, used by
import) and `PosterExtractor` (single poster frame; **only** on the degraded
no-QtMultimedia path).

**`src/` glue:**
- `PreviewController` — the editor playback head. Smooths `timeMs` between coarse
  MediaPlayer updates and derives **both** the camera rect and cursor state from
  that ONE time, in C++, so QML never runs `evaluate()` per frame.
- `CursorPlayback` — click ripples active at the current instant.
- `EditorWindowManager` / `HudManager` — per-window `QQmlContext` idiom; each
  cascades its children off one `deleteLater()`. `HudManager` owns the
  always-on-top recording HUD (layer-shell when `HAVE_LAYERSHELL`).
- `RecentProjects` — JSON index next to the settings `.conf`; best-effort, prunes
  dead entries.
- `AutozoomSelfTest` / `MotionSelfTest` — the headless engine harnesses behind
  `--autozoom-test` / `--motion-test`.

**`qml/`** — `StudioMain` (sidebar + recents), `SettingsPage`, `EditorWindow`,
`InspectorPanel`, `Timeline`, `ZoomRectEditor`, `ExportDialog`, `RecordingHud`,
`SmokeTestDialog`, and `composition/` (`CompositionRoot`, `CursorOverlay`,
`PreviewVideo`). **New QML files must be added to `qt_add_qml_module`** in
`CMakeLists.txt` or lupdate won't scan them.

**`tests/`** — 11 unit tests (`ctest --test-dir build`), all green: cursor/click
tracks, zoom timeline, project, recorder math, recent projects, cursor smoother,
keyframe engine, cursor playback, trajectory metrics, preview controller.

**Dev CLI flags** (the headless verification surface — use them):
`--import <file>` (+ `--selftest`), `--autozoom-test`, `--motion-test`,
`--export-test` (+ `--format`/`--aspect`/`--bg`/`--trim-in`/`--trim-out`/
`--cancel-ms`), `--hud-test`, `--smoke-test`, `--page <projects|settings>`.

## Correctness landmines (hard-won — do not relearn these)

### Settings / persistence

- **NEVER use a QSettings group named `general`/`General` (any case).** It
  collides with INI's magic `General` section: writes serialize as `[%General]`,
  a *fresh* process parses that back as group `"General"`, and QSettings reads
  are case-sensitive so the read misses and returns defaults **every launch**.
  Studio's settings are **top-level bare keys** for exactly this reason.
- **Verify persistence from a FRESH process, never the writing process.**
  QSettings reads served from the in-process cache will lie. Launch a second
  process and dump `allKeys()` to check.
- **QSettings only flushes on `sync()`/destructor.** Abnormal exit loses
  everything since launch. That is why `StudioSettings` has the ~800 ms
  debounce-sync + `aboutToQuit` flush, and `main.cpp` installs SIGINT/SIGTERM/
  SIGHUP self-pipe handlers so destructors run. Don't remove them.
- **One config file**, named once via the kit's `UnisicKit::setConfigName(...)`
  in `main.cpp` BEFORE anything constructs a `StudioSettings` or the kit
  `ThemeController` — both write the same `~/.config/<name>/<name>.conf`. Don't
  introduce a second path.
- **Theme (`ui/theme`) is owned by the kit `ThemeController`** — do NOT duplicate
  it in `StudioSettings`.

### QML / kit

- **The kit's `ThemeController` is a module QML singleton** (engine-created); the
  `IconImageProvider` shares that one instance via `ThemeController::instance()`.
  Do NOT `qmlRegisterSingletonInstance` it into any URI — that clobbers the
  module's other `QML_ELEMENT` types.
- **All colors come from `Theme` tokens** (kit `qml/Theme.qml`); never hardcode a
  hex in QML. **Reuse the kit `U*` components** (`UButton`, `SidebarItem`,
  `UHoverTip`, `UIcon`, …) — don't reinvent a styled control inline.
- **Icons are freedesktop `iconName`s via `UIcon`** (served by `image://icon`);
  `UIcon` must stay `asynchronous: false` (the provider runs on the GUI thread).
- **Basic style is forced** (`QQuickStyle::setStyle("Basic")`). No Kirigami, no
  Breeze QML, no Qt Quick Controls default styling.

### Pipeline invariants (do not break these)

- **ONE composition.** `qml/composition/CompositionRoot.qml` drives **both** the
  live preview and the offscreen export — that is what makes export WYSIWYG *by
  construction*. Never fork a second styling implementation for export; if
  something looks right in preview and wrong in the file, the bug is in what got
  parented into the slot, not in a missing export-side copy.
- **ONE evaluator.** Preview and export both run `SpringCameraEvaluator`. Do not
  fork the motion code (`docs/NEXT-screen-studio-feel.md` restates this as a hard
  constraint). One *instance* per stream, though — the spring is stateful.
- **`struct CursorSample` collides.** The kit's `PipeWireGrabber.h` and the
  project's `CursorTrack.h` BOTH declare a **global** `struct CursorSample` with
  different fields, so **no single `.cpp` can include both.** `StudioRecorder`
  speaks to the kit grabber; `RecordingAssembler` speaks to the project model;
  they exchange only the neutral `Raw*` structs. That is why the assembler is its
  own translation unit — don't "simplify" it back in.
- **Cursor is captured in Metadata mode**, so the pointer is *not* in the pixels —
  the overlay draws it, in both preview and export. A "missing cursor" is an
  overlay/shape-registry bug, not a capture bug.
- **Coordinate systems:** cursor samples are **stream pixels**; the engine
  normalises to frame fractions `[0,1]`. Keyframe rects are the visible sub-rect
  of the source frame, also `[0,1]` — `(0,0,1,1)` means no zoom.

### Process / single-instance

- **Single-instance socket is keyed on UID alone**, deliberately not on any
  session/display env var — those disagree across autostart vs click vs
  keybind-spawn and would split into duplicate config writers. A second launch
  forwards a bare `raise` and exits. Dev builds use their own socket so stable
  and dev run side by side.
- **Dev build is a fully separate app** (`UNISIC_DEV_BUILD`, auto ON for local
  builds): own app name `unisic-studio-dev`, own desktop id, own single-instance
  socket, own config dir — so a build-tree binary never shadows or fights an
  installed stable Studio.

## Memory-safety and leak discipline (Qt)

- **Every `QObject` gets a parent or an explicit owner.** A parentless `new` that
  nobody stores is a leak (`StudioApp` owns `StudioSettings` via `this`).
- **Never `delete` a QObject with pending signals/events — use `deleteLater()`.**
  See the single-instance socket handling in `main.cpp`
  (`QLocalSocket::disconnected → deleteLater`).
- **Scope lambdas that capture raw pointers** — give the connection a context
  object (3-arg `connect`) so it auto-disconnects when that object dies.
- **Temp files / child processes must be cleaned up on every exit path, including
  signals.** This is **live, not hypothetical**: ffmpeg/ffprobe children
  (recorder, `FrameDecoder`, `VideoProbe`, `PosterExtractor`), the master `.mkv`
  in the XDG cache, `QTemporaryDir` posters, the `ClickCapture` thread, and the
  portal/PipeWire session all need a teardown path on stop **and** cancel **and**
  signal. When you add a subsystem, add its teardown in the same change.
- **The per-window `QQmlContext` idiom** (`EditorWindowManager`, `HudManager`):
  the context and the project's children are parented so that a single
  `project->deleteLater()` / `window->deleteLater()` cascades the whole tree.
  Preserve that shape — it is what keeps close paths leak-free.

## Internationalization (REQUIREMENT)

Every user-facing string MUST be wrapped in `qsTr()` (QML) / `tr()` (C++). English
is the source language and **still the only language shipped** — its `.ts` mirrors
the source text (translation == source). The wiring (`qt_add_lupdate` /
`qt_add_lrelease`, `HAVE_TRANSLATIONS`) is already structured to take more
languages: add `i18n/studio_<code>.ts` to `STUDIO_TS_FILES` in `CMakeLists.txt`
and translate. Workflow for any new/changed string: (1) add the `qsTr`/`tr` call;
(2) `cmake --build build --target update_translations` — lupdate appends new
strings as `type="unfinished"`; (3) fill every unfinished `<translation>` and drop
the marker. A plain build bakes `.qm` into the qrc, so `unfinished`/empty entries
silently fall back to English — do not ship those. Keep placeholders (`%1`) and
tokens verbatim. New QML files must be listed in `qt_add_qml_module` so lupdate
scans them.

## Verifying a change (do NOT skip this)

This is a GUI + Wayland app. Unit tests barely touch the load-bearing parts.
**The real verification is exercising the affected flow on a live Wayland session
and observing behavior** — not "it compiles."

1. **Build clean:** `cmake --build build` with no new warnings (kit-submodule
   warnings are pre-existing and out of scope — fix them upstream, not here).
2. **Unit suite:** `ctest --test-dir build` — 11 tests, all green. The engine
   tests pin determinism; if you touch `src/engine/`, they are not optional.
3. **Smoke offscreen:** `QT_QPA_PLATFORM=offscreen ./build/unisic-studio` boots
   with no QML load errors (`qrc` / missing-module errors on stderr).
   **⚠ Offscreen is NOT enough for render/export.** The offscreen QPA plugin
   cannot initialize a real GL RHI on many Mesa/EGL stacks, so `--export-test`,
   `--smoke-test` and `--hud-test` must run on a **live Wayland session**.
4. **Exercise the actual flow** on a real session, matching what you touched:
   - *settings* → change it, fully quit, relaunch a **fresh process**, confirm it
     persisted (fresh process — the in-process cache lies).
   - *engine/motion* → `--autozoom-test` / `--motion-test` and **report the
     numbers** (smoothness, settling, bounds), then watch a real clip: the camera
     must spring, hold without swimming, and ease out. Cheap-looking motion is a
     bug here, not a nitpick (`docs/NEXT-screen-studio-feel.md`).
   - *render/export* → `--export-test` on a live session, then **open the file**;
     it must match the preview frame-for-frame (WYSIWYG invariant).
   - *recording* → record a real clip on KWin Wayland; confirm cursor, clicks and
     the HUD, and that stop **and** cancel both leave no ffmpeg straggler.
5. **Watch for leaks/stragglers:** no orphaned ffmpeg, no runaway idle CPU, no
   growing RSS. `pgrep -x ffmpeg` after a stop/cancel should be empty.

Never report "done" for an untested runtime change. If you couldn't run it, say so
and name what still needs verification.

## Dependency policy

- **Do not add a new library** (Qt module, system `.so`, bundled source) without
  a strong justification and maintainer sign-off. Prefer shelling out to an
  already-required helper (ffmpeg) or a small self-contained implementation over
  a new link-time dependency.
- **Do not pull in Kirigami, Breeze, KDE Frameworks, or Boost.** The UI is the
  hand-built kit design system on Qt Quick Basic.
- New optional features that need a heavy dep follow the kit's
  `HAVE_PIPEWIRE`-style compile-time-guard pattern so the default build stays lean.

## Commits

- **Conventional Commits:** `feat:`, `fix:`, `refactor:`, `docs:`, `chore:`, etc.
  Subject in imperative mood, ≤ ~72 chars.
- **Explain the "why," especially for a landmine fix** — name the mechanism (e.g.
  the `[%General]` case-collision) so the fix isn't undone.
- **Branch off `main`; don't commit or push unless the human asks.** One logical
  change per PR.
