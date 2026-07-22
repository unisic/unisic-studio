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
    // is imminent (countdown) or live.
    if (s == StudioApp::RecArming)
        return;

    // A monitor screencast bakes any overlay surface into the captured pixels —
    // the portal cannot exclude it. So when hudHideWhileRecording is on, the HUD
    // is shown only for the (excised) countdown and the brief finalize; during the
    // LIVE capture it is destroyed so nothing burns in. Stop/pause/cancel while
    // hidden come from bound hotkeys running `unisic-studio --stop/--pause/--cancel`.
    const bool live = (s == StudioApp::RecRecording || s == StudioApp::RecPaused);
    if (live && m_app->settings() && m_app->settings()->hudHideWhileRecording()) {
        closeHud();
        return;
    }
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
    if (auto *ls = LayerShellQt::Window::get(win)) {
        using LW = LayerShellQt::Window;
        // Map the user's placement preference to an anchor set + edge margins.
        const QString place = (m_app && m_app->settings())
                                  ? m_app->settings()->hudPlacement()
                                  : QStringLiteral("bottomCenter");
        const bool top   = place.startsWith(QLatin1String("top"));
        const bool left  = place.endsWith(QLatin1String("Left"));
        const bool right = place.endsWith(QLatin1String("Right"));
        LW::Anchors anchors = top ? LW::AnchorTop : LW::AnchorBottom;
        if (left)  anchors |= LW::AnchorLeft;
        if (right) anchors |= LW::AnchorRight;
        // 16px from the anchored edge; a wider inset on a chosen side so a corner
        // pill does not hug the screen edge.
        const int side = (left || right) ? 24 : 0;
        ls->setScope(QStringLiteral("unisic-studio-hud"));
        ls->setLayer(LW::LayerOverlay);          // above every window (incl. fullscreen)
        ls->setAnchors(anchors);
        ls->setMargins(QMargins(left ? side : 0, top ? 16 : 0,
                                right ? side : 0, top ? 0 : 16));
        ls->setExclusiveZone(0);                 // reserve no space / push nothing
        ls->setKeyboardInteractivity(LW::KeyboardInteractivityNone); // never steal focus
        fprintf(stderr, "HudManager: HUD path = layer-shell overlay (%s, platform=%s)\n",
                qPrintable(place), qPrintable(QGuiApplication::platformName()));
        fflush(stderr);
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
