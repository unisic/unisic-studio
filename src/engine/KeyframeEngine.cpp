#include "KeyframeEngine.h"

#include "CursorSmoother.h"
#include "ClickTrack.h"
#include "CursorTrack.h"
#include "Easing.h"

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
        {"idleAfterMs", idleAfterMs},
        {"idleSpeedFracPerSec", idleSpeedFracPerSec},
        {"zoomMin", zoomMin},
        {"zoomMax", zoomMax},
        {"marginFrac", marginFrac},
        {"deadZoneFrac", deadZoneFrac},
        {"dwellMinMs", dwellMinMs},
        {"dwellSpeedFracPerSec", dwellSpeedFracPerSec},
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
    p.idleAfterMs = i("idleAfterMs", p.idleAfterMs);
    p.idleSpeedFracPerSec = d("idleSpeedFracPerSec", p.idleSpeedFracPerSec);
    p.zoomMin = d("zoomMin", p.zoomMin);
    p.zoomMax = d("zoomMax", p.zoomMax);
    p.marginFrac = d("marginFrac", p.marginFrac);
    p.deadZoneFrac = d("deadZoneFrac", p.deadZoneFrac);
    p.dwellMinMs = i("dwellMinMs", p.dwellMinMs);
    p.dwellSpeedFracPerSec = d("dwellSpeedFracPerSec", p.dwellSpeedFracPerSec);
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
// evaluate(): rect at t under the "hold, then ease into each keyframe" model
// documented in the header. Same routine drives preview and export.
// ---------------------------------------------------------------------------
namespace {

QRectF lerpRect(const QRectF &a, const QRectF &b, double e)
{
    return QRectF(a.x() + (b.x() - a.x()) * e,
                  a.y() + (b.y() - a.y()) * e,
                  a.width() + (b.width() - a.width()) * e,
                  a.height() + (b.height() - a.height()) * e);
}

} // namespace

QRectF KeyframeEngine::evaluate(const QVector<Keyframe> &kfs, qint64 tMs)
{
    if (kfs.isEmpty())
        return QRectF(0, 0, 1, 1);          // no camera == whole frame
    if (tMs <= kfs.first().tMs)
        return kfs.first().rect;
    if (tMs >= kfs.last().tMs)
        return kfs.last().rect;

    // First keyframe strictly after t; the one before it is A.
    int hi = 1;
    while (hi < kfs.size() && kfs.at(hi).tMs <= tMs)
        ++hi;
    const Keyframe &A = kfs.at(hi - 1);
    const Keyframe &B = kfs.at(hi);

    const qint64 span = B.tMs - A.tMs;
    if (span <= 0)
        return B.rect;
    const qint64 d = std::min<qint64>(B.easeInMs > 0 ? B.easeInMs : span, span);
    const qint64 transitionStart = B.tMs - d;
    if (tMs <= transitionStart)
        return A.rect;                       // still holding A's rect
    const double u = double(tMs - transitionStart) / double(d);
    return lerpRect(A.rect, B.rect, Easing::preset(u));
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
constexpr double kMoveEmitFrac = 0.01;      // emit a pan keyframe once the camera
                                            // center shifts > 1% of the frame
constexpr int kPanEaseMs = 300;             // pan smoothing (clamped to the gap)

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
};

struct Span {
    qint64 start = 0;      // begin zoom-in
    qint64 end = 0;        // begin zoom-out
    QRectF bbox;
    bool dwell = false;
    QVector<qint64> clickTimes;
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
QVector<Segment> clusterClicks(const ClickTrack &clicks, const Params &p, double W, double H)
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
        const bool joins = open
            && (e.tMs - prevT) < p.clickClusterGapMs
            && std::hypot(e.x - prevPx, e.y - prevPy) / W < p.clickClusterDistFrac;
        if (!joins) {
            close();
            cur = Segment{};
            cur.tFirst = e.tMs;
            cur.bbox = QRectF(QPointF(e.x / W, e.y / H), QPointF(e.x / W, e.y / H));
            open = true;
        }
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
        if (cam.spd.at(i) >= thresh) { ++i; continue; }
        const int runStart = i - 1;         // include the sample the slow run departs from
        double sx = cam.x.at(runStart), sy = cam.y.at(runStart);
        int cnt = 1;
        while (i < cam.t.size() && cam.spd.at(i) < thresh) {
            sx += cam.x.at(i); sy += cam.y.at(i); ++cnt;
            ++i;
        }
        const qint64 t0 = cam.t.at(runStart);
        const qint64 t1 = cam.t.at(std::min(i, int(cam.t.size()) - 1));
        if (t1 - t0 < p.dwellMinMs)
            continue;
        Segment s;
        s.dwell = true;
        s.tFirst = t0;
        s.tLast = std::min(t1, durationMs);
        const double mx = sx / cnt, my = sy / cnt;
        s.bbox = QRectF(mx, my, 0, 0);      // a point; zoom uses the dwell level
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
    const qint64 mergeGap = qint64(p.zoomOutMs) + p.zoomInMs;
    for (const Segment &sg : segs) {
        Span s;
        s.start = std::max<qint64>(0, sg.tFirst - p.leadInMs);
        s.end = sg.tLast + p.tailMs;
        s.bbox = sg.bbox;
        s.dwell = sg.dwell;
        s.clickTimes = sg.clickTimes;
        if (!spans.isEmpty() && s.start - spans.last().end < mergeGap) {
            // Retarget instead of out+in: keep zoomed, union the framing.
            Span &prev = spans.last();
            prev.end = std::max(prev.end, s.end);
            prev.bbox = prev.bbox.united(s.bbox);
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
        if (s.end > durationMs)
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
};

// Emit the keyframes for one span into `out`. Returns nothing; the dead-zone
// camera runs at 10 Hz and only lays down a pan keyframe when the center has
// actually moved > 1% of the frame (coalesced, so the count stays low).
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
    }

    // The camera freezes at the cluster centre through the zoom-in ramp, then
    // the dead-zone follows the cursor. bbox.center() is exactly the click
    // point for a single-click cluster (a 0-size bbox), which frames it well.
    QPointF center = s.bbox.center();

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

    // Dead-zone pan. Half-extents of the inner dead zone the cursor roams free.
    QPointF lastEmitted = center;
    for (qint64 t = s.start + p.zoomInMs + kCameraTickMs; t < s.end; t += kCameraTickMs) {
        const QRectF view = g.rectAt(center, z);
        const double hx = p.deadZoneFrac * view.width() / 2.0;
        const double hy = p.deadZoneFrac * view.height() / 2.0;
        const QPointF c = cam.sample(t);
        const double dx = c.x() - center.x();
        const double dy = c.y() - center.y();
        // Pan only enough to pull the cursor back onto the dead-zone edge.
        if (dx > hx)      center.setX(center.x() + (dx - hx));
        else if (dx < -hx) center.setX(center.x() + (dx + hx));
        if (dy > hy)      center.setY(center.y() + (dy - hy));
        else if (dy < -hy) center.setY(center.y() + (dy + hy));
        if (std::hypot(center.x() - lastEmitted.x(), center.y() - lastEmitted.y()) > kMoveEmitFrac) {
            mk(t, g.rectAt(center, z), kPanEaseMs, p.zoomOutMs);
            lastEmitted = center;
        }
    }

    // Zoom-out: ease zoomed -> full over [end, end+zoomOutMs].
    mk(s.end + p.zoomOutMs, g.full(), p.zoomOutMs, p.zoomOutMs);
}

// ---- crop-to-fill (aspect fill) base camera --------------------------------
// The base (non-zoomed) crop in fill mode is the largest centred OUTPUT-aspect
// rect of the source (g.rectAt(center, 1)). It slow-pans to follow the cursor
// with a LARGE dead zone, so the action stays framed without the twitchy motion
// of the tight in-span camera. Sampled at span boundaries too.
constexpr double kFillBaseDeadZone = 0.55;  // large → the base crop drifts slowly
constexpr int kBasePanEaseMs = 700;         // gentle ease for a base pan

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
    return bt;
}

// A fill-mode span: identical to emitSpan but the zoom-in eases FROM the base
// crop and the zoom-out returns TO the (cursor-following) base crop rather than
// the whole frame — so there is never a letterboxed full-frame section.
void emitSpanFill(const Span &s, const Cam &cam, const Geometry &g, const Params &p,
                  const BaseTrack &base, qint64 durationMs, QVector<Keyframe> &out)
{
    double z;
    if (s.dwell)
        z = std::max(1.0, p.zoomMin + (1.0 - p.zoomMin) * 0.25);
    else
        z = g.zoomForBbox(s.bbox, p);

    QPointF center = s.bbox.center();
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

    QPointF lastEmitted = center;
    for (qint64 t = s.start + p.zoomInMs + kCameraTickMs; t < s.end; t += kCameraTickMs) {
        const QRectF view = g.rectAt(center, z);
        const double hx = p.deadZoneFrac * view.width() / 2.0;
        const double hy = p.deadZoneFrac * view.height() / 2.0;
        const QPointF c = cam.sample(t);
        const double dx = c.x() - center.x();
        const double dy = c.y() - center.y();
        if (dx > hx)       center.setX(center.x() + (dx - hx));
        else if (dx < -hx) center.setX(center.x() + (dx + hx));
        if (dy > hy)       center.setY(center.y() + (dy - hy));
        else if (dy < -hy) center.setY(center.y() + (dy + hy));
        if (std::hypot(center.x() - lastEmitted.x(), center.y() - lastEmitted.y()) > kMoveEmitFrac) {
            mk(t, g.rectAt(center, z), kPanEaseMs, p.zoomOutMs);
            lastEmitted = center;
        }
    }

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
                                           const QVector<Keyframe> &pinned)
{
    const double W = videoSize.width() > 0 ? videoSize.width() : 1.0;
    const double H = videoSize.height() > 0 ? videoSize.height() : 1.0;

    Geometry g;
    g.init(W, H, aspectRatio(aspect, W / H));

    const Cam cam = buildCam(cursor, W, H);

    // Segments: click clusters, or the dwell fallback when there are no clicks.
    QVector<Segment> segs;
    bool anyDown = false;
    for (const ClickEvent &e : clicks.events())
        if (e.state == ClickEvent::Down) { anyDown = true; break; }
    if (anyDown)
        segs = clusterClicks(clicks, params, W, H);
    else
        segs = dwellSegments(cam, params, durationMs);

    QVector<Span> spans = shapeSpans(segs, params, durationMs);

    // Route around pinned (Manual/locked) keyframes.
    QVector<QPair<qint64, qint64>> windows;
    windows.reserve(pinned.size());
    for (const Keyframe &k : pinned)
        windows.append({k.tMs - k.easeInMs, k.tMs + k.easeOutMs});

    // Assemble. Fit mode opens on the full frame and returns to it between spans;
    // fill mode opens on the (cursor-following) OUTPUT-aspect crop and returns to
    // it, slow-panning through the idle gaps so there is never a letterbox section.
    QVector<Keyframe> out;
    if (!params.fill) {
        Keyframe first;
        first.tMs = 0;
        first.rect = g.full();
        first.easeInMs = 0;
        first.easeOutMs = 0;
        first.source = ZoomTimeline::Auto;
        out.append(first);

        for (Span &s : spans) {
            if (!windows.isEmpty() && !trimSpanAgainstPinned(s, windows, params))
                continue;                    // dropped: overlaps a pinned kf
            emitSpan(s, cam, g, params, durationMs, out);
        }
    } else {
        const BaseTrack base = buildBaseTrack(cam, g, durationMs);

        Keyframe first;
        first.tMs = 0;
        first.rect = g.rectAt(base.at(0), 1.0);
        first.easeInMs = 0;
        first.easeOutMs = 0;
        first.source = ZoomTimeline::Auto;
        out.append(first);

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
            if (std::hypot(base.c.at(i).x() - lastEmitted.x(),
                           base.c.at(i).y() - lastEmitted.y()) > kMoveEmitFrac) {
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
            for (const auto &w : windows)
                if (k.tMs > w.first && k.tMs < w.second) { inside = true; break; }
            if (!inside)
                filtered.append(k);
        }
        out = filtered;
    }

    // Strict, stable sort by time, then force strictly-increasing tMs so no two
    // keyframes share an instant (the model contract) — collisions only happen
    // at clamped span boundaries, so a 1 ms nudge is harmless.
    std::stable_sort(out.begin(), out.end(),
                     [](const Keyframe &a, const Keyframe &b) { return a.tMs < b.tMs; });
    for (int i = 1; i < out.size(); ++i)
        if (out[i].tMs <= out[i - 1].tMs)
            out[i].tMs = out[i - 1].tMs + 1;

    return out;
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
