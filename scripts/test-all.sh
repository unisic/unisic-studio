#!/usr/bin/env bash
# test-all.sh — the full functional test environment for unisic-studio.
#
# Runs every automated check the app ships: the CTest unit suite, an offscreen
# boot smoke, the hidden dev-aid flags (--import --selftest, --smoke-test,
# --autozoom-test, --motion-test, --hud-test) and an --export-test matrix over
# formats/aspects/trim/cancel, validating each produced file with ffprobe.
#
# GL note: the export/render path needs a real GPU session (the offscreen QPA
# cannot init the GL RHI on most Mesa stacks — documented in RenderPipeline).
# Steps needing GL are skipped with a warning when no Wayland/X session exists.
#
# Single-instance note: the dev build owns a UID-keyed socket. A running
# interactive dev Studio would swallow every flag-driven launch as a bare
# "raise", so we refuse to start while one is up.
#
# Usage: scripts/test-all.sh [--quick]
#   --quick  skip the slower export matrix (keeps one mp4 + one gif export)

set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
BIN=$ROOT/build/unisic-studio
QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

PASS=0; FAIL=0; SKIP=0
declare -a FAILURES=()

say()  { printf '\n\033[1m== %s ==\033[0m\n' "$1"; }
ok()   { PASS=$((PASS+1)); printf '\033[32mPASS\033[0m %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); FAILURES+=("$1"); printf '\033[31mFAIL\033[0m %s\n' "$1"; }
skip() { SKIP=$((SKIP+1)); printf '\033[33mSKIP\033[0m %s\n' "$1"; }

WORK=$(mktemp -d /tmp/unisic-studio-tests.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

# ---- preconditions ---------------------------------------------------------
say "preconditions"
command -v ffmpeg  >/dev/null || { echo "ffmpeg missing";  exit 2; }
command -v ffprobe >/dev/null || { echo "ffprobe missing"; exit 2; }

# A live interactive instance would swallow flag launches as "raise".
if pgrep -f "$BIN" >/dev/null 2>&1; then
    echo "ERROR: a unisic-studio instance from this build tree is already running." >&2
    echo "Close it (or: pkill -TERM -f build/unisic-studio) and re-run." >&2
    exit 2
fi

HAVE_SESSION=0
[ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ] && HAVE_SESSION=1
[ $HAVE_SESSION -eq 1 ] || echo "note: no Wayland/X session — GL-dependent steps will be skipped"

# ---- build -----------------------------------------------------------------
say "build"
if cmake --build "$ROOT/build" >"$WORK/build.log" 2>&1; then
    ok "cmake --build"
else
    bad "cmake --build (see $WORK/build.log)"
    tail -20 "$WORK/build.log"
    exit 1
fi

# ---- unit tests ------------------------------------------------------------
say "ctest unit suite"
if (cd "$ROOT/build" && ctest --output-on-failure >"$WORK/ctest.log" 2>&1); then
    ok "ctest ($(grep -c 'Passed' "$WORK/ctest.log") tests)"
else
    bad "ctest"; tail -30 "$WORK/ctest.log"
fi

# ---- offscreen boot smoke --------------------------------------------------
# Boots the full QML engine without a session; SIGTERM after 4 s must exit
# cleanly (the signal self-pipe flushes settings). QML load errors land on
# stderr and fail the step.
say "offscreen boot smoke"
QT_QPA_PLATFORM=offscreen timeout --signal=TERM --kill-after=5 4 \
    "$BIN" >"$WORK/boot.log" 2>&1
rc=$?
if [ $rc -ne 124 ] && [ $rc -ne 0 ]; then          # 124 = timeout fired (app was alive) = good
    bad "offscreen boot (exit $rc)"; tail -10 "$WORK/boot.log"
elif grep -qiE "qml.*error|module.*not installed|failed to load" "$WORK/boot.log"; then
    bad "offscreen boot: QML errors"; grep -iE "qml|error" "$WORK/boot.log" | head -5
else
    ok "offscreen boot + clean SIGTERM exit"
fi

# ---- synthetic test clip ---------------------------------------------------
say "test assets"
CLIP=$WORK/clip.mp4
if ffmpeg -y -loglevel error \
      -f lavfi -i "testsrc2=size=1280x720:rate=30:duration=6" \
      -f lavfi -i "sine=frequency=440:duration=6" \
      -c:v libx264 -pix_fmt yuv420p -c:a aac "$CLIP"; then
    ok "synthetic clip (1280x720@30, 6 s)"
else
    bad "ffmpeg clip generation"; exit 1
fi

# ---- import + save/reload selftest -----------------------------------------
say "import + project save/reload selftest"
if QT_QPA_PLATFORM=offscreen timeout 60 "$BIN" --import "$CLIP" --selftest \
       >"$WORK/selftest.log" 2>&1; then
    ok "--import --selftest ($(grep -o 'reload=[A-Z]*' "$WORK/selftest.log" | head -1))"
else
    bad "--import --selftest"; tail -10 "$WORK/selftest.log"
fi

# ---- GL-dependent steps ----------------------------------------------------
if [ $HAVE_SESSION -eq 0 ]; then
    skip "smoke-test / autozoom / motion / hud / export matrix (no GL session)"
else
    say "built-in smoke test (F8 equivalent)"
    if timeout 300 "$BIN" --smoke-test >"$WORK/smoke.log" 2>&1; then
        ok "--smoke-test ($(grep -oE '[0-9]+ PASS' "$WORK/smoke.log" | tail -1))"
    else
        bad "--smoke-test"; grep -E "FAIL" "$WORK/smoke.log" | head -10
    fi

    say "motion/autozoom self-tests"
    for t in autozoom-test motion-test; do
        if timeout 180 "$BIN" --$t >"$WORK/$t.log" 2>&1; then
            ok "--$t"
        else
            bad "--$t"; tail -10 "$WORK/$t.log"
        fi
    done

    say "recording HUD component"
    if timeout 60 "$BIN" --hud-test >"$WORK/hud.log" 2>&1; then
        ok "--hud-test"
    else
        bad "--hud-test"; tail -5 "$WORK/hud.log"
    fi

    # ---- export matrix -----------------------------------------------------
    # Each row: label | extra flags | expected codec | expected WxH
    say "export matrix"
    probe() { ffprobe -v error -select_streams v:0 -show_entries \
              stream=codec_name,width,height -of csv=p=0 "$1" 2>/dev/null; }

    run_export() { # label out flags... ; expects EXPECT_CODEC EXPECT_DIM set
        local label=$1 out=$2; shift 2
        if ! timeout 240 "$BIN" --export-test "$CLIP" "$out" "$@" \
                >"$WORK/export-$label.log" 2>&1; then
            bad "export $label ($(tail -1 "$WORK/export-$label.log"))"; return
        fi
        [ -s "$out" ] || { bad "export $label: no output file"; return; }
        local got; got=$(probe "$out")
        case $got in
            "$EXPECT_CODEC,$EXPECT_DIM") ok "export $label ($got)";;
            *) bad "export $label: ffprobe '$got', want '$EXPECT_CODEC,$EXPECT_DIM'";;
        esac
    }

    EXPECT_CODEC=h264 EXPECT_DIM=1280,720 run_export mp4 "$WORK/out.mp4"
    EXPECT_CODEC=gif  EXPECT_DIM=1280,720 run_export gif "$WORK/out.gif" --format gif

    if [ $QUICK -eq 0 ]; then
        EXPECT_CODEC=vp9  EXPECT_DIM=1280,720 run_export webm "$WORK/out.webm" --format webm
        EXPECT_CODEC=h264 EXPECT_DIM=1280,720 run_export aspect-16x9 "$WORK/a169.mp4" --aspect 16:9
        EXPECT_CODEC=h264 EXPECT_DIM=1280,720 run_export aspect-9x16 "$WORK/a916.mp4" --aspect 9:16
        EXPECT_CODEC=h264 EXPECT_DIM=1280,720 run_export aspect-1x1 "$WORK/a11.mp4" --aspect 1:1
        EXPECT_CODEC=h264 EXPECT_DIM=1280,720 run_export trim "$WORK/trim.mp4" --trim-in 1000 --trim-out 4000
        # trim duration check: ~3 s
        d=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$WORK/trim.mp4" 2>/dev/null)
        awk -v d="$d" 'BEGIN{exit !(d>2.5 && d<3.5)}' \
            && ok "trim duration ${d}s (want ~3)" || bad "trim duration ${d}s (want ~3)"
        EXPECT_CODEC=h264 EXPECT_DIM=1280,720 run_export desktopblur "$WORK/blur.mp4" --bg desktopBlur

        # cancel path: partial output must be deleted, no orphan ffmpeg. A short
        # clip can finish before the timer fires — only judge the run if the
        # log proves the cancel actually happened ("cancelling" line).
        if timeout 60 "$BIN" --export-test "$CLIP" "$WORK/cancel.mp4" --cancel-ms 300 \
               >"$WORK/export-cancel.log" 2>&1; then
            if ! grep -q "cancelling" "$WORK/export-cancel.log"; then
                skip "cancel path (export outran the 300 ms cancel timer)"
            else
                sleep 1
                if [ -e "$WORK/cancel.mp4" ]; then bad "cancel: partial output left on disk"
                elif pgrep -f "ffmpeg.*cancel.mp4" >/dev/null; then bad "cancel: orphan ffmpeg"
                else ok "cancel path (partial deleted, no orphan ffmpeg)"; fi
            fi
        else
            bad "export cancel ($(tail -1 "$WORK/export-cancel.log"))"
        fi
    fi
fi

# ---- summary ---------------------------------------------------------------
say "summary"
printf '%d pass, %d fail, %d skip\n' "$PASS" "$FAIL" "$SKIP"
if [ $FAIL -gt 0 ]; then
    printf 'failed:\n'; printf '  - %s\n' "${FAILURES[@]}"
    exit 1
fi
exit 0
