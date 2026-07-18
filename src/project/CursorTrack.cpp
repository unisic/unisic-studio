#include "CursorTrack.h"

#include <QJsonArray>
#include <algorithm>

bool CursorTrack::append(const CursorSample &s)
{
    if (!m_samples.isEmpty() && s.tMs < m_samples.constLast().tMs)
        return false;   // out-of-order — drop it (see header)
    m_samples.append(s);
    return true;
}

qint64 CursorTrack::durationMs() const
{
    return m_samples.isEmpty() ? 0 : m_samples.constLast().tMs;
}

CursorSample CursorTrack::sample(qint64 t) const
{
    if (m_samples.isEmpty())
        return CursorSample{t, 0.0, 0.0, false, -1};

    // First sample with tMs >= t.
    const auto it = std::lower_bound(
        m_samples.cbegin(), m_samples.cend(), t,
        [](const CursorSample &s, qint64 tt) { return s.tMs < tt; });

    if (it == m_samples.cbegin()) {         // at or before the first sample
        CursorSample r = m_samples.constFirst();
        r.tMs = t;
        return r;
    }
    if (it == m_samples.cend()) {           // after the last sample
        CursorSample r = m_samples.constLast();
        r.tMs = t;
        return r;
    }

    const CursorSample &hi = *it;
    if (hi.tMs == t)                        // exact hit
        return CursorSample{t, hi.x, hi.y, hi.visible, hi.shapeId};

    const CursorSample &lo = *(it - 1);
    const qint64 span = hi.tMs - lo.tMs;
    // Guard equal-timestamp neighbours (allowed by append) against /0.
    const double f = span > 0 ? double(t - lo.tMs) / double(span) : 0.0;
    // visible/shapeId inherit from the earlier neighbour; only x/y interpolate.
    return CursorSample{t,
                        lo.x + f * (hi.x - lo.x),
                        lo.y + f * (hi.y - lo.y),
                        lo.visible, lo.shapeId};
}

// Sort by start, then merge any ranges that touch or overlap. Empty/reversed
// ranges (start >= end) are dropped. The result is disjoint and ordered, which
// lets excise() sweep it in a single pass.
static QList<QPair<qint64, qint64>> normalizeRanges(QList<QPair<qint64, qint64>> r)
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

void CursorTrack::excise(const QList<QPair<qint64, qint64>> &ranges)
{
    const auto norm = normalizeRanges(ranges);
    if (norm.isEmpty())
        return;

    QList<CursorSample> kept;
    kept.reserve(m_samples.size());
    for (CursorSample s : std::as_const(m_samples)) {
        qint64 shift = 0;
        bool removed = false;
        for (const auto &rg : norm) {
            if (s.tMs >= rg.first && s.tMs < rg.second) {   // inside a hole
                removed = true;
                break;
            }
            if (rg.second <= s.tMs)                          // hole fully before us
                shift += rg.second - rg.first;
        }
        if (removed)
            continue;
        s.tMs -= shift;
        kept.append(s);
    }
    m_samples = std::move(kept);
}

QJsonObject CursorTrack::toJson() const
{
    QJsonArray dt, xs, ys, visRuns, shapeRuns;

    qint64 prevT = 0;
    int prevX = 0, prevY = 0;
    for (int i = 0; i < m_samples.size(); ++i) {
        const CursorSample &s = m_samples.at(i);
        const int xi = qRound(s.x);
        const int yi = qRound(s.y);
        if (i == 0) {
            dt.append(double(s.tMs));
            xs.append(xi);
            ys.append(yi);
        } else {
            dt.append(double(s.tMs - prevT));
            xs.append(xi - prevX);
            ys.append(yi - prevY);
        }
        prevT = s.tMs;
        prevX = xi;
        prevY = yi;
    }

    // visible: RLE of the FALSE runs only (default is true).
    for (int i = 0; i < m_samples.size();) {
        if (m_samples.at(i).visible) { ++i; continue; }
        const int start = i;
        while (i < m_samples.size() && !m_samples.at(i).visible)
            ++i;
        visRuns.append(QJsonArray{start, i - start});
    }

    // shape: RLE pairs [shapeId, runLen] covering every sample.
    for (int i = 0; i < m_samples.size();) {
        const int id = m_samples.at(i).shapeId;
        int run = 0;
        while (i < m_samples.size() && m_samples.at(i).shapeId == id) {
            ++i;
            ++run;
        }
        shapeRuns.append(QJsonArray{id, run});
    }

    QJsonArray shapes;
    for (const CursorShape &sh : m_shapes) {
        shapes.append(QJsonObject{
            {QStringLiteral("id"), sh.id},
            {QStringLiteral("hx"), sh.hotspotX},
            {QStringLiteral("hy"), sh.hotspotY},
            {QStringLiteral("png"), QString::fromLatin1(sh.png.toBase64())},
        });
    }

    return QJsonObject{
        {QStringLiteral("dt"), dt},
        {QStringLiteral("x"), xs},
        {QStringLiteral("y"), ys},
        {QStringLiteral("visible"), visRuns},
        {QStringLiteral("shape"), shapeRuns},
        {QStringLiteral("shapes"), shapes},
    };
}

CursorTrack CursorTrack::fromJson(const QJsonObject &o)
{
    CursorTrack t;

    const QJsonArray dt = o.value(QStringLiteral("dt")).toArray();
    const QJsonArray xs = o.value(QStringLiteral("x")).toArray();
    const QJsonArray ys = o.value(QStringLiteral("y")).toArray();
    const int n = dt.size();

    qint64 accT = 0;
    qint64 accX = 0, accY = 0;
    for (int i = 0; i < n; ++i) {
        // Clamp: a negative delta in a corrupt/hand-edited file would make tMs
        // non-monotonic and break sample()'s binary search invariant.
        accT += qMax<qint64>(0, qint64(dt.at(i).toDouble()));
        accX += qint64(xs.at(i).toDouble());
        accY += qint64(ys.at(i).toDouble());
        // First element is absolute; deltas above make the accumulator absolute
        // from i==0 too since acc starts at 0.
        CursorSample s;
        s.tMs = accT;
        s.x = double(accX);
        s.y = double(accY);
        s.visible = true;
        s.shapeId = -1;
        t.m_samples.append(s);
    }

    for (const QJsonValue &v : o.value(QStringLiteral("visible")).toArray()) {
        const QJsonArray run = v.toArray();
        const int start = run.at(0).toInt();
        const int len = run.at(1).toInt();
        for (int i = start; i < start + len && i < t.m_samples.size(); ++i)
            t.m_samples[i].visible = false;
    }

    int idx = 0;
    for (const QJsonValue &v : o.value(QStringLiteral("shape")).toArray()) {
        const QJsonArray run = v.toArray();
        const int id = run.at(0).toInt();
        const int len = run.at(1).toInt();
        for (int i = 0; i < len && idx < t.m_samples.size(); ++i, ++idx)
            t.m_samples[idx].shapeId = id;
    }

    for (const QJsonValue &v : o.value(QStringLiteral("shapes")).toArray()) {
        const QJsonObject so = v.toObject();
        CursorShape sh;
        sh.id = so.value(QStringLiteral("id")).toInt();
        sh.hotspotX = so.value(QStringLiteral("hx")).toInt();
        sh.hotspotY = so.value(QStringLiteral("hy")).toInt();
        sh.png = QByteArray::fromBase64(so.value(QStringLiteral("png")).toString().toLatin1());
        t.m_shapes.append(sh);
    }

    return t;
}
