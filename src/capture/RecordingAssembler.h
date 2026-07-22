#pragma once
#include <QImage>
#include <QList>
#include <QPair>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QVector>
#include <cstdint>

// Turns a finished recording's raw capture data into a StudioProject sidecar.
//
// It exists as its own translation unit for a hard reason: the kit's
// PipeWireGrabber.h and the project's CursorTrack.h BOTH declare a global
// `struct CursorSample` with different fields, so no single .cpp can include
// both. StudioRecorder speaks to the kit grabber; this assembler speaks to the
// project model; they exchange only the neutral Raw* structs below.
namespace RecordingAssembler {

// A cursor sample straight off the wire: CLOCK_MONOTONIC ns + stream pixels, with
// the spa_meta_cursor shape id (0 = unknown) rather than a track shape index.
struct RawCursor {
    qint64 tMonoNs = 0;
    double x = 0.0;
    double y = 0.0;
    bool visible = true;
    int spaShapeId = 0;
};

// A libinput button event: CLOCK_MONOTONIC ns, a Qt::MouseButton int, pressed/released.
struct RawClick {
    qint64 tMonoNs = 0;
    int button = 0;
    bool pressed = false;
};

// A distinct cursor bitmap seen during capture, keyed by its spa_meta_cursor id.
struct RawShape {
    int spaId = 0;
    QImage image;
    QPoint hotspot;
};

struct Input {
    QVector<RawCursor> cursors;
    QVector<RawClick> clicks;
    QVector<RawShape> shapes;
    // Key-DOWN timestamps only (CLOCK_MONOTONIC ns) — no keycodes are ever
    // captured. Coalesced into [startMs,endMs] typing bursts, pauses excised,
    // before anything is written. Empty unless the user opted into typing capture.
    QVector<qint64> keyDownMonoNs;
    QList<QPair<qint64, qint64>> pauseMonoNs;   // completed [startMonoNs,endMonoNs)

    qint64 t0MonoNs = 0;       // pts of the first sampled frame == video-time origin
    int fps = 60;
    qint64 framesWritten = 0;  // encoded frame count (incl. frozen pause frames)
    QSize videoSize;           // encoded (even-clamped) size

    QString masterAbsPath;     // final .mkv, already in the projects directory
    QString sidecarPath;       // where to write the .unisicstudio
    QString webcamAbsPath;     // optional webcam sidecar .mkv (empty = none)

    QString cursorMode;        // metadata|embedded|none
    bool hadClickCapture = false;
    QString compositor;        // XDG_CURRENT_DESKTOP
};

// Build the CursorTrack + ClickTrack in video time, excise the pause spans from
// both, assemble a StudioProject (video fields + recording metadata + default
// style) and save the sidecar. Returns true on success; on failure returns false
// and, if error != nullptr, sets an untranslated reason (the caller wraps/tr()s).
//
// Cursor sample tMonoNs -> video-ms clamps at 0 (video time starts at 0). Click
// positions are resolved from the pre-excision cursor track at the click's instant
// (CursorTrack::sample clamps to the first/last sample outside the recorded span),
// then both tracks are excised together so a click keeps agreeing with the cursor.
bool assembleAndSave(const Input &in, QString *error);

// Coalesce sorted key-down times (video-ms) into [start,end] typing bursts: keys
// closer than mergeGapMs join one burst, each burst's end is padded by tailPadMs
// so the zoom lingers past the last stroke, and a burst is dropped unless it holds
// at least minKeysPerBurst strokes (so a stray keypress never triggers a zoom).
// Pure function — unit-tested directly. PRIVACY: input is timestamps only.
QList<QPair<qint64, qint64>> coalesceTypingBursts(const QVector<qint64> &downMs,
                                                  qint64 mergeGapMs, qint64 tailPadMs,
                                                  int minKeysPerBurst);

} // namespace RecordingAssembler
