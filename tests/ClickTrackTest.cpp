#include "ClickTrack.h"

#include <QTest>

class ClickTrackTest : public QObject
{
    Q_OBJECT

private slots:
    void appendMonotonicAndRoundtrip();
    void exciseRemovesInsideAndShiftsLater();
};

void ClickTrackTest::appendMonotonicAndRoundtrip()
{
    ClickTrack t;
    QVERIFY(t.append({100, Qt::LeftButton, ClickEvent::Down, 10.5, 20.5}));
    QVERIFY(t.append({140, Qt::LeftButton, ClickEvent::Up, 10.5, 20.5}));
    QVERIFY(t.append({140, Qt::RightButton, ClickEvent::Down, 30.0, 40.0})); // equal ok
    QVERIFY(!t.append({50, Qt::LeftButton, ClickEvent::Down, 0, 0}));        // backwards
    QCOMPARE(t.count(), 3);

    const ClickTrack r = ClickTrack::fromJson(t.toJson());
    QCOMPARE(r.count(), 3);
    for (int i = 0; i < 3; ++i) {
        const ClickEvent &a = t.events().at(i);
        const ClickEvent &b = r.events().at(i);
        QCOMPARE(b.tMs, a.tMs);
        QCOMPARE(b.button, a.button);
        QCOMPARE(int(b.state), int(a.state));
        QCOMPARE(b.x, a.x);
        QCOMPARE(b.y, a.y);
    }
    QCOMPARE(r.events().at(1).state, ClickEvent::Up);
}

void ClickTrackTest::exciseRemovesInsideAndShiftsLater()
{
    ClickTrack t;
    for (qint64 tm : {0, 100, 200, 300})
        t.append({tm, Qt::LeftButton, ClickEvent::Down, double(tm), 0});

    t.excise({{100, 300}});   // drops 100 & 200; 300 kept, shifted to 100

    QCOMPARE(t.count(), 2);
    QCOMPARE(t.events().at(0).tMs, qint64(0));
    QCOMPARE(t.events().at(1).tMs, qint64(100));
    QCOMPARE(int(t.events().at(1).x), 300);   // position untouched, only retimed
}

QTEST_GUILESS_MAIN(ClickTrackTest)
#include "ClickTrackTest.moc"
