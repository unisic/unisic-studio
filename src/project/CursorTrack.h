#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <cstdint>

// A recorded cursor motion track: the pointer position sampled over the video
// timeline, plus the distinct cursor bitmaps ("shapes") it wore. Value type on
// purpose — no QObject, no signals — so the zoom/pan engine and the tests can
// hold, copy and diff it cheaply, entirely offscreen. Time is video-ms (the
// video is CFR, so a millisecond maps to a definite frame); positions are
// stream pixels straight from PipeWire's spa_meta_cursor, i.e. integers.
//
// JSON on disk is COLUMNAR + DELTA-encoded, because a raw array of
// {t,x,y,visible,shape} objects is dominated by repeated key strings and
// full-magnitude numbers that gzip poorly. The columnar form groups like with
// like and stores small deltas that pack tight:
//
//   {
//     "dt":     [t0, t1-t0, t2-t1, ...],     // first ABSOLUTE ms, rest deltas
//     "x":      [x0, x1-x0, x2-x1, ...],     // first absolute px, rest deltas
//     "y":      [y0, y1-y0, y2-y1, ...],     //   (x/y rounded to int stream px)
//     "visible":[[startIdx,len], ...],       // RLE of the FALSE runs only
//     "shape":  [[shapeId,runLen], ...],     // RLE of the per-sample shapeId
//     "shapes": [{"id","hx","hy","png": <base64>}, ...]
//   }
//
// visible defaults true, so only the runs where the cursor is HIDDEN are
// listed (start index into the sample array + run length); everything else is
// visible. shape is a plain run-length list covering every sample. x/y are
// rounded to int before delta-ing, so the roundtrip is LOSSLESS for the
// integer stream-pixel positions we actually capture (sub-pixel values from
// interpolation are never stored — only appended raw samples are).
struct CursorSample {
    qint64 tMs = 0;
    double x = 0.0;
    double y = 0.0;
    bool visible = true;
    int shapeId = -1;   // index into shapes(); -1 = no bitmap / unknown
};

struct CursorShape {
    int id = 0;
    int hotspotX = 0;
    int hotspotY = 0;
    QByteArray png;     // the cursor bitmap, PNG-encoded
};

class CursorTrack
{
public:
    // Append the next sample. Enforces a non-decreasing timeline: a sample
    // whose tMs is earlier than the last is DROPPED (returns false) — PipeWire
    // meta can momentarily repeat/regress and an out-of-order sample would
    // break the binary search in sample(). Equal timestamps are allowed.
    bool append(const CursorSample &s);

    // Cursor state at video-ms t. Binary search + linear interpolation of
    // x/y between the two neighbouring samples; visible/shapeId are taken from
    // the EARLIER neighbour (state changes take effect at their own sample and
    // hold until the next). Clamps to the first/last sample outside the range.
    // Empty track → a hidden default at t. The returned sample's tMs is t.
    CursorSample sample(qint64 t) const;

    bool isEmpty() const { return m_samples.isEmpty(); }
    int count() const { return m_samples.size(); }

    // Highest sample timestamp (tracks run in video time from 0); 0 if empty.
    qint64 durationMs() const;

    const QList<CursorSample> &samples() const { return m_samples; }
    const QList<CursorShape> &shapes() const { return m_shapes; }
    void setShapes(const QList<CursorShape> &shapes) { m_shapes = shapes; }
    void clear() { m_samples.clear(); m_shapes.clear(); }

    // Remove every sample inside any [start, end) range and pull later
    // timestamps left by the total excised length before them — this is pause
    // removal, so the timeline must close up seamlessly. end is EXCLUSIVE so
    // that adjacent excisions ([a,b) then [b,c)) compose without double-
    // counting the shared instant b. Ranges are normalized internally (sorted,
    // merged when they touch or overlap); pure and deterministic.
    void excise(const QList<QPair<qint64, qint64>> &ranges);

    QJsonObject toJson() const;
    static CursorTrack fromJson(const QJsonObject &o);

private:
    QList<CursorSample> m_samples;   // sorted, non-decreasing tMs
    QList<CursorShape> m_shapes;
};
