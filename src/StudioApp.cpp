#include "StudioApp.h"
#include <QCoreApplication>

StudioApp::StudioApp(QObject *parent)
    : QObject(parent)
    , m_settings(new StudioSettings(this))
{
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

void StudioApp::quit()
{
    QCoreApplication::quit();
}
