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
// struct means the caller inserts our output with zero conversion. Camera
// playback is handled by SpringCameraEvaluator below; each preview/export owns
// one evaluator so its state and forward cache cannot leak into another stream.
//
// Coordinate systems: cursor samples are STREAM pixels; videoSize is that
// stream's pixel size. Internally we normalise to frame fractions [0,1]x[0,1].
// Output keyframe rects are the visible sub-rect of the SOURCE frame, also in
// [0,1] — rect (0,0,1,1) means "no zoom, whole frame".
//
// Timing semantics: tMs remains the intended arrival time used by the editor and
// generator. The spring starts chasing a keyframe at tMs-easeInMs (never before
// the previous keyframe), preserving pre-click lead-in without snapping to the
// target at tMs. easeOutMs remains generator/pinned-window metadata.
class KeyframeEngine
{
public:
    using Keyframe = ZoomTimeline::Keyframe;

    // Tunables, round-tripped through ZoomTimeline::autoParams() as JSON so a
    // project reload regenerates identically. All times in ms, all *Frac values
    // are fractions of frame WIDTH unless noted.
    struct Params {
        // --- click clustering ---
        int clickClusterGapMs = 1300;      // max gap between clicks in one cluster
        double clickClusterDistFrac = 0.18;// max spread (of width) within a cluster
        // --- span timing around a cluster ---
        int leadInMs = 650;                // start zooming this long before 1st click
        int tailMs = 1100;                 // hold this long after the last click
        int zoomInMs = 650;                // full -> zoomed transition duration
        int zoomOutMs = 1100;              // zoomed -> full transition duration
        int minHoldMs = 1200;              // min time held at full zoom per span
        // --- idle / return-to-full ---
        int idleAfterMs = 2500;            // sub-threshold speed this long -> zoom out
        double idleSpeedFracPerSec = 0.02; // speed threshold, width-fraction / s
        // --- zoom geometry ---
        double zoomMin = 1.45;             // gentlest zoom (linear scale vs full)
        double zoomMax = 2.45;             // premium default cap; intensity can reach 2.55
        double marginFrac = 0.12;          // breathing room around interaction bbox
        // --- dead-zone camera ---
        double deadZoneFrac = 0.50;        // inner fraction of the view the cursor
                                           // roams without camera swimming
        // --- dwell fallback (no clicks) ---
        int dwellMinMs = 1100;             // min sustained slow-motion to count
        double dwellSpeedFracPerSec = 0.02;// speed threshold for a dwell
        // --- user-facing motion controls ---
        double zoomIntensity = 0.72;       // 0=no automatic interaction zoom, 1=strong
        double motionSmoothness = 0.68;    // 0=responsive/stiff, 1=soft/floaty spring
        // --- crop-to-fill (aspect fill mode) ---
        // When true, the base (non-zoomed) camera is the largest centred OUTPUT-
        // aspect crop of the source instead of the whole frame, and it slow-pans to
        // follow the cursor between interaction segments (Screen-Studio behaviour, no
        // letterbox bars). Coincides with the whole frame for a source-aspect output.
        bool fill = false;

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
    // typingBursts are [startMs,endMs] spans of sustained keyboard activity
    // (timing only — never keycodes). Each becomes dwell evidence anchored on the
    // cursor position at its start, so the camera HOLDS its zoom through typing
    // (usually into a field you just clicked) instead of yo-yoing out mid-type.
    static QVector<Keyframe> generate(const CursorTrack &cursor,
                                      const ClickTrack &clicks,
                                      QSize videoSize,
                                      qint64 durationMs,
                                      const QString &aspect,
                                      const Params &params,
                                      const QVector<Keyframe> &pinned = {},
                                      const QVector<QPair<qint64, qint64>> &typingBursts = {});

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

// Deterministic physical camera shared by preview, export, and motion tests.
// Targets come from ZoomTimeline keyframes; center uses a critically-damped
// spring while log-size uses a near-critical spring for a small, bounded zoom
// overshoot. Simulation advances on a canonical 5 ms grid. Arbitrary query
// remainders are evaluated from a copy of the grid state, so 30 fps export and
// ~60 fps preview return the same rect for the same absolute timestamp.
// Forward playback memoizes state; backward seeks resume from the nearest 500 ms
// checkpoint and replay deterministically.
class SpringCameraEvaluator
{
public:
    using Keyframe = ZoomTimeline::Keyframe;

    struct Parameters {
        double smoothness = 0.68;
        double centerStiffness = 150.0;     // mass=1; omega=sqrt(stiffness)
        double zoomStiffness = 105.0;
        double centerDampingRatio = 1.0;    // no positional overshoot
        double zoomDampingRatio = 0.86;     // ~0.5% scale overshoot
        double maxCenterVelocity = 1.35;    // normalized frame units / second
        double maxZoomVelocity = 2.0;       // log-size units / second
        int stepMs = 5;
        int checkpointMs = 500;
    };

    SpringCameraEvaluator();
    explicit SpringCameraEvaluator(const QVector<Keyframe> &keyframes,
                                   double smoothness = 0.68);

    void setTimeline(const QVector<Keyframe> &keyframes, double smoothness = 0.68);
    void setTimeline(const QVector<Keyframe> &keyframes, const Parameters &parameters);
    QRectF evaluate(qint64 tMs);
    QRectF targetAt(qint64 tMs) const;

    int cachedCheckpointCount() const { return m_checkpoints.size(); }
    Parameters parameters() const { return m_parameters; }
    static Parameters parametersForSmoothness(double smoothness);

private:
    struct TargetEvent {
        qint64 atUs = 0;
        QRectF rect;
    };
    struct State {
        qint64 timeUs = 0;
        QPointF center;
        QPointF centerVelocity;
        QPointF logSize;
        QPointF logSizeVelocity;
        QRectF target;
        int nextEvent = 0;
    };

    static QRectF boundedRect(const QRectF &rect);
    static QRectF rectForState(State *state);
    void advance(State *state, qint64 destinationUs) const;
    void integrate(State *state, double dtSeconds) const;
    void applyEvents(State *state) const;
    State stateForCanonicalTime(qint64 canonicalUs);

    Parameters m_parameters;
    QVector<TargetEvent> m_events;
    QVector<State> m_checkpoints;
    State m_initial;
    State m_forward;
    bool m_hasTimeline = false;
};
