#pragma once
#include <QObject>
#include <QVector>

class QQmlEngine;
class QQmlContext;
class QQuickWindow;
class StudioProject;
class PosterExtractor;

// Owns the lifecycle of editor windows so StudioApp's facade doesn't. Each
// window gets its OWN QQmlContext carrying an `editorProject` context property
// (the StudioProject) — the per-window-context idiom from Unisic's
// AppContext::openEditor. The context prop is set BEFORE component.create() so
// QML resolves it at bind time.
//
// Ownership: the StudioProject is reparented under this manager; the per-window
// QQmlContext and any PosterExtractor are parented to that project, so a single
// project->deleteLater() on window close cascades them all. The window itself is
// top-level and deleteLater()'d explicitly. No leaks across any close path.
class EditorWindowManager : public QObject
{
    Q_OBJECT

public:
    explicit EditorWindowManager(QObject *parent = nullptr);
    ~EditorWindowManager() override;

    void setEngine(QQmlEngine *engine) { m_engine = engine; }

    // Open an editor window for `project` (ownership taken). `posterNeeded` asks
    // for an async poster-frame extraction (used when live QtMultimedia playback
    // is unavailable). Returns false and deletes `project` on failure.
    bool openEditor(StudioProject *project, bool posterNeeded);

    int openCount() const { return m_windows.size(); }

signals:
    void openCountChanged();

private:
    struct Open {
        QQuickWindow *win = nullptr;
        StudioProject *project = nullptr;   // owns its ctx + poster as children
    };

    void closeWindow(QQuickWindow *win);

    QQmlEngine *m_engine = nullptr;
    QVector<Open> m_windows;
};
