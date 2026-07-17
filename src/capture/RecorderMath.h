#pragma once
#include <QList>
#include <QPair>
#include <algorithm>
#include <cstdint>

// Pure time-mapping helpers shared by StudioRecorder and its unit tests. Header-
// only and Core-only (QList/QPair) so RecorderMathTest links nothing heavy and
// the arithmetic can be exercised entirely offscreen.
//
// One clock underlies everything: CLOCK_MONOTONIC. Video frames, cursor samples
// and click events are all stamped in that domain; t0MonoNs is the pts of the
// FIRST sampled frame, so it is the origin of the video timeline.
namespace RecorderMath {

// CLOCK_MONOTONIC ns → video-ms. A frame/cursor/click stamped exactly at t0 maps
// to 0. Rounds to the nearest millisecond, symmetric around zero. May be negative
// for a sample captured just before the first frame (cursor meta can arrive during
// the portal/countdown pre-roll); the recorder clamps those to 0 when it builds a
// track, but the mapping itself stays pure so the offset is testable.
inline qint64 monoNsToVideoMs(qint64 tMonoNs, qint64 t0MonoNs)
{
    const qint64 d = tMonoNs - t0MonoNs;
    return d >= 0 ? (d + 500000) / 1000000 : -(((-d) + 500000) / 1000000);
}

// Sort, drop empty/reversed spans, merge any that touch or overlap into a disjoint
// ascending list. Same contract as CursorTrack's internal normalizer, so the
// ranges handed to ffmpeg's excise filter and to the tracks agree exactly.
inline QList<QPair<qint64, qint64>> normalizeRanges(QList<QPair<qint64, qint64>> r)
{
    r.erase(std::remove_if(r.begin(), r.end(),
                           [](const QPair<qint64, qint64> &p) { return p.first >= p.second; }),
            r.end());
    std::sort(r.begin(), r.end());
    QList<QPair<qint64, qint64>> out;
    for (const auto &p : std::as_const(r)) {
        if (!out.isEmpty() && p.first <= out.last().second)
            out.last().second = qMax(out.last().second, p.second);
        else
            out.append(p);
    }
    return out;
}

// Pause spans recorded as [startMonoNs, endMonoNs) → normalized video-ms ranges,
// ready for CursorTrack/ClickTrack::excise and the ffmpeg excision filtergraph.
inline QList<QPair<qint64, qint64>> pauseRangesToVideoMs(
    const QList<QPair<qint64, qint64>> &monoNsRanges, qint64 t0MonoNs)
{
    QList<QPair<qint64, qint64>> ms;
    ms.reserve(monoNsRanges.size());
    for (const auto &r : monoNsRanges)
        ms.append({monoNsToVideoMs(r.first, t0MonoNs), monoNsToVideoMs(r.second, t0MonoNs)});
    return normalizeRanges(ms);
}

} // namespace RecorderMath
