#pragma once
#include <QAbstractListModel>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QRectF>
#include <QVariantMap>
#include <QVector>

// The ordered list of zoom/pan keyframes that drive the camera over the video.
// A keyframe pins a normalized [0,1] viewport rect at a video-ms instant; the
// renderer eases between consecutive keyframes. Keyframes are auto-generated
// from the cursor/click tracks (source == Auto) or placed/edited by the user
// (source == Manual); regenerating the automatic camera must never discard the
// user's own work, hence clearAuto() spares Manual and anything locked.
//
// QAbstractListModel so the timeline UI binds to it directly. The backing list
// is always kept sorted by tMs — every mutation that can change ordering routes
// through the sorted-insert / re-sort helpers so a row index means the same
// thing to the model and to callers.
class ZoomTimeline : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(double zoomIntensity READ zoomIntensity WRITE setZoomIntensity
               NOTIFY zoomIntensityChanged)
    Q_PROPERTY(double motionSmoothness READ motionSmoothness WRITE setMotionSmoothness
               NOTIFY motionSmoothnessChanged)

public:
    enum Source { Auto = 0, Manual = 1 };
    Q_ENUM(Source)

    struct Keyframe {
        qint64 tMs = 0;
        QRectF rect;            // normalized [0,1] viewport
        int easeInMs = 300;
        int easeOutMs = 300;
        Source source = Manual;
        bool locked = false;
    };

    enum Roles {
        TMsRole = Qt::UserRole + 1,
        RectRole,
        EaseInRole,
        EaseOutRole,
        SourceRole,
        LockedRole,
    };

    explicit ZoomTimeline(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Sorted insert by tMs; returns the row it landed at. Ties keep insertion
    // order (inserted after equal-tMs keyframes already present).
    int addKeyframe(const Keyframe &kf);

    // Bulk sorted insert: adds every keyframe under a SINGLE model reset and emits
    // countChanged()/changed() ONCE, so a live PreviewController recomputes once
    // instead of once per keyframe (the regenerate binding storm). Tie ordering
    // matches per-item addKeyframe(): existing rows keep their place, appended rows
    // follow in argument order.
    void addKeyframes(const QList<Keyframe> &kfs);
    // Replace unlocked Auto rows with one generated batch under a single model
    // reset. Manual and locked rows survive. Used by regeneration to avoid two
    // preview/cache resets (clear + insert).
    void replaceAutoKeyframes(const QList<Keyframe> &kfs);

    Q_INVOKABLE void removeAt(int index);

    // Retime a keyframe and re-sort. Returns its new row (== index when the
    // move doesn't cross a neighbour), or -1 if index is out of range.
    Q_INVOKABLE int moveKeyframe(int index, qint64 newT);

    Q_INVOKABLE void setKeyframeRect(int index, const QRectF &rect);

    // Batch rect update under ONE changed()/dataChanged pulse (a per-row loop of
    // setKeyframeRect would recompute the preview N times). System-side use only
    // (aspect re-projection): unlike setKeyframeRect it does NOT promote rows to
    // Manual — the caller re-frames rows it already knows are Manual/locked, and
    // silently converting a locked Auto row would change its regenerate semantics.
    void setKeyframeRects(const QVector<QPair<int, QRectF>> &updates);
    Q_INVOKABLE void setKeyframeLocked(int index, bool locked);
    Q_INVOKABLE void setKeyframeEasing(int index, int easeInMs, int easeOutMs);

    // Drop every auto-generated keyframe that the user hasn't locked, ahead of
    // regenerating the automatic camera. Manual and locked keyframes survive.
    Q_INVOKABLE void clearAuto();

    // Row snapshot for QML (the inspector's keyframe section binds to this):
    // {tMs, x, y, w, h, easeInMs, easeOutMs, source, locked}. Empty map when
    // index is out of range.
    Q_INVOKABLE QVariantMap keyframeAt(int index) const;

    const QList<Keyframe> &keyframes() const { return m_keyframes; }
    void clear();

    // Generator parameters round-trip as one JSON object. The two primary motion
    // controls have typed properties for QML; all lower-level tuning remains
    // opaque to the model.
    static constexpr double DefaultZoomIntensity = 0.72;
    static constexpr double DefaultMotionSmoothness = 0.68;
    double zoomIntensity() const;
    double motionSmoothness() const;
    void setZoomIntensity(double value);
    void setMotionSmoothness(double value);
    QJsonObject autoParams() const { return m_autoParams; }
    void setAutoParams(const QJsonObject &p);

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &o);

signals:
    void countChanged();
    void zoomIntensityChanged();
    void motionSmoothnessChanged();
    // Coalesced "something mutated" pulse; StudioProject hangs dirty-tracking
    // off it rather than subscribing to every fine-grained model signal.
    void changed();

private:
    // Row of the first keyframe with tMs > t (the sorted-insert position).
    int insertionRow(qint64 t) const;
    // Lock an unlocked Auto keyframe the user just edited so regeneration
    // cannot silently discard the edit.
    void pinOnUserEdit(int index);

    QList<Keyframe> m_keyframes;
    QJsonObject m_autoParams;
};
