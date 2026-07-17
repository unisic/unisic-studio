#include "RenderPipeline.h"

#include "CursorPlayback.h"
#include "CursorShapeProvider.h"
#include "FrameDecoder.h"
#include "VideoFrameItem.h"
#include "engine/KeyframeEngine.h"
#include "media/FfmpegUtil.h"
#include "project/StyleModel.h"

#include <QFile>
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
    teardownScene();
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

    QString err;
    if (!buildScene(&err)) {
        fail(err);
        return;
    }

    // Two throwaway passes so QML anchors settle and the VideoFrameItem inherits
    // the composition's video-slot size before the first real frame is composed.
    renderOneFrameGeometry();
    renderOneFrameGeometry();

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
    m_root->setProperty("timeMs", double(m_s.trimInMs));

    // Cursor overlay: the recording was captured in Metadata mode, so without
    // this the pointer is invisible. Same object type as the live preview drives.
    m_cursorPlayback = new CursorPlayback(m_s.projectId, this);
    m_cursorPlayback->setTracks(m_s.cursor, m_s.clicks, m_s.videoSize);
    CursorShapeProvider::registerShapes(m_s.projectId, m_s.cursor.shapes());
    m_shapesRegistered = true;
    m_root->setProperty("cursorPlayback",
                        QVariant::fromValue(static_cast<QObject *>(m_cursorPlayback)));
    // Camera at the first exported instant (subsequent frames set it in onFrame).
    m_root->setProperty("zoomRect", KeyframeEngine::evaluate(m_s.keyframes, m_s.trimInMs));
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
        *error = tr("Offscreen GPU rendering could not be initialized on this system. "
                    "Try again with the environment variable QT_QUICK_BACKEND=software.");
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
    args << QStringLiteral("-y") << QStringLiteral("-f") << QStringLiteral("rawvideo")
         << QStringLiteral("-pix_fmt") << QStringLiteral("bgra") << QStringLiteral("-video_size")
         << QStringLiteral("%1x%2").arg(m_s.outW).arg(m_s.outH) << QStringLiteral("-framerate")
         << QString::number(m_s.fps) << QStringLiteral("-i") << QStringLiteral("pipe:0")
         // Audio (and A/V trim) from the master; ? makes audio optional.
         << QStringLiteral("-ss") << ss << QStringLiteral("-t") << dur << QStringLiteral("-i")
         << m_s.master << QStringLiteral("-map") << QStringLiteral("0:v:0") << QStringLiteral("-map")
         << QStringLiteral("1:a:0?");
    args += videoCodecArgs(m_s.format, m_s.crf, m_s.preferHardware);
    if (m_s.format == QLatin1String("webm"))
        args << QStringLiteral("-c:a") << QStringLiteral("libopus") << QStringLiteral("-b:a")
             << QStringLiteral("128k");
    else
        args << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-b:a")
             << QStringLiteral("192k");
    args << QStringLiteral("-shortest") << m_s.outputPath;

    m_encoder = new QProcess;
    connect(m_encoder, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (!m_active || m_canceled)
            return;
        if (code == 0 && m_finishing) {
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
    if (!m_encoder->waitForStarted(5000)) {
        const QString reason = m_encoder->errorString();
        fail(tr("Could not start the encoder: %1").arg(reason));
    }
}

void RenderPipeline::onFrame(int index, const QImage &frame)
{
    if (!m_active || m_canceled || !m_rc)
        return;

    m_gl->makeCurrent(m_surface);
    m_videoItem->setFrame(frame);
    const qint64 tMs = m_s.trimInMs + qint64(qRound(double(index) * 1000.0 / m_s.fps));
    m_root->setProperty("timeMs", double(tMs));
    // Camera + cursor for THIS frame — one evaluate path shared with the preview.
    m_root->setProperty("zoomRect", KeyframeEngine::evaluate(m_s.keyframes, tMs));
    if (m_cursorPlayback)
        m_cursorPlayback->setTime(tMs);

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

void RenderPipeline::finish()
{
    if (!m_active)
        return;
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
    teardownScene();
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
    teardownScene();
    deletePartialOutput();
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
    teardownScene();
    deletePartialOutput();
}

void RenderPipeline::deletePartialOutput()
{
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
}
