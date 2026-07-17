#include "PreviewController.h"

#include "CursorPlayback.h"
#include "KeyframeEngine.h"
#include "StudioProject.h"
#include "ZoomTimeline.h"

#include <QSignalSpy>
#include <QTest>

class PreviewControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void scrubAndBackwardSeekMatchSharedEvaluator();
    void playbackStopsExactlyAtTrimOut();
};

static StudioProject *makeProject(QObject *parent)
{
    auto *project = new StudioProject(parent);
    project->setDurationMs(3000);
    project->setVideoSize(QSize(1280, 720));
    CursorTrack cursor;
    cursor.append({0, 200, 200, true, 0});
    cursor.append({1000, 900, 400, true, 0});
    cursor.append({3000, 900, 400, true, 0});
    project->setCursorTrack(cursor);

    ZoomTimeline::Keyframe base;
    base.tMs = 0;
    base.rect = QRectF(0, 0, 1, 1);
    base.easeInMs = 0;
    ZoomTimeline::Keyframe zoom;
    zoom.tMs = 1000;
    zoom.rect = QRectF(0.35, 0.25, 0.5, 0.5);
    zoom.easeInMs = 500;
    project->zoom()->addKeyframes({base, zoom});
    return project;
}

void PreviewControllerTest::scrubAndBackwardSeekMatchSharedEvaluator()
{
    QScopedPointer<StudioProject> project(makeProject(nullptr));
    PreviewController preview(project.data());
    SpringCameraEvaluator reference(project->zoom()->keyframes(),
                                    project->zoom()->motionSmoothness());

    const qint64 probes[] = {1500, 100, 900, 3000, 500, 1500};
    for (qint64 time : probes) {
        preview.snap(time);
        QCOMPARE(qint64(preview.timeMs()), time);
        QCOMPARE(preview.zoomRect(), reference.evaluate(time));
        QCOMPARE(qint64(preview.cursor()->timeMs()), time);
    }

    project->zoom()->setMotionSmoothness(0.95);
    SpringCameraEvaluator soft(project->zoom()->keyframes(), 0.95);
    QCOMPARE(preview.zoomRect(), soft.evaluate(qint64(preview.timeMs())));
}

void PreviewControllerTest::playbackStopsExactlyAtTrimOut()
{
    QScopedPointer<StudioProject> project(makeProject(nullptr));
    project->setTrimInMs(100);
    project->setTrimOutMs(360);
    PreviewController preview(project.data());
    QSignalSpy ended(&preview, &PreviewController::playbackRangeEnded);

    preview.sync(320, true);
    QTRY_COMPARE_WITH_TIMEOUT(ended.count(), 1, 500);
    QCOMPARE(preview.timeMs(), qreal(360));
    QCOMPARE(preview.cursor()->timeMs(), qreal(360));

    // Once stopped, elapsed wall time cannot drift camera/cursor past trim-out.
    QTest::qWait(60);
    QCOMPARE(preview.timeMs(), qreal(360));
}

QTEST_GUILESS_MAIN(PreviewControllerTest)
#include "PreviewControllerTest.moc"
