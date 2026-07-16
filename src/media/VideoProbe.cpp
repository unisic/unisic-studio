#include "VideoProbe.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

VideoProbe::VideoProbe(QObject *parent)
    : QObject(parent)
{
}

VideoProbe::~VideoProbe()
{
    killRunning();
}

QString VideoProbe::ffprobePath()
{
    // Resolve once: PATH + the standard exec locations. Empty when not installed.
    static const QString path = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    return path;
}

void VideoProbe::killRunning()
{
    if (!m_proc)
        return;
    // Detach signals so a kill-triggered finished() doesn't re-enter, then stop
    // the child cleanly — no orphaned ffprobe outlives us.
    m_proc->disconnect(this);
    if (m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(1000);
    }
    m_proc->deleteLater();
    m_proc = nullptr;
}

void VideoProbe::probe(const QString &file)
{
    const QString exe = ffprobePath();
    if (exe.isEmpty()) {
        emit failed(tr("ffprobe was not found on your system — install ffmpeg."));
        return;
    }
    if (file.isEmpty() || !QFileInfo::exists(file)) {
        emit failed(tr("File does not exist: %1").arg(file));
        return;
    }

    killRunning(); // a re-entrant probe supersedes the previous one

    m_file = file;
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_proc, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus) { handleFinished(code); });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!m_proc)
            return;
        const QString reason = m_proc->errorString();
        killRunning();
        emit failed(tr("Could not run ffprobe: %1").arg(reason));
    });
    m_proc->start(exe, {QStringLiteral("-v"), QStringLiteral("quiet"),
                        QStringLiteral("-print_format"), QStringLiteral("json"),
                        QStringLiteral("-show_streams"), QStringLiteral("-show_format"),
                        file});
}

void VideoProbe::handleFinished(int exitCode)
{
    if (!m_proc)
        return;
    const QByteArray out = m_proc->readAllStandardOutput();
    m_proc->deleteLater();
    m_proc = nullptr;

    if (exitCode != 0) {
        emit failed(tr("ffprobe could not read %1").arg(QFileInfo(m_file).fileName()));
        return;
    }

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(out, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        emit failed(tr("ffprobe output for %1 could not be parsed")
                        .arg(QFileInfo(m_file).fileName()));
        return;
    }
    const QJsonObject root = doc.object();

    // First video stream carries the geometry + frame rate.
    QJsonObject video;
    const QJsonArray streams = root.value(QStringLiteral("streams")).toArray();
    for (const QJsonValue &s : streams) {
        const QJsonObject o = s.toObject();
        if (o.value(QStringLiteral("codec_type")).toString() == QStringLiteral("video")) {
            video = o;
            break;
        }
    }
    if (video.isEmpty()) {
        emit failed(tr("%1 has no video stream").arg(QFileInfo(m_file).fileName()));
        return;
    }

    const QSize size(video.value(QStringLiteral("width")).toInt(),
                     video.value(QStringLiteral("height")).toInt());

    // Frame rate is a "num/den" rational; prefer avg over the (often ceil'd)
    // r_frame_rate. Guard a zero/absent denominator.
    auto parseRate = [](const QString &r) -> double {
        const int slash = r.indexOf(QLatin1Char('/'));
        if (slash < 0)
            return r.toDouble();
        const double num = r.left(slash).toDouble();
        const double den = r.mid(slash + 1).toDouble();
        return den > 0.0 ? num / den : 0.0;
    };
    double fps = parseRate(video.value(QStringLiteral("avg_frame_rate")).toString());
    if (fps <= 0.0)
        fps = parseRate(video.value(QStringLiteral("r_frame_rate")).toString());

    // Duration in seconds lives in format (container) first, then the stream.
    const QJsonObject format = root.value(QStringLiteral("format")).toObject();
    double seconds = format.value(QStringLiteral("duration")).toString().toDouble();
    if (seconds <= 0.0)
        seconds = video.value(QStringLiteral("duration")).toString().toDouble();
    const qint64 durationMs = qint64(seconds * 1000.0 + 0.5);

    emit probed(durationMs, fps, size);
}
