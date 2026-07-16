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
qt6-qtdeclarative-devel qt6-qtsvg-devel` (Fedora); `pipewire-devel` is optional
(kit capture path, guarded). Runtime deps (ffmpeg, Qt Multimedia) land as the
feature set grows.

## What Unisic Studio is (and is not)

Unisic Studio is a **Screen Studio-like post-production tool for screen
recordings** on Linux Wayland: import a raw capture, auto-generate zoom/pan from
the cursor and click tracks, style it, and export via ffmpeg. Sibling of Unisic;
shares the `unisic-kit` foundation. GPLv3. Zero telemetry.

**It is NOT** a live screen recorder (that is Unisic's job), a general video
editor, a cloud service, a cross-platform app, or an X11 tool.

**This repo is at M0 (skeleton):** app boots, themed window opens, settings
persist. No recording/editing/export logic yet — do not add it speculatively.

## Prime directives

Every change is judged against these, in order:

1. **Lightweight.** Small binary, small dependency set, fast startup, low idle
   RAM/CPU.
2. **Correct.** No regressions in the load-bearing subsystems (settings
   persistence today; capture/render/export later). When in doubt, verify on a
   real Wayland session (see below).
3. **No leaks.** Every `new` needs an owner; every temp file, D-Bus handle, and
   ffmpeg/PipeWire resource needs a teardown path.
4. **No feature creep.** Adding code is a cost. The best change is often a
   smaller diff, or a deletion.

## Architecture map (M0)

- `external/unisic-kit/` — **submodule.** The shared foundation: `Unisic.Kit` QML
  design system (`Theme` singleton, the `U*` components, `SidebarItem`, …), plus
  C++ `ThemeController`, `IconImageProvider`, `ConfigPath`, `SettingMacro.h`, and
  the portal/PipeWire capture stack. **Kit changes go through the unisic-kit
  repo, never edited in-place under `external/`.**
- `src/main.cpp` — entry point: `QApplication`, dev-vs-stable identity, kit
  `ConfigPath` naming (`unisic-studio` / `unisic-studio-dev`), `QQuickStyle=Basic`,
  Breeze icon-fallback pinning, single-instance socket (UID-keyed, second launch
  forwards a bare `raise`), `IconImageProvider` registration, translator install,
  QML engine load.
- `src/StudioApp.{h,cpp}` — **the facade** exposed to QML as context property
  `Studio` (analogous to Unisic's `App`). Minimal today: `version`, `devBuild`,
  `settings`, `quit()`. It will grow to own the project model and the
  capture/render/export subsystems — keep new behaviour in focused subsystem
  classes that `StudioApp` wires up, not piled onto the facade.
- `src/StudioSettings.{h,cpp}` — persisted settings as `Q_PROPERTY`s via the
  kit's `U_SETTING` macro, on the kit `ConfigPath` file, with the 800 ms
  debounce-sync + `aboutToQuit` flush. Tiny on purpose.
- `qml/StudioMain.qml` — themed window: sidebar (Projects / Settings) + centered
  empty state.

**Planned layout** (do not scaffold ahead of need): `src/project/` (the project
document model + persistence), `src/engine/` (zoom/pan generation from cursor and
click tracks), `src/capture/` (import / recording bridge), `src/render/` (the
ffmpeg export pipeline). Studio-specific deep design lives in the maintainer's
approved plan, not invented here.

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
- **Temp files / child processes (ffmpeg later) must be cleaned up on every exit
  path, including signals.** When you add a subsystem, add its teardown in the
  same change.

## Internationalization (REQUIREMENT)

Every user-facing string MUST be wrapped in `qsTr()` (QML) / `tr()` (C++). English
is the source language and **the only language shipped at M0** — its `.ts` mirrors
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
2. **Smoke offscreen:** `QT_QPA_PLATFORM=offscreen ./build/unisic-studio` boots
   with no QML load errors (`qrc` / missing-module errors on stderr).
3. **Exercise the actual flow** on a real session: window opens themed, sidebar
   responds. Settings change → change it, fully quit, relaunch a **fresh
   process**, confirm it persisted.
4. **Watch for leaks/stragglers:** no runaway idle CPU, no growing RSS.

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
