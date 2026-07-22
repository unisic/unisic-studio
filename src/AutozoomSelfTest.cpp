#include "AutozoomSelfTest.h"

#include "CursorPlayback.h"
#include "StudioApp.h"
#include "engine/KeyframeEngine.h"
#include "engine/TrajectoryMetrics.h"
#include "project/ClickTrack.h"
#include "project/CursorTrack.h"
#include "project/StudioProject.h"
#include "project/StyleModel.h"
#include "project/ZoomTimeline.h"
#include "render/ExportController.h"
#include "render/FrameDecoder.h"

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRectF>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

namespace {

constexpr int kW = 1280;
constexpr int kH = 720;

void moveLine(CursorTrack &t, double x0, double y0, double x1, double y1, qint64 t0, qint64 t1,
              int step = 16)
{
    const int n = int((t1 - t0) / step);
    for (int i = 0; i <= n; ++i) {
        const double f = n > 0 ? double(i) / n : 0.0;
        CursorSample s;
        s.tMs = t0 + qint64(i) * step;
        s.x = x0 + (x1 - x0) * f;
        s.y = y0 + (y1 - y0) * f;
        t.append(s);
    }
}

void hold(CursorTrack &t, double x, double y, qint64 t0, qint64 t1, int step = 16)
{
    for (qint64 tt = t0; tt <= t1; tt += step) {
        CursorSample s;
        s.tMs = tt;
        s.x = x;
        s.y = y;
        t.append(s);
    }
}

void click(ClickTrack &c, qint64 t, double x, double y)
{
    ClickEvent d;
    d.tMs = t;
    d.button = 1;
    d.state = ClickEvent::Down;
    d.x = x;
    d.y = y;
    ClickEvent u = d;
    u.tMs = t + 30;
    u.state = ClickEvent::Up;
    c.append(d);
    c.append(u);
}

bool isFull(const QRectF &r)
{
    return std::fabs(r.x()) < 1e-6 && std::fabs(r.y()) < 1e-6 && std::fabs(r.width() - 1.0) < 1e-6
        && std::fabs(r.height() - 1.0) < 1e-6;
}

double rectDelta(const QRectF &a, const QRectF &b)
{
    return std::max({std::fabs(a.x() - b.x()), std::fabs(a.y() - b.y()),
                     std::fabs(a.width() - b.width()), std::fabs(a.height() - b.height())});
}

// mean per-channel abs difference over a WxH region of two RGB24 kW*kH buffers.
double regionDiff(const QByteArray &a, const QByteArray &b, int x0, int y0, int w, int h)
{
    if (a.size() < kW * kH * 3 || b.size() < kW * kH * 3)
        return -1.0;
    const uchar *pa = reinterpret_cast<const uchar *>(a.constData());
    const uchar *pb = reinterpret_cast<const uchar *>(b.constData());
    double sum = 0.0;
    long cnt = 0;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) {
            const int i = (y * kW + x) * 3;
            for (int c = 0; c < 3; ++c) {
                sum += std::abs(int(pa[i + c]) - int(pb[i + c]));
                ++cnt;
            }
        }
    return cnt ? sum / cnt : 0.0;
}

// fraction of green-dominant pixels in a square window (the 'dot' cursor colour).
double greenFraction(const QByteArray &a, int cx, int cy, int rad)
{
    if (a.size() < kW * kH * 3)
        return -1.0;
    const uchar *p = reinterpret_cast<const uchar *>(a.constData());
    long hit = 0, tot = 0;
    for (int y = cy - rad; y < cy + rad; ++y)
        for (int x = cx - rad; x < cx + rad; ++x) {
            if (x < 0 || y < 0 || x >= kW || y >= kH)
                continue;
            const int i = (y * kW + x) * 3;
            const int r = p[i], g = p[i + 1], b = p[i + 2];
            ++tot;
            if (g > 140 && r < 130 && b < 130)
                ++hit;
        }
    return tot ? double(hit) / tot : 0.0;
}

QByteArray extractFrame(const QString &ffmpeg, const QString &video, int tMs)
{
    QProcess ff;
    QStringList a;
    a << QStringLiteral("-v") << QStringLiteral("error") << QStringLiteral("-ss")
      << QString::number(tMs / 1000.0, 'f', 3) << QStringLiteral("-i") << video
      << QStringLiteral("-frames:v") << QStringLiteral("1") << QStringLiteral("-vf")
      << QStringLiteral("scale=%1:%2").arg(kW).arg(kH) << QStringLiteral("-f")
      << QStringLiteral("rawvideo") << QStringLiteral("-pix_fmt") << QStringLiteral("rgb24")
      << QStringLiteral("pipe:1");
    ff.start(ffmpeg, a);
    ff.waitForFinished(30000);
    return ff.readAllStandardOutput();
}

QByteArray serializeZoom(StudioProject *p)
{
    return QJsonDocument(p->zoom()->toJson()).toJson(QJsonDocument::Compact);
}

void fail(const char *msg)
{
    fprintf(stderr, "autozoom-test: FAIL — %s\n", msg);
    fflush(stderr);
    QCoreApplication::exit(1);
}

} // namespace

void AutozoomSelfTest::run(StudioApp *studio)
{
    const QString ffmpeg = FrameDecoder::ffmpegPath();
    if (ffmpeg.isEmpty()) {
        fail("ffmpeg not found");
        return;
    }

    static QTemporaryDir dir;
    if (!dir.isValid()) {
        fail("could not create temp dir");
        return;
    }
    const QString testsrc = dir.filePath(QStringLiteral("src.mp4"));
    const QString outA = dir.filePath(QStringLiteral("zoom.mp4"));
    const QString outB = dir.filePath(QStringLiteral("flat.mp4"));

    // --- synthesise the master video (moving test pattern, edge detail) ---
    {
        QProcess gen;
        QStringList a;
        a << QStringLiteral("-y") << QStringLiteral("-v") << QStringLiteral("error")
          << QStringLiteral("-f") << QStringLiteral("lavfi") << QStringLiteral("-i")
          << QStringLiteral("testsrc=size=%1x%2:rate=30:duration=8").arg(kW).arg(kH)
          << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << testsrc;
        gen.start(ffmpeg, a);
        gen.waitForFinished(60000);
        if (!QFileInfo::exists(testsrc)) {
            fail("could not synthesise testsrc video");
            return;
        }
    }

    // --- scripted recording: approach P, dwell + a 3-click cluster, leave ---
    const double px = 900.0, py = 360.0; // cluster point P (stream px)
    auto *p = new StudioProject(qApp);
    p->setVideoAbsPath(testsrc);
    p->setDurationMs(8000);
    p->setFps(30.0);
    p->setVideoSize(QSize(kW, kH));
    p->setCursorMode(QStringLiteral("metadata"));
    p->setHadClickCapture(true);
    {
        CursorTrack cur;
        moveLine(cur, 200, 150, px, py, 0, 3000);
        hold(cur, px, py, 3016, 5000);
        moveLine(cur, px, py, 300, 600, 5016, 8000);
        p->setCursorTrack(cur);
        ClickTrack clk;
        click(clk, 3800, px, py);
        click(clk, 4000, px + 6, py + 4);
        click(clk, 4200, px - 4, py - 3);
        p->setClickTrack(clk);
    }
    StyleModel *st = p->style();
    st->setPaddingPct(0);          // video fills the frame → borders are footage
    st->setCornerRadius(0);
    st->setFrameStyle(QStringLiteral("none"));
    st->setBackgroundType(QStringLiteral("color"));
    st->setBackgroundColor(QColor(0, 0, 0));
    st->setCursorStyle(QStringLiteral("dot"));
    st->setClickRipple(true);
    st->setRippleColor(QColor(0, 255, 0)); // distinctive dot colour to probe
    st->setCursorScale(1.0);

    // --- generate + determinism (regenerate twice, byte-compare) ---
    studio->regenerateZoom(p);
    const QByteArray s1 = serializeZoom(p);
    const int kfCount = p->zoom()->keyframes().size();
    studio->regenerateZoom(p);
    const QByteArray s2 = serializeZoom(p);
    const bool deterministic = (s1 == s2);

    SpringCameraEvaluator camera(p->zoom()->keyframes(), p->zoom()->motionSmoothness());
    const QRectF zAtCluster = camera.evaluate(4000);
    const QRectF zBefore = camera.evaluate(1000);
    const bool spanCoversCluster = kfCount > 0 && !isFull(zAtCluster) && isFull(zBefore);

    fprintf(stderr, "autozoom-test: keyframes=%d deterministic=%s spanCoversCluster=%s\n", kfCount,
            deterministic ? "yes" : "no", spanCoversCluster ? "yes" : "no");
    fprintf(stderr, "autozoom-test:   zoomRect@4000=(%.3f,%.3f,%.3f,%.3f) full@1000=%s\n",
            zAtCluster.x(), zAtCluster.y(), zAtCluster.width(), zAtCluster.height(),
            isFull(zBefore) ? "yes" : "no");

    // --- production spring trajectory at preview cadence -------------------
    QVector<TrajectorySample> trajectory;
    SpringCameraEvaluator traceCamera(p->zoom()->keyframes(), p->zoom()->motionSmoothness());
    CursorPlayback traceCursor(QStringLiteral("autozoom-trajectory"));
    traceCursor.setTracks(p->cursorTrack(), p->clickTrack(), p->videoSize());
    for (int frame = 0; frame <= 8 * 60; ++frame) {
        const qint64 tMs = qint64(qRound(frame * 1000.0 / 60.0));
        traceCursor.setTime(tMs);
        trajectory.append({tMs, traceCamera.evaluate(tMs), traceCamera.targetAt(tMs),
                           QPointF(traceCursor.nx(), traceCursor.ny()),
                           traceCursor.cursorVisible()});
    }
    bool trajectoryDeterministic = true;
    SpringCameraEvaluator checkCamera(p->zoom()->keyframes(), p->zoom()->motionSmoothness());
    CursorPlayback checkCursor(QStringLiteral("autozoom-check"));
    checkCursor.setTracks(p->cursorTrack(), p->clickTrack(), p->videoSize());
    for (const TrajectorySample &sample : std::as_const(trajectory)) {
        checkCursor.setTime(sample.tMs);
        if (rectDelta(checkCamera.evaluate(sample.tMs), sample.cameraRect) > 1e-12
            || std::hypot(checkCursor.nx() - sample.cursor.x(),
                          checkCursor.ny() - sample.cursor.y()) > 1e-12) {
            trajectoryDeterministic = false;
            break;
        }
    }
    const QString trajectoryPath = QDir(QDir::tempPath()).filePath(
        QStringLiteral("unisic-studio-autozoom-trajectory.csv"));
    const bool trajectoryWritten = writeTrajectoryCsv(trajectoryPath, trajectory);
    const TrajectoryMetrics motion = analyzeTrajectory(trajectory, 4400, 4900);
    const bool motionOk = motion.finite && motion.bounded && trajectoryDeterministic
                          && trajectoryWritten && motion.maxCameraEdgeJump < 0.03
                          && motion.maxCenterOvershootRatio < 0.02
                          && motion.maxZoomOvershootRatio < 0.02
                          && motion.unsettledSegments == 0 && motion.maxSettlingMs <= 1200
                          && motion.holdDrift < 0.0025 && motion.holdRmsVelocity < 0.02;
    fprintf(stderr,
            "autozoom-test: trajectory=%s deterministic=%s bounded=%s samples=%lld\n",
            qPrintable(trajectoryPath), trajectoryDeterministic ? "yes" : "no",
            motion.bounded ? "yes" : "no", static_cast<long long>(trajectory.size()));
    fprintf(stderr,
            "autozoom-test: motion edgeJump=%.6f centerV=%.3f centerA=%.3f zoomV=%.3f "
            "zoomA=%.3f overshoot(center=%.4f zoom=%.4f) hold(drift=%.6f rmsV=%.6f) "
            "settleMax=%lldms settled=%d unsettled=%d\n",
            motion.maxCameraEdgeJump, motion.maxCameraCenterVelocity,
            motion.maxCameraCenterAcceleration, motion.maxZoomVelocity,
            motion.maxZoomAcceleration, motion.maxCenterOvershootRatio,
            motion.maxZoomOvershootRatio, motion.holdDrift, motion.holdRmsVelocity,
            static_cast<long long>(motion.maxSettlingMs), motion.settledSegments,
            motion.unsettledSegments);

    // --- export with the camera + dot cursor + ripple on ---
    auto runExport = [&](const QString &out) -> bool {
        ExportController ex;
        ex.setFormat(QStringLiteral("mp4"));
        ex.setResolution(QStringLiteral("custom"));
        ex.setCustomWidth(kW);
        ex.setCustomHeight(kH);
        ex.setFpsMode(QStringLiteral("30"));
        ex.setQuality(85);
        ex.setOutputPath(out);
        QEventLoop loop;
        bool ok = false;
        QObject::connect(&ex, &ExportController::stateChanged, &loop, [&] {
            if (ex.state() == ExportController::Done) {
                ok = true;
                loop.quit();
            } else if (ex.state() == ExportController::Error) {
                fprintf(stderr, "autozoom-test:   export error: %s\n", qPrintable(ex.errorString()));
                loop.quit();
            }
        });
        ex.start(p);
        loop.exec();
        return ok;
    };

    const bool okA = runExport(outA);
    // Reference: same everything, camera OFF (clear the timeline → identity crop).
    p->zoom()->clear();
    const bool okB = runExport(outB);

    // --- 10-minute timing (decides sync vs. async) ---
    double bestMs = 1e18;
    int bigCount = 0;
    {
        CursorTrack big;
        moveLine(big, 100, 100, kW - 100, kH - 100, 0, 600000, 33);
        ClickTrack bigClk;
        for (qint64 t = 5000; t < 595000; t += 5000)
            click(bigClk, t, 200 + (t % (kW - 400)), 150 + (t % (kH - 300)));
        KeyframeEngine::Params params;
        QElapsedTimer tm;
        for (int k = 0; k < 5; ++k) {
            tm.restart();
            const auto kfs = KeyframeEngine::generate(big, bigClk, QSize(kW, kH), 600000,
                                                      QStringLiteral("16:9"), params, {});
            bestMs = std::min(bestMs, tm.nsecsElapsed() / 1.0e6);
            bigCount = kfs.size();
        }
        fprintf(stderr,
                "autozoom-test: 10min generate: best %.2f ms over 5 runs (%d samples -> %d kf)\n",
                bestMs, big.count(), bigCount);
    }

    // --- pixel probes ---
    bool pixelOk = false, greenOk = false, preOk = false;
    double borderDiff = -1, preDiff = -1, green = -1;
    int cxScreen = -1, cyScreen = -1;
    if (okA && okB) {
        const QByteArray fA4 = extractFrame(ffmpeg, outA, 4000);
        const QByteArray fB4 = extractFrame(ffmpeg, outB, 4000);
        const QByteArray fA1 = extractFrame(ffmpeg, outA, 1000);
        const QByteArray fB1 = extractFrame(ffmpeg, outB, 1000);

        // Border region: zoom crops it away, so A (zoomed) != B (flat) at 4000.
        borderDiff = regionDiff(fA4, fB4, 0, 0, 160, 160);
        // Before the cluster both are full-frame + same cursor → near identical.
        preDiff = regionDiff(fA1, fB1, 0, 0, 160, 160);

        // Expected on-screen cursor position at 4000 (evaluate + composition math).
        traceCursor.setTime(4000);
        const double nx = traceCursor.nx(), ny = traceCursor.ny();
        const double sx = (nx - zAtCluster.x()) / zAtCluster.width();
        const double sy = (ny - zAtCluster.y()) / zAtCluster.height();
        cxScreen = int(sx * kW);
        cyScreen = int(sy * kH);
        green = greenFraction(fA4, cxScreen, cyScreen, 40);

        pixelOk = borderDiff > 12.0;
        preOk = preDiff >= 0.0 && preDiff < 8.0;
        greenOk = green > 0.03;
        fprintf(stderr,
                "autozoom-test: borderDiff@4000=%.2f (want>12)  preDiff@1000=%.2f (want<8)\n",
                borderDiff, preDiff);
        fprintf(stderr, "autozoom-test: cursor dot green@(%d,%d)=%.3f (want>0.03)\n", cxScreen,
                cyScreen, green);
    }

    const bool ok = spanCoversCluster && deterministic && motionOk && okA && okB && pixelOk
                    && preOk && greenOk;
    fprintf(stderr, "autozoom-test: %s\n", ok ? "OK" : "FAIL");
    fflush(stderr);
    QCoreApplication::exit(ok ? 0 : 1);
}
