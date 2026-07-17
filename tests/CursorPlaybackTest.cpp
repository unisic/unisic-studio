#include "CursorPlayback.h"

#include "ClickTrack.h"
#include "CursorSmoother.h"
#include "CursorTrack.h"

#include <QSize>
#include <QTest>

#include <cmath>

// CursorPlayback renders from a one-euro-SMOOTHED copy of the track (the virtual
// cursor glides instead of reproducing raw jitter), so the reference the lookup
// must agree with is the smoothed track, not the raw one. Smoothing preserves
// tMs / visible / shapeId verbatim, so the index/visibility logic is unchanged.
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

class CursorPlaybackTest : public QObject
{
    Q_OBJECT

private slots:
    void lookupMatchesCursorTrack();
    void lookupHandlesSeeksAndClamp();
    void ripplesWindow();
};

// The cached-index lookup + interpolation must agree with the reference
// CursorTrack::sample() at every probed instant, whatever the access order.
void CursorPlaybackTest::lookupMatchesCursorTrack()
{
    const CursorTrack track = scriptedCursor();
    const QSize video(1920, 1080);
    ClickTrack clicks;

    CursorPlayback pb(QStringLiteral("test"));
    pb.setTracks(track, clicks, video);
    const CursorTrack ref_track = smoothedRef(track);

    // Sequential forward sweep at fine granularity (the O(1) hot path).
    for (qint64 t = 0; t <= 7000; t += 7) {
        pb.setTime(t);
        const CursorSample ref = ref_track.sample(t);
        QVERIFY(std::fabs(pb.nx() - ref.x / video.width()) < 1e-9);
        QVERIFY(std::fabs(pb.ny() - ref.y / video.height()) < 1e-9);
        QCOMPARE(pb.cursorVisible(), ref.visible);
    }
}

void CursorPlaybackTest::lookupHandlesSeeksAndClamp()
{
    const CursorTrack track = scriptedCursor();
    const QSize video(1920, 1080);
    CursorPlayback pb(QStringLiteral("test"));
    pb.setTracks(track, ClickTrack{}, video);
    const CursorTrack ref_track = smoothedRef(track);

    // Random-ish jumps (backward seeks, out-of-range clamps) must still match.
    const qint64 probes[] = {5000, 10, 3333, 0, 6999, 200, -100, 99999, 1500};
    for (qint64 t : probes) {
        pb.setTime(t);
        const CursorSample ref = ref_track.sample(t);
        QVERIFY(std::fabs(pb.nx() - ref.x / video.width()) < 1e-9);
        QVERIFY(std::fabs(pb.ny() - ref.y / video.height()) < 1e-9);
    }
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

    pb.setTime(1150);
    QCOMPARE(m->rowCount(), 2);              // both early clicks overlap

    pb.setTime(1600);
    QCOMPARE(m->rowCount(), 0);              // both expired (rippleMs=420)

    pb.setTime(5100);
    QCOMPARE(m->rowCount(), 1);              // isolated click active

    pb.setTime(9000);
    QCOMPARE(m->rowCount(), 0);              // long after
}

QTEST_GUILESS_MAIN(CursorPlaybackTest)
#include "CursorPlaybackTest.moc"
