#include "TrajectoryMetrics.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class TrajectoryMetricsTest : public QObject
{
    Q_OBJECT

private slots:
    void constantHoldHasNoJitter();
    void detectsVelocityOvershootAndBounds();
    void writesCsv();
};

void TrajectoryMetricsTest::constantHoldHasNoJitter()
{
    QVector<TrajectorySample> samples;
    const QRectF rect(0.2, 0.2, 0.5, 0.5);
    for (qint64 t = 0; t <= 1000; t += 20)
        samples.append({t, rect, rect, QPointF(0.5, 0.5), true});
    const TrajectoryMetrics metrics = analyzeTrajectory(samples, 200, 800);
    QVERIFY(metrics.finite);
    QVERIFY(metrics.bounded);
    QCOMPARE(metrics.maxCameraEdgeJump, 0.0);
    QCOMPARE(metrics.maxCameraCenterVelocity, 0.0);
    QCOMPARE(metrics.maxZoomVelocity, 0.0);
    QCOMPARE(metrics.holdDrift, 0.0);
    QCOMPARE(metrics.holdRmsVelocity, 0.0);
}

void TrajectoryMetricsTest::detectsVelocityOvershootAndBounds()
{
    QVector<TrajectorySample> samples;
    const QRectF start(0, 0, 1, 1);
    const QRectF target(0.35, 0.25, 0.5, 0.5);
    samples.append({0, start, target, QPointF(0.1, 0.1), true});
    samples.append({100, QRectF(0.21, 0.15, 0.7, 0.7), target,
                    QPointF(0.2, 0.1), true});
    samples.append({200, QRectF(0.38, 0.26, 0.48, 0.48), target,
                    QPointF(0.3, 0.1), true});
    const TrajectoryMetrics metrics = analyzeTrajectory(samples);
    QVERIFY(metrics.finite);
    QVERIFY(metrics.bounded);
    QVERIFY(metrics.maxCameraEdgeJump > 0.0);
    QVERIFY(metrics.maxCameraCenterVelocity > 0.0);
    QVERIFY(metrics.maxZoomVelocity > 0.0);
    QVERIFY(metrics.maxZoomOvershootRatio > 0.0);
    QVERIFY(metrics.maxCenterOvershootRatio > 0.0);
    QVERIFY(metrics.maxCursorVelocity > 0.0);

    samples.last().cameraRect = QRectF(-0.1, 0, 1, 1);
    QVERIFY(!analyzeTrajectory(samples).bounded);
}

void TrajectoryMetricsTest::writesCsv()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("trajectory.csv"));
    const QVector<TrajectorySample> samples{
        {0, QRectF(0, 0, 1, 1), QRectF(0, 0, 1, 1), QPointF(0.5, 0.5), true}};
    QVERIFY(writeTrajectoryCsv(path, samples));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray csv = file.readAll();
    QVERIFY(csv.startsWith("t_ms,target_x"));
    QVERIFY(csv.contains("0,0.000000000"));
}

QTEST_GUILESS_MAIN(TrajectoryMetricsTest)
#include "TrajectoryMetricsTest.moc"
