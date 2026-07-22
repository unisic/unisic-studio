#include "RecordingAssembler.h"
#include "RecorderMath.h"

#include "project/ClickTrack.h"
#include "project/CursorTrack.h"
#include "project/StudioProject.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <algorithm>

namespace RecordingAssembler {

QList<QPair<qint64, qint64>> coalesceTypingBursts(const QVector<qint64> &downMs,
                                                  qint64 mergeGapMs, qint64 tailPadMs,
                                                  int minKeysPerBurst)
{
    QList<QPair<qint64, qint64>> out;
    if (downMs.isEmpty())
        return out;
    QVector<qint64> t = downMs;
    std::sort(t.begin(), t.end());

    qint64 start = t.first();
    qint64 last = t.first();
    int keys = 1;
    auto flush = [&] {
        if (keys >= minKeysPerBurst)
            out.append({start, last + tailPadMs});
    };
    for (int i = 1; i < t.size(); ++i) {
        if (t.at(i) - last <= mergeGapMs) {
            last = t.at(i);
            ++keys;
        } else {
            flush();
            start = t.at(i);
            last = t.at(i);
            keys = 1;
        }
    }
    flush();
    return out;
}

namespace {
// Excise pause ranges from a set of point times: a time inside any [s,e) is
// dropped, and every later time shifts left by the total excised length before
// it. Mirrors CursorTrack/ClickTrack::excise so typing stays in sync with them.
QVector<qint64> exciseTimes(const QVector<qint64> &times,
                            const QList<QPair<qint64, qint64>> &ranges)
{
    if (ranges.isEmpty())
        return times;
    QList<QPair<qint64, qint64>> norm = ranges;
    std::sort(norm.begin(), norm.end());
    QVector<qint64> out;
    out.reserve(times.size());
    for (qint64 t : times) {
        qint64 shift = 0;
        bool dropped = false;
        for (const auto &r : norm) {
            if (t >= r.first && t < r.second) { dropped = true; break; }
            if (t >= r.second) shift += r.second - r.first;
        }
        if (!dropped)
            out.append(qMax<qint64>(0, t - shift));
    }
    return out;
}
} // namespace

bool assembleAndSave(const Input &in, QString *error)
{
    // Pause spans -> normalized video-ms ranges. Same computation (RecorderMath)
    // the recorder used for the ffmpeg excise pass, so the tracks and the master
    // are cut on identical boundaries.
    const QList<QPair<qint64, qint64>> videoMsRanges =
        RecorderMath::pauseRangesToVideoMs(in.pauseMonoNs, in.t0MonoNs);

    // --- cursor track ---------------------------------------------------------
    CursorTrack cursor;

    // Shapes: assign each spa_meta_cursor id a stable 0-based track index (the
    // CursorSample::shapeId convention) and PNG-encode the bitmap.
    QList<CursorShape> shapeList;
    QHash<int, int> spaToIndex;
    shapeList.reserve(in.shapes.size());
    for (const RawShape &sh : in.shapes) {
        const int index = shapeList.size();
        QByteArray png;
        {
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            sh.image.save(&buf, "PNG");
        }
        shapeList.append(CursorShape{index, sh.hotspot.x(), sh.hotspot.y(), png});
        spaToIndex.insert(sh.spaId, index);
    }
    cursor.setShapes(shapeList);

    for (const RawCursor &c : in.cursors) {
        // Clamp to 0: a sample captured just before the first frame maps negative,
        // but video time starts at 0. Equal clamped timestamps are allowed.
        const qint64 tMs = qMax<qint64>(0, RecorderMath::monoNsToVideoMs(c.tMonoNs, in.t0MonoNs));
        const int shapeIdx = spaToIndex.value(c.spaShapeId, -1);
        cursor.append(CursorSample{tMs, c.x, c.y, c.visible, shapeIdx});
    }

    // --- click track ----------------------------------------------------------
    // Resolve each click's position from the cursor track BEFORE excision, so the
    // click agrees with where the pointer was; the excise below then shifts both
    // together. sample() clamps clicks before the first / after the last sample.
    ClickTrack clicks;
    for (const RawClick &k : in.clicks) {
        const qint64 tMs = qMax<qint64>(0, RecorderMath::monoNsToVideoMs(k.tMonoNs, in.t0MonoNs));
        const CursorSample at = cursor.sample(tMs);
        ClickEvent e;
        e.tMs = tMs;
        e.button = k.button;
        e.state = k.pressed ? ClickEvent::Down : ClickEvent::Up;
        e.x = at.x;
        e.y = at.y;
        clicks.append(e);
    }

    // --- typing bursts --------------------------------------------------------
    // Key-down times → video-ms, excise pauses (in sync with cursor/clicks), then
    // coalesce into [start,end] bursts. Timing only; no key content exists here.
    QList<QPair<qint64, qint64>> typingBursts;
    if (!in.keyDownMonoNs.isEmpty()) {
        QVector<qint64> downMs;
        downMs.reserve(in.keyDownMonoNs.size());
        for (qint64 ns : in.keyDownMonoNs)
            downMs.append(qMax<qint64>(0, RecorderMath::monoNsToVideoMs(ns, in.t0MonoNs)));
        downMs = exciseTimes(downMs, videoMsRanges);
        // ~1.5s gap splits bursts; hold ~0.6s past the last stroke; ignore < 3
        // strokes so a stray keypress can't yank the camera.
        typingBursts = coalesceTypingBursts(downMs, 1500, 600, 3);
    }

    // Same ranges cut from both tracks (and the master) → gapless, still in sync.
    cursor.excise(videoMsRanges);
    clicks.excise(videoMsRanges);

    // --- project --------------------------------------------------------------
    qint64 excisedMs = 0;
    for (const auto &r : videoMsRanges)
        excisedMs += r.second - r.first;
    const qint64 totalMs = in.fps > 0 ? qRound64(in.framesWritten * 1000.0 / in.fps) : 0;
    const qint64 durationMs = qMax<qint64>(0, totalMs - excisedMs);

    StudioProject project;
    project.setVideoAbsPath(in.masterAbsPath);
    // Master + sidecar share a directory, so the relative path is just the name.
    project.setVideoRelPath(
        QFileInfo(in.sidecarPath).absoluteDir().relativeFilePath(in.masterAbsPath));
    project.setDurationMs(durationMs);
    project.setFps(in.fps);
    project.setVideoSize(in.videoSize);
    project.setVideoHash(StudioProject::computeVideoHash(in.masterAbsPath));
    project.setCompositor(in.compositor);
    project.setCursorMode(in.cursorMode);
    project.setT0MonoNs(in.t0MonoNs);
    project.setHadClickCapture(in.hadClickCapture);
    if (!in.webcamAbsPath.isEmpty()) {
        project.setWebcamAbsPath(in.webcamAbsPath);
        project.setWebcamRelPath(
            QFileInfo(in.sidecarPath).absoluteDir().relativeFilePath(in.webcamAbsPath));
    }
    project.setCursorTrack(cursor);
    project.setClickTrack(clicks);
    project.setTypingBursts(typingBursts);

    QString saveErr;
    if (!project.save(in.sidecarPath, &saveErr)) {
        if (error)
            *error = saveErr.isEmpty() ? QStringLiteral("could not write the project file") : saveErr;
        return false;
    }
    return true;
}

} // namespace RecordingAssembler
