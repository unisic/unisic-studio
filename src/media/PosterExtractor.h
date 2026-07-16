#pragma once
#include <QObject>
#include <QString>

class QProcess;
class QTemporaryDir;

// Extracts a single poster frame from a video via
// `ffmpeg -ss <ms> -i <file> -frames:v 1 -y <out.png>`. Used ONLY on the
// degraded editor path where the QtMultimedia QML plugin is absent
// (Studio.capVideoPlayback == false), so the canvas can still show a still
// instead of an empty rectangle.
//
// The PNG is written into a private QTemporaryDir owned by this object, so it is
// swept when the extractor dies with its editor window — no stray temp files.
// ffmpeg is located via QStandardPaths; when absent extract() emits failed().
// The QProcess is parented + killed on teardown (leak discipline).
class PosterExtractor : public QObject
{
    Q_OBJECT

public:
    explicit PosterExtractor(QObject *parent = nullptr);
    ~PosterExtractor() override;

    static QString ffmpegPath();
    static bool available() { return !ffmpegPath().isEmpty(); }

    // Extract a frame at `atMs` (clamped to >= 0). Result via extracted()/failed().
    void extract(const QString &file, qint64 atMs = 0);

signals:
    // Absolute path to the written PNG (valid while this object is alive).
    void extracted(const QString &pngPath);
    void failed(const QString &reason);

private:
    void handleFinished(int exitCode);
    void killRunning();

    QProcess *m_proc = nullptr;
    QTemporaryDir *m_dir = nullptr;
    QString m_outPath;
};
