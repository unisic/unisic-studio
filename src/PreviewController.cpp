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

    m_timer.setInterval(16); // ~60 Hz smoothing
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &PreviewController::tick);

    resetCamera();
}

PreviewController::~PreviewController()
{
    CursorShapeProvider::releaseProject(m_projectId);
}

void PreviewController::sync(qint64 positionMs, bool playing)
{
    m_anchorMs = qBound<qint64>(0, positionMs, qMax<qint64>(0, m_durationMs));
    m_elapsed.restart();
    m_playing = playing;
    if (playing) {
        if (!m_timer.isActive())
            m_timer.start();
    } else {
        m_timer.stop();
    }
    setTimeMs(m_anchorMs);
}

void PreviewController::snap(qint64 positionMs)
{
    m_playing = false;
    m_timer.stop();
    m_anchorMs = qBound<qint64>(0, positionMs, qMax<qint64>(0, m_durationMs));
    setTimeMs(m_anchorMs);
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
        m_playing = false;
        m_timer.stop();
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
