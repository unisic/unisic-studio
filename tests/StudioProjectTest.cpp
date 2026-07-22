#include "ClickTrack.h"
#include "CursorTrack.h"
#include "StudioProject.h"
#include "StyleModel.h"
#include "ZoomTimeline.h"

#include <QDir>
#include <QFile>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTest>

class StudioProjectTest : public QObject
{
    Q_OBJECT

private slots:
    void saveLoadRoundtripEquality();
    void refusesNewerSchema();
    void relativePathResolutionAndMissingVideo();
    void dirtyFlagLifecycle();
    void premiumDefaultsAndLegacyFallback();

private:
    static void writeFile(const QString &path, const QByteArray &bytes)
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }
};

// gcc/clang can't put a QVERIFY (which returns) inside a non-void static helper
// used in a slot without warnings; keep writeFile above returning void is fine
// because QVERIFY inside a void member is legal.

void StudioProjectTest::saveLoadRoundtripEquality()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QDir dir(tmp.path());
    const QString videoPath = QFileInfo(dir.filePath(QStringLiteral("master.mkv"))).absoluteFilePath();
    writeFile(videoPath, QByteArray("fake-mkv-bytes-0123456789", 25));

    StudioProject p;
    p.setVideoRelPath(QStringLiteral("master.mkv"));
    p.setVideoAbsPath(videoPath);
    p.setDurationMs(65432);
    p.setFps(59.94);
    p.setVideoSize(QSize(2560, 1440));
    p.setVideoHash(StudioProject::computeVideoHash(videoPath));
    p.setCompositor(QStringLiteral("kwin_wayland"));
    p.setCursorMode(QStringLiteral("metadata"));
    p.setT0MonoNs(Q_INT64_C(1234567890123));
    p.setHadClickCapture(true);
    p.setTrimInMs(500);
    p.setTrimOutMs(60000);
    p.setAudioMuted(true);
    p.setAudioVolume(1.5);                 // out of range → clamped to 1.0
    QCOMPARE(p.audioVolume(), 1.0);
    p.setAudioVolume(0.35);

    CursorTrack cur;
    cur.append({0, 100, 200, true, 0});
    cur.append({16, 110, 205, true, 0});
    cur.append({33, 120, 210, false, 1});
    cur.setShapes({{0, 1, 1, QByteArray("A")}, {1, 2, 2, QByteArray("BB")}});
    p.setCursorTrack(cur);

    ClickTrack clk;
    clk.append({16, Qt::LeftButton, ClickEvent::Down, 110, 205});
    clk.append({40, Qt::LeftButton, ClickEvent::Up, 122, 211});
    p.setClickTrack(clk);

    p.setTypingBursts({{2000, 4200}, {9000, 11500}});

    ZoomTimeline::Keyframe k;
    k.tMs = 1000;
    k.rect = QRectF(0.2, 0.2, 0.6, 0.6);
    k.source = ZoomTimeline::Auto;
    k.locked = true;
    p.zoom()->addKeyframe(k);
    p.zoom()->setAutoParams(QJsonObject{{QStringLiteral("strength"), 1.25},
                                        {QStringLiteral("zoomIntensity"), 0.61},
                                        {QStringLiteral("motionSmoothness"), 0.84}});

    p.style()->setBackgroundType(QStringLiteral("gradient"));
    p.style()->setPaddingPct(14.5);
    p.style()->setCornerRadius(20);
    p.style()->setClickRipple(false);
    p.style()->setRippleColor(QColor(0x11, 0x22, 0x33, 0x80));

    p.setExportSettings(QJsonObject{{QStringLiteral("codec"), QStringLiteral("h264")},
                                    {QStringLiteral("crf"), 18}});

    const QString projPath = dir.filePath(QStringLiteral("p.unisicstudio"));
    QString err;
    QVERIFY2(p.save(projPath, &err), qPrintable(err));

    QString loadErr;
    QScopedPointer<StudioProject> r(StudioProject::load(projPath, &loadErr));
    QVERIFY2(!r.isNull(), qPrintable(loadErr));

    // Scalars.
    QCOMPARE(r->videoRelPath(), p.videoRelPath());
    QCOMPARE(r->videoAbsPath(), p.videoAbsPath());
    QCOMPARE(r->durationMs(), p.durationMs());
    QCOMPARE(r->fps(), p.fps());
    QCOMPARE(r->videoSize(), p.videoSize());
    QCOMPARE(r->videoHash(), p.videoHash());
    QCOMPARE(r->compositor(), p.compositor());
    QCOMPARE(r->cursorMode(), p.cursorMode());
    QCOMPARE(r->t0MonoNs(), p.t0MonoNs());
    QCOMPARE(r->hadClickCapture(), p.hadClickCapture());
    QCOMPARE(r->typingBursts(), p.typingBursts());
    QCOMPARE(r->trimInMs(), p.trimInMs());
    QCOMPARE(r->trimOutMs(), p.trimOutMs());
    QCOMPARE(r->audioMuted(), true);
    QCOMPARE(r->audioVolume(), 0.35);

    // Cursor track.
    QCOMPARE(r->cursorTrack().count(), 3);
    QCOMPARE(int(r->cursorTrack().samples().at(2).x), 120);
    QCOMPARE(r->cursorTrack().samples().at(2).visible, false);
    QCOMPARE(r->cursorTrack().samples().at(2).shapeId, 1);
    QCOMPARE(r->cursorTrack().shapes().size(), 2);
    QCOMPARE(r->cursorTrack().shapes().at(1).png, QByteArray("BB"));

    // Click track.
    QCOMPARE(r->clickTrack().count(), 2);
    QCOMPARE(r->clickTrack().events().at(1).state, ClickEvent::Up);
    QCOMPARE(r->clickTrack().events().at(1).tMs, qint64(40));

    // Zoom.
    QCOMPARE(r->zoom()->rowCount(), 1);
    QCOMPARE(r->zoom()->keyframes().at(0).rect, QRectF(0.2, 0.2, 0.6, 0.6));
    QCOMPARE(r->zoom()->keyframes().at(0).locked, true);
    QCOMPARE(r->zoom()->autoParams().value(QStringLiteral("strength")).toDouble(), 1.25);
    QCOMPARE(r->zoom()->zoomIntensity(), 0.61);
    QCOMPARE(r->zoom()->motionSmoothness(), 0.84);

    // Style (incl. alpha on the ripple colour).
    QCOMPARE(r->style()->backgroundType(), QStringLiteral("gradient"));
    QCOMPARE(r->style()->paddingPct(), 14.5);
    QCOMPARE(r->style()->cornerRadius(), 20);
    QCOMPARE(r->style()->clickRipple(), false);
    QCOMPARE(r->style()->rippleColor(), QColor(0x11, 0x22, 0x33, 0x80));

    // Export settings passthrough.
    QCOMPARE(r->exportSettings().value(QStringLiteral("crf")).toInt(), 18);

    // Video resolved via the relative path; nothing missing.
    QCOMPARE(r->videoResolved(), videoPath);
    QCOMPARE(r->videoMissing(), false);

    // Additive clip audio: a project saved before the "audio" object existed
    // loads with the defaults (unmuted, full volume) — never zeroed.
    const QString legacyPath = dir.filePath(QStringLiteral("legacy.unisicstudio"));
    writeFile(legacyPath, QByteArray("{\"schemaVersion\":1,\"video\":{}}"));
    QScopedPointer<StudioProject> legacy(StudioProject::load(legacyPath));
    QVERIFY(!legacy.isNull());
    QCOMPARE(legacy->audioMuted(), false);
    QCOMPARE(legacy->audioVolume(), 1.0);
}

void StudioProjectTest::refusesNewerSchema()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("future.unisicstudio"));
    writeFile(path, QByteArray("{\"schemaVersion\":2,\"video\":{}}"));

    QString err;
    QScopedPointer<StudioProject> r(StudioProject::load(path, &err));
    QVERIFY(r.isNull());
    QVERIFY(!err.isEmpty());
    QVERIFY2(err.contains(QStringLiteral("newer")), qPrintable(err));
}

void StudioProjectTest::relativePathResolutionAndMissingVideo()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QDir root(tmp.path());

    // Author a project in dirA next to its video, recording an absPath in dirA.
    QVERIFY(root.mkpath(QStringLiteral("a")));
    const QDir a(root.filePath(QStringLiteral("a")));
    const QString videoA = QFileInfo(a.filePath(QStringLiteral("master.mkv"))).absoluteFilePath();
    writeFile(videoA, QByteArray("vid"));

    StudioProject p;
    p.setVideoRelPath(QStringLiteral("master.mkv"));
    p.setVideoAbsPath(videoA);
    const QString projA = a.filePath(QStringLiteral("p.unisicstudio"));
    QVERIFY(p.save(projA));

    // Move the bundle (project + video) to dirB. The recorded absPath still
    // points at dirA, but the RELATIVE path must win and resolve inside dirB.
    QVERIFY(root.mkpath(QStringLiteral("b")));
    const QDir b(root.filePath(QStringLiteral("b")));
    const QString videoB = QFileInfo(b.filePath(QStringLiteral("master.mkv"))).absoluteFilePath();
    const QString projB = b.filePath(QStringLiteral("p.unisicstudio"));
    QVERIFY(QFile::copy(videoA, videoB));
    QVERIFY(QFile::copy(projA, projB));

    QScopedPointer<StudioProject> moved(StudioProject::load(projB));
    QVERIFY(!moved.isNull());
    QCOMPARE(moved->videoMissing(), false);
    QCOMPARE(moved->videoResolved(), videoB);   // rel-first, not the dirA absPath

    // Now a project whose video is gone from both paths → flagged missing.
    StudioProject gone;
    gone.setVideoRelPath(QStringLiteral("nope.mkv"));
    gone.setVideoAbsPath(root.filePath(QStringLiteral("nowhere/xyz.mkv")));
    const QString projGone = a.filePath(QStringLiteral("gone.unisicstudio"));
    QVERIFY(gone.save(projGone));

    QScopedPointer<StudioProject> missing(StudioProject::load(projGone));
    QVERIFY(!missing.isNull());
    QCOMPARE(missing->videoMissing(), true);
    QVERIFY(missing->videoResolved().isEmpty());
}

void StudioProjectTest::dirtyFlagLifecycle()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = QDir(tmp.path()).filePath(QStringLiteral("d.unisicstudio"));

    StudioProject p;
    QCOMPARE(p.dirty(), false);            // fresh document is clean

    p.style()->setPaddingPct(20);          // a child-model mutation dirties it
    QCOMPARE(p.dirty(), true);

    QVERIFY(p.save(path));
    QCOMPARE(p.dirty(), false);            // save clears

    ZoomTimeline::Keyframe k;
    k.tMs = 500;
    p.zoom()->addKeyframe(k);              // zoom mutation dirties again
    QCOMPARE(p.dirty(), true);

    p.setDurationMs(9999);                 // scalar setter dirties too
    QVERIFY(p.dirty());

    QScopedPointer<StudioProject> r(StudioProject::load(path));
    QVERIFY(!r.isNull());
    QCOMPARE(r->dirty(), false);           // a just-loaded document is clean
}

void StudioProjectTest::premiumDefaultsAndLegacyFallback()
{
    StyleModel style;
    QCOMPARE(style.backgroundType(), QStringLiteral("gradient"));
    QCOMPARE(style.gradientStart(), QColor(0x17, 0x15, 0x3B));
    QCOMPARE(style.gradientEnd(), QColor(0x2E, 0x23, 0x6C));
    QCOMPARE(style.paddingPct(), 10.5);
    QCOMPARE(style.cornerRadius(), 16);
    QCOMPARE(style.shadowBlur(), 56);
    QCOMPARE(style.shadowOpacity(), 0.30);
    QCOMPARE(style.shadowOffsetY(), 12);
    QCOMPARE(style.fillMode(), QStringLiteral("fill"));
    QCOMPARE(style.cursorScale(), 1.65);
    QCOMPARE(style.clickRipple(), true);

    // Missing additive keys in an old schema-1 project preserve polished
    // compiled defaults rather than zeroing new settings.
    style.fromJson(QJsonObject{{QStringLiteral("frameStyle"), QStringLiteral("minimal")}});
    QCOMPARE(style.backgroundType(), QStringLiteral("gradient"));
    QCOMPARE(style.paddingPct(), 10.5);
    QCOMPARE(style.cursorScale(), 1.65);
}

QTEST_GUILESS_MAIN(StudioProjectTest)
#include "StudioProjectTest.moc"
