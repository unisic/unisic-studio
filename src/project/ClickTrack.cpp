#include "ClickTrack.h"

#include <QJsonObject>
#include <algorithm>

bool ClickTrack::append(const ClickEvent &e)
{
    if (!m_events.isEmpty() && e.tMs < m_events.constLast().tMs)
        return false;
    m_events.append(e);
    return true;
}

// Duplicated from CursorTrack.cpp on purpose: the two tracks are independent
// value types and sharing a helper would couple their translation units for a
// dozen trivial lines. Sort, drop empties, merge touching/overlapping.
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

void ClickTrack::excise(const QList<QPair<qint64, qint64>> &ranges)
{
    const auto norm = normalizeRanges(ranges);
    if (norm.isEmpty())
        return;

    QList<ClickEvent> kept;
    kept.reserve(m_events.size());
    for (ClickEvent e : std::as_const(m_events)) {
        qint64 shift = 0;
        bool removed = false;
        for (const auto &rg : norm) {
            if (e.tMs >= rg.first && e.tMs < rg.second) {
                removed = true;
                break;
            }
            if (rg.second <= e.tMs)
                shift += rg.second - rg.first;
        }
        if (removed)
            continue;
        e.tMs -= shift;
        kept.append(e);
    }
    m_events = std::move(kept);
}

QJsonArray ClickTrack::toJson() const
{
    QJsonArray a;
    for (const ClickEvent &e : m_events) {
        a.append(QJsonObject{
            {QStringLiteral("t"), double(e.tMs)},
            {QStringLiteral("b"), e.button},
            {QStringLiteral("s"), int(e.state)},
            {QStringLiteral("x"), e.x},
            {QStringLiteral("y"), e.y},
        });
    }
    return a;
}

ClickTrack ClickTrack::fromJson(const QJsonArray &a)
{
    ClickTrack t;
    for (const QJsonValue &v : a) {
        const QJsonObject o = v.toObject();
        ClickEvent e;
        e.tMs = qint64(o.value(QStringLiteral("t")).toDouble());
        e.button = o.value(QStringLiteral("b")).toInt();
        e.state = o.value(QStringLiteral("s")).toInt() == int(ClickEvent::Up)
                      ? ClickEvent::Up
                      : ClickEvent::Down;
        e.x = o.value(QStringLiteral("x")).toDouble();
        e.y = o.value(QStringLiteral("y")).toDouble();
        t.m_events.append(e);
    }
    return t;
}
