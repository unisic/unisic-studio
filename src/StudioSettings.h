#pragma once
#include <QObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QFile>
#include <QDebug>
#include <QCoreApplication>
#include <SettingMacro.h>   // kit: U_SETTING
#include <ConfigPath.h>     // kit: UnisicKit::filePath()
#include <qqmlregistration.h>

// Persisted Studio settings, one QSettings-backed Q_PROPERTY per line via the
// kit's U_SETTING macro. Deliberately tiny for M0 — it will grow as the
// project/engine/export subsystems land.
//
// The theme (`ui/theme`) is owned by the kit's ThemeController and is NOT
// duplicated here — both write the SAME config file (UnisicKit::filePath()).
//
// LANDMINE: never put a key in a QSettings group named "general"/"General" —
// it collides with INI's magic General section (writes as [%General], parses
// back as group "General", case-sensitive reads miss → resets every launch).
// These are all top-level bare keys (plain [General] section) for that reason.
class StudioSettings : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by StudioApp")

    Q_PROPERTY(QString projectsDirectory READ projectsDirectory WRITE setProjectsDirectory NOTIFY projectsDirectoryChanged)
    Q_PROPERTY(int windowWidth READ windowWidth WRITE setWindowWidth NOTIFY windowWidthChanged)
    Q_PROPERTY(int windowHeight READ windowHeight WRITE setWindowHeight NOTIFY windowHeightChanged)

    // --- recording (M2) ---
    Q_PROPERTY(int recordFps READ recordFps WRITE setRecordFps NOTIFY recordFpsChanged)
    Q_PROPERTY(int masterCrf READ masterCrf WRITE setMasterCrf NOTIFY masterCrfChanged)
    Q_PROPERTY(bool recordSystemAudio READ recordSystemAudio WRITE setRecordSystemAudio NOTIFY recordSystemAudioChanged)
    Q_PROPERTY(bool recordMicrophone READ recordMicrophone WRITE setRecordMicrophone NOTIFY recordMicrophoneChanged)
    Q_PROPERTY(int recordCountdownSec READ recordCountdownSec WRITE setRecordCountdownSec NOTIFY recordCountdownSecChanged)
    Q_PROPERTY(int recordMaxDurationSec READ recordMaxDurationSec WRITE setRecordMaxDurationSec NOTIFY recordMaxDurationSecChanged)
    Q_PROPERTY(bool clickCaptureEnabled READ clickCaptureEnabled WRITE setClickCaptureEnabled NOTIFY clickCaptureEnabledChanged)
    Q_PROPERTY(bool hideWindowWhileRecording READ hideWindowWhileRecording WRITE setHideWindowWhileRecording NOTIFY hideWindowWhileRecordingChanged)
    Q_PROPERTY(bool hudCollapseWhileRecording READ hudCollapseWhileRecording WRITE setHudCollapseWhileRecording NOTIFY hudCollapseWhileRecordingChanged)
    // Optional webcam capture (default OFF): records a v4l2 device into a sidecar
    // alongside the screen, composited as an overlay in the editor.
    Q_PROPERTY(bool recordWebcam READ recordWebcam WRITE setRecordWebcam NOTIFY recordWebcamChanged)
    Q_PROPERTY(QString webcamDevice READ webcamDevice WRITE setWebcamDevice NOTIFY webcamDeviceChanged)

public:
    explicit StudioSettings(QObject *parent = nullptr) : QObject(parent)
    {
        // QSettings only guarantees a flush in its destructor — any abnormal
        // exit (crash, SIGKILL, logout, Ctrl+C) silently loses every change
        // since launch. Debounce a sync() ~800 ms after each write so changes
        // hit disk shortly after being made (pattern from Unisic's Settings).
        m_syncTimer.setSingleShot(true);
        m_syncTimer.setInterval(800);
        connect(&m_syncTimer, &QTimer::timeout, this, [this] { m_s.sync(); });
        // Belt-and-suspenders flush on every quit path (covers exits that skip
        // the QSettings destructor; the self-pipe signal handlers in main.cpp
        // route SIGINT/SIGTERM/SIGHUP through aboutToQuit).
        if (qApp)
            connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { m_s.sync(); });
    }

    static QString defaultProjectsDir()
    {
        // No mkpath here: U_SETTING evaluates the default on every read, so a
        // side effect would run per QML binding evaluation and resurrect the
        // directory even after the user picked a different one and deleted it.
        static const QString d =
            QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)
            + QStringLiteral("/UnisicStudio");
        return d;
    }

    U_SETTING(QString, projectsDirectory, setProjectsDirectory, "projectsDirectory", defaultProjectsDir())
    U_SETTING(int, windowWidth, setWindowWidth, "windowWidth", 1280)
    U_SETTING(int, windowHeight, setWindowHeight, "windowHeight", 800)

    // Recording (M2). Bare top-level keys, no "general" group (the [%General]
    // landmine). recordMaxDurationSec 0 = unlimited; masterCrf is x264 CRF (lower
    // is higher quality); recordFps is the sampler's fixed CFR rate.
    U_SETTING(int, recordFps, setRecordFps, "recordFps", 60)
    U_SETTING(int, masterCrf, setMasterCrf, "masterCrf", 17)
    U_SETTING(bool, recordSystemAudio, setRecordSystemAudio, "recordSystemAudio", true)
    U_SETTING(bool, recordMicrophone, setRecordMicrophone, "recordMicrophone", false)
    U_SETTING(int, recordCountdownSec, setRecordCountdownSec, "recordCountdownSec", 3)
    U_SETTING(int, recordMaxDurationSec, setRecordMaxDurationSec, "recordMaxDurationSec", 0)
    U_SETTING(bool, clickCaptureEnabled, setClickCaptureEnabled, "clickCaptureEnabled", true)
    // Hide the main window while a recording is live (like a screen recorder), so
    // the shell doesn't land in the capture. It returns on stop/cancel/fail.
    U_SETTING(bool, hideWindowWhileRecording, setHideWindowWhileRecording, "hideWindowWhileRecording", true)
    // Collapse the recording HUD to a minimal bottom-edge sliver after commit
    // (expands on hover / while paused). Screencast can't exclude the HUD from the
    // capture, so this is the honest burn-in mitigation. Default ON.
    U_SETTING(bool, hudCollapseWhileRecording, setHudCollapseWhileRecording, "hudCollapseWhileRecording", true)
    // Webcam: default OFF (opt-in, privacy). Device is a v4l2 path.
    U_SETTING(bool, recordWebcam, setRecordWebcam, "recordWebcam", false)
    U_SETTING(QString, webcamDevice, setWebcamDevice, "webcamDevice", QStringLiteral("/dev/video0"))

signals:
    void projectsDirectoryChanged();
    void windowWidthChanged();
    void windowHeightChanged();
    void recordFpsChanged();
    void masterCrfChanged();
    void recordSystemAudioChanged();
    void recordMicrophoneChanged();
    void recordCountdownSecChanged();
    void recordMaxDurationSecChanged();
    void clickCaptureEnabledChanged();
    void hideWindowWhileRecordingChanged();
    void hudCollapseWhileRecordingChanged();
    void recordWebcamChanged();
    void webcamDeviceChanged();

private:
    QSettings m_s{UnisicKit::filePath(), QSettings::IniFormat};
    QTimer m_syncTimer;
};
