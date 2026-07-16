#include "StudioApp.h"

#include "EditorWindowManager.h"
#include "RecentProjects.h"
#include "media/VideoProbe.h"
#include "project/StudioProject.h"

#include <QCoreApplication>
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
