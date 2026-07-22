# Cursor asset provenance

Every file in this directory is original work created for Unisic Studio and is
covered by the project's GPLv3 license. This note records how each was made, so
the license claim is verifiable and stays true.

## pointer.svg

- **Origin:** adapted from the **capitaine-cursors** theme's `default` (dark)
  pointer — © 2016 Keefer Rourke and others, **LGPL-3.0-or-later**, itself a
  clean-room redraw based on the KDE Breeze theme (NOT extracted Apple art).
  Upstream: <https://github.com/keeferrourke/capitaine-cursors>,
  `src/svg/dark/default.svg`. Full license text vendored beside this file as
  `LICENSE.capitaine-cursors`. LGPL-3.0 material may be combined into this
  GPLv3 project; the combined work remains GPLv3, this file's own terms remain
  LGPL-3.0-or-later.
- **Adaptation (ours):** the baked Gaussian-blur shadow path was removed
  (`qml/composition/CursorOverlay.qml` supplies the shadow), and the art was
  rescaled/translated onto a 24×32 canvas so the arrow tip sits exactly at the
  interaction hotspot (2, 1.5). No shape redrawing.
- **Deliberately not named after any OS or vendor.** It is the "Studio pointer".
- Geometry contract: 24×32 viewBox, hotspot (2, 1.5);
  `qml/composition/CursorOverlay.qml` hardcodes those ratios, so the viewBox and
  hotspot must not change without updating that file in lockstep.

### If you add a cursor asset here

Draw it yourself or vendor only a theme whose license is GPLv3-compatible AND
which is a genuine clean-room redraw (verify — some "open" cursor themes on
gnome-look / the KDE store are repackaged proprietary art). Record its exact
origin and license in this file. Never ship extracted macOS/Windows cursors:
they are licensed-not-sold assets whose redistribution the vendor forbids, and a
GPLv3 header on them would make this project's license notice false.
