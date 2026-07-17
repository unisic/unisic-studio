# Prompt: make Unisic Studio feel like Screen Studio

You are working on **unisic-studio** (`/run/media/dean/00E7-8685/FUN/Dev/unisic studio`,
C++20/Qt6/QML, kit submodule at `external/unisic-kit`). The app records screen +
cursor/click metadata and auto-generates zoom/pan for a polished screencast, à la
screen.studio. The pipeline works end-to-end (record → project → auto-zoom → preview →
export MP4/WebM/GIF) but the **motion and cursor feel cheap and unusable** — nowhere near
Screen Studio. This task is a quality overhaul, not new features. Read `CLAUDE.md` +
`AGENTS.md` first. Verify on a live Wayland session, not just "it compiles".

## The core problem (read before touching anything)

Screen Studio's signature is **physical motion**: the camera and the cursor move with
**spring dynamics** (mass–stiffness–damping), not linear/eased keyframes. Everything
overshoots slightly, settles, and never jitters. The current implementation uses a
one-euro filter for the cursor and cubic-bezier keyframe interpolation for the camera
(`src/engine/KeyframeEngine.cpp`, `evaluate()`), which is fundamentally the wrong model
and is why it reads as "amateur". **Replacing the interpolation model with a spring
simulation is the single highest-impact change.** Do that first.

## 1. Camera motion → critically-damped spring (highest priority)

- Introduce a spring-based camera evaluator. Instead of interpolating between keyframe
  rects with `easeIn/easeOut`, treat the keyframes as a sequence of **targets** and run a
  per-frame critically-damped spring (or double-spring: one for center, one for zoom
  scale) that chases the current target rect. Parameters: stiffness, damping ratio
  (default critically damped ≈ 1.0, expose a small "smoothness" 0..1 that maps to
  stiffness), max velocity clamp.
- The spring is **stateful over time**, so `evaluate(t)` must become a deterministic
  simulation from t=0 to t (fixed timestep, e.g. 4–8ms substeps, cached forward so
  scrubbing/looping is cheap — memoize the last simulated state and only re-simulate
  forward; on seek-backwards, resimulate from the nearest cached checkpoint). Preview and
  export MUST call the identical evaluator (this is already the invariant — keep it).
- Result must have: smooth zoom-IN with a touch of overshoot, a stable HOLD (no swimming
  while "held"), and a gentle zoom-OUT that decelerates into full frame. No linear ramps,
  no visible keyframe "snapping".
- Keep the existing keyframe model (`ZoomTimeline`, Auto/Manual/locked) as the **target
  source**; only the interpolation between targets changes. Manual editing still works.
- Add engine tests: spring never exceeds frame bounds, settles within tolerance of the
  target within N ms, deterministic for fixed input, continuity (no rect jump > small ε
  between consecutive output frames).

## 2. Cursor: this is the make-or-break feature — fix it properly

Current cursor (`src/CursorPlayback.*`, `qml/composition/CursorOverlay.qml`,
`resources/cursors/pointer.svg`) still looks/feels wrong. Rebuild the cursor feel:

- **Spring-follow cursor.** The *rendered* cursor position must chase the real
  (smoothed) position with its OWN spring — softer than the camera — so it glides and
  catches up with a slight lag/overshoot, the Screen-Studio "floaty" cursor. Not 1:1, not
  one-euro-only. (Keep the raw track for click-position resolution and engine input.)
- **Cursor quality & scaling.** Ship a crisp, correctly-hotspotted pointer. When the
  camera zooms in, the cursor should **not** shrink to nothing and should stay
  comfortably readable — scale the cursor partly independent of zoom (e.g. cursor keeps a
  roughly constant on-screen size, configurable). Render it above the video, inside the
  zoom transform but with its own scale compensation. Verify crispness at 1×, 2×, 2.8×.
- **Cursor styles that actually look good:** a premium default pointer (macOS-like:
  white fill, dark outline, soft shadow, subtle size), plus `system` (recorded bitmap),
  and a couple of tasteful alternates. Kill anything that looks flat/programmer-art.
- **Idle auto-hide:** fade the cursor out after it's been still for ~2s, fade back on
  movement (Screen Studio does this — reduces clutter during pauses).
- **Click feedback that looks premium:** replace the flat ripple with a tasteful click
  effect — a soft expanding ring + brief cursor "press" dip, correct color/opacity,
  ~350–450ms, eased with a spring/out-curve. Optional subtle radial highlight
  (spotlight) around the click. Make it look designed, not debug-drawn.
- Everything must be a **pure function of time** so preview == export exactly.

## 3. Auto-zoom intelligence (so it looks smart, not random)

The user reports auto-zooms are inaccurate. Tune `KeyframeEngine::generate()`:

- Zoom targets should **center on the actual click/activity point** and frame it
  naturally — not over-zoom, not clip the point to an edge. Adaptive zoom factor (tighter
  for a small dense cluster, looser for spread-out activity), sane default max ≈ 2.2–2.6.
- Correct timing: quick-but-soft zoom-in shortly BEFORE/at the click, comfortable hold
  through the interaction, unhurried zoom-out on idle. Merge nearby interactions instead
  of pumping in-and-out (the "yo-yo" is a classic tell of a bad auto-zoomer).
- No zoom on trivial cursor twitches; require real dwell/click evidence. Dead-zone
  camera so small cursor moves inside the framed area don't cause panning ("swimming").
- Make the whole thing tunable via a small **"Zoom intensity"** control (0..1) in the UI
  that scales factor + frequency, and a **"Motion smoothness"** control that maps to the
  spring stiffness from §1. These two sliders are what a user reaches for first.

## 4. Premium defaults (first recording must look good with zero tweaking)

Screen Studio's out-of-box look sells it. Set defaults in `StyleModel` so a fresh import
immediately looks polished: generous padding, a soft realistic drop shadow, ~12–16px
corner radius, a pleasant default gradient background, cursor size ~1.5–1.7×, click
highlight on, auto-zoom + spring motion on. Pick a good default aspect handling (fill for
vertical/square, fit for source). The empty/first-run state should look like a product,
not a test harness. Review the default background preset list — make the first one a
tasteful, muted gradient (not a loud one).

## 5. Preview smoothness

- Preview must render the spring evaluator at display rate (target 60fps) via the
  existing `PreviewController` clock — smooth, no stutter, no per-frame QML JS evaluation
  (keep evaluation in C++, expose a NOTIFY-ing rect + cursor state, as now). Confirm on
  weaker hardware it degrades gracefully (cache effect layers; never recompute the
  blurred background per frame).
- Playback controls must feel right: Space play/pause already exists — make sure the
  playhead, spring camera, and cursor all stay perfectly in sync during scrub and loop.

## Constraints & process

- One evaluator for preview and export (WYSIWYG) — do not fork the motion code.
- Theme tokens + kit components only; no hardcoded hex; no new link-time deps; no kit
  edits (kit changes go through the kit repo). Keep QObject ownership/teardown discipline.
- Don't regress the working pipeline (record, GIF/MP4/WebM export, trim, HUD layer-shell,
  keyboard shortcuts, on-canvas zoom rect editor).
- Add dev CLI/tests you can actually run headless: extend `--autozoom-test` to dump the
  spring-camera rect trajectory and assert smoothness/settling metrics; a
  `--motion-test` that exports a clip and measures frame-to-frame rect velocity/accel to
  prove no jitter and bounded overshoot. Report the numbers.
- Live-session pass on KWin Wayland: record a real 30–60s clip, watch the cursor glide,
  the camera spring, the click effects; confirm export matches preview. Screen-record a
  short before/after if possible.

## Definition of done

A non-technical viewer, shown a 30s exported clip, should read it as "a polished Screen
Studio-style screencast" — floaty spring cursor, smooth spring camera that zooms into
clicks and eases back, premium default styling — with nothing that looks janky, linear,
jittery, or debug-drawn. Ship the two tuning sliders (Zoom intensity, Motion smoothness)
so the feel is adjustable.
