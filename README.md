# Unisic Studio

Unisic Studio is a Screen Studio-like post-production tool for screen recordings
on **Linux Wayland**. It turns a raw capture into a polished video: automatic
zoom and pan driven by the cursor and click tracks, styling, and export via
ffmpeg. It is a sibling of [Unisic](https://github.com/unisic/unisic) and shares
its foundation library, [unisic-kit](https://github.com/unisic/unisic-kit)
(the `Unisic.Kit` QML design system + the C++ theme/config/capture pieces).

## Status

**M0 — skeleton.** The app boots, a themed main window opens, and settings
persistence is wired. There is no recording, editing, or export logic yet: the
main window shows an empty state with the two primary actions disabled
(`Import Video…` → M1, `New Recording` → M2).

## Building

Requires **C++20**, **Qt 6.5+**, **CMake** (Ninja recommended). unisic-kit is a
git submodule, so clone recursively:

```sh
git clone --recurse-submodules <repo-url>
cd unisic-studio
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/unisic-studio
```

Already cloned without `--recurse-submodules`? Run
`git submodule update --init --recursive`.

Build dependencies (Fedora package names; use your distro's equivalents):

- `qt6-qtbase-devel` — Core, Gui, Widgets, DBus, Concurrent
- `qt6-qtdeclarative-devel` — Quick, Qml, QuickControls2, the QML module tooling
- `qt6-qtsvg-devel` — renders the bundled symbolic icons
- `pipewire-devel` *(optional)* — enables the kit's `PipeWireGrabber` screen-frame
  capture. Without it the build still succeeds; only that path is dropped.

Runtime dependencies land later as the feature set grows: **ffmpeg** (export),
and Qt Multimedia (preview playback).

## Submodule note

The `external/unisic-kit` submodule currently points at a local path. It will be
switched to `https://github.com/unisic/unisic-kit` once unisic-kit is published.
Kit changes go through the unisic-kit repo — never edit files in-place under
`external/unisic-kit`.

## TODO

- **Distinct app icon.** `resources/icons/unisic-studio.svg` is currently a copy
  of Unisic's mark as a placeholder. Replace it with a Studio-specific icon.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
