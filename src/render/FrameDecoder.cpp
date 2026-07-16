#include "FrameDecoder.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>

// Frames the worker keeps ahead of the consumer (see the header's BACKPRESSURE
// note). Small on purpose: bounds in-flight memory to N source frames.
static constexpr int kCreditsAhead = 4;

FrameDecoder::FrameDecoder(QObject *parent)
    : QObject(parent)
    , m_credits(kCreditsAhead)
{
}

FrameDecoder::~FrameDecoder()
{
    cancel();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
}

QString FrameDecoder::ffmpegPath()
{
    static const QString path = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    return path;
}

void FrameDecoder::start(const QString &master, qint64 trimInMs, qint64 durMs, double fps,
                         const QSize &frameSize)
{
    m_master = master;
    m_trimInMs = trimInMs;
    m_durMs = durMs;
    m_fps = fps > 0 ? fps : 30.0;
    m_frameSize = frameSize;

    m_thread = QThread::create([this] { run(); });
    m_thread->setObjectName(QStringLiteral("FrameDecoder"));
    m_thread->start();
}

void FrameDecoder::requestNext()
{
    m_credits.release(1);
}

void FrameDecoder::cancel()
{
    m_cancel.storeRelease(1);
    // Wake a worker parked on acquire() so it can observe the cancel flag.
    m_credits.release(kCreditsAhead + 1);
}

void FrameDecoder::run()
{
    const QString exe = ffmpegPath();
    const int w = m_frameSize.width();
    const int h = m_frameSize.height();
    if (exe.isEmpty()) {
        if (!m_cancel.loadAcquire())
            emit error(tr("ffmpeg was not found on your system — install ffmpeg."));
        return;
    }
    if (w <= 0 || h <= 0) {
        if (!m_cancel.loadAcquire())
            emit error(tr("Invalid frame size for decoding."));
        return;
    }

    const double ss = qMax<qint64>(0, m_trimInMs) / 1000.0;
    const double dur = qMax<qint64>(0, m_durMs) / 1000.0;
    const QStringList args = {
        QStringLiteral("-v"),       QStringLiteral("error"),
        QStringLiteral("-nostdin"),
        QStringLiteral("-ss"),      QString::number(ss, 'f', 6),
        QStringLiteral("-i"),       m_master,
        QStringLiteral("-t"),       QString::number(dur, 'f', 6),
        QStringLiteral("-vf"),
        QStringLiteral("fps=%1,scale=%2:%3:flags=bicubic").arg(m_fps).arg(w).arg(h),
        QStringLiteral("-f"),       QStringLiteral("rawvideo"),
        QStringLiteral("-pix_fmt"), QStringLiteral("bgra"),
        QStringLiteral("pipe:1")};

    auto *proc = new QProcess;
    proc->start(exe, args, QIODevice::ReadOnly);
    if (!proc->waitForStarted(5000)) {
        const QString reason = proc->errorString();
        delete proc;
        if (!m_cancel.loadAcquire())
            emit error(tr("Could not start ffmpeg: %1").arg(reason));
        return;
    }

    // Blocking teardown is fine HERE: this runs on the decoder's own worker
    // thread, so a terminate/kill/wait never freezes the GUI. (The GUI-thread
    // encoder in RenderPipeline uses the non-blocking FfmpegUtil::stopProcess
    // instead, for the same reason in reverse.)
    auto killProc = [&proc] {
        if (!proc)
            return;
        proc->closeWriteChannel();
        if (proc->state() != QProcess::NotRunning) {
            proc->terminate();
            if (!proc->waitForFinished(1000)) {
                proc->kill();
                proc->waitForFinished(1000);
            }
        }
        delete proc;
        proc = nullptr;
    };

    const qint64 frameBytes = qint64(w) * h * 4;
    int index = 0;
    bool aborted = false;
    bool failed = false;

    for (;;) {
        m_credits.acquire(1);
        if (m_cancel.loadAcquire()) { aborted = true; break; }

        QImage img(w, h, QImage::Format_ARGB32);
        // Format_ARGB32 packs each scanline at exactly w*4 bytes (already a
        // multiple of 4), so the buffer is contiguous and ffmpeg's tightly
        // packed bgra output reads straight into it.
        uchar *dst = img.bits();
        qint64 got = 0;
        while (got < frameBytes) {
            if (m_cancel.loadAcquire()) { aborted = true; break; }
            const qint64 n = proc->read(reinterpret_cast<char *>(dst) + got, frameBytes - got);
            if (n > 0) {
                got += n;
            } else if (n == 0) {
                if (proc->state() == QProcess::NotRunning && proc->bytesAvailable() == 0)
                    break; // EOF
                if (!proc->waitForReadyRead(15000)) {
                    if (proc->state() == QProcess::NotRunning)
                        break; // finished between checks
                    failed = true;
                    break; // stalled
                }
            } else {
                failed = true; // read error
                break;
            }
        }
        if (aborted || failed)
            break;
        if (got == frameBytes) {
            emit frameReady(index++, img);
            continue;
        }
        break; // clean EOF (got == 0) or trailing partial frame
    }

    QByteArray errOut;
    int exitCode = 0;
    if (proc) {
        errOut = proc->readAllStandardError();
        if (!aborted)
            proc->waitForFinished(2000);
        exitCode = proc->exitCode();
    }
    killProc();

    if (aborted)
        return; // cancel(): stay silent
    if (failed || exitCode != 0) {
        QString msg = QString::fromLocal8Bit(errOut).trimmed();
        if (msg.isEmpty())
            msg = tr("ffmpeg exited with code %1 while decoding.").arg(exitCode);
        emit error(msg);
        return;
    }
    emit finished(index);
}
