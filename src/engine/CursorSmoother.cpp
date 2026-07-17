#include "CursorSmoother.h"

#include <cmath>

namespace {

// Smoothing factor for a first-order low-pass at cutoff Hz over a step of dt
// seconds: alpha = 1 / (1 + tau/dt), tau = 1/(2*pi*cutoff). Larger alpha ==
// less smoothing (follows the input); smaller == heavier smoothing.
double lowpassAlpha(double cutoffHz, double dt)
{
    const double tau = 1.0 / (2.0 * M_PI * cutoffHz);
    return 1.0 / (1.0 + tau / dt);
}

// Exponential low-pass that remembers its last output. One instance per axis
// for the signal, one per axis for the derivative.
struct LowPass {
    double prev = 0.0;
    bool seeded = false;

    double filter(double x, double alpha)
    {
        if (!seeded) {
            prev = x;
            seeded = true;
            return x;
        }
        prev = alpha * x + (1.0 - alpha) * prev;
        return prev;
    }
};

// One-euro state for a single scalar channel (x or y tracked independently).
struct OneEuro {
    double minCutoff, beta, dCutoff;
    LowPass value;   // the signal filter
    LowPass deriv;   // the speed pre-filter
    double prevRaw = 0.0;
    bool have = false;

    double filter(double x, double dt)
    {
        if (!have) {
            have = true;
            prevRaw = x;
            value.filter(x, 1.0);   // seed: first output == first input
            return x;
        }
        // Speed estimate from the raw signal, then pre-filtered so the
        // adaptive cutoff itself doesn't chatter.
        const double speed = (x - prevRaw) / dt;
        prevRaw = x;
        const double edSpeed = deriv.filter(speed, lowpassAlpha(dCutoff, dt));
        const double cutoff = minCutoff + beta * std::fabs(edSpeed);
        return value.filter(x, lowpassAlpha(cutoff, dt));
    }
};

} // namespace

QVector<CursorSample> CursorSmoother::smooth(const CursorTrack &track) const
{
    const QList<CursorSample> &in = track.samples();
    QVector<CursorSample> out;
    out.reserve(in.size());
    if (in.isEmpty())
        return out;

    OneEuro fx{m_p.minCutoff, m_p.beta, m_p.dCutoff, {}, {}, 0.0, false};
    OneEuro fy{m_p.minCutoff, m_p.beta, m_p.dCutoff, {}, {}, 0.0, false};

    qint64 prevT = in.first().tMs;
    double lastX = in.first().x;
    double lastY = in.first().y;

    for (int i = 0; i < in.size(); ++i) {
        CursorSample s = in.at(i);
        const double dt = (s.tMs - prevT) / 1000.0;
        if (i == 0) {
            lastX = fx.filter(s.x, 1.0);
            lastY = fy.filter(s.y, 1.0);
        } else if (dt <= 0.0) {
            // Equal/regressed timestamp (CursorTrack permits equal tMs): the
            // filter step is undefined for dt<=0, so hold the previous filtered
            // position rather than divide by zero.
            s.x = lastX;
            s.y = lastY;
            out.append(s);
            continue;
        } else {
            lastX = fx.filter(s.x, dt);
            lastY = fy.filter(s.y, dt);
        }
        prevT = s.tMs;
        s.x = lastX;
        s.y = lastY;
        out.append(s);
    }
    return out;
}
