#pragma once
#include <QObject>
#include <QTimer>
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
class HudManager;       // fwd-decl: owns the always-on-top recording HUD window
class StudioRecorder;   // fwd-decl: capture subsystem, wired up in the .cpp

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

    // Recording state machine, exposed thin (the logic lives in StudioRecorder).
    Q_PROPERTY(int recorderState READ recorderState NOTIFY recorderStateChanged)
    Q_PROPERTY(int recorderElapsed READ recorderElapsed NOTIFY recorderElapsedChanged)
    Q_PROPERTY(int recorderCountdown READ recorderCountdown NOTIFY recorderCountdownChanged)
    // Click-capture (libinput) availability; -1 = not yet probed. Probe lazily via
    // refreshInputPermission() — never at startup.
    Q_PROPERTY(int inputPermissionStatus READ inputPermissionStatus NOTIFY inputPermissionStatusChanged)

public:
    explicit StudioApp(QObject *parent = nullptr);

    // Recording phases surfaced to the shell. Arming = portal negotiating / stream
    // held; Countdown = pre-roll ticking; Finalizing = encoder closing + excise +
    // sidecar write. Mirrors StudioRecorder's internal states plus the app-owned
    // countdown, so the QML can drive one control from a single enum.
    enum RecorderState { RecIdle, RecArming, RecCountdown, RecRecording, RecPaused, RecFinalizing };
    Q_ENUM(RecorderState)

    QString version() const;
    bool devBuild() const;
    StudioSettings *settings() const { return m_settings; }
    bool capVideoPlayback() const;
    QVariantList recentProjects() const;

    int recorderState() const { return m_recorderState; }
    int recorderElapsed() const;
    int recorderCountdown() const { return m_recorderCountdown; }
    int inputPermissionStatus() const { return m_inputPermission; }

    // Wire the QML engine once at startup so per-window editor contexts can be
    // built. Called from main.cpp after the engine exists.
    void setEngine(QQmlEngine *engine);

    // Dev/self-test only: force-show the recording HUD in its idle state so its
    // QML can be instantiated headlessly without a live portal session. Used by
    // main.cpp's hidden --hud-test flag. Returns false if the component failed.
    bool devShowRecordingHud();

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

    // Native folder picker for the projects directory (Settings → Storage).
    // Returns the chosen absolute path, or empty on cancel (QML keeps the old one).
    Q_INVOKABLE QString pickProjectsDirectory(const QString &startDir = QString());

    // Put `text` on the system clipboard (Settings input-permission "Copy" action).
    Q_INVOKABLE void copyToClipboard(const QString &text);

    // --- recording (M2) ---
    // Begin a recording: constructs the recorder on first use (startup stays
    // lazy), negotiates the portal, runs the countdown, then records. finished →
    // the project opens in an editor window.
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void togglePauseRecording();
    Q_INVOKABLE void cancelRecording();

    // Re-probe click-capture (libinput) availability on demand; updates
    // inputPermissionStatus. Cheap, but not free — call it when the UI needs it,
    // not on a timer.
    Q_INVOKABLE void refreshInputPermission();
    // The copyable command that grants click-capture access (adds the user to the
    // `input` group). The UI wraps its own explanatory text around it.
    Q_INVOKABLE QString inputPermissionFixHint() const;

signals:
    void recentProjectsChanged();
    void recorderStateChanged();
    void recorderElapsedChanged();
    void recorderCountdownChanged();
    void inputPermissionStatusChanged();
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

    void ensureRecorder();                 // lazy construct + wire on first use
    void setRecorderState(RecorderState s);
    void onRecorderArmed();                // start the countdown (or commit now)
    void stopCountdown();

    // Parent-owned (constructed with `this`): die with the facade, no leaks.
    StudioSettings *m_settings;
    RecentProjects *m_recent;
    EditorWindowManager *m_editors;
    HudManager *m_hud;

    // Recording. m_recorder is null until the first startRecording() so the app
    // pulls in no PipeWire/libinput state at launch.
    StudioRecorder *m_recorder = nullptr;
    RecorderState m_recorderState = RecIdle;
    QTimer m_countdownTimer;
    int m_recorderCountdown = 0;
    int m_inputPermission = -1;            // -1 = not yet probed
};
