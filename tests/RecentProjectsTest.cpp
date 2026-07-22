#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <ConfigPath.h> // kit (header-only): UnisicKit::setConfigName/filePath

#include "RecentProjects.h"

// RecentProjects: the recents index the launcher grid binds to. Exercises the
// recordOpened contract and the remove() API behind the grid's delete-recording
// action (drop + persist + changed()). Runs under QStandardPaths test mode with
// its own config name, so it never touches a real ~/.config/unisic-studio*.
class RecentProjectsTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        UnisicKit::setConfigName(QStringLiteral("unisic-studio-recents-test"));
    }
    void init() { QFile::remove(indexFile()); }
    void cleanupTestCase() { QFile::remove(indexFile()); }

    void removeDropsEntryPersistsAndSignals();
    void removeUnknownIsANoop();

private:
    static QString indexFile()
    {
        return QFileInfo(UnisicKit::filePath()).absolutePath()
               + QStringLiteral("/recent-projects.json");
    }
    // load() prunes entries whose file no longer exists, so persisted-state
    // checks need paths that really do.
    static QString makeFile(const QTemporaryDir &dir, const QString &name)
    {
        const QString p = dir.filePath(name);
        QFile f(p);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write("x");
        f.close();
        return QFileInfo(p).absoluteFilePath();
    }
};

void RecentProjectsTest::removeDropsEntryPersistsAndSignals()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = makeFile(dir, QStringLiteral("a.unisicstudio"));
    const QString b = makeFile(dir, QStringLiteral("b.unisicstudio"));
    QVERIFY(!a.isEmpty() && !b.isEmpty());

    RecentProjects recents;
    recents.recordOpened(a, QStringLiteral("a"), 1000);
    recents.recordOpened(b, QStringLiteral("b"), 2000);
    QCOMPARE(recents.list().size(), 2);

    QSignalSpy changed(&recents, &RecentProjects::changed);
    QVERIFY(recents.remove(a));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(recents.list().size(), 1);
    QCOMPARE(recents.list().first().toMap().value(QStringLiteral("path")).toString(), b);

    // A fresh instance re-loads the index: the removal must have persisted.
    RecentProjects reloaded;
    QCOMPARE(reloaded.list().size(), 1);
    QCOMPARE(reloaded.list().first().toMap().value(QStringLiteral("path")).toString(), b);
}

void RecentProjectsTest::removeUnknownIsANoop()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = makeFile(dir, QStringLiteral("a.unisicstudio"));
    QVERIFY(!a.isEmpty());

    RecentProjects recents;
    recents.recordOpened(a, QStringLiteral("a"), 1000);

    QSignalSpy changed(&recents, &RecentProjects::changed);
    QVERIFY(!recents.remove(dir.filePath(QStringLiteral("missing.unisicstudio"))));
    QVERIFY(!recents.remove(QString()));
    QCOMPARE(changed.count(), 0);
    QCOMPARE(recents.list().size(), 1);
}

QTEST_GUILESS_MAIN(RecentProjectsTest)
#include "RecentProjectsTest.moc"
