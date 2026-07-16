#pragma once
#include <QObject>
#include <QVariantList>
#include <qqmlregistration.h>
#include "StudioSettings.h"
// Full type (not a forward decl): the saveProject(StudioProject*) Q_INVOKABLEs
// force moc to register the StudioProject* metatype, which needs the complete
// type.
#include "project/StudioProject.h"

class QQmlEngine;
class RecentProjects;
class EditorWindowManager;

// Application facade exposed to QML as the "Studio" context property (analogous
// to Unisic's "App"). It COORDINATES — the substantial logic lives in focused
// classes it wires up (RecentProjects, EditorWindowManager, VideoProbe), not
// piled onto this facade. It grows further as capture/render/export land.
class StudioApp : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Context property")

    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool devBuild READ devBuild CONSTANT)
    Q_PROPERTY(StudioSettings *settings READ settings CONSTANT)
    // QtMultimedia QML plugin present → the editor shows a live video preview;
    // otherwise it degrades to a static poster frame. CONSTANT: fixed per run.
    Q_PROPERTY(bool capVideoPlayback READ capVideoPlayback CONSTANT)
    // Recent-project tiles for the launcher: [{path,name,durationMs,lastOpened}].
    Q_PROPERTY(QVariantList recentProjects READ recentProjects NOTIFY recentProjectsChanged)

public:
    explicit StudioApp(QObject *parent = nullptr);

    QString version() const;
    bool devBuild() const;
    StudioSettings *settings() const { return m_settings; }
    bool capVideoPlayback() const;
    QVariantList recentProjects() const;

    // Wire the QML engine once at startup so per-window editor contexts can be
    // built. Called from main.cpp after the engine exists.
    void setEngine(QQmlEngine *engine);

    Q_INVOKABLE void quit();

    // Import: native open dialog → async ffprobe → new StudioProject → editor
    // window. Errors surface via notified().
    Q_INVOKABLE void importVideo();
    // Import a specific file with no dialog. Reused by the hidden --import dev
    // aid and callable from QML.
    Q_INVOKABLE void importFile(const QString &path);

    // Open an existing project (accepts a file:// url or a plain path).
    Q_INVOKABLE bool openProject(const QString &pathOrUrl);

    // Save silently: to the project's current path, or (first save) into the
    // configured projects directory named after the video. Save As always asks.
    Q_INVOKABLE bool saveProject(StudioProject *project);
    Q_INVOKABLE bool saveProjectAs(StudioProject *project);

    // Native image picker for the wallpaper-background field. Empty on cancel.
    Q_INVOKABLE QString pickWallpaper(const QString &startDir = QString());

    // Native "save as" dialog for the export destination. `format` is the file
    // extension ("mp4"/"webm") used for the filter and default suffix. Returns a
    // plain local path (empty on cancel).
    Q_INVOKABLE QString pickExportOutput(const QString &format, const QString &startPath = QString());

    // Open the containing folder of `path` in the file manager (xdg-open via
    // QDesktopServices). Used by the export "Reveal in folder" action.
    Q_INVOKABLE void revealInFolder(const QString &path);

signals:
    void recentProjectsChanged();
    // A user-facing message the shell surfaces as a toast (error=true → styled
    // as a failure).
    void notified(const QString &message, bool error);
    // Emitted after an import fully wires up (probe → project → editor window).
    // The project is owned by the editor window; do not delete it. Used by the
    // --import dev self-test in main.cpp.
    void imported(StudioProject *project);

private:
    bool doSave(StudioProject *project, const QString &path);
    QString defaultSavePath(StudioProject *project) const;

    // Parent-owned (constructed with `this`): die with the facade, no leaks.
    StudioSettings *m_settings;
    RecentProjects *m_recent;
    EditorWindowManager *m_editors;
};
