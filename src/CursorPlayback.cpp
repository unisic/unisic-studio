#include "CursorPlayback.h"

#include "engine/CursorSmoother.h"

#include <QImage>

#include <utility>

// ---------------------------------------------------------------------------
// CursorRippleModel
// ---------------------------------------------------------------------------
CursorRippleModel::CursorRippleModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int CursorRippleModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant CursorRippleModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Ripple &r = m_rows.at(index.row());
    switch (role) {
    case NxRole:  return r.nx;
    case NyRole:  return r.ny;
    case TMsRole: return double(r.tMs);
    default:      return {};
    }
}

QHash<int, QByteArray> CursorRippleModel::roleNames() const
{
    return {{NxRole, "nx"}, {NyRole, "ny"}, {TMsRole, "tMs"}};
}

void CursorRippleModel::setActive(const QVector<Ripple> &next)
{
    // Identity == the click's index; when the identity list is unchanged the
    // active set has not changed (only the phase, which QML derives from time),
    // so we emit nothing and keep the delegates alive.
    if (next.size() == m_rows.size()) {
        bool same = true;
        for (int i = 0; i < next.size(); ++i)
            if (next.at(i).idx != m_rows.at(i).idx) { same = false; break; }
        if (same)
            return;
    }
    beginResetModel();
    m_rows = next;
    endResetModel();
}

// ---------------------------------------------------------------------------
// CursorPlayback
// ---------------------------------------------------------------------------
CursorPlayback::CursorPlayback(QString projectId, QObject *parent)
    : QObject(parent)
    , m_projectId(std::move(projectId))
    , m_ripples(new CursorRippleModel(this))
{
}

void CursorPlayback::setTracks(const CursorTrack &cursor, const ClickTrack &clicks, QSize videoSize)
{
    // The overlay renders from a SMOOTHED cursor track: raw PipeWire samples are
    // integer stream pixels that jitter a pixel or two at rest, so a raw pointer
    // twitches. One-euro filtering (engine defaults) gives a virtual cursor that
    // glides. tMs / visible / shapeId pass through the filter unchanged, so the
    // O(1) index lookup and shape/visibility logic are identical to the raw track;
    // only x/y are eased. This is a private cached copy — the project keeps the raw
    // track for the zoom engine and click-position resolution.
    m_cursor.clear();
    const CursorSmoother smoother;
    for (const CursorSample &s : smoother.smooth(cursor))
        m_cursor.append(s);
    m_invW = videoSize.width() > 0 ? 1.0 / videoSize.width() : 1.0;
    m_invH = videoSize.height() > 0 ? 1.0 / videoSize.height() : 1.0;

    m_downs.clear();
    for (const ClickEvent &e : clicks.events())
        if (e.state == ClickEvent::Down)
            m_downs.append(e);

    m_shapes.clear();
    for (const CursorShape &sh : cursor.shapes()) {
        ShapeInfo info;
        info.hotspotX = sh.hotspotX;
        info.hotspotY = sh.hotspotY;
        QImage img;
        if (img.loadFromData(sh.png, "PNG") && !img.isNull()) {
            info.w = img.width();
            info.h = img.height();
        }
        info.url = QStringLiteral("image://cursorshape/%1/%2").arg(m_projectId).arg(sh.id);
        m_shapes.insert(sh.id, info);
    }

    m_lastIdx = 0;
    m_downCursor = 0;
    m_prevT = -1;
    m_shapeId = -2;
    // Re-evaluate at the current time against the new tracks.
    const qint64 t = qint64(m_timeMs);
    m_timeMs = -1; // force setTime to re-emit
    setTime(t);
}

int CursorPlayback::sampleIndexFor(qint64 t) const
{
    const QList<CursorSample> &s = m_cursor.samples();
    const int n = s.size();
    if (n == 0)
        return -1;
    int i = m_lastIdx;
    if (i < 0) i = 0;
    if (i > n - 1) i = n - 1;
    // Walk the cached hint toward t. Sequential playback moves it 0-1 steps.
    if (s.at(i).tMs <= t) {
        while (i + 1 < n && s.at(i + 1).tMs <= t)
            ++i;
    } else {
        while (i > 0 && s.at(i).tMs > t)
            --i;
    }
    m_lastIdx = i;
    return i;
}

void CursorPlayback::applyShape(int shapeId)
{
    if (shapeId == m_shapeId)
        return;
    m_shapeId = shapeId;
    const auto it = m_shapes.constFind(shapeId);
    if (it == m_shapes.constEnd()) {
        m_shapeUrl.clear();
        m_shapeW = m_shapeH = 0;
        m_hotspotX = m_hotspotY = 0.0;
    } else {
        m_shapeUrl = it->url;
        m_shapeW = it->w;
        m_shapeH = it->h;
        m_hotspotX = it->hotspotX;
        m_hotspotY = it->hotspotY;
    }
    emit shapeChanged();
}

void CursorPlayback::setTime(qint64 tMs)
{
    if (qint64(m_timeMs) == tMs && m_prevT == tMs)
        return;
    const bool timeMoved = qint64(m_timeMs) != tMs;
    m_timeMs = tMs;
    if (timeMoved)
        emit timeChanged();

    // --- cursor sample ---
    const QList<CursorSample> &s = m_cursor.samples();
    const int n = s.size();
    if (n == 0) {
        if (m_visible) { m_visible = false; emit sampleChanged(); }
    } else {
        const int i = sampleIndexFor(tMs);
        const CursorSample &a = s.at(i);
        double x = a.x, y = a.y;
        if (i + 1 < n && tMs > a.tMs) {
            const CursorSample &b = s.at(i + 1);
            const double sp = double(b.tMs - a.tMs);
            const double f = sp > 0.0 ? double(tMs - a.tMs) / sp : 0.0;
            x = a.x + (b.x - a.x) * f;
            y = a.y + (b.y - a.y) * f;
        }
        m_nx = x * m_invW;
        m_ny = y * m_invH;
        m_visible = a.visible;              // state from the earlier neighbour
        applyShape(a.shapeId);
        emit sampleChanged();
    }

    // --- active ripples ---
    // Reset the forward scan hint on a backward seek.
    if (m_prevT < 0 || tMs < m_prevT)
        m_downCursor = 0;
    m_prevT = tMs;
    const qint64 windowStart = tMs - kRippleMs;
    m_activeScratch.clear();
    // Advance the hint past clicks that have fully expired.
    while (m_downCursor < m_downs.size() && m_downs.at(m_downCursor).tMs < windowStart)
        ++m_downCursor;
    for (int j = m_downCursor; j < m_downs.size(); ++j) {
        const ClickEvent &e = m_downs.at(j);
        if (e.tMs > tMs)
            break;                          // future clicks (sorted) — done
        if (e.tMs < windowStart)
            continue;                       // just crossed the hint edge
        m_activeScratch.append({e.x * m_invW, e.y * m_invH, e.tMs, j});
    }
    m_ripples->setActive(m_activeScratch);

    // Time since the most recent click at/or-before t (the 'press' feedback). A
    // pure function of time, so preview and export dip identically. Binary search
    // the sorted down list for the last event with tMs <= t.
    m_msSinceClick = -1.0;
    if (!m_downs.isEmpty()) {
        int lo = 0, hi = m_downs.size() - 1, idx = -1;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (m_downs.at(mid).tMs <= tMs) { idx = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        if (idx >= 0)
            m_msSinceClick = double(tMs - m_downs.at(idx).tMs);
    }
}

QAbstractListModel *CursorPlayback::ripples() const
{
    return m_ripples;
}
