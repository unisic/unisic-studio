#include "CursorPlayback.h"

#include "ClickTrack.h"
#include "CursorSmoother.h"
#include "CursorTrack.h"

#include <QSize>
#include <QTest>

#include <cmath>

// CursorPlayback first applies one-euro smoothing, then a deterministic soft
// spring. Raw/smoothed samples remain the target reference; rendered positions
// intentionally lag and may overshoot slightly.
static CursorTrack smoothedRef(const CursorTrack &raw)
{
    CursorTrack out;
    const CursorSmoother sm;
    for (const CursorSample &s : sm.smooth(raw))
        out.append(s);
    return out;
}

// A scripted cursor path: a diagonal move with a couple of irregular gaps so the
// lookup's cached-index advance is exercised over uneven spacing.
static CursorTrack scriptedCursor()
{
    CursorTrack t;
    const int n = 200;
    for (int i = 0; i < n; ++i) {
        CursorSample s;
        s.tMs = qint64(i) * 33 + (i % 7 == 0 ? 5 : 0); // slightly irregular dt
        s.x = 100.0 + i * 8.0;
        s.y = 80.0 + i * 4.0;
        s.visible = (i % 50 != 0);                     // a few hidden samples
        s.shapeId = (i < 100) ? 0 : 1;                 // shape switch mid-way
        t.append(s);
    }
    return t;
}

static CursorTrack moveThenHold()
{
    CursorTrack track;
    for (qint64 t = 0; t <= 1000; t += 20)
        track.append({t, 200, 250, true, 0});
    for (qint64 t = 1020; t <= 2000; t += 20) {
        const double f = double(t - 1000) / 1000.0;
        track.append({t, 200 + 1200 * f, 250 + 300 * f, true, 0});
    }
    for (qint64 t = 2020; t <= 6500; t += 20)
        track.append({t, 1400, 550, true, 0});
    return track;
}

// A violent flick: 1700 px in 240 ms (~3.7 screens/s), far faster than the
// spring's catch-up speed — exercises the rendered-lag cap.
static CursorTrack fastFlick()
{
    CursorTrack track;
    for (qint64 t = 0; t <= 1000; t += 20)
        track.append({t, 100, 500, true, 0});
    for (qint64 t = 1020; t <= 1240; t += 20) {
        const double f = double(t - 1000) / 240.0;
        track.append({t, 100 + 1700 * f, 500 + 60 * f, true, 0});
    }
    for (qint64 t = 1260; t <= 3500; t += 20)
        track.append({t, 1800, 560, true, 0});
    return track;
}

// Move, idle long enough for the fade-out to BEGIN but not finish, then move
// again — the wake fade must resume from the partial level, not snap to 1.
static CursorTrack moveIdleMove()
{
    CursorTrack track;
    for (qint64 t = 0; t <= 1000; t += 20)
        track.append({t, 200, 250, true, 0});
    for (qint64 t = 1020; t < 2000; t += 20) {
        const double f = double(t - 1000) / 1000.0;
        track.append({t, 200 + 1200 * f, 250 + 300 * f, true, 0});
    }
    for (qint64 t = 2000; t < 4600; t += 20)
        track.append({t, 1400, 550, true, 0});
    for (qint64 t = 4600; t < 5400; t += 20) {
        const double f = double(t - 4600) / 800.0;
        track.append({t, 1400 - 800 * f, 550 - 250 * f, true, 0});
    }
    for (qint64 t = 5400; t <= 7200; t += 20)
        track.append({t, 600, 300, true, 0});
    return track;
}

// Linear interpolation over the smoothed samples, normalised — the spring's
// actual chase target at an arbitrary time.
static QPointF smoothedTargetAt(const CursorTrack &smoothed, qint64 tMs, const QSize &video)
{
    const QList<CursorSample> &s = smoothed.samples();
    int i = 0;
    while (i + 1 < s.size() && s.at(i + 1).tMs <= tMs)
        ++i;
    double x = s.at(i).x;
    double y = s.at(i).y;
    if (i + 1 < s.size() && tMs > s.at(i).tMs) {
        const double span = double(s.at(i + 1).tMs - s.at(i).tMs);
        const double f = span > 0 ? std::min(1.0, double(tMs - s.at(i).tMs) / span) : 0.0;
        x += (s.at(i + 1).x - x) * f;
        y += (s.at(i + 1).y - y) * f;
    }
    return QPointF(x / video.width(), y / video.height());
}

class CursorPlaybackTest : public QObject
{
    Q_OBJECT

private slots:
    void springFollowLagsOvershootsAndSettles();
    void fastFlickLagBoundedAndSettles();
    void deterministicAcrossSeeksAndCadence();
    void idleHideAndWake();
    void wakeMidFadeIsContinuous();
    void ripplesWindow();
};

void CursorPlaybackTest::springFollowLagsOvershootsAndSettles()
{
    const CursorTrack track = moveThenHold();
    const QSize video(1920, 1080);
    CursorPlayback pb(QStringLiteral("test"));
    pb.setTracks(track, ClickTrack{}, video);
    const CursorTrack ref_track = smoothedRef(track);

    QCOMPARE(pb.nx(), 200.0 / video.width());
    QCOMPARE(pb.ny(), 250.0 / video.height());

    // During deliberate travel the rendered cursor trails its smoothed target.
    pb.setTime(1500);
    const CursorSample targetMid = ref_track.sample(1500);
    QVERIFY(pb.nx() < targetMid.x / video.width() - 0.01);
    QVERIFY(pb.ny() < targetMid.y / video.height() - 0.003);

    double maxX = 0.0;
    double maxJump = 0.0;
    double previousX = pb.nx();
    for (qint64 t = 1516; t <= 3800; t += 16) {
        pb.setTime(t);
        maxX = std::max(maxX, double(pb.nx()));
        maxJump = std::max(maxJump, std::fabs(double(pb.nx()) - previousX));
        previousX = pb.nx();
    }
    const double finalX = 1400.0 / video.width();
    QVERIFY(maxX > finalX);                    // restrained physical overshoot
    QVERIFY(maxX < finalX + 0.012);            // never a cartoon bounce
    QVERIFY(maxJump < 0.02);                   // no rendered-position snap

    pb.setTime(3600);
    QVERIFY(std::fabs(pb.nx() - finalX) < 3e-4);
    QVERIFY(std::fabs(pb.ny() - 550.0 / video.height()) < 3e-4);

    // ...and STAYS settled — later probes (including after the idle fade-out
    // has hidden the pointer) never drift off the track point.
    for (const qint64 t : {qint64(4400), qint64(5200), qint64(6400)}) {
        pb.setTime(t);
        QVERIFY(std::fabs(pb.nx() - finalX) < 3e-4);
        QVERIFY(std::fabs(pb.ny() - 550.0 / video.height()) < 3e-4);
    }
}

// During a flick far faster than the spring's catch-up speed the rendered
// pointer must be towed within a bounded distance of its target (a cursor half
// a screen behind reads as broken), stay continuous, and settle cleanly.
void CursorPlaybackTest::fastFlickLagBoundedAndSettles()
{
    const CursorTrack track = fastFlick();
    const QSize video(1920, 1080);
    CursorPlayback pb(QStringLiteral("flick"));
    pb.setTracks(track, ClickTrack{}, video);
    const CursorTrack ref = smoothedRef(track);

    // Lag cap 0.06 + a little slack for the 5 ms substep target sampling.
    double maxLag = 0.0;
    double maxStep = 0.0;
    double previousX = -1.0;
    double previousY = -1.0;
    for (qint64 t = 1000; t <= 1600; t += 8) {
        pb.setTime(t);
        const QPointF target = smoothedTargetAt(ref, t, video);
        maxLag = std::max(maxLag, std::hypot(pb.nx() - target.x(), pb.ny() - target.y()));
        if (previousX >= 0.0)
            maxStep = std::max(maxStep,
                               std::hypot(pb.nx() - previousX, pb.ny() - previousY));
        previousX = pb.nx();
        previousY = pb.ny();
    }
    QVERIFY(maxLag < 0.075);
    QVERIFY(maxLag > 0.05);       // the flick genuinely engaged the cap
    QVERIFY(maxStep < 0.05);      // towing stays continuous, no teleport

    // Release: converge on the flick's endpoint with only a small overshoot.
    const double finalX = 1800.0 / video.width();
    const double finalY = 560.0 / video.height();
    double maxX = 0.0;
    for (qint64 t = 1240; t <= 2400; t += 8) {
        pb.setTime(t);
        maxX = std::max(maxX, double(pb.nx()));
    }
    QVERIFY(maxX < finalX + 0.02);
    pb.setTime(2000);
    QVERIFY(std::fabs(pb.nx() - finalX) < 2e-3);
    for (const qint64 t : {qint64(2600), qint64(3000), qint64(3400)}) {
        pb.setTime(t);
        QVERIFY(std::fabs(pb.nx() - finalX) < 3e-4);
        QVERIFY(std::fabs(pb.ny() - finalY) < 3e-4);
    }
}

void CursorPlaybackTest::deterministicAcrossSeeksAndCadence()
{
    const CursorTrack track = moveThenHold();
    const QSize video(1920, 1080);
    CursorPlayback scrubbed(QStringLiteral("scrubbed"));
    scrubbed.setTracks(track, ClickTrack{}, video);
    const qint64 probes[] = {5000, 10, 3333, 0, 6200, 200, -100, 99999, 1500};
    for (qint64 t : probes) {
        CursorPlayback direct(QStringLiteral("direct"));
        direct.setTracks(track, ClickTrack{}, video);
        direct.setTime(t);
        scrubbed.setTime(t);
        QVERIFY(std::fabs(scrubbed.nx() - direct.nx()) < 1e-12);
        QVERIFY(std::fabs(scrubbed.ny() - direct.ny()) < 1e-12);
        QCOMPARE(scrubbed.cursorOpacity(), direct.cursorOpacity());
        QCOMPARE(scrubbed.sampleIndexFor(t), direct.sampleIndexFor(t));
    }

    // Full sweeps over one probe grid in forward, backward and shuffled order
    // agree pointwise: the checkpointed forward simulation is order-blind.
    QVector<qint64> grid;
    for (qint64 t = 0; t <= 6400; t += 97)
        grid.append(t);
    const int n = grid.size();
    QVector<qreal> fx(n), fy(n), fo(n);
    CursorPlayback forward(QStringLiteral("forward"));
    forward.setTracks(track, ClickTrack{}, video);
    for (int i = 0; i < n; ++i) {
        forward.setTime(grid.at(i));
        fx[i] = forward.nx();
        fy[i] = forward.ny();
        fo[i] = forward.cursorOpacity();
    }
    CursorPlayback backward(QStringLiteral("backward"));
    backward.setTracks(track, ClickTrack{}, video);
    for (int i = n - 1; i >= 0; --i) {
        backward.setTime(grid.at(i));
        QVERIFY(std::fabs(backward.nx() - fx.at(i)) < 1e-12);
        QVERIFY(std::fabs(backward.ny() - fy.at(i)) < 1e-12);
        QVERIFY(std::fabs(backward.cursorOpacity() - fo.at(i)) < 1e-12);
    }
    // Stride 37 is coprime with the grid size, so this visits every index once
    // in a scrambled order without needing a random source.
    CursorPlayback shuffled(QStringLiteral("shuffled"));
    shuffled.setTracks(track, ClickTrack{}, video);
    for (int k = 0; k < n; ++k) {
        const int i = int((qint64(k) * 37) % n);
        shuffled.setTime(grid.at(i));
        QVERIFY(std::fabs(shuffled.nx() - fx.at(i)) < 1e-12);
        QVERIFY(std::fabs(shuffled.ny() - fy.at(i)) < 1e-12);
        QVERIFY(std::fabs(shuffled.cursorOpacity() - fo.at(i)) < 1e-12);
    }
}

void CursorPlaybackTest::idleHideAndWake()
{
    const CursorTrack track = moveThenHold();
    CursorPlayback pb(QStringLiteral("idle"));
    pb.setTracks(track, ClickTrack{}, QSize(1920, 1080));

    pb.setTime(3800); // motion ended around 2.1 s; still inside idle grace
    QCOMPARE(pb.cursorOpacity(), 1.0);
    QVERIFY(pb.cursorVisible());
    pb.setTime(4750); // fade-out interval (after smoother + spring settle)
    QVERIFY(pb.cursorOpacity() > 0.0 && pb.cursorOpacity() < 1.0);
    pb.setTime(5300);
    QCOMPARE(pb.cursorOpacity(), 0.0);
    QVERIFY(!pb.cursorVisible());

    // Backward seek into movement reproduces the visible state immediately and
    // does not retain hidden state from the later timestamp.
    pb.setTime(1600);
    QCOMPARE(pb.cursorOpacity(), 1.0);
    QVERIFY(pb.cursorVisible());
}

// Motion resuming while the idle fade-out is only partway done must pick the
// wake fade up from the partial level — never snap opacity back to 1 between
// two consecutive frames.
void CursorPlaybackTest::wakeMidFadeIsContinuous()
{
    const CursorTrack track = moveIdleMove();
    CursorPlayback pb(QStringLiteral("wake"));
    pb.setTracks(track, ClickTrack{}, QSize(1920, 1080));

    qreal minOpacity = 1.0;
    qreal maxStep = 0.0;
    qreal previous = -1.0;
    for (qint64 t = 2500; t <= 6800; t += 8) {
        pb.setTime(t);
        const qreal op = pb.cursorOpacity();
        if (previous >= 0.0)
            maxStep = std::max(maxStep, std::fabs(op - previous));
        minOpacity = std::min(minOpacity, op);
        previous = op;
    }
    // Guards: the fade-out genuinely began (dip below 0.9) but had not finished
    // (never reached 0) when the second move woke the pointer.
    QVERIFY(minOpacity < 0.90);
    QVERIFY(minOpacity > 0.03);
    // The wake resumed from that partial level: bounded per-8ms change, no snap.
    QVERIFY(maxStep < 0.09);
    // Fully awake shortly after movement resumes.
    pb.setTime(5200);
    QCOMPARE(pb.cursorOpacity(), 1.0);
}

// A ripple is active only inside [downT, downT + rippleMs); the model reflects
// exactly the clicks whose window covers the current time.
void CursorPlaybackTest::ripplesWindow()
{
    CursorTrack track = scriptedCursor();
    ClickTrack clicks;
    auto down = [&](qint64 t, double x, double y) {
        ClickEvent e; e.tMs = t; e.button = 1; e.state = ClickEvent::Down; e.x = x; e.y = y;
        clicks.append(e);
    };
    down(1000, 500, 400);
    down(1100, 520, 410);   // overlaps the first (within rippleMs)
    down(5000, 900, 600);   // isolated

    CursorPlayback pb(QStringLiteral("test"));
    pb.setTracks(track, clicks, QSize(1920, 1080));
    QAbstractListModel *m = pb.ripples();

    pb.setTime(0);
    QCOMPARE(m->rowCount(), 0);              // before any click

    pb.setTime(1050);
    QCOMPARE(m->rowCount(), 1);              // first click active
    const qreal rippleX = m->data(m->index(0, 0), CursorRippleModel::NxRole).toReal();
    CursorPlayback clickReference(QStringLiteral("reference"));
    clickReference.setTracks(track, clicks, QSize(1920, 1080));
    clickReference.setTime(1000);
    QVERIFY(std::fabs(rippleX - clickReference.nx()) < 1e-12);
    QVERIFY(pb.pressScale() < 1.0);

    pb.setTime(1200);
    QCOMPARE(m->rowCount(), 2);              // both early clicks overlap
    QVERIFY(pb.msSinceClick() == 100.0);
    QVERIFY(pb.pressScale() > 1.0);           // restrained rebound after second click

    pb.setTime(1600);
    QCOMPARE(m->rowCount(), 0);              // both expired (rippleMs=440)
    QCOMPARE(pb.msSinceClick(), -1.0);

    pb.setTime(5100);
    QCOMPARE(m->rowCount(), 1);              // isolated click active

    pb.setTime(9000);
    QCOMPARE(m->rowCount(), 0);              // long after
}

QTEST_GUILESS_MAIN(CursorPlaybackTest)
#include "CursorPlaybackTest.moc"
