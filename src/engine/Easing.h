#pragma once
#include <cmath>

// Cubic-bezier easing, header-only, no Qt dependency (pure double math). The
// curve is the CSS/Screen-Studio timing function: two control points p1, p2 in
// the unit square with the endpoints pinned at (0,0) and (1,1). evaluate(t)
// takes an INPUT progress fraction t in [0,1] (elapsed / duration) and returns
// the eased OUTPUT fraction y.
//
// The catch is that the bezier is parameterised by its own curve parameter u,
// not by x directly: x(u) and y(u) are both cubics in u. So evaluate() first
// solves x(u) == t for u (Newton-Raphson, with a bisection fallback when the
// derivative is near zero or Newton steps out of range), then returns y(u).
// Deterministic and allocation-free — the engine and the renderer must agree
// bit-for-bit, so there are no clocks, no randomness, no lookup tables.
namespace Easing {

// One coordinate of the cubic bezier with endpoints 0 and 1: control values c1,
// c2. B(u) = 3(1-u)^2 u c1 + 3(1-u) u^2 c2 + u^3.
inline double bezierAxis(double c1, double c2, double u)
{
    const double v = 1.0 - u;
    return 3.0 * v * v * u * c1 + 3.0 * v * u * u * c2 + u * u * u;
}

// Derivative dB/du of the axis cubic — used by the Newton solver.
inline double bezierAxisDeriv(double c1, double c2, double u)
{
    const double v = 1.0 - u;
    return 3.0 * v * v * c1 + 6.0 * v * u * (c2 - c1) + 3.0 * u * u * (1.0 - c2);
}

// Solve x(u) == t for u, then return y(u). p1x/p1y/p2x/p2y are the two control
// points; t is the input fraction. Endpoints and out-of-range inputs are
// clamped so callers never have to guard.
inline double evaluate(double p1x, double p1y, double p2x, double p2y, double t)
{
    if (t <= 0.0)
        return 0.0;
    if (t >= 1.0)
        return 1.0;

    // Newton-Raphson from a sensible seed (u == t is a good start for
    // near-linear curves). Cap iterations; the curve is monotonic in x for the
    // control points we use, so a handful of steps converge tightly.
    double u = t;
    for (int i = 0; i < 8; ++i) {
        const double x = bezierAxis(p1x, p2x, u) - t;
        if (std::fabs(x) < 1e-7)
            return bezierAxis(p1y, p2y, u);
        const double dx = bezierAxisDeriv(p1x, p2x, u);
        if (std::fabs(dx) < 1e-7)
            break;                 // flat — hand off to bisection
        u -= x / dx;
    }

    // Bisection fallback: guaranteed to converge because x(u) is monotonic on
    // [0,1] for these control points.
    double lo = 0.0, hi = 1.0;
    u = t;
    for (int i = 0; i < 32; ++i) {
        const double x = bezierAxis(p1x, p2x, u);
        if (std::fabs(x - t) < 1e-7)
            break;
        if (x < t)
            lo = u;
        else
            hi = u;
        u = 0.5 * (lo + hi);
    }
    return bezierAxis(p1y, p2y, u);
}

// The house easing preset — Material "standard" (0.4, 0, 0.2, 1): a brisk start
// that settles softly. Every auto keyframe transition uses this so preview and
// export look identical. evaluate() with these four constants baked in.
inline double preset(double t)
{
    return evaluate(0.4, 0.0, 0.2, 1.0, t);
}

} // namespace Easing
