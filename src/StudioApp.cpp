#include "StudioApp.h"

#include "AutozoomSelfTest.h"
#include "EditorWindowManager.h"
#include "HudManager.h"
#include "RecentProjects.h"
#include "capture/InputPermission.h"
#include "capture/StudioRecorder.h"
#include "engine/KeyframeEngine.h"
#include "media/VideoProbe.h"
#include "project/StudioProject.h"
#include "render/ExportController.h"
#include "render/FrameDecoder.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QPointF>
#include <QProcess>
#include <QRectF>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

StudioApp::StudioApp(QObject *parent)
    : QObject(parent)
    , m_settings(new StudioSettings(this))
    , m_recent(new RecentProjects(this))
    , m_editors(new EditorWindowManager(this))
    , m_hud(new HudManager(this, this))
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
    m_hud->setEngine(engine);
}

bool StudioApp::devShowRecordingHud()
{
    return m_hud->showHud();
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
    // Seed the auto-camera for a recording that carries a cursor track but no
    // zoom yet (a just-recorded project, or an older file predating the engine).
    generateZoom(p, /*onlyIfEmpty=*/true);
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

QString StudioApp::pickProjectsDirectory(const QString &startDir)
{
    QString start = startDir;
    if (start.startsWith(QStringLiteral("file:")))
        start = QUrl(start).toLocalFile();
    if (start.isEmpty() || !QFileInfo::exists(start))
        start = m_settings->projectsDirectory();
    if (start.isEmpty() || !QFileInfo::exists(start))
        start = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (start.isEmpty())
        start = QDir::homePath();
    // Native portal dialog (not a QML Dialog: Basic style falls back to an ugly one).
    return QFileDialog::getExistingDirectory(nullptr, tr("Choose projects folder"), start);
}

void StudioApp::copyToClipboard(const QString &text)
{
    if (QClipboard *cb = QGuiApplication::clipboard())
        cb->setText(text);
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
    // Probe click-capture once here (cheap, not on a timer) so the HUD's
    // click-capture status dot reflects real permission for this session.
    if (m_settings->clickCaptureEnabled())
        refreshInputPermission();
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

bool StudioApp::webcamDeviceAvailable(const QString &device) const
{
    return !device.isEmpty() && QFileInfo::exists(device);
}

// --- auto-zoom (M3) ----------------------------------------------------------

void StudioApp::generateZoom(StudioProject *project, bool onlyIfEmpty)
{
    if (!project)
        return;
    ZoomTimeline *zoom = project->zoom();
    if (project->cursorTrack().isEmpty())
        return;                                   // nothing to derive a camera from
    if (onlyIfEmpty && !zoom->keyframes().isEmpty())
        return;                                   // already has a timeline

    const KeyframeEngine::Params params = KeyframeEngine::Params::fromJson(zoom->autoParams());

    // Pinned = the survivors clearAuto() keeps (Manual or locked) so the engine
    // routes its auto spans around the user's own work.
    QVector<KeyframeEngine::Keyframe> pinned;
    for (const auto &kf : zoom->keyframes())
        if (kf.source == ZoomTimeline::Manual || kf.locked)
            pinned.append(kf);

    const QString aspect =
        project->style() ? project->style()->aspect() : QStringLiteral("source");

    QElapsedTimer timer;
    timer.start();
    const QVector<KeyframeEngine::Keyframe> kfs =
        KeyframeEngine::generate(project->cursorTrack(), project->clickTrack(),
                                 project->videoSize(), project->durationMs(), aspect, params,
                                 pinned);
    const double genMs = timer.nsecsElapsed() / 1.0e6;

    zoom->clearAuto();                            // drop old autos, keep Manual + locked
    zoom->addKeyframes(kfs);                       // bulk sorted insert — one changed()
    zoom->setAutoParams(params.toJson());         // persist the params used

    qInfo("autozoom: %lld keyframes from %lld cursor samples in %.2f ms",
          static_cast<long long>(kfs.size()),
          static_cast<long long>(project->cursorTrack().count()), genMs);
}

void StudioApp::regenerateZoom(StudioProject *project)
{
    generateZoom(project, /*onlyIfEmpty=*/false);
}

int StudioApp::addManualZoom(StudioProject *project, qint64 tMs, double cx, double cy, double zoom)
{
    if (!project)
        return -1;
    const QString aspect =
        project->style() ? project->style()->aspect() : QStringLiteral("source");
    ZoomTimeline::Keyframe kf;
    kf.tMs = qBound<qint64>(0, tMs, qMax<qint64>(0, project->durationMs()));
    kf.rect = KeyframeEngine::cameraRect(project->videoSize(), aspect, QPointF(cx, cy), zoom);
    kf.easeInMs = 650;
    kf.easeOutMs = 900;
    kf.source = ZoomTimeline::Manual;
    return project->zoom()->addKeyframe(kf);
}

int StudioApp::addResetZoom(StudioProject *project, qint64 tMs)
{
    if (!project)
        return -1;
    ZoomTimeline::Keyframe kf;
    kf.tMs = qBound<qint64>(0, tMs, qMax<qint64>(0, project->durationMs()));
    kf.rect = QRectF(0, 0, 1, 1);
    kf.easeInMs = 650;
    kf.easeOutMs = 900;
    kf.source = ZoomTimeline::Manual;
    return project->zoom()->addKeyframe(kf);
}

void StudioApp::setZoomFactor(StudioProject *project, int index, double zoom)
{
    if (!project)
        return;
    const QVariantMap m = project->zoom()->keyframeAt(index);
    if (m.isEmpty())
        return;
    const QRectF cur(m.value(QStringLiteral("x")).toDouble(), m.value(QStringLiteral("y")).toDouble(),
                     m.value(QStringLiteral("w")).toDouble(), m.value(QStringLiteral("h")).toDouble());
    const QString aspect =
        project->style() ? project->style()->aspect() : QStringLiteral("source");
    const QRectF rect =
        KeyframeEngine::cameraRect(project->videoSize(), aspect, cur.center(), zoom);
    project->zoom()->setKeyframeRect(index, rect);
}

void StudioApp::nudgeZoom(StudioProject *project, int index, double dxFrac, double dyFrac)
{
    if (!project)
        return;
    const QVariantMap m = project->zoom()->keyframeAt(index);
    if (m.isEmpty())
        return;
    const double w = m.value(QStringLiteral("w")).toDouble();
    const double h = m.value(QStringLiteral("h")).toDouble();
    double x = m.value(QStringLiteral("x")).toDouble() + dxFrac;
    double y = m.value(QStringLiteral("y")).toDouble() + dyFrac;
    x = std::clamp(x, 0.0, std::max(0.0, 1.0 - w));
    y = std::clamp(y, 0.0, std::max(0.0, 1.0 - h));
    project->zoom()->setKeyframeRect(index, QRectF(x, y, w, h));
}

double StudioApp::zoomFactorOf(StudioProject *project, int index)
{
    if (!project)
        return 1.0;
    const QVariantMap m = project->zoom()->keyframeAt(index);
    if (m.isEmpty())
        return 1.0;
    const QRectF cur(m.value(QStringLiteral("x")).toDouble(), m.value(QStringLiteral("y")).toDouble(),
                     m.value(QStringLiteral("w")).toDouble(), m.value(QStringLiteral("h")).toDouble());
    const QString aspect =
        project->style() ? project->style()->aspect() : QStringLiteral("source");
    return KeyframeEngine::zoomOfRect(project->videoSize(), aspect, cur);
}

// --- developer aids (dev builds only) ----------------------------------------

QString StudioApp::devMakeTestVideo()
{
    const QString exe = FrameDecoder::ffmpegPath();
    if (exe.isEmpty())
        return QString();
    if (m_smokeDir.isEmpty())
        m_smokeDir = QDir(QDir::tempPath()).filePath(QStringLiteral("unisic-studio-smoke"));
    QDir().mkpath(m_smokeDir);
    const QString path = m_smokeDir + QStringLiteral("/fixture.mp4");
    QProcess p;
    p.start(exe, {QStringLiteral("-y"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                  QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("testsrc2=size=640x360:rate=30:duration=1.2"),
                  QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("sine=frequency=440:duration=1.2"), QStringLiteral("-c:v"),
                  QStringLiteral("libx264"), QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
                  QStringLiteral("-c:a"), QStringLiteral("aac"), QStringLiteral("-shortest"), path});
    if (!p.waitForFinished(20000) || p.exitCode() != 0 || !QFileInfo::exists(path))
        return QString();
    return path;
}

void StudioApp::smokeLog(const QString &line)
{
    m_smokeLog += line + QLatin1Char('\n');
    emit smokeTestChanged();
}

void StudioApp::smokeNext()
{
    if (m_smokeIdx >= m_smokeSteps.size()) {
        m_smokeRunning = false;
        // Token-count the transcript (each step line carries exactly one token).
        const int pass = m_smokeLog.count(QStringLiteral("PASS"));
        const int fail = m_smokeLog.count(QStringLiteral("FAIL"));
        const int skip = m_smokeLog.count(QStringLiteral("SKIP"));
        smokeLog(tr("=== done: %1 PASS, %2 FAIL, %3 SKIP%4 ===")
                     .arg(pass)
                     .arg(fail)
                     .arg(skip)
                     .arg(fail > 0 ? tr(" — FAILURES PRESENT") : QString()));
        m_smokeSteps.clear();
        emit smokeTestChanged();
        return;
    }
    m_smokeSteps[m_smokeIdx++]();
}

void StudioApp::runSmokeExport(const QString &format, const QString &label)
{
    auto *p = new StudioProject(this);
    p->setVideoAbsPath(m_smokeDir + QStringLiteral("/fixture.mp4"));
    p->setDurationMs(1200);
    p->setFps(30.0);
    p->setVideoSize(QSize(640, 360));
    p->style()->setBackgroundType(QStringLiteral("gradient"));
    auto *ex = new ExportController(this);
    ex->setFormat(format);
    ex->setResolution(QStringLiteral("custom"));
    ex->setCustomWidth(640);
    ex->setCustomHeight(360);
    ex->setFpsMode(QStringLiteral("30"));
    const QString out = m_smokeDir + QStringLiteral("/smoke.") + ex->extension();
    ex->setOutputPath(out);
    connect(ex, &ExportController::stateChanged, this, [this, ex, p, label, out] {
        if (ex->state() == ExportController::Done)
            smokeLog(tr("%1: PASS (%2 KB)").arg(label).arg(QFileInfo(out).size() / 1024));
        else if (ex->state() == ExportController::Error)
            smokeLog(tr("%1: FAIL (%2)").arg(label, ex->errorString()));
        else
            return; // Running / intermediate — wait for a terminal state
        ex->deleteLater();
        p->deleteLater();
        smokeNext();
    });
    ex->start(p);
}

void StudioApp::runSmokeTest()
{
    if (!devBuild() || m_smokeRunning)
        return;
    m_smokeRunning = true;
    m_smokeLog.clear();
    m_smokeIdx = 0;
    m_smokeSteps.clear();
    smokeLog(tr("=== Unisic Studio smoke test ==="));

    const QString fixture = devMakeTestVideo();
    if (fixture.isEmpty()) {
        smokeLog(tr("fixture video: FAIL (could not generate a test clip — is ffmpeg installed?)"));
        smokeLog(tr("=== smoke test aborted ==="));
        m_smokeRunning = false;
        emit smokeTestChanged();
        return;
    }
    smokeLog(tr("fixture video: PASS (%1)").arg(QFileInfo(fixture).fileName()));

    // Step 1 — project save/reload roundtrip.
    m_smokeSteps.append([this, fixture] {
        StudioProject p;
        p.setVideoAbsPath(fixture);
        p.setDurationMs(1200);
        p.setFps(30.0);
        p.setVideoSize(QSize(640, 360));
        p.style()->setPaddingPct(10);
        const QString path = m_smokeDir + QStringLiteral("/roundtrip.unisicstudio");
        QString err;
        bool ok = p.save(path, &err);
        if (ok) {
            QScopedPointer<StudioProject> r(StudioProject::load(path, &err));
            ok = !r.isNull() && r->durationMs() == 1200 && r->videoSize() == QSize(640, 360)
                 && qFuzzyCompare(r->fps(), 30.0) && qFuzzyCompare(r->style()->paddingPct(), 10.0);
        }
        smokeLog(ok ? tr("project roundtrip: PASS")
                    : tr("project roundtrip: FAIL (%1)").arg(err));
        smokeNext();
    });

    // Step 2 — auto-zoom engine generate on synthetic tracks.
    m_smokeSteps.append([this] {
        StudioProject p;
        p.setVideoSize(QSize(640, 360));
        p.setDurationMs(2000);
        p.setFps(30.0);
        CursorTrack ct;
        for (int t = 0; t <= 2000; t += 40) {
            CursorSample s;
            s.tMs = t;
            s.x = (t < 500) ? 200 + t * 0.2 : 300; // approach (300,180), then dwell
            s.y = 180;
            ct.append(s);
        }
        p.setCursorTrack(ct);
        ClickTrack clk;
        ClickEvent down;
        down.tMs = 520;
        down.button = 1;
        down.state = ClickEvent::Down;
        down.x = 300;
        down.y = 180;
        ClickEvent up = down;
        up.tMs = 560;
        up.state = ClickEvent::Up;
        clk.append(down);
        clk.append(up);
        p.setClickTrack(clk);
        generateZoom(&p, /*onlyIfEmpty=*/false);
        const int n = p.zoom()->keyframes().size();
        smokeLog(n > 0 ? tr("engine generate: PASS (%1 keyframes)").arg(n)
                       : tr("engine generate: FAIL (no keyframes from a click cluster)"));
        smokeNext();
    });

    // Step 3 — VideoProbe on the fixture (async).
    m_smokeSteps.append([this, fixture] {
        auto *probe = new VideoProbe(this);
        connect(probe, &VideoProbe::probed, this,
                [this, probe](qint64 dur, double fps, const QSize &sz) {
                    const bool ok = dur >= 1000 && dur <= 1600 && sz == QSize(640, 360) && fps > 0;
                    smokeLog(ok ? tr("video probe: PASS (%1 ms, %2x%3, %4 fps)")
                                      .arg(dur)
                                      .arg(sz.width())
                                      .arg(sz.height())
                                      .arg(fps, 0, 'f', 1)
                                : tr("video probe: FAIL (%1 ms, %2x%3)")
                                      .arg(dur)
                                      .arg(sz.width())
                                      .arg(sz.height()));
                    probe->deleteLater();
                    smokeNext();
                });
        connect(probe, &VideoProbe::failed, this, [this, probe](const QString &r) {
            smokeLog(tr("video probe: FAIL (%1)").arg(r));
            probe->deleteLater();
            smokeNext();
        });
        probe->probe(fixture);
    });

    // Step 4 — offscreen render + MP4 export (proves the QQuickRenderControl path).
    m_smokeSteps.append(
        [this] { runSmokeExport(QStringLiteral("mp4"), tr("offscreen render + MP4 export")); });
    // Step 5 — GIF two-pass palette export.
    m_smokeSteps.append([this] { runSmokeExport(QStringLiteral("gif"), tr("GIF export")); });

    // Step 6 — recorder arm + cancel WITHOUT committing. An ISOLATED recorder (not
    // the app's, whose armed() drives a countdown → commit): arm, and on armed()
    // cancel before any frame is encoded. Skipped when no desktop portal is on the
    // bus; a 3 s fallback skips when the portal needs interactive consent.
    m_smokeSteps.append([this] {
        auto *iface = QDBusConnection::sessionBus().interface();
        if (!iface
            || !iface->isServiceRegistered(QStringLiteral("org.freedesktop.portal.Desktop"))) {
            smokeLog(tr("recorder arm+cancel: SKIP (no desktop portal on the session bus)"));
            smokeNext();
            return;
        }
        auto *guard = new QObject(this);
        auto *rec = new StudioRecorder(m_settings, guard);
        auto done = QSharedPointer<bool>::create(false);
        auto finishStep = [this, guard, rec, done](const QString &line) {
            if (*done)
                return;
            *done = true;
            rec->cancel(); // no commit() ever ran → nothing was encoded
            smokeLog(line);
            guard->deleteLater();
            smokeNext();
        };
        connect(rec, &StudioRecorder::armed, guard, [finishStep] {
            finishStep(tr("recorder arm+cancel: PASS (armed then cancelled, no commit)"));
        });
        connect(rec, &StudioRecorder::failed, guard, [finishStep](const QString &e) {
            finishStep(tr("recorder arm+cancel: SKIP (portal/stream unavailable: %1)").arg(e));
        });
        QTimer::singleShot(3000, guard, [finishStep] {
            finishStep(tr("recorder arm+cancel: SKIP (portal did not arm — needs interactive consent)"));
        });
        rec->start(/*holdForCommit=*/true);
    });

    smokeNext();
}

void StudioApp::devImportTestVideo()
{
    if (!devBuild())
        return;
    const QString v = devMakeTestVideo();
    if (v.isEmpty()) {
        emit notified(tr("Could not generate a test video (is ffmpeg installed?)."), true);
        return;
    }
    importFile(v);
}

void StudioApp::devOpenRecordingHud()
{
    if (!devBuild())
        return;
    if (!devShowRecordingHud())
        emit notified(tr("Recording HUD failed to load."), true);
}

void StudioApp::devRunExportTest()
{
    if (!devBuild())
        return;
    const QString v = devMakeTestVideo();
    if (v.isEmpty()) {
        emit notified(tr("Could not generate a test video (is ffmpeg installed?)."), true);
        return;
    }
    auto *p = new StudioProject(this);
    p->setVideoAbsPath(v);
    p->setDurationMs(1200);
    p->setFps(30.0);
    p->setVideoSize(QSize(640, 360));
    p->style()->setBackgroundType(QStringLiteral("gradient"));
    auto *ex = new ExportController(this);
    ex->setFormat(QStringLiteral("mp4"));
    ex->setResolution(QStringLiteral("custom"));
    ex->setCustomWidth(640);
    ex->setCustomHeight(360);
    ex->setFpsMode(QStringLiteral("30"));
    const QString out = QDir(QDir::tempPath()).filePath(QStringLiteral("unisic-studio-export-test.mp4"));
    ex->setOutputPath(out);
    connect(ex, &ExportController::stateChanged, this, [this, ex, p, out] {
        if (ex->state() == ExportController::Done) {
            emit notified(tr("Export test OK: %1").arg(out), false);
            ex->deleteLater();
            p->deleteLater();
        } else if (ex->state() == ExportController::Error) {
            emit notified(tr("Export test failed: %1").arg(ex->errorString()), true);
            ex->deleteLater();
            p->deleteLater();
        }
    });
    ex->start(p);
}

void StudioApp::devRunAutozoomTest()
{
    if (!devBuild())
        return;
    // In-app auto-zoom check (the exhaustive exit-coded pass is the
    // --autozoom-test CLI aid — it quits the process, so it is unsuitable for a
    // button). Synthesise a click cluster and assert the engine builds a camera.
    StudioProject p;
    p.setVideoSize(QSize(1280, 720));
    p.setDurationMs(3000);
    p.setFps(30.0);
    CursorTrack ct;
    for (int t = 0; t <= 3000; t += 40) {
        CursorSample s;
        s.tMs = t;
        s.x = (t < 800) ? 400 + t * 0.25 : 600;
        s.y = 360;
        ct.append(s);
    }
    p.setCursorTrack(ct);
    ClickTrack clk;
    ClickEvent d;
    d.tMs = 820;
    d.button = 1;
    d.state = ClickEvent::Down;
    d.x = 600;
    d.y = 360;
    ClickEvent u = d;
    u.tMs = 860;
    u.state = ClickEvent::Up;
    clk.append(d);
    clk.append(u);
    p.setClickTrack(clk);
    generateZoom(&p, /*onlyIfEmpty=*/false);
    const int n = p.zoom()->keyframes().size();
    emit notified(n > 0 ? tr("Auto-zoom test OK: %n keyframe(s).", "", n)
                        : tr("Auto-zoom test produced no keyframes."),
                  n == 0);
}
