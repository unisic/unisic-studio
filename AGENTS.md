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

**It is NOT:** a live screen recorder (Unisic owns that), a general video editor,
a cloud service, a cross-platform app, an X11 tool, or a kitchen-sink utility.
Every feature request is measured against "does a screen-recording
post-production workflow genuinely need this?"

**Current milestone: M0 (skeleton).** The app boots, a themed main window opens,
and settings persistence is wired. There is no recording/editing/export logic
yet. Do not add it speculatively — Studio-specific deep design lives in the
maintainer's approved plan.

### Non-negotiable product constraints

- **Wayland-first, legit paths only.** When capture/import lands it goes through
  the kit's portal/PipeWire stack — no X11 hacks, no screen-scraping, no
  compositor-specific bypasses of the security model.
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
   persistence today; the project model, zoom/pan engine and export pipeline
   later. When in doubt, verify on a real Wayland session (§7).
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
  (+ `Widgets DBus Concurrent QuickControls2` Qt modules).
- **Optional, compile-time guarded:** `pipewire-devel` → the kit's
  `PipeWireGrabber` capture path. The build succeeds without it (warning printed).
- **Runtime helpers shelled out, not linked (later milestones):** `ffmpeg`
  (export). Detect with `QStandardPaths::findExecutable`, degrade gracefully,
  never crash if absent.

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

## 5. Repository map (M0)

```
external/unisic-kit/   SUBMODULE. Shared foundation (Unisic.Kit QML module + C++
                       theme/config/capture). Never edit in-place.
src/
  main.cpp             Entry point: QApplication, dev/stable identity, kit
                       ConfigPath naming, QQuickStyle=Basic, Breeze icon fallback,
                       single-instance socket (UID-keyed, bare "raise"),
                       IconImageProvider, translator install, engine load.
  StudioApp.{h,cpp}    THE facade exposed to QML as context property `Studio`.
                       Minimal (version/devBuild/settings/quit) — grows later.
  StudioSettings.{h,cpp}  Persisted settings as Q_PROPERTYs (kit U_SETTING macro),
                       800 ms debounce-sync + aboutToQuit flush.
qml/
  StudioMain.qml       Themed window: sidebar + centered empty state.
resources/             .desktop, AppStream metainfo, app icon (placeholder).
i18n/                  studio_en.ts (English source; structure ready for more).
```

**Planned (do not scaffold ahead of need):** `src/project/` (project document
model + persistence), `src/engine/` (zoom/pan generation from cursor/click
tracks), `src/capture/` (import / recording bridge), `src/render/` (ffmpeg export
pipeline).

The `src/` tree is meant to stay comprehensible in an afternoon. If a file
balloons, extract a focused helper rather than piling onto `StudioApp`.

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
- **Temp files and child processes (ffmpeg, later) must be cleaned up on every
  exit path**, including signals. Prefer `QTemporaryFile`/`QTemporaryDir` RAII.
- **When you add a subsystem, add its teardown in the same PR.** Construction and
  destruction are one change, not two.

**Before committing anything nontrivial, mentally trace: who owns this object, and
when/where does it die?**

---

## 8. Internationalization (REQUIREMENT)

Every user-facing string MUST be wrapped in `qsTr()` (QML) / `tr()` (C++). English
is the source language and **the only language shipped at M0** — its `.ts` mirrors
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
2. **Smoke offscreen:** `QT_QPA_PLATFORM=offscreen ./build/unisic-studio` boots
   with no QML load errors (`qrc` / missing-module errors on stderr).
3. **Exercise the actual flow on a real session:** the window opens themed, the
   sidebar responds. Settings change → change it, fully quit, relaunch a **fresh
   process**, confirm it persisted (fresh process — see §6).
4. **Watch for leaks/stragglers:** no orphaned helper processes, no runaway idle
   CPU, no growing RSS.

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
- ❌ Add recording/editing/export logic before its milestone and the approved
  plan.
- ❌ Report "done" on a runtime change you didn't actually run.

---

*When this file and the code disagree, the code is authoritative — but fix the
drift: update this file in the same PR.*
