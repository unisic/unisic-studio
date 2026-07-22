#include "TrajectoryMetrics.h"

#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace {

double zoomValue(const QRectF &rect)
{
    return -0.5 * (std::log(std::max(1.0e-9, rect.width()))
                   + std::log(std::max(1.0e-9, rect.height())));
}

double rectError(const QRectF &a, const QRectF &b)
{
    return std::max({std::fabs(a.x() - b.x()), std::fabs(a.y() - b.y()),
                     std::fabs(a.width() - b.width()), std::fabs(a.height() - b.height())});
}

bool sameTarget(const QRectF &a, const QRectF &b)
{
    return rectError(a, b) < 1.0e-12;
}

double projectionOvershoot(const QPointF &start, const QPointF &target,
                           const QPointF &value)
{
    const QPointF delta = target - start;
    const double length2 = QPointF::dotProduct(delta, delta);
    if (length2 < 1.0e-12)
        return 0.0;
    const double progress = QPointF::dotProduct(value - start, delta) / length2;
    return std::max(0.0, progress - 1.0);
}

} // namespace

TrajectoryMetrics analyzeTrajectory(const QVector<TrajectorySample> &samples,
                                    qint64 holdStartMs, qint64 holdEndMs,
                                    double settlingTolerance)
{
    TrajectoryMetrics metrics;
    if (samples.isEmpty()) {
        metrics.finite = false;
        metrics.bounded = false;
        return metrics;
    }

    QVector<QPointF> cameraVelocity(samples.size());
    QVector<double> zoomVelocity(samples.size());
    QVector<QPointF> cursorVelocity(samples.size());
    double holdVelocitySquares = 0.0;
    int holdVelocityCount = 0;
    bool haveHoldOrigin = false;
    QRectF holdOrigin;

    for (int i = 0; i < samples.size(); ++i) {
        const TrajectorySample &sample = samples.at(i);
        const QRectF &rect = sample.cameraRect;
        const bool sampleFinite = std::isfinite(rect.x()) && std::isfinite(rect.y())
                                  && std::isfinite(rect.width())
                                  && std::isfinite(rect.height())
                                  && std::isfinite(sample.cursor.x())
                                  && std::isfinite(sample.cursor.y())
                                  && std::isfinite(sample.cameraTarget.x())
                                  && std::isfinite(sample.cameraTarget.y())
                                  && std::isfinite(sample.cameraTarget.width())
                                  && std::isfinite(sample.cameraTarget.height())
                                  && sample.cameraTarget.width() > 0.0
                                  && sample.cameraTarget.height() > 0.0;
        metrics.finite = metrics.finite && sampleFinite;
        metrics.bounded = metrics.bounded && rect.width() > 0.0 && rect.height() > 0.0
                          && rect.x() >= -1.0e-9 && rect.y() >= -1.0e-9
                          && rect.right() <= 1.0 + 1.0e-9
                          && rect.bottom() <= 1.0 + 1.0e-9;

        if (holdStartMs >= 0 && sample.tMs >= holdStartMs && sample.tMs <= holdEndMs) {
            if (!haveHoldOrigin) {
                holdOrigin = rect;
                haveHoldOrigin = true;
            }
            metrics.holdDrift = std::max(metrics.holdDrift, rectError(rect, holdOrigin));
        }
        if (i == 0)
            continue;

        const TrajectorySample &previous = samples.at(i - 1);
        const double dt = double(sample.tMs - previous.tMs) / 1000.0;
        if (dt <= 0.0)
            continue;
        metrics.maxCameraEdgeJump = std::max(metrics.maxCameraEdgeJump,
                                             rectError(rect, previous.cameraRect));
        cameraVelocity[i] = (rect.center() - previous.cameraRect.center()) / dt;
        zoomVelocity[i] = (zoomValue(rect) - zoomValue(previous.cameraRect)) / dt;
        metrics.maxCameraCenterVelocity =
            std::max(metrics.maxCameraCenterVelocity,
                     std::hypot(cameraVelocity.at(i).x(), cameraVelocity.at(i).y()));
        metrics.maxZoomVelocity = std::max(metrics.maxZoomVelocity,
                                           std::fabs(zoomVelocity.at(i)));

        if (sample.cursorVisible && previous.cursorVisible) {
            cursorVelocity[i] = (sample.cursor - previous.cursor) / dt;
            metrics.maxCursorVelocity =
                std::max(metrics.maxCursorVelocity,
                         std::hypot(cursorVelocity.at(i).x(), cursorVelocity.at(i).y()));
        }

        if (i > 1) {
            metrics.maxCameraCenterAcceleration =
                std::max(metrics.maxCameraCenterAcceleration,
                         std::hypot(cameraVelocity.at(i).x() - cameraVelocity.at(i - 1).x(),
                                    cameraVelocity.at(i).y() - cameraVelocity.at(i - 1).y())
                             / dt);
            metrics.maxZoomAcceleration =
                std::max(metrics.maxZoomAcceleration,
                         std::fabs(zoomVelocity.at(i) - zoomVelocity.at(i - 1)) / dt);
            if (sample.cursorVisible && previous.cursorVisible
                && samples.at(i - 2).cursorVisible) {
                metrics.maxCursorAcceleration =
                    std::max(metrics.maxCursorAcceleration,
                             std::hypot(cursorVelocity.at(i).x() - cursorVelocity.at(i - 1).x(),
                                        cursorVelocity.at(i).y() - cursorVelocity.at(i - 1).y())
                                 / dt);
            }
        }

        if (holdStartMs >= 0 && sample.tMs >= holdStartMs && sample.tMs <= holdEndMs) {
            const double combined = QPointF::dotProduct(cameraVelocity.at(i), cameraVelocity.at(i))
                                    + zoomVelocity.at(i) * zoomVelocity.at(i);
            holdVelocitySquares += combined;
            ++holdVelocityCount;
        }
    }
    metrics.holdRmsVelocity = holdVelocityCount > 0
                                  ? std::sqrt(holdVelocitySquares / holdVelocityCount)
                                  : 0.0;

    // Analyze each constant-target segment independently. Settling requires a
    // full 150 ms tolerance window, avoiding a false "settled" result at the
    // spring's first target crossing.
    int segmentStart = 0;
    while (segmentStart < samples.size()) {
        int segmentEnd = segmentStart + 1;
        while (segmentEnd < samples.size()
               && sameTarget(samples.at(segmentEnd).cameraTarget,
                             samples.at(segmentStart).cameraTarget))
            ++segmentEnd;

        const QRectF target = samples.at(segmentStart).cameraTarget;
        const QRectF startRect = segmentStart > 0 ? samples.at(segmentStart - 1).cameraRect
                                                   : samples.at(segmentStart).cameraRect;
        const QPointF startCenter = startRect.center();
        const QPointF targetCenter = target.center();
        const double startZoom = zoomValue(startRect);
        const double targetZoom = zoomValue(target);
        bool settled = false;
        const bool assessable = samples.at(segmentEnd - 1).tMs
                                - samples.at(segmentStart).tMs >= 150;
        for (int i = segmentStart; i < segmentEnd; ++i) {
            metrics.maxCenterOvershootRatio =
                std::max(metrics.maxCenterOvershootRatio,
                         projectionOvershoot(startCenter, targetCenter,
                                             samples.at(i).cameraRect.center()));
            const double zoomDelta = targetZoom - startZoom;
            if (std::fabs(zoomDelta) > 1.0e-9) {
                const double progress = (zoomValue(samples.at(i).cameraRect) - startZoom) / zoomDelta;
                metrics.maxZoomOvershootRatio =
                    std::max(metrics.maxZoomOvershootRatio, std::max(0.0, progress - 1.0));
            }
        }

        for (int i = segmentStart; i < segmentEnd; ++i) {
            if (samples.at(segmentEnd - 1).tMs - samples.at(i).tMs < 150)
                break;
            bool stable = true;
            for (int j = i; j < segmentEnd && samples.at(j).tMs - samples.at(i).tMs <= 150; ++j) {
                if (rectError(samples.at(j).cameraRect, target) > settlingTolerance) {
                    stable = false;
                    break;
                }
            }
            if (stable) {
                metrics.maxSettlingMs =
                    std::max(metrics.maxSettlingMs,
                             samples.at(i).tMs - samples.at(segmentStart).tMs);
                settled = true;
                break;
            }
        }
        if (assessable) {
            if (settled)
                ++metrics.settledSegments;
            else
                ++metrics.unsettledSegments;
        }
        segmentStart = segmentEnd;
    }

    return metrics;
}

bool writeTrajectoryCsv(const QString &path, const QVector<TrajectorySample> &samples)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream stream(&file);
    stream.setRealNumberNotation(QTextStream::FixedNotation);
    stream.setRealNumberPrecision(9);
    stream << "t_ms,target_x,target_y,target_w,target_h,x,y,w,h,cursor_x,cursor_y,cursor_visible\n";
    for (const TrajectorySample &sample : samples) {
        stream << sample.tMs << ',' << sample.cameraTarget.x() << ',' << sample.cameraTarget.y()
               << ',' << sample.cameraTarget.width() << ',' << sample.cameraTarget.height() << ','
               << sample.cameraRect.x() << ',' << sample.cameraRect.y() << ','
               << sample.cameraRect.width() << ',' << sample.cameraRect.height() << ','
               << sample.cursor.x() << ',' << sample.cursor.y() << ','
               << (sample.cursorVisible ? 1 : 0) << '\n';
    }
    return file.commit();
}
