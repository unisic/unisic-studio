#include "RecordingAssembler.h"
#include "RecorderMath.h"

#include "project/ClickTrack.h"
#include "project/CursorTrack.h"
#include "project/StudioProject.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QHash>

namespace RecordingAssembler {

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

    QString saveErr;
    if (!project.save(in.sidecarPath, &saveErr)) {
        if (error)
            *error = saveErr.isEmpty() ? QStringLiteral("could not write the project file") : saveErr;
        return false;
    }
    return true;
}

} // namespace RecordingAssembler
