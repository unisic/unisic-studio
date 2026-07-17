#include "HudManager.h"

#include "StudioApp.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QUrl>

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

    win->show();
    win->raise();
    return true;
}

void HudManager::closeHud()
{
    if (!m_win)
        return;
    QQuickWindow *w = m_win;
    m_win = nullptr;
    w->deleteLater(); // cascades the child QQmlContext
}
