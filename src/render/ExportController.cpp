#include "ExportController.h"

#include "RenderPipeline.h"
#include "project/StudioProject.h"
#include "project/StyleModel.h"

#include <QFileInfo>

ExportController::ExportController(QObject *parent)
    : QObject(parent)
{
}

ExportController::~ExportController() = default;

QString ExportController::extension() const
{
    if (m_format == QLatin1String("webm"))
        return QStringLiteral("webm");
    if (m_format == QLatin1String("gif"))
        return QStringLiteral("gif");
    return QStringLiteral("mp4");
}

void ExportController::setFormat(const QString &v)
{
    if (m_format == v)
        return;
    m_format = v;
    emit formatChanged();
}

void ExportController::setResolution(const QString &v)
{
    if (m_resolution == v)
        return;
    m_resolution = v;
    emit resolutionChanged();
}

void ExportController::setCustomWidth(int v)
{
    if (m_customWidth == v)
        return;
    m_customWidth = v;
    emit customWidthChanged();
}

void ExportController::setCustomHeight(int v)
{
    if (m_customHeight == v)
        return;
    m_customHeight = v;
    emit customHeightChanged();
}

void ExportController::setFpsMode(const QString &v)
{
    if (m_fpsMode == v)
        return;
    m_fpsMode = v;
    emit fpsModeChanged();
}

void ExportController::setQuality(int v)
{
    v = qBound(0, v, 100);
    if (m_quality == v)
        return;
    m_quality = v;
    emit qualityChanged();
}

void ExportController::setOutputPath(const QString &v)
{
    if (m_outputPath == v)
        return;
    m_outputPath = v;
    emit outputPathChanged();
}

void ExportController::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}

QString ExportController::suggestedOutputPath(StudioProject *project) const
{
    if (!project)
        return QString();
    const QString video = project->videoResolved().isEmpty() ? project->videoAbsPath()
                                                             : project->videoResolved();
    const QFileInfo fi(video);
    QString base = fi.completeBaseName();
    if (base.isEmpty())
        base = QStringLiteral("export");
    const QString dir = fi.absolutePath();
    return (dir.isEmpty() ? base : dir + QLatin1Char('/') + base) + QStringLiteral("-export.")
           + extension();
}

void ExportController::start(StudioProject *project)
{
    if (m_state == Running)
        return;
    m_error.clear();

    if (!project) {
        m_error = tr("No project to export.");
        setState(Error);
        return;
    }
    const QString master = project->videoResolved().isEmpty() ? project->videoAbsPath()
                                                             : project->videoResolved();
    if (master.isEmpty() || !QFileInfo::exists(master)) {
        m_error = tr("The source video could not be found.");
        setState(Error);
        return;
    }
    if (m_outputPath.isEmpty())
        m_outputPath = suggestedOutputPath(project);
    if (m_outputPath.isEmpty()) {
        m_error = tr("Choose a destination file.");
        setState(Error);
        return;
    }

    // --- Resolve dimensions from the resolution preset + the styled aspect so
    //     the export has no letterbox bars (except explicit custom sizes). ---
    StyleModel *style = project->style();
    const QSize src = project->videoSize();
    const double srcAspect =
        (src.height() > 0) ? double(src.width()) / src.height() : (16.0 / 9.0);
    double compAspect = srcAspect;
    const QString aspect = style ? style->aspect() : QStringLiteral("source");
    if (aspect == QLatin1String("16:9"))
        compAspect = 16.0 / 9.0;
    else if (aspect == QLatin1String("9:16"))
        compAspect = 9.0 / 16.0;
    else if (aspect == QLatin1String("1:1"))
        compAspect = 1.0;

    int outW = 0, outH = 0;
    if (m_resolution == QLatin1String("custom")) {
        outW = qMax(2, m_customWidth);
        outH = qMax(2, m_customHeight);
    } else {
        int targetH = src.height() > 0 ? src.height() : 1080;
        if (m_resolution == QLatin1String("1080p"))
            targetH = 1080;
        else if (m_resolution == QLatin1String("720p"))
            targetH = 720;
        outH = targetH;
        outW = int(qRound(targetH * compAspect));
    }

    // --- fps + trim + quality ---
    double fps = project->fps() > 0 ? project->fps() : 30.0;
    if (m_fpsMode == QLatin1String("30"))
        fps = 30.0;
    else if (m_fpsMode == QLatin1String("60"))
        fps = 60.0;
    // GIF: cap at 30 fps regardless of the chosen mode (60 fps gifs bloat wildly
    // for no perceptible gain and many viewers clamp them anyway).
    if (m_format == QLatin1String("gif"))
        fps = qMin(fps, 30.0);

    const qint64 trimIn = qMax<qint64>(0, project->trimInMs());
    const qint64 effOut =
        project->trimOutMs() > trimIn ? project->trimOutMs() : project->durationMs();
    const qint64 durMs = qMax<qint64>(1, effOut - trimIn);

    // quality 0..100 → crf 34..18 (higher quality = lower crf = bigger/better).
    const int crf = int(qRound(34.0 - m_quality / 100.0 * 16.0));

    RenderPipeline::Settings s;
    s.master = master;
    s.style = style;
    s.videoSize = src;
    s.trimInMs = trimIn;
    s.durMs = durMs;
    s.fps = fps;
    s.outW = outW;
    s.outH = outH;
    s.format = m_format;
    s.crf = crf;
    // GIF palette quality 0..2 from the same 0..100 slider (fast/small → best).
    s.gifQuality = m_quality < 34 ? 0 : (m_quality < 67 ? 1 : 2);
    s.preferHardware = false; // M1: software x264/vp9 by default (robust, tested)
    s.outputPath = m_outputPath;
    // Camera + cursor overlay: snapshot the live models so the offscreen render
    // is decoupled from any concurrent editing (copies are cheap/COW).
    s.keyframes = project->zoom()->keyframes();
    s.cursor = project->cursorTrack();
    s.clicks = project->clickTrack();
    s.projectId = QString::number(quintptr(project), 16);

    m_progress = 0.0;
    m_framesDone = 0;
    m_totalFrames = 0;
    m_etaMs = 0;
    emit progressChanged();

    delete m_pipeline; // any prior run's object
    m_pipeline = new RenderPipeline(this);
    connect(m_pipeline, &RenderPipeline::progress, this,
            [this](int done, int total, qint64 eta) {
                m_framesDone = done;
                m_totalFrames = total;
                m_etaMs = eta;
                m_progress = total > 0 ? double(done) / total : 0.0;
                emit progressChanged();
            });
    connect(m_pipeline, &RenderPipeline::finished, this, [this] { setState(Done); });
    connect(m_pipeline, &RenderPipeline::failed, this, [this](const QString &msg) {
        m_error = msg;
        setState(Error);
    });

    setState(Running);
    m_pipeline->start(s);
}

void ExportController::cancel()
{
    if (m_state != Running)
        return;
    if (m_pipeline)
        m_pipeline->cancel();
    setState(Cancelled);
}

void ExportController::reset()
{
    if (m_state == Running)
        return;
    m_error.clear();
    m_progress = 0.0;
    m_framesDone = 0;
    m_totalFrames = 0;
    m_etaMs = 0;
    emit progressChanged();
    setState(Idle);
}
