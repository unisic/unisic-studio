#include "StudioApp.h"

#include "EditorWindowManager.h"
#include "RecentProjects.h"
#include "capture/InputPermission.h"
#include "capture/StudioRecorder.h"
#include "media/VideoProbe.h"
#include "project/StudioProject.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QStandardPaths>
#include <QUrl>

StudioApp::StudioApp(QObject *parent)
    : QObject(parent)
    , m_settings(new StudioSettings(this))
    , m_recent(new RecentProjects(this))
    , m_editors(new EditorWindowManager(this))
{
    connect(m_recent, &RecentProjects::changed, this, &StudioApp::recentProjectsChanged);

    // Countdown pre-roll owned by the facade (the recorder only arms/commits — the
    // portal dialog and this countdown must never land in the file).
    m_countdownTimer.setInterval(1000);
    m_countdownTimer.setTimerType(Qt::CoarseTimer);
    connect(&m_countdownTimer, &QTimer::timeout, this, [this] {
        if (--m_recorderCountdown > 0) {
            emit recorderCountdownChanged();
            return;
        }
        m_countdownTimer.stop();
        emit recorderCountdownChanged();          // now 0
        if (m_recorder)
            m_recorder->commit();
    });
}

QString StudioApp::version() const
{
    return QStringLiteral(STUDIO_VERSION);
}

bool StudioApp::devBuild() const
{
#ifdef UNISIC_DEV_BUILD
    return true;
#else
    return false;
#endif
}

bool StudioApp::capVideoPlayback() const
{
    // The preview imports QtMultimedia purely from QML (no C++ link), so the
    // capability is just "is the module's runtime plugin installed in the QML
    // import path" — qt6-qtmultimedia ships it even without its -devel package.
    // Cached: the answer can't change within a run. (Mirrors Unisic.)
    static const bool ok = QFileInfo::exists(
        QLibraryInfo::path(QLibraryInfo::QmlImportsPath)
        + QStringLiteral("/QtMultimedia/qmldir"));
    return ok;
}

QVariantList StudioApp::recentProjects() const
{
    return m_recent->list();
}

void StudioApp::setEngine(QQmlEngine *engine)
{
    m_editors->setEngine(engine);
}

void StudioApp::quit()
{
    QCoreApplication::quit();
}

void StudioApp::importVideo()
{
    // Native picker (NOT a QML Dialog: under Basic style that falls back to the
    // ugly Quick-styled dialog — the platform/portal dialog is the right one).
    const QString start = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString path = QFileDialog::getOpenFileName(
        nullptr, tr("Import Video"), start.isEmpty() ? QDir::homePath() : start,
        tr("Videos (*.mp4 *.webm *.mkv *.mov)"));
    if (path.isEmpty())
        return; // cancelled
    importFile(path);
}

void StudioApp::importFile(const QString &path)
{
    QString file = path;
    if (file.startsWith(QStringLiteral("file:")))
        file = QUrl(file).toLocalFile();
    if (file.isEmpty() || !QFileInfo::exists(file)) {
        emit notified(tr("File not found: %1").arg(file), true);
        return;
    }
    const QString abs = QFileInfo(file).absoluteFilePath();

    // Async probe; the probe is parented to `this` and self-deletes when done.
    auto *probe = new VideoProbe(this);
    connect(probe, &VideoProbe::probed, this,
            [this, probe, abs](qint64 durationMs, double fps, const QSize &size) {
                auto *p = new StudioProject;
                p->setVideoAbsPath(abs);
                p->setVideoRelPath(QFileInfo(abs).fileName()); // finalized at save
                p->setDurationMs(durationMs);
                p->setFps(fps);
                p->setVideoSize(size);
                p->setVideoHash(StudioProject::computeVideoHash(abs));
                qInfo("import: %s (%lldms, %.3ffps, %dx%d)",
                      qPrintable(QFileInfo(abs).fileName()),
                      static_cast<long long>(durationMs), fps, size.width(), size.height());
                if (m_editors->openEditor(p, !capVideoPlayback()))
                    emit imported(p);
                else
                    emit notified(tr("Could not open the editor window."), true);
                probe->deleteLater();
            });
    connect(probe, &VideoProbe::failed, this, [this, probe](const QString &reason) {
        emit notified(reason, true);
        probe->deleteLater();
    });
    probe->probe(abs);
}

bool StudioApp::openProject(const QString &pathOrUrl)
{
    QString path = pathOrUrl;
    if (path.startsWith(QStringLiteral("file:")))
        path = QUrl(path).toLocalFile();
    const QString abs = QFileInfo(path).absoluteFilePath();

    QString err;
    StudioProject *p = StudioProject::load(abs, &err);
    if (!p) {
        emit notified(err.isEmpty() ? tr("Could not open project") : err, true);
        return false;
    }
    // Remember where it came from so subsequent saves are silent.
    p->setProperty("_sourcePath", abs);
    m_recent->recordOpened(abs, QFileInfo(abs).completeBaseName(), p->durationMs());
    if (!m_editors->openEditor(p, !capVideoPlayback())) {
        emit notified(tr("Could not open the editor window."), true);
        return false;
    }
    return true;
}

bool StudioApp::saveProject(StudioProject *project)
{
    if (!project)
        return false;
    QString path = project->property("_sourcePath").toString();
    if (path.isEmpty())
        path = defaultSavePath(project); // first save → default projects dir
    if (path.isEmpty())
        return false;
    return doSave(project, path);
}

bool StudioApp::saveProjectAs(StudioProject *project)
{
    if (!project)
        return false;
    QString start = project->property("_sourcePath").toString();
    if (start.isEmpty())
        start = defaultSavePath(project);
    const QString path = QFileDialog::getSaveFileName(
        nullptr, tr("Save Project As"), start,
        tr("Unisic Studio Project (*.%1)").arg(StudioProject::projectExtension()));
    if (path.isEmpty())
        return false;
    return doSave(project, path);
}

QString StudioApp::defaultSavePath(StudioProject *project) const
{
    QDir dir(m_settings->projectsDirectory());
    if (!dir.exists())
        dir.mkpath(QStringLiteral("."));
    QString base = QFileInfo(project->videoAbsPath()).completeBaseName();
    if (base.isEmpty())
        base = tr("Untitled");
    const QString ext = StudioProject::projectExtension();
    QString candidate = dir.filePath(base + QLatin1Char('.') + ext);
    // Don't clobber an unrelated existing file on a first save.
    int n = 2;
    while (QFileInfo::exists(candidate))
        candidate = dir.filePath(QStringLiteral("%1-%2.%3").arg(base).arg(n++).arg(ext));
    return candidate;
}

bool StudioApp::doSave(StudioProject *project, const QString &path)
{
    const QString abs = QFileInfo(path).absoluteFilePath();
    const QDir dir = QFileInfo(abs).absoluteDir();
    // Finalize the portable relative path now that the project's home is known
    // (load() prefers relPath so project+video move together as a bundle).
    if (!project->videoAbsPath().isEmpty())
        project->setVideoRelPath(dir.relativeFilePath(project->videoAbsPath()));

    QString err;
    if (!project->save(abs, &err)) {
        emit notified(err, true);
        return false;
    }
    project->setProperty("_sourcePath", abs);
    m_recent->recordOpened(abs, QFileInfo(abs).completeBaseName(), project->durationMs());
    emit notified(tr("Project saved"), false);
    return true;
}

QString StudioApp::pickWallpaper(const QString &startDir)
{
    const QString start = startDir.isEmpty()
                              ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                              : startDir;
    return QFileDialog::getOpenFileName(
        nullptr, tr("Choose wallpaper image"), start.isEmpty() ? QDir::homePath() : start,
        tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
}

QString StudioApp::pickExportOutput(const QString &format, const QString &startPath)
{
    const QString ext = format.isEmpty() ? QStringLiteral("mp4") : format;
    QString start = startPath;
    if (start.startsWith(QStringLiteral("file:")))
        start = QUrl(start).toLocalFile();
    if (start.isEmpty())
        start = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (start.isEmpty())
        start = QDir::homePath();

    QString selected = QFileDialog::getSaveFileName(
        nullptr, tr("Export video"), start,
        tr("%1 video (*.%2)").arg(ext.toUpper(), ext));
    if (selected.isEmpty())
        return QString(); // cancelled
    // Enforce the format's suffix (the portal dialog may omit it).
    if (!selected.endsWith(QLatin1Char('.') + ext, Qt::CaseInsensitive))
        selected += QLatin1Char('.') + ext;
    return selected;
}

void StudioApp::revealInFolder(const QString &path)
{
    QString local = path;
    if (local.startsWith(QStringLiteral("file:")))
        local = QUrl(local).toLocalFile();
    const QString dir = QFileInfo(local).absolutePath();
    if (!dir.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

// --- recording ---------------------------------------------------------------

int StudioApp::recorderElapsed() const
{
    return m_recorder ? m_recorder->elapsedSeconds() : 0;
}

void StudioApp::setRecorderState(RecorderState s)
{
    if (m_recorderState == s)
        return;
    m_recorderState = s;
    emit recorderStateChanged();
}

void StudioApp::stopCountdown()
{
    m_countdownTimer.stop();
    if (m_recorderCountdown != 0) {
        m_recorderCountdown = 0;
        emit recorderCountdownChanged();
    }
}

void StudioApp::ensureRecorder()
{
    if (m_recorder)
        return;
    m_recorder = new StudioRecorder(m_settings, this);
    connect(m_recorder, &StudioRecorder::armed, this, &StudioApp::onRecorderArmed);
    connect(m_recorder, &StudioRecorder::started, this,
            [this] { setRecorderState(RecRecording); });
    connect(m_recorder, &StudioRecorder::paused, this,
            [this](bool p) { setRecorderState(p ? RecPaused : RecRecording); });
    connect(m_recorder, &StudioRecorder::elapsedChanged, this, &StudioApp::recorderElapsedChanged);
    connect(m_recorder, &StudioRecorder::failed, this, [this](const QString &e) {
        stopCountdown();
        setRecorderState(RecIdle);
        if (e != QLatin1String("cancelled"))
            emit notified(tr("Recording failed: %1").arg(e), true);
    });
    connect(m_recorder, &StudioRecorder::finished, this, [this](const QString &projectPath) {
        stopCountdown();
        setRecorderState(RecIdle);
        // Opens a fresh StudioProject in an editor window (the recorder's own copy
        // was a throwaway used only to write the sidecar).
        if (!openProject(projectPath))
            emit notified(tr("Recorded, but the project could not be opened: %1").arg(projectPath), true);
    });
}

void StudioApp::startRecording()
{
    ensureRecorder();
    if (m_recorderState != RecIdle || m_recorder->recording())
        return;
    setRecorderState(RecArming);
    m_recorder->start(/*holdForCommit=*/true);
}

void StudioApp::onRecorderArmed()
{
    const int secs = qBound(0, m_settings->recordCountdownSec(), 10);
    if (secs <= 0) {
        // No pre-roll: commit immediately (state stays Arming until started()).
        m_recorder->commit();
        return;
    }
    m_recorderCountdown = secs;
    setRecorderState(RecCountdown);
    emit recorderCountdownChanged();
    m_countdownTimer.start();
}

void StudioApp::stopRecording()
{
    if (!m_recorder)
        return;
    // A stop during arming/countdown (before any frame) is a cancel.
    if (m_recorderState == RecArming || m_recorderState == RecCountdown) {
        cancelRecording();
        return;
    }
    if (!m_recorder->recording())
        return;
    m_recorder->stop();
    setRecorderState(RecFinalizing);
}

void StudioApp::togglePauseRecording()
{
    if (m_recorder)
        m_recorder->togglePause();
}

void StudioApp::cancelRecording()
{
    stopCountdown();
    if (m_recorder)
        m_recorder->cancel();
    setRecorderState(RecIdle);
}

void StudioApp::refreshInputPermission()
{
    const int s = int(InputPermission::probe());
    if (s != m_inputPermission) {
        m_inputPermission = s;
        emit inputPermissionStatusChanged();
    }
}

QString StudioApp::inputPermissionFixHint() const
{
    return InputPermission::fixHint();
}
