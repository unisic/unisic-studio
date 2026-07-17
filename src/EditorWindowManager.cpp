#include "EditorWindowManager.h"

#include "PreviewController.h"
#include "media/PosterExtractor.h"
#include "project/StudioProject.h"

#include <QFileInfo>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QUrl>

EditorWindowManager::EditorWindowManager(QObject *parent)
    : QObject(parent)
{
}

EditorWindowManager::~EditorWindowManager() = default;

bool EditorWindowManager::openEditor(StudioProject *project, bool posterNeeded)
{
    if (!project)
        return false;
    if (!m_engine) {
        qWarning("EditorWindowManager: no engine set");
        project->deleteLater();
        return false;
    }

    QQmlComponent component(
        m_engine, QUrl(QStringLiteral("qrc:/qt/qml/UnisicStudio/qml/EditorWindow.qml")));
    if (component.isError()) {
        qWarning() << "EditorWindowManager:" << component.errorString();
        project->deleteLater();
        return false;
    }

    // Manager owns the project; the per-window context is a CHILD of the project
    // so it dies with it. Context prop set BEFORE create() (Unisic idiom).
    project->setParent(this);
    auto *ctx = new QQmlContext(m_engine->rootContext(), project);
    ctx->setContextProperty(QStringLiteral("editorProject"), project);
    // Per-window preview head: smooths time, evaluates the camera + cursor. Child
    // of the project so it dies with the window (releases its shape registration).
    auto *preview = new PreviewController(project, project);
    ctx->setContextProperty(QStringLiteral("preview"), preview);

    QObject *obj = component.create(ctx);
    auto *win = qobject_cast<QQuickWindow *>(obj);
    if (!win) {
        qWarning() << "EditorWindowManager: root is not a Window";
        delete obj;             // whatever QML built, if anything
        project->deleteLater(); // reaps ctx (its child)
        return false;
    }

    m_windows.append({win, project});

    // Close → tear the whole window down. Context object is `project` so the
    // connection auto-drops when the project is destroyed.
    connect(win, &QQuickWindow::visibleChanged, project, [this, win](bool visible) {
        if (!visible)
            closeWindow(win);
    });

    // No-QtMultimedia fallback: extract a still so the canvas isn't empty. The
    // extractor is parented to the project (cascade-deleted); it writes into its
    // own QTemporaryDir, swept on teardown. A freshly imported project has no
    // resolved path yet, so fall back to the absolute path (same rule as QML).
    const QString video = project->videoResolved().isEmpty() ? project->videoAbsPath()
                                                             : project->videoResolved();
    if (posterNeeded && !video.isEmpty() && QFileInfo::exists(video)
        && PosterExtractor::available()) {
        auto *poster = new PosterExtractor(project);
        connect(poster, &PosterExtractor::extracted, win, [win](const QString &png) {
            win->setProperty("posterSource", QUrl::fromLocalFile(png).toString());
        });
        poster->extract(video, 0);
    }

    win->show();
    win->requestActivate();
    emit openCountChanged();
    return true;
}

void EditorWindowManager::closeWindow(QQuickWindow *win)
{
    for (int i = 0; i < m_windows.size(); ++i) {
        if (m_windows.at(i).win != win)
            continue;
        StudioProject *project = m_windows.at(i).project;
        m_windows.removeAt(i);
        win->deleteLater();
        project->deleteLater(); // cascades ctx + poster (its children)
        emit openCountChanged();
        return;
    }
    // Not found → a duplicate visibleChanged(false) during teardown; ignore.
}
