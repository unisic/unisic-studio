#pragma once
#include <QObject>

class QQmlEngine;
class QQuickWindow;
class StudioApp;

// Owns the single always-on-top recording HUD window. It is a SEPARATE top-level
// frameless window (deliberately NOT parented into StudioMain) built with its OWN
// QQmlContext — the per-window-context idiom from EditorWindowManager. The context
// is a child of the engine root, so the HUD inherits the `Studio` context property
// and the `Theme`/`ThemeController` singletons and reads recorder state directly.
//
// Lifecycle: the HUD is shown once a recording is actually live (countdown →
// recording → paused → finalizing) and destroyed the moment the recorder returns
// to idle (stop, cancel or fail). During arming (the portal picker is up) no HUD
// is shown yet.
//
// Ownership / no leaks: the per-window QQmlContext is reparented under the window,
// so a single window->deleteLater() on close cascades it. The QML blink animation
// is bound to the window's visibility, so it stops when the HUD hides/closes — no
// runaway timer survives teardown.
class HudManager : public QObject
{
    Q_OBJECT
public:
    explicit HudManager(StudioApp *app, QObject *parent = nullptr);
    ~HudManager() override;

    void setEngine(QQmlEngine *engine) { m_engine = engine; }

    // Create + show the HUD in whatever the current recorder state is. Idempotent.
    // Returns false if the component failed to build (used by the --hud-test dev
    // self-test in main.cpp to verify the QML instantiates without a real portal
    // session). Normal callers go through syncToRecorderState().
    bool showHud();

public slots:
    // Create/destroy the HUD to match the facade's recorder state. Wired to
    // StudioApp::recorderStateChanged.
    void syncToRecorderState();

private:
    void closeHud();

    QQmlEngine *m_engine = nullptr;
    StudioApp *m_app;
    QQuickWindow *m_win = nullptr;   // top-level; owned here, deleteLater()'d on close
};
