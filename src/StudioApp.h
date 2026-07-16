#pragma once
#include <QObject>
#include <qqmlregistration.h>
#include "StudioSettings.h"

// Application facade exposed to QML as the "Studio" context property (analogous
// to Unisic's "App"). Minimal for the M0 skeleton — it will grow to own the
// project model, capture/render/export subsystems and the after-record
// pipeline. Keep new behaviour in focused subsystem classes that StudioApp
// wires up, not piled onto this facade.
class StudioApp : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Context property")

    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool devBuild READ devBuild CONSTANT)
    Q_PROPERTY(StudioSettings *settings READ settings CONSTANT)

public:
    explicit StudioApp(QObject *parent = nullptr);

    QString version() const;
    bool devBuild() const;
    StudioSettings *settings() const { return m_settings; }

    Q_INVOKABLE void quit();

private:
    // Parent-owned: constructed with `this` as parent so it dies with the app
    // facade — no manual delete, no leak.
    StudioSettings *m_settings;
};
