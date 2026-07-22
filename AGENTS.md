# AGENTS.md

Contributor guide for **any** AI coding agent working on Unisic Studio — Cursor,
Aider, Zed, Codex, Continue, Cline, Windsurf, GitHub Copilot Agent, Claude Code,
and humans reading over their shoulder. This is the canonical, tool-agnostic
contract. If your tool also reads `CLAUDE.md`, that file carries the same rules
plus deep per-subsystem notes — read both; where they overlap they agree, and if
they ever disagree, **this file and the actual code win.**

> **Read this whole file before your first edit.** Unisic Studio is small on
> purpose. The hard problems are often *invisible* — QSettings persistence
> quirks, Qt object lifetimes, the shared-submodule boundary. A change that
> "looks obviously correct" has repeatedly been the wrong one in its sibling
> project. Treat the landmines below as landmines, not trivia.

---

## 1. What Unisic Studio is (and is not)

Unisic Studio is a **Screen Studio-like post-production tool for screen
recordings** on Linux Wayland: import a raw capture, auto-generate zoom and pan
from the cursor and click tracks, style it, and export via ffmpeg. Stack:
**C++20, Qt 6 (6.5+), Qt Quick / QML**, fully custom UI from the shared
`unisic-kit` design system. GPLv3. Zero telemetry. It is a **sibling of Unisic**
(the live screenshot/recording tool) and shares its foundation library.

**It is NOT:** a live screen recorder for casual clips (Unisic owns that), a
general video editor, a cloud service, a cross-platform app, an X11 tool, or a
kitchen-sink utility. Studio *does* record — but only as the front door to
post-production, through the kit's portal/PipeWire stack, never as a general
capture utility. Every feature request is measured against "does a
screen-recording post-production workflow genuinely need this?"

**Current state: the pipeline is live end-to-end** — record (portal + PipeWire +
libinput clicks) → project sidecar → auto-zoom → editor preview → export
(MP4/WebM/GIF). ~15.8k lines across `src/` + `qml/`, 11 unit tests, all green.
**README's "Status & roadmap" is the authoritative statement of what is done** (it
calls this **v1**); in-code comments label the subsystem generations M1–M4. Don't
re-derive a milestone here — read README and the code.

**Known outstanding:** webcam **export** compositing (`src/render/RenderPipeline.cpp`
~L222 — the schema, the recording capture and the editor *preview* all carry the
webcam; export does not yet composite the second layer), and broader live-session
testing across compositors and hardware.

**`docs/NEXT-screen-studio-feel.md` is the LIVE SPEC** for the current motion and
cursor-feel work (spring camera, cursor dynamics, tuning sliders) — read it before
touching `src/engine/`. Note it is a *quality overhaul*, not a feature request:
"no feature creep" (§2) still binds, and on a mature pipeline the bar for **new**
surface is higher, not lower.

### Non-negotiable product constraints

- **Wayland-first, legit paths only.** Capture goes through the kit's
  portal/PipeWire stack — no X11 hacks, no screen-scraping, no
  compositor-specific bypasses of the security model. `ClickCapture` opens
  libinput devices with a **plain open, never `EVIOCGRAB`**: Studio observes
  input, it never steals it.
- **All UI colors flow from the kit's `Theme` tokens** (`qml/Theme.qml` in
  unisic-kit) — never hardcode a hex in a component. The mandatory palette
  (Primary `#17153B`, Secondary `#2E236C`, Tertiary `#433D8B`, Accent `#C8ACD6`)
  is enforced through Theme.
- **Zero telemetry, no network calls except user-configured export/upload.**

---

## 2. Prime directives

Every change is judged against these, in order:

1. **Lightweight.** Small binary, small dependency set, fast startup, low idle
   RAM/CPU.
2. **Correct.** No regressions in the load-bearing subsystems — settings
   persistence, the recorder, the project sidecar, the zoom/motion engine and the
   render/export path are **all live and all load-bearing now**. When in doubt,
   verify on a real Wayland session (§9).
3. **No leaks.** Qt makes ownership easy to get wrong. Every `new` needs an
   owner; every temp file, D-Bus handle, and ffmpeg/PipeWire resource needs a
   teardown path.
4. **No feature creep.** Adding code is a cost. The best PR is often a smaller
   diff, or a deletion.

If a change trades any of these away, it needs an explicit, written justification
in the PR — not a silent assumption.

---

## 3. The unisic-kit submodule

`external/unisic-kit` is a **git submodule** — the shared foundation of Unisic and
Unisic Studio (the `Unisic.Kit` QML design system + C++ `ThemeController`,
`IconImageProvider`, `ConfigPath`, `SettingMacro.h`, and the portal/PipeWire
capture stack).

- **Kit changes go through the unisic-kit repo, never edited in-place under
  `external/`.** An edit under `external/unisic-kit` is a detached, unversioned
  change that the next `submodule update` silently discards. If you need a kit
  behaviour change, make it in the kit repo, land it, then bump the submodule
  pointer here in a dedicated commit.
- **Clone with `--recurse-submodules`** (or `git submodule update --init
  --recursive`). The submodule URL currently points at a local path; it will be
  switched to `https://github.com/unisic/unisic-kit` once published.
- **Reuse kit components** (`UButton`, `SidebarItem`, `UHoverTip`, `UIcon`, …) —
  don't reinvent a styled control inline. Import `Unisic.Kit` in QML.

---

## 4. Build, run, and dependencies

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/unisic-studio
```

- **Toolchain:** CMake ≥ 3.21, a C++20 compiler, Ninja.
- **Required:** `qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg-devel`
  (Qt 6.5+, components `Core Gui Widgets Quick Qml QuickControls2 DBus Concurrent
  OpenGL`).
- **Optional, compile-time guarded** (the build succeeds without each, warning
  printed):
  - `pipewire-devel` → the kit's `PipeWireGrabber` capture path (`HAVE_PIPEWIRE`).
  - `libinput` + `libudev` → click capture (`HAVE_LIBINPUT`). Without it
    `ClickCapture` is inert by design: auto-zoom still works from cursor motion.
  - `LayerShellQt` → the layer-shell recording HUD (`HAVE_LAYERSHELL`).
  - Qt `LinguistTools` → translation build (`HAVE_TRANSLATIONS`).
- **Runtime helpers shelled out, not linked — live today:** `ffmpeg` (record
  encode, frame decode, export) and `ffprobe` (import probe). Located via
  `QStandardPaths::findExecutable`; degrade gracefully, never crash if absent.
- **QtMultimedia is a RUNTIME-only QML plugin**, not a link-time dep: the build
  succeeds without it and the app gates on `Studio.capVideoPlayback`, falling back
  to a `PosterExtractor` still frame. It **is** a hard dependency of the *packages*
  (live preview + webcam).
- **Tests:** `ctest --test-dir build` (11 tests; needs Qt6 `Test`).

**Dependency policy (this is a lightweight app):**
- **Do not add a new library** — Qt module, system `.so`, or bundled source —
  without a strong justification and maintainer sign-off. Prefer shelling out to
  an already-required helper, or a small self-contained implementation, over a
  new link-time dependency.
- **Do not pull in Kirigami, Breeze, KDE Frameworks, or Boost.** The UI is the
  hand-built kit design system on Qt Quick Basic style. `QQuickStyle::setStyle
  ("Basic")` is set in `main.cpp` for exactly this reason.
- New optional heavy deps must follow the kit's `HAVE_PIPEWIRE`-style
  compile-time-guard pattern so the default build stays lean.

---

## 5. Repository map

Everything below **exists and ships.** The headers are densely commented and are
the real documentation — skim the `.h` before you touch the `.cpp`.

```
external/unisic-kit/   SUBMODULE. Shared foundation (Unisic.Kit QML module + C++
                       theme/config/capture). Never edit in-place.
src/
  main.cpp             Entry: QApplication, dev/stable identity, kit ConfigPath
                       naming, QQuickStyle=Basic, Breeze icon fallback,
                       single-instance socket (UID-keyed, bare "raise"),
                       IconImageProvider, translator install, engine load,
                       dev CLI flags (below).
  StudioApp.{h,cpp}    THE facade exposed to QML as context property `Studio`:
                       import, open/save, recording control, zoom regenerate/edit,
                       pickers, recents, input permission. AT ITS SIZE LIMIT —
                       new behaviour goes in a subsystem it wires up, not here.
  StudioSettings.{h,cpp}  Persisted settings as Q_PROPERTYs (kit U_SETTING macro),
                       800 ms debounce-sync + aboutToQuit flush. Bare top-level
                       keys (see §6).
  PreviewController.*  Editor playback head: smooths timeMs between coarse
                       MediaPlayer updates; derives camera rect + cursor state
                       from that ONE time, in C++ (QML never evaluates per frame).
  CursorPlayback.*     Click ripples active at the current instant.
  EditorWindowManager.*  Editor window lifecycle; per-window QQmlContext idiom.
  HudManager.*         The always-on-top recording HUD (layer-shell when built).
  RecentProjects.*     JSON recents index beside the settings .conf; prunes dead.
  AutozoomSelfTest.*   Headless engine harness  -> --autozoom-test
  MotionSelfTest.*     Headless motion harness  -> --motion-test
  capture/             RECORDING.
    StudioRecorder.*     Session orchestrator: portal ScreenCast (cursor METADATA
                         mode) -> kit PipeWireGrabber -> fixed-FPS sampler ->
                         ffmpeg rawvideo stdin -> master .mkv (H.264/CRF + FLAC)
                         in XDG cache. Countdown, pause excision, cancel.
    ClickCapture.*       libinput/udev pointer buttons on its own thread. PLAIN
                         open, never EVIOCGRAB. Inert without HAVE_LIBINPUT.
    InputPermission.*    On-demand probe; raw enum + command string (tr() at UI).
    RecordingAssembler.* Raw capture -> StudioProject. Own TU for a hard reason
                         (the CursorSample collision, §6).
    RecorderMath.h       Pure sampling/timing math (unit-tested).
  project/             THE .unisicstudio SIDECAR DOCUMENT.
    StudioProject.*      schemaVersion 1, compact JSON, ATOMIC write; refuses a
                         newer schema. Master video by relative + absolute path.
                         Owns ZoomTimeline/StyleModel; dirty tracking.
    ZoomTimeline.*       QAbstractListModel of keyframes, ALWAYS sorted by tMs.
                         clearAuto() spares Manual + locked (never eat user work).
    StyleModel.*         Background/padding/rounding/shadow/frame/aspect/cursor.
    CursorTrack.* ClickTrack.*  Recorded input tracks.
  engine/              MOTION. Pure logic: Core+Gui only, no QML/clocks/randomness;
                       identical inputs -> byte-identical output (tests pin it).
    KeyframeEngine.*     Cursor path + clicks -> ZoomTimeline of Auto keyframes.
                         Also declares SpringCameraEvaluator: keyframes are
                         TARGETS, a stateful spring chases them; evaluate(t) is a
                         deterministic sim with a forward cache. ONE evaluator
                         instance per preview/export stream (state must not leak).
    CursorSmoother.*     One-euro filter over a CursorTrack; non-mutating.
    TrajectoryMetrics.*  Smoothness/settling/bounds metrics the self-tests assert.
    Easing.h             Header-only cubic-bezier, no Qt. Engine + renderer agree.
  render/              EXPORT.
    RenderPipeline.*     Renders THE SAME composition/CompositionRoot.qml as the
                         live preview, offscreen, at export resolution.
    ExportController.*   Thin QML façade -> RenderPipeline::Settings + progress.
    FrameDecoder.*       ffmpeg -> raw BGRA frames on a worker thread, with a
                         credit-semaphore backpressure (N=4) bounding memory.
    VideoFrameItem.*     QQuickItem bridging CPU frames to a SG texture; parented
                         into CompositionRoot's videoSlot (same slot as preview).
    CursorShapeProvider.*  image://cursorshape/<projectId>/<shapeId>; process-wide,
                         mutex-guarded, ref-counted per project.
  media/
    VideoProbe.*         Async ffprobe -> duration/fps/size (used by import).
    PosterExtractor.*    One poster frame; ONLY on the degraded no-QtMultimedia
                         path.
qml/
  StudioMain.qml       Sidebar (Projects / Settings) + recents grid.
  SettingsPage.qml  EditorWindow.qml  InspectorPanel.qml  Timeline.qml
  ZoomRectEditor.qml  ExportDialog.qml  RecordingHud.qml  SmokeTestDialog.qml
  composition/         CompositionRoot.qml (THE one composition — preview AND
                       export), CursorOverlay.qml, PreviewVideo.qml.
tests/                 11 unit tests -> ctest --test-dir build
docs/                  NEXT-screen-studio-feel.md = LIVE SPEC for motion work.
                       perf/ = audit reports.
resources/             .desktop, AppStream metainfo, app icon.
i18n/                  studio_en.ts (English source; structure ready for more).
packaging/             Arch PKGBUILD, Fedora/openSUSE spec, cpack DEB.
```

**Dev CLI flags — this is the headless verification surface, use it:**
`--import <file>` (+ `--selftest`), `--autozoom-test`, `--motion-test`,
`--export-test` (+ `--format` / `--aspect` / `--bg` / `--trim-in` / `--trim-out` /
`--cancel-ms`), `--hud-test`, `--smoke-test`, `--page <projects|settings>`.

The `src/` tree is meant to stay comprehensible in an afternoon — it is ~15.8k
lines now, so that goal is load-bearing, not aspirational. If a file balloons,
extract a focused helper rather than piling onto `StudioApp`.

**New QML files must be added to `qt_add_qml_module`** in `CMakeLists.txt`, or
they ship unscanned by lupdate (§8).

---

## 6. Correctness landmines (hard-won — do not relearn these)

### Settings / persistence

- **NEVER use a QSettings group named `general`/`General` (any case).** It
  collides with INI's magic `General` section: writes serialize as `[%General]`,
  a *fresh* process parses that back as group `"General"`, and QSettings reads are
  case-sensitive so the read misses and returns defaults **every launch**.
  Studio's settings are **top-level bare keys** for exactly this reason. Qt's own
  docs warn: "Do not use a group called 'General'."
- **Verify persistence from a FRESH process, never the writing process.**
  In-process QSettings reads are served from cache and will *lie*. Launch a
  second process and dump `allKeys()`.
- **QSettings only flushes on `sync()`/destructor.** Abnormal exit loses
  everything since launch. That is why `StudioSettings` has the ~800 ms
  debounce-sync + `aboutToQuit` flush, and `main.cpp` installs SIGINT/SIGTERM/
  SIGHUP self-pipe handlers *specifically so destructors run.* Don't remove them.
- **One config file**, named once via `UnisicKit::setConfigName(...)` in
  `main.cpp` BEFORE anything constructs a `StudioSettings` or the kit
  `ThemeController` — both write the same `~/.config/<name>/<name>.conf`
  (`unisic-studio`, or `unisic-studio-dev` for a dev build). Don't reintroduce a
  second path.
- **Theme (`ui/theme`) is owned by the kit `ThemeController`** — do NOT duplicate
  it in `StudioSettings`.

### QML singleton / kit

- **Do NOT `qmlRegisterSingletonInstance` the kit's `ThemeController` into any
  URI.** It is a module QML singleton (engine-created); the `IconImageProvider`
  shares the one instance via `ThemeController::instance()`. Registering it
  imperatively clobbers the module's other `QML_ELEMENT` types.
- **Icons are freedesktop `iconName`s via `UIcon`** (served by the C++
  `image://icon` provider); `UIcon` must stay `asynchronous: false` — the
  provider runs on the GUI thread and `QIcon::fromTheme`/`qApp->palette()` are not
  thread-safe.

### Pipeline invariants (do not break these)

- **ONE composition.** `qml/composition/CompositionRoot.qml` drives **both** the
  live preview and the offscreen export — that is what makes export WYSIWYG *by
  construction*. Never fork a second styling implementation for export. If preview
  looks right and the exported file doesn't, the bug is in what got parented into
  the slot, not in a missing export-side copy.
- **ONE evaluator.** Preview and export both run `SpringCameraEvaluator`; do not
  fork the motion code (`docs/NEXT-screen-studio-feel.md` restates this as a hard
  constraint). One *instance* per stream, though — the spring is stateful, and
  shared state across streams is a bug.
- **`struct CursorSample` collides.** The kit's `PipeWireGrabber.h` and the
  project's `CursorTrack.h` BOTH declare a **global** `struct CursorSample` with
  different fields, so **no single `.cpp` can include both.** `StudioRecorder`
  talks to the kit grabber; `RecordingAssembler` talks to the project model; they
  exchange only the neutral `Raw*` structs. That is *why* the assembler is its own
  translation unit — do not "simplify" it back into the recorder.
- **The cursor is captured in Metadata mode**, so the pointer is **not** baked
  into the pixels — the overlay draws it, in preview and export alike. A missing
  cursor is an overlay/shape-registry bug, not a capture bug.
- **Coordinate systems:** cursor samples are **stream pixels**; the engine
  normalises to frame fractions `[0,1]`. Keyframe rects are the visible sub-rect
  of the source frame, also `[0,1]` — `(0,0,1,1)` means "no zoom, whole frame".
- **The auto camera must never eat manual work.** Regeneration goes through
  `ZoomTimeline::clearAuto()`, which spares `Manual` and locked keyframes.

### Process / single-instance

- **Single-instance socket is keyed on UID alone**, deliberately not on any
  session/display env var — those disagree across autostart vs click vs
  keybind-spawn and would split into duplicate instances racing the config file.
  A second launch forwards a bare `raise` and exits.
- **Dev build is a fully separate app** (`UNISIC_DEV_BUILD`, auto ON for local
  builds): own app name, desktop id, single-instance socket and config dir — so a
  build-tree binary never shadows or fights an installed stable Studio.

---

## 7. Memory-safety and leak discipline (Qt)

- **Every `QObject` gets a parent, or an explicit owner.** A parentless `new
  QObject` that nobody stores is a leak. (`StudioApp` owns `StudioSettings` via
  `this`.)
- **Never `delete` a QObject that has pending signals/events queued to it — use
  `deleteLater()`.** See the single-instance socket handling in `main.cpp`.
- **Disconnect or scope lambdas that capture raw pointers.** Give the connection a
  context object (3-arg `connect`) so it auto-disconnects when that object dies.
- **Temp files and child processes must be cleaned up on every exit path**,
  including signals. Prefer `QTemporaryFile`/`QTemporaryDir` RAII. This is **live,
  not hypothetical**: ffmpeg/ffprobe children (recorder, `FrameDecoder`,
  `VideoProbe`, `PosterExtractor`), the master `.mkv` in the XDG cache, poster
  temp dirs, the `ClickCapture` thread, and the portal/PipeWire session each need
  a teardown path on stop **and** cancel **and** signal.
- **Keep the per-window `QQmlContext` idiom** (`EditorWindowManager`,
  `HudManager`): the context and the project's children are parented so one
  `project->deleteLater()` / `window->deleteLater()` cascades the whole tree. That
  shape is what keeps every close path leak-free — preserve it.
- **When you add a subsystem, add its teardown in the same PR.** Construction and
  destruction are one change, not two.

**Before committing anything nontrivial, mentally trace: who owns this object, and
when/where does it die?**

---

## 8. Internationalization (REQUIREMENT)

Every user-facing string MUST be wrapped in `qsTr()` (QML) / `tr()` (C++). English
is the source language and **still the only language shipped** — its `.ts` mirrors
the source text (translation == source). The build wiring (`qt_add_lupdate` /
`qt_add_lrelease`, `HAVE_TRANSLATIONS`) is already structured to take more
languages later: add `i18n/studio_<code>.ts` to `STUDIO_TS_FILES` in
`CMakeLists.txt` and translate.

Workflow for any new/changed string: (1) add the `qsTr`/`tr` call; (2)
`cmake --build build --target update_translations` — lupdate rescans C++ + QML and
appends new strings as `type="unfinished"`; (3) fill every unfinished
`<translation>` and drop the marker. A plain build bakes `.qm` into the qrc, so
`unfinished`/empty entries silently fall back to English — never ship those. Keep
placeholders (`%1`) and tokens verbatim. New QML files must be listed in
`qt_add_qml_module` so lupdate scans them.

---

## 9. Verifying a change (do NOT skip this)

This app is GUI + Wayland. Unit tests barely touch the load-bearing parts. **The
real verification is exercising the affected flow on a live Wayland session and
observing behavior** — not "it compiles."

1. **Build clean:** `cmake --build build` with no new warnings. (Warnings inside
   the `external/unisic-kit` submodule are pre-existing and out of scope — fix
   them upstream in the kit repo, not here.)
2. **Unit suite:** `ctest --test-dir build` — 11 tests, all green. The engine
   tests pin determinism; if you touch `src/engine/`, running them is mandatory.
3. **Smoke offscreen:** `QT_QPA_PLATFORM=offscreen ./build/unisic-studio` boots
   with no QML load errors (`qrc` / missing-module errors on stderr).
   **⚠ Offscreen is NOT sufficient for render/export/HUD.** The `offscreen` QPA
   plugin cannot initialize a real GL RHI on many Mesa/EGL stacks, so
   `--export-test`, `--smoke-test` and `--hud-test` must run on a **live Wayland
   session**. "It passed offscreen" is not evidence for the render path.
4. **Exercise the actual flow on a real session**, matching what you touched:
   - *settings* → change it, fully quit, relaunch a **fresh process**, confirm it
     persisted (fresh process — see §6; the in-process cache lies).
   - *engine/motion* → `--autozoom-test` / `--motion-test`, and **report the
     numbers** (smoothness, settling, bounds). Then watch a real clip: the camera
     must spring, hold without swimming, and ease out. Cheap-looking motion is a
     bug here, not a nitpick — see `docs/NEXT-screen-studio-feel.md`.
   - *render/export* → `--export-test` live, then **open the output file**: it
     must match the preview (the WYSIWYG invariant in §6).
   - *recording* → record a real clip on KWin Wayland; confirm cursor, clicks and
     HUD, and that stop **and** cancel each leave no ffmpeg straggler.
5. **Watch for leaks/stragglers:** no orphaned helper processes, no runaway idle
   CPU, no growing RSS. `pgrep -x ffmpeg` after stop/cancel should be empty.

Never report "done" for an untested runtime change. If you couldn't run it, state
that plainly and describe what still needs verification.

---

## 10. Commits and PRs

- **Conventional Commits:** `feat:`, `fix:`, `refactor:`, `docs:`, `chore:`, etc.
  Subject in imperative mood, ≤ ~72 chars.
- **Explain the "why," especially for a landmine fix.** Name the mechanism (the
  `[%General]` case-collision, the UID-keyed socket) so the fix isn't undone.
- **Branch off `main`; don't commit or push unless the human asks.** Never
  force-push shared branches. **One logical change per PR.**
- A bump of the `external/unisic-kit` submodule pointer is its own commit,
  separate from app changes, with the kit commit range in the message.

---

## 11. Quick "do NOT" list

- ❌ Edit files in-place under `external/unisic-kit` (submodule — change the kit
  repo, then bump the pointer).
- ❌ Introduce a QSettings group named `general`/`General`.
- ❌ Verify persistence by reading from the writing process.
- ❌ Duplicate the kit-owned `ui/theme` setting in `StudioSettings`.
- ❌ `qmlRegisterSingletonInstance` the kit's `ThemeController` into any URI.
- ❌ Add Kirigami / Breeze / KDE Frameworks / Boost / any heavy framework.
- ❌ Hardcode colors, version strings, or config paths.
- ❌ `new` a QObject with no owner, or `delete` one with pending events.
- ❌ Fork the composition or the motion evaluator so preview and export diverge.
- ❌ Include both the kit's `PipeWireGrabber.h` and the project's `CursorTrack.h`
  in one `.cpp` (the `CursorSample` collision — §6).
- ❌ Discard the user's Manual/locked keyframes when regenerating the auto camera.
- ❌ Claim the render/export path works because it passed under
  `QT_QPA_PLATFORM=offscreen` (no real GL RHI there — §9).
- ❌ Leave an ffmpeg child, portal session or temp file alive on a cancel path.
- ❌ Report "done" on a runtime change you didn't actually run.

---

*When this file and the code disagree, the code is authoritative — but fix the
drift: update this file in the same PR.*
