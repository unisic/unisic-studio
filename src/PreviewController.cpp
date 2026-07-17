#include "PreviewController.h"

#include "CursorPlayback.h"
#include "engine/KeyframeEngine.h"
#include "project/StudioProject.h"
#include "project/ZoomTimeline.h"
#include "render/CursorShapeProvider.h"

PreviewController::PreviewController(StudioProject *project, QObject *parent)
    : QObject(parent)
    , m_zoom(project->zoom())
    , m_projectId(QString::number(quintptr(project), 16))
    , m_durationMs(project->durationMs())
{
    m_cursor = new CursorPlayback(m_projectId, this);
    m_cursor->setTracks(project->cursorTrack(), project->clickTrack(), project->videoSize());
    CursorShapeProvider::registerShapes(m_projectId, project->cursorTrack().shapes());

    // A keyframe edit (drag, add, delete, regenerate) must refresh the frozen
    // preview even when time hasn't moved.
    connect(m_zoom, &ZoomTimeline::changed, this, [this] { recompute(); });

    m_timer.setInterval(16); // ~60 Hz smoothing
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &PreviewController::tick);

    recompute();
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
    const QRectF r = KeyframeEngine::evaluate(m_zoom->keyframes(), qint64(m_timeMs));
    if (r != m_zoomRect) {
        m_zoomRect = r;
        emit zoomRectChanged();
    }
    m_cursor->setTime(qint64(m_timeMs));
}
