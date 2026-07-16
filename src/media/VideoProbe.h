#pragma once
#include <QObject>
#include <QSize>
#include <QString>

class QProcess;

// Async media probe: shells out to `ffprobe -v quiet -print_format json
// -show_streams -show_format <file>`, parses the first video stream, and emits
// probed(durationMs, fps, size) or failed(reason). Used by import to fill a new
// StudioProject's video metadata.
//
// ffprobe is located once via QStandardPaths::findExecutable; when it is absent
// probe() emits failed() immediately without spawning anything. The QProcess is
// parented to this and torn down in the destructor (and on a re-entrant probe),
// so no orphaned ffprobe survives an early window/app teardown — leak
// discipline per AGENTS.md.
class VideoProbe : public QObject
{
    Q_OBJECT

public:
    explicit VideoProbe(QObject *parent = nullptr);
    ~VideoProbe() override;

    // Absolute path to ffprobe, or empty if not found. Cached after first call.
    static QString ffprobePath();
    static bool available() { return !ffprobePath().isEmpty(); }

    // Kick off an async probe of `file`. A call while a previous probe is still
    // running cancels the previous one. Results arrive via probed()/failed().
    void probe(const QString &file);

signals:
    void probed(qint64 durationMs, double fps, const QSize &size);
    void failed(const QString &reason);

private:
    void handleFinished(int exitCode);
    void killRunning();

    QProcess *m_proc = nullptr;
    QString m_file;
};
