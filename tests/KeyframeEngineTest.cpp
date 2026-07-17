#include "KeyframeEngine.h"

#include "ClickTrack.h"
#include "CursorTrack.h"

#include <QRectF>
#include <QSize>
#include <QTest>
#include <QVector>

#include <cmath>

using Keyframe = KeyframeEngine::Keyframe;

// --- fixtures --------------------------------------------------------------

static const QSize kVideo(1920, 1080);
static const double kW = 1920.0, kH = 1080.0;
static const double kOutAspect = 16.0 / 9.0;   // matches the source aspect

// Append a straight cursor move (linear x/y) between two instants.
static void moveLine(CursorTrack &t, double x0, double y0, double x1, double y1,
                     qint64 t0, qint64 t1, int stepMs = 16)
{
    const int n = int((t1 - t0) / stepMs);
    for (int i = 0; i <= n; ++i) {
        const double f = n > 0 ? double(i) / n : 0.0;
        CursorSample s;
        s.tMs = t0 + qint64(i) * stepMs;
        s.x = x0 + (x1 - x0) * f;
        s.y = y0 + (y1 - y0) * f;
        t.append(s);
    }
}

// Hold the cursor still at a point across an interval (a dwell).
static void hold(CursorTrack &t, double x, double y, qint64 t0, qint64 t1, int stepMs = 16)
{
    for (qint64 tt = t0; tt <= t1; tt += stepMs) {
        CursorSample s; s.tMs = tt; s.x = x; s.y = y;
        t.append(s);
    }
}

// A click == a Down then a matching Up 30 ms later.
static void click(ClickTrack &c, qint64 t, double x, double y)
{
    ClickEvent d; d.tMs = t;      d.button = 1; d.state = ClickEvent::Down; d.x = x; d.y = y;
    ClickEvent u; u.tMs = t + 30; u.button = 1; u.state = ClickEvent::Up;   u.x = x; u.y = y;
    c.append(d);
    c.append(u);
}

static bool isFull(const QRectF &r)
{
    return std::fabs(r.x()) < 1e-6 && std::fabs(r.y()) < 1e-6
        && std::fabs(r.width() - 1.0) < 1e-6 && std::fabs(r.height() - 1.0) < 1e-6;
}

// --- test ------------------------------------------------------------------

class KeyframeEngineTest : public QObject
{
    Q_OBJECT

private:
    // Shared structural guarantees every generated timeline must satisfy.
    void checkInvariants(const QVector<Keyframe> &kfs, qint64 duration, double outAspect);

private slots:
    void singleClusterOneSpan();
    void twoDistantClustersFullBetween();
    void twoNearClustersMerge();
    void idleTailReturnsToFull();
    void noClicksDwellFallback();
    void pinnedRoutesAround();
    void nonMatchingAspectRects();
    void fillModeBaseRectAspect();
    void evaluateContinuity();
    void determinism();
    void paramsJsonRoundtrip();
};

void KeyframeEngineTest::checkInvariants(const QVector<Keyframe> &kfs, qint64 duration,
                                         double outAspect)
{
    QVERIFY(!kfs.isEmpty());

    // Strictly sorted by tMs.
    for (int i = 1; i < kfs.size(); ++i)
        QVERIFY(kfs.at(i).tMs > kfs.at(i - 1).tMs);

    // First keyframe: t == 0, full frame.
    QCOMPARE(kfs.first().tMs, qint64(0));
    QVERIFY(isFull(kfs.first().rect));

    // Last keyframe: full frame, within the video.
    QVERIFY(isFull(kfs.last().rect));
    QVERIFY(kfs.last().tMs <= duration);

    for (const Keyframe &k : kfs) {
        // Every rect within [0,1].
        QVERIFY(k.rect.x() >= -1e-9);
        QVERIFY(k.rect.y() >= -1e-9);
        QVERIFY(k.rect.right() <= 1.0 + 1e-9);
        QVERIFY(k.rect.bottom() <= 1.0 + 1e-9);
        QVERIFY(k.source == ZoomTimeline::Auto);
        // Zoomed rects carry the OUTPUT aspect exactly (full-frame rects keep
        // the source aspect, so they are exempt from this check).
        if (!isFull(k.rect)) {
            const double ratio = (k.rect.width() * kW) / (k.rect.height() * kH);
            QVERIFY(std::fabs(ratio - outAspect) / outAspect < 1e-6);
        }
    }
}

static int fullReturns(const QVector<Keyframe> &kfs)
{
    int n = 0;
    for (const Keyframe &k : kfs)
        if (k.tMs > 0 && isFull(k.rect))
            ++n;
    return n;
}

void KeyframeEngineTest::singleClusterOneSpan()
{
    CursorTrack cur;
    moveLine(cur, 200, 300, 1600, 800, 0, 8000);
    ClickTrack clk;
    click(clk, 3000, 900, 500);
    click(clk, 3300, 920, 510);
    click(clk, 3600, 910, 505);

    const qint64 dur = 8000;
    const auto kfs = KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", {});
    checkInvariants(kfs, dur, kOutAspect);

    // Exactly one zoom span: one zoom-out to full (besides the t==0 opener).
    QCOMPARE(fullReturns(kfs), 1);

    // In/out easing durations present.
    bool haveZoomIn = false, haveZoomOut = false;
    for (const Keyframe &k : kfs) {
        if (!isFull(k.rect) && k.easeInMs == 650) haveZoomIn = true;
        if (isFull(k.rect) && k.tMs > 0 && k.easeInMs == 900) haveZoomOut = true;
    }
    QVERIFY(haveZoomIn);
    QVERIFY(haveZoomOut);

    // Every click is inside the zoomed span.
    for (qint64 t : {qint64(3000), qint64(3300), qint64(3600)})
        QVERIFY(!isFull(KeyframeEngine::evaluate(kfs, t)));
}

void KeyframeEngineTest::twoDistantClustersFullBetween()
{
    CursorTrack cur;
    moveLine(cur, 300, 300, 1500, 800, 0, 16000);
    ClickTrack clk;
    click(clk, 3000, 500, 400);
    click(clk, 3300, 520, 410);
    click(clk, 3600, 510, 405);
    click(clk, 11000, 1400, 700);
    click(clk, 11300, 1420, 710);
    click(clk, 11600, 1410, 705);

    const qint64 dur = 16000;
    const auto kfs = KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", {});
    checkInvariants(kfs, dur, kOutAspect);

    QCOMPARE(fullReturns(kfs), 2);                                  // two separate spans
    QVERIFY(isFull(KeyframeEngine::evaluate(kfs, 8000)));           // full between them
    QVERIFY(!isFull(KeyframeEngine::evaluate(kfs, 3300)));
    QVERIFY(!isFull(KeyframeEngine::evaluate(kfs, 11300)));
}

void KeyframeEngineTest::twoNearClustersMerge()
{
    CursorTrack cur;
    moveLine(cur, 400, 350, 1500, 750, 0, 9000);
    ClickTrack clk;
    click(clk, 3000, 500, 400);      // cluster A (far from B spatially -> two clusters)
    click(clk, 4000, 1400, 700);     // cluster B (close in time -> spans merge)

    const qint64 dur = 9000;
    const auto kfs = KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", {});
    checkInvariants(kfs, dur, kOutAspect);

    QCOMPARE(fullReturns(kfs), 1);                                  // retargeted, not out+in
    QVERIFY(!isFull(KeyframeEngine::evaluate(kfs, 3500)));          // stays zoomed between
}

void KeyframeEngineTest::idleTailReturnsToFull()
{
    CursorTrack cur;
    moveLine(cur, 200, 300, 900, 500, 0, 3000);
    hold(cur, 900, 500, 3016, 10000);          // idle after the click, no more clicks
    ClickTrack clk;
    click(clk, 3000, 900, 500);

    const qint64 dur = 10000;
    const auto kfs = KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", {});
    checkInvariants(kfs, dur, kOutAspect);

    QVERIFY(!isFull(KeyframeEngine::evaluate(kfs, 3000)));          // zoomed on the click
    QVERIFY(isFull(KeyframeEngine::evaluate(kfs, 9000)));           // back to full on idle
}

void KeyframeEngineTest::noClicksDwellFallback()
{
    CursorTrack cur;
    moveLine(cur, 200, 200, 960, 540, 0, 2000);
    hold(cur, 960, 540, 2016, 5000);           // a 3 s dwell
    moveLine(cur, 960, 540, 1600, 900, 5016, 8000);
    ClickTrack clk;                            // no clicks at all

    const qint64 dur = 8000;
    const auto kfs = KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", {});
    checkInvariants(kfs, dur, kOutAspect);

    // The dwell fallback produced a zoom.
    bool anyZoom = false;
    for (const Keyframe &k : kfs)
        if (!isFull(k.rect)) anyZoom = true;
    QVERIFY(anyZoom);
    QVERIFY(!isFull(KeyframeEngine::evaluate(kfs, 3500)));          // zoomed during the dwell
}

void KeyframeEngineTest::pinnedRoutesAround()
{
    CursorTrack cur;
    moveLine(cur, 200, 300, 1600, 800, 0, 12000);
    ClickTrack clk;
    click(clk, 3000, 900, 500);
    click(clk, 3300, 920, 510);

    // A user keyframe pinned in the gap after the span.
    Keyframe pin;
    pin.tMs = 7000; pin.rect = QRectF(0.2, 0.2, 0.5, 0.5);
    pin.easeInMs = 300; pin.easeOutMs = 300; pin.source = ZoomTimeline::Manual;

    const qint64 dur = 12000;
    const auto kfs = KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", {}, {pin});
    checkInvariants(kfs, dur, kOutAspect);

    // No auto keyframe lands inside the pinned window [6700, 7300].
    for (const Keyframe &k : kfs)
        QVERIFY(k.tMs <= 6700 || k.tMs >= 7300);

    QVERIFY(!isFull(KeyframeEngine::evaluate(kfs, 3000)));          // cluster still covered
}

void KeyframeEngineTest::nonMatchingAspectRects()
{
    CursorTrack cur;
    moveLine(cur, 200, 300, 1600, 800, 0, 8000);
    ClickTrack clk;
    click(clk, 3000, 900, 500);
    click(clk, 3300, 920, 510);

    const qint64 dur = 8000;
    // Square output out of a 16:9 source: zoom rects must carry a 1:1 pixel
    // aspect even though the full-frame rects stay 16:9.
    const auto kfs = KeyframeEngine::generate(cur, clk, kVideo, dur, "1:1", {});
    checkInvariants(kfs, dur, 1.0);
}

void KeyframeEngineTest::fillModeBaseRectAspect()
{
    CursorTrack cur;
    moveLine(cur, 200, 300, 1600, 800, 0, 8000);
    ClickTrack clk;
    click(clk, 3000, 900, 500);
    click(clk, 3300, 920, 510);
    click(clk, 3600, 910, 505);

    const qint64 dur = 8000;
    // 9:16 output out of a 16:9 source, fill mode: the base (non-zoomed) camera is
    // the largest centred output-aspect crop, NOT the whole frame — so EVERY rect
    // (base and zoomed alike) carries the output pixel aspect and there is no
    // letterboxed full-frame keyframe anywhere.
    KeyframeEngine::Params p;
    p.fill = true;
    const double outAspect = 9.0 / 16.0;
    const auto kfs = KeyframeEngine::generate(cur, clk, kVideo, dur, "9:16", p);

    QVERIFY(!kfs.isEmpty());
    // Strictly sorted.
    for (int i = 1; i < kfs.size(); ++i)
        QVERIFY(kfs.at(i).tMs > kfs.at(i - 1).tMs);
    // First keyframe: t == 0 and an output-aspect crop (NOT the full frame).
    QCOMPARE(kfs.first().tMs, qint64(0));
    QVERIFY(!isFull(kfs.first().rect));
    // No keyframe is ever the full frame; every rect carries the output aspect and
    // stays inside [0,1].
    for (const Keyframe &k : kfs) {
        QVERIFY(!isFull(k.rect));
        QVERIFY(k.rect.x() >= -1e-9);
        QVERIFY(k.rect.y() >= -1e-9);
        QVERIFY(k.rect.right() <= 1.0 + 1e-9);
        QVERIFY(k.rect.bottom() <= 1.0 + 1e-9);
        const double ratio = (k.rect.width() * kW) / (k.rect.height() * kH);
        QVERIFY(std::fabs(ratio - outAspect) / outAspect < 1e-6);
    }
    // The cluster is still zoomed in (tighter than the base crop).
    const QRectF base = KeyframeEngine::evaluate(kfs, 0);
    const QRectF atCluster = KeyframeEngine::evaluate(kfs, 3300);
    QVERIFY(atCluster.width() < base.width() - 1e-6);

    // Determinism holds in fill mode too.
    const auto kfs2 = KeyframeEngine::generate(cur, clk, kVideo, dur, "9:16", p);
    QCOMPARE(kfs.size(), kfs2.size());
    for (int i = 0; i < kfs.size(); ++i) {
        QCOMPARE(kfs.at(i).tMs, kfs2.at(i).tMs);
        QCOMPARE(kfs.at(i).rect, kfs2.at(i).rect);
    }
}

void KeyframeEngineTest::evaluateContinuity()
{
    CursorTrack cur;
    moveLine(cur, 700, 400, 900, 500, 0, 3000);
    moveLine(cur, 900, 500, 1100, 600, 3016, 6000);            // slow pan through the hold
    moveLine(cur, 1100, 600, 1150, 620, 6016, 12000);
    ClickTrack clk;
    // A wide, centred, chained cluster (each click within the spread limit of
    // the previous). The wide vertical spread pins the zoom to the gentle floor
    // (zoomMin) and centring keeps the rect edges near the frame edges, so the
    // eased transitions never jump more than 2% of the frame per 16 ms — even at
    // the bezier's steepest point.
    click(clk, 3000, 760, 340);
    click(clk, 3250, 960, 490);
    click(clk, 3500, 1160, 610);
    click(clk, 3750, 1020, 710);
    click(clk, 4000, 820, 690);

    const qint64 dur = 12000;
    const auto kfs = KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", {});
    checkInvariants(kfs, dur, kOutAspect);

    // Sampled continuity: no rect edge jumps more than 2% of the frame between
    // consecutive 16 ms samples.
    QRectF prev = KeyframeEngine::evaluate(kfs, 0);
    for (qint64 t = 16; t <= dur; t += 16) {
        const QRectF r = KeyframeEngine::evaluate(kfs, t);
        QVERIFY(std::fabs(r.x() - prev.x()) < 0.02);
        QVERIFY(std::fabs(r.y() - prev.y()) < 0.02);
        QVERIFY(std::fabs(r.right() - prev.right()) < 0.02);
        QVERIFY(std::fabs(r.bottom() - prev.bottom()) < 0.02);
        prev = r;
    }

    // Exact match at every keyframe instant.
    for (const Keyframe &k : kfs) {
        const QRectF r = KeyframeEngine::evaluate(kfs, k.tMs);
        QVERIFY(std::fabs(r.x() - k.rect.x()) < 1e-9);
        QVERIFY(std::fabs(r.y() - k.rect.y()) < 1e-9);
        QVERIFY(std::fabs(r.width() - k.rect.width()) < 1e-9);
        QVERIFY(std::fabs(r.height() - k.rect.height()) < 1e-9);
    }
}

void KeyframeEngineTest::determinism()
{
    CursorTrack cur;
    moveLine(cur, 200, 300, 1600, 800, 0, 10000);
    ClickTrack clk;
    click(clk, 3000, 900, 500);
    click(clk, 3300, 920, 510);
    click(clk, 6000, 400, 700);

    const qint64 dur = 10000;
    const auto a = KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", {});
    const auto b = KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", {});
    QCOMPARE(a.size(), b.size());
    for (int i = 0; i < a.size(); ++i) {
        QCOMPARE(a.at(i).tMs, b.at(i).tMs);
        QCOMPARE(a.at(i).rect, b.at(i).rect);
        QCOMPARE(a.at(i).easeInMs, b.at(i).easeInMs);
        QCOMPARE(a.at(i).easeOutMs, b.at(i).easeOutMs);
        QCOMPARE(int(a.at(i).source), int(b.at(i).source));
    }
}

void KeyframeEngineTest::paramsJsonRoundtrip()
{
    KeyframeEngine::Params p;
    p.zoomMax = 3.1;
    p.leadInMs = 777;
    p.deadZoneFrac = 0.42;
    p.idleSpeedFracPerSec = 0.033;

    const KeyframeEngine::Params r = KeyframeEngine::Params::fromJson(p.toJson());
    QCOMPARE(r.clickClusterGapMs, p.clickClusterGapMs);
    QCOMPARE(r.clickClusterDistFrac, p.clickClusterDistFrac);
    QCOMPARE(r.leadInMs, p.leadInMs);
    QCOMPARE(r.tailMs, p.tailMs);
    QCOMPARE(r.zoomInMs, p.zoomInMs);
    QCOMPARE(r.zoomOutMs, p.zoomOutMs);
    QCOMPARE(r.minHoldMs, p.minHoldMs);
    QCOMPARE(r.idleAfterMs, p.idleAfterMs);
    QCOMPARE(r.idleSpeedFracPerSec, p.idleSpeedFracPerSec);
    QCOMPARE(r.zoomMin, p.zoomMin);
    QCOMPARE(r.zoomMax, p.zoomMax);
    QCOMPARE(r.marginFrac, p.marginFrac);
    QCOMPARE(r.deadZoneFrac, p.deadZoneFrac);
    QCOMPARE(r.dwellMinMs, p.dwellMinMs);
    QCOMPARE(r.dwellSpeedFracPerSec, p.dwellSpeedFracPerSec);
}

QTEST_GUILESS_MAIN(KeyframeEngineTest)
#include "KeyframeEngineTest.moc"
