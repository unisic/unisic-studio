#pragma once
#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVector>

#include "project/ClickTrack.h"
#include "project/CursorTrack.h"

// The click ripples active at the current playback instant. Each row is a click
// (normalised position + its down-time); the QML delegate derives the ripple's
// expand/fade phase from (CursorPlayback.timeMs - tMs), so the MODEL changes
// only when the active SET changes (a ripple begins or expires) — never every
// frame. That keeps the render hot path free of per-frame model churn.
class CursorRippleModel : public QAbstractListModel
{
    Q_OBJECT
public:
    struct Ripple {
        double nx = 0.0;
        double ny = 0.0;
        qint64 tMs = 0;
        int idx = 0;   // stable identity = index of the click in the down list
    };

    enum Roles { NxRole = Qt::UserRole + 1, NyRole, TMsRole };

    explicit CursorRippleModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replace the active set. No-op (no signal) when the identity list is
    // unchanged, so a steady frame does not touch the model.
    void setActive(const QVector<Ripple> &next);

private:
    QVector<Ripple> m_rows;
};

// Drives the cursor overlay: given a video-ms time, exposes the pointer's
// normalised position / visibility / current shape bitmap and the active click
// ripples. Pure playback state — it holds COPIES of the tracks (decoupled from
// the live editable project) and never mutates them. The cursor lookup caches
// the last sample index for O(1) sequential advance, and the steady path reuses
// its scratch buffers, so ticking it every frame allocates nothing.
//
// Shape bitmaps are served through image://cursorshape/<projectId>/<shapeId>;
// this class only formats those URLs and reports each shape's pixel size +
// hotspot — the owning controller registers the actual bytes with
// CursorShapeProvider. Kept free of any QtQuick dependency so it unit-tests
// under Core+Gui alone.
class CursorPlayback : public QObject
{
    Q_OBJECT

    Q_PROPERTY(qreal timeMs READ timeMs NOTIFY timeChanged)
    Q_PROPERTY(bool cursorVisible READ cursorVisible NOTIFY sampleChanged)
    Q_PROPERTY(qreal nx READ nx NOTIFY sampleChanged)
    Q_PROPERTY(qreal ny READ ny NOTIFY sampleChanged)
    Q_PROPERTY(QString shapeUrl READ shapeUrl NOTIFY shapeChanged)
    Q_PROPERTY(int shapeWidth READ shapeWidth NOTIFY shapeChanged)
    Q_PROPERTY(int shapeHeight READ shapeHeight NOTIFY shapeChanged)
    Q_PROPERTY(qreal hotspotX READ hotspotX NOTIFY shapeChanged)
    Q_PROPERTY(qreal hotspotY READ hotspotY NOTIFY shapeChanged)
    // Milliseconds since the most recent click at/or-before the current time, or
    // -1 if none yet. A pure function of time (like the ripples), so the cursor
    // 'press' feedback it drives is identical in preview and export.
    Q_PROPERTY(qreal msSinceClick READ msSinceClick NOTIFY sampleChanged)
    Q_PROPERTY(int rippleMs READ rippleMs CONSTANT)
    Q_PROPERTY(QAbstractListModel *ripples READ ripples CONSTANT)

public:
    explicit CursorPlayback(QString projectId, QObject *parent = nullptr);

    // Snapshot the tracks; videoSize maps stream px -> normalised [0,1]. Decodes
    // each shape once (size + hotspot only) and pre-builds its image URL.
    void setTracks(const CursorTrack &cursor, const ClickTrack &clicks, QSize videoSize);

    // Advance to video-ms t (cached-index sequential lookup + ripple refresh).
    void setTime(qint64 tMs);

    // Test/probe helper: index i with samples[i].tMs <= t (clamped), using the
    // cached hint. Public so a unit test can pin the O(1) lookup against
    // CursorTrack::sample.
    int sampleIndexFor(qint64 t) const;

    qreal timeMs() const { return m_timeMs; }
    bool cursorVisible() const { return m_visible; }
    qreal nx() const { return m_nx; }
    qreal ny() const { return m_ny; }
    QString shapeUrl() const { return m_shapeUrl; }
    int shapeWidth() const { return m_shapeW; }
    int shapeHeight() const { return m_shapeH; }
    qreal hotspotX() const { return m_hotspotX; }
    qreal hotspotY() const { return m_hotspotY; }
    qreal msSinceClick() const { return m_msSinceClick; }
    int rippleMs() const { return kRippleMs; }
    QAbstractListModel *ripples() const;

    const QString &projectId() const { return m_projectId; }

signals:
    void timeChanged();
    void sampleChanged();
    void shapeChanged();

private:
    void applyShape(int shapeId);

    static constexpr int kRippleMs = 420;

    QString m_projectId;
    CursorTrack m_cursor;
    QVector<ClickEvent> m_downs;        // down events only, sorted by tMs

    struct ShapeInfo {
        int w = 0;
        int h = 0;
        double hotspotX = 0.0;
        double hotspotY = 0.0;
        QString url;
    };
    QHash<int, ShapeInfo> m_shapes;
    double m_invW = 1.0;
    double m_invH = 1.0;

    qreal m_timeMs = 0.0;
    bool m_visible = false;
    qreal m_nx = 0.5;
    qreal m_ny = 0.5;
    int m_shapeId = -2;                 // -2 == "never applied", forces first emit
    QString m_shapeUrl;
    int m_shapeW = 0;
    int m_shapeH = 0;
    qreal m_hotspotX = 0.0;
    qreal m_hotspotY = 0.0;
    qreal m_msSinceClick = -1.0;        // for the cursor 'press' feedback

    mutable int m_lastIdx = 0;          // cursor-sample lookup hint
    int m_downCursor = 0;               // ripple-scan hint
    qint64 m_prevT = -1;                // detect backward seeks

    CursorRippleModel *m_ripples;
    QVector<CursorRippleModel::Ripple> m_activeScratch; // reused each setTime
};
