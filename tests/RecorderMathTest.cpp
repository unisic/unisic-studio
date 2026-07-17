#include "RecorderMath.h"

#include "ClickTrack.h"
#include "CursorTrack.h"

#include <QTest>

using RecorderMath::monoNsToVideoMs;
using RecorderMath::pauseRangesToVideoMs;

class RecorderMathTest : public QObject
{
    Q_OBJECT

private slots:
    void monoToVideoMsMapping();
    void pauseRangesConvertAndMerge();
    void clickPositionsResolvedThenExcised();
};

void RecorderMathTest::monoToVideoMsMapping()
{
    const qint64 t0 = 1'000'000'000;   // arbitrary CLOCK_MONOTONIC origin (ns)

    // A frame at t0 is video-ms 0.
    QCOMPARE(monoNsToVideoMs(t0, t0), qint64(0));
    // Whole-millisecond offsets.
    QCOMPARE(monoNsToVideoMs(t0 + 16'000'000, t0), qint64(16));
    QCOMPARE(monoNsToVideoMs(t0 + 1'000'000'000, t0), qint64(1000));
    // Rounds to nearest: .4 ms down, .5 ms up.
    QCOMPARE(monoNsToVideoMs(t0 + 16'400'000, t0), qint64(16));
    QCOMPARE(monoNsToVideoMs(t0 + 16'500'000, t0), qint64(17));
    // Before t0 (a cursor sample from the pre-roll): symmetric rounding, negative.
    QCOMPARE(monoNsToVideoMs(t0 - 2'000'000, t0), qint64(-2));
    QCOMPARE(monoNsToVideoMs(t0 - 2'500'000, t0), qint64(-3));
    // The offset is exactly t0: a different origin shifts everything.
    QCOMPARE(monoNsToVideoMs(5'000'000'000, 2'000'000'000), qint64(3000));
}

void RecorderMathTest::pauseRangesConvertAndMerge()
{
    const qint64 t0 = 1'000'000'000;
    using RangeList = QList<QPair<qint64, qint64>>;

    // One span [t0+1s, t0+3s) → [1000, 3000] ms.
    {
        const RangeList in{{t0 + 1'000'000'000, t0 + 3'000'000'000}};
        QCOMPARE(pauseRangesToVideoMs(in, t0), (RangeList{{1000, 3000}}));
    }
    // Touching spans merge (no double-count of the shared instant).
    {
        const RangeList in{{t0 + 1'000'000'000, t0 + 2'000'000'000},
                           {t0 + 2'000'000'000, t0 + 3'000'000'000}};
        QCOMPARE(pauseRangesToVideoMs(in, t0), (RangeList{{1000, 3000}}));
    }
    // Overlapping spans merge.
    {
        const RangeList in{{t0 + 1'000'000'000, t0 + 2'500'000'000},
                           {t0 + 2'000'000'000, t0 + 3'000'000'000}};
        QCOMPARE(pauseRangesToVideoMs(in, t0), (RangeList{{1000, 3000}}));
    }
    // Unsorted, disjoint spans come back sorted and separate.
    {
        const RangeList in{{t0 + 2'000'000'000, t0 + 3'000'000'000},
                           {t0 + 500'000'000, t0 + 1'000'000'000}};
        QCOMPARE(pauseRangesToVideoMs(in, t0), (RangeList{{500, 1000}, {2000, 3000}}));
    }
    // Empty / reversed spans are dropped.
    {
        const RangeList in{{t0 + 2'000'000'000, t0 + 2'000'000'000},
                           {t0 + 3'000'000'000, t0 + 1'000'000'000}};
        QCOMPARE(pauseRangesToVideoMs(in, t0), RangeList{});
    }
}

// Mirrors RecordingAssembler: resolve each click's position from the cursor track
// at its instant, THEN excise the same pause ranges from both tracks. A click
// keeps the position it was resolved to; only its timestamp shifts left.
void RecorderMathTest::clickPositionsResolvedThenExcised()
{
    CursorTrack cursor;
    for (qint64 t : {0, 100, 200, 300, 400})
        cursor.append(CursorSample{t, double(t), double(t), true, -1});

    // Click video-times: mid-segment, inside the pause, after the pause, and past
    // the last sample (clamps to the last position).
    const QList<qint64> clickTimes{50, 150, 350, 999};
    ClickTrack clicks;
    for (qint64 t : clickTimes) {
        const CursorSample at = cursor.sample(t);
        clicks.append(ClickEvent{t, int(Qt::LeftButton), ClickEvent::Down, at.x, at.y});
    }

    // Sanity on the resolved positions before excision.
    QCOMPARE(int(clicks.events().at(0).x), 50);    // interpolated on [0,100]
    QCOMPARE(int(clicks.events().at(1).x), 150);   // interpolated on [100,200]
    QCOMPARE(int(clicks.events().at(2).x), 350);   // interpolated on [300,400]
    QCOMPARE(int(clicks.events().at(3).x), 400);   // clamped to the last sample

    const QList<QPair<qint64, qint64>> ranges{{100, 300}};
    cursor.excise(ranges);
    clicks.excise(ranges);

    // Cursor: 100 & 200 removed; 300→100, 400→200 (positions kept).
    QList<qint64> cursorTimes;
    for (const CursorSample &s : cursor.samples())
        cursorTimes << s.tMs;
    QCOMPARE(cursorTimes, (QList<qint64>{0, 100, 200}));
    QCOMPARE(int(cursor.samples().at(1).x), 300);

    // Clicks: t=150 (inside the hole) removed; the rest shift left by 200 and keep
    // their resolved positions.
    QCOMPARE(clicks.count(), 3);
    QCOMPARE(clicks.events().at(0).tMs, qint64(50));
    QCOMPARE(int(clicks.events().at(0).x), 50);
    QCOMPARE(clicks.events().at(1).tMs, qint64(150));   // was 350, shifted by 200
    QCOMPARE(int(clicks.events().at(1).x), 350);
    QCOMPARE(clicks.events().at(2).tMs, qint64(799));   // was 999, shifted by 200
    QCOMPARE(int(clicks.events().at(2).x), 400);
}

QTEST_GUILESS_MAIN(RecorderMathTest)
#include "RecorderMathTest.moc"
