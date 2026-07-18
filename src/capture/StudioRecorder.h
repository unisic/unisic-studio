#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVector>
#include <functional>

#include "RecordingAssembler.h"   // neutral Raw* structs (NO kit/project CursorSample)

class StudioSettings;
class ScreenCastSession;
class PipeWireGrabber;   // fwd-decl only: including its header would drag the kit
class ClickCapture;      // CursorSample struct in, colliding with the project one
class QProcess;

// The recording session orchestrator. Modeled on Unisic's GifRecorder but leaner:
// no GIF, no instant replay, no window/region cropping — it records the full
// stream the portal picker hands over (monitor OR window; the picker chooses).
//
// Pipeline:
//   portal ScreenCast (CursorMode::Metadata, MONITOR|WINDOW)
//     -> PipeWireGrabber (latest frame + out-of-band cursor samples)
//     -> fixed-FPS wall-clock sampler -> ffmpeg (rawvideo stdin -> master .mkv,
//        H.264/CRF + FLAC) in the XDG cache
//   cursor samples + libinput clicks + cursor shapes accumulate in parallel
//   on stop: (optional pause excision) -> build CursorTrack/ClickTrack in video
//   time -> write the master + .unisicstudio sidecar into the projects directory.
//
// Commit hold (mirrors GifRecorder): start(holdForCommit=true) negotiates the
// portal FIRST, emits armed() when the stream is live, then WAITS — encoding
// begins only on commit(), so the portal dialog and the UI countdown never land
// in the file.
//
// Time base: everything is CLOCK_MONOTONIC. t0MonoNs is the pts of the first
// SAMPLED frame; cursor/click timestamps map to video-ms via RecorderMath.
//
// The project-model assembly (kit CursorSample vs project CursorSample collide in
// one TU) is delegated to RecordingAssembler, which this class never mixes with
// the kit grabber header.
class StudioRecorder : public QObject
{
    Q_OBJECT
public:
    explicit StudioRecorder(StudioSettings *settings, QObject *parent = nullptr);
    ~StudioRecorder() override;

    bool recording() const { return m_state != Idle; }
    bool isPaused() const { return m_paused; }   // NB: not paused() — that is the signal
    bool canPause() const { return m_state == Recording; }
    // Recorded (not wall-clock) seconds: frozen during a pause and excluding every
    // completed pause, so it matches the excised master.
    int elapsedSeconds() const;

    // holdForCommit=true: negotiate the portal, arm, and WAIT for commit() before
    // encoding (the caller runs its countdown in between). false: begin encoding
    // as soon as the stream format is ready.
    void start(bool holdForCommit);
    void commit();          // release a hold: begin encoding now. No-op unless armed.
    void togglePause();     // v1: sampler freezes on the last frame, audio flows on
    void stop();            // finalize -> build project -> finished(projectPath)
    void cancel();          // discard everything, delete temps, no orphans

signals:
    void armed();                          // stream live, encoding held for commit()
    void started();                        // encoding began
    void paused(bool paused);
    // stop() ran (user OR recorder-initiated: max-duration timer, portal session
    // closed) — the facade must show Finalizing, not a still-ticking Recording.
    void finalizing();
    void elapsedChanged();                 // ~1 Hz while recording
    void failed(const QString &error);     // "cancelled" is filtered by the UI
    void finished(const QString &projectPath);   // path to the .unisicstudio sidecar

private:
    enum State { Idle, Starting, Recording, Finalizing };

    void openPortalSession();
    void onStreamReady(int fd, uint nodeId, const QSize &size, const QPoint &pos);
    void onFormatReady(const QSize &streamSize);
    void beginEncoding(const QSize &streamSize);
    void sampleFrame();
    void drainCursorSamples();
    void onEncoderFinished(int code, bool crashed);
    void finalize();                       // build tracks + project, save sidecar
    void maybeExcise(std::function<void()> then);
    // Move the raw capture out of the crash-sweep's reach after a finalize
    // failure; clears m_rawMasterPath and returns the surviving path.
    QString rescueRawMaster();
    void stopGrabber();
    void teardownProcesses();
    void cleanup();
    void fail(const QString &msg);

    // Optional webcam sidecar capture (a SECOND, independent ffmpeg reading the
    // v4l2 device). Best-effort: a webcam failure never aborts the screen
    // recording. startWebcamCapture spawns it when the setting is on and the
    // device exists; stopWebcamCapture stops it cleanly (writes 'q') so the mkv
    // is finalized before finalize() moves it next to the master.
    void startWebcamCapture(const QString &base);
    void stopWebcamCapture();

    static qint64 nowMonoNs();
    static bool moveFile(const QString &src, const QString &dst);
    // ffmpeg args cutting the given video-ms pause spans from input into output,
    // the SAME ranges from video and audio so they stay synced. Static for the
    // reader; replicates GifRecorder::pauseExciseArgs (kit FfmpegUtil has none).
    static QStringList pauseExciseArgs(const QString &input, const QString &output,
                                       const QList<QPair<qint64, qint64>> &videoMsRanges,
                                       bool hasAudio, int crf);

    StudioSettings *m_settings;
    State m_state = Idle;

    // Commit hold.
    bool m_holdForCommit = false;
    bool m_armed = false;
    bool m_committed = false;
    QSize m_heldStreamSize;

    ScreenCastSession *m_session = nullptr;
    PipeWireGrabber *m_grabber = nullptr;   // owned; guarded by HAVE_PIPEWIRE in .cpp
    ClickCapture *m_clicks = nullptr;
    QProcess *m_ffmpeg = nullptr;
    QProcess *m_converter = nullptr;   // pause-excision pass
    QProcess *m_webcamProc = nullptr;  // optional v4l2 webcam sidecar encoder
    QString m_webcamRawPath;           // encoder target in the XDG cache
    QString m_webcamFinalPath;         // final _webcam.mkv beside the master

    QTimer m_sampler;
    QTimer m_maxTimer;
    QTimer m_elapsedTick;
    QTimer m_cursorDrain;
    QElapsedTimer m_elapsed;

    // Video / pacing.
    int m_fps = 60;
    qint64 m_framesWritten = 0;
    QSize m_streamSize;    // full delivered stream (frame-size sanity check)
    QSize m_encodeSize;    // even-clamped size actually encoded
    QString m_pixelFormat = QStringLiteral("bgra");
    bool m_hasAudio = false;
    QByteArray m_lastFrame;
    quint64 m_lastSampledSeq = 0;
    // Countdown pre-roll guard (see beginEncoding/sampleFrame): the newest frame
    // seq at commit; sampleFrame skips it until fresh damage arrives.
    quint64 m_preRollSeq = 0;
    bool m_awaitFreshFrame = false;

    // Time base.
    qint64 m_t0MonoNs = 0;
    bool m_haveT0 = false;

    // Pause bookkeeping.
    bool m_paused = false;
    qint64 m_pauseStartMonoNs = 0;
    qint64 m_pauseStartElapsedMs = 0;
    qint64 m_pausedTotalMs = 0;
    qint64 m_maxRemainingMs = 0;
    QList<QPair<qint64, qint64>> m_pauseIntervals;   // completed [startMonoNs,endMonoNs)

    // Track accumulation (raw; converted to video-time only at finalize). Neutral
    // Raw* structs so this TU never includes a CursorSample-defining header.
    QVector<RecordingAssembler::RawCursor> m_cursorRaw;
    QVector<RecordingAssembler::RawClick> m_clickRaw;
    QVector<RecordingAssembler::RawShape> m_shapes;

    // Recording metadata for the sidecar.
    QString m_cursorMode = QStringLiteral("none");   // metadata|embedded|none
    bool m_hadClickCapture = false;

    // Paths.
    QString m_rawMasterPath;   // encoder target in the XDG cache
    QString m_masterPath;      // final .mkv in the projects directory
    QString m_sidecarPath;     // final .unisicstudio next to it
};
