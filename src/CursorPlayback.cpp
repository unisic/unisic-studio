#include "CursorPlayback.h"

#include "engine/CursorSmoother.h"

#include <QImage>

#include <algorithm>
#include <cmath>
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
    resetSpring();
    buildMotionRuns();

    m_downs.clear();
    m_downPositions.clear();
    for (const ClickEvent &e : clicks.events())
        if (e.state == ClickEvent::Down) {
            m_downs.append(e);
            m_downPositions.append(m_cursor.isEmpty()
                                       ? QPointF(e.x * m_invW, e.y * m_invH)
                                       : renderedPositionAt(e.tMs));
        }
    // Click-position precomputation advanced the cache; playback starts from a
    // clean canonical state so query order cannot influence later results.
    resetSpring();

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

void CursorPlayback::resetSpring()
{
    m_springInitial = {};
    int hint = 0;
    m_springInitial.position = targetAt(0, &hint);
    m_springInitial.sampleHint = hint;
    m_springForward = m_springInitial;
    m_springCheckpoints = {m_springInitial};
}

QPointF CursorPlayback::targetAt(qint64 timeUs, int *sampleHint) const
{
    const QList<CursorSample> &samples = m_cursor.samples();
    if (samples.isEmpty())
        return QPointF(0.5, 0.5);

    int i = std::clamp(*sampleHint, 0, int(samples.size()) - 1);
    while (i + 1 < samples.size() && samples.at(i + 1).tMs * 1000 <= timeUs)
        ++i;
    while (i > 0 && samples.at(i).tMs * 1000 > timeUs)
        --i;
    *sampleHint = i;

    const CursorSample &a = samples.at(i);
    double x = a.x;
    double y = a.y;
    if (i + 1 < samples.size() && timeUs > a.tMs * 1000) {
        const CursorSample &b = samples.at(i + 1);
        const qint64 spanUs = (b.tMs - a.tMs) * 1000;
        const double f = spanUs > 0 ? double(timeUs - a.tMs * 1000) / spanUs : 0.0;
        x += (b.x - a.x) * std::clamp(f, 0.0, 1.0);
        y += (b.y - a.y) * std::clamp(f, 0.0, 1.0);
    }
    return QPointF(x * m_invW, y * m_invH);
}

void CursorPlayback::advanceSpring(SpringState *state, qint64 destinationUs) const
{
    // Softer than SpringCameraEvaluator's center spring (95-255) so the pointer
    // glides with visible lag while the camera stays authoritative.
    constexpr double stiffness = 82.0;
    // Slightly underdamped: one restrained physical overshoot, never a wobble.
    constexpr double dampingRatio = 0.78;
    // Caps spring-driven catch-up speed (normalized frames/s) after seeks/jumps.
    constexpr double maxVelocity = 3.0;
    // Hard bound on how far the rendered pointer may trail its target (~6% of
    // the frame, ~115 px at 1920) — a fast flick outruns the soft spring, and a
    // cursor half a screen behind reads as broken rather than floaty.
    constexpr double maxLagDistance = 0.06;
    const double omega = std::sqrt(stiffness);
    const double damping = 2.0 * dampingRatio * omega;

    while (state->timeUs < destinationUs) {
        const qint64 nextUs = std::min(destinationUs,
                                       state->timeUs + qint64(kSpringStepMs) * 1000);
        const qint64 midpointUs = state->timeUs + (nextUs - state->timeUs) / 2;
        const QPointF target = targetAt(midpointUs, &state->sampleHint);
        const double dt = double(nextUs - state->timeUs) / 1.0e6;
        QPointF acceleration(stiffness * (target.x() - state->position.x())
                                 - damping * state->velocity.x(),
                             stiffness * (target.y() - state->position.y())
                                 - damping * state->velocity.y());
        state->velocity += acceleration * dt;
        const double speed = std::hypot(state->velocity.x(), state->velocity.y());
        if (speed > maxVelocity) {
            const double scale = maxVelocity / speed;
            state->velocity *= scale;
        }
        state->position += state->velocity * dt;

        // Beyond maxLagDistance the pointer is towed elastically at the
        // target's own pace; the spring resumes and finishes the catch-up as
        // soon as the target slows. Position-only — velocity stays
        // spring-integrated, which keeps the release overshoot tiny.
        const QPointF lagVector = target - state->position;
        const double lag = std::hypot(lagVector.x(), lagVector.y());
        if (lag > maxLagDistance)
            state->position = target - lagVector * (maxLagDistance / lag);

        const double oldX = state->position.x();
        const double oldY = state->position.y();
        state->position.setX(std::clamp(oldX, 0.0, 1.0));
        state->position.setY(std::clamp(oldY, 0.0, 1.0));
        if ((oldX < 0.0 && state->velocity.x() < 0.0)
            || (oldX > 1.0 && state->velocity.x() > 0.0))
            state->velocity.setX(0.0);
        if ((oldY < 0.0 && state->velocity.y() < 0.0)
            || (oldY > 1.0 && state->velocity.y() > 0.0))
            state->velocity.setY(0.0);
        state->timeUs = nextUs;
    }
}

CursorPlayback::SpringState CursorPlayback::springStateFor(qint64 timeUs)
{
    const qint64 canonicalUs = timeUs - timeUs % (qint64(kSpringStepMs) * 1000);
    if (canonicalUs >= m_springForward.timeUs) {
        while (m_springForward.timeUs < canonicalUs) {
            advanceSpring(&m_springForward,
                          std::min(canonicalUs, m_springForward.timeUs
                                                   + qint64(kSpringStepMs) * 1000));
            if (m_springForward.timeUs > 0
                && m_springForward.timeUs % (qint64(kCheckpointMs) * 1000) == 0
                && m_springCheckpoints.last().timeUs != m_springForward.timeUs)
                m_springCheckpoints.append(m_springForward);
        }
        return m_springForward;
    }

    const auto it = std::upper_bound(
        m_springCheckpoints.cbegin(), m_springCheckpoints.cend(), canonicalUs,
        [](qint64 time, const SpringState &checkpoint) { return time < checkpoint.timeUs; });
    SpringState state = it == m_springCheckpoints.cbegin() ? m_springInitial : *(it - 1);
    advanceSpring(&state, canonicalUs);
    return state;
}

QPointF CursorPlayback::renderedPositionAt(qint64 timeMs)
{
    const qint64 timeUs = std::max<qint64>(0, timeMs) * 1000;
    SpringState state = springStateFor(timeUs);
    if (state.timeUs < timeUs)
        advanceSpring(&state, timeUs);
    return state.position;
}

void CursorPlayback::buildMotionRuns()
{
    m_motionRuns.clear();
    const QList<CursorSample> &samples = m_cursor.samples();
    if (samples.size() < 2)
        return;

    // Significant events require ~3 source-normalized pixels of accumulated
    // travel. Nearby events merge, rejecting one-euro residual rest jitter while
    // retaining slow deliberate movement.
    constexpr double movementDistance = 0.0015;
    constexpr qint64 mergeGapMs = 220;
    QPointF anchor(samples.first().x * m_invW, samples.first().y * m_invH);
    for (int i = 1; i < samples.size(); ++i) {
        const QPointF point(samples.at(i).x * m_invW, samples.at(i).y * m_invH);
        if (std::hypot(point.x() - anchor.x(), point.y() - anchor.y()) < movementDistance)
            continue;
        const qint64 eventMs = samples.at(i).tMs;
        const qint64 startMs = std::max<qint64>(samples.first().tMs, eventMs - 80);
        const qint64 endMs = eventMs + 120;
        if (!m_motionRuns.isEmpty() && startMs - m_motionRuns.last().endMs <= mergeGapMs)
            m_motionRuns.last().endMs = endMs;
        else
            m_motionRuns.append({startMs, endMs});
        anchor = point;
    }
}

qreal CursorPlayback::opacityAt(qint64 timeMs, bool recordedVisible) const
{
    if (!recordedVisible || m_cursor.isEmpty())
        return 0.0;
    const qint64 firstMs = m_cursor.samples().first().tMs;
    const MotionRun *current = nullptr;
    const MotionRun *previous = nullptr;
    for (const MotionRun &run : m_motionRuns) {
        if (run.startMs > timeMs)
            break;
        previous = current;
        current = &run;
    }

    auto smooth = [](double value) {
        const double x = std::clamp(value, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    };
    if (current && timeMs <= current->endMs) {
        const qint64 priorEnd = previous ? previous->endMs : firstMs;
        const qint64 idleBeforeRun = current->startMs - priorEnd;
        // Level the idle fade-out had reached when this run began. Waking
        // mid-fade resumes from that level — the old binary check snapped a
        // partially faded pointer straight back to 1.0.
        const double base = idleBeforeRun <= kIdleDelayMs
                                ? 1.0
                                : 1.0 - smooth(double(idleBeforeRun - kIdleDelayMs) / kIdleFadeMs);
        if (base >= 1.0)
            return 1.0;
        // Written as 1 - remainder so a finished wake fade yields exactly 1.0.
        return 1.0 - (1.0 - base)
                         * (1.0 - smooth(double(timeMs - current->startMs) / kWakeFadeMs));
    }

    const qint64 lastMotion = current ? current->endMs : firstMs;
    const qint64 idleMs = timeMs - lastMotion;
    if (idleMs <= kIdleDelayMs)
        return 1.0;
    return 1.0 - smooth(double(idleMs - kIdleDelayMs) / kIdleFadeMs);
}

qreal CursorPlayback::pressScaleAt(qreal msSinceClick)
{
    if (msSinceClick < 0.0 || msSinceClick >= kRippleMs)
        return 1.0;
    // Immediate tactile dip, then one restrained near-critical rebound.
    return 1.0 - 0.12 * std::exp(-msSinceClick / 105.0)
                     * std::cos(msSinceClick * 0.026);
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

    const qreal oldNx = m_nx;
    const qreal oldNy = m_ny;
    const bool oldVisible = m_visible;
    const qreal oldOpacity = m_opacity;
    const qreal oldClickAge = m_msSinceClick;
    const qreal oldPressScale = m_pressScale;

    // --- recorded state + spring-rendered position ---
    const QList<CursorSample> &s = m_cursor.samples();
    const int n = s.size();
    if (n == 0) {
        m_visible = false;
        m_opacity = 0.0;
    } else {
        const int i = sampleIndexFor(tMs);
        const CursorSample &a = s.at(i);
        const QPointF rendered = renderedPositionAt(tMs);
        m_nx = rendered.x();
        m_ny = rendered.y();
        m_opacity = opacityAt(tMs, a.visible);
        m_visible = a.visible && m_opacity > 0.001;
        applyShape(a.shapeId);
    }

    // --- active ripples ---
    // Reset the forward scan hint on a backward seek.
    if (m_prevT < 0 || tMs < m_prevT)
        m_downCursor = 0;
    m_prevT = tMs;
    const qint64 windowStart = tMs - kRippleMs;
    m_activeScratch.clear();
    // Advance the hint past clicks that have fully expired.
    while (m_downCursor < m_downs.size() && m_downs.at(m_downCursor).tMs <= windowStart)
        ++m_downCursor;
    for (int j = m_downCursor; j < m_downs.size(); ++j) {
        const ClickEvent &e = m_downs.at(j);
        if (e.tMs > tMs)
            break;                          // future clicks (sorted) — done
        if (e.tMs <= windowStart)
            continue;                       // just crossed the hint edge
        const QPointF position = m_downPositions.value(j, QPointF(e.x * m_invW, e.y * m_invH));
        m_activeScratch.append({position.x(), position.y(), e.tMs, j});
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
        if (idx >= 0) {
            const qint64 age = tMs - m_downs.at(idx).tMs;
            if (age < kRippleMs)
                m_msSinceClick = double(age);
        }
    }
    m_pressScale = pressScaleAt(m_msSinceClick);

    // Emit only after every time-derived field is coherent. This fixes the old
    // one-frame-stale msSinceClick notification and avoids idle NOTIFY storms.
    if (!qFuzzyCompare(oldNx, m_nx) || !qFuzzyCompare(oldNy, m_ny)
        || oldVisible != m_visible || !qFuzzyCompare(oldOpacity, m_opacity)
        || !qFuzzyCompare(oldClickAge, m_msSinceClick)
        || !qFuzzyCompare(oldPressScale, m_pressScale))
        emit sampleChanged();
}

QAbstractListModel *CursorPlayback::ripples() const
{
    return m_ripples;
}
