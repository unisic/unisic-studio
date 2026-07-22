#pragma once
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantList>
#include <QVector>
#include <functional>
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
class QQuickWindow;     // fwd-decl: the shell window handle (lifetime rule)

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
    // Whether a portal source pick is stored (gates the Settings "Forget" row).
    // The token itself stays out of QML — it's an opaque portal handle.
    Q_PROPERTY(bool hasRememberedScreencastSource READ hasRememberedScreencastSource
               NOTIFY hasRememberedScreencastSourceChanged)

    // Recording state machine, exposed thin (the logic lives in StudioRecorder).
    Q_PROPERTY(int recorderState READ recorderState NOTIFY recorderStateChanged)
    Q_PROPERTY(int recorderElapsed READ recorderElapsed NOTIFY recorderElapsedChanged)
    Q_PROPERTY(int recorderCountdown READ recorderCountdown NOTIFY recorderCountdownChanged)
    // Click-capture (libinput) availability; -1 = not yet probed. Probe lazily via
    // refreshInputPermission() — never at startup.
    Q_PROPERTY(int inputPermissionStatus READ inputPermissionStatus NOTIFY inputPermissionStatusChanged)

    // Dev-build smoke test transcript + running flag (Developer pane / F8). A
    // single running QString of PASS/FAIL/SKIP lines, live-updated per line.
    Q_PROPERTY(QString smokeTestLog READ smokeTestLog NOTIFY smokeTestChanged)
    Q_PROPERTY(bool smokeTestRunning READ smokeTestRunning NOTIFY smokeTestChanged)

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
    QString smokeTestLog() const { return m_smokeLog; }
    bool smokeTestRunning() const { return m_smokeRunning; }

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

    // Delete a recording from disk and drop it from the recents grid: the
    // sidecar, the master video it references, and any webcam clip. Trash-first
    // (recoverable), plain remove() fallback on mounts without a trash dir.
    // Refuses while the project is open in an editor window. IRREVERSIBLE past
    // the trash — QML must confirm first (the grid's UConfirmDialog does).
    // Outcome (including trash vs permanent) is surfaced via notified();
    // returns true once the recording is gone from disk and the grid.
    Q_INVOKABLE bool deleteRecording(const QString &pathOrUrl);

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
    // QDesktopServices). Used by the export "Show in folder" action.
    Q_INVOKABLE void revealInFolder(const QString &path);

    // Open `path` itself in the user's default app (the export "Open file" action).
    Q_INVOKABLE void openFile(const QString &path);

    // Native folder picker for the export destination directory. Returns the chosen
    // absolute path, or empty on cancel (the dialog keeps the previous folder).
    Q_INVOKABLE QString pickExportFolder(const QString &startDir = QString());

    // Native folder picker for the projects directory (Settings → Storage).
    // Returns the chosen absolute path, or empty on cancel (QML keeps the old one).
    Q_INVOKABLE QString pickProjectsDirectory(const QString &startDir = QString());

    // Put `text` on the system clipboard (Settings input-permission "Copy" action).
    Q_INVOKABLE void copyToClipboard(const QString &text);

    // Overwrite-confirmation support for the export dialog.
    Q_INVOKABLE bool fileExists(const QString &path) const;

    // --- recording (M2) ---
    // Begin a recording: constructs the recorder on first use (startup stays
    // lazy), negotiates the portal, runs the countdown, then records. finished →
    // the project opens in an editor window.
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();
    Q_INVOKABLE void togglePauseRecording();
    Q_INVOKABLE void cancelRecording();
    // Drop the stored portal source pick, so the next recording asks again.
    Q_INVOKABLE void forgetScreencastSource();
    bool hasRememberedScreencastSource() const;

    // The shell window, for explicit lifetime: the process ends when the last
    // window the user can see is gone (see maybeQuitOnAllWindowsClosed).
    void setMainWindow(QQuickWindow *win);

    // Re-show and raise the projects/launcher window. Closing it while an editor
    // is open only HIDES it (it is the QML engine root, not destroyed); without
    // this the session was orphaned — no way back to the launcher, and the next
    // editor-close would silently quit. The editor's "Projects" affordance calls
    // this. Mirrors the single-instance raise in main.cpp.
    Q_INVOKABLE void showMainWindow();

    // --- auto-zoom (M3) --------------------------------------------------------
    // Re-run the auto-camera with the project's current Params, preserving Manual
    // and locked keyframes (clearAuto → generate(pinned) → insert). Also the
    // entry point the project-open path uses to seed a fresh recording.
    Q_INVOKABLE void regenerateZoom(StudioProject *project);

    // Add a Manual zoom keyframe at tMs framed on (cx,cy) with linear factor
    // `zoom` (>=1); geometry matches the auto camera exactly. Returns its row.
    Q_INVOKABLE int addManualZoom(StudioProject *project, qint64 tMs, double cx, double cy,
                                  double zoom);
    // Add a Manual full-frame (reset) keyframe at tMs. Returns its row.
    Q_INVOKABLE int addResetZoom(StudioProject *project, qint64 tMs);
    // Rescale keyframe `index`'s rect around its centre to linear factor `zoom`.
    Q_INVOKABLE void setZoomFactor(StudioProject *project, int index, double zoom);
    // Shift keyframe `index`'s camera centre by (dxFrac,dyFrac) of the frame,
    // clamped so the rect stays inside [0,1] (its size is preserved).
    Q_INVOKABLE void nudgeZoom(StudioProject *project, int index, double dxFrac, double dyFrac);
    // The linear zoom factor of keyframe `index` (seeds the inspector slider).
    Q_INVOKABLE double zoomFactorOf(StudioProject *project, int index);
    // Aspect that keyframe rects must carry to display undistorted (output
    // aspect in fill mode, "source" in fit mode).
    static QString rectAspectFor(StudioProject *project);

    // Re-probe click-capture (libinput) availability on demand; updates
    // inputPermissionStatus. Cheap, but not free — call it when the UI needs it,
    // not on a timer.
    Q_INVOKABLE void refreshInputPermission();
    // The copyable command that grants click-capture access (adds the user to the
    // `input` group). The UI wraps its own explanatory text around it.
    Q_INVOKABLE QString inputPermissionFixHint() const;

    // Whether a v4l2 webcam device node exists (Settings gates the webcam toggle
    // on it). Cheap stat; call from QML when the pane opens, not on a timer.
    Q_INVOKABLE bool webcamDeviceAvailable(const QString &device) const;

    // --- developer aids (dev builds only; each is a no-op in a release build) ---
    // Sequential smoke test across every load-bearing path (project roundtrip,
    // engine generate, video probe, offscreen render + MP4 export, GIF pass,
    // recorder arm+cancel). Reports PASS/FAIL/SKIP per step into smokeTestLog.
    Q_INVOKABLE void runSmokeTest();
    // Single-action dev buttons mirroring each user-facing path.
    Q_INVOKABLE void devImportTestVideo();   // generate a testsrc + import it
    Q_INVOKABLE void devOpenRecordingHud();  // show the HUD in its idle state
    Q_INVOKABLE void devRunExportTest();     // export a generated clip to MP4
    Q_INVOKABLE void devRunAutozoomTest();   // run the auto-zoom self-test

signals:
    void recentProjectsChanged();
    void hasRememberedScreencastSourceChanged();
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

    // Smoke test transcript changed (a line appended, or running toggled).
    void smokeTestChanged();

private:
    bool doSave(StudioProject *project, const QString &path);
    QString defaultSavePath(StudioProject *project) const;

    // Auto-camera generation. onlyIfEmpty: skip when the timeline already has
    // keyframes (the open-path seeding rule). Runs synchronously — measured well
    // under the 50 ms budget even on a 10-min track (see --autozoom-test).
    void generateZoom(StudioProject *project, bool onlyIfEmpty);

    void ensureRecorder();                 // lazy construct + wire on first use
    // Explicit process lifetime: quit when the shell window is hidden AND no
    // editor windows remain AND no recording is live. quitOnLastWindowClosed
    // proved racy here (the process intermittently outlived its last window),
    // so the rule is enforced deliberately instead of inferred by Qt.
    void maybeQuitOnAllWindowsClosed();
    void setRecorderState(RecorderState s);
    void onRecorderArmed();                // start the countdown (or commit now)
    void stopCountdown();

    // Smoke-test machinery (dev builds). smokeNext() drives the step queue;
    // smokeLog() appends a line and emits. devMakeTestVideo() writes a short
    // testsrc clip into m_smokeDir and returns its path (empty on failure).
    // runSmokeExport() is a shared step body for the MP4/GIF export checks.
    void smokeLog(const QString &line);
    void smokeNext();
    QString devMakeTestVideo();
    void runSmokeExport(const QString &format, const QString &label);

    // The shell window (owned by the QML engine; QPointer so a destroyed window
    // reads as "not visible" instead of dangling).
    QPointer<QQuickWindow> m_mainWin;

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

    // Smoke test (dev builds). One run at a time; a QString transcript plus a
    // lambda step queue (mirrors Unisic's AppContext::runSmokeTest).
    QString m_smokeLog;
    bool m_smokeRunning = false;
    QVector<std::function<void()>> m_smokeSteps;
    int m_smokeIdx = 0;
    QString m_smokeDir;                    // temp dir for this run's fixtures/outputs
};
