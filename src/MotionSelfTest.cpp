#include "MotionSelfTest.h"

#include "CursorPlayback.h"
#include "StudioApp.h"
#include "engine/KeyframeEngine.h"
#include "engine/TrajectoryMetrics.h"
#include "project/ClickTrack.h"
#include "project/CursorTrack.h"
#include "project/StudioProject.h"
#include "project/StyleModel.h"
#include "render/FrameDecoder.h"
#include "render/RenderPipeline.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSharedPointer>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace {

constexpr int kSourceWidth = 960;
constexpr int kSourceHeight = 540;
constexpr int kOutputWidth = 304;
constexpr int kOutputHeight = 540;
constexpr qint64 kDurationMs = 7000;
constexpr qint64 kTrimInMs = 500;
constexpr double kFps = 60.0;

void appendLine(CursorTrack *track, QPointF from, QPointF to, qint64 startMs, qint64 endMs)
{
    for (qint64 time = startMs; time <= endMs; time += 16) {
        const double phase = double(time - startMs) / std::max<qint64>(1, endMs - startMs);
        const QPointF point = from + (to - from) * phase;
        track->append({time, point.x(), point.y(), true, 0});
    }
}

void appendHold(CursorTrack *track, QPointF point, qint64 startMs, qint64 endMs)
{
    for (qint64 time = startMs; time <= endMs; time += 16)
        track->append({time, point.x(), point.y(), true, 0});
}

void appendClick(ClickTrack *track, qint64 timeMs, QPointF point)
{
    track->append({timeMs, Qt::LeftButton, ClickEvent::Down, point.x(), point.y()});
    track->append({timeMs + 35, Qt::LeftButton, ClickEvent::Up, point.x(), point.y()});
}

double rectDelta(const QRectF &a, const QRectF &b)
{
    return std::max({std::fabs(a.x() - b.x()), std::fabs(a.y() - b.y()),
                     std::fabs(a.width() - b.width()), std::fabs(a.height() - b.height())});
}

QByteArray extractRgbFrame(const QString &ffmpeg, const QString &video, qint64 timeMs)
{
    QProcess process;
    process.start(ffmpeg,
                  {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-ss"),
                   QString::number(timeMs / 1000.0, 'f', 3), QStringLiteral("-i"), video,
                   QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-f"),
                   QStringLiteral("rawvideo"), QStringLiteral("-pix_fmt"), QStringLiteral("rgb24"),
                   QStringLiteral("pipe:1")});
    process.waitForFinished(30000);
    return process.readAllStandardOutput();
}

struct PixelBounds {
    int width = 0;
    int height = 0;
    int pixels = 0;
    double centerX = 0.0;
    double centerY = 0.0;
};

PixelBounds greenCursorBounds(const QByteArray &rgb, int centerX, int centerY)
{
    if (rgb.size() < kOutputWidth * kOutputHeight * 3)
        return {};
    int minX = kOutputWidth, minY = kOutputHeight, maxX = -1, maxY = -1;
    qint64 sumX = 0, sumY = 0;
    int hits = 0;
    const uchar *data = reinterpret_cast<const uchar *>(rgb.constData());
    for (int y = std::max(0, centerY - 55); y <= std::min(kOutputHeight - 1, centerY + 55); ++y) {
        for (int x = std::max(0, centerX - 55); x <= std::min(kOutputWidth - 1, centerX + 55); ++x) {
            const int offset = (y * kOutputWidth + x) * 3;
            const int red = data[offset];
            const int green = data[offset + 1];
            const int blue = data[offset + 2];
            if (green - red < 65 || green - blue < 65)
                continue;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
            sumX += x;
            sumY += y;
            ++hits;
        }
    }
    return maxX >= minX ? PixelBounds{maxX - minX + 1, maxY - minY + 1, hits,
                                      double(sumX) / hits, double(sumY) / hits}
                        : PixelBounds{};
}

void exitFailure(const QString &message)
{
    fprintf(stderr, "motion-test: FAIL: %s\n", qPrintable(message));
    fflush(stderr);
    QCoreApplication::exit(1);
}

} // namespace

void MotionSelfTest::run(StudioApp *studio)
{
    const QString ffmpeg = FrameDecoder::ffmpegPath();
    if (ffmpeg.isEmpty()) {
        exitFailure(QStringLiteral("ffmpeg not found"));
        return;
    }

    static QTemporaryDir sourceDir;
    if (!sourceDir.isValid()) {
        exitFailure(QStringLiteral("could not create source temp dir"));
        return;
    }
    const QString source = sourceDir.filePath(QStringLiteral("motion-source.mp4"));
    const QString output =
        QDir(QDir::tempPath()).filePath(QStringLiteral("unisic-studio-motion-test.mp4"));
    const QString trajectoryPath =
        QDir(QDir::tempPath()).filePath(QStringLiteral("unisic-studio-motion-trajectory.csv"));
    QFile::remove(output);

    QProcess generator;
    generator.start(ffmpeg,
                    {QStringLiteral("-y"), QStringLiteral("-v"), QStringLiteral("error"),
                     QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
                     QStringLiteral("testsrc2=size=%1x%2:rate=60:duration=7")
                         .arg(kSourceWidth)
                         .arg(kSourceHeight),
                     QStringLiteral("-vf"), QStringLiteral("hue=s=0"),
                     QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), source});
    if (!generator.waitForFinished(60000) || generator.exitCode() != 0
        || !QFileInfo::exists(source)) {
        exitFailure(QStringLiteral("could not synthesize source clip"));
        return;
    }

    auto *project = new StudioProject(qApp);
    project->setVideoAbsPath(source);
    project->setDurationMs(kDurationMs);
    project->setFps(kFps);
    project->setVideoSize(QSize(kSourceWidth, kSourceHeight));
    project->setCursorMode(QStringLiteral("metadata"));
    project->setHadClickCapture(true);

    const QPointF start(150, 135);
    const QPointF focus(700, 300);
    CursorTrack cursor;
    appendHold(&cursor, start, 0, 600);
    appendLine(&cursor, start, focus, 616, 1800);
    appendHold(&cursor, focus, 1816, 4300);
    const QPointF finalCursor = focus + QPointF(30, 0);
    appendLine(&cursor, focus, finalCursor, 4316, 4700);
    appendHold(&cursor, finalCursor, 4716, kDurationMs);
    project->setCursorTrack(cursor);
    ClickTrack clicks;
    appendClick(&clicks, 2600, focus);
    appendClick(&clicks, 2800, focus + QPointF(5, -4));
    project->setClickTrack(clicks);

    StyleModel *style = project->style();
    style->setAspect(QStringLiteral("9:16"));
    style->setFillMode(QStringLiteral("fill"));
    style->setPaddingPct(0);
    style->setCornerRadius(0);
    style->setShadowOpacity(0);
    style->setFrameStyle(QStringLiteral("none"));
    style->setBackgroundType(QStringLiteral("gradient"));
    style->setCursorStyle(QStringLiteral("dot"));
    style->setCursorScale(1.65);
    style->setClickRipple(true);
    style->setRippleColor(QColor(0, 255, 0));

    studio->regenerateZoom(project);
    // Final manual target exercises the supported editor maximum independently
    // of the smarter auto cap (2.55x). The cursor-size compensation must remain
    // readable and crisp at 2.8x too.
    studio->addManualZoom(project, 6300, finalCursor.x() / kSourceWidth,
                          finalCursor.y() / kSourceHeight, 2.8);

    RenderPipeline::Settings settings;
    settings.master = source;
    settings.style = style;
    settings.videoSize = project->videoSize();
    settings.trimInMs = kTrimInMs;
    settings.durMs = kDurationMs - kTrimInMs;
    settings.fps = kFps;
    settings.outW = kOutputWidth;
    settings.outH = kOutputHeight;
    settings.format = QStringLiteral("mp4");
    settings.crf = 20;
    settings.outputPath = output;
    settings.keyframes = project->zoom()->keyframes();
    settings.motionSmoothness = project->zoom()->motionSmoothness();
    settings.cursor = project->cursorTrack();
    settings.clicks = project->clickTrack();
    settings.projectId = QStringLiteral("motion-self-test");
    settings.collectMotionSamples = true;

    auto samples = QSharedPointer<QVector<TrajectorySample>>::create();
    auto targetLookup = QSharedPointer<SpringCameraEvaluator>::create(
        settings.keyframes, settings.motionSmoothness);
    auto *pipeline = new RenderPipeline(qApp);
    QObject::connect(pipeline, &RenderPipeline::motionSampled, qApp,
                     [samples, targetLookup](int, qint64 timeMs, const QRectF &rect,
                                             const QPointF &cursorPosition, bool visible) {
        samples->append({timeMs, rect, targetLookup->targetAt(timeMs), cursorPosition, visible});
    });
    QObject::connect(pipeline, &RenderPipeline::failed, qApp,
                     [](const QString &message) { exitFailure(message); });
    QObject::connect(pipeline, &RenderPipeline::finished, qApp,
                     [samples, project, settings, output, trajectoryPath, ffmpeg] {
        const bool csvOk = writeTrajectoryCsv(trajectoryPath, *samples);
        const TrajectoryMetrics metrics = analyzeTrajectory(*samples, 3300, 3750);

        bool deterministic = true;
        SpringCameraEvaluator camera(settings.keyframes, settings.motionSmoothness);
        CursorPlayback cursorPlayback(QStringLiteral("motion-direct"));
        cursorPlayback.setTracks(settings.cursor, settings.clicks, settings.videoSize);
        for (const TrajectorySample &sample : std::as_const(*samples)) {
            cursorPlayback.setTime(sample.tMs);
            if (rectDelta(camera.evaluate(sample.tMs), sample.cameraRect) > 1e-12
                || std::hypot(cursorPlayback.nx() - sample.cursor.x(),
                              cursorPlayback.ny() - sample.cursor.y()) > 1e-12) {
                deterministic = false;
                break;
            }
        }

        QProcess verify;
        verify.start(ffmpeg,
                     {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"), output,
                      QStringLiteral("-f"), QStringLiteral("null"), QStringLiteral("-")});
        const bool decoded = verify.waitForFinished(60000) && verify.exitCode() == 0;
        const int expectedFrames = int(std::lround(settings.durMs / 1000.0 * kFps));
        const bool frameCountOk = std::abs(samples->size() - expectedFrames) <= 1;
        const bool outputOk = QFileInfo::exists(output) && QFileInfo(output).size() > 0 && decoded;

        auto sampleAt = [samples](qint64 timeMs) -> TrajectorySample {
            const auto it = std::min_element(
                samples->cbegin(), samples->cend(), [timeMs](const TrajectorySample &a,
                                                             const TrajectorySample &b) {
                    return std::llabs(a.tMs - timeMs) < std::llabs(b.tMs - timeMs);
                });
            return it == samples->cend() ? TrajectorySample{} : *it;
        };
        const qint64 cursorProbeTimes[] = {1000, 3500, 6800};
        PixelBounds cursorBounds[3];
        double cameraZooms[3] = {};
        double cursorCenterErrors[3] = {};
        const double baseCameraWidth = KeyframeEngine::cameraRect(
            settings.videoSize, QStringLiteral("9:16"), QPointF(0.5, 0.5), 1.0).width();
        bool cursorScaleOk = true;
        for (int i = 0; i < 3; ++i) {
            const TrajectorySample sample = sampleAt(cursorProbeTimes[i]);
            cameraZooms[i] = baseCameraWidth / std::max(1.0e-9, sample.cameraRect.width());
            const int x = int(std::lround((sample.cursor.x() - sample.cameraRect.x())
                                          / sample.cameraRect.width() * kOutputWidth));
            const int y = int(std::lround((sample.cursor.y() - sample.cameraRect.y())
                                          / sample.cameraRect.height() * kOutputHeight));
            cursorBounds[i] = greenCursorBounds(
                extractRgbFrame(ffmpeg, output,
                                cursorProbeTimes[i] - settings.trimInMs), x, y);
            cursorCenterErrors[i] = std::hypot(cursorBounds[i].centerX - x,
                                               cursorBounds[i].centerY - y);
            cursorScaleOk = cursorScaleOk && cursorBounds[i].width >= 8
                            && cursorBounds[i].height >= 8 && cursorBounds[i].width <= 50
                            && cursorBounds[i].height <= 50 && cursorCenterErrors[i] < 3.5;
        }
        const int minCursorWidth = std::min({cursorBounds[0].width, cursorBounds[1].width,
                                             cursorBounds[2].width});
        const int maxCursorWidth = std::max({cursorBounds[0].width, cursorBounds[1].width,
                                             cursorBounds[2].width});
        const int minCursorHeight = std::min({cursorBounds[0].height, cursorBounds[1].height,
                                              cursorBounds[2].height});
        const int maxCursorHeight = std::max({cursorBounds[0].height, cursorBounds[1].height,
                                              cursorBounds[2].height});
        cursorScaleOk = cursorScaleOk && minCursorWidth > 0
                         && double(maxCursorWidth) / minCursorWidth < 1.30
                         && minCursorHeight > 0
                         && double(maxCursorHeight) / minCursorHeight < 1.30
                         && cameraZooms[0] < 1.05 && cameraZooms[1] > 1.35
                        && cameraZooms[2] > 2.65;
        // Follow-pan smoothness guard. The old distance-gated pan targets produced a
        // "staircase" the per-frame edge-jump budget (0.03) could NOT catch: a run of
        // sub-jump steps still passes it. Frame-to-frame center ACCELERATION is the
        // discriminator — a staircase spikes it, a smoothed glide keeps it low. This
        // budget is provisional: it is set generously so it never false-fails, and
        // should be tightened toward the measured post-smoothing baseline the first
        // time --motion-test runs on a live Wayland session (the value is printed as
        // "centerA=" below). See emitDeadZonePan() in KeyframeEngine.cpp.
        constexpr double kCenterAccelBudget = 40.0;   // frame-fractions / s^2, provisional
        const bool metricsOk = metrics.finite && metrics.bounded
                               && metrics.maxCameraEdgeJump < 0.03
                               && metrics.maxCameraCenterVelocity < 1.40
                               && metrics.maxCameraCenterAcceleration < kCenterAccelBudget
                               && metrics.maxZoomVelocity < 3.0
                               && metrics.maxCursorVelocity < 3.05
                               && metrics.maxCenterOvershootRatio < 0.02
                               && metrics.maxZoomOvershootRatio < 0.02
                               && metrics.unsettledSegments == 0
                               && metrics.maxSettlingMs <= 1200
                               && metrics.holdDrift < 0.0025
                               && metrics.holdRmsVelocity < 0.02;

        fprintf(stderr,
                "motion-test: trim=%lldms frames=%lld expected=%d output=%s decoded=%s "
                "deterministic=%s "
                "bounded=%s\n",
                static_cast<long long>(settings.trimInMs),
                static_cast<long long>(samples->size()), expectedFrames, qPrintable(output),
                decoded ? "yes" : "no", deterministic ? "yes" : "no",
                metrics.bounded ? "yes" : "no");
        fprintf(stderr,
                "motion-test: edgeJump=%.6f centerV=%.3f centerA=%.3f zoomV=%.3f "
                "zoomA=%.3f cursorV=%.3f cursorA=%.3f\n",
                metrics.maxCameraEdgeJump, metrics.maxCameraCenterVelocity,
                metrics.maxCameraCenterAcceleration, metrics.maxZoomVelocity,
                metrics.maxZoomAcceleration, metrics.maxCursorVelocity,
                metrics.maxCursorAcceleration);
        fprintf(stderr,
                "motion-test: overshoot(center=%.4f zoom=%.4f) hold(drift=%.6f rmsV=%.6f) "
                "settleMax=%lldms settled=%d unsettled=%d trajectory=%s\n",
                metrics.maxCenterOvershootRatio, metrics.maxZoomOvershootRatio,
                metrics.holdDrift, metrics.holdRmsVelocity,
                static_cast<long long>(metrics.maxSettlingMs), metrics.settledSegments,
                metrics.unsettledSegments, qPrintable(trajectoryPath));
        fprintf(stderr,
                "motion-test: 9:16 cursor bounds 1x=%dx%d(e=%.2fpx)  "
                "auto(%.2fx)=%dx%d(e=%.2fpx)  manual(%.2fx)=%dx%d(e=%.2fpx) "
                "compensation=%s\n",
                cursorBounds[0].width, cursorBounds[0].height, cursorCenterErrors[0],
                cameraZooms[1], cursorBounds[1].width, cursorBounds[1].height,
                cursorCenterErrors[1], cameraZooms[2], cursorBounds[2].width,
                cursorBounds[2].height, cursorCenterErrors[2],
                cursorScaleOk ? "yes" : "no");

        const bool ok = csvOk && outputOk && frameCountOk && deterministic && metricsOk
                        && cursorScaleOk;
        fprintf(stderr, "motion-test: %s\n", ok ? "OK" : "FAIL");
        fflush(stderr);
        QCoreApplication::exit(ok ? 0 : 1);
        Q_UNUSED(project)
    });
    QTimer::singleShot(120000, pipeline, [pipeline] {
        pipeline->cancel();
        exitFailure(QStringLiteral("export timed out"));
    });
    pipeline->start(settings);
}
