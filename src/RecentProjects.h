#pragma once
#include <QObject>
#include <QString>
#include <QVariantList>

// The recent-projects index: a small JSON file next to the settings .conf
// (same config dir, one source of truth via UnisicKit::filePath()). Each entry
// is {path, name, durationMs, lastOpened} where lastOpened is msecs-since-epoch
// so QML can format it with `new Date(ms)`. Most-recent first, capped, and
// entries whose file no longer exists are pruned on load so the grid never
// offers a dead tile.
//
// Owned by StudioApp (parented). Persistence is best-effort: a write failure is
// logged, not fatal — the index is a convenience, not load-bearing state.
class RecentProjects : public QObject
{
    Q_OBJECT

public:
    explicit RecentProjects(QObject *parent = nullptr);

    // Entries as QVariantMaps for QML consumption (keys: path/name/durationMs/
    // lastOpened). Most-recent first.
    QVariantList list() const { return m_list; }

    // Record that `path` was just opened/saved: de-dupes by path, moves it to the
    // front, stamps lastOpened (now), persists, and emits changed().
    void recordOpened(const QString &path, const QString &name, qint64 durationMs);

signals:
    void changed();

private:
    static QString indexPath();
    void load();
    void persist() const;

    QVariantList m_list;
    static constexpr int kMaxEntries = 12;
};
