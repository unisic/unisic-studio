#include "HudManager.h"

#include "StudioApp.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QUrl>

#include <cstdio>

#ifdef HAVE_LAYERSHELL
#include <QGuiApplication>
#include <QMargins>
#include <LayerShellQt/window.h>
#include <wayland-client.h>
#include <cstring>
#endif

HudManager::HudManager(StudioApp *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
{
    // Follow the recorder through the facade's single state enum so one signal
    // drives create + destroy. (StudioApp owns us and the recorder, so `app` is
    // guaranteed to outlive this object.)
    connect(m_app, &StudioApp::recorderStateChanged, this, &HudManager::syncToRecorderState);
}

HudManager::~HudManager()
{
    // Top-level window is not QObject-parented (it's a window), so drop it
    // explicitly. deleteLater keeps the "no delete with pending events" rule.
    if (m_win)
        m_win->deleteLater();
}

void HudManager::syncToRecorderState()
{
    const int s = m_app->recorderState();
    if (s == StudioApp::RecIdle) {
        closeHud();
        return;
    }
    // Arming = the portal picker dialog is up; the HUD only appears once capture
    // is imminent (countdown) or live. Every other non-idle state shows it.
    if (s == StudioApp::RecArming)
        return;
    showHud();
}

bool HudManager::showHud()
{
    if (m_win) {
        m_win->show();
        m_win->raise();
        return true;
    }
    if (!m_engine) {
        qWarning("HudManager: no engine set");
        return false;
    }

    QQmlComponent component(
        m_engine, QUrl(QStringLiteral("qrc:/qt/qml/UnisicStudio/qml/RecordingHud.qml")));
    if (component.isError()) {
        qWarning() << "HudManager:" << component.errorString();
        return false;
    }

    // Own per-window context (child of root → inherits `Studio`, `Theme`, …).
    auto *ctx = new QQmlContext(m_engine->rootContext(), this);
    QObject *obj = component.create(ctx);
    auto *win = qobject_cast<QQuickWindow *>(obj);
    if (!win) {
        qWarning("HudManager: RecordingHud root is not a Window");
        delete obj; // whatever QML built, if anything
        delete ctx;
        return false;
    }
    ctx->setParent(win); // window owns its context; cascades on window teardown
    m_win = win;

    // Layer-shell role MUST be set before the first show() (it configures the
    // surface role that show() commits). RecordingHud.qml keeps `visible: false`
    // for exactly this reason; we show it here once configured.
    configureLayerShell(win);
    win->show();
    win->raise();
    return true;
}

void HudManager::configureLayerShell(QQuickWindow *win)
{
#ifdef HAVE_LAYERSHELL
    if (!compositorSupportsLayerShell()) {
        // fprintf (not qInfo): a system qtlogging.ini can mute *.info.
        fprintf(stderr, "HudManager: HUD path = plain toplevel (platform=%s, no zwlr_layer_shell_v1)\n",
                qPrintable(QGuiApplication::platformName()));
        fflush(stderr);
        return; // GNOME/Mutter etc. — plain toplevel fallback (can't self-position)
    }
    fprintf(stderr, "HudManager: HUD path = layer-shell overlay bottom-centre (platform=%s)\n",
            qPrintable(QGuiApplication::platformName()));
    fflush(stderr);
    if (auto *ls = LayerShellQt::Window::get(win)) {
        using LW = LayerShellQt::Window;
        ls->setScope(QStringLiteral("unisic-studio-hud"));
        ls->setLayer(LW::LayerOverlay);          // above every window (incl. fullscreen)
        ls->setAnchors(LW::AnchorBottom);        // bottom edge, horizontally centred
        ls->setMargins(QMargins(0, 0, 0, 16));   // small gap from the screen edge
        ls->setExclusiveZone(0);                 // reserve no space / push nothing
        ls->setKeyboardInteractivity(LW::KeyboardInteractivityNone); // never steal focus
    }
#else
    Q_UNUSED(win);
    fprintf(stderr, "HudManager: HUD path = plain toplevel (built without layer-shell)\n");
    fflush(stderr);
#endif
}

bool HudManager::compositorSupportsLayerShell()
{
#ifdef HAVE_LAYERSHELL
    // Probe once: a throwaway wl_display connection (isolated from Qt's own, so the
    // registry roundtrip can't disturb Qt's dispatch), enumerate globals, look for
    // zwlr_layer_shell_v1. Adapted from Unisic's LayerShellNotifier.
    static const int cached = [] {
        if (!QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
            return 0;
        struct wl_display *display = wl_display_connect(nullptr);
        if (!display)
            return 0;
        bool found = false;
        struct wl_registry *registry = wl_display_get_registry(display);
        static const wl_registry_listener listener = {
            [](void *data, wl_registry *, uint32_t, const char *iface, uint32_t) {
                if (std::strcmp(iface, "zwlr_layer_shell_v1") == 0)
                    *static_cast<bool *>(data) = true;
            },
            [](void *, wl_registry *, uint32_t) {},
        };
        wl_registry_add_listener(registry, &listener, &found);
        wl_display_roundtrip(display); // one roundtrip delivers the global advertisements
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return found ? 1 : 0;
    }();
    return cached != 0;
#else
    return false;
#endif
}

void HudManager::closeHud()
{
    if (!m_win)
        return;
    QQuickWindow *w = m_win;
    m_win = nullptr;
    w->deleteLater(); // cascades the child QQmlContext
}
