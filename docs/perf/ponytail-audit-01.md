# Ponytail Performance Audit #1 — Unisic Studio

Date: 2026-07-17 · Auditor: ponytail-perf-audit (measurement-first, **no code changes**)
Target: `unisic-studio` (Qt6/QML, Wayland; kit submodule; PipeWire capture;
QQuickRenderControl offscreen export; ffmpeg subprocesses). Milestone ≈ M3.

---

## verdict: PASS

Pure application logic idles perfectly (offscreen: 0.0% CPU, 82.9 MB flat, 2
threads, ~zero wakeups for 40 s). Every timer in the code is recording- or
playback-scoped; there is no idle polling, heartbeat, or background thread that
wakes with no work. The export/render path has bounded memory (4-frame decoder
backpressure + 32 MB encoder write cap), leaks nothing meaningful (2.42 MB
one-time at exit), and reaps its ffmpeg children on normal finish, explicit
cancel, and app-quit-mid-export. `KeyframeEngine::generate()` on a 10-minute
60 Hz track is 0.54 ms. No race, deadlock, hang, or runaway was reproduced.

Findings are optimizations (export inner-loop allocation churn) and one
cleanup gap (partial output left on quit-during-export). **No P0, no P1.**

---

## baseline

`baseline: build=Release(-O3 -DNDEBUG) for idle/RSS, RelWithDebInfo(-O2 -g) for heaptrack/perf/gdb; idle CPU avg=0.0%; idle CPU peak=0.0% (steady); RSS: offscreen-idle=82.9MB, live main-window idle=189MB, editor-open+paused=233MB; heap live bytes: export peak=100.6MB, leaked-at-exit=2.42MB (one-time); memory slope=~0 per idle-minute (flat); threads: offscreen=2, live-idle=8; wakeups: ~0 ctx-switches over 40s idle (offscreen vctx frozen at 10).`

Host: Fedora, Linux 7.1.3, 16 cores, 31 GB RAM, NVIDIA GPU (proprietary GL,
`cuda` GL worker thread present), live Wayland session. Tools present:
heaptrack, perf (paranoid=2, user-space OK), valgrind, gdb, powertop, lsof,
busctl, ffmpeg, pmap. Absent: strace, qmlprofiler, gammaray.

### Idle evidence (the load-bearing result)

| scenario | duration | CPU | RSS | threads | wakeups |
|---|---|---|---|---|---|
| offscreen, main window | 40 s | 0.0% (flat) | 82.9 MB (constant) | 2 | vctx frozen @10 (asleep) |
| live, main window (pre-input) | 27 s | 0.0% (flat) | 189.8 MB (constant) | 8 | vctx 79 total in 27 s |
| live, editor open + preview paused (`--import`, offscreen) | 30 s | 0.0% after t≈3 s | 233.8 MB (constant) | 4 | vctx frozen @900 (asleep) |

Startup work is one-time: offscreen reaches flat `utime=0.10 s` within ~1 s of
launch; live main window is up and at 0% within ~1 s. No recurring user-code
stack while idle. Live idle threads are `unisic-studio` (main),
2×`WaylandEventThr`, `QQmlThread`, `QDBusConnection`, and one NVIDIA GL `cuda`
thread — none of which spin.

Commands (representative):
```sh
# pure app-logic idle (no GPU, no compositor) — isolates timers/polling
QT_QPA_PLATFORM=offscreen  ./build/unisic-studio          # sampled /proc every 1s, 40s
# live main-window idle
./build/unisic-studio                                     # sampled /proc every 1s
# editor open, preview NOT played, deterministic (no click needed)
QT_QPA_PLATFORM=offscreen  ./build/unisic-studio --import /tmp/studio-src6.mp4
```
Sampler read `utime+stime` from `/proc/<pid>/stat`, RSS from `statm`, threads
from `/proc/<pid>/task`, ctxt-switches from `/proc/<pid>/status`.

---

## Ranked findings

`P2 allocation: [src/render/RenderPipeline.cpp:327] The encoder frame writer emits one QProcess::write() per scanline (720 writes/frame). Evidence: heaptrack peak consumer = 37.37 MB over 86,399 calls via QByteArray→QRingBuffer::reserve←QProcess::writeData←writeFrameToEncoder for a 120-frame 1280×720 export (86,399 ≈ 720×120). Root cause: the loop writes grabbed.constScanLine(y) row by row, but Format_ARGB32 at this width is contiguous (w*4 is 4-byte aligned — the code even relies on that in FrameDecoder). Each small write forces a fresh QRingChunk allocation whenever the encoder pipe is behind. Minimum fix: when bytesPerLine()==width*4, write the whole frame once — m_encoder->write(grabbed.constBits(), grabbed.sizeInBytes()) — else keep the per-row loop. Cuts ~86k allocations to ~120. Verify: re-run heaptrack on --export-test; the QRingBuffer peak-consumer call count drops from ~86k to ~frame-count.`

`P2 allocation: [src/render/RenderPipeline.cpp:308] Every exported frame does grabbed.convertTo(QImage::Format_ARGB32) after m_fbo->toImage(), a full-frame format conversion copy per frame (3.6 MB/frame at 720p), on top of the readback allocation and VideoFrameItem::setFrame's m_pending.detach() copy and per-frame window()->createTextureFromImage(). Evidence: heaptrack — export churns 387k allocation calls / 46k temporaries in 2.64 s while peak heap stays 100 MB (frames are freed each iteration, so this is churn, not growth). Root cause: the FBO read format (toImage default) differs from ffmpeg's expected bgra byte order, so a conversion is forced; and setFrame detaches although FrameDecoder already hands over a freshly-allocated QImage per frame (nothing reuses that buffer). Minimum fix: request the FBO/read path in a layout that matches the pipe (avoid the convertTo), and drop the detach() when the producer does not reuse its buffer. Deletion over addition — no new cache. Verify: heaptrack temporary-allocation count per exported frame before/after.`

`P2 lifecycle: [src/render/RenderPipeline.cpp:80] A partial export file is left on disk when the app is quit (SIGTERM) mid-export. Evidence: 5/5 SIGTERM-during-export runs left a 13–40 KB truncated .mp4 at the user's chosen output path (runtime). Root cause: ~RenderPipeline() reaps the decoder + encoder + scene but does NOT call deletePartialOutput(); only cancel() and fail() delete the partial. Quit routes through qApp→~ExportController→~RenderPipeline, skipping that cleanup — a broken file masquerades as a finished export. Minimum fix: call deletePartialOutput() from ~RenderPipeline() (or from cancel() invoked on ExportController when the app is quitting while state==Running). Verify: start --export-test, SIGTERM at ~1 s, assert the output path does not exist. (Note: teardown is otherwise clean — 0.4–1.0 s to exit, zero orphan ffmpeg across 5 runs; explicit ExportController::cancel() already deletes the partial and leaves no orphan.)`

`P3 uncertain: [src/StudioApp.cpp:458] Regenerating the auto-camera fires the preview recompute O(N) times per regenerate. Evidence: static — generateZoom() loops zoom->addKeyframe(kf); each addKeyframe (ZoomTimeline.cpp:71) emits changed(), and PreviewController connects ZoomTimeline::changed → recompute() (PreviewController.cpp:21), which runs KeyframeEngine::evaluate over the growing list. For a 10-minute recording generate() emits ~350 keyframes (measured), so a live editor recomputes ~350× per regenerate (≈O(N²) evaluate work) plus ~350 model row-insert signals driving Timeline delegate churn. generate() itself is only 0.54 ms (measured, refuted as a hot path), but the per-keyframe changed()/recompute storm to a live preview + Timeline was NOT measured end-to-end. Minimum fix: batch the emission — insert the keyframes under one beginResetModel/endResetModel and emit changed() once after the loop. Verify: instrument recompute() call count during regenerateZoom on a 10-min project with an editor open.`

`P3 uncertain: [qml/composition/PreviewVideo.qml:25] The editor preview decodes the FULL-resolution master. Evidence: driving playback of a 2560×1440@60 master (via the live session) pinned ~20–30% CPU, ~50 QtMultimedia/libavcodec decoder threads, and ~500 MB RSS; a paused preview is 0% (measured). Root cause: MediaPlayer decodes native resolution regardless of the viewport size, so previewing a 4K/1440p recording costs full decode even though the on-screen preview is smaller. This is legitimate work, but a viewport-scaled decode would cost less. Uncertain: the runtime numbers above are contaminated by the test-harness phantom clicks (see limitations) and were not isolated; also QtMultimedia offers no easy downscale-decode knob. Verify: play a 1440p and a 720p master with the pointer parked off-window and compare steady-state CPU/threads.`

`P3 uncertain: [live RSS] Live RSS does not fully return after heavy preview playback. Evidence: during phantom-click-driven 1440p60 playback, RSS climbed 189 → ~500–596 MB and settled at 497 MB (did not return to 189 MB), but it cycled rather than growing monotonically across the run. heaptrack on the export path reports only 2.42 MB leaked at exit. Root cause (likely): glibc-malloc arena retention + NVIDIA GL/decoder buffer caching, not a code-level leak. Not a confirmed leak. Verify: repeated open→play→close of an editor in one process with GammaRay live-object counts (not runnable here — no GammaRay, no in-process open/close driver).`

---

## coverage

Scenarios actually executed:

1. **Cold launch** — offscreen + live. Startup work is one-time (~0.2 CPU-s,
   window usable < ~1 s). ✓
2. **Stabilized idle** — offscreen 40 s (0.0%, 82.9 MB, 2 threads, ~0 wakeups);
   live main-window 27 s (0.0%, 189 MB, 8 threads); editor-open + preview paused
   30 s (0.0%, 233 MB). All flat. ✓
3. **Core workflow / export** — `--export-test` (720p@30, 120 frames) under
   heaptrack: peak heap 100.6 MB, 2.42 MB leaked (one-time), bounded (4-credit
   decoder backpressure + 32 MB encoder cap), no orphan ffmpeg. Allocation
   hot-loop identified. `--autozoom-test` end-to-end (generate ×5 + 2 GL exports
   + pixel probes) exit 0, deterministic, peak RSS 476 MB, no orphan ffmpeg. ✓
4. **Auto-zoom (M3)** — `--autozoom-test`: `KeyframeEngine::generate()` 10-min /
   18,182 samples → 350 kf in **0.54 ms** (best of 5). Preview per-tick path
   (CursorPlayback, cached-hint interpolation) reviewed — O(1)/tick, scratch
   reused. ✓ (generate() hot-path suspect **refuted**.)
5. **Concurrency / cancel** — explicit cancel mid-export: clean (no orphan,
   partial deleted). SIGTERM mid-export ×5: no hang (0.4–1.0 s exit), zero orphan
   ffmpeg. ✓
6. **Shutdown / signals** — SIGINT/SIGTERM self-pipe path verified to run
   destructors (app exits cleanly, ffmpeg reaped). Partial-output cleanup gap
   found (see P2 lifecycle). ✓

Static-only (verified by reading + tracing callers, not executed at runtime):

- **Recorder** (`StudioRecorder`, `ClickCapture`, `RecordingAssembler`) — timer
  inventory (`m_sampler`/`m_maxTimer`/`m_elapsedTick`/`m_cursorDrain`) all
  recording-scoped and stopped in `stop()`/`cancel()`; PipeWire grabber joined
  **off the GUI thread** via `QtConcurrent::run` (avoids blocking the repaint);
  `ClickCapture` worker uses `poll(-1)` + eventfd stop (no busy-wait) and is
  `wait()`-joined; encoder/converter `QProcess` reaped via `finished` +
  `deleteLater`, non-blocking `FfmpegUtil::stopProcess` on teardown. Disciplined.
- **Window lifecycle** (`EditorWindowManager`, `HudManager`) — per-window
  `QQmlContext` + `PreviewController` + `PosterExtractor` parented to the project;
  close cascades `win->deleteLater()` + `project->deleteLater()`; HUD window
  `deleteLater()` cascades its context. Ownership looks correct.

---

## limitations

- **Live recording NOT executed** (per instruction: the xdg portal picker needs a
  human). All recorder findings are static. Runtime recorder behavior — actual
  sampler cadence, PipeWire thread join under load, pause/excise ffmpeg pass,
  audio mixing — is **not measured**.
- **In-process lifecycle loop (open/close editor ≥30×) NOT executed.** No headless
  CLI drives repeated in-process window open/close, and GammaRay is absent, so
  per-iteration live-object growth (HudManager/EditorWindowManager QQmlContext +
  window destruction) is unproven at runtime. Teardown reviewed statically; a
  single editor open→app-quit reclaimed to a stable 233 MB. The P3 "RSS retention"
  item needs this loop to resolve.
- **Test-harness phantom input.** The automated live Wayland session delivered
  stray pointer *release* events onto the recent-project tile (confirmed by a GDB
  backtrace: `QQuickMouseArea::clicked → mouseReleaseEvent → StudioApp::openProject`).
  These auto-opened an editor **and** started playback of a large master,
  producing the transient 50-thread / ~600 MB / 20–30% CPU bursts first seen
  during idle sampling. **This is a measurement artifact, not an app defect** —
  it is why the P3 preview/RSS numbers are marked uncertain and why the clean
  idle baseline was taken offscreen and via deterministic `--import`.
- **perf call-graphs thin.** Release is `-O3` without frame pointers; the idle
  process was genuinely asleep during the 6 s `perf record` window (0.27 MB, empty
  report — consistent with 0% CPU), so no idle user-stack was captured (there was
  none to capture). Hot-path attribution came from heaptrack on RelWithDebInfo.
- **No ThreadSanitizer run.** Races were reasoned statically (recorder cross-thread
  hand-offs are `Qt::QueuedConnection`-marshaled to the GUI thread; VideoFrameItem
  guards `m_pending` with a mutex and hops to the GUI thread from workers). A TSan
  stress build was not produced this pass.

---

## net

`net: confirmed leaks=0; growth paths=0 (export peak plateaus; leaked-at-exit 2.42 MB is one-time Qt/QML/GL singletons + test's qApp-parented objects); races=0 reproduced; deadlocks=0 reproduced; idle recurring stacks=0; estimated removable steady-state RAM=unknown (idle is already near-floor: 83 MB offscreen / 189 MB live; the P2 export-loop churn is transient, not steady-state).`

Idle clean. Memory plateaus. No races or deadlocks reproduced. The two P2
allocation items and the P2 partial-cleanup gap are worth a small follow-up diff;
the P3 items need the un-executed runtime coverage (live recording, in-process
lifecycle loop, clean preview-playback isolation) before acting.
