#include "RecentProjects.h"

#include <ConfigPath.h> // kit: UnisicKit::filePath()

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QVariantMap>

RecentProjects::RecentProjects(QObject *parent)
    : QObject(parent)
{
    load();
}

QString RecentProjects::indexPath()
{
    // Same directory as the settings .conf — one config dir, named once in
    // main.cpp via UnisicKit::setConfigName().
    return QFileInfo(UnisicKit::filePath()).absolutePath()
           + QStringLiteral("/recent-projects.json");
}

void RecentProjects::load()
{
    m_list.clear();
    QFile f(indexPath());
    if (!f.open(QIODevice::ReadOnly))
        return; // no index yet — empty is fine
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isArray())
        return;

    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        const QString path = o.value(QStringLiteral("path")).toString();
        // Prune entries whose project file is gone so the grid never offers a
        // dead tile.
        if (path.isEmpty() || !QFileInfo::exists(path))
            continue;
        QVariantMap m;
        m.insert(QStringLiteral("path"), path);
        m.insert(QStringLiteral("name"), o.value(QStringLiteral("name")).toString());
        m.insert(QStringLiteral("durationMs"),
                 qint64(o.value(QStringLiteral("durationMs")).toDouble()));
        m.insert(QStringLiteral("lastOpened"),
                 qint64(o.value(QStringLiteral("lastOpened")).toDouble()));
        m_list.append(m);
    }
}

void RecentProjects::recordOpened(const QString &path, const QString &name, qint64 durationMs)
{
    if (path.isEmpty())
        return;
    const QString abs = QFileInfo(path).absoluteFilePath();

    // De-dupe by absolute path.
    for (int i = m_list.size() - 1; i >= 0; --i) {
        if (m_list.at(i).toMap().value(QStringLiteral("path")).toString() == abs)
            m_list.removeAt(i);
    }

    QVariantMap m;
    m.insert(QStringLiteral("path"), abs);
    m.insert(QStringLiteral("name"), name);
    m.insert(QStringLiteral("durationMs"), durationMs);
    m.insert(QStringLiteral("lastOpened"), QDateTime::currentMSecsSinceEpoch());
    m_list.prepend(m);

    while (m_list.size() > kMaxEntries)
        m_list.removeLast();

    persist();
    emit changed();
}

void RecentProjects::persist() const
{
    QJsonArray arr;
    for (const QVariant &v : m_list) {
        const QVariantMap m = v.toMap();
        QJsonObject o;
        o.insert(QStringLiteral("path"), m.value(QStringLiteral("path")).toString());
        o.insert(QStringLiteral("name"), m.value(QStringLiteral("name")).toString());
        o.insert(QStringLiteral("durationMs"),
                 double(m.value(QStringLiteral("durationMs")).toLongLong()));
        o.insert(QStringLiteral("lastOpened"),
                 double(m.value(QStringLiteral("lastOpened")).toLongLong()));
        arr.append(o);
    }
    QSaveFile f(indexPath());
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning("RecentProjects: cannot write %s", qPrintable(indexPath()));
        return;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
    if (!f.commit())
        qWarning("RecentProjects: failed to commit index");
}
