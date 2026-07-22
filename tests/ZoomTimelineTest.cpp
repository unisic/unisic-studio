#include "ZoomTimeline.h"

#include <QSignalSpy>
#include <QTest>

class ZoomTimelineTest : public QObject
{
    Q_OBJECT

private slots:
    void addKeyframeInsertsSorted();
    void moveKeyframeReSorts();
    void clearAutoSparesLockedAndManual();
    void editingPromotesAutoToManual();
    void batchRectUpdateSinglePulse();
    void editedAutoSurvivesRegenerate();
    void replaceAutoUsesSingleReset();
    void motionPropertiesClampAndNotify();
    void jsonRoundtrip();
};

static ZoomTimeline::Keyframe kf(qint64 t, ZoomTimeline::Source src = ZoomTimeline::Manual,
                                 bool locked = false)
{
    ZoomTimeline::Keyframe k;
    k.tMs = t;
    k.rect = QRectF(0.1, 0.1, 0.5, 0.5);
    k.source = src;
    k.locked = locked;
    return k;
}

void ZoomTimelineTest::addKeyframeInsertsSorted()
{
    ZoomTimeline z;
    QCOMPARE(z.addKeyframe(kf(300)), 0);
    QCOMPARE(z.addKeyframe(kf(100)), 0);   // lands before 300
    QCOMPARE(z.addKeyframe(kf(200)), 1);   // between them
    QCOMPARE(z.rowCount(), 3);
    QCOMPARE(z.keyframes().at(0).tMs, qint64(100));
    QCOMPARE(z.keyframes().at(1).tMs, qint64(200));
    QCOMPARE(z.keyframes().at(2).tMs, qint64(300));
}

void ZoomTimelineTest::moveKeyframeReSorts()
{
    ZoomTimeline z;
    z.addKeyframe(kf(100));
    z.addKeyframe(kf(200));
    z.addKeyframe(kf(300));

    // Drag the first keyframe past the last: it must re-sort to the end.
    const int newRow = z.moveKeyframe(0, 350);
    QCOMPARE(newRow, 2);
    QCOMPARE(z.keyframes().at(0).tMs, qint64(200));
    QCOMPARE(z.keyframes().at(1).tMs, qint64(300));
    QCOMPARE(z.keyframes().at(2).tMs, qint64(350));

    QCOMPARE(z.moveKeyframe(99, 0), -1);   // out of range
}

void ZoomTimelineTest::clearAutoSparesLockedAndManual()
{
    ZoomTimeline z;
    z.addKeyframe(kf(100, ZoomTimeline::Auto, false));    // dropped
    z.addKeyframe(kf(200, ZoomTimeline::Auto, true));     // spared: locked
    z.addKeyframe(kf(300, ZoomTimeline::Manual, false));  // spared: manual
    z.addKeyframe(kf(400, ZoomTimeline::Auto, false));    // dropped

    z.clearAuto();
    QCOMPARE(z.rowCount(), 2);
    QCOMPARE(z.keyframes().at(0).tMs, qint64(200));
    QCOMPARE(z.keyframes().at(1).tMs, qint64(300));
}

// Editing an auto keyframe must hand it to the user. Without this the next
// regenerate drops the edit as its own output.
void ZoomTimelineTest::editingPromotesAutoToManual()
{
    ZoomTimeline z;
    z.addKeyframe(kf(100, ZoomTimeline::Auto, false));
    z.addKeyframe(kf(200, ZoomTimeline::Auto, false));
    z.addKeyframe(kf(300, ZoomTimeline::Auto, false));

    // Retiming, re-framing and re-easing are all user edits.
    const int moved = z.moveKeyframe(0, 150);
    QCOMPARE(z.keyframes().at(moved).source, ZoomTimeline::Manual);

    QSignalSpy dataSpy(&z, &QAbstractItemModel::dataChanged);
    z.setKeyframeRect(1, QRectF(0.3, 0.3, 0.4, 0.4));
    QCOMPARE(z.keyframes().at(1).source, ZoomTimeline::Manual);
    QCOMPARE(z.keyframes().at(1).rect, QRectF(0.3, 0.3, 0.4, 0.4));
    // SourceRole must ride along or the timeline pill keeps its Auto tint.
    QCOMPARE(dataSpy.count(), 1);
    QVERIFY(dataSpy.at(0).at(2).value<QList<int>>().contains(ZoomTimeline::SourceRole));

    z.setKeyframeEasing(2, 120, 140);
    QCOMPARE(z.keyframes().at(2).source, ZoomTimeline::Manual);
}

// setKeyframeRects is the aspect-reprojection batch path: ONE changed() pulse
// for N rows, and — unlike setKeyframeRect — no promotion to Manual (it
// re-frames rows the caller already knows are pinned; converting a locked Auto
// row would change its regenerate semantics).
void ZoomTimelineTest::batchRectUpdateSinglePulse()
{
    ZoomTimeline z;
    z.addKeyframe(kf(100, ZoomTimeline::Auto, true));      // locked auto
    z.addKeyframe(kf(200, ZoomTimeline::Manual, false));
    z.addKeyframe(kf(300, ZoomTimeline::Manual, false));

    QSignalSpy changedSpy(&z, &ZoomTimeline::changed);
    z.setKeyframeRects({{0, QRectF(0.1, 0.1, 0.4, 0.4)},
                        {2, QRectF(0.3, 0.3, 0.5, 0.5)},
                        {99, QRectF(0, 0, 1, 1)}});        // out of range: ignored
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(z.keyframes().at(0).rect, QRectF(0.1, 0.1, 0.4, 0.4));
    QCOMPARE(z.keyframes().at(0).source, ZoomTimeline::Auto);   // NOT promoted
    QCOMPARE(z.keyframes().at(2).rect, QRectF(0.3, 0.3, 0.5, 0.5));

    QSignalSpy noopSpy(&z, &ZoomTimeline::changed);
    z.setKeyframeRects({{-1, QRectF()}, {7, QRectF()}});   // nothing valid
    QCOMPARE(noopSpy.count(), 0);
}

// The end-to-end shape of the user's complaint: edit an auto keyframe, then let
// the engine regenerate. The edit must still be there.
void ZoomTimelineTest::editedAutoSurvivesRegenerate()
{
    ZoomTimeline z;
    z.addKeyframe(kf(100, ZoomTimeline::Auto, false));
    z.addKeyframe(kf(200, ZoomTimeline::Auto, false));

    const QRectF edited(0.25, 0.25, 0.5, 0.5);
    z.setKeyframeRect(1, edited);

    // Regeneration replaces every untouched Auto row with a fresh batch.
    z.replaceAutoKeyframes({kf(400, ZoomTimeline::Auto, false)});

    QCOMPARE(z.rowCount(), 2);
    QCOMPARE(z.keyframes().at(0).tMs, qint64(200)); // the edited one survived
    QCOMPARE(z.keyframes().at(0).rect, edited);
    QCOMPARE(z.keyframes().at(1).tMs, qint64(400)); // the untouched one was replaced
}

void ZoomTimelineTest::replaceAutoUsesSingleReset()
{
    ZoomTimeline timeline;
    timeline.addKeyframe(kf(100, ZoomTimeline::Auto, false));
    timeline.addKeyframe(kf(200, ZoomTimeline::Auto, true));
    timeline.addKeyframe(kf(300, ZoomTimeline::Manual, false));
    QSignalSpy resetSpy(&timeline, &QAbstractItemModel::modelReset);
    QSignalSpy changedSpy(&timeline, &ZoomTimeline::changed);

    timeline.replaceAutoKeyframes({kf(400, ZoomTimeline::Auto, false),
                                   kf(500, ZoomTimeline::Auto, false)});
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(changedSpy.count(), 1);
    QCOMPARE(timeline.rowCount(), 4);
    QCOMPARE(timeline.keyframes().at(0).tMs, qint64(200)); // locked Auto survived
    QCOMPARE(timeline.keyframes().at(1).tMs, qint64(300)); // Manual survived
}

void ZoomTimelineTest::jsonRoundtrip()
{
    ZoomTimeline z;
    ZoomTimeline::Keyframe a = kf(100, ZoomTimeline::Auto, true);
    a.rect = QRectF(0.2, 0.25, 0.5, 0.4);
    a.easeInMs = 250;
    a.easeOutMs = 400;
    z.addKeyframe(a);
    z.addKeyframe(kf(500, ZoomTimeline::Manual, false));
    z.setAutoParams(QJsonObject{{QStringLiteral("strength"), 1.5},
                                {QStringLiteral("dwellMs"), 800},
                                {QStringLiteral("zoomIntensity"), 0.42},
                                {QStringLiteral("motionSmoothness"), 0.81}});

    ZoomTimeline r;
    r.fromJson(z.toJson());
    QCOMPARE(r.rowCount(), 2);
    const ZoomTimeline::Keyframe &b = r.keyframes().at(0);
    QCOMPARE(b.tMs, qint64(100));
    QCOMPARE(b.rect, QRectF(0.2, 0.25, 0.5, 0.4));
    QCOMPARE(b.easeInMs, 250);
    QCOMPARE(b.easeOutMs, 400);
    QCOMPARE(b.source, ZoomTimeline::Auto);
    QCOMPARE(b.locked, true);
    QCOMPARE(r.keyframes().at(1).source, ZoomTimeline::Manual);
    QCOMPARE(r.autoParams().value(QStringLiteral("dwellMs")).toInt(), 800);
    QCOMPARE(r.zoomIntensity(), 0.42);
    QCOMPARE(r.motionSmoothness(), 0.81);
}

void ZoomTimelineTest::motionPropertiesClampAndNotify()
{
    ZoomTimeline z;
    QCOMPARE(z.zoomIntensity(), ZoomTimeline::DefaultZoomIntensity);
    QCOMPARE(z.motionSmoothness(), ZoomTimeline::DefaultMotionSmoothness);

    QSignalSpy intensitySpy(&z, &ZoomTimeline::zoomIntensityChanged);
    QSignalSpy smoothnessSpy(&z, &ZoomTimeline::motionSmoothnessChanged);
    QSignalSpy changedSpy(&z, &ZoomTimeline::changed);
    z.setZoomIntensity(-3.0);
    z.setMotionSmoothness(4.0);
    QCOMPARE(z.zoomIntensity(), 0.0);
    QCOMPARE(z.motionSmoothness(), 1.0);
    QCOMPARE(intensitySpy.count(), 1);
    QCOMPARE(smoothnessSpy.count(), 1);
    QCOMPARE(changedSpy.count(), 2);

    z.setZoomIntensity(0.0);
    z.setMotionSmoothness(1.0);
    QCOMPARE(changedSpy.count(), 2); // no duplicate mutation pulses
}

QTEST_GUILESS_MAIN(ZoomTimelineTest)
#include "ZoomTimelineTest.moc"
