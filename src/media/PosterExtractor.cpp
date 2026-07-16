#include "PosterExtractor.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

PosterExtractor::PosterExtractor(QObject *parent)
    : QObject(parent)
{
}

PosterExtractor::~PosterExtractor()
{
    killRunning();
    delete m_dir; // QTemporaryDir removes its tree on destruction
}

QString PosterExtractor::ffmpegPath()
{
    static const QString path = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    return path;
}

void PosterExtractor::killRunning()
{
    if (!m_proc)
        return;
    m_proc->disconnect(this);
    if (m_proc->state() != QProcess::NotRunning) {
        m_proc->kill();
        m_proc->waitForFinished(1000);
    }
    m_proc->deleteLater();
    m_proc = nullptr;
}

void PosterExtractor::extract(const QString &file, qint64 atMs)
{
    const QString exe = ffmpegPath();
    if (exe.isEmpty()) {
        emit failed(tr("ffmpeg was not found on your system — install ffmpeg."));
        return;
    }
    if (file.isEmpty() || !QFileInfo::exists(file)) {
        emit failed(tr("File does not exist: %1").arg(file));
        return;
    }

    killRunning();

    if (!m_dir) {
        m_dir = new QTemporaryDir();
        if (!m_dir->isValid()) {
            delete m_dir;
            m_dir = nullptr;
            emit failed(tr("Could not create a temporary directory for the poster frame."));
            return;
        }
    }
    m_outPath = QDir(m_dir->path()).filePath(QStringLiteral("poster.png"));

    const double seconds = qMax(qint64(0), atMs) / 1000.0;
    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus) { handleFinished(code); });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!m_proc)
            return;
        const QString reason = m_proc->errorString();
        killRunning();
        emit failed(tr("Could not run ffmpeg: %1").arg(reason));
    });
    // -ss before -i: fast input-side seek. One frame, overwrite, quiet.
    m_proc->start(exe, {QStringLiteral("-nostdin"), QStringLiteral("-loglevel"),
                        QStringLiteral("error"), QStringLiteral("-ss"),
                        QString::number(seconds, 'f', 3), QStringLiteral("-i"), file,
                        QStringLiteral("-frames:v"), QStringLiteral("1"),
                        QStringLiteral("-y"), m_outPath});
}

void PosterExtractor::handleFinished(int exitCode)
{
    if (!m_proc)
        return;
    m_proc->deleteLater();
    m_proc = nullptr;

    if (exitCode == 0 && QFileInfo::exists(m_outPath))
        emit extracted(m_outPath);
    else
        emit failed(tr("ffmpeg could not extract a poster frame."));
}
