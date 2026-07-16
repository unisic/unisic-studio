#include "CursorTrack.h"

#include <QTest>

class CursorTrackTest : public QObject
{
    Q_OBJECT

private slots:
    void sampleAtBeforeAfterBetween();
    void appendEnforcesMonotonicTime();
    void jsonRoundtripIsLossless();
    void jsonRoundtripHandlesShapesAndVisibleRuns();
    void exciseRemovesInsideAndShiftsLater();
    void exciseBoundaryIsStartInclusiveEndExclusive();
    void exciseMultiAndTouchingRanges();
    void emptyTrackBehaviour();
};

static CursorSample s(qint64 t, double x, double y, bool vis = true, int shape = -1)
{
    return CursorSample{t, x, y, vis, shape};
}

void CursorTrackTest::sampleAtBeforeAfterBetween()
{
    CursorTrack t;
    QVERIFY(t.append(s(0, 0, 0, true, 1)));
    QVERIFY(t.append(s(100, 100, 200, true, 1)));
    QVERIFY(t.append(s(200, 300, 100, false, 2)));

    // Before the first sample → clamp to first (position held), query t kept.
    const CursorSample before = t.sample(-50);
    QCOMPARE(before.tMs, qint64(-50));
    QCOMPARE(int(before.x), 0);
    QCOMPARE(before.visible, true);
    QCOMPARE(before.shapeId, 1);

    // Exact hit on the first sample.
    QCOMPARE(int(t.sample(0).x), 0);

    // Between 0 and 100 (f = 0.5): x/y interpolate; visible/shape from earlier.
    const CursorSample mid = t.sample(50);
    QCOMPARE(int(mid.x), 50);
    QCOMPARE(int(mid.y), 100);
    QCOMPARE(mid.shapeId, 1);
    QCOMPARE(mid.visible, true);

    // Between 100 and 200: earlier neighbour (idx 1) is still visible+shape1
    // even though the later neighbour is hidden+shape2.
    const CursorSample mid2 = t.sample(150);
    QCOMPARE(int(mid2.x), 200);
    QCOMPARE(int(mid2.y), 150);
    QCOMPARE(mid2.visible, true);
    QCOMPARE(mid2.shapeId, 1);

    // Exact last, and after the last → clamp to last.
    QCOMPARE(t.sample(200).visible, false);
    QCOMPARE(t.sample(200).shapeId, 2);
    const CursorSample after = t.sample(999);
    QCOMPARE(after.tMs, qint64(999));
    QCOMPARE(int(after.x), 300);
    QCOMPARE(after.visible, false);
}

void CursorTrackTest::appendEnforcesMonotonicTime()
{
    CursorTrack t;
    QVERIFY(t.append(s(0, 0, 0)));
    QVERIFY(t.append(s(100, 1, 1)));
    QVERIFY(t.append(s(100, 2, 2)));    // equal timestamp allowed
    QVERIFY(!t.append(s(50, 9, 9)));    // backwards rejected
    QCOMPARE(t.count(), 3);
    QCOMPARE(int(t.samples().constLast().x), 2);
}

void CursorTrackTest::jsonRoundtripIsLossless()
{
    CursorTrack t;
    // Negative deltas (x decreasing), a visible=false run, two shapes.
    t.append(s(0, 500, 500, true, 0));
    t.append(s(16, 480, 512, true, 0));
    t.append(s(33, 470, 530, false, 0));
    t.append(s(50, 465, 545, false, 1));
    t.append(s(66, 700, 400, true, 1));

    const CursorTrack r = CursorTrack::fromJson(t.toJson());
    QCOMPARE(r.count(), t.count());
    for (int i = 0; i < t.count(); ++i) {
        const CursorSample &a = t.samples().at(i);
        const CursorSample &b = r.samples().at(i);
        QCOMPARE(b.tMs, a.tMs);
        QCOMPARE(int(b.x), int(a.x));
        QCOMPARE(int(b.y), int(a.y));
        QCOMPARE(b.visible, a.visible);
        QCOMPARE(b.shapeId, a.shapeId);
    }
}

void CursorTrackTest::jsonRoundtripHandlesShapesAndVisibleRuns()
{
    CursorTrack t;
    t.append(s(0, 10, 10, false, 0));   // starts hidden
    t.append(s(10, 20, 20, false, 0));
    t.append(s(20, 30, 30, true, 1));
    QList<CursorShape> shapes{
        {0, 1, 2, QByteArray("\x89PNG-fake-A", 11)},
        {1, 3, 4, QByteArray("\x89PNG-fake-BB", 12)},
    };
    t.setShapes(shapes);

    const CursorTrack r = CursorTrack::fromJson(t.toJson());
    QCOMPARE(r.samples().at(0).visible, false);
    QCOMPARE(r.samples().at(1).visible, false);
    QCOMPARE(r.samples().at(2).visible, true);
    QCOMPARE(r.shapes().size(), 2);
    QCOMPARE(r.shapes().at(0).hotspotX, 1);
    QCOMPARE(r.shapes().at(1).hotspotY, 4);
    QCOMPARE(r.shapes().at(0).png, shapes.at(0).png);
    QCOMPARE(r.shapes().at(1).png, shapes.at(1).png);
}

void CursorTrackTest::exciseRemovesInsideAndShiftsLater()
{
    CursorTrack t;
    for (qint64 tm : {0, 100, 200, 300, 400})
        t.append(s(tm, double(tm), 0));

    t.excise({{100, 300}});   // remove 100 & 200; 300,400 shift left by 200

    QList<qint64> got;
    for (const CursorSample &sm : t.samples())
        got << sm.tMs;
    QCOMPARE(got, (QList<qint64>{0, 100, 200}));
    // The surviving samples keep their original positions, only retimed.
    QCOMPARE(int(t.samples().at(1).x), 300);
    QCOMPARE(int(t.samples().at(2).x), 400);
}

void CursorTrackTest::exciseBoundaryIsStartInclusiveEndExclusive()
{
    CursorTrack t;
    for (qint64 tm : {0, 100, 200, 300})
        t.append(s(tm, double(tm), 0));

    // A sample exactly at start (100) is removed; one exactly at end (200) is
    // kept — [start, end) — proving the half-open interval.
    t.excise({{100, 200}});

    QList<qint64> got;
    for (const CursorSample &sm : t.samples())
        got << sm.tMs;
    QCOMPARE(got, (QList<qint64>{0, 100, 200}));  // 0, (200→100), (300→200)
    QCOMPARE(int(t.samples().at(1).x), 200);      // the kept end-boundary sample
}

void CursorTrackTest::exciseMultiAndTouchingRanges()
{
    {
        CursorTrack t;
        for (qint64 tm : {0, 100, 200, 300, 400})
            t.append(s(tm, double(tm), 0));
        t.excise({{50, 150}, {250, 350}});   // drops 100 and 300
        QList<qint64> got;
        for (const CursorSample &sm : t.samples())
            got << sm.tMs;
        QCOMPARE(got, (QList<qint64>{0, 100, 200}));
        QCOMPARE(int(t.samples().at(2).x), 400);
    }
    {
        // Touching ranges must merge, not double-count the shared instant.
        CursorTrack t;
        for (qint64 tm : {0, 100, 200, 300})
            t.append(s(tm, double(tm), 0));
        t.excise({{0, 100}, {100, 200}});   // == [0,200): drops 0 and 100
        QList<qint64> got;
        for (const CursorSample &sm : t.samples())
            got << sm.tMs;
        QCOMPARE(got, (QList<qint64>{0, 100}));   // (200→0), (300→100)
        QCOMPARE(int(t.samples().at(0).x), 200);
    }
}

void CursorTrackTest::emptyTrackBehaviour()
{
    CursorTrack t;
    QVERIFY(t.isEmpty());
    QCOMPARE(t.durationMs(), qint64(0));

    const CursorSample def = t.sample(1234);
    QCOMPARE(def.tMs, qint64(1234));
    QCOMPARE(def.visible, false);   // nothing recorded → hidden

    t.excise({{0, 100}});           // no-op, must not crash
    QVERIFY(t.isEmpty());

    const CursorTrack r = CursorTrack::fromJson(t.toJson());
    QVERIFY(r.isEmpty());
}

QTEST_GUILESS_MAIN(CursorTrackTest)
#include "CursorTrackTest.moc"
