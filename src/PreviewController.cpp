#include "PreviewController.h"

#include "CursorPlayback.h"
#include "project/StudioProject.h"
#include "project/ZoomTimeline.h"
#include "render/CursorShapeProvider.h"

PreviewController::PreviewController(StudioProject *project, QObject *parent)
    : QObject(parent)
    , m_zoom(project->zoom())
    , m_project(project)
    , m_projectId(QString::number(quintptr(project), 16))
    , m_durationMs(project->durationMs())
{
    m_cursor = new CursorPlayback(m_projectId, this);
    m_cursor->setTracks(project->cursorTrack(), project->clickTrack(), project->videoSize());
    CursorShapeProvider::registerShapes(m_projectId, project->cursorTrack().shapes());

    // A target or smoothness edit invalidates spring checkpoints. Rebuild the
    // local evaluator, then refresh the frozen frame even when time has not moved.
    connect(m_zoom, &QAbstractItemModel::modelReset, this, &PreviewController::resetCamera);
    connect(m_zoom, &QAbstractItemModel::rowsInserted, this,
            [this] { resetCamera(); });
    connect(m_zoom, &QAbstractItemModel::rowsRemoved, this,
            [this] { resetCamera(); });
    connect(m_zoom, &QAbstractItemModel::dataChanged, this,
            [this] { resetCamera(); });
    connect(m_zoom, &ZoomTimeline::motionSmoothnessChanged,
            this, &PreviewController::resetCamera);

    // No QTimer here: while playing, the QML FrameAnimation in the editor calls
    // frameTick() once per rendered frame, so camera/cursor updates are vsync-
    // locked (a fixed-interval timer beat against the compositor refresh and
    // showed up as juddery zooms; it also undersampled >60 Hz displays).
    resetCamera();
}

PreviewController::~PreviewController()
{
    CursorShapeProvider::releaseProject(m_projectId);
}

void PreviewController::sync(qint64 positionMs, bool playing)
{
    qint64 minimum = 0;
    qint64 maximum = qMax<qint64>(0, m_durationMs);
    if (playing && m_project && m_project->trimOutMs() > m_project->trimInMs()) {
        minimum = qBound<qint64>(0, m_project->trimInMs(), maximum);
        maximum = qBound<qint64>(minimum, m_project->trimOutMs(), maximum);
    }
    m_anchorMs = qBound(minimum, positionMs, maximum);
    m_elapsed.restart();
    setPlaying(playing);
    setTimeMs(m_anchorMs);
}

void PreviewController::snap(qint64 positionMs)
{
    setPlaying(false);
    m_anchorMs = qBound<qint64>(0, positionMs, qMax<qint64>(0, m_durationMs));
    setTimeMs(m_anchorMs);
}

void PreviewController::frameTick()
{
    tick();
}

void PreviewController::setPlaying(bool p)
{
    if (m_playing == p)
        return;
    m_playing = p;
    emit playingChanged();
}

void PreviewController::tick()
{
    if (!m_playing)
        return;
    qreal t = qreal(m_anchorMs) + qreal(m_elapsed.elapsed());
    qint64 playbackEnd = m_durationMs;
    if (m_project && m_project->trimOutMs() > m_project->trimInMs())
        playbackEnd = qMin(playbackEnd, m_project->trimOutMs());
    if (playbackEnd > 0 && t >= playbackEnd) {
        setPlaying(false);
        setTimeMs(playbackEnd);
        emit playbackRangeEnded();
        return;
    }
    if (m_durationMs > 0 && t > m_durationMs)
        t = m_durationMs;
    setTimeMs(t);
}

void PreviewController::setTimeMs(qreal t)
{
    if (qFuzzyCompare(m_timeMs, t))
        return;
    m_timeMs = t;
    emit timeMsChanged();
    recompute();
}

void PreviewController::recompute()
{
    const QRectF r = m_camera.evaluate(qint64(m_timeMs));
    if (r != m_zoomRect) {
        m_zoomRect = r;
        emit zoomRectChanged();
    }
    m_cursor->setTime(qint64(m_timeMs));
}

void PreviewController::resetCamera()
{
    m_camera.setTimeline(m_zoom->keyframes(), m_zoom->motionSmoothness());
    recompute();
}
