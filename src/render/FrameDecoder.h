#pragma once
#include <QAtomicInt>
#include <QImage>
#include <QObject>
#include <QSemaphore>
#include <QSize>
#include <QString>

class QThread;
class QProcess;

// Decodes a trimmed span of the master recording into a stream of raw BGRA
// frames, one QImage per frame, on a dedicated worker thread. It shells out to
//
//   ffmpeg -v error -ss <trimIn> -i <master> -t <dur>
//          -vf fps=<fps>,scale=<w>:<h>:flags=bicubic
//          -f rawvideo -pix_fmt bgra pipe:1
//
// and reads EXACT w*h*4-byte chunks from stdout into QImage(Format_ARGB32)
// frames (ffmpeg `bgra` == ARGB32 byte order on little-endian).
//
// BACKPRESSURE (credit semaphore, N frames ahead)
//   The worker holds N=4 credits. It acquires one credit before emitting each
//   frame, so at most N frames are ever in flight (emitted but not yet consumed)
//   — this bounds memory to N source frames regardless of how far behind the
//   renderer/encoder falls. The consumer calls requestNext() after it finishes a
//   frame, which releases one credit and lets the worker read+emit the next. The
//   worker BLOCKS in its own thread when the consumer is N behind; the consumer
//   never blocks. ffmpeg's stdout pipe naturally back-pressures ffmpeg itself
//   while the worker is parked. (Chosen over a shared blocking queue so the
//   consumer stays purely signal-driven and the GUI thread never waits.)
//
// Teardown: cancel() (or destruction) sets a flag, wakes the parked worker, and
// stops the ffmpeg process on every exit path (kit FfmpegUtil::stopProcess) — no
// orphaned child, no leaked thread.
class FrameDecoder : public QObject
{
    Q_OBJECT

public:
    explicit FrameDecoder(QObject *parent = nullptr);
    ~FrameDecoder() override;

    static QString ffmpegPath();
    static bool available() { return !ffmpegPath().isEmpty(); }

    // Kick off decoding on a worker thread. frameSize is the exact pixel size
    // each emitted frame will have (the video is scaled to it). Emits
    // frameReady()/finished()/error().
    void start(const QString &master, qint64 trimInMs, qint64 durMs, double fps,
               const QSize &frameSize);

    // Consumer ack: releases one credit so the worker may emit one more frame.
    void requestNext();

    // Stop decoding and reap ffmpeg. Safe to call from the consumer thread and
    // idempotent; no finished()/error() is emitted afterwards.
    void cancel();

signals:
    // index is 0-based and monotonic. QImage is an independent copy.
    void frameReady(int index, const QImage &frame);
    void finished(int frameCount);
    void error(const QString &message);

private:
    void run(); // worker-thread entry

    QThread *m_thread = nullptr;
    QSemaphore m_credits;
    QAtomicInt m_cancel{0};

    // Set once before the thread starts; read only on the worker thread.
    QString m_master;
    qint64 m_trimInMs = 0;
    qint64 m_durMs = 0;
    double m_fps = 30.0;
    QSize m_frameSize;
};
