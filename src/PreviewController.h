#pragma once
#include "engine/KeyframeEngine.h"

#include <QElapsedTimer>
#include <QObject>
#include <QRectF>
#include <QString>

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
    // True between sync(playing=true) and pause/snap/range-end. Drives the QML
    // FrameAnimation that calls frameTick() once per RENDERED frame — vsync-
    // locked, so the camera advances exactly once per displayed frame. A fixed
    // 16 ms QTimer drifted in phase against the compositor's vsync (and under-
    // sampled >60 Hz displays outright), which read as juddery zooms.
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)

public:
    explicit PreviewController(StudioProject *project, QObject *parent = nullptr);
    ~PreviewController() override;

    qreal timeMs() const { return m_timeMs; }
    QRectF zoomRect() const { return m_zoomRect; }
    CursorPlayback *cursor() const { return m_cursor; }
    bool playing() const { return m_playing; }

    // MediaPlayer position / play-state changed: re-anchor the smoothing clock so
    // timeMs tracks true position while interpolating between coarse updates.
    Q_INVOKABLE void sync(qint64 positionMs, bool playing);
    // Seek or pause: snap exactly to positionMs and stop advancing.
    Q_INVOKABLE void snap(qint64 positionMs);
    // One render-frame step; called by the QML FrameAnimation while playing.
    Q_INVOKABLE void frameTick();

signals:
    void timeMsChanged();
    void zoomRectChanged();
    void playingChanged();
    void playbackRangeEnded();

private:
    void tick();
    void setPlaying(bool p);
    void setTimeMs(qreal t);
    void recompute();   // zoomRect + cursor at m_timeMs
    void resetCamera();

    ZoomTimeline *m_zoom = nullptr;
    StudioProject *m_project = nullptr;
    CursorPlayback *m_cursor = nullptr;
    QString m_projectId;
    qint64 m_durationMs = 0;

    QElapsedTimer m_elapsed;
    bool m_playing = false;
    qint64 m_anchorMs = 0;

    qreal m_timeMs = 0.0;
    QRectF m_zoomRect = QRectF(0, 0, 1, 1);
    SpringCameraEvaluator m_camera;
};
