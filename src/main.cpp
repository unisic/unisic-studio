#include "StudioApp.h"
#include <ConfigPath.h>              // kit: UnisicKit::setConfigName
#include <theme/IconImageProvider.h> // kit: image://icon provider
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QIcon>
#include <QStyleHints>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTranslator>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>
#include <QDir>
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

// Second launch: hand the running instance a bare "raise" and exit. Returns
// true when a peer accepted the message.
static bool notifyExistingInstance(const QString &serverName)
{
    QLocalSocket socket;
    socket.connectToServer(serverName, QIODevice::WriteOnly);
    if (!socket.waitForConnected(200))
        return false;
    socket.write("raise\n");
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

    // Single instance: a second launch forwards a bare "raise" and exits, so
    // only one process ever writes the config file. Only the genuinely
    // environmental no-peer case falls through to a fresh instance.
    const QString serverName = singleInstanceServerName();
    if (notifyExistingInstance(serverName))
        return 0;
    QLocalServer::removeServer(serverName); // clear a stale socket left by a crash
    auto *server = new QLocalServer(&app);
    if (!server->listen(serverName)) {
        // A live peer may have taken the name between our probe and listen() —
        // hand off rather than boot a duplicate config writer.
        if (notifyExistingInstance(serverName))
            return 0;
        qWarning() << "Could not create single-instance server:" << server->errorString();
    }
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                     [serverName] { QLocalServer::removeServer(serverName); });

    QQuickStyle::setStyle(QStringLiteral("Basic")); // fully custom look, no platform theme

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

    QQmlApplicationEngine engine;
    // The kit's ThemeController is a module QML singleton (engine-created); the
    // provider shares that one instance lazily via ThemeController::instance(),
    // so nullptr here is correct (matches Unisic).
    engine.addImageProvider(QStringLiteral("icon"), new IconImageProvider(nullptr));
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
        QObject::connect(server, &QLocalServer::newConnection, &app, [server, mainWin] {
            while (QLocalSocket *socket = server->nextPendingConnection()) {
                QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                if (mainWin) {
                    mainWin->show();
                    mainWin->raise();
                    mainWin->requestActivate();
                }
            }
        });
    }

    // Hidden dev aid: `--import <file>` drives the import path programmatically
    // (headless verification of probe → project → editor, no file dialog).
    // Deferred to the event loop so the engine and main window are fully up.
    // With `--selftest` it additionally saves → reloads → compares the project
    // and exits with a non-zero code on any mismatch (CI/functional check).
    {
        const QStringList args = app.arguments();
        const int idx = args.indexOf(QStringLiteral("--import"));
        const bool selftest = args.contains(QStringLiteral("--selftest"));
        if (idx >= 0 && idx + 1 < args.size()) {
            const QString file = args.at(idx + 1);
            if (selftest) {
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

    return app.exec();
}
