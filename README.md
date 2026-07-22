> [!CAUTION]
> This app is very experimental; it is not even in alpha. Use wisely 

# Unisic Studio

Unisic Studio is a **Screen Studio-like post-production tool for screen
recordings** on **Linux Wayland**. Import a raw capture (or record one), and it
turns it into a polished video: automatic zoom and pan driven by the cursor and
click tracks, a styled background and frame, non-destructive trim, an optional
webcam overlay, and export to MP4, WebM or GIF via ffmpeg.

It is a sibling of [Unisic](https://github.com/unisic/unisic) — the screenshot &
screen recorder — and shares its foundation library,
[unisic-kit](https://github.com/unisic/unisic-kit) (the `Unisic.Kit` QML design
system plus the C++ theme/config/capture pieces). GPLv3, zero telemetry.

**It is NOT** a live screen recorder for casual clips (that is Unisic's job), a
general-purpose video editor, a cloud service, a cross-platform app, or an X11
tool. It does one thing: make screen recordings look good.

> Screenshots: see [`docs/screenshots/`](docs/screenshots/) — **TODO**, real
> captures pending (the metainfo references `editor.png` / `export.png`).

## Features

- **Import or record** — open any screen recording, or capture one in-app via the
  desktop ScreenCast portal + PipeWire (monitor or window; the portal picker
  chooses). Recordings carry a cursor track, click track, and cursor bitmaps.
- **Automatic zoom & pan** — the engine reads the cursor path and clicks and
  generates a Screen-Studio-style camera: zoom in on the action, follow it, ease
  back out. Every auto keyframe is editable — add Manual keyframes, lock, nudge,
  retime, or regenerate.
- **Styling** — background as a flat color, a gradient (with curated presets), a
  wallpaper image, or **desktop blur** (a blurred copy of the recording's first
  frame); adjustable padding, corner radius, drop shadow, and a window frame
  (none / minimal hairline / macOS-style title bar).
- **Cursor & clicks** — scale the pointer, restyle it, and draw a ripple on each
  click.
- **Trim** — drag the in/out handles; the preview plays only the trimmed range and
  export honors it exactly.
- **Webcam overlay** *(optional)* — record a webcam alongside the screen and show
  it as a corner picture-in-picture (circle or rounded rectangle, any corner,
  scalable). Off by default. *Export compositing of the webcam is a work in
  progress; the editor preview shows it today.*
- **Export** — MP4 (H.264), WebM (VP9) or GIF (two-pass palette). Resolution
  presets, frame-rate cap, and a quality slider. Export is WYSIWYG — it renders
  the exact same composition the editor previews.
- **Themes & i18n** — the kit's nine palettes (including one that follows the
  system light/dark scheme) and full `qsTr()` wrapping. English ships at v1.

## Compositor support

Unisic Studio runs anywhere Wayland does, but how *complete* it feels depends on
what the compositor exposes to the ScreenCast portal — the same reality Unisic
lives with, not a choice:

- **KDE Plasma / KWin — full.** Native ScreenCast capture with cursor metadata,
  the whole editor, and export. The reference target.
- **GNOME / Mutter — full editing, capture works.** Import, editing, styling and
  export are complete; recording works through the portal. Some cursor niceties
  depend on Mutter's ScreenCast cursor mode.
- **wlroots** (Sway, Hyprland, river, Wayfire, COSMIC…) **— degraded cursor.**
  Capture and editing work; on backends that don't deliver cursor *metadata* the
  auto-zoom still runs off clicks but the cursor overlay is reduced.
- **X11 sessions** (e.g. Cinnamon) **— import-only.** No ScreenCast backend, so
  recording is unavailable; importing an existing video, editing and export all
  work.

## Build from source

Requires **C++20**, **Qt 6.5+**, **CMake** (Ninja recommended). unisic-kit is a
git submodule, so clone recursively:

```sh
git clone --recurse-submodules https://github.com/unisic/unisic-studio
cd unisic-studio
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/unisic-studio
```

Already cloned without `--recurse-submodules`? Run
`git submodule update --init --recursive`.

Dependencies (Fedora package names; use your distro's equivalents):

- `qt6-qtbase-devel` — Core, Gui, Widgets, DBus, Concurrent, OpenGL
- `qt6-qtdeclarative-devel` — Quick, Qml, QuickControls2, the QML tooling
- `qt6-qtsvg-devel` — the bundled symbolic icons
- `qt6-qtmultimedia` — **runtime**: the editor's live video preview and the
  webcam overlay both `import QtMultimedia` (with its ffmpeg backend)
- `ffmpeg` — **runtime**: decode/encode for import, preview posters and export
- `pipewire-devel` *(optional)* — the kit's `PipeWireGrabber` capture path
- `libinput-devel` + `systemd-devel` *(optional)* — click capture (see below);
  without them the build still succeeds and click capture is inert

## Click capture & the `input` group

Auto-zoom is best when it knows *when* you clicked. Studio reads mouse-button
timings via libinput, which needs read access to `/dev/input/*` — granted by
membership in the `input` group:

```sh
sudo usermod -aG input "$USER"
```

Log out and back in for it to take effect. **Be honest about what this grants:**
`input`-group membership lets any software you run observe *all* input devices
(keyboards included). Studio only reads mouse **button timings**, and only while
you are recording — but the permission itself is broad. If you'd rather not,
leave it off: auto-zoom still works from cursor motion, just with less precise
click-driven framing. Settings → Recording shows the exact status and the
copyable command.

## Status & roadmap

**v1 (this milestone).** Done and building green: import, the auto-zoom engine,
the live editor + preview, styling (backgrounds incl. gradient presets & desktop
blur, padding/rounding/shadow/frames), trim, portal + PipeWire recording, click
capture, and MP4/WebM/GIF export. The offscreen render/export path is verified on
a live Wayland session (it needs a real GL RHI — the `offscreen` QPA plugin can't
initialize it on many Mesa/EGL stacks).

Still to finish: **webcam export compositing** (the schema, recording capture and
editor preview are in place; export does not yet composite the second layer), and
broader **live-session testing** across compositors and hardware.

## Packaging

- **Arch** — `packaging/arch/PKGBUILD`
- **Fedora / openSUSE** — `packaging/unisic-studio.spec`
- **Debian / Ubuntu** — `cpack -G DEB` (config in `CMakeLists.txt`; the deb
  postinst refreshes the MIME + desktop caches so `*.unisicstudio` opens in the
  editor)

`QtMultimedia` is a hard runtime dependency of the packages (live preview +
webcam). **AppImage is intentionally skipped** — it is a CI-only artifact.

## Development

The kit is a submodule — kit changes go through the unisic-kit repo, never edited
in-place under `external/unisic-kit`. Dev builds are a fully separate app
identity (`unisic-studio-dev`: own config, socket and desktop id) so a build-tree
binary never fights an installed stable Studio. Dev builds also add an **F8**
smoke test and a **Developer** settings pane that exercises each path in
isolation.

## Privacy

Unisic Studio collects nothing. No telemetry, no crash reporting, no analytics,
no account, no network requests.

## TODO

- **Distinct app icon.** `resources/icons/unisic-studio.svg` is a placeholder
  copy of Unisic's mark — replace it with a Studio-specific icon.
- **Screenshots.** Populate `docs/screenshots/` with real captures.
- **Webcam export compositing** (see roadmap).

## License

GPL-3.0-only. See [`LICENSE`](LICENSE).
