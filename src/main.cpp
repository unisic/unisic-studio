#include "StudioApp.h"
#include "AutozoomSelfTest.h"
#include "MotionSelfTest.h"
#include "project/StudioProject.h"
#include "project/StyleModel.h"
#include "media/VideoProbe.h"
#include "render/ExportController.h"
#include "render/CursorShapeProvider.h" // image://cursorshape (cursor overlay)
#include <ConfigPath.h>              // kit: UnisicKit::setConfigName
#include <theme/IconImageProvider.h> // kit: image://icon provider
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QIcon>
#include <QStyleHints>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTranslator>
#include <QPointer>
#include <QSharedPointer>
#include <QSocketNotifier>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QScopedPointer>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

// SIGINT/SIGTERM/SIGHUP must run destructors (QSettings flush) — the default
// handlers kill the process cold and every setting change since launch is lost.
// Self-pipe pattern: the handler only write()s (async-signal-safe); the
// notifier quits the event loop, so aboutToQuit and destructors run.
// (Simplified from Unisic's main.cpp.)
static int sigQuitFd[2] = {-1, -1};
static void unixSignalHandler(int)
{
    const char one = 1;
    (void)::write(sigQuitFd[1], &one, sizeof(one));
}
static void installSignalHandlers(QCoreApplication *app)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sigQuitFd) != 0)
        return;
    auto *notifier = new QSocketNotifier(sigQuitFd[0], QSocketNotifier::Read, app);
    QObject::connect(notifier, &QSocketNotifier::activated, app, [] {
        char tmp;
        (void)::read(sigQuitFd[0], &tmp, sizeof(tmp));
        QCoreApplication::quit();
    });
    struct sigaction sa{};
    sa.sa_handler = unixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
}

static QString singleInstanceServerName()
{
    // Key on UID ALONE — deliberately not on any session/display env var, which
    // disagree across autostart vs an interactive click vs a compositor keybind
    // spawn and would split the app into duplicate instances racing the same
    // config file. Dev builds use their own socket so stable and dev can run
    // side by side. (Same reasoning as Unisic's main.cpp.)
#ifdef UNISIC_DEV_BUILD
    return QStringLiteral("app.unisic.UnisicStudioDev.%1").arg(getuid());
#else
    return QStringLiteral("app.unisic.UnisicStudio.%1").arg(getuid());
#endif
}

// Second launch: hand the running instance either a bare "raise" or, when a
// project file was passed on the command line (double-click), "open <path>" so
// the single live instance opens it. Returns true when a peer accepted.
static bool notifyExistingInstance(const QString &serverName, const QString &openPath)
{
    QLocalSocket socket;
    socket.connectToServer(serverName, QIODevice::WriteOnly);
    if (!socket.waitForConnected(200))
        return false;
    const QByteArray msg = openPath.isEmpty()
                               ? QByteArray("raise\n")
                               : ("open " + openPath.toUtf8() + "\n");
    socket.write(msg);
    socket.flush();
    socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    return true;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv); // QApplication: Widgets needed for future tray/dialogs
    // Dev builds are a SEPARATE app: own application name (which moves every
    // QStandardPaths location), own desktop id, own single-instance socket and
    // own config dir — so a build-tree binary never shadows or fights an
    // installed stable Studio. Mirrors Unisic's dev-identity pattern.
#ifdef UNISIC_DEV_BUILD
    app.setApplicationName(QStringLiteral("unisic-studio-dev"));
    app.setApplicationDisplayName(QStringLiteral("Unisic Studio (dev)"));
    app.setDesktopFileName(QStringLiteral("app.unisic.UnisicStudioDev"));
    UnisicKit::setConfigName(QStringLiteral("unisic-studio-dev"));
#else
    app.setApplicationName(QStringLiteral("unisic-studio"));
    app.setApplicationDisplayName(QStringLiteral("Unisic Studio"));
    app.setDesktopFileName(QStringLiteral("app.unisic.UnisicStudio"));
    // Name the kit config file ONCE, BEFORE anything constructs a Settings or
    // ThemeController — everything derives from it (~/.config/<name>/<name>.conf).
    UnisicKit::setConfigName(QStringLiteral("unisic-studio"));
#endif
    app.setApplicationVersion(QStringLiteral(STUDIO_VERSION));
    app.setOrganizationName(QStringLiteral("Unisic"));
    app.setWindowIcon(QIcon(QStringLiteral(":/resources/icons/unisic-studio.svg")));

    installSignalHandlers(&app);

    const QStringList arguments = app.arguments();
    const bool autozoomTestMode = arguments.contains(QStringLiteral("--autozoom-test"));
    const bool motionTestMode = arguments.contains(QStringLiteral("--motion-test"));
    if (autozoomTestMode && motionTestMode) {
        fprintf(stderr, "Choose only one of --autozoom-test and --motion-test.\n");
        return 2;
    }
    const bool isolatedMotionTest = autozoomTestMode || motionTestMode;

    // A .unisicstudio project passed positionally (file-manager double-click via
    // the desktop file's `%f`, or `unisic-studio foo.unisicstudio`) opens in the
    // editor. Filtered by suffix so the hidden --import/--export-test flag values
    // (video paths) are never mistaken for a project to open.
    QString fileToOpen;
    for (const QString &a : app.arguments().mid(1)) {
        if (a.startsWith(QLatin1Char('-')))
            continue;
        if (a.endsWith(QLatin1String(".unisicstudio"), Qt::CaseInsensitive)
            && QFileInfo::exists(a)) {
            fileToOpen = QFileInfo(a).absoluteFilePath();
            break;
        }
    }

    // Dev/CI aid flags drive a scripted run to completion and exit — forwarding
    // them to a live instance as a bare "raise" (the old behaviour) silently
    // exited 0 with NO work done, which reads as a fake pass in CI. They opt out
    // of single-instance entirely, like the motion tests.
    const bool devAidMode = arguments.contains(QStringLiteral("--import"))
                            || arguments.contains(QStringLiteral("--export-test"))
                            || arguments.contains(QStringLiteral("--smoke-test"))
                            || arguments.contains(QStringLiteral("--hud-test"))
                            || arguments.contains(QStringLiteral("--page"));

    // Single instance: a second launch forwards "raise" (or "open <path>") and
    // exits, so only one process ever writes the config file. Only the genuinely
    // environmental no-peer case falls through to a fresh instance.
    const QString serverName = singleInstanceServerName();
    auto *server = new QLocalServer(&app);
    if (!isolatedMotionTest && !devAidMode) {
        if (notifyExistingInstance(serverName, fileToOpen))
            return 0;
        // listen() FIRST, without removing the socket: an unconditional
        // removeServer() here could unlink a LIVE peer's socket when two
        // launches race between the probe and listen(), splitting the app into
        // duplicate config writers. Only after a failed listen AND a failed
        // connect (peer truly dead) is the socket provably stale.
        if (!server->listen(serverName)) {
            if (notifyExistingInstance(serverName, fileToOpen))
                return 0;
            QLocalServer::removeServer(serverName); // stale socket left by a crash
            if (!server->listen(serverName))
                qWarning() << "Could not create single-instance server:"
                           << server->errorString();
        }
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                         [serverName] { QLocalServer::removeServer(serverName); });
    }

    QQuickStyle::setStyle(QStringLiteral("Basic")); // fully custom look, no platform theme

    // Pin the OpenGL RHI backend for the whole process. The offscreen export
    // (RenderPipeline) renders CompositionRoot into a QOpenGLFramebufferObject via
    // the PUBLIC QQuickRenderTarget::fromOpenGLTexture path — the RHI's private
    // texture-target headers are not shipped in the SDK, and the software backend
    // cannot render QtQuick.Effects (the composition's shadow/rounded mask). On
    // Linux/Mesa OpenGL is already the default, so this is consistent, not a
    // regression, and it must be set before the first QQuickWindow exists.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    // Resolve missing symbolic icon names against Breeze so tool/action glyphs
    // still appear under desktop themes that lack them; re-pin on runtime
    // light/dark flips (a fallback frozen at startup keeps serving the old
    // scheme's glyphs). Same pattern as Unisic's main.cpp.
    const bool dark = QGuiApplication::styleHints()
                      && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    QIcon::setFallbackThemeName(dark ? QStringLiteral("breeze-dark") : QStringLiteral("breeze"));
    if (auto *hints = QGuiApplication::styleHints()) {
        QObject::connect(hints, &QStyleHints::colorSchemeChanged, &app,
                         [](Qt::ColorScheme scheme) {
            QIcon::setFallbackThemeName(scheme == Qt::ColorScheme::Dark
                                            ? QStringLiteral("breeze-dark")
                                            : QStringLiteral("breeze"));
        });
    }

    // Install the UI-language translator BEFORE the engine loads, so every qsTr
    // in the QML resolves on first paint. English-only for M0; the structure is
    // ready for more languages (add a .ts, load by code here). Simplified from
    // Unisic's applyLanguage.
#ifdef HAVE_TRANSLATIONS
    auto *translator = new QTranslator(&app);
    if (translator->load(QStringLiteral(":/i18n/studio_en.qm")))
        app.installTranslator(translator);
    else
        delete translator;
#endif

    StudioApp studio;

    // Motion integration tests are isolated from the regular UID-keyed socket
    // and skip the visible main QML engine. RenderPipeline creates the exact
    // CompositionRoot engine it needs, so these remain unattended GL tests and
    // can run while a normal Studio instance is open.
    if (isolatedMotionTest) {
        if (motionTestMode)
            QTimer::singleShot(0, &studio, [&studio] { MotionSelfTest::run(&studio); });
        else
            QTimer::singleShot(0, &studio, [&studio] { AutozoomSelfTest::run(&studio); });
        return app.exec();
    }

    QQmlApplicationEngine engine;
    // The kit's ThemeController is a module QML singleton (engine-created); the
    // provider shares that one instance lazily via ThemeController::instance(),
    // so nullptr here is correct (matches Unisic).
    engine.addImageProvider(QStringLiteral("icon"), new IconImageProvider(nullptr));
    // Cursor overlay bitmaps (recorded shapes) served by project id + shape id.
    engine.addImageProvider(QStringLiteral("cursorshape"), new CursorShapeProvider());
    engine.rootContext()->setContextProperty(QStringLiteral("Studio"), &studio);
    // Give the facade the engine so it can build per-window editor QQmlContexts.
    studio.setEngine(&engine);

    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/UnisicStudio/qml/StudioMain.qml")));

    // Single-instance "raise": a second launch pings the socket; bring the main
    // window forward. Only a bare raise for now.
    QPointer<QQuickWindow> mainWin =
        qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (server->isListening()) {
        QObject::connect(server, &QLocalServer::newConnection, &app, [server, mainWin, &studio] {
            while (QLocalSocket *socket = server->nextPendingConnection()) {
                QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                // The message is tiny and flushed immediately by the sender.
                QByteArray buf;
                if (socket->waitForReadyRead(300))
                    buf = socket->readAll();
                const QString text = QString::fromUtf8(buf).trimmed();
                if (text.startsWith(QLatin1String("open "))) {
                    const QString path = text.mid(5).trimmed();
                    if (!path.isEmpty())
                        studio.openProject(path); // opens its own editor window
                }
                if (mainWin) {
                    mainWin->show();
                    mainWin->raise();
                    mainWin->requestActivate();
                }
            }
        });
    }

    // This IS the single live instance: open a positionally-passed project once
    // the engine and main window are fully up.
    if (!fileToOpen.isEmpty())
        QTimer::singleShot(0, &studio, [&studio, fileToOpen] { studio.openProject(fileToOpen); });

    // Hidden dev aid: `--import <file>` drives the import path programmatically
    // (headless verification of probe → project → editor, no file dialog).
    // Deferred to the event loop so the engine and main window are fully up.
    // With `--selftest` it additionally saves → reloads → compares the project
    // and exits with a non-zero code on any mismatch (CI/functional check).
    {
        const QStringList args = app.arguments();
        const int idx = args.indexOf(QStringLiteral("--import"));
        const bool selftest = args.contains(QStringLiteral("--selftest"));
        if (idx >= 0 && idx + 1 >= args.size()) {
            fprintf(stderr, "--import needs a video file argument.\n");
            return 2;
        }
        if (idx >= 0 && idx + 1 < args.size()) {
            const QString file = args.at(idx + 1);
            if (selftest) {
                // A failed import surfaces only as a notified(…, error=true)
                // toast — without this hook the selftest would sit in the event
                // loop forever instead of reporting the failure.
                QObject::connect(&studio, &StudioApp::notified, &app,
                                 [](const QString &message, bool error) {
                    if (!error)
                        return;
                    fprintf(stderr, "selftest: FAILED: %s\n", qPrintable(message));
                    fflush(stderr);
                    QCoreApplication::exit(2);
                });
                QObject::connect(&studio, &StudioApp::imported, &app, [&studio](StudioProject *p) {
                    // Exercise the REAL facade save path (default projects dir +
                    // portable relPath + recents index), then reload from the
                    // path it chose and compare. fprintf (not qInfo) so a system
                    // qtlogging.ini that mutes *.info can't swallow the result.
                    const bool saved = studio.saveProject(p);
                    const QString path = p->property("_sourcePath").toString();
                    fprintf(stderr, "selftest: editor window created; saveProject=%s (%s)\n",
                            saved ? "OK" : "FAIL", qPrintable(path));
                    bool ok = saved && !path.isEmpty();
                    if (ok) {
                        QString err;
                        QScopedPointer<StudioProject> r(StudioProject::load(path, &err));
                        ok = !r.isNull() && r->durationMs() == p->durationMs()
                             && r->videoSize() == p->videoSize() && qFuzzyCompare(r->fps(), p->fps())
                             && !r->videoMissing();
                        fprintf(stderr,
                                "selftest: reload=%s (dur=%lldms size=%dx%d fps=%.3f videoResolved=%s)\n",
                                ok ? "OK" : "MISMATCH", static_cast<long long>(p->durationMs()),
                                p->videoSize().width(), p->videoSize().height(), p->fps(),
                                r.isNull() ? "" : qPrintable(r->videoResolved()));
                    }
                    fflush(stderr);
                    QCoreApplication::exit(ok ? 0 : 2);
                });
            }
            QTimer::singleShot(0, &studio, [&studio, file] { studio.importFile(file); });
        }
    }

    // Hidden dev aid: `--export-test <in> <out>` drives the FULL offscreen export
    // headlessly — probe → project → apply a non-default style → export MP4
    // 1280x720@30 (default quality) → exit 0 on success, 1 on failure. It never
    // opens an editor window, so it exercises exactly the render/export path.
    // Runs on a live Wayland session (offscreen QPA cannot init the GL RHI on many
    // Mesa/EGL stacks — the documented sharp edge in RenderPipeline).
    {
        const QStringList args = app.arguments();
        const int idx = args.indexOf(QStringLiteral("--export-test"));
        if (idx >= 0 && idx + 2 < args.size()) {
            const QString in = args.at(idx + 1);
            const QString out = args.at(idx + 2);
            // Optional: `--cancel-ms <n>` cancels the export n ms after it starts,
            // then exits 0 once teardown settles — exercises the cancel path
            // (no orphan ffmpeg, partial output deleted).
            int cancelMs = 0;
            const int ci = args.indexOf(QStringLiteral("--cancel-ms"));
            if (ci >= 0 && ci + 1 < args.size())
                cancelMs = args.at(ci + 1).toInt();
            QString fmt = QStringLiteral("mp4");
            const int fi = args.indexOf(QStringLiteral("--format"));
            if (fi >= 0 && fi + 1 < args.size())
                fmt = args.at(fi + 1);
            // Optional: `--bg <type>` overrides the background (color/gradient/
            // wallpaper/desktopBlur) so the desktopBlur poster path can be
            // exercised headlessly.
            QString bg = QStringLiteral("gradient");
            const int bi = args.indexOf(QStringLiteral("--bg"));
            if (bi >= 0 && bi + 1 < args.size())
                bg = args.at(bi + 1);
            // Optional: `--aspect <source|16:9|9:16|1:1>` exercises the crop-to-fill
            // path (the base view becomes an output-aspect crop, no letterbox).
            QString aspect = QStringLiteral("source");
            const int ai = args.indexOf(QStringLiteral("--aspect"));
            if (ai >= 0 && ai + 1 < args.size())
                aspect = args.at(ai + 1);
            // Optional: `--trim-in <ms>` / `--trim-out <ms>` exercise the trim
            // path (preview clamp shares the same range; export uses -ss/-t).
            qint64 trimIn = -1, trimOut = -1;
            const int tii = args.indexOf(QStringLiteral("--trim-in"));
            if (tii >= 0 && tii + 1 < args.size())
                trimIn = args.at(tii + 1).toLongLong();
            const int toi = args.indexOf(QStringLiteral("--trim-out"));
            if (toi >= 0 && toi + 1 < args.size())
                trimOut = args.at(toi + 1).toLongLong();
            QTimer::singleShot(0, &app, [in, out, cancelMs, fmt, bg, trimIn, trimOut, aspect] {
                auto *probe = new VideoProbe(qApp);
                QObject::connect(probe, &VideoProbe::failed, qApp, [](const QString &r) {
                    fprintf(stderr, "export-test: probe failed: %s\n", qPrintable(r));
                    fflush(stderr);
                    QCoreApplication::exit(1);
                });
                QObject::connect(
                    probe, &VideoProbe::probed, qApp,
                    [in, out, cancelMs, fmt, bg, trimIn, trimOut, aspect](qint64 durationMs,
                                                                         double fps,
                                                                         const QSize &size) {
                        auto *p = new StudioProject(qApp);
                        p->setVideoAbsPath(in);
                        p->setDurationMs(durationMs);
                        p->setFps(fps);
                        p->setVideoSize(size);
                        if (trimIn >= 0)
                            p->setTrimInMs(trimIn);
                        if (trimOut >= 0)
                            p->setTrimOutMs(trimOut);
                        // A clearly non-default style so the pixel check can prove
                        // the composition (not the raw video) was rendered.
                        StyleModel *st = p->style();
                        st->setPaddingPct(12);
                        st->setCornerRadius(20);
                        st->setBackgroundType(bg);
                        st->setAspect(aspect);
                        st->setGradientStart(QColor(0xE0, 0x10, 0x40));
                        st->setGradientEnd(QColor(0x10, 0x20, 0xE0));

                        auto *ex = new ExportController(qApp);
                        ex->setFormat(fmt);
                        ex->setResolution(QStringLiteral("custom"));
                        ex->setCustomWidth(1280);
                        ex->setCustomHeight(720);
                        ex->setFpsMode(QStringLiteral("30"));
                        ex->setOutputPath(out);
                        QObject::connect(ex, &ExportController::stateChanged, qApp, [ex] {
                            if (ex->state() == ExportController::Done) {
                                fprintf(stderr, "export-test: OK\n");
                                fflush(stderr);
                                QCoreApplication::exit(0);
                            } else if (ex->state() == ExportController::Error) {
                                fprintf(stderr, "export-test: FAILED: %s\n",
                                        qPrintable(ex->errorString()));
                                fflush(stderr);
                                QCoreApplication::exit(1);
                            }
                        });
                        ex->start(p);
                        if (cancelMs > 0) {
                            QTimer::singleShot(cancelMs, ex, [ex] {
                                fprintf(stderr, "export-test: cancelling\n");
                                fflush(stderr);
                                ex->cancel();
                            });
                            // Give the non-blocking encoder teardown (SIGTERM →
                            // SIGKILL after 1 s) time to reap before we exit.
                            QTimer::singleShot(cancelMs + 2500, qApp, [] {
                                fprintf(stderr, "export-test: cancelled-clean\n");
                                fflush(stderr);
                                QCoreApplication::exit(0);
                            });
                        }
                    });
                probe->probe(in);
            });
        }
    }

    // Hidden dev aids for headless UI verification (no real recording/portal):
    //   --page settings   open the Settings page on the main window at launch
    //   --hud-test        instantiate the recording HUD in its idle state, then
    //                     exit 0 (component OK) / 3 (build failed). Proves the QML
    //                     loads under `offscreen` without a live PipeWire session.
    {
        const QStringList args = app.arguments();
        const int pi = args.indexOf(QStringLiteral("--page"));
        if (pi >= 0 && pi + 1 < args.size() && mainWin) {
            const QString page = args.at(pi + 1);
            const int idx = page == QLatin1String("settings") ? 1 : 0;
            QTimer::singleShot(0, mainWin, [mainWin, idx] {
                if (mainWin)
                    mainWin->setProperty("currentPage", idx);
            });
        }
        // Dev aid: run the F8 smoke test headlessly, print the transcript, and
        // exit 0 (no FAIL lines) / 4 (a step failed). Runs on a live session
        // (the offscreen render steps need the GL RHI).
        if (args.contains(QStringLiteral("--smoke-test"))) {
            auto reported = QSharedPointer<bool>::create(false);
            QObject::connect(&studio, &StudioApp::smokeTestChanged, &app, [&studio, reported] {
                if (studio.smokeTestRunning() || *reported)
                    return;
                *reported = true;
                fprintf(stderr, "%s", qPrintable(studio.smokeTestLog()));
                fflush(stderr);
                // "FAILURES PRESENT" is only in the summary when a step failed
                // (the count line reads "0 FAIL" on success — don't match that).
                const bool ok = !studio.smokeTestLog().contains(QStringLiteral("FAILURES PRESENT"));
                QCoreApplication::exit(ok ? 0 : 4);
            });
            QTimer::singleShot(0, &studio, [&studio] { studio.runSmokeTest(); });
        }
        if (args.contains(QStringLiteral("--hud-test"))) {
            QTimer::singleShot(0, &app, [&studio] {
                const bool ok = studio.devShowRecordingHud();
                fprintf(stderr, "hud-test: %s\n", ok ? "OK" : "FAILED");
                fflush(stderr);
                QTimer::singleShot(500, qApp, [ok] { QCoreApplication::exit(ok ? 0 : 3); });
            });
        }
    }

    return app.exec();
}
