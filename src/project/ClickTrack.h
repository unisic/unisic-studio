#pragma once
#include <QJsonArray>
#include <QList>
#include <QPair>
#include <cstdint>

// Mouse-button events over the video timeline. Captured from libinput, which
// gives button + timestamp only — the x/y here are resolved after the fact from
// the CursorTrack at the same instant, so a click always agrees with where the
// pointer was. Value type, like CursorTrack: Core-only, testable offscreen.
//
// JSON is a plain array of {t,b,s,x,y} objects (clicks are sparse, so the
// columnar delta packing that pays off for the dense cursor track would only
// add complexity here). state is stored as an int (0 = Down, 1 = Up).
struct ClickEvent {
    enum State { Down = 0, Up = 1 };

    qint64 tMs = 0;
    int button = 0;     // Qt::MouseButton-compatible (Qt::LeftButton, ...)
    State state = Down;
    double x = 0.0;
    double y = 0.0;
};

class ClickTrack
{
public:
    // Non-decreasing timeline, same contract as CursorTrack::append: an event
    // earlier than the last is dropped (returns false). Equal timestamps are
    // allowed (a fast down/up or two buttons can share a millisecond).
    bool append(const ClickEvent &e);

    bool isEmpty() const { return m_events.isEmpty(); }
    int count() const { return m_events.size(); }
    const QList<ClickEvent> &events() const { return m_events; }
    void clear() { m_events.clear(); }

    // Same pause-removal semantics as CursorTrack::excise: events inside any
    // [start, end) range are removed and later timestamps shift left by the
    // total excised length before them. end exclusive; ranges normalized.
    void excise(const QList<QPair<qint64, qint64>> &ranges);

    QJsonArray toJson() const;
    static ClickTrack fromJson(const QJsonArray &a);

private:
    QList<ClickEvent> m_events;
};
