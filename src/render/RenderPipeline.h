#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QSize>
#include <QString>

#include "engine/KeyframeEngine.h"
#include "project/ClickTrack.h"
#include "project/CursorTrack.h"
#include "project/ZoomTimeline.h"

class QOpenGLContext;
class CursorPlayback;
class QOffscreenSurface;
class QOpenGLFramebufferObject;
class QQuickRenderControl;
class QQuickWindow;
class QQmlEngine;
class QQmlComponent;
class QQuickItem;
class QProcess;
class StyleModel;
class VideoFrameItem;
class FrameDecoder;

// The export orchestrator. It renders THE SAME qml/composition/CompositionRoot.qml
// that drives the live editor preview, offscreen, at export resolution — so there
// is ONE styling implementation and export is WYSIWYG by construction. Nothing
// here re-derives the look; it only feeds CompositionRoot the project's StyleModel
// + a VideoFrameItem fed by decoded frames, grabs each composed frame, and pipes
// it to an ffmpeg encoder.
//
// RENDER BACKEND (known Wayland/EGL sharp edge — read before touching)
//   Offscreen QQuickRenderControl needs a live RHI. The RHI's own private texture
//   render-target headers are not part of the public SDK, so this uses the public
//   route: force the OpenGL RHI (QQuickWindow::setGraphicsApi(OpenGL), done once in
//   main.cpp), create our own QOpenGLContext + QOffscreenSurface, and render into a
//   QOpenGLFramebufferObject exposed via QQuickRenderTarget::fromOpenGLTexture. The
//   OpenGL RHI backend renders QtQuick.Effects (MultiEffect shadow/mask) correctly,
//   unlike the software backend — which is why software is NOT the primary path.
//   Per-driver, initialize() can still fail (notably under the `offscreen`/`xcb`
//   QPA plugins on some Mesa/EGL stacks). On failure we emit a clear error telling
//   the user to retry with QT_QUICK_BACKEND=software; we do NOT ship a second
//   render path in M1.
//
// FRAME PACING
//   Frames arrive from FrameDecoder via a queued frameReady() signal — i.e. one
//   frame per event-loop iteration already — so the GUI thread never blocks on a
//   long render loop. Each frame: push into the VideoFrameItem, set timeMs, sync +
//   render the scene graph, read back the FBO, write raw BGRA to the encoder, then
//   ack the decoder for the next frame.
//
// TEARDOWN
//   cancel()/failure/normal-finish all funnel through the same teardown: stop the
//   decoder (worker joined), stop the encoder (non-blocking FfmpegUtil::stopProcess),
//   delete a partial output on abort, and destroy the render-control stack in the
//   order the docs require (context current → invalidate → scene → control → window
//   → engine → fbo → doneCurrent → surface → context). No orphan ffmpeg, no leaked
//   FBO/engine.
class RenderPipeline : public QObject
{
    Q_OBJECT

public:
    struct Settings {
        QString master;        // resolved absolute path to the source video
        StyleModel *style = nullptr;
        QSize videoSize;       // source pixel size (decode resolution)
        qint64 trimInMs = 0;
        qint64 durMs = 0;      // exported span length
        double fps = 30.0;     // export frame rate
        int outW = 1920;       // export width  (even-guarded by start())
        int outH = 1080;       // export height (even-guarded by start())
        QString format = QStringLiteral("mp4"); // "mp4" | "webm" | "gif"
        int crf = 22;          // quality (lower = better)
        int gifQuality = 1;    // gif palette quality 0..2 (fast/small → best); unused off gif
        bool preferHardware = false;
        QString outputPath;
        // --- camera + cursor overlay (M3) ---
        QList<ZoomTimeline::Keyframe> keyframes; // camera timeline snapshot
        double motionSmoothness = ZoomTimeline::DefaultMotionSmoothness;
        CursorTrack cursor;    // overlay source (copies — decoupled, thread-safe)
        ClickTrack clicks;
        QString projectId;     // keys the image://cursorshape bitmaps
        bool collectMotionSamples = false; // dev harness only; zero hot-path signals otherwise
    };

    explicit RenderPipeline(QObject *parent = nullptr);
    ~RenderPipeline() override;

    // Begin exporting. Emits progress()/finished()/failed(). One export per object.
    void start(const Settings &settings);

    // Abort: reaps ffmpeg, deletes the partial output, tears the scene down. No
    // finished()/failed() follows.
    void cancel();

    int totalFramesEstimate() const { return m_totalEstimate; }

signals:
    void progress(int framesDone, int totalEstimate, qint64 etaMs);
    void motionSampled(int frameIndex, qint64 timeMs, const QRectF &cameraRect,
                       const QPointF &cursorPosition, bool cursorVisible);
    void finished();
    void failed(const QString &message);

private:
    bool isGif() const { return m_s.format == QLatin1String("gif"); }
    bool buildScene(QString *error);   // GL context + render control + CompositionRoot
    void startEncoder();
    void onFrame(int index, const QImage &frame);
    void onDecodeFinished(int frameCount);
    void renderOneFrameGeometry();     // one throwaway pass to settle QML layout
    void writeFrameToEncoder(const QImage &grabbed);
    // GIF is a true two-pass palette conversion (mirrors Unisic's GifRecorder):
    // the encoder writes a LOSSLESS intermediate .mkv from the raw pipe, then
    // pass 1 (palettegen) writes a palette PNG and pass 2 (paletteuse) renders
    // the .gif. A single split-graph command buffers every frame in RAM, so the
    // temp-file route is deliberate.
    void startGifPalettegen();         // pass 1: intermediate → palette PNG
    void startGifPaletteuse();         // pass 2: intermediate + palette → gif
    void deleteGifTemps();
    // Move a finished temp file onto the destination: same-dir rename (atomic) with
    // a cross-device copy+remove fallback (e.g. temp fs != a FAT destination).
    static bool moveIntoPlace(const QString &src, const QString &dst);
    void finish();                     // clean success finalize
    void fail(const QString &message);
    void teardownScene();
    void deletePartialOutput();

    Settings m_s;
    int m_totalEstimate = 1;
    int m_framesDone = 0;
    bool m_active = false;
    bool m_canceled = false;
    bool m_finishing = false;          // decoder done; draining encoder
    QElapsedTimer m_clock;

    // Render-control stack (owned; destroyed in teardownScene()).
    QOpenGLContext *m_gl = nullptr;
    QOffscreenSurface *m_surface = nullptr;
    QQuickRenderControl *m_rc = nullptr;
    QQuickWindow *m_window = nullptr;
    QQmlEngine *m_engine = nullptr;
    QQmlComponent *m_component = nullptr;
    QQuickItem *m_root = nullptr;       // CompositionRoot instance (owns m_videoItem)
    VideoFrameItem *m_videoItem = nullptr;
    CursorPlayback *m_cursorPlayback = nullptr; // child of this; drives the overlay
    bool m_shapesRegistered = false;    // release CursorShapeProvider on teardown
    QOpenGLFramebufferObject *m_fbo = nullptr;

    FrameDecoder *m_decoder = nullptr;  // child of this
    QProcess *m_encoder = nullptr;      // reaped via FfmpegUtil::stopProcess
    QProcess *m_converter = nullptr;    // gif palette pass (reaped via FfmpegUtil::stopProcess)
    QString m_gifIntermediate;          // lossless .mkv the encoder writes for gif
    QString m_gifPalette;               // palette .png from pass 1
    QString m_gifFinalTemp;             // paletteuse writes here, then moved to output
    QString m_posterTemp;               // one-shot poster PNG for the desktopBlur bg
    QByteArray m_rowBuf;                // reused row-pack scratch (padded-stride path only)
    SpringCameraEvaluator m_camera;
};
