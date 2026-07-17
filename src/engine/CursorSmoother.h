#pragma once
#include "CursorTrack.h"

#include <QVector>

// One-euro filter over a CursorTrack. Raw PipeWire cursor samples are integer
// stream pixels and jitter by a pixel or two at rest; feeding that straight into
// the zoom/pan camera makes it twitch. The one-euro filter (Casiez, Roussel,
// Vogel 2012) is the right tool: a low-pass whose cutoff RISES with pointer
// speed, so a slow hover is smoothed hard while a fast deliberate move is
// tracked closely with little lag. Pure, deterministic, non-mutating — hands
// back a fresh sample vector, never touches the input track.
//
// Timestamps are irregular (samples arrive per-frame, and pause-excision closes
// gaps), so dt is taken from each sample delta rather than assumed fixed; a
// non-positive dt (equal or regressed timestamps — CursorTrack allows equal
// tMs) is guarded by reusing the previous filtered value.
class CursorSmoother
{
public:
    struct Params {
        // Baseline cutoff in Hz at zero speed: lower == smoother/laggier at
        // rest. 1.0 Hz kills resting jitter without visible lag on a hover.
        double minCutoff = 1.0;
        // Speed coupling. cutoff = minCutoff + beta*|speed|, speed in px/s, so
        // beta carries units of Hz per (px/s). 0.007 was tuned on the synthetic
        // paths in CursorSmootherTest: at a brisk ~800 px/s move it lifts the
        // cutoff to ~6.6 Hz (tracks the intent), while sub-10 px/s resting
        // jitter stays near the 1 Hz floor (damped hard). Raise it if fast
        // moves feel laggy, lower it if fast moves feel jittery.
        double beta = 0.007;
        // Cutoff of the derivative pre-filter, Hz. 1.0 keeps the speed estimate
        // itself from chattering. Rarely needs tuning.
        double dCutoff = 1.0;
    };

    CursorSmoother() = default;
    explicit CursorSmoother(const Params &p) : m_p(p) {}

    // Smoothed copy of the track's samples: tMs / visible / shapeId are carried
    // through verbatim; only x/y are filtered. First sample is emitted
    // unchanged (the filter seeds on it), so the endpoints are preserved.
    QVector<CursorSample> smooth(const CursorTrack &track) const;

private:
    Params m_p;
};
