#pragma once
#include <QObject>
#include <QString>
#include <qqmlregistration.h>

class StudioProject;
class RenderPipeline;

// The QML-facing export façade. The ExportDialog binds its controls to these
// Q_PROPERTYs, then calls start(project); the heavy lifting lives in
// RenderPipeline (created per run and torn down on finish/cancel). Kept thin on
// purpose — it maps UI choices (format / resolution preset / fps / quality) to a
// RenderPipeline::Settings and relays progress + state back to QML.
class ExportController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString format READ format WRITE setFormat NOTIFY formatChanged)
    // "source" | "1080p" | "720p" | "custom"
    Q_PROPERTY(QString resolution READ resolution WRITE setResolution NOTIFY resolutionChanged)
    Q_PROPERTY(int customWidth READ customWidth WRITE setCustomWidth NOTIFY customWidthChanged)
    Q_PROPERTY(int customHeight READ customHeight WRITE setCustomHeight NOTIFY customHeightChanged)
    // "source" | "30" | "60"
    Q_PROPERTY(QString fpsMode READ fpsMode WRITE setFpsMode NOTIFY fpsModeChanged)
    Q_PROPERTY(int quality READ quality WRITE setQuality NOTIFY qualityChanged) // 0..100
    Q_PROPERTY(QString outputPath READ outputPath WRITE setOutputPath NOTIFY outputPathChanged)

    Q_PROPERTY(double progress READ progress NOTIFY progressChanged) // 0..1
    // GIF only: frames done, palette passes running (bar full, "finishing…").
    Q_PROPERTY(bool finalizing READ finalizing NOTIFY progressChanged)
    Q_PROPERTY(int framesDone READ framesDone NOTIFY progressChanged)
    Q_PROPERTY(int totalFrames READ totalFrames NOTIFY progressChanged)
    Q_PROPERTY(qint64 etaMs READ etaMs NOTIFY progressChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY stateChanged)
    // The default file extension for the current format ("mp4"/"webm").
    Q_PROPERTY(QString extension READ extension NOTIFY formatChanged)

public:
    enum State { Idle, Running, Done, Error, Cancelled };
    Q_ENUM(State)

    explicit ExportController(QObject *parent = nullptr);
    ~ExportController() override;

    State state() const { return m_state; }
    QString format() const { return m_format; }
    QString resolution() const { return m_resolution; }
    int customWidth() const { return m_customWidth; }
    int customHeight() const { return m_customHeight; }
    QString fpsMode() const { return m_fpsMode; }
    int quality() const { return m_quality; }
    QString outputPath() const { return m_outputPath; }
    double progress() const { return m_progress; }
    bool finalizing() const { return m_finalizing; }
    int framesDone() const { return m_framesDone; }
    int totalFrames() const { return m_totalFrames; }
    qint64 etaMs() const { return m_etaMs; }
    QString errorString() const { return m_error; }
    QString extension() const;

    void setFormat(const QString &v);
    void setResolution(const QString &v);
    void setCustomWidth(int v);
    void setCustomHeight(int v);
    void setFpsMode(const QString &v);
    void setQuality(int v);
    void setOutputPath(const QString &v);

    // Build a sensible default output path for `project` given the current format
    // (video basename + "-export" in the video's own folder). QML seeds the
    // destination field with this when the dialog opens.
    Q_INVOKABLE QString suggestedOutputPath(StudioProject *project) const;

    // Split defaults for the folder + filename destination fields: the video's own
    // folder, and its base name (no extension). The dialog seeds its editable
    // filename from the base name + a date and appends the format's extension.
    Q_INVOKABLE QString suggestedDir(StudioProject *project) const;
    Q_INVOKABLE QString suggestedBaseName(StudioProject *project) const;

    // Begin an export of `project` with the current settings. No-op if already
    // Running or if inputs are invalid (emits an Error state with a message).
    Q_INVOKABLE void start(StudioProject *project);
    Q_INVOKABLE void cancel();
    // Return to Idle (clearing progress/error) so a reopened dialog shows the
    // form again. No-op while Running.
    Q_INVOKABLE void reset();

signals:
    void stateChanged();
    void formatChanged();
    void resolutionChanged();
    void customWidthChanged();
    void customHeightChanged();
    void fpsModeChanged();
    void qualityChanged();
    void outputPathChanged();
    void progressChanged();

private:
    void setState(State s);

    State m_state = Idle;
    QString m_format = QStringLiteral("mp4");
    // Defaults MUST match one of ExportDialog's quality presets, or every chip
    // renders unchecked on open and the row reads as "nothing selected".
    // (1080p, 70) is "Balanced". 1080p caps rather than upscales, so a smaller
    // source still exports at its native height.
    QString m_resolution = QStringLiteral("1080p");
    int m_customWidth = 1920;
    int m_customHeight = 1080;
    QString m_fpsMode = QStringLiteral("source");
    int m_quality = 70;
    QString m_outputPath;

    double m_progress = 0.0;
    bool m_finalizing = false;
    int m_framesDone = 0;
    int m_totalFrames = 0;
    qint64 m_etaMs = 0;
    QString m_error;

    RenderPipeline *m_pipeline = nullptr; // per-run, parented to this
};
