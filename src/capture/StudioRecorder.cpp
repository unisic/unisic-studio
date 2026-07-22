#include "StudioRecorder.h"

#include "ClickCapture.h"
#include "InputPermission.h"
#include "RecorderMath.h"
#include "StudioSettings.h"

#include "capture/ScreenCastSession.h"   // kit
#include "media/FfmpegUtil.h"            // kit: stopProcess
#include "record/PipeWireGrabber.h"      // kit: grabber + CursorSample (kit variant)

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QProcess>
#include <QStandardPaths>
#include <QtConcurrent>

#include <cstring>
#include <ctime>

StudioRecorder::StudioRecorder(StudioSettings *settings, QObject *parent)
    : QObject(parent), m_settings(settings)
{
    m_sampler.setTimerType(Qt::PreciseTimer);
    connect(&m_sampler, &QTimer::timeout, this, &StudioRecorder::sampleFrame);

    m_maxTimer.setSingleShot(true);
    connect(&m_maxTimer, &QTimer::timeout, this, &StudioRecorder::stop);

    // 1 s tick: the readout shows whole seconds, so a faster tick would wake the
    // GUI thread for identical text.
    m_elapsedTick.setInterval(1000);
    m_elapsedTick.setTimerType(Qt::CoarseTimer);
    connect(&m_elapsedTick, &QTimer::timeout, this, &StudioRecorder::elapsedChanged);

    // Cursor meta arrives at the metadata rate; drain it in bulk a couple of times
    // a second rather than polling the grabber every sampler tick.
    m_cursorDrain.setInterval(500);
    connect(&m_cursorDrain, &QTimer::timeout, this, &StudioRecorder::drainCursorSamples);

    m_clicks = new ClickCapture(this);
    // buttonEvent is emitted from the libinput poll thread → queued explicitly (the
    // append below then runs safely on the GUI thread, single-writer).
    connect(m_clicks, &ClickCapture::buttonEvent, this,
            [this](qint64 tUsec, int button, bool pressed) {
                // Only inside the recording window; a stray event outside it has no
                // place on the timeline. Clicks inside a pause are excised later.
                if (m_state == Recording)
                    m_clickRaw.append({tUsec * 1000, button, pressed});
            }, Qt::QueuedConnection);
    // Key-DOWN timing only (never keycodes) — collected as ns to coalesce into
    // typing bursts at assembly. Queued: emitted from the poll thread.
    connect(m_clicks, &ClickCapture::keyActivity, this,
            [this](qint64 tUsec) {
                if (m_state == Recording)
                    m_keyDownRaw.append(tUsec * 1000);
            }, Qt::QueuedConnection);
}

StudioRecorder::~StudioRecorder()
{
    cancel();
}

qint64 StudioRecorder::nowMonoNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

bool StudioRecorder::moveFile(const QString &src, const QString &dst)
{
    QFile::remove(dst);
    if (QFile::rename(src, dst))
        return true;
    // Cross-device (cache on one fs, projects dir on another — e.g. a FAT drive):
    // rename fails with EXDEV, so fall back to copy + remove.
    if (QFile::copy(src, dst)) {
        QFile::remove(src);
        return true;
    }
    return false;
}

int StudioRecorder::elapsedSeconds() const
{
    if (!m_elapsed.isValid())
        return 0;
    const qint64 base = m_paused ? m_pauseStartElapsedMs : m_elapsed.elapsed();
    return int(qMax<qint64>(0, base - m_pausedTotalMs) / 1000);
}

void StudioRecorder::start(bool holdForCommit)
{
#ifndef HAVE_PIPEWIRE
    Q_UNUSED(holdForCommit)
    emit failed(tr("Unisic Studio was built without PipeWire support, so recording is unavailable"));
#else
    if (m_state != Idle)
        return;
    // Sweep raw masters orphaned by a crash/SIGKILL (multi-GB in ~/.cache). We
    // have not created ours yet this session, and a prior completed recording has
    // already moved its raw out, so only crash debris matches.
    {
        QDir cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
        const QStringList stale = cache.entryList({QStringLiteral("unisic-studio-rec-*"),
                                                   QStringLiteral("unisic-studio-webcam-*")},
                                                  QDir::Files);
        for (const QString &f : stale)
            cache.remove(f);
    }

    m_state = Starting;
    m_holdForCommit = holdForCommit;
    m_armed = false;
    m_committed = false;
    m_heldStreamSize = QSize();
    m_framesWritten = 0;
    m_lastFrame.clear();
    m_lastSampledSeq = 0;
    m_haveT0 = false;
    m_t0MonoNs = 0;
    m_paused = false;
    m_pauseStartMonoNs = 0;
    m_pauseStartElapsedMs = 0;
    m_pausedTotalMs = 0;
    m_maxRemainingMs = 0;
    m_pauseIntervals.clear();
    m_cursorRaw.clear();
    m_clickRaw.clear();
    m_keyDownRaw.clear();
    m_shapes.clear();
    m_hasAudio = false;
    m_hadClickCapture = false;
    m_cursorMode = QStringLiteral("none");
    m_rawMasterPath.clear();
    m_masterPath.clear();
    m_sidecarPath.clear();

    openPortalSession();
#endif
}

void StudioRecorder::openPortalSession()
{
    m_session = new ScreenCastSession(this);
    connect(m_session, &ScreenCastSession::ready, this, &StudioRecorder::onStreamReady);
    connect(m_session, &ScreenCastSession::failed, this, [this](const QString &e) {
        // A revoked or stale token must degrade to the picker, not wedge the
        // recorder forever. "cancelled" is the user saying no — their token is
        // still good, so don't punish them for it.
        if (!m_replayedToken.isEmpty() && e != QLatin1String("cancelled"))
            m_settings->setScreencastRestoreToken(QString());
        fail(e);
    });
    connect(m_session, &ScreenCastSession::sessionClosed, this, [this] {
        if (m_state == Recording)
            stop();                                   // finalize what we have
        else if (m_state == Starting)
            fail(tr("Screen sharing was stopped"));
    });
    // Remember the portal's source pick so the picker only appears once. The
    // signal fires unconditionally whenever the portal supports tokens —
    // including with an empty string when it omits the key — so guard, or a
    // good token gets erased and the picker is back every launch.
    connect(m_session, &ScreenCastSession::restoreTokenChanged, this,
            [this](const QString &token) {
                if (!token.isEmpty())
                    m_settings->setScreencastRestoreToken(token);
            });
    // Metadata cursor (re-rendered from the track, never baked into the frames);
    // MONITOR|WINDOW so the portal picker offers a monitor OR a single window.
    // The stored token replays last time's pick; an empty/stale one just means
    // the portal asks again.
    m_replayedToken = m_settings->screencastRestoreToken();
    m_session->start(ScreenCastSession::CursorMode::Metadata, 3u, m_replayedToken);
}

void StudioRecorder::onStreamReady(int fd, uint nodeId, const QSize &, const QPoint &)
{
#ifdef HAVE_PIPEWIRE
    m_grabber = new PipeWireGrabber(this);
    connect(m_grabber, &PipeWireGrabber::formatReady, this, &StudioRecorder::onFormatReady);
    connect(m_grabber, &PipeWireGrabber::streamError, this, [this](const QString &e) {
        if (m_state == Starting || m_state == Recording)
            fail(e);
    });
    // Shapes arrive on the PipeWire thread → queued. Collected throughout (a shape
    // is only referenced if a sample wears its id).
    connect(m_grabber, &PipeWireGrabber::cursorShapeChanged, this,
            [this](int id, const QImage &image, const QPoint &hotspot) {
                m_shapes.append({id, image, hotspot});
            }, Qt::QueuedConnection);

    const ScreenCastSession::CursorMode mode = m_session->effectiveCursorMode();
    switch (mode) {
    case ScreenCastSession::CursorMode::Metadata: m_cursorMode = QStringLiteral("metadata"); break;
    case ScreenCastSession::CursorMode::Embedded: m_cursorMode = QStringLiteral("embedded"); break;
    default:                                      m_cursorMode = QStringLiteral("none");     break;
    }
    const bool wantCursorMeta = mode == ScreenCastSession::CursorMode::Metadata;
    if (!m_grabber->start(fd, nodeId, qBound(1, m_settings->recordFps(), 240), wantCursorMeta))
        fail(tr("Failed to connect to the PipeWire stream"));
#else
    Q_UNUSED(fd)
    Q_UNUSED(nodeId)
#endif
}

void StudioRecorder::onFormatReady(const QSize &streamSize)
{
    if (m_state != Starting)
        return;
    // Commit hold: the portal dialog is resolved, the stream is live. Announce
    // arming and WAIT — encoding begins only on commit(), so no countdown frame
    // lands in the file.
    if (m_holdForCommit && !m_committed) {
        if (!m_armed) {
            m_armed = true;
            m_heldStreamSize = streamSize;
            emit armed();
        }
        return;
    }
    beginEncoding(streamSize);
}

void StudioRecorder::commit()
{
    if (m_state != Starting || !m_armed || m_committed)
        return;
    m_committed = true;
    beginEncoding(m_heldStreamSize);
}

void StudioRecorder::beginEncoding(const QSize &streamSize)
{
    if (m_state != Starting)
        return;
    if (!streamSize.isValid() || streamSize.width() < 2 || streamSize.height() < 2) {
        fail(tr("PipeWire returned an invalid stream size"));
        return;
    }
    m_streamSize = streamSize;
    // yuv420p needs even dimensions; window streams are frequently odd-sized. Crop
    // the raw frame's bottom/right edge to the even size in sampleFrame().
    m_encodeSize = QSize(streamSize.width() & ~1, streamSize.height() & ~1);
    if (m_encodeSize.width() < 2 || m_encodeSize.height() < 2) {
        fail(tr("Recording stream is too small"));
        return;
    }
    m_fps = qBound(1, m_settings->recordFps(), 240);
    m_framesWritten = 0;
#ifdef HAVE_PIPEWIRE
    if (m_grabber)
        m_pixelFormat = m_grabber->pixelFormat();
#endif

    const QDateTime now = QDateTime::currentDateTime();
    const QString stamp = now.toString(QStringLiteral("yyyyMMdd_hhmmss"));

    // Raw master on disk-backed XDG cache (NOT /tmp: it is tmpfs on Fedora and
    // minutes of H.264 could still be large), moved into the projects dir on finish.
    const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheBase);
    m_rawMasterPath = cacheBase + QStringLiteral("/unisic-studio-rec-%1.mkv").arg(stamp);

    const QString outDir = m_settings->projectsDirectory();
    QDir().mkpath(outDir);
    QString base = QStringLiteral("Unisic_Studio_%1").arg(stamp);
    m_masterPath = outDir + QLatin1Char('/') + base + QStringLiteral(".mkv");
    m_sidecarPath = outDir + QLatin1Char('/') + base + QStringLiteral(".unisicstudio");
    for (int i = 1; QFileInfo::exists(m_masterPath) || QFileInfo::exists(m_sidecarPath); ++i) {
        base = QStringLiteral("Unisic_Studio_%1-%2").arg(stamp).arg(i);
        m_masterPath = outDir + QLatin1Char('/') + base + QStringLiteral(".mkv");
        m_sidecarPath = outDir + QLatin1Char('/') + base + QStringLiteral(".unisicstudio");
    }

    const int crf = qBound(0, m_settings->masterCrf(), 51);
    QStringList args{QStringLiteral("-y"), QStringLiteral("-nostats"),
                     QStringLiteral("-loglevel"), QStringLiteral("error"),
                     QStringLiteral("-f"), QStringLiteral("rawvideo"),
                     QStringLiteral("-pix_fmt"), m_pixelFormat,
                     QStringLiteral("-video_size"),
                     QStringLiteral("%1x%2").arg(m_encodeSize.width()).arg(m_encodeSize.height()),
                     QStringLiteral("-framerate"), QString::number(m_fps),
                     // Two whole frames absorb hand-off jitter without letting
                     // ffmpeg build a huge independent reservoir.
                     QStringLiteral("-thread_queue_size"), QStringLiteral("2"),
                     QStringLiteral("-i"), QStringLiteral("-")};

    // Optional audio: system monitor and/or microphone, mixed when both.
    QStringList audioDevs;
    if (m_settings->recordSystemAudio())
        audioDevs << QStringLiteral("@DEFAULT_MONITOR@");
    if (m_settings->recordMicrophone())
        audioDevs << QStringLiteral("default");
    QVector<int> audioInputs;
    int nextInput = 1;
    for (const QString &dev : std::as_const(audioDevs)) {
        args << QStringLiteral("-f") << QStringLiteral("pulse")
             << QStringLiteral("-thread_queue_size") << QStringLiteral("1024")
             << QStringLiteral("-i") << dev;
        audioInputs << nextInput++;
    }

    args << QStringLiteral("-c:v") << QStringLiteral("libx264")
         << QStringLiteral("-crf") << QString::number(crf)
         << QStringLiteral("-preset") << QStringLiteral("fast")
         << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");

    QString audioMap;
    if (audioInputs.size() == 1) {
        audioMap = QStringLiteral("%1:a").arg(audioInputs.first());
    } else if (audioInputs.size() >= 2) {
        QString labels;
        for (int idx : std::as_const(audioInputs))
            labels += QStringLiteral("[%1:a]").arg(idx);
        args << QStringLiteral("-filter_complex")
             << labels + QStringLiteral("amix=inputs=%1:duration=longest:normalize=0[aout]")
                             .arg(audioInputs.size());
        audioMap = QStringLiteral("[aout]");
    }

    args << QStringLiteral("-map") << QStringLiteral("0:v");
    if (!audioMap.isEmpty()) {
        // FLAC in the master; -shortest lets the never-EOF pulse capture end with
        // the video pipe.
        args << QStringLiteral("-map") << audioMap
             << QStringLiteral("-c:a") << QStringLiteral("flac")
             << QStringLiteral("-shortest");
        m_hasAudio = true;
    }
    args << m_rawMasterPath;

    m_ffmpeg = new QProcess(this);
    m_ffmpeg->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_ffmpeg, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if ((m_state == Recording || m_state == Starting) && e == QProcess::FailedToStart)
            fail(tr("ffmpeg could not be started. Is it installed?"));
    });
    connect(m_ffmpeg, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        auto *p = m_ffmpeg;
        if (!p)
            return;
        const QByteArray out = p->readAll();
        m_ffmpeg = nullptr;
        p->deleteLater();
        if (m_state == Recording || m_state == Starting) {
            if (!out.isEmpty())
                qWarning() << out;
            fail(st == QProcess::CrashExit
                     ? tr("Recording encoder crashed")
                     : tr("Recording encoder stopped unexpectedly (code %1)").arg(code));
            return;
        }
        if (m_state != Finalizing)
            return;
        if (code != 0 || st == QProcess::CrashExit) {
            if (!out.isEmpty())
                qWarning() << out;
            fail(tr("Recording encoder failed (code %1)").arg(code));
            return;
        }
        onEncoderFinished(code, st == QProcess::CrashExit);
    });
    auto *encoder = m_ffmpeg;
    encoder->start(QStringLiteral("ffmpeg"), args);
    if (!encoder->waitForStarted(3000)) {
        if (m_ffmpeg == encoder && m_state == Starting)
            fail(tr("ffmpeg could not be started. Is it installed?"));
        return;
    }
    if (m_ffmpeg != encoder)
        return;

    m_state = Recording;
    startWebcamCapture(base); // best-effort; never fails the screen recording
    // Provisional t0 so a stop before the first frame still gives cursor/click
    // timestamps a sane origin; the first sampled frame overwrites it with the
    // real pts (m_haveT0 stays false until then).
    m_t0MonoNs = nowMonoNs();
    m_haveT0 = false;
    m_elapsed.start();
    m_elapsedTick.start();
    m_sampler.start(qMax(1, 1000 / m_fps));
#ifdef HAVE_PIPEWIRE
    // Discard cursor meta accumulated during the portal/countdown pre-roll so only
    // motion from t0 onward is recorded.
    if (m_grabber)
        m_grabber->takeCursorSamples();
    // Snapshot the newest pre-roll frame's seq: the grabber ran through the
    // whole countdown, so at the first sampler tick latestFrame() would hand
    // back a STALE frame (typically the countdown pill repaint) whose pts also
    // predates commit — it would become frame 0 AND poison t0, shifting every
    // cursor/click timestamp. sampleFrame() skips it until fresh damage arrives.
    m_preRollSeq = 0;
    m_awaitFreshFrame = false;
    if (m_grabber) {
        QByteArray preFrame;
        qint64 prePts = 0;
        if (m_grabber->latestFrame(preFrame, &m_preRollSeq, &prePts))
            m_awaitFreshFrame = true;
    }
#endif
    m_cursorDrain.start();

    // The libinput thread is shared by click and typing capture, so start it when
    // EITHER is enabled — a typing-only user still needs it running. Key capture
    // is off in the thread unless typing zoom is explicitly consented to.
    m_hadClickCapture = false;
    const bool wantClicks = m_settings->clickCaptureEnabled();
    const bool wantTyping = m_settings->typingZoomEnabled();
    if ((wantClicks || wantTyping)
        && InputPermission::probe() == InputPermission::Available) {
        m_clicks->setCaptureKeys(wantTyping);
        m_clicks->start();
        m_hadClickCapture = wantClicks && m_clicks->isRunning();
    }

    const int maxSec = m_settings->recordMaxDurationSec();
    if (maxSec > 0)
        m_maxTimer.start(maxSec * 1000);

    emit started();
}

void StudioRecorder::sampleFrame()
{
#ifdef HAVE_PIPEWIRE
    if (m_state != Recording || !m_grabber || !m_ffmpeg)
        return;
    const qsizetype frameBytes = qsizetype(m_encodeSize.width()) * m_encodeSize.height() * 4;
    if (frameBytes <= 0)
        return;
    // Bounded backlog (≤6 frames, ≤192 MB): drop a sample instead of aborting.
    const qsizetype absoluteCap = qsizetype(192) * 1024 * 1024;
    const qsizetype queuedFrames = qBound(qsizetype(1), absoluteCap / frameBytes, qsizetype(6));
    const qsizetype writeCap = frameBytes * queuedFrames;
    if (m_ffmpeg->bytesToWrite() + frameBytes > writeCap)
        return;

    const qsizetype expected = qsizetype(m_streamSize.width()) * m_streamSize.height() * 4;
    QByteArray frame, encoded;
    quint64 seq = 0;
    qint64 pts = 0;
    if (!m_paused && m_grabber->latestFrame(frame, &seq, &pts) && frame.size() == expected) {
        // Pre-roll guard: this is still the frame delivered BEFORE commit (the
        // countdown pill). Wait briefly for fresh damage; after 500 ms accept
        // the held frame anyway (a truly static screen delivers nothing new).
        bool staleFrame = false;
        if (m_awaitFreshFrame) {
            if (seq == m_preRollSeq) {
                if (m_elapsed.elapsed() < 500)
                    return;
                staleFrame = true;   // its pts predates commit — unusable for t0
            }
            m_awaitFreshFrame = false;
        }
        if (!m_haveT0) {
            // t0 = pts of the FIRST sampled frame. Fallback to CLOCK_MONOTONIC now
            // if the kit did not stamp a pts (0) — same clock, sub-frame skew only.
            m_t0MonoNs = (pts > 0 && !staleFrame) ? pts : nowMonoNs();
            m_haveT0 = true;
        }
        if (seq == m_lastSampledSeq && !m_lastFrame.isEmpty()) {
            encoded = m_lastFrame;                     // damage-driven: unchanged frame
        } else {
            if (m_encodeSize == m_streamSize) {
                encoded = frame;
            } else {
                encoded.resize(frameBytes);            // crop bottom/right to even size
                const qsizetype srcStride = qsizetype(m_streamSize.width()) * 4;
                const qsizetype dstStride = qsizetype(m_encodeSize.width()) * 4;
                const char *src = frame.constData();
                char *dst = encoded.data();
                for (int y = 0; y < m_encodeSize.height(); ++y) {
                    memcpy(dst, src, dstStride);
                    src += srcStride;
                    dst += dstStride;
                }
            }
            m_lastFrame = encoded;
            m_lastSampledSeq = seq;
        }
    } else {
        encoded = m_lastFrame;   // paused / no frame yet / renegotiated: sample-and-hold
    }
    if (encoded.isEmpty())
        return;

    // Wall-clock pacing: the container claims an exact -framerate, so duplicate or
    // drop to hold CFR against the truncating timer interval and dropped ticks.
    const qint64 target = m_elapsed.elapsed() * m_fps / 1000 + 1;
    if (target <= m_framesWritten)
        return;
    qint64 n = qMin<qint64>(target - m_framesWritten, m_fps);   // ≤1 s burst
    n = qMin<qint64>(n, (writeCap - m_ffmpeg->bytesToWrite()) / frameBytes);
    qint64 accepted = 0;
    for (; accepted < n; ++accepted) {
        qsizetype offset = 0;
        while (offset < encoded.size()) {
            const qint64 written = m_ffmpeg->write(encoded.constData() + offset,
                                                   encoded.size() - offset);
            if (written <= 0)
                break;
            offset += written;
        }
        if (offset != encoded.size())
            break;
    }
    m_framesWritten += accepted;
#endif
}

void StudioRecorder::drainCursorSamples()
{
#ifdef HAVE_PIPEWIRE
    if (!m_grabber)
        return;
    const QVector<CursorSample> batch = m_grabber->takeCursorSamples();   // kit CursorSample
    m_cursorRaw.reserve(m_cursorRaw.size() + batch.size());
    for (const CursorSample &s : batch)
        m_cursorRaw.append({s.tMonoNs, s.x, s.y, s.visible, s.shapeId});
#endif
}

void StudioRecorder::togglePause()
{
    if (m_paused) {
        if (m_state != Recording)
            return;
        m_pauseIntervals.append({m_pauseStartMonoNs, nowMonoNs()});
        m_pausedTotalMs += m_elapsed.elapsed() - m_pauseStartElapsedMs;
        m_paused = false;
        m_elapsedTick.start();
        if (m_maxRemainingMs > 0) {
            m_maxTimer.start(int(m_maxRemainingMs));
            m_maxRemainingMs = 0;
        }
        emit paused(false);
        emit elapsedChanged();
    } else {
        if (!canPause())
            return;
        m_paused = true;
        m_pauseStartMonoNs = nowMonoNs();
        m_pauseStartElapsedMs = m_elapsed.elapsed();
        m_elapsedTick.stop();
        if (m_maxTimer.isActive()) {
            m_maxRemainingMs = qMax(1, m_maxTimer.remainingTime());
            m_maxTimer.stop();
        }
        emit paused(true);
        emit elapsedChanged();
    }
}

void StudioRecorder::stop()
{
    if (m_state != Recording) {
        if (m_state == Starting)
            fail(QStringLiteral("cancelled"));   // filtered by the UI
        return;
    }
    // Stopped while paused: close the open span so its frozen tail is excised too.
    if (m_paused) {
        m_pauseIntervals.append({m_pauseStartMonoNs, nowMonoNs()});
        m_paused = false;
        emit paused(false);
    }
    m_state = Finalizing;
    emit finalizing();
    m_sampler.stop();
    m_maxTimer.stop();
    m_elapsedTick.stop();
    m_cursorDrain.stop();
    m_clicks->stop();
    drainCursorSamples();        // final drain BEFORE the grabber is detached
    stopGrabber();
    if (m_session) {
        m_session->disconnect(this);
        m_session->deleteLater();
        m_session = nullptr;
    }
    stopWebcamCapture();             // finalize the webcam mkv before we move it
    if (!m_ffmpeg) {
        fail(tr("Recording encoder is not running"));
        return;
    }
    m_ffmpeg->closeWriteChannel();   // EOF → ffmpeg finalizes; finished() continues
    m_lastFrame.clear();
    emit elapsedChanged();
}

void StudioRecorder::startWebcamCapture(const QString &base)
{
    if (!m_settings->recordWebcam())
        return;
    const QString dev = m_settings->webcamDevice();
    if (dev.isEmpty() || !QFileInfo::exists(dev))
        return; // no device — the Settings toggle already gates on this

    const QString cacheBase = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cacheBase);
    m_webcamRawPath = cacheBase + QStringLiteral("/unisic-studio-webcam-%1.mkv").arg(base);
    m_webcamFinalPath =
        m_settings->projectsDirectory() + QLatin1Char('/') + base + QStringLiteral("_webcam.mkv");

    // A separate ffmpeg reading the v4l2 device. NOT -nostdin: stopWebcamCapture()
    // writes 'q' so the container is finalized cleanly. Any failure just drops the
    // webcam (the screen recording is unaffected).
    const QStringList args{QStringLiteral("-nostats"), QStringLiteral("-loglevel"),
                           QStringLiteral("error"), QStringLiteral("-f"), QStringLiteral("v4l2"),
                           QStringLiteral("-i"), dev, QStringLiteral("-c:v"),
                           QStringLiteral("libx264"), QStringLiteral("-preset"),
                           QStringLiteral("veryfast"), QStringLiteral("-pix_fmt"),
                           QStringLiteral("yuv420p"), QStringLiteral("-y"), m_webcamRawPath};
    m_webcamProc = new QProcess(this);
    connect(m_webcamProc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        // Drop the webcam quietly; screen recording continues.
        if (m_webcamProc) {
            m_webcamProc->deleteLater();
            m_webcamProc = nullptr;
        }
        if (!m_webcamRawPath.isEmpty())
            QFile::remove(m_webcamRawPath);
        m_webcamRawPath.clear();
        m_webcamFinalPath.clear();
    });
    m_webcamProc->start(QStringLiteral("ffmpeg"), args);
    if (!m_webcamProc->waitForStarted(3000)) {
        qWarning() << "webcam capture could not start on" << dev;
        FfmpegUtil::stopProcess(m_webcamProc);
        m_webcamRawPath.clear();
        m_webcamFinalPath.clear();
    }
}

void StudioRecorder::stopWebcamCapture()
{
    if (!m_webcamProc)
        return;
    if (m_webcamProc->state() == QProcess::Running) {
        m_webcamProc->write("q\n");   // ffmpeg's clean-stop key → finalizes the mkv
        m_webcamProc->closeWriteChannel();
        m_webcamProc->waitForFinished(4000);
    }
    // Fully reap (SIGKILL fallback if 'q' didn't land); nulls the pointer.
    FfmpegUtil::stopProcess(m_webcamProc);
}

void StudioRecorder::stopGrabber()
{
#ifdef HAVE_PIPEWIRE
    if (!m_grabber)
        return;
    // pw_thread_loop_stop JOINS the capture thread; doing that on the GUI thread
    // would block the repaint that should show "Finalizing". Detach and stop on a
    // worker; only it touches the object until deleteLater lands back here.
    PipeWireGrabber *g = m_grabber;
    m_grabber = nullptr;
    g->disconnect(this);
    g->setParent(nullptr);
    (void)QtConcurrent::run([g] {
        g->stop();
        QMetaObject::invokeMethod(g, "deleteLater", Qt::QueuedConnection);
    });
#endif
}

void StudioRecorder::onEncoderFinished(int, bool)
{
    // Raw master is on disk. Excise pauses (if any) into the final master, then
    // assemble the project + sidecar.
    maybeExcise([this] { finalize(); });
}

// Preserve the just-finished raw capture after a finalize failure. Moved next to
// the intended destination: left in the cache it would match the crash-debris
// sweep on the next start() and be silently deleted. Clears m_rawMasterPath so
// the fail()→cancel() cleanup can't remove it; returns the path that survives.
QString StudioRecorder::rescueRawMaster()
{
    const QString raw = m_rawMasterPath;
    m_rawMasterPath.clear();
    const QString rescue = m_masterPath + QStringLiteral(".raw.mkv");
    if (!m_masterPath.isEmpty() && moveFile(raw, rescue))
        return rescue;
    return raw;
}

void StudioRecorder::maybeExcise(std::function<void()> then)
{
    const QList<QPair<qint64, qint64>> videoMsRanges =
        RecorderMath::pauseRangesToVideoMs(m_pauseIntervals, m_t0MonoNs);

    if (videoMsRanges.isEmpty()) {
        if (!moveFile(m_rawMasterPath, m_masterPath)) {
            // NEVER delete the just-finished capture on a finalize failure —
            // point the user at the surviving raw file instead.
            fail(tr("Could not move the recording into the projects folder. "
                    "The raw capture was kept at %1").arg(rescueRawMaster()));
            return;
        }
        then();
        return;
    }

    const int crf = qBound(0, m_settings->masterCrf(), 51);
    const QStringList args =
        pauseExciseArgs(m_rawMasterPath, m_masterPath, videoMsRanges, m_hasAudio, crf);

    auto *conv = new QProcess(this);
    m_converter = conv;
    conv->setProcessChannelMode(QProcess::MergedChannels);
    connect(conv, &QProcess::finished, this,
            [this, conv, then](int code, QProcess::ExitStatus st) {
                if (m_converter != conv)
                    return;
                const QByteArray out = conv->readAll();
                m_converter = nullptr;
                conv->deleteLater();
                if (code != 0 || st == QProcess::CrashExit || !QFileInfo::exists(m_masterPath)) {
                    qWarning() << out;
                    QFile::remove(m_masterPath);   // partial excise output only
                    fail(tr("Removing the paused sections failed. "
                            "The raw capture was kept at %1").arg(rescueRawMaster()));
                    return;
                }
                QFile::remove(m_rawMasterPath);
                then();
            });
    connect(conv, &QProcess::errorOccurred, this, [this, conv](QProcess::ProcessError e) {
        if (m_converter != conv || e != QProcess::FailedToStart)
            return;
        m_converter = nullptr;
        conv->deleteLater();
        QFile::remove(m_masterPath);
        fail(tr("ffmpeg could not be started. The raw capture was kept at %1")
                 .arg(rescueRawMaster()));
    });
    conv->start(QStringLiteral("ffmpeg"), args);
    if (!conv->waitForStarted(3000) && m_converter == conv) {
        m_converter = nullptr;
        conv->deleteLater();
        QFile::remove(m_masterPath);
        fail(tr("ffmpeg could not be started. The raw capture was kept at %1")
                 .arg(rescueRawMaster()));
    }
}

void StudioRecorder::finalize()
{
    RecordingAssembler::Input in;
    in.cursors = m_cursorRaw;
    in.clicks = m_clickRaw;
    in.keyDownMonoNs = m_keyDownRaw;
    in.shapes = m_shapes;
    in.pauseMonoNs = m_pauseIntervals;
    in.t0MonoNs = m_t0MonoNs;
    in.fps = m_fps;
    in.framesWritten = m_framesWritten;
    in.videoSize = m_encodeSize;
    in.masterAbsPath = QFileInfo(m_masterPath).absoluteFilePath();
    in.sidecarPath = QFileInfo(m_sidecarPath).absoluteFilePath();
    in.cursorMode = m_cursorMode;
    in.hadClickCapture = m_hadClickCapture;
    in.compositor = qEnvironmentVariable("XDG_CURRENT_DESKTOP");

    // Move the finalized webcam sidecar next to the master (best-effort; a failure
    // just omits the overlay from the project).
    if (!m_webcamRawPath.isEmpty() && QFileInfo::exists(m_webcamRawPath)
        && !m_webcamFinalPath.isEmpty()) {
        if (moveFile(m_webcamRawPath, m_webcamFinalPath))
            in.webcamAbsPath = QFileInfo(m_webcamFinalPath).absoluteFilePath();
        else
            QFile::remove(m_webcamRawPath);
    }

    QString err;
    if (!RecordingAssembler::assembleAndSave(in, &err)) {
        QFile::remove(m_masterPath);
        fail(tr("Could not save the recording project: %1").arg(err));
        return;
    }
    const QString sidecar = in.sidecarPath;
    cleanup();
    emit finished(sidecar);
}

QStringList StudioRecorder::pauseExciseArgs(const QString &input, const QString &output,
                                            const QList<QPair<qint64, qint64>> &videoMsRanges,
                                            bool hasAudio, int crf)
{
    // keep = a frame/sample whose video time is NOT inside any paused span. Cutting
    // the SAME ranges from video and audio preserves sync; setpts/asetpts restamp
    // the survivors onto a gapless timeline.
    QStringList inside;
    for (const auto &iv : videoMsRanges)
        inside << QStringLiteral("between(t,%1,%2)")
                      .arg(iv.first / 1000.0, 0, 'f', 3)
                      .arg(iv.second / 1000.0, 0, 'f', 3);
    const QString keep = QStringLiteral("not(%1)").arg(inside.join(QLatin1Char('+')));
    const QString vf = QStringLiteral("select='%1',setpts=N/FRAME_RATE/TB").arg(keep);
    const QString af = QStringLiteral("aselect='%1',asetpts=N/SR/TB").arg(keep);

    QStringList args{QStringLiteral("-y"), QStringLiteral("-nostats"),
                     QStringLiteral("-loglevel"), QStringLiteral("error"),
                     QStringLiteral("-i"), input,
                     QStringLiteral("-map"), QStringLiteral("0:v:0"),
                     QStringLiteral("-vf"), vf,
                     QStringLiteral("-c:v"), QStringLiteral("libx264"),
                     QStringLiteral("-crf"), QString::number(crf),
                     QStringLiteral("-preset"), QStringLiteral("fast"),
                     QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p")};
    if (hasAudio)
        args << QStringLiteral("-map") << QStringLiteral("0:a:0?")
             << QStringLiteral("-af") << af
             << QStringLiteral("-c:a") << QStringLiteral("flac");
    args << output;
    return args;
}

void StudioRecorder::teardownProcesses()
{
    FfmpegUtil::stopProcess(m_ffmpeg);     // non-blocking; nulls the pointer
    FfmpegUtil::stopProcess(m_converter);
    FfmpegUtil::stopProcess(m_webcamProc);
}

void StudioRecorder::cancel()
{
    m_sampler.stop();
    m_maxTimer.stop();
    m_elapsedTick.stop();
    m_cursorDrain.stop();
    if (m_clicks)
        m_clicks->stop();
    stopGrabber();
    if (m_session) {
        m_session->disconnect(this);
        m_session->deleteLater();
        m_session = nullptr;
    }
    teardownProcesses();
    if (!m_rawMasterPath.isEmpty())
        QFile::remove(m_rawMasterPath);
    if (!m_webcamRawPath.isEmpty())
        QFile::remove(m_webcamRawPath);
    if (!m_webcamFinalPath.isEmpty())
        QFile::remove(m_webcamFinalPath);
    // A partial master/sidecar can exist if we were finalizing.
    if (m_state == Finalizing) {
        if (!m_masterPath.isEmpty())
            QFile::remove(m_masterPath);
        if (!m_sidecarPath.isEmpty())
            QFile::remove(m_sidecarPath);
    }
    cleanup();
}

void StudioRecorder::cleanup()
{
    m_state = Idle;
    if (m_paused) {
        m_paused = false;
        emit paused(false);
    }
    m_pauseStartMonoNs = 0;
    m_pauseStartElapsedMs = 0;
    m_pausedTotalMs = 0;
    m_maxRemainingMs = 0;
    m_pauseIntervals.clear();
    m_cursorRaw.clear();
    m_clickRaw.clear();
    m_keyDownRaw.clear();
    m_shapes.clear();
    m_lastFrame.clear();
    m_lastSampledSeq = 0;
    m_framesWritten = 0;
    m_haveT0 = false;
    m_t0MonoNs = 0;
    m_hasAudio = false;
    m_armed = false;
    m_committed = false;
    m_holdForCommit = false;
    m_heldStreamSize = QSize();
    m_rawMasterPath.clear();
    m_masterPath.clear();
    m_sidecarPath.clear();
    m_webcamRawPath.clear();
    m_webcamFinalPath.clear();
    m_elapsed.invalidate();
    emit elapsedChanged();
}

void StudioRecorder::fail(const QString &msg)
{
    // Single-fire: a streamError/formatReady queued from the PipeWire thread just
    // before teardown is still delivered afterwards; the Idle guard drops it.
    if (m_state == Idle)
        return;
    qWarning() << "StudioRecorder:" << msg;
    cancel();
    emit failed(msg);
}
