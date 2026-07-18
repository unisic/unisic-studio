#pragma once
#include "engine/KeyframeEngine.h"

#include <QElapsedTimer>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QTimer>

class StudioProject;
class ZoomTimeline;
class CursorPlayback;

// The editor preview's playback head. It smooths a per-frame `timeMs` between the
// MediaPlayer's coarse position updates (QElapsedTimer, snapping on seek/pause),
// and from that ONE time it derives the two things the composition needs — the
// camera `zoomRect` (via the shared SpringCameraEvaluator) and the cursor
// overlay state (CursorPlayback). Both are C++ properties recomputed on the clock
// tick or on a keyframe edit, so QML never re-runs evaluate() per frame.
//
// One PreviewController is created per editor window (context property `preview`),
// parented to the StudioProject so it dies with the window. It registers the
// project's cursor bitmaps with CursorShapeProvider for its lifetime.
class PreviewController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(qreal timeMs READ timeMs NOTIFY timeMsChanged)
    Q_PROPERTY(QRectF zoomRect READ zoomRect NOTIFY zoomRectChanged)
    Q_PROPERTY(CursorPlayback *cursor READ cursor CONSTANT)

public:
    explicit PreviewController(StudioProject *project, QObject *parent = nullptr);
    ~PreviewController() override;

    qreal timeMs() const { return m_timeMs; }
    QRectF zoomRect() const { return m_zoomRect; }
    CursorPlayback *cursor() const { return m_cursor; }

    // MediaPlayer position / play-state changed: re-anchor the smoothing clock so
    // timeMs tracks true position while interpolating between coarse updates.
    Q_INVOKABLE void sync(qint64 positionMs, bool playing);
    // Seek or pause: snap exactly to positionMs and stop advancing.
    Q_INVOKABLE void snap(qint64 positionMs);

signals:
    void timeMsChanged();
    void zoomRectChanged();
    void playbackRangeEnded();

private:
    void tick();
    void setTimeMs(qreal t);
    void recompute();   // zoomRect + cursor at m_timeMs
    void resetCamera();

    ZoomTimeline *m_zoom = nullptr;
    StudioProject *m_project = nullptr;
    CursorPlayback *m_cursor = nullptr;
    QString m_projectId;
    // Live, not cached at construction: the async probe can deliver the real
    // duration AFTER the editor (and this controller) is built — a snapshot
    // would clamp the playhead to the stale value (0 for duration-less imports).
    qint64 durationMs() const;

    QTimer m_timer;
    QElapsedTimer m_elapsed;
    bool m_playing = false;
    qint64 m_anchorMs = 0;

    qreal m_timeMs = 0.0;
    QRectF m_zoomRect = QRectF(0, 0, 1, 1);
    SpringCameraEvaluator m_camera;
};
