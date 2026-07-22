#include "KeyframeEngine.h"

#include "ClickTrack.h"
#include "CursorTrack.h"

#include <QRectF>
#include <QSize>
#include <QTest>
#include <QVector>

#include <cmath>
#include <limits>

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

static QRectF evaluate(const QVector<Keyframe> &keyframes, qint64 tMs,
                       double smoothness = ZoomTimeline::DefaultMotionSmoothness)
{
    SpringCameraEvaluator evaluator(keyframes, smoothness);
    return evaluator.evaluate(tMs);
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
    void typingBurstHoldsZoom();
    void driveByClickDoesNotZoom();
    void loneClickZoomsSofterThanCluster();
    void trivialMotionDoesNotZoom();
    void invalidClicksFallBackToDwell();
    void postDurationDwellDoesNotZoom();
    void generatedTimesStayWithinDuration();
    void nonPositiveDurationIsSafe();
    void intensityScalesZoomAndFrequency();
    void clickTargetKeepsActivityOffEdges();
    void pinnedRoutesAround();
    void nonMatchingAspectRects();
    void aspectReprojectionRoundtrip();
    void fillModeBaseRectAspect();
    void evaluateContinuity();
    void springBoundsSettlingAndHold();
    void springDeterministicAcrossSeekOrder();
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

    // In/out target lead durations present.
    const KeyframeEngine::Params defaults;
    bool haveZoomIn = false, haveZoomOut = false;
    for (const Keyframe &k : kfs) {
        if (!isFull(k.rect) && k.easeInMs == defaults.zoomInMs) haveZoomIn = true;
        if (isFull(k.rect) && k.tMs > 0 && k.easeInMs == defaults.zoomOutMs) haveZoomOut = true;
    }
    QVERIFY(haveZoomIn);
    QVERIFY(haveZoomOut);

    // Every click is inside the zoomed span.
    for (qint64 t : {qint64(3000), qint64(3300), qint64(3600)})
        QVERIFY(!isFull(evaluate(kfs, t)));
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
    QVERIFY(isFull(evaluate(kfs, 8000)));                           // full between them
    QVERIFY(!isFull(evaluate(kfs, 3300)));
    QVERIFY(!isFull(evaluate(kfs, 11300)));
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
    QVERIFY(!isFull(evaluate(kfs, 3500)));                          // stays zoomed between
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

    QVERIFY(!isFull(evaluate(kfs, 3000)));                          // zoomed on the click
    QVERIFY(isFull(evaluate(kfs, 9000)));                           // back to full on idle
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
    QVERIFY(!isFull(evaluate(kfs, 3500)));                          // zoomed during the dwell
}

void KeyframeEngineTest::trivialMotionDoesNotZoom()
{
    CursorTrack cur;
    // Tiny sub-threshold drift with no approach/leave evidence must not create a
    // camera event merely because the pointer happened to sit still.
    for (qint64 t = 0; t <= 8000; t += 16) {
        CursorSample sample;
        sample.tMs = t;
        sample.x = 960.0 + std::sin(t / 200.0) * 0.4;
        sample.y = 540.0 + std::cos(t / 170.0) * 0.4;
        cur.append(sample);
    }
    const auto kfs = KeyframeEngine::generate(cur, ClickTrack{}, kVideo, 8000, "16:9", {});
    QCOMPARE(kfs.size(), 1);
    QVERIFY(isFull(kfs.first().rect));
}

// A typing burst is dwell evidence: the camera must hold its zoom through the
// burst instead of retracting right after the click that opened the field.
void KeyframeEngineTest::typingBurstHoldsZoom()
{
    CursorTrack cur;
    moveLine(cur, 200, 200, 960, 540, 0, 800);
    hold(cur, 960, 540, 816, 12000);       // parked in the field being typed into
    ClickTrack clk;
    click(clk, 900, 960, 540);             // click into the field

    KeyframeEngine::Params params;         // defaults
    const qint64 dur = 13000;

    const auto without =
        KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", params, {}, {});
    const auto withTyping =
        KeyframeEngine::generate(cur, clk, kVideo, dur, "16:9", params, {}, {{2000, 9000}});

    auto lastTime = [](const QVector<Keyframe> &kfs) {
        qint64 mx = 0;
        for (const auto &k : kfs)
            mx = std::max(mx, k.tMs);
        return mx;
    };
    QVERIFY(without.size() >= 2);
    QVERIFY(withTyping.size() >= 2);
    // The lone click zooms back out early; the burst keeps the camera engaged
    // well past it (holding through the whole [2000,9000] typing run).
    QVERIFY(lastTime(withTyping) > lastTime(without) + 2500);
    checkInvariants(withTyping, dur, kOutAspect);
}

// A lone click while the cursor is traveling through (no dwell after it) is not
// zoom-worthy — the drive-by filter must drop it rather than produce a random
// zoom in the middle of a mouse move.
void KeyframeEngineTest::driveByClickDoesNotZoom()
{
    CursorTrack cur;
    // Fast zig-zag travel (each leg ~a full frame crossing in <1s) — never
    // settles anywhere, so neither click clusters nor dwell runs form.
    moveLine(cur, 100, 150, 1820, 930, 0, 900);
    moveLine(cur, 1820, 930, 120, 880, 916, 1800);
    moveLine(cur, 120, 880, 1800, 160, 1816, 2700);
    moveLine(cur, 1800, 160, 150, 900, 2716, 3600);
    moveLine(cur, 150, 900, 1820, 500, 3616, 4500);
    moveLine(cur, 1820, 500, 130, 300, 4516, 5400);
    moveLine(cur, 130, 300, 1750, 950, 5416, 6400);
    ClickTrack clk;
    click(clk, 3100, 960, 540);       // one click mid-travel, cursor keeps going

    const auto kfs = KeyframeEngine::generate(cur, clk, kVideo, 6400, "16:9", {});
    checkInvariants(kfs, 6400, kOutAspect);
    for (const auto &k : kfs)
        QVERIFY2(isFull(k.rect), "drive-by click must not produce a zoom");
}

// A lone click the user stays on zooms WIDER than a dense multi-click cluster at
// the same spot — repetition earns the tight framing.
void KeyframeEngineTest::loneClickZoomsSofterThanCluster()
{
    auto minWidth = [](const QVector<Keyframe> &kfs) {
        double w = 1.0;
        for (const auto &k : kfs)
            w = std::min(w, k.rect.width());
        return w;
    };

    CursorTrack cur;
    moveLine(cur, 200, 200, 960, 540, 0, 1800);
    hold(cur, 960, 540, 1816, 6400);

    ClickTrack lone;
    click(lone, 2000, 960, 540);
    ClickTrack cluster;
    click(cluster, 2000, 950, 535);
    click(cluster, 2350, 965, 545);
    click(cluster, 2700, 955, 540);

    const auto kfsLone = KeyframeEngine::generate(cur, lone, kVideo, 6400, "16:9", {});
    const auto kfsCluster = KeyframeEngine::generate(cur, cluster, kVideo, 6400, "16:9", {});
    const double wLone = minWidth(kfsLone);
    const double wCluster = minWidth(kfsCluster);
    QVERIFY2(wLone < 1.0, "lone dwelled click must still zoom");
    QVERIFY2(wCluster < 1.0, "cluster must zoom");
    // Softening: the lone click's tightest rect is wider than the cluster's.
    QVERIFY2(wLone > wCluster + 0.02,
             qPrintable(QStringLiteral("lone %1 vs cluster %2").arg(wLone).arg(wCluster)));
}

void KeyframeEngineTest::invalidClicksFallBackToDwell()
{
    CursorTrack cur;
    moveLine(cur, 200, 200, 960, 540, 0, 1800);
    hold(cur, 960, 540, 1816, 4300);
    moveLine(cur, 960, 540, 1500, 800, 4316, 6500);
    ClickTrack clicks;
    click(clicks, -100, 960, 540);  // valid point, but before project start
    click(clicks, 2500, -100, -100); // global click outside captured stream
    click(clicks, 5000, std::numeric_limits<double>::quiet_NaN(), 540);
    click(clicks, 7000, 960, 540);   // valid point, but beyond project duration

    const qint64 duration = 6500;
    const auto kfs = KeyframeEngine::generate(cur, clicks, kVideo, duration, "16:9", {});
    const auto dwellOnly = KeyframeEngine::generate(
        cur, ClickTrack{}, kVideo, duration, "16:9", {});
    checkInvariants(kfs, duration, kOutAspect);
    QCOMPARE(kfs.size(), dwellOnly.size());
    bool zoomed = false;
    for (int i = 0; i < kfs.size(); ++i) {
        const Keyframe &keyframe = kfs.at(i);
        zoomed = zoomed || !isFull(keyframe.rect);
        QCOMPARE(keyframe.tMs, dwellOnly.at(i).tMs);
        QCOMPARE(keyframe.rect, dwellOnly.at(i).rect);
    }
    QVERIFY(zoomed); // invalid click cannot suppress valid dwell evidence
}

void KeyframeEngineTest::generatedTimesStayWithinDuration()
{
    CursorTrack cur;
    hold(cur, 960, 540, 0, 300);
    ClickTrack clicks;
    click(clicks, 100, 960, 540); // zoom-in and zoom-out both clamp to final instant
    const qint64 duration = 300;
    const auto kfs = KeyframeEngine::generate(cur, clicks, kVideo, duration, "16:9", {});
    QCOMPARE(kfs.size(), 2);
    for (int i = 0; i < kfs.size(); ++i) {
        QVERIFY(kfs.at(i).tMs >= 0);
        QVERIFY(kfs.at(i).tMs <= duration);
        if (i > 0)
            QVERIFY(kfs.at(i).tMs > kfs.at(i - 1).tMs);
    }
    QCOMPARE(kfs.last().tMs, duration);
    QVERIFY(isFull(kfs.last().rect)); // final zoom-out replaces colliding zoom target
}

void KeyframeEngineTest::postDurationDwellDoesNotZoom()
{
    CursorTrack cur;
    moveLine(cur, 200, 200, 960, 540, 0, 1000);
    hold(cur, 960, 540, 1016, 4000);

    // Only ~500 ms of the hold is inside the clip. Samples after duration must
    // not satisfy the ~1.3 s dwell threshold.
    const auto kfs = KeyframeEngine::generate(
        cur, ClickTrack{}, kVideo, 1500, "16:9", {});
    QCOMPARE(kfs.size(), 1);
    QVERIFY(isFull(kfs.first().rect));
}

void KeyframeEngineTest::nonPositiveDurationIsSafe()
{
    CursorTrack cur;
    hold(cur, 960, 540, 0, 1000);
    for (const qint64 duration : {qint64(0), qint64(-1)}) {
        const auto kfs = KeyframeEngine::generate(
            cur, ClickTrack{}, kVideo, duration, "16:9", {});
        QCOMPARE(kfs.size(), 1);
        QCOMPARE(kfs.first().tMs, qint64(0));
        QVERIFY(isFull(kfs.first().rect));
    }
}

void KeyframeEngineTest::intensityScalesZoomAndFrequency()
{
    CursorTrack cur;
    moveLine(cur, 300, 300, 1600, 760, 0, 12000);
    ClickTrack clicks;
    click(clicks, 3000, 500, 400);
    click(clicks, 6800, 1450, 700);

    KeyframeEngine::Params off;
    off.zoomIntensity = 0.0;
    const auto offKfs = KeyframeEngine::generate(cur, clicks, kVideo, 12000, "16:9", off);
    QCOMPARE(offKfs.size(), 1);
    QVERIFY(isFull(offKfs.first().rect));

    KeyframeEngine::Params low;
    low.zoomIntensity = 0.30;
    const auto lowKfs = KeyframeEngine::generate(cur, clicks, kVideo, 12000, "16:9", low);
    KeyframeEngine::Params high;
    high.zoomIntensity = 1.0;
    const auto highKfs = KeyframeEngine::generate(cur, clicks, kVideo, 12000, "16:9", high);

    auto tightestWidth = [](const QVector<Keyframe> &kfs) {
        double width = 1.0;
        for (const Keyframe &keyframe : kfs)
            width = std::min(width, keyframe.rect.width());
        return width;
    };
    QVERIFY(tightestWidth(lowKfs) > tightestWidth(highKfs) + 0.05);
    QVERIFY(fullReturns(lowKfs) < fullReturns(highKfs)); // low intensity merges the gap
}

void KeyframeEngineTest::clickTargetKeepsActivityOffEdges()
{
    CursorTrack cur;
    moveLine(cur, 900, 500, 1800, 120, 0, 5000);
    ClickTrack clicks;
    click(clicks, 3000, 1800, 120);
    const auto kfs = KeyframeEngine::generate(cur, clicks, kVideo, 7000, "16:9", {});

    QRectF target;
    for (const Keyframe &keyframe : kfs) {
        if (!isFull(keyframe.rect)) {
            target = keyframe.rect;
            break;
        }
    }
    QVERIFY(target.isValid());
    const QPointF point(1800.0 / kW, 120.0 / kH);
    QVERIFY(target.contains(point));
    const double rx = (point.x() - target.x()) / target.width();
    const double ry = (point.y() - target.y()) / target.height();
    QVERIFY(rx > 0.10 && rx < 0.90);
    QVERIFY(ry > 0.10 && ry < 0.90);
    QVERIFY(KeyframeEngine::zoomOfRect(kVideo, "16:9", target) <= 2.55 + 1e-9);
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

    QVERIFY(!isFull(evaluate(kfs, 3000)));                          // cluster still covered
}

// Aspect re-projection contract (drives the Manual-keyframe re-frame on aspect
// change): recover (center, zoom) from a rect under its ORIGINAL aspect, re-frame
// under the new one. Zoom must survive the round trip and the new rect must carry
// the NEW output aspect in pixels.
void KeyframeEngineTest::aspectReprojectionRoundtrip()
{
    const QPointF center(0.62, 0.44);
    const double zoom = 2.0;

    const QRectF r169 = KeyframeEngine::cameraRect(kVideo, "16:9", center, zoom);
    QCOMPARE(KeyframeEngine::zoomOfRect(kVideo, "16:9", r169), zoom);

    const double z = KeyframeEngine::zoomOfRect(kVideo, "16:9", r169);
    const QRectF r916 = KeyframeEngine::cameraRect(kVideo, "9:16", r169.center(), z);

    // Same zoom level under the new aspect…
    QCOMPARE(KeyframeEngine::zoomOfRect(kVideo, "9:16", r916), zoom);
    // …and the rect is genuinely output-shaped: pixel aspect == 9/16.
    const double pixAspect = (r916.width() * kW) / (r916.height() * kH);
    QVERIFY(std::fabs(pixAspect - 9.0 / 16.0) < 1e-9);
    // Center preserved up to the in-bounds clamp.
    QVERIFY(std::fabs(r916.center().x() - center.x()) < 0.25);
    QVERIFY(std::fabs(r916.center().y() - center.y()) < 0.25);
    // And a rect that was NOT re-projected renders distorted by construction —
    // the 16:9 rect's pixel aspect is not 9/16 (this is the bug being fixed).
    const double stale = (r169.width() * kW) / (r169.height() * kH);
    QVERIFY(std::fabs(stale - 9.0 / 16.0) > 0.5);
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
    const QRectF base = evaluate(kfs, 0);
    const QRectF atCluster = evaluate(kfs, 3300);
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
    SpringCameraEvaluator evaluator(kfs);
    QRectF prev = evaluator.evaluate(0);
    for (qint64 t = 16; t <= dur; t += 16) {
        const QRectF r = evaluator.evaluate(t);
        QVERIFY(std::fabs(r.x() - prev.x()) < 0.02);
        QVERIFY(std::fabs(r.y() - prev.y()) < 0.02);
        QVERIFY(std::fabs(r.right() - prev.right()) < 0.02);
        QVERIFY(std::fabs(r.bottom() - prev.bottom()) < 0.02);
        prev = r;
    }

    // No snap at authored keyframe times: the state remains continuous while the
    // physical spring converges. The target itself is still exact and stable.
    for (const Keyframe &k : kfs)
        QCOMPARE(evaluator.targetAt(k.tMs), k.rect);
}

static QVector<Keyframe> springFixture()
{
    Keyframe base;
    base.tMs = 0;
    base.rect = QRectF(0, 0, 1, 1);
    base.easeInMs = 0;

    Keyframe zoom;
    zoom.tMs = 1000;
    zoom.rect = QRectF(0.46, 0.32, 0.42, 0.42);
    zoom.easeInMs = 500; // target activates at 500 ms

    Keyframe reset;
    reset.tMs = 3200;
    reset.rect = QRectF(0, 0, 1, 1);
    reset.easeInMs = 900; // target activates at 2300 ms
    return {base, zoom, reset};
}

static double rectError(const QRectF &a, const QRectF &b)
{
    return std::max({std::fabs(a.x() - b.x()), std::fabs(a.y() - b.y()),
                     std::fabs(a.width() - b.width()), std::fabs(a.height() - b.height())});
}

void KeyframeEngineTest::springBoundsSettlingAndHold()
{
    const QVector<Keyframe> kfs = springFixture();
    SpringCameraEvaluator evaluator(kfs);
    const QRectF zoomTarget = kfs.at(1).rect;

    QRectF previous = evaluator.evaluate(0);
    double maxFrameJump = 0.0;
    double minZoomWidth = 1.0;
    for (qint64 t = 5; t <= 4500; t += 5) {
        const QRectF rect = evaluator.evaluate(t);
        QVERIFY(std::isfinite(rect.x()));
        QVERIFY(std::isfinite(rect.y()));
        QVERIFY(std::isfinite(rect.width()));
        QVERIFY(std::isfinite(rect.height()));
        QVERIFY2(rect.x() >= -1e-12 && rect.y() >= -1e-12,
                 "spring camera escaped the frame at the top/left");
        QVERIFY2(rect.right() <= 1.0 + 1e-12 && rect.bottom() <= 1.0 + 1e-12,
                 "spring camera escaped the frame at the bottom/right");
        QVERIFY(rect.width() > 0.0 && rect.height() > 0.0);
        maxFrameJump = std::max(maxFrameJump, rectError(rect, previous));
        if (t >= 500 && t < 2300)
            minZoomWidth = std::min(minZoomWidth, rect.width());
        previous = rect;
    }

    // 900 ms after target activation the near-critical spring is visually
    // settled. Zoom spring retains only a tiny designed overshoot (<1.5%).
    QVERIFY2(rectError(evaluator.evaluate(1400), zoomTarget) < 8e-4,
             "camera did not settle on the zoom target within 900 ms");
    QVERIFY(minZoomWidth < zoomTarget.width());
    QVERIFY(minZoomWidth > zoomTarget.width() * 0.985);

    // A held target becomes effectively motionless; no camera swimming.
    const QRectF holdA = evaluator.evaluate(1750);
    const QRectF holdB = evaluator.evaluate(2200);
    QVERIFY(rectError(holdA, holdB) < 8e-5);

    // 5 ms substeps are much tighter than a display frame; this guards any
    // accidental target snap or unstable integration.
    QVERIFY(maxFrameJump < 0.012);
    QVERIFY(rectError(evaluator.evaluate(4200), QRectF(0, 0, 1, 1)) < 1e-5);
}

void KeyframeEngineTest::springDeterministicAcrossSeekOrder()
{
    const QVector<Keyframe> kfs = springFixture();
    QVector<QPair<qint64, QRectF>> reference;
    SpringCameraEvaluator sequential(kfs);
    for (qint64 t = 0; t <= 4500; t += 17)
        reference.append({t, sequential.evaluate(t)});
    QVERIFY(sequential.cachedCheckpointCount() >= 9);

    // Query cadence cannot affect state: each direct simulation must agree with
    // the forward-cached 17 ms trajectory.
    for (const auto &sample : std::as_const(reference)) {
        SpringCameraEvaluator direct(kfs);
        QVERIFY(rectError(direct.evaluate(sample.first), sample.second) < 1e-12);
    }

    // Backward scrubs restore a checkpoint then replay; later forward values are
    // byte-stable within floating-point arithmetic.
    SpringCameraEvaluator scrubbed(kfs);
    const qint64 order[] = {4100, 650, 2992, 120, 4500, 1800, 2300, 999, 3200};
    for (qint64 t : order) {
        SpringCameraEvaluator direct(kfs);
        const QRectF expected = direct.evaluate(t);
        const QRectF actual = scrubbed.evaluate(t);
        QVERIFY(rectError(actual, expected) < 1e-12);
        QCOMPARE(scrubbed.evaluate(t), actual); // repeated time is idempotent
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
    p.zoomIntensity = 0.37;
    p.motionSmoothness = 0.91;

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
    QCOMPARE(r.zoomIntensity, p.zoomIntensity);
    QCOMPARE(r.motionSmoothness, p.motionSmoothness);
}

QTEST_GUILESS_MAIN(KeyframeEngineTest)
#include "KeyframeEngineTest.moc"
