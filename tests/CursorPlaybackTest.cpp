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

class CursorPlaybackTest : public QObject
{
    Q_OBJECT

private slots:
    void springFollowLagsOvershootsAndSettles();
    void deterministicAcrossSeeksAndCadence();
    void idleHideAndWake();
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
