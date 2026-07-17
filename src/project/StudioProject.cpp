#include "StudioProject.h"

#include "StyleModel.h"
#include "ZoomTimeline.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

StudioProject::StudioProject(QObject *parent)
    : QObject(parent)
    , m_zoom(new ZoomTimeline(this))
    , m_style(new StyleModel(this))
{
    // Any edit inside a child model dirties the document. 3-arg connect with
    // `this` as context so these auto-disconnect when the project dies.
    connect(m_zoom, &ZoomTimeline::changed, this, &StudioProject::markDirty);
    connect(m_style, &StyleModel::changed, this, &StudioProject::markDirty);
}

void StudioProject::markDirty()
{
    if (m_dirty)
        return;
    m_dirty = true;
    emit dirtyChanged();
}

void StudioProject::clearDirty()
{
    if (!m_dirty)
        return;
    m_dirty = false;
    emit dirtyChanged();
}

// --- scalar setters: store, mark dirty, emit the grouped NOTIFY ---

void StudioProject::setVideoRelPath(const QString &v)
{
    if (m_videoRelPath == v) return;
    m_videoRelPath = v;
    markDirty();
    emit videoChanged();
}

void StudioProject::setVideoAbsPath(const QString &v)
{
    if (m_videoAbsPath == v) return;
    m_videoAbsPath = v;
    markDirty();
    emit videoChanged();
}

void StudioProject::setDurationMs(qint64 v)
{
    if (m_durationMs == v) return;
    m_durationMs = v;
    markDirty();
    emit videoChanged();
}

void StudioProject::setFps(double v)
{
    if (qFuzzyCompare(m_fps, v)) return;
    m_fps = v;
    markDirty();
    emit videoChanged();
}

void StudioProject::setVideoSize(const QSize &v)
{
    if (m_videoSize == v) return;
    m_videoSize = v;
    markDirty();
    emit videoChanged();
}

void StudioProject::setVideoHash(const QString &v)
{
    if (m_videoHash == v) return;
    m_videoHash = v;
    markDirty();
    emit videoChanged();
}

void StudioProject::setCompositor(const QString &v)
{
    if (m_compositor == v) return;
    m_compositor = v;
    markDirty();
    emit recordingChanged();
}

void StudioProject::setCursorMode(const QString &v)
{
    if (m_cursorMode == v) return;
    m_cursorMode = v;
    markDirty();
    emit recordingChanged();
}

void StudioProject::setT0MonoNs(qint64 v)
{
    if (m_t0MonoNs == v) return;
    m_t0MonoNs = v;
    markDirty();
    emit recordingChanged();
}

void StudioProject::setHadClickCapture(bool v)
{
    if (m_hadClickCapture == v) return;
    m_hadClickCapture = v;
    markDirty();
    emit recordingChanged();
}

void StudioProject::setTrimInMs(qint64 v)
{
    if (m_trimInMs == v) return;
    m_trimInMs = v;
    markDirty();
    emit trimChanged();
}

void StudioProject::setTrimOutMs(qint64 v)
{
    if (m_trimOutMs == v) return;
    m_trimOutMs = v;
    markDirty();
    emit trimChanged();
}

QVariantList StudioProject::clickDownTimesMs() const
{
    QVariantList out;
    for (const ClickEvent &e : m_clicks.events())
        if (e.state == ClickEvent::Down)
            out.append(double(e.tMs));
    return out;
}

QString StudioProject::computeVideoHash(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray head = f.read(64 * 1024);
    f.close();
    QFileInfo fi(path);
    const quint16 crc = qChecksum(QByteArrayView(head));
    return QStringLiteral("%1-%2-%3")
        .arg(fi.size())
        .arg(fi.lastModified().toMSecsSinceEpoch())
        .arg(crc, 4, 16, QLatin1Char('0'));
}

QJsonObject StudioProject::toJson() const
{
    QJsonObject video{
        {QStringLiteral("relPath"), m_videoRelPath},
        {QStringLiteral("absPath"), m_videoAbsPath},
        {QStringLiteral("durationMs"), double(m_durationMs)},
        {QStringLiteral("fps"), m_fps},
        {QStringLiteral("width"), m_videoSize.width()},
        {QStringLiteral("height"), m_videoSize.height()},
        {QStringLiteral("hash"), m_videoHash},
    };
    QJsonObject recording{
        {QStringLiteral("compositor"), m_compositor},
        {QStringLiteral("cursorMode"), m_cursorMode},
        {QStringLiteral("t0MonoNs"), double(m_t0MonoNs)},
        {QStringLiteral("hadClickCapture"), m_hadClickCapture},
    };
    QJsonObject trim{
        {QStringLiteral("inMs"), double(m_trimInMs)},
        {QStringLiteral("outMs"), double(m_trimOutMs)},
    };
    return QJsonObject{
        {QStringLiteral("schemaVersion"), kSchemaVersion},
        {QStringLiteral("video"), video},
        {QStringLiteral("recording"), recording},
        {QStringLiteral("trim"), trim},
        {QStringLiteral("cursor"), m_cursor.toJson()},
        {QStringLiteral("clicks"), m_clicks.toJson()},
        {QStringLiteral("zoom"), m_zoom->toJson()},
        {QStringLiteral("style"), m_style->toJson()},
        {QStringLiteral("exportSettings"), m_exportSettings},
    };
}

bool StudioProject::save(const QString &path, QString *error)
{
    const QByteArray bytes = QJsonDocument(toJson()).toJson(QJsonDocument::Compact);

    // QSaveFile: writes to a temp sibling and atomically renames on commit, so a
    // crash mid-write never corrupts an existing project file.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = tr("Cannot open %1 for writing: %2").arg(path, f.errorString());
        return false;
    }
    if (f.write(bytes) != bytes.size() || !f.commit()) {
        if (error)
            *error = tr("Failed to write %1: %2").arg(path, f.errorString());
        return false;
    }
    clearDirty();
    return true;
}

void StudioProject::resolveVideo(const QString &projectFilePath)
{
    const QDir dir = QFileInfo(projectFilePath).absoluteDir();

    // Prefer the RELATIVE path (project + video moved together as a bundle);
    // fall back to the recorded ABSOLUTE path (project opened from elsewhere,
    // video still in place). Neither found → flag missing for the UI.
    m_videoResolved.clear();
    if (!m_videoRelPath.isEmpty()) {
        const QString rel = QFileInfo(dir.filePath(m_videoRelPath)).absoluteFilePath();
        if (QFileInfo::exists(rel))
            m_videoResolved = rel;
    }
    if (m_videoResolved.isEmpty() && !m_videoAbsPath.isEmpty()
        && QFileInfo::exists(m_videoAbsPath)) {
        m_videoResolved = m_videoAbsPath;
    }
    m_videoMissing = m_videoResolved.isEmpty();
}

StudioProject *StudioProject::load(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("Cannot open %1: %2").arg(path, f.errorString());
        return nullptr;
    }
    QByteArray raw = f.readAll();
    f.close();

    // Reserved compressed-container sniff (see header). v1 writes uncompressed,
    // so this only fires on a future/foreign compressed file.
    if (raw.size() >= 2 && quint8(raw.at(0)) == 0x1F && quint8(raw.at(1)) == 0x8B) {
        const QByteArray un = qUncompress(raw);
        if (un.isEmpty()) {
            if (error)
                *error = tr("%1 looks compressed but could not be decompressed").arg(path);
            return nullptr;
        }
        raw = un;
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = tr("%1 is not a valid project file: %2").arg(path, perr.errorString());
        return nullptr;
    }
    const QJsonObject o = doc.object();

    const int ver = o.value(QStringLiteral("schemaVersion")).toInt(0);
    if (ver > kSchemaVersion) {
        if (error)
            *error = tr("%1 was made by a newer version of Unisic Studio "
                        "(schema %2, this build understands %3)")
                         .arg(path).arg(ver).arg(kSchemaVersion);
        return nullptr;
    }

    auto *p = new StudioProject;

    const QJsonObject video = o.value(QStringLiteral("video")).toObject();
    p->m_videoRelPath = video.value(QStringLiteral("relPath")).toString();
    p->m_videoAbsPath = video.value(QStringLiteral("absPath")).toString();
    p->m_durationMs = qint64(video.value(QStringLiteral("durationMs")).toDouble());
    p->m_fps = video.value(QStringLiteral("fps")).toDouble();
    p->m_videoSize = QSize(video.value(QStringLiteral("width")).toInt(),
                           video.value(QStringLiteral("height")).toInt());
    p->m_videoHash = video.value(QStringLiteral("hash")).toString();

    const QJsonObject rec = o.value(QStringLiteral("recording")).toObject();
    p->m_compositor = rec.value(QStringLiteral("compositor")).toString();
    p->m_cursorMode = rec.value(QStringLiteral("cursorMode")).toString(QStringLiteral("none"));
    p->m_t0MonoNs = qint64(rec.value(QStringLiteral("t0MonoNs")).toDouble());
    p->m_hadClickCapture = rec.value(QStringLiteral("hadClickCapture")).toBool();

    const QJsonObject trim = o.value(QStringLiteral("trim")).toObject();
    p->m_trimInMs = qint64(trim.value(QStringLiteral("inMs")).toDouble());
    p->m_trimOutMs = qint64(trim.value(QStringLiteral("outMs")).toDouble());

    p->m_cursor = CursorTrack::fromJson(o.value(QStringLiteral("cursor")).toObject());
    p->m_clicks = ClickTrack::fromJson(o.value(QStringLiteral("clicks")).toArray());
    p->m_zoom->fromJson(o.value(QStringLiteral("zoom")).toObject());
    p->m_style->fromJson(o.value(QStringLiteral("style")).toObject());
    p->m_exportSettings = o.value(QStringLiteral("exportSettings")).toObject();

    p->resolveVideo(path);

    // The child fromJson() calls fired changed()/markDirty(); a just-loaded
    // project is by definition clean.
    p->clearDirty();
    return p;
}
