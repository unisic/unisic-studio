#pragma once
#include "ZoomTimeline.h"

#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>

class CursorTrack;
class ClickTrack;

// The auto-keyframe engine: turns a recorded cursor path + click track into a
// ZoomTimeline of Auto keyframes — the Screen-Studio-style "zoom in on what the
// user is doing, follow it, zoom back out" camera. Pure logic: Core+Gui only,
// no QML, no clocks, no randomness. Identical inputs always give byte-identical
// output (the tests pin this).
//
// Keyframe type: we REUSE ZoomTimeline::Keyframe rather than mirror it. The
// engine's whole job is to produce rows for a ZoomTimeline, so sharing the
// struct means the caller inserts our output with zero conversion, and the
// evaluate() overloads below can be fed the live model list (Manual keyframes
// included) unchanged.
//
// Coordinate systems: cursor samples are STREAM pixels; videoSize is that
// stream's pixel size. Internally we normalise to frame fractions [0,1]x[0,1].
// Output keyframe rects are the visible sub-rect of the SOURCE frame, also in
// [0,1] — rect (0,0,1,1) means "no zoom, whole frame".
//
// Easing semantics (preview and export MUST read these the same way): a
// keyframe carries how to reach ITS rect from the previous keyframe. Between
// keyframe A (time tA) and B (time tB), the rect HOLDS at A.rect until the last
// B.easeInMs of the interval, then eases A.rect -> B.rect over
// min(B.easeInMs, tB-tA) using the Easing::preset curve, landing exactly on
// B.rect at tB. Before the first / after the last keyframe the rect clamps.
// (easeOutMs is metadata the generator uses to time the zoom-out keyframe; the
// evaluator only needs easeInMs because every transition is an "ease into B".)
class KeyframeEngine
{
public:
    using Keyframe = ZoomTimeline::Keyframe;

    // Tunables, round-tripped through ZoomTimeline::autoParams() as JSON so a
    // project reload regenerates identically. All times in ms, all *Frac values
    // are fractions of frame WIDTH unless noted.
    struct Params {
        // --- click clustering ---
        int clickClusterGapMs = 1200;      // max gap between clicks in one cluster
        double clickClusterDistFrac = 0.18;// max spread (of width) within a cluster
        // --- span timing around a cluster ---
        int leadInMs = 650;                // start zooming this long before 1st click
        int tailMs = 900;                  // hold this long after the last click
        int zoomInMs = 650;                // full -> zoomed transition duration
        int zoomOutMs = 900;               // zoomed -> full transition duration
        int minHoldMs = 1000;              // min time held at full zoom per span
        // --- idle / return-to-full ---
        int idleAfterMs = 2500;            // sub-threshold speed this long -> zoom out
        double idleSpeedFracPerSec = 0.02; // speed threshold, width-fraction / s
        // --- zoom geometry ---
        double zoomMin = 1.4;              // gentlest zoom (linear scale vs full)
        double zoomMax = 2.5;              // tightest zoom
        double marginFrac = 0.12;          // padding around the cluster bbox
        // --- dead-zone camera ---
        double deadZoneFrac = 0.35;        // inner fraction of the view the cursor
                                           // roams without the camera panning
        // --- dwell fallback (no clicks) ---
        int dwellMinMs = 900;              // min sustained slow-motion to count
        double dwellSpeedFracPerSec = 0.02;// speed threshold for a dwell

        QJsonObject toJson() const;
        static Params fromJson(const QJsonObject &o);
    };

    // Produce Auto keyframes for the whole video. `aspect` is the OUTPUT aspect
    // ("16:9", "4:3", …); empty/invalid falls back to the source aspect. The
    // caller owns the regeneration contract: it calls ZoomTimeline::clearAuto()
    // then inserts these; it passes the surviving `pinned` keyframes (Manual or
    // locked) so the engine can route AROUND them — any auto span overlapping a
    // pinned keyframe's [t-easeIn, t+easeOut] window is trimmed away from the
    // window, and dropped if trimming leaves it too short to be worth a zoom.
    static QVector<Keyframe> generate(const CursorTrack &cursor,
                                      const ClickTrack &clicks,
                                      QSize videoSize,
                                      qint64 durationMs,
                                      const QString &aspect,
                                      const Params &params,
                                      const QVector<Keyframe> &pinned = {});

    // Visible rect at time t for a sorted keyframe list (see easing semantics
    // above). Qt6 QVector IS QList, so this single overload also accepts the
    // live ZoomTimeline::keyframes() list (Manual keyframes included) that
    // preview and export evaluate.
    static QRectF evaluate(const QVector<Keyframe> &keyframes, qint64 tMs);

    // Parse an "W:H" aspect string to a ratio; <=0 or malformed -> fallback.
    static double aspectRatio(const QString &aspect, double fallback);

    // The visible sub-rect (normalised [0,1], carrying the OUTPUT aspect) for a
    // camera centred at `center` (normalised frame coords) zoomed by linear
    // factor `zoom` (>=1). Same letterbox-fit + edge-clamp geometry generate()
    // uses internally, exposed so Manual keyframes placed by the UI frame
    // identically to Auto ones. `aspect` is the OUTPUT aspect ("16:9", …);
    // "source"/malformed falls back to the source aspect from videoSize.
    static QRectF cameraRect(QSize videoSize, const QString &aspect, QPointF center,
                             double zoom);

    // The linear zoom factor implied by a camera rect (inverse of cameraRect's
    // sizing) for the given output aspect — 1.0 == full frame. Used by the
    // inspector to seed its zoom slider from the selected keyframe.
    static double zoomOfRect(QSize videoSize, const QString &aspect, const QRectF &rect);
};
