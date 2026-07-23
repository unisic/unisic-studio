#include "RenderPipeline.h"

#include "CursorPlayback.h"
#include "CursorShapeProvider.h"
#include "FrameDecoder.h"
#include "VideoFrameItem.h"
#include "media/FfmpegUtil.h"
#include "project/StyleModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSizeF>
#include <QUrl>
#include <QVariant>

#include <cstring>

namespace {

// Round DOWN to even — H.264/VP9 with yuv420p need even dimensions.
int evenDown(int v)
{
    return v - (v & 1);
}

// Encoder-side video codec args. libx264 (crf, veryfast) is the default; nvenc is
// used only when hardware is available AND requested; vaapi is deliberately out of
// scope for M1 (it needs an hwupload filter chain over the raw pipe). WebM uses
// libvpx-vp9. Graceful fallback to openh264/mpeg4 keeps a GPL-less ffmpeg working.
QStringList videoCodecArgs(const QString &format, int crf, bool preferHardware)
{
    QStringList a;
    if (format == QLatin1String("webm")) {
        a << QStringLiteral("-c:v") << QStringLiteral("libvpx-vp9")
          << QStringLiteral("-crf") << QString::number(crf) << QStringLiteral("-b:v")
          << QStringLiteral("0") << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
        return a;
    }
    if (preferHardware && FfmpegUtil::hardwareEncoderAvailable(QStringLiteral("nvenc"))) {
        a << QStringLiteral("-c:v") << QStringLiteral("h264_nvenc") << QStringLiteral("-preset")
          << QStringLiteral("p5") << QStringLiteral("-cq") << QString::number(crf)
          << QStringLiteral("-b:v") << QStringLiteral("0") << QStringLiteral("-pix_fmt")
          << QStringLiteral("yuv420p") << QStringLiteral("-movflags") << QStringLiteral("+faststart");
        return a;
    }
    const QString enc = FfmpegUtil::encoderUsable(QStringLiteral("libx264"))
                            ? QStringLiteral("libx264")
                            : (FfmpegUtil::encoderUsable(QStringLiteral("libopenh264"))
                                   ? QStringLiteral("libopenh264")
                                   : QStringLiteral("mpeg4"));
    a << QStringLiteral("-c:v") << enc;
    if (enc == QLatin1String("libx264"))
        a << QStringLiteral("-preset") << QStringLiteral("veryfast") << QStringLiteral("-crf")
          << QString::number(crf);
    else if (enc == QLatin1String("libopenh264"))
        a << QStringLiteral("-b:v") << QStringLiteral("4M");
    else
        a << QStringLiteral("-q:v") << QStringLiteral("4");
    a << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << QStringLiteral("-movflags")
      << QStringLiteral("+faststart");
    return a;
}

} // namespace

RenderPipeline::RenderPipeline(QObject *parent)
    : QObject(parent)
{
}

RenderPipeline::~RenderPipeline()
{
    // Reap children and GL resources in the required order even if the owner
    // forgot to cancel/finish.
    if (m_decoder) {
        m_decoder->disconnect(this);
        m_decoder->cancel();
        delete m_decoder; // destructor joins the worker thread
        m_decoder = nullptr;
    }
    if (m_encoder)
        FfmpegUtil::stopProcess(m_encoder);
    if (m_converter)
        FfmpegUtil::stopProcess(m_converter);
    teardownScene();
    if (m_active)
        deleteGifTemps();
    // Torn down while an export was still running — the app was quit mid-export,
    // which routes qApp→~ExportController→~RenderPipeline WITHOUT a cancel()/fail().
    // The file on disk is a truncated partial; remove it so a broken clip can't
    // masquerade as a finished export. A successful finish() has already cleared
    // m_active (its real output is preserved); cancel()/fail() already deleted
    // theirs, so this is then a no-op.
    if (m_active)
        deletePartialOutput();
}

void RenderPipeline::start(const Settings &settings)
{
    m_s = settings;
    m_s.outW = qMax(2, evenDown(m_s.outW));
    m_s.outH = qMax(2, evenDown(m_s.outH));
    const double durSec = qMax<qint64>(0, m_s.durMs) / 1000.0;
    m_totalEstimate = qMax(1, int(qRound(durSec * (m_s.fps > 0 ? m_s.fps : 30.0))));
    m_framesDone = 0;
    m_active = true;
    m_canceled = false;
    m_finishing = false;
    m_wroteOutput = false;

    QString err;
    if (!buildScene(&err)) {
        fail(err);
        return;
    }

    // Two throwaway passes so QML anchors settle and the VideoFrameItem inherits
    // the composition's video-slot size before the first real frame is composed.
    renderOneFrameGeometry();
    renderOneFrameGeometry();

    // The wallpaper/desktopBlur backgrounds decode on Qt's image thread
    // (asynchronous: true) — without this bounded wait the first exported
    // frames render before the image lands and the clip opens on the flat
    // fill colour. User input is excluded; 3 s cap keeps a broken image
    // file from hanging the export start.
    if (m_root && !m_root->property("backgroundReady").toBool()) {
        QElapsedTimer bgWait;
        bgWait.start();
        while (bgWait.elapsed() < 3000 && !m_root->property("backgroundReady").toBool())
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
        renderOneFrameGeometry(); // once more with the loaded image
    }

    startEncoder();
    if (!m_active) // startEncoder failed and already called fail()
        return;

    m_decoder = new FrameDecoder(this);
    connect(m_decoder, &FrameDecoder::frameReady, this, &RenderPipeline::onFrame);
    connect(m_decoder, &FrameDecoder::finished, this, &RenderPipeline::onDecodeFinished);
    connect(m_decoder, &FrameDecoder::error, this, [this](const QString &m) { fail(m); });

    m_clock.start();
    m_decoder->start(m_s.master, m_s.trimInMs, m_s.durMs, m_s.fps, m_s.videoSize);
}

bool RenderPipeline::buildScene(QString *error)
{
    m_camera.setTimeline(m_s.keyframes, m_s.motionSmoothness);

    m_gl = new QOpenGLContext;
    if (!m_gl->create()) {
        *error = tr("Could not create an OpenGL context for rendering.");
        return false;
    }
    m_surface = new QOffscreenSurface;
    m_surface->setFormat(m_gl->format());
    m_surface->create();
    if (!m_surface->isValid()) {
        *error = tr("Could not create an offscreen rendering surface.");
        return false;
    }
    if (!m_gl->makeCurrent(m_surface)) {
        *error = tr("Could not activate the OpenGL rendering context.");
        return false;
    }

    m_rc = new QQuickRenderControl(this);
    m_window = new QQuickWindow(m_rc);
    m_window->setGraphicsDevice(QQuickGraphicsDevice::fromOpenGLContext(m_gl));
    m_window->setColor(Qt::black); // fills any letterbox margin (opaque, not garbage)
    m_window->setGeometry(0, 0, m_s.outW, m_s.outH);

    m_engine = new QQmlEngine(this);
    // The overlay resolves recorded cursor bitmaps through image://cursorshape;
    // the export engine needs its own provider instance (shared static registry).
    m_engine->addImageProvider(QStringLiteral("cursorshape"), new CursorShapeProvider());
    m_component = new QQmlComponent(
        m_engine,
        QUrl(QStringLiteral("qrc:/qt/qml/UnisicStudio/qml/composition/CompositionRoot.qml")));
    if (m_component->isError()) {
        *error = m_component->errorString();
        return false;
    }
    QObject *obj = m_component->create();
    m_root = qobject_cast<QQuickItem *>(obj);
    if (!m_root) {
        delete obj;
        *error = tr("The composition scene failed to load.");
        return false;
    }
    m_root->setParentItem(m_window->contentItem()); // visual only; we own the pointer
    m_root->setWidth(m_s.outW);
    m_root->setHeight(m_s.outH);
    m_root->setProperty("styleModel", QVariant::fromValue(static_cast<QObject *>(m_s.style)));
    m_root->setProperty("videoSize", QVariant::fromValue(QSizeF(m_s.videoSize)));

    // The "desktopBlur" background needs a poster of the first frame. Extract it
    // ONCE here (synchronously — a single ffmpeg frame, ~100 ms) and hand the
    // shared composition the file:// URL; the blur is then computed once on a
    // cached layer, never per frame. Only pay the cost when it's actually the
    // selected background. Deleted in teardownScene() on every exit path.
    if (m_s.style && m_s.style->backgroundType() == QLatin1String("desktopBlur")) {
        const QString exe = FrameDecoder::ffmpegPath();
        if (!exe.isEmpty()) {
            m_posterTemp = QDir(QDir::tempPath())
                               .filePath(QStringLiteral("unisic-studio-poster-%1.png").arg(m_s.projectId));
            const QString ss = QString::number(qMax<qint64>(0, m_s.trimInMs) / 1000.0, 'f', 3);
            QProcess poster;
            poster.start(exe, {QStringLiteral("-y"), QStringLiteral("-loglevel"),
                               QStringLiteral("error"), QStringLiteral("-ss"), ss,
                               QStringLiteral("-i"), m_s.master, QStringLiteral("-frames:v"),
                               QStringLiteral("1"), m_posterTemp});
            if (poster.waitForFinished(5000) && QFile::exists(m_posterTemp)) {
                m_root->setProperty("posterSource", QUrl::fromLocalFile(m_posterTemp).toString());
            } else {
                // Timeout: reap the straggler and remove any partial PNG —
                // clearing the path alone would leak the temp (teardownScene
                // only deletes what m_posterTemp still names).
                poster.kill();
                poster.waitForFinished(1000);
                QFile::remove(m_posterTemp);
                m_posterTemp.clear(); // composition falls back to fill colour
            }
        }
    }

    // Cursor overlay: the recording was captured in Metadata mode, so without
    // this the pointer is invisible. Same object type as the live preview drives.
    // TODO(webcam export): the composition has a webcamSlot and the project
    // carries webcamResolved() (StudioProject), but export does NOT yet composite
    // the webcam — that needs a SECOND FrameDecoder feeding a second VideoFrameItem
    // parented into CompositionRoot.webcamSlot, decoded in lockstep with the master.
    // The preview already shows it; export currently omits it (M4 scope decision).
    m_cursorPlayback = new CursorPlayback(m_s.projectId, this);
    m_cursorPlayback->setTracks(m_s.cursor, m_s.clicks, m_s.videoSize);
    CursorShapeProvider::registerShapes(m_s.projectId, m_s.cursor.shapes());
    m_shapesRegistered = true;
    m_root->setProperty("cursorPlayback",
                        QVariant::fromValue(static_cast<QObject *>(m_cursorPlayback)));
    // Camera at the first exported instant (subsequent frames set it in onFrame).
    m_root->setProperty("zoomRect", m_camera.evaluate(m_s.trimInMs));
    m_cursorPlayback->setTime(m_s.trimInMs);

    auto *slot = qobject_cast<QQuickItem *>(m_root->property("videoSlot").value<QObject *>());
    if (!slot) {
        *error = tr("The composition scene is missing its video slot.");
        return false;
    }
    // Owned (QObject parent = slot) → dies with the composition subtree.
    m_videoItem = new VideoFrameItem(slot);
    auto syncSize = [this, slot] {
        m_videoItem->setSize(QSizeF(slot->width(), slot->height()));
    };
    connect(slot, &QQuickItem::widthChanged, m_videoItem, syncSize);
    connect(slot, &QQuickItem::heightChanged, m_videoItem, syncSize);
    syncSize();

    if (!m_rc->initialize()) {
        // Do NOT suggest QT_QUICK_BACKEND=software here: this pipeline is
        // hard-wired to OpenGL (FBO + fromOpenGLContext) and the software
        // backend cannot render QtQuick.Effects — that advice can never work.
        *error = tr("Offscreen GPU rendering could not be initialized. Exporting "
                    "needs a working OpenGL session (a running desktop with GPU "
                    "drivers); it is not available over a headless connection.");
        return false;
    }

    m_fbo = new QOpenGLFramebufferObject(QSize(m_s.outW, m_s.outH),
                                         QOpenGLFramebufferObject::CombinedDepthStencil);
    m_window->setRenderTarget(
        QQuickRenderTarget::fromOpenGLTexture(m_fbo->texture(), QSize(m_s.outW, m_s.outH)));
    return true;
}

void RenderPipeline::renderOneFrameGeometry()
{
    if (!m_rc || !m_gl)
        return;
    m_gl->makeCurrent(m_surface);
    m_rc->polishItems();
    m_rc->beginFrame();
    m_rc->sync();
    m_rc->render();
    m_rc->endFrame();
}

void RenderPipeline::startEncoder()
{
    const QString exe = FrameDecoder::ffmpegPath();
    if (exe.isEmpty()) {
        fail(tr("ffmpeg was not found on your system — install ffmpeg."));
        return;
    }

    const QString ss = QString::number(qMax<qint64>(0, m_s.trimInMs) / 1000.0, 'f', 6);
    const QString dur = QString::number(qMax<qint64>(0, m_s.durMs) / 1000.0, 'f', 6);

    QStringList args;
    // Common raw-pipe input header (the composed BGRA frames from the renderer).
    // -nostats/-loglevel error: without them ffmpeg's per-frame stats accumulate
    // unread in the QProcess stderr buffer for the whole export (real errors
    // still come through for the failure message).
    args << QStringLiteral("-y") << QStringLiteral("-nostats") << QStringLiteral("-loglevel")
         << QStringLiteral("error") << QStringLiteral("-f") << QStringLiteral("rawvideo")
         << QStringLiteral("-pix_fmt") << QStringLiteral("bgra") << QStringLiteral("-video_size")
         << QStringLiteral("%1x%2").arg(m_s.outW).arg(m_s.outH) << QStringLiteral("-framerate")
         << QString::number(m_s.fps) << QStringLiteral("-i") << QStringLiteral("pipe:0");

    if (isGif()) {
        // GIF has no audio and cannot be piped in one shot (palettegen must see
        // every frame first), so the encoder only writes a LOSSLESS intermediate
        // .mkv here; the two palette passes run once it finishes. ffv1/bgr0 is a
        // lossless reorder of the BGRA pipe (gif carries no alpha), keeping the
        // colours exact for palettegen. Temp beside the output; cleaned on every
        // exit path (finish/fail/cancel/dtor).
        m_gifIntermediate = m_s.outputPath + QStringLiteral(".tmp.mkv");
        m_gifPalette = m_s.outputPath + QStringLiteral(".palette.png");
        // The final gif is rendered to a sibling temp, then moved onto the
        // destination in one step — so the real path never holds a half-written
        // file, and a failed pass leaves no bogus output masquerading as success.
        m_gifFinalTemp = m_s.outputPath + QStringLiteral(".partial.gif");
        args << QStringLiteral("-an") << QStringLiteral("-c:v") << QStringLiteral("ffv1")
             << QStringLiteral("-pix_fmt") << QStringLiteral("bgr0") << m_gifIntermediate;
    } else if (m_s.audioMuted) {
        // Clip audio muted → skip the master input entirely and drop the audio
        // stream (-an): smaller file, and WYSIWYG with the muted preview.
        args << QStringLiteral("-map") << QStringLiteral("0:v:0");
        args += videoCodecArgs(m_s.format, m_s.crf, m_s.preferHardware);
        args << QStringLiteral("-an") << m_s.outputPath;
    } else {
        // Audio (and A/V trim) from the master; ? makes audio optional.
        args << QStringLiteral("-ss") << ss << QStringLiteral("-t") << dur << QStringLiteral("-i")
             << m_s.master << QStringLiteral("-map") << QStringLiteral("0:v:0") << QStringLiteral("-map")
             << QStringLiteral("1:a:0?");
        args += videoCodecArgs(m_s.format, m_s.crf, m_s.preferHardware);
        if (m_s.format == QLatin1String("webm"))
            args << QStringLiteral("-c:a") << QStringLiteral("libopus") << QStringLiteral("-b:a")
                 << QStringLiteral("128k");
        else
            args << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-b:a")
                 << QStringLiteral("192k");
        // Clip volume < 1 → linear gain on the (re-encoded anyway) audio. The
        // same scale the preview's AudioOutput.volume applies. A no-op when the
        // optional 1:a:0? map matched nothing (per-stream filters need a stream).
        if (m_s.audioVolume < 0.9995)
            args << QStringLiteral("-af")
                 << QStringLiteral("volume=%1").arg(QString::number(m_s.audioVolume, 'f', 4));
        // No -shortest: both streams are already bounded (-ss/-t on the master,
        // the pipe delivers exactly durMs*fps frames) — with it, a master whose
        // audio track is shorter than the video would truncate the whole export.
        args << m_s.outputPath;
    }

    m_encoder = new QProcess;
    connect(m_encoder, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (!m_active || m_canceled)
            return;
        if (code == 0 && m_finishing) {
            if (isGif())
                startGifPalettegen(); // intermediate is written; convert to gif
            else
                finish();
        } else {
            QString msg = QString::fromLocal8Bit(m_encoder->readAllStandardError()).trimmed();
            if (msg.isEmpty())
                msg = tr("The encoder exited with code %1.").arg(code);
            fail(msg);
        }
    });
    connect(m_encoder, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!m_active || m_canceled || !m_encoder)
            return;
        fail(tr("Could not run the encoder: %1").arg(m_encoder->errorString()));
    });

    m_encoder->start(exe, args, QIODevice::ReadWrite);
    // errorOccurred(FailedToStart) fires synchronously from inside start() when
    // the exe is missing → the lambda runs fail() → stopProcess() NULLS
    // m_encoder. Never touch it unguarded after start().
    if (!m_encoder)
        return;
    if (!m_encoder->waitForStarted(5000)) {
        const QString reason = m_encoder->errorString();
        fail(tr("Could not start the encoder: %1").arg(reason));
        return;
    }
    // Direct-encode formats: ffmpeg -y truncates the destination the moment it
    // starts — from here on an abort really does leave a partial file that
    // deletePartialOutput() must clean. GIF never sets this: its destination is
    // only ever created by moveIntoPlace() on success, so abort cleanup must
    // not touch a pre-existing file there.
    m_wroteOutput = !isGif();
}

void RenderPipeline::onFrame(int index, const QImage &frame)
{
    if (!m_active || m_canceled || !m_rc)
        return;

    m_gl->makeCurrent(m_surface);
    m_videoItem->setFrame(frame);
    const qint64 tMs = m_s.trimInMs + qint64(qRound(double(index) * 1000.0 / m_s.fps));
    // Camera + cursor for THIS frame — one evaluate path shared with the preview.
    const QRectF cameraRect = m_camera.evaluate(tMs);
    m_root->setProperty("zoomRect", cameraRect);
    if (m_cursorPlayback)
        m_cursorPlayback->setTime(tMs);
    if (m_s.collectMotionSamples) {
        emit motionSampled(index, tMs, cameraRect,
                           m_cursorPlayback
                               ? QPointF(m_cursorPlayback->nx(), m_cursorPlayback->ny())
                               : QPointF(0.5, 0.5),
                           m_cursorPlayback && m_cursorPlayback->cursorVisible());
    }

    m_rc->polishItems();
    m_rc->beginFrame();
    m_rc->sync();
    m_rc->render();
    m_rc->endFrame();

    QImage grabbed = m_fbo->toImage(); // top-down already
    // ffmpeg's 'bgra' rawvideo == QImage::Format_ARGB32 byte order on little-endian
    // (Format_RGB32 is the same layout with an implicit 0xff alpha). Only pay for
    // the full-frame conversion copy when the readback isn't already in that layout;
    // some GL backends hand back ARGB32/RGB32 directly. (This host's FBO yields
    // RGBA8888_Premultiplied, so the convert still runs here.)
    if (grabbed.format() != QImage::Format_ARGB32 && grabbed.format() != QImage::Format_RGB32)
        grabbed.convertTo(QImage::Format_ARGB32);
    writeFrameToEncoder(grabbed);

    m_framesDone = index + 1;
    const qint64 el = m_clock.elapsed();
    const qint64 eta =
        m_framesDone > 0 ? qint64(double(el) / m_framesDone * (m_totalEstimate - m_framesDone)) : 0;
    emit progress(m_framesDone, qMax(m_framesDone, m_totalEstimate), qMax<qint64>(0, eta));

    if (m_decoder)
        m_decoder->requestNext();
}

void RenderPipeline::writeFrameToEncoder(const QImage &grabbed)
{
    if (!m_encoder || m_encoder->state() == QProcess::NotRunning)
        return;
    const int w = grabbed.width();
    const int h = grabbed.height();
    const qint64 rowBytes = qint64(w) * 4;
    if (grabbed.bytesPerLine() == rowBytes) {
        // Scanlines are contiguous (w*4 is 4-byte aligned, as ARGB32 is at this
        // width) — write the whole frame in ONE call instead of h per-row writes.
        // Each small write forced a fresh QRingChunk allocation whenever the encoder
        // pipe fell behind; this collapses ~h allocations/frame to ~1.
        m_encoder->write(reinterpret_cast<const char *>(grabbed.constBits()), rowBytes * h);
    } else {
        // Padded stride: pack the rows into a reused buffer once, then a single
        // write (still one QRingChunk, not h).
        m_rowBuf.resize(rowBytes * h);
        char *dst = m_rowBuf.data();
        for (int y = 0; y < h; ++y) {
            memcpy(dst, grabbed.constScanLine(y), size_t(rowBytes));
            dst += rowBytes;
        }
        m_encoder->write(m_rowBuf.constData(), m_rowBuf.size());
    }
    // Bound the write buffer if the encoder ever falls behind the renderer.
    while (m_encoder && m_encoder->bytesToWrite() > (32 * 1024 * 1024))
        m_encoder->waitForBytesWritten(200);
}

void RenderPipeline::onDecodeFinished(int frameCount)
{
    if (!m_active || m_canceled)
        return;
    m_totalEstimate = qMax(1, frameCount);
    m_finishing = true;
    if (m_encoder && m_encoder->state() != QProcess::NotRunning)
        m_encoder->closeWriteChannel(); // flush + let the encoder finalize the file
    else
        finish();
}

void RenderPipeline::startGifPalettegen()
{
    emit finalizing();
    // Pass 1: read the lossless intermediate, write an optimal 256-colour palette
    // PNG. stats_mode scales with quality (see FfmpegUtil::gifPaletteGenFilter).
    const QString exe = FrameDecoder::ffmpegPath();
    QStringList args;
    args << QStringLiteral("-y") << QStringLiteral("-nostats") << QStringLiteral("-loglevel")
         << QStringLiteral("error") << QStringLiteral("-i") << m_gifIntermediate
         << QStringLiteral("-vf") << FfmpegUtil::gifPaletteGenFilter(m_s.gifQuality) << m_gifPalette;

    m_converter = new QProcess;
    connect(m_converter, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (!m_active || m_canceled || !m_converter)
            return;
        QProcess *conv = m_converter;
        m_converter = nullptr;
        conv->deleteLater();
        if (code == 0)
            startGifPaletteuse();
        else {
            QString msg = QString::fromLocal8Bit(conv->readAllStandardError()).trimmed();
            fail(msg.isEmpty() ? tr("GIF palette generation failed (code %1).").arg(code) : msg);
        }
    });
    connect(m_converter, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!m_active || m_canceled || !m_converter)
            return;
        fail(tr("Could not run the GIF converter: %1").arg(m_converter->errorString()));
    });
    m_converter->start(exe, args, QIODevice::ReadOnly);
}

void RenderPipeline::startGifPaletteuse()
{
    // Pass 2: map the intermediate through the palette to the final .gif. dither
    // scales with quality (FfmpegUtil::gifPaletteUseFilter); ffmpeg defaults the
    // gif muxer to infinite looping (-loop 0), which is what we want.
    const QString exe = FrameDecoder::ffmpegPath();
    const QString lavfi =
        QStringLiteral("[0:v][1:v]%1").arg(FfmpegUtil::gifPaletteUseFilter(m_s.gifQuality));
    QStringList args;
    args << QStringLiteral("-y") << QStringLiteral("-nostats") << QStringLiteral("-loglevel")
         << QStringLiteral("error") << QStringLiteral("-i") << m_gifIntermediate
         << QStringLiteral("-i") << m_gifPalette << QStringLiteral("-lavfi") << lavfi
         << m_gifFinalTemp;

    m_converter = new QProcess;
    connect(m_converter, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (!m_active || m_canceled || !m_converter)
            return;
        QProcess *conv = m_converter;
        m_converter = nullptr;
        conv->deleteLater();
        if (code != 0) {
            QString msg = QString::fromLocal8Bit(conv->readAllStandardError()).trimmed();
            fail(msg.isEmpty() ? tr("GIF rendering failed (code %1).").arg(code) : msg);
            return;
        }
        // paletteuse can exit 0 having written nothing (bad filter, full disk) —
        // check the temp before moving so a silent no-output can't reach finish().
        if (!QFileInfo::exists(m_gifFinalTemp) || QFileInfo(m_gifFinalTemp).size() <= 0) {
            fail(tr("The GIF converter produced no output."));
            return;
        }
        if (!moveIntoPlace(m_gifFinalTemp, m_s.outputPath)) {
            fail(tr("The GIF was rendered but could not be saved to %1.")
                     .arg(QDir::toNativeSeparators(m_s.outputPath)));
            return;
        }
        finish(); // deletes the intermediate + palette via deleteGifTemps()
    });
    connect(m_converter, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!m_active || m_canceled || !m_converter)
            return;
        fail(tr("Could not run the GIF converter: %1").arg(m_converter->errorString()));
    });
    m_converter->start(exe, args, QIODevice::ReadOnly);
}

void RenderPipeline::deleteGifTemps()
{
    if (!m_gifIntermediate.isEmpty() && QFile::exists(m_gifIntermediate))
        QFile::remove(m_gifIntermediate);
    if (!m_gifPalette.isEmpty() && QFile::exists(m_gifPalette))
        QFile::remove(m_gifPalette);
    if (!m_gifFinalTemp.isEmpty() && QFile::exists(m_gifFinalTemp))
        QFile::remove(m_gifFinalTemp);
}

bool RenderPipeline::moveIntoPlace(const QString &src, const QString &dst)
{
    if (src == dst)
        return QFile::exists(dst);
    QFile::remove(dst);
    if (QFile::rename(src, dst)) // same directory → atomic; the common path
        return true;
    // Cross-device rename fails with EXDEV (e.g. a temp on tmpfs vs. a FAT
    // destination) — fall back to copy + remove, same as StudioRecorder::moveFile.
    if (QFile::copy(src, dst)) {
        QFile::remove(src);
        return true;
    }
    return false;
}

void RenderPipeline::finish()
{
    if (!m_active)
        return;
    // A finished export MUST leave a real, non-empty file at the destination.
    // Guards the whole "silent success, no file on disk" class (a muxer that wrote
    // nothing, a stage that exited 0 without producing output): surface it as a
    // visible failure instead of a fake Done. m_active is still true here, so
    // fail() runs.
    if (m_s.outputPath.isEmpty() || !QFileInfo::exists(m_s.outputPath)
            || QFileInfo(m_s.outputPath).size() <= 0) {
        fail(tr("The export finished but produced no output file."));
        return;
    }
    m_active = false;
    emit progress(m_totalEstimate, m_totalEstimate, 0);

    if (m_decoder) {
        m_decoder->disconnect(this);
        m_decoder->cancel();
        delete m_decoder;
        m_decoder = nullptr;
    }
    if (m_encoder)
        FfmpegUtil::stopProcess(m_encoder);
    if (m_converter)
        FfmpegUtil::stopProcess(m_converter);
    teardownScene();
    deleteGifTemps();
    emit finished();
}

void RenderPipeline::fail(const QString &message)
{
    if (!m_active)
        return;
    m_active = false;

    if (m_decoder) {
        m_decoder->disconnect(this);
        m_decoder->cancel();
        delete m_decoder;
        m_decoder = nullptr;
    }
    if (m_encoder)
        FfmpegUtil::stopProcess(m_encoder);
    if (m_converter)
        FfmpegUtil::stopProcess(m_converter);
    teardownScene();
    deletePartialOutput();
    deleteGifTemps();
    emit failed(message);
}

void RenderPipeline::cancel()
{
    if (!m_active && !m_decoder && !m_encoder && !m_rc)
        return;
    m_canceled = true;
    m_active = false;

    if (m_decoder) {
        m_decoder->disconnect(this);
        m_decoder->cancel();
        delete m_decoder;
        m_decoder = nullptr;
    }
    if (m_encoder)
        FfmpegUtil::stopProcess(m_encoder);
    if (m_converter)
        FfmpegUtil::stopProcess(m_converter);
    teardownScene();
    deletePartialOutput();
    deleteGifTemps();
}

void RenderPipeline::deletePartialOutput()
{
    // Only delete what THIS run actually wrote. A GIF abort (or any failure
    // before the encoder started) would otherwise destroy the user's previous
    // export sitting at the same date-stamped destination path.
    if (!m_wroteOutput)
        return;
    if (!m_s.outputPath.isEmpty() && QFile::exists(m_s.outputPath))
        QFile::remove(m_s.outputPath);
}

void RenderPipeline::teardownScene()
{
    // Destruction order the render-control docs demand: make the GL context
    // current, invalidate the scene graph, then delete scene → control → window →
    // engine → fbo, and only then drop the context/surface.
    if (m_gl && m_surface)
        m_gl->makeCurrent(m_surface);
    if (m_rc)
        m_rc->invalidate();

    delete m_root; // composition subtree, incl. the VideoFrameItem child
    m_root = nullptr;
    m_videoItem = nullptr; // was a child of the subtree above
    // Overlay driver (child of this, not the scene subtree) + its shape holds.
    delete m_cursorPlayback;
    m_cursorPlayback = nullptr;
    if (m_shapesRegistered) {
        CursorShapeProvider::releaseProject(m_s.projectId);
        m_shapesRegistered = false;
    }
    delete m_rc;
    m_rc = nullptr;
    delete m_component;
    m_component = nullptr;
    delete m_window;
    m_window = nullptr;
    delete m_engine;
    m_engine = nullptr;
    delete m_fbo;
    m_fbo = nullptr;

    if (m_gl)
        m_gl->doneCurrent();
    delete m_surface;
    m_surface = nullptr;
    delete m_gl;
    m_gl = nullptr;

    // One-shot desktopBlur poster temp (if any) — every export exit path funnels
    // through teardownScene(), so this is the single cleanup point.
    if (!m_posterTemp.isEmpty()) {
        if (QFile::exists(m_posterTemp))
            QFile::remove(m_posterTemp);
        m_posterTemp.clear();
    }
}
