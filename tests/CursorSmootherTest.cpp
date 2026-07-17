#include "CursorSmoother.h"
#include "CursorTrack.h"

#include <QtGlobal>
#include <QTest>
#include <QVector>

#include <cmath>

// Deterministic jitter: no RNG (the engine must stay reproducible). A fixed
// alternating/sinusoidal wobble stands in for pixel-level capture noise.
static double jitter(int i)
{
    return 2.0 * std::sin(i * 1.7) + 1.0 * ((i % 2) ? 1.0 : -1.0);
}

// Variance of the first differences (discrete velocity) — a roughness measure
// that jitter inflates and smoothing reduces, insensitive to constant lag.
static double velVariance(const QVector<double> &v)
{
    if (v.size() < 2)
        return 0.0;
    QVector<double> d;
    d.reserve(v.size() - 1);
    for (int i = 1; i < v.size(); ++i)
        d.append(v.at(i) - v.at(i - 1));
    double mean = 0;
    for (double x : d) mean += x;
    mean /= d.size();
    double var = 0;
    for (double x : d) var += (x - mean) * (x - mean);
    return var / d.size();
}

class CursorSmootherTest : public QObject
{
    Q_OBJECT

private slots:
    void reducesJitterVariance();
    void preservesEndpoints();
    void preservesMonotonicTimestamps();
    void guardsNonPositiveDt();
};

// A jittery straight-line drag that then settles (as a real pointer does when
// it reaches its target), plus wobble throughout. The settle lets the filter
// converge to the final position so the endpoint is preserved despite the lag a
// low-pass necessarily has on a moving ramp.
static CursorTrack jitteryDrag(int nMove = 200, int stepMs = 16)
{
    CursorTrack t;
    double fx = 300.0, fy = 400.0;
    for (int i = 0; i < nMove; ++i) {
        fx = 300.0 + i * 4.0;
        fy = 400.0 + i * 1.5;
        CursorSample s;
        s.tMs = qint64(i) * stepMs;
        s.x = fx + jitter(i);
        s.y = fy + jitter(i + 7);
        t.append(s);
    }
    for (int i = 0; i < 40; ++i) {          // ~640 ms settle at the final point
        CursorSample s;
        s.tMs = qint64(nMove + i) * stepMs;
        s.x = fx + jitter(nMove + i);
        s.y = fy + jitter(nMove + i + 7);
        t.append(s);
    }
    return t;
}

void CursorSmootherTest::reducesJitterVariance()
{
    const CursorTrack in = jitteryDrag();
    const QVector<CursorSample> out = CursorSmoother().smooth(in);
    QCOMPARE(out.size(), in.count());

    QVector<double> rawX, smoothX;
    for (const CursorSample &s : in.samples()) rawX.append(s.x);
    for (const CursorSample &s : out) smoothX.append(s.x);

    // Smoothing must strictly reduce the high-frequency roughness.
    QVERIFY(velVariance(smoothX) < velVariance(rawX));
}

void CursorSmootherTest::preservesEndpoints()
{
    const CursorTrack in = jitteryDrag();
    const QVector<CursorSample> out = CursorSmoother().smooth(in);

    // First sample is the filter seed — emitted verbatim.
    QCOMPARE(out.first().x, in.samples().first().x);
    QCOMPARE(out.first().y, in.samples().first().y);

    // At steady state the filter tracks; the last smoothed point differs from
    // the last raw point by at most the jitter amplitude (~3 px here).
    QVERIFY(std::fabs(out.last().x - in.samples().last().x) < 5.0);
    QVERIFY(std::fabs(out.last().y - in.samples().last().y) < 5.0);
}

void CursorSmootherTest::preservesMonotonicTimestamps()
{
    const CursorTrack in = jitteryDrag();
    const QVector<CursorSample> out = CursorSmoother().smooth(in);
    QCOMPARE(out.size(), in.count());
    for (int i = 0; i < out.size(); ++i) {
        QCOMPARE(out.at(i).tMs, in.samples().at(i).tMs);   // carried through
        if (i > 0)
            QVERIFY(out.at(i).tMs >= out.at(i - 1).tMs);   // non-decreasing
    }
}

void CursorSmootherTest::guardsNonPositiveDt()
{
    CursorTrack t;
    // Two samples share an instant (CursorTrack permits equal tMs) — dt == 0
    // for the second; the filter must not divide by zero or emit NaN.
    CursorSample a; a.tMs = 0;   a.x = 100; a.y = 100; t.append(a);
    CursorSample b; b.tMs = 16;  b.x = 140; b.y = 120; t.append(b);
    CursorSample c; c.tMs = 16;  c.x = 900; c.y = 900; t.append(c);   // dt == 0
    CursorSample d; d.tMs = 32;  d.x = 180; d.y = 160; t.append(d);

    const QVector<CursorSample> out = CursorSmoother().smooth(t);
    QCOMPARE(out.size(), 4);
    for (const CursorSample &s : out) {
        QVERIFY(std::isfinite(s.x));
        QVERIFY(std::isfinite(s.y));
    }
    // The dt==0 sample holds the previous filtered value rather than spiking to
    // its own wild (900,900) coordinates.
    QCOMPARE(out.at(2).x, out.at(1).x);
    QCOMPARE(out.at(2).y, out.at(1).y);
}

QTEST_GUILESS_MAIN(CursorSmootherTest)
#include "CursorSmootherTest.moc"
