#pragma once
#include <QObject>

#include <atomic>

class QThread;

// Global pointer-button capture via libinput's udev backend, on its own thread.
// libinput hands us button + a CLOCK_MONOTONIC timestamp only (no coordinates) —
// StudioRecorder resolves each click's position from the cursor track at the same
// instant, so the two always agree.
//
// It opens devices with a PLAIN open (no EVIOCGRAB): it observes input, never
// steals it. Compile-guarded on HAVE_LIBINPUT — without it the class is inert
// (start() does nothing, no signal ever fires), so the recorder can own and wire
// one unconditionally and only ever start it when InputPermission == Available.
class ClickCapture : public QObject
{
    Q_OBJECT
public:
    explicit ClickCapture(QObject *parent = nullptr);
    ~ClickCapture() override;

    // Opt in to keyboard-TIMING capture (never keycodes) before start(). Default
    // off. When off, no keyActivity() ever fires and no key event is inspected
    // beyond its type — the capture path is byte-identical to click-only. Thread-
    // safe to set before start(); it is read atomically in the poll loop.
    void setCaptureKeys(bool on) { m_captureKeys.store(on); }

    // Idempotent. Spins up the libinput poll thread; a second call while already
    // running is a no-op. Without libinput support it does nothing.
    void start();
    // Idempotent. Wakes the poll thread through the stop eventfd, joins it, and
    // releases the eventfd. Safe to call when not running.
    void stop();
    bool isRunning() const { return m_running; }

signals:
    // A pointer button changed state. tUsec is CLOCK_MONOTONIC microseconds
    // (libinput's own clock — the same domain as frame pts / cursor tMonoNs, in
    // ns). button is a Qt::MouseButton int (LeftButton / RightButton /
    // MiddleButton). Emitted from the poll thread, so the connection is queued.
    void buttonEvent(qint64 tUsec, int button, bool pressed);

    // A key went DOWN at tUsec (CLOCK_MONOTONIC microseconds). PRIVACY: timing
    // only — the keycode is never read, never emitted, never stored. Used solely
    // to know that typing was happening at time T so the camera can hold its
    // zoom. Only emitted while typing capture is enabled (see setCaptureKeys()).
    void keyActivity(qint64 tUsec);

private:
    bool m_running = false;
    std::atomic<bool> m_captureKeys{false};
#ifdef HAVE_LIBINPUT
    void run();               // poll loop; runs on m_thread
    QThread *m_thread = nullptr;
    int m_stopFd = -1;        // eventfd: a write wakes and stops the poll loop
#endif
};
