#include "KeyframeEngine.h"

#include "CursorSmoother.h"
#include "ClickTrack.h"
#include "CursorTrack.h"
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Params <-> JSON. Stored verbatim in ZoomTimeline::autoParams() so a reload
// regenerates the identical camera. Missing keys fall back to the default.
// ---------------------------------------------------------------------------
QJsonObject KeyframeEngine::Params::toJson() const
{
    return QJsonObject{
        {"clickClusterGapMs", clickClusterGapMs},
        {"clickClusterDistFrac", clickClusterDistFrac},
        {"leadInMs", leadInMs},
        {"tailMs", tailMs},
        {"zoomInMs", zoomInMs},
        {"zoomOutMs", zoomOutMs},
        {"minHoldMs", minHoldMs},
        {"zoomMin", zoomMin},
        {"zoomMax", zoomMax},
        {"marginFrac", marginFrac},
        {"deadZoneFrac", deadZoneFrac},
        {"dwellMinMs", dwellMinMs},
        {"dwellSpeedFracPerSec", dwellSpeedFracPerSec},
        {"zoomIntensity", zoomIntensity},
        {"motionSmoothness", motionSmoothness},
        {"fill", fill},
    };
}

KeyframeEngine::Params KeyframeEngine::Params::fromJson(const QJsonObject &o)
{
    Params p;
    const auto i = [&](const char *k, int dv) { return o.contains(k) ? o.value(k).toInt(dv) : dv; };
    const auto d = [&](const char *k, double dv) { return o.contains(k) ? o.value(k).toDouble(dv) : dv; };
    p.clickClusterGapMs = i("clickClusterGapMs", p.clickClusterGapMs);
    p.clickClusterDistFrac = d("clickClusterDistFrac", p.clickClusterDistFrac);
    p.leadInMs = i("leadInMs", p.leadInMs);
    p.tailMs = i("tailMs", p.tailMs);
    p.zoomInMs = i("zoomInMs", p.zoomInMs);
    p.zoomOutMs = i("zoomOutMs", p.zoomOutMs);
    p.minHoldMs = i("minHoldMs", p.minHoldMs);
    p.zoomMin = d("zoomMin", p.zoomMin);
    p.zoomMax = d("zoomMax", p.zoomMax);
    p.marginFrac = d("marginFrac", p.marginFrac);
    p.deadZoneFrac = d("deadZoneFrac", p.deadZoneFrac);
    p.dwellMinMs = i("dwellMinMs", p.dwellMinMs);
    p.dwellSpeedFracPerSec = d("dwellSpeedFracPerSec", p.dwellSpeedFracPerSec);
    p.zoomIntensity = std::clamp(d("zoomIntensity", p.zoomIntensity), 0.0, 1.0);
    p.motionSmoothness = std::clamp(d("motionSmoothness", p.motionSmoothness), 0.0, 1.0);
    p.fill = o.contains("fill") ? o.value("fill").toBool(p.fill) : p.fill;
    return p;
}

double KeyframeEngine::aspectRatio(const QString &aspect, double fallback)
{
    const int colon = aspect.indexOf(':');
    if (colon <= 0)
        return fallback;
    bool okW = false, okH = false;
    const double w = aspect.left(colon).toDouble(&okW);
    const double h = aspect.mid(colon + 1).toDouble(&okH);
    if (!okW || !okH || w <= 0.0 || h <= 0.0)
        return fallback;
    return w / h;
}

// ---------------------------------------------------------------------------
// SpringCameraEvaluator
// ---------------------------------------------------------------------------
namespace {

constexpr double kMinRectSize = 1.0e-5;
constexpr double kPositionSnap = 2.0e-6;
constexpr double kVelocitySnap = 2.0e-5;

double finiteOr(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

void clampVector(QPointF *v, double maxLength)
{
    const double length = std::hypot(v->x(), v->y());
    if (length <= maxLength || length <= 0.0)
        return;
    const double scale = maxLength / length;
    v->setX(v->x() * scale);
    v->setY(v->y() * scale);
}

} // namespace

SpringCameraEvaluator::SpringCameraEvaluator()
{
    setTimeline({}, parametersForSmoothness(m_parameters.smoothness));
}

SpringCameraEvaluator::SpringCameraEvaluator(const QVector<Keyframe> &keyframes,
                                             double smoothness)
{
    setTimeline(keyframes, smoothness);
}

SpringCameraEvaluator::Parameters
SpringCameraEvaluator::parametersForSmoothness(double smoothness)
{
    Parameters p;
    p.smoothness = std::clamp(finiteOr(smoothness, p.smoothness), 0.0, 1.0);
    // Higher smoothness means a softer spring. Values keep the 2% settling
    // window around 300-650 ms, fitting the generator's 650/900 ms lead times.
    p.centerStiffness = 255.0 + (95.0 - 255.0) * p.smoothness;
    p.zoomStiffness = 205.0 + (62.0 - 205.0) * p.smoothness;
    return p;
}

QRectF SpringCameraEvaluator::boundedRect(const QRectF &rect)
{
    double w = std::clamp(std::fabs(finiteOr(rect.width(), 1.0)), kMinRectSize, 1.0);
    double h = std::clamp(std::fabs(finiteOr(rect.height(), 1.0)), kMinRectSize, 1.0);
    double x = finiteOr(rect.x(), 0.0);
    double y = finiteOr(rect.y(), 0.0);
    x = std::clamp(x, 0.0, 1.0 - w);
    y = std::clamp(y, 0.0, 1.0 - h);
    return QRectF(x, y, w, h);
}

void SpringCameraEvaluator::setTimeline(const QVector<Keyframe> &keyframes,
                                        double smoothness)
{
    setTimeline(keyframes, parametersForSmoothness(smoothness));
}

void SpringCameraEvaluator::setTimeline(const QVector<Keyframe> &keyframes,
                                        const Parameters &parameters)
{
    m_parameters = parameters;
    m_parameters.smoothness = std::clamp(finiteOr(parameters.smoothness, 0.68), 0.0, 1.0);
    m_parameters.centerStiffness = std::max(1.0, finiteOr(parameters.centerStiffness, 150.0));
    m_parameters.zoomStiffness = std::max(1.0, finiteOr(parameters.zoomStiffness, 105.0));
    m_parameters.centerDampingRatio =
        std::clamp(finiteOr(parameters.centerDampingRatio, 1.0), 0.05, 3.0);
    m_parameters.zoomDampingRatio =
        std::clamp(finiteOr(parameters.zoomDampingRatio, 0.86), 0.05, 3.0);
    m_parameters.maxCenterVelocity =
        std::max(0.01, finiteOr(parameters.maxCenterVelocity, 1.35));
    m_parameters.maxZoomVelocity =
        std::max(0.01, finiteOr(parameters.maxZoomVelocity, 2.0));
    m_parameters.stepMs = std::clamp(parameters.stepMs, 1, 16);
    m_parameters.checkpointMs = std::max(m_parameters.stepMs, parameters.checkpointMs);
    m_parameters.checkpointMs -= m_parameters.checkpointMs % m_parameters.stepMs;

    QVector<Keyframe> sorted = keyframes;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Keyframe &a, const Keyframe &b) { return a.tMs < b.tMs; });

    m_events.clear();
    m_events.reserve(sorted.size());
    QRectF initialRect(0, 0, 1, 1);
    for (const Keyframe &keyframe : std::as_const(sorted)) {
        if (keyframe.tMs <= 0)
            initialRect = boundedRect(keyframe.rect);
    }
    for (int i = 0; i < sorted.size(); ++i) {
        const Keyframe &keyframe = sorted.at(i);
        const qint64 previousTime = i > 0 ? sorted.at(i - 1).tMs : 0;
        const qint64 lead = std::max(0, keyframe.easeInMs);
        const qint64 activationMs =
            std::max<qint64>(0, std::max(previousTime, keyframe.tMs - lead));
        m_events.append({activationMs * 1000, boundedRect(keyframe.rect)});
    }
    std::stable_sort(m_events.begin(), m_events.end(),
                     [](const TargetEvent &a, const TargetEvent &b) { return a.atUs < b.atUs; });

    m_initial = {};
    m_initial.center = initialRect.center();
    m_initial.logSize = QPointF(std::log(initialRect.width()), std::log(initialRect.height()));
    m_initial.target = initialRect;
    applyEvents(&m_initial); // target switches at t=0; state itself stays at its authored opener
    m_forward = m_initial;
    m_checkpoints = {m_initial};
    m_hasTimeline = !sorted.isEmpty();
}

void SpringCameraEvaluator::applyEvents(State *state) const
{
    while (state->nextEvent < m_events.size()
           && m_events.at(state->nextEvent).atUs <= state->timeUs) {
        state->target = m_events.at(state->nextEvent).rect;
        ++state->nextEvent;
    }
}

void SpringCameraEvaluator::integrate(State *state, double dt) const
{
    const QPointF targetCenter = state->target.center();
    const QPointF targetLog(std::log(state->target.width()), std::log(state->target.height()));

    const double centerOmega = std::sqrt(m_parameters.centerStiffness);
    const double centerDamping =
        2.0 * m_parameters.centerDampingRatio * centerOmega;
    QPointF centerAcceleration(
        m_parameters.centerStiffness * (targetCenter.x() - state->center.x())
            - centerDamping * state->centerVelocity.x(),
        m_parameters.centerStiffness * (targetCenter.y() - state->center.y())
            - centerDamping * state->centerVelocity.y());
    state->centerVelocity += centerAcceleration * dt;
    clampVector(&state->centerVelocity, m_parameters.maxCenterVelocity);
    state->center += state->centerVelocity * dt;

    const double zoomOmega = std::sqrt(m_parameters.zoomStiffness);
    // Zoom-in keeps the restrained near-critical overshoot. Returning to a base
    // edge uses critical damping so it decelerates into frame bounds instead of
    // colliding with the clamp at full size.
    const bool returningToFrameEdge = state->target.width() >= 1.0 - 1.0e-9
                                      || state->target.height() >= 1.0 - 1.0e-9;
    const double zoomDampingRatio = returningToFrameEdge
                                        ? std::max(1.0, m_parameters.zoomDampingRatio)
                                        : m_parameters.zoomDampingRatio;
    const double zoomDamping = 2.0 * zoomDampingRatio * zoomOmega;
    QPointF zoomAcceleration(
        m_parameters.zoomStiffness * (targetLog.x() - state->logSize.x())
            - zoomDamping * state->logSizeVelocity.x(),
        m_parameters.zoomStiffness * (targetLog.y() - state->logSize.y())
            - zoomDamping * state->logSizeVelocity.y());
    state->logSizeVelocity += zoomAcceleration * dt;
    clampVector(&state->logSizeVelocity, m_parameters.maxZoomVelocity);
    state->logSize += state->logSizeVelocity * dt;

    if (std::hypot(targetCenter.x() - state->center.x(),
                   targetCenter.y() - state->center.y()) < kPositionSnap
        && std::hypot(state->centerVelocity.x(), state->centerVelocity.y()) < kVelocitySnap) {
        state->center = targetCenter;
        state->centerVelocity = {};
    }
    if (std::hypot(targetLog.x() - state->logSize.x(),
                   targetLog.y() - state->logSize.y()) < kPositionSnap
        && std::hypot(state->logSizeVelocity.x(), state->logSizeVelocity.y()) < kVelocitySnap) {
        state->logSize = targetLog;
        state->logSizeVelocity = {};
    }

    // Reconstruct once per substep to enforce positive dimensions and frame
    // bounds. If a clamp blocks velocity, remove only its outward component.
    (void)rectForState(state);
}

QRectF SpringCameraEvaluator::rectForState(State *state)
{
    double w = std::clamp(std::exp(state->logSize.x()), kMinRectSize, 1.0);
    double h = std::clamp(std::exp(state->logSize.y()), kMinRectSize, 1.0);
    if (w >= 1.0 && state->logSizeVelocity.x() > 0.0) {
        state->logSize.setX(0.0);
        state->logSizeVelocity.setX(0.0);
    }
    if (h >= 1.0 && state->logSizeVelocity.y() > 0.0) {
        state->logSize.setY(0.0);
        state->logSizeVelocity.setY(0.0);
    }

    const double minX = w / 2.0;
    const double maxX = 1.0 - minX;
    const double minY = h / 2.0;
    const double maxY = 1.0 - minY;
    const double oldX = state->center.x();
    const double oldY = state->center.y();
    state->center.setX(std::clamp(oldX, minX, maxX));
    state->center.setY(std::clamp(oldY, minY, maxY));
    if ((oldX < minX && state->centerVelocity.x() < 0.0)
        || (oldX > maxX && state->centerVelocity.x() > 0.0))
        state->centerVelocity.setX(0.0);
    if ((oldY < minY && state->centerVelocity.y() < 0.0)
        || (oldY > maxY && state->centerVelocity.y() > 0.0))
        state->centerVelocity.setY(0.0);

    return QRectF(state->center.x() - w / 2.0, state->center.y() - h / 2.0, w, h);
}

void SpringCameraEvaluator::advance(State *state, qint64 destinationUs) const
{
    while (state->timeUs < destinationUs) {
        qint64 segmentEnd = destinationUs;
        if (state->nextEvent < m_events.size())
            segmentEnd = std::min(segmentEnd, m_events.at(state->nextEvent).atUs);
        if (segmentEnd > state->timeUs) {
            integrate(state, double(segmentEnd - state->timeUs) / 1.0e6);
            state->timeUs = segmentEnd;
        }
        applyEvents(state);
    }
}

SpringCameraEvaluator::State
SpringCameraEvaluator::stateForCanonicalTime(qint64 canonicalUs)
{
    const qint64 stepUs = qint64(m_parameters.stepMs) * 1000;
    const qint64 checkpointUs = qint64(m_parameters.checkpointMs) * 1000;
    if (canonicalUs >= m_forward.timeUs) {
        while (m_forward.timeUs < canonicalUs) {
            advance(&m_forward, std::min(canonicalUs, m_forward.timeUs + stepUs));
            if (m_forward.timeUs > 0 && m_forward.timeUs % checkpointUs == 0
                && (m_checkpoints.isEmpty()
                    || m_checkpoints.last().timeUs != m_forward.timeUs))
                m_checkpoints.append(m_forward);
        }
        return m_forward;
    }

    const auto it = std::upper_bound(
        m_checkpoints.cbegin(), m_checkpoints.cend(), canonicalUs,
        [](qint64 time, const State &checkpoint) { return time < checkpoint.timeUs; });
    State state = it == m_checkpoints.cbegin() ? m_initial : *(it - 1);
    while (state.timeUs < canonicalUs)
        advance(&state, std::min(canonicalUs, state.timeUs + stepUs));
    return state;
}

QRectF SpringCameraEvaluator::evaluate(qint64 tMs)
{
    if (!m_hasTimeline)
        return QRectF(0, 0, 1, 1);
    const qint64 targetUs = std::max<qint64>(0, tMs) * 1000;
    const qint64 stepUs = qint64(m_parameters.stepMs) * 1000;
    const qint64 canonicalUs = targetUs - targetUs % stepUs;
    State state = stateForCanonicalTime(canonicalUs);
    if (state.timeUs < targetUs)
        advance(&state, targetUs);
    return rectForState(&state);
}

QRectF SpringCameraEvaluator::targetAt(qint64 tMs) const
{
    if (!m_hasTimeline)
        return QRectF(0, 0, 1, 1);
    QRectF target = m_initial.target;
    const qint64 targetUs = std::max<qint64>(0, tMs) * 1000;
    for (const TargetEvent &event : m_events) {
        if (event.atUs > targetUs)
            break;
        target = event.rect;
    }
    return target;
}

// ---------------------------------------------------------------------------
// generate(): the auto-camera. Pipeline: smooth -> segment -> span shaping
// (merge + minHold + idle cut) -> pinned-trim -> per-span keyframe emission ->
// assemble + strict-sort. All geometry lives in normalised [0,1] frame coords;
// aspect handling is done in pixel space so the emitted rects carry the OUTPUT
// aspect exactly.
// ---------------------------------------------------------------------------
namespace {

using Keyframe = KeyframeEngine::Keyframe;
using Params = KeyframeEngine::Params;

constexpr int kCameraTickMs = 100;          // dead-zone re-evaluation cadence (10 Hz)
// Drive-by suppression: a LONE click is only zoom-worthy if the cursor actually
// stays around afterwards. If it has moved more than kDriveByDistFrac of the
// frame within kPostClickDwellMs of the click, the user was passing through
// (dismissing a popup mid-travel, etc.) and a zoom there reads as random.
// Multi-click clusters are always kept — repetition IS the evidence.
constexpr int kPostClickDwellMs = 650;
constexpr double kDriveByDistFrac = 0.16;
// A single click framed at the tightest allowed zoom looks jumpy; soften the
// depth toward 1.0 so lone interactions get a comfortable, wider framing while
// dense clusters keep the full fit-the-bbox zoom.
constexpr double kSingleClickZoomSoften = 0.72;
constexpr int kPanEaseMs = 450;             // spring-target lead for a deliberate pan

// Smoothed cursor in normalised [0,1] frame coords + per-sample speed in
// width-fraction / second. Lookups clamp outside the recorded range.
struct Cam {
    QVector<qint64> t;
    QVector<double> x, y;
    QVector<double> spd;   // spd[i] == motion over interval (i-1, i]

    bool empty() const { return t.isEmpty(); }

    int indexFor(qint64 tt) const
    {
        int lo = 0, hi = int(t.size()) - 1, r = 0;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            if (t.at(mid) <= tt) { r = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        return r;
    }

    QPointF sample(qint64 tt) const
    {
        if (t.isEmpty()) return QPointF(0.5, 0.5);
        if (tt <= t.first()) return QPointF(x.first(), y.first());
        if (tt >= t.last())  return QPointF(x.last(), y.last());
        const int i = indexFor(tt);
        if (i + 1 >= t.size()) return QPointF(x.at(i), y.at(i));
        const double sp = double(t.at(i + 1) - t.at(i));
        const double f = sp > 0 ? double(tt - t.at(i)) / sp : 0.0;
        return QPointF(x.at(i) + (x.at(i + 1) - x.at(i)) * f,
                       y.at(i) + (y.at(i + 1) - y.at(i)) * f);
    }

    double speed(qint64 tt) const
    {
        if (spd.isEmpty()) return 0.0;
        const int i = indexFor(tt);
        return spd.at(std::min(i + 1, int(spd.size()) - 1));
    }
};

struct Segment {
    qint64 tFirst = 0;
    qint64 tLast = 0;
    QRectF bbox;           // normalised bbox of the cluster / dwell points
    bool dwell = false;
    QVector<qint64> clickTimes;
    QPointF focus = QPointF(0.5, 0.5);
    int evidenceCount = 0;
};

struct Span {
    qint64 start = 0;      // begin zoom-in
    qint64 end = 0;        // begin zoom-out
    QRectF bbox;
    bool dwell = false;
    QVector<qint64> clickTimes;
    QPointF focus = QPointF(0.5, 0.5);
    int evidenceCount = 0;
};

Cam buildCam(const CursorTrack &cursor, double W, double H)
{
    CursorSmoother sm;
    const QVector<CursorSample> s = sm.smooth(cursor);
    Cam c;
    const int n = s.size();
    c.t.reserve(n); c.x.reserve(n); c.y.reserve(n); c.spd.reserve(n);
    for (int i = 0; i < n; ++i) {
        c.t.append(s.at(i).tMs);
        c.x.append(s.at(i).x / W);
        c.y.append(s.at(i).y / H);
        if (i == 0) {
            c.spd.append(0.0);
        } else {
            const double dtS = (s.at(i).tMs - s.at(i - 1).tMs) / 1000.0;
            const double dPx = std::hypot(s.at(i).x - s.at(i - 1).x, s.at(i).y - s.at(i - 1).y);
            c.spd.append(dtS > 0 ? (dPx / W) / dtS : 0.0);
        }
    }
    return c;
}

// Cluster Down events: a click joins the open cluster when it is within
// clickClusterGapMs of the previous click AND within clickClusterDistFrac of
// the frame width from it. Otherwise it opens a new cluster.
QVector<Segment> clusterClicks(const ClickTrack &clicks, const Params &p, double W, double H,
                               qint64 durationMs)
{
    QVector<Segment> segs;
    Segment cur;
    bool open = false;
    qint64 prevT = 0;
    double prevPx = 0, prevPy = 0;

    auto close = [&]() { if (open) { segs.append(cur); open = false; } };

    for (const ClickEvent &e : clicks.events()) {
        if (e.state != ClickEvent::Down)
            continue;
        if (e.tMs < 0 || e.tMs >= durationMs || !std::isfinite(e.x) || !std::isfinite(e.y)
            || e.x < 0.0 || e.y < 0.0 || e.x > W || e.y > H)
            continue; // global libinput click outside the captured stream
        const QPointF point(e.x / W, e.y / H);
        const bool joins = open
            && (e.tMs - prevT) < p.clickClusterGapMs
            && std::hypot(e.x - prevPx, e.y - prevPy) / W < p.clickClusterDistFrac;
        if (!joins) {
            close();
            cur = Segment{};
            cur.tFirst = e.tMs;
            cur.bbox = QRectF(point, point);
            cur.focus = point;
            cur.evidenceCount = 0;
            open = true;
        }
        cur.focus = (cur.focus * cur.evidenceCount + point) / (cur.evidenceCount + 1);
        ++cur.evidenceCount;
        cur.tLast = e.tMs;
        cur.clickTimes.append(e.tMs);
        QRectF b = cur.bbox;
        b.setLeft(std::min(b.left(), e.x / W));
        b.setRight(std::max(b.right(), e.x / W));
        b.setTop(std::min(b.top(), e.y / H));
        b.setBottom(std::max(b.bottom(), e.y / H));
        cur.bbox = b;
        prevT = e.tMs;
        prevPx = e.x;
        prevPy = e.y;
    }
    close();
    return segs;
}

// Dwell fallback (no clicks): maximal runs where the smoothed cursor stays
// below the dwell speed threshold for >= dwellMinMs become synthetic segments
// centred on the run's mean position.
QVector<Segment> dwellSegments(const Cam &cam, const Params &p, qint64 durationMs)
{
    QVector<Segment> segs;
    if (cam.t.size() < 2)
        return segs;
    const double thresh = p.dwellSpeedFracPerSec;
    int i = 1;
    while (i < cam.t.size()) {
        if (cam.t.at(i) > durationMs)
            break;
        if (cam.spd.at(i) >= thresh) { ++i; continue; }
        const int runStart = i - 1;         // include the sample the slow run departs from
        double sx = cam.x.at(runStart), sy = cam.y.at(runStart);
        int cnt = 1;
        while (i < cam.t.size() && cam.t.at(i) <= durationMs && cam.spd.at(i) < thresh) {
            sx += cam.x.at(i); sy += cam.y.at(i); ++cnt;
            ++i;
        }
        const qint64 t0 = cam.t.at(runStart);
        if (t0 >= durationMs)
            continue;
        const qint64 t1 = std::min(cam.t.at(std::min(i, int(cam.t.size()) - 1)), durationMs);
        const int effectiveDwellMs =
            int(std::lround(p.dwellMinMs * (1.75 - 0.75 * p.zoomIntensity)));
        if (t1 - t0 < effectiveDwellMs)
            continue;
        // A static pointer for the whole clip is not interaction evidence. Keep
        // only a dwell that was approached or deliberately departed.
        bool activityEvidence = false;
        const int before = std::max(1, runStart - 32);
        const int after = std::min(int(cam.spd.size()) - 1, i + 32);
        for (int j = before; j <= after; ++j) {
            if (cam.t.at(j) <= durationMs && (j < runStart || j >= i)
                && cam.spd.at(j) >= p.dwellSpeedFracPerSec * 1.5) {
                activityEvidence = true;
                break;
            }
        }
        if (!activityEvidence)
            continue;
        Segment s;
        s.dwell = true;
        s.tFirst = t0;
        s.tLast = std::min(t1, durationMs);
        const double mx = sx / cnt, my = sy / cnt;
        s.bbox = QRectF(mx, my, 0, 0);      // a point; zoom uses the dwell level
        s.focus = QPointF(mx, my);
        s.evidenceCount = 1;
        segs.append(s);
    }
    return segs;
}

// Shape segments into spans: extend by lead-in / tail, merge neighbours whose
// gap is small enough that a zoom-out+zoom-in would be churn, and enforce the
// minimum hold. Segments arrive sorted by time.
QVector<Span> shapeSpans(const QVector<Segment> &segs, const Params &p, qint64 durationMs)
{
    QVector<Span> spans;
    // Lower intensity means fewer camera events: retain one framing through a
    // longer idle gap instead of pumping out and immediately back in.
    const qint64 mergeGap = qint64(p.zoomOutMs) + p.zoomInMs
                            + qint64(std::lround((1.0 - p.zoomIntensity) * 1800.0));
    for (const Segment &sg : segs) {
        Span s;
        s.start = std::max<qint64>(0, sg.tFirst - p.leadInMs);
        s.end = sg.tLast + p.tailMs;
        s.bbox = sg.bbox;
        s.dwell = sg.dwell;
        s.clickTimes = sg.clickTimes;
        s.focus = sg.focus;
        s.evidenceCount = sg.evidenceCount;
        if (!spans.isEmpty() && s.start - spans.last().end < mergeGap) {
            // Retarget instead of out+in: keep zoomed, union the framing.
            Span &prev = spans.last();
            const int evidence = prev.evidenceCount + s.evidenceCount;
            if (evidence > 0)
                prev.focus = (prev.focus * prev.evidenceCount + s.focus * s.evidenceCount)
                             / evidence;
            prev.evidenceCount = evidence;
            prev.end = std::max(prev.end, s.end);
            // Not united(): Qt treats a 0-size rect (single-click bbox, dwell
            // point) as null and drops it from the union, which would silently
            // discard that click's position from the merged framing.
            QRectF u = prev.bbox;
            u.setLeft(std::min(u.left(), s.bbox.left()));
            u.setTop(std::min(u.top(), s.bbox.top()));
            u.setRight(std::max(u.right(), s.bbox.right()));
            u.setBottom(std::max(u.bottom(), s.bbox.bottom()));
            prev.bbox = u;
            prev.dwell = prev.dwell && s.dwell;
            prev.clickTimes += s.clickTimes;
        } else {
            spans.append(s);
        }
    }
    // Enforce minHold and clamp the trailing zoom-out into the video.
    for (Span &s : spans) {
        const qint64 minEnd = s.start + p.zoomInMs + p.minHoldMs;
        if (s.end < minEnd)
            s.end = minEnd;
        // The zoom-out animation runs to s.end + zoomOutMs; pull the span in
        // whenever THAT would overrun the clip, or the export ends frozen
        // mid-zoom-out for any interaction near the clip end.
        if (s.end + p.zoomOutMs > durationMs)
            s.end = std::max(s.start + p.zoomInMs, durationMs - p.zoomOutMs);
    }
    return spans;
}

// Trim a span away from pinned-keyframe windows so auto keyframes never fight a
// user keyframe. Simplest correct rule: if a window sits at an edge, pull that
// edge past it; if a window sits in the interior, keep only the larger side; if
// the window swallows the span or trimming leaves it too short to be a real
// zoom, drop the span (return false).
bool trimSpanAgainstPinned(Span &s, const QVector<QPair<qint64, qint64>> &windows, const Params &p)
{
    // A span's [start,end] covers the zoom-in ramp plus the hold; the zoom-out
    // extends PAST end, so the minimum meaningful length is zoomIn + minHold.
    const qint64 minLen = qint64(p.zoomInMs) + p.minHoldMs;
    for (const auto &w : windows) {
        const qint64 ws = w.first, we = w.second;
        if (we <= s.start || ws >= s.end)
            continue;                        // no overlap
        if (ws <= s.start && we >= s.end)
            return false;                    // fully covered
        if (ws <= s.start) {                 // covers the start
            s.start = we;
        } else if (we >= s.end) {            // covers the end
            s.end = ws;
        } else {                             // interior: keep the larger side
            if (ws - s.start >= s.end - we)
                s.end = ws;
            else
                s.start = we;
        }
    }
    return (s.end - s.start) >= minLen;
}

// Aspect + zoom geometry, all captured once per generate().
struct Geometry {
    double W, H, outAspect;
    double bw0, bh0;        // z==1 base rect (normalised) fitting outAspect in frame

    // z==1: largest rect of outAspect that fits the frame (letterbox fit).
    void init(double w, double h, double aspect)
    {
        W = w; H = h; outAspect = aspect;
        const double srcAspect = W / H;
        if (outAspect >= srcAspect) { bw0 = 1.0; bh0 = srcAspect / outAspect; }
        else                        { bh0 = 1.0; bw0 = outAspect / srcAspect; }
    }

    QRectF full() const { return QRectF(0, 0, 1, 1); }

    QRectF rectAt(QPointF center, double z) const
    {
        const double w = bw0 / z, h = bh0 / z;
        double cx = std::clamp(center.x(), w / 2.0, 1.0 - w / 2.0);
        double cy = std::clamp(center.y(), h / 2.0, 1.0 - h / 2.0);
        return QRectF(cx - w / 2.0, cy - h / 2.0, w, h);
    }

    // Tightest zoom (clamped) that still contains a padded bbox of the cluster.
    double zoomForBbox(const QRectF &bbox, const Params &p) const
    {
        const double srcAspect = W / H;
        const double padX = p.marginFrac;                 // width-fraction margin
        const double padY = p.marginFrac * srcAspect;     // same pixels, normalised by H
        const double bw = std::min(1.0, bbox.width() + 2 * padX);
        const double bh = std::min(1.0, bbox.height() + 2 * padY);
        const double zFit = std::min(bw0 / std::max(bw, 1e-6), bh0 / std::max(bh, 1e-6));
        return std::clamp(zFit, p.zoomMin, p.zoomMax);
    }

    QPointF centerForBbox(const QRectF &bbox, QPointF focus, double z, const Params &p) const
    {
        const double srcAspect = W / H;
        const double padX = p.marginFrac;
        const double padY = p.marginFrac * srcAspect;
        const double left = std::max(0.0, bbox.left() - padX);
        const double right = std::min(1.0, bbox.right() + padX);
        const double top = std::max(0.0, bbox.top() - padY);
        const double bottom = std::min(1.0, bbox.bottom() + padY);
        const double viewW = bw0 / z;
        const double viewH = bh0 / z;
        const double minCx = right - viewW / 2.0;
        const double maxCx = left + viewW / 2.0;
        const double minCy = bottom - viewH / 2.0;
        const double maxCy = top + viewH / 2.0;
        if (minCx <= maxCx)
            focus.setX(std::clamp(focus.x(), minCx, maxCx));
        else
            focus.setX(bbox.center().x());
        if (minCy <= maxCy)
            focus.setY(std::clamp(focus.y(), minCy, maxCy));
        else
            focus.setY(bbox.center().y());
        return rectAt(focus, z).center();
    }
};

double intensityZoom(double zoom, const Params &p)
{
    const double strength = std::clamp(p.zoomIntensity / ZoomTimeline::DefaultZoomIntensity,
                                       0.0, 1.25);
    return std::clamp(1.0 + (zoom - 1.0) * strength, 1.0, 2.55);
}

// ---- shared dead-zone pan emitter ------------------------------------------
// The follow-pan targets used to be distance-gated at ~0.045 of the frame, so the
// spring reached each sparse target, settled, then lurched to the next — a visible
// staircase ("poszarpane"). Two changes fix it without forking the spring: (1) the
// per-tick dead-zone centres are low-passed so the path has no step kinks, and (2)
// targets are emitted at a fine spatial floor OR a time floor shorter than the
// spring's settle time, so the camera is always mid-flight during a pan and glides.
// A pure hold never crosses kPanHoldEps, so a stationary cursor still emits zero
// pan keyframes (keeps the timeline — and the exact-count engine tests — clean).
constexpr double kPanEmitFrac     = 0.018;  // spatial floor between pan targets
constexpr int    kPanMaxGapMs     = 180;    // time floor while panning (< spring settle)
constexpr double kPanHoldEps      = 0.0022; // per-emit move below this == a hold: skip
constexpr int    kPanSmoothPasses = 3;      // [1,2,1]/4 low-pass passes over the centres

// Runs the dead-zone follower over [firstTick, spanEnd) from `seed` and appends a
// smooth sequence of Auto pan keyframes. `zoomOutLead` is the easeOut lead carried
// on each keyframe (so a later merge/zoom-out reads a consistent tail).
void emitDeadZonePan(QPointF seed, double z, const Cam &cam, const Geometry &g,
                     const Params &p, qint64 firstTick, qint64 spanEnd,
                     int zoomOutLead, qint64 durationMs, QVector<Keyframe> &out)
{
    QVector<qint64> times;
    QVector<QPointF> centers;
    QPointF center = seed;
    for (qint64 t = firstTick; t < spanEnd; t += kCameraTickMs) {
        const QRectF view = g.rectAt(center, z);
        const double hx = p.deadZoneFrac * view.width() / 2.0;
        const double hy = p.deadZoneFrac * view.height() / 2.0;
        const QPointF c = cam.sample(t);
        const double dx = c.x() - center.x();
        const double dy = c.y() - center.y();
        // Pan only enough to pull the cursor back onto the dead-zone edge.
        if (dx > hx)       center.setX(center.x() + (dx - hx));
        else if (dx < -hx) center.setX(center.x() + (dx + hx));
        if (dy > hy)       center.setY(center.y() + (dy - hy));
        else if (dy < -hy) center.setY(center.y() + (dy + hy));
        center = g.rectAt(center, z).center();
        times.append(t);
        centers.append(center);
    }
    if (centers.isEmpty())
        return;

    // Low-pass the collected centres (symmetric [1,2,1]/4). Endpoints are held, so
    // the overall path neither shifts nor shrinks — only the step kinks round off.
    QVector<QPointF> sc = centers;
    for (int pass = 0; pass < kPanSmoothPasses; ++pass) {
        QVector<QPointF> next = sc;
        for (int i = 1; i + 1 < sc.size(); ++i) {
            next[i].setX(0.25 * sc[i - 1].x() + 0.5 * sc[i].x() + 0.25 * sc[i + 1].x());
            next[i].setY(0.25 * sc[i - 1].y() + 0.5 * sc[i].y() + 0.25 * sc[i + 1].y());
        }
        sc = next;
    }

    // Emit along the smoothed curve: fine spatial floor for fast pans, time floor
    // for slow ones, and kPanHoldEps so a frozen centre emits nothing.
    QPointF lastPt = seed;
    qint64 lastT = firstTick - kPanMaxGapMs;
    for (int i = 0; i < sc.size(); ++i) {
        const double d = std::hypot(sc.at(i).x() - lastPt.x(), sc.at(i).y() - lastPt.y());
        const bool farEnough  = d > kPanEmitFrac;
        const bool timeEnough = d > kPanHoldEps && (times.at(i) - lastT) >= kPanMaxGapMs;
        if (!farEnough && !timeEnough)
            continue;
        Keyframe k;
        k.tMs = std::clamp<qint64>(times.at(i), 0, durationMs);
        k.rect = g.rectAt(sc.at(i), z);
        k.easeInMs = kPanEaseMs;
        k.easeOutMs = zoomOutLead;
        k.source = ZoomTimeline::Auto;
        k.locked = false;
        out.append(k);
        lastPt = sc.at(i);
        lastT = times.at(i);
    }
}

// Emit the keyframes for one span into `out`. The dead-zone camera runs at 10 Hz;
// emitDeadZonePan() smooths and lays down the follow-pan targets.
void emitSpan(const Span &s, const Cam &cam, const Geometry &g, const Params &p,
              qint64 durationMs, QVector<Keyframe> &out)
{
    // Zoom factor: dwell spans use a gentler fixed level (blend zoomMin 25% of
    // the way toward 1.0); click spans fit the cluster bbox.
    double z;
    if (s.dwell) {
        z = std::max(1.0, p.zoomMin + (1.0 - p.zoomMin) * 0.25);
    } else {
        z = g.zoomForBbox(s.bbox, p);
        if (s.evidenceCount <= 1)
            z = 1.0 + (z - 1.0) * kSingleClickZoomSoften;   // lone click: wider framing
    }
    z = intensityZoom(z, p);

    // The camera freezes at the cluster centre through the zoom-in ramp, then
    // the dead-zone follows the cursor. bbox.center() is exactly the click
    // point for a single-click cluster (a 0-size bbox), which frames it well.
    QPointF center = g.centerForBbox(s.bbox, s.focus, z, p);

    auto mk = [&](qint64 t, QRectF rect, int easeIn, int easeOut) {
        Keyframe k;
        k.tMs = std::clamp<qint64>(t, 0, durationMs);
        k.rect = rect;
        k.easeInMs = easeIn;
        k.easeOutMs = easeOut;
        k.source = ZoomTimeline::Auto;
        k.locked = false;
        out.append(k);
    };

    // Zoom-in: ease full -> zoomed over [start, start+zoomInMs], landing here.
    mk(s.start + p.zoomInMs, g.rectAt(center, z), p.zoomInMs, p.zoomOutMs);

    // Dead-zone follow-pan (smoothed, spring-friendly cadence).
    emitDeadZonePan(center, z, cam, g, p, s.start + p.zoomInMs + kCameraTickMs, s.end,
                    p.zoomOutMs, durationMs, out);

    // Zoom-out: ease zoomed -> full over [end, end+zoomOutMs].
    mk(s.end + p.zoomOutMs, g.full(), p.zoomOutMs, p.zoomOutMs);
}

// ---- crop-to-fill (aspect fill) base camera --------------------------------
// The base (non-zoomed) crop in fill mode is the largest centred OUTPUT-aspect
// rect of the source (g.rectAt(center, 1)). It slow-pans to follow the cursor
// with a LARGE dead zone, so the action stays framed without the twitchy motion
// of the tight in-span camera. Sampled at span boundaries too.
constexpr double kFillBaseDeadZone = 0.68;  // large -> base framing stays calm
constexpr int kBasePanEaseMs = 900;         // gentle spring-target lead for base pan

struct BaseTrack {
    QVector<qint64> t;
    QVector<QPointF> c;

    QPointF at(qint64 tt) const
    {
        if (t.isEmpty()) return QPointF(0.5, 0.5);
        if (tt <= t.first()) return c.first();
        if (tt >= t.last())  return c.last();
        int lo = 0, hi = int(t.size()) - 1, i = 0;
        while (lo <= hi) {
            const int m = (lo + hi) / 2;
            if (t.at(m) <= tt) { i = m; lo = m + 1; } else hi = m - 1;
        }
        if (i + 1 >= t.size()) return c.at(i);
        const double sp = double(t.at(i + 1) - t.at(i));
        const double f = sp > 0 ? double(tt - t.at(i)) / sp : 0.0;
        return QPointF(c.at(i).x() + (c.at(i + 1).x() - c.at(i).x()) * f,
                       c.at(i).y() + (c.at(i + 1).y() - c.at(i).y()) * f);
    }
};

BaseTrack buildBaseTrack(const Cam &cam, const Geometry &g, qint64 durationMs)
{
    BaseTrack bt;
    QPointF center = g.rectAt(cam.sample(0), 1.0).center();   // clamped fill-crop centre
    bt.t.append(0);
    bt.c.append(center);
    for (qint64 t = kCameraTickMs; t <= durationMs; t += kCameraTickMs) {
        const QRectF view = g.rectAt(center, 1.0);
        const double hx = kFillBaseDeadZone * view.width() / 2.0;
        const double hy = kFillBaseDeadZone * view.height() / 2.0;
        const QPointF c = cam.sample(t);
        const double dx = c.x() - center.x();
        const double dy = c.y() - center.y();
        if (dx > hx)       center.setX(center.x() + (dx - hx));
        else if (dx < -hx) center.setX(center.x() + (dx + hx));
        if (dy > hy)       center.setY(center.y() + (dy - hy));
        else if (dy < -hy) center.setY(center.y() + (dy + hy));
        center = g.rectAt(center, 1.0).center();   // re-clamp into the pan range
        bt.t.append(t);
        bt.c.append(center);
    }
    // Low-pass the base-crop centres (endpoints held) so the slow follow reads as a
    // smooth glide, not the dead-zone's step kinks — same fix as emitDeadZonePan.
    for (int pass = 0; pass < kPanSmoothPasses; ++pass) {
        QVector<QPointF> next = bt.c;
        for (int i = 1; i + 1 < bt.c.size(); ++i) {
            next[i].setX(0.25 * bt.c.at(i - 1).x() + 0.5 * bt.c.at(i).x() + 0.25 * bt.c.at(i + 1).x());
            next[i].setY(0.25 * bt.c.at(i - 1).y() + 0.5 * bt.c.at(i).y() + 0.25 * bt.c.at(i + 1).y());
        }
        bt.c = next;
    }
    return bt;
}

// A fill-mode span: identical to emitSpan but the zoom-in eases FROM the base
// crop and the zoom-out returns TO the (cursor-following) base crop rather than
// the whole frame — so there is never a letterboxed full-frame section.
void emitSpanFill(const Span &s, const Cam &cam, const Geometry &g, const Params &p,
                  const BaseTrack &base, qint64 durationMs, QVector<Keyframe> &out)
{
    double z;
    if (s.dwell) {
        z = std::max(1.0, p.zoomMin + (1.0 - p.zoomMin) * 0.25);
    } else {
        z = g.zoomForBbox(s.bbox, p);
        if (s.evidenceCount <= 1)
            z = 1.0 + (z - 1.0) * kSingleClickZoomSoften;   // lone click: wider framing
    }
    z = intensityZoom(z, p);

    QPointF center = g.centerForBbox(s.bbox, s.focus, z, p);
    auto mk = [&](qint64 t, QRectF rect, int easeIn, int easeOut) {
        Keyframe k;
        k.tMs = std::clamp<qint64>(t, 0, durationMs);
        k.rect = rect;
        k.easeInMs = easeIn;
        k.easeOutMs = easeOut;
        k.source = ZoomTimeline::Auto;
        k.locked = false;
        out.append(k);
    };

    // Base framing at the start of the ramp so the zoom-in eases from the crop
    // actually on screen (not a stale hold from an older base pan).
    mk(s.start, g.rectAt(base.at(s.start), 1.0), kBasePanEaseMs, p.zoomOutMs);
    // Zoom-in onto the cluster centroid.
    mk(s.start + p.zoomInMs, g.rectAt(center, z), p.zoomInMs, p.zoomOutMs);

    // Dead-zone follow-pan (smoothed, spring-friendly cadence).
    emitDeadZonePan(center, z, cam, g, p, s.start + p.zoomInMs + kCameraTickMs, s.end,
                    p.zoomOutMs, durationMs, out);

    // Zoom-out back to the base crop (following the cursor at that instant).
    mk(s.end + p.zoomOutMs, g.rectAt(base.at(s.end + p.zoomOutMs), 1.0), p.zoomOutMs, p.zoomOutMs);
}

} // namespace

QVector<Keyframe> KeyframeEngine::generate(const CursorTrack &cursor,
                                           const ClickTrack &clicks,
                                           QSize videoSize,
                                           qint64 durationMs,
                                           const QString &aspect,
                                           const Params &params,
                                           const QVector<Keyframe> &pinned,
                                           const QVector<QPair<qint64, qint64>> &typingBursts)
{
    durationMs = std::max<qint64>(0, durationMs);
    const double W = videoSize.width() > 0 ? videoSize.width() : 1.0;
    const double H = videoSize.height() > 0 ? videoSize.height() : 1.0;

    Geometry g;
    // Camera rects display stretched-to-fill the composition's video region:
    // OUTPUT-aspect in fill mode, but SOURCE-aspect in fit mode (the letterboxed
    // card keeps the source shape) — an output-aspect rect there would render
    // every zoom stretched by outputAspect/sourceAspect.
    g.init(W, H, params.fill ? aspectRatio(aspect, W / H) : W / H);

    const Cam cam = buildCam(cursor, W, H);

    // Segments: click clusters, or the dwell fallback when there are no clicks.
    QVector<Segment> segs;
    bool anyDown = false;
    for (const ClickEvent &e : clicks.events())
        if (e.state == ClickEvent::Down) { anyDown = true; break; }
    if (anyDown) {
        segs = clusterClicks(clicks, params, W, H, durationMs);
        // Drive-by suppression (see kPostClickDwellMs): drop lone clicks the
        // cursor immediately travelled away from. Runs before the dwell
        // fallback so an all-drive-by recording still gets dwell framing.
        if (!cam.empty()) {
            QVector<Segment> kept;
            kept.reserve(segs.size());
            for (const Segment &s : segs) {
                if (s.evidenceCount >= 2) {
                    kept.append(s);
                    continue;
                }
                const int i0 = cam.indexFor(s.tFirst);
                const int i1 =
                    cam.indexFor(std::min(durationMs, s.tFirst + kPostClickDwellMs));
                if (std::hypot(cam.x.at(i1) - cam.x.at(i0), cam.y.at(i1) - cam.y.at(i0))
                    <= kDriveByDistFrac)
                    kept.append(s);
            }
            segs = kept;
        }
        if (segs.isEmpty())
            segs = dwellSegments(cam, params, durationMs);
    } else {
        segs = dwellSegments(cam, params, durationMs);
    }
    // Typing activity is dwell evidence: fold each burst in as a dwell segment
    // anchored where the cursor was when typing began (the field you clicked
    // into). shapeSpans then merges it with a nearby click zoom and holds the
    // framing through the burst instead of pumping out and back in.
    for (const auto &burst : typingBursts) {
        const qint64 bStart = std::max<qint64>(0, burst.first);
        const qint64 bEnd = std::min(burst.second, durationMs);
        if (bEnd <= bStart)
            continue;
        Segment s;
        s.dwell = true;
        s.tFirst = bStart;
        s.tLast = bEnd;
        QPointF focus(0.5, 0.5);
        if (!cam.empty()) {
            const int idx = cam.indexFor(bStart);
            focus = QPointF(cam.x.at(idx), cam.y.at(idx));
        }
        s.bbox = QRectF(focus, QSizeF(0, 0));
        s.focus = focus;
        s.evidenceCount = 1;
        segs.append(s);
    }
    if (!typingBursts.isEmpty())
        std::stable_sort(segs.begin(), segs.end(),
                         [](const Segment &a, const Segment &b) { return a.tFirst < b.tFirst; });

    if (params.zoomIntensity <= 0.02)
        segs.clear();

    QVector<Span> spans = shapeSpans(segs, params, durationMs);

    // Route around pinned (Manual/locked) keyframes.
    QVector<QPair<qint64, qint64>> windows;
    windows.reserve(pinned.size());
    for (const Keyframe &k : pinned)
        windows.append({k.tMs - k.easeInMs, k.tMs + k.easeOutMs});

    // Assemble. Fit mode opens on the full frame and returns to it between spans;
    // fill mode opens on the (cursor-following) OUTPUT-aspect crop and returns to
    // it, slow-panning through the idle gaps so there is never a letterbox section.
    // The opener is a span-less single keyframe, so the `windows` routing below
    // can't protect it: a pinned kf at t=0 has a zero-width window and the
    // belt-and-braces filter compares strictly. Emitting an Auto opener anyway
    // would stack it at t=0 next to the user's own — and since survivors sort
    // first, the Auto one lands LAST, which is exactly the one setKeyframes()
    // reads for initialRect. The user's edit would lose to a duplicate.
    bool havePinnedOpener = false;
    for (const Keyframe &k : pinned) {
        if (k.tMs <= 0) {
            havePinnedOpener = true;
            break;
        }
    }

    QVector<Keyframe> out;
    if (!params.fill) {
        if (!havePinnedOpener) {
            Keyframe first;
            first.tMs = 0;
            first.rect = g.full();
            first.easeInMs = 0;
            first.easeOutMs = 0;
            first.source = ZoomTimeline::Auto;
            out.append(first);
        }

        for (Span &s : spans) {
            if (!windows.isEmpty() && !trimSpanAgainstPinned(s, windows, params))
                continue;                    // dropped: overlaps a pinned kf
            emitSpan(s, cam, g, params, durationMs, out);
        }
    } else {
        const BaseTrack base = buildBaseTrack(cam, g, durationMs);

        if (!havePinnedOpener) {
            Keyframe first;
            first.tMs = 0;
            first.rect = g.rectAt(base.at(0), 1.0);
            first.easeInMs = 0;
            first.easeOutMs = 0;
            first.source = ZoomTimeline::Auto;
            out.append(first);
        }

        // Surviving spans + their [start, end+zoomOut] active intervals (base pans
        // are suppressed inside these — the span owns the framing there).
        QVector<Span> keptSpans;
        QVector<QPair<qint64, qint64>> active;
        for (Span s : spans) {
            if (!windows.isEmpty() && !trimSpanAgainstPinned(s, windows, params))
                continue;
            active.append({s.start, s.end + params.zoomOutMs});
            keptSpans.append(s);
        }
        auto inActive = [&](qint64 t) {
            for (const auto &iv : active)
                if (t >= iv.first && t <= iv.second)
                    return true;
            return false;
        };

        // Idle base pans across the whole timeline (a slow follow), skipping the
        // span-active regions so the zoom keyframes are not fought.
        QPointF lastEmitted = base.c.value(0, QPointF(0.5, 0.5));
        for (int i = 1; i < base.t.size(); ++i) {
            const qint64 t = base.t.at(i);
            if (inActive(t)) {
                lastEmitted = base.c.at(i);  // resume measuring from where the span left the base
                continue;
            }
            const QRectF baseView = g.rectAt(base.c.at(i), 1.0);
            const double emitDistance =
                std::max(0.025, std::min(baseView.width(), baseView.height()) * 0.10);
            if (params.zoomIntensity > 0.02
                && std::hypot(base.c.at(i).x() - lastEmitted.x(),
                              base.c.at(i).y() - lastEmitted.y()) > emitDistance) {
                Keyframe k;
                k.tMs = t;
                k.rect = g.rectAt(base.c.at(i), 1.0);
                k.easeInMs = kBasePanEaseMs;
                k.easeOutMs = kBasePanEaseMs;
                k.source = ZoomTimeline::Auto;
                k.locked = false;
                out.append(k);
                lastEmitted = base.c.at(i);
            }
        }

        for (const Span &s : keptSpans)
            emitSpanFill(s, cam, g, params, base, durationMs, out);
    }

    // Drop any auto keyframe that still lands inside a pinned window (belt and
    // braces for pans emitted right at a trimmed edge).
    if (!windows.isEmpty()) {
        QVector<Keyframe> filtered;
        filtered.reserve(out.size());
        for (const Keyframe &k : out) {
            bool inside = false;
            // Never drop the t=0 opener: it seeds the initial framing (fill
            // mode would otherwise open letterboxed until the first keyframe).
            if (k.tMs > 0)
                for (const auto &w : windows)
                    if (k.tMs > w.first && k.tMs < w.second) { inside = true; break; }
            if (!inside)
                filtered.append(k);
        }
        out = filtered;
    }

    // Strict, stable sort by time, then resolve clamped boundary collisions. A
    // final-instant collision keeps the last target instead of nudging beyond
    // durationMs.
    std::stable_sort(out.begin(), out.end(),
                     [](const Keyframe &a, const Keyframe &b) { return a.tMs < b.tMs; });
    QVector<Keyframe> strict;
    strict.reserve(out.size());
    for (Keyframe keyframe : std::as_const(out)) {
        keyframe.tMs = std::clamp<qint64>(keyframe.tMs, 0, durationMs);
        if (strict.isEmpty() || keyframe.tMs > strict.last().tMs) {
            strict.append(keyframe);
        } else if (strict.last().tMs < durationMs) {
            keyframe.tMs = strict.last().tMs + 1;
            strict.append(keyframe);
        } else {
            keyframe.tMs = durationMs;
            strict.last() = keyframe; // last target at the final instant wins
        }
    }

    return strict;
}

// ---------------------------------------------------------------------------
// Manual-keyframe geometry: one source of truth with the generator's camera so
// user-placed zooms frame exactly like the auto ones.
// ---------------------------------------------------------------------------
QRectF KeyframeEngine::cameraRect(QSize videoSize, const QString &aspect, QPointF center,
                                  double zoom)
{
    const double W = videoSize.width() > 0 ? videoSize.width() : 1.0;
    const double H = videoSize.height() > 0 ? videoSize.height() : 1.0;
    Geometry g;
    g.init(W, H, aspectRatio(aspect, W / H));
    return g.rectAt(center, std::max(1.0, zoom));
}

double KeyframeEngine::zoomOfRect(QSize videoSize, const QString &aspect, const QRectF &rect)
{
    if (rect.width() <= 0.0)
        return 1.0;
    const double W = videoSize.width() > 0 ? videoSize.width() : 1.0;
    const double H = videoSize.height() > 0 ? videoSize.height() : 1.0;
    Geometry g;
    g.init(W, H, aspectRatio(aspect, W / H));
    // rectAt sizes width as bw0 / z, so z == bw0 / width.
    return std::max(1.0, g.bw0 / rect.width());
}
