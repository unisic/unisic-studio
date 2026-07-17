#pragma once
#include <QJsonObject>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVariantList>

#include "ClickTrack.h"
#include "CursorTrack.h"
// Full definitions (not forward decls): moc registers metatypes for the
// ZoomTimeline*/StyleModel* Q_PROPERTYs, which needs the pointee complete.
#include "StyleModel.h"
#include "ZoomTimeline.h"

// The sidecar `.unisicstudio` document: everything needed to re-open a recording
// for non-destructive editing that ISN'T the video itself. It points at the
// master video by relative + absolute path, carries the cursor and click tracks,
// the zoom/pan timeline, the visual style, trim, and opaque export settings.
//
// Ownership: the ZoomTimeline and StyleModel are child QObjects (parented to
// this). The cursor/click tracks are value members. dirty flips true on any
// mutation routed through this class or signalled by the child models, and
// clears on save()/load().
//
// On-disk format is schemaVersion 1, compact JSON, written atomically. load()
// refuses a newer schema outright. A leading gzip magic (0x1F 0x8B) is reserved
// as a compressed-container escape hatch — sniffed and qUncompress-ed on read —
// but v1 always WRITES uncompressed, so the branch is dormant forward-compat.
class StudioProject : public QObject
{
    Q_OBJECT

    Q_PROPERTY(int schemaVersion READ schemaVersion CONSTANT)
    Q_PROPERTY(QString videoRelPath READ videoRelPath WRITE setVideoRelPath NOTIFY videoChanged)
    Q_PROPERTY(QString videoAbsPath READ videoAbsPath WRITE setVideoAbsPath NOTIFY videoChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs WRITE setDurationMs NOTIFY videoChanged)
    Q_PROPERTY(double fps READ fps WRITE setFps NOTIFY videoChanged)
    Q_PROPERTY(QSize videoSize READ videoSize WRITE setVideoSize NOTIFY videoChanged)
    Q_PROPERTY(QString videoHash READ videoHash WRITE setVideoHash NOTIFY videoChanged)
    Q_PROPERTY(QString videoResolved READ videoResolved NOTIFY videoChanged)
    Q_PROPERTY(bool videoMissing READ videoMissing NOTIFY videoChanged)

    Q_PROPERTY(QString compositor READ compositor WRITE setCompositor NOTIFY recordingChanged)
    Q_PROPERTY(QString cursorMode READ cursorMode WRITE setCursorMode NOTIFY recordingChanged)
    Q_PROPERTY(qint64 t0MonoNs READ t0MonoNs WRITE setT0MonoNs NOTIFY recordingChanged)
    Q_PROPERTY(bool hadClickCapture READ hadClickCapture WRITE setHadClickCapture NOTIFY recordingChanged)
    // Optional webcam sidecar recorded alongside the screen (schemaVersion stays
    // 1 — additive). Resolved like the master video (rel first, then abs).
    Q_PROPERTY(QString webcamResolved READ webcamResolved NOTIFY videoChanged)
    Q_PROPERTY(bool hasWebcam READ hasWebcam NOTIFY videoChanged)

    Q_PROPERTY(qint64 trimInMs READ trimInMs WRITE setTrimInMs NOTIFY trimChanged)
    Q_PROPERTY(qint64 trimOutMs READ trimOutMs WRITE setTrimOutMs NOTIFY trimChanged)

    Q_PROPERTY(ZoomTimeline *zoom READ zoom CONSTANT)
    Q_PROPERTY(StyleModel *style READ style CONSTANT)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)

public:
    explicit StudioProject(QObject *parent = nullptr);

    static constexpr int kSchemaVersion = 1;
    int schemaVersion() const { return kSchemaVersion; }

    // The file extension (no dot). One source of truth for filters/save dialogs.
    static QString projectExtension() { return QStringLiteral("unisicstudio"); }

    // Cheap, ADVISORY relink fingerprint: "<size>-<mtimeEpochMs>-<crc16>" where
    // crc16 is qChecksum of the first 64 KiB. Not cryptographic — it exists only
    // to warn when a resolved video is plainly not the one this project recorded
    // (re-encoded, truncated, swapped). Empty string if the path can't be read.
    static QString computeVideoHash(const QString &path);

    // --- video ---
    QString videoRelPath() const { return m_videoRelPath; }
    QString videoAbsPath() const { return m_videoAbsPath; }
    qint64 durationMs() const { return m_durationMs; }
    double fps() const { return m_fps; }
    QSize videoSize() const { return m_videoSize; }
    QString videoHash() const { return m_videoHash; }
    // Absolute path to the video that load() actually found (rel first, then
    // abs), or empty if neither existed. Only meaningful after load().
    QString videoResolved() const { return m_videoResolved; }
    bool videoMissing() const { return m_videoMissing; }

    void setVideoRelPath(const QString &v);
    void setVideoAbsPath(const QString &v);
    void setDurationMs(qint64 v);
    void setFps(double v);
    void setVideoSize(const QSize &v);
    void setVideoHash(const QString &v);

    // --- recording metadata ---
    QString compositor() const { return m_compositor; }
    QString cursorMode() const { return m_cursorMode; }   // metadata|embedded|none
    qint64 t0MonoNs() const { return m_t0MonoNs; }
    bool hadClickCapture() const { return m_hadClickCapture; }

    void setCompositor(const QString &v);
    void setCursorMode(const QString &v);
    void setT0MonoNs(qint64 v);
    void setHadClickCapture(bool v);

    // --- webcam sidecar (optional) ---
    QString webcamRelPath() const { return m_webcamRelPath; }
    QString webcamAbsPath() const { return m_webcamAbsPath; }
    QString webcamResolved() const { return m_webcamResolved; }
    bool hasWebcam() const { return !m_webcamResolved.isEmpty(); }
    void setWebcamRelPath(const QString &v);
    void setWebcamAbsPath(const QString &v);

    // --- trim ---
    qint64 trimInMs() const { return m_trimInMs; }
    qint64 trimOutMs() const { return m_trimOutMs; }
    void setTrimInMs(qint64 v);
    void setTrimOutMs(qint64 v);

    // --- tracks (value members; edits should mark dirty via markDirty()) ---
    const CursorTrack &cursorTrack() const { return m_cursor; }
    CursorTrack &cursorTrack() { return m_cursor; }
    void setCursorTrack(const CursorTrack &t) { m_cursor = t; markDirty(); }
    const ClickTrack &clickTrack() const { return m_clicks; }
    ClickTrack &clickTrack() { return m_clicks; }
    void setClickTrack(const ClickTrack &t) { m_clicks = t; markDirty(); }

    ZoomTimeline *zoom() const { return m_zoom; }
    StyleModel *style() const { return m_style; }

    // Down-event times (ms) for the timeline's click markers. Immutable for a
    // loaded project, so QML can bind it once.
    Q_INVOKABLE QVariantList clickDownTimesMs() const;

    // --- export settings (opaque passthrough for the render layer) ---
    QJsonObject exportSettings() const { return m_exportSettings; }
    void setExportSettings(const QJsonObject &o) { m_exportSettings = o; markDirty(); }

    bool dirty() const { return m_dirty; }
    // Call after mutating a value track (or anything not routed through a
    // setter) so unsaved edits are visible to the UI.
    void markDirty();

    // Atomic (QSaveFile) compact-JSON write. Clears dirty on success. On failure
    // returns false and, if error != nullptr, a human-readable reason (tr()'d).
    bool save(const QString &path, QString *error = nullptr);

    // Parse a project file. Returns a heap StudioProject (caller owns; pass a
    // parent via setParent if desired) or nullptr with *error set. Resolves the
    // video path and sets videoMissing. Refuses schemaVersion > kSchemaVersion.
    static StudioProject *load(const QString &path, QString *error = nullptr);

signals:
    void videoChanged();
    void recordingChanged();
    void trimChanged();
    void dirtyChanged();

private:
    QJsonObject toJson() const;
    void resolveVideo(const QString &projectFilePath);
    void clearDirty();

    QString m_videoRelPath;
    QString m_videoAbsPath;
    qint64 m_durationMs = 0;
    double m_fps = 0.0;
    QSize m_videoSize;
    QString m_videoHash;
    QString m_videoResolved;
    bool m_videoMissing = false;

    QString m_compositor;
    QString m_cursorMode = QStringLiteral("none");
    qint64 m_t0MonoNs = 0;
    bool m_hadClickCapture = false;

    QString m_webcamRelPath;
    QString m_webcamAbsPath;
    QString m_webcamResolved;   // resolved at load(); empty when absent/missing

    qint64 m_trimInMs = 0;
    qint64 m_trimOutMs = 0;

    CursorTrack m_cursor;
    ClickTrack m_clicks;
    ZoomTimeline *m_zoom;   // child (parented to this)
    StyleModel *m_style;    // child (parented to this)
    QJsonObject m_exportSettings;

    bool m_dirty = false;
};
