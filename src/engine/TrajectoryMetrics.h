#pragma once

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

struct TrajectorySample {
    qint64 tMs = 0;
    QRectF cameraRect;
    QRectF cameraTarget;
    QPointF cursor;
    bool cursorVisible = false;
};

struct TrajectoryMetrics {
    bool finite = true;
    bool bounded = true;
    double maxCameraEdgeJump = 0.0;
    double maxCameraCenterVelocity = 0.0;
    double maxCameraCenterAcceleration = 0.0;
    double maxZoomVelocity = 0.0;
    double maxZoomAcceleration = 0.0;
    double maxCursorVelocity = 0.0;
    double maxCursorAcceleration = 0.0;
    double maxCenterOvershootRatio = 0.0;
    double maxZoomOvershootRatio = 0.0;
    qint64 maxSettlingMs = 0;
    int settledSegments = 0;
    int unsettledSegments = 0;
    double holdDrift = 0.0;
    double holdRmsVelocity = 0.0;
};

TrajectoryMetrics analyzeTrajectory(const QVector<TrajectorySample> &samples,
                                    qint64 holdStartMs = -1,
                                    qint64 holdEndMs = -1,
                                    double settlingTolerance = 0.0015);
bool writeTrajectoryCsv(const QString &path, const QVector<TrajectorySample> &samples);
