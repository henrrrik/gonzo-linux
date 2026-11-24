#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <deque>

class QQuickWindow;

// Kinetic scroll physics for Flickable:
// - Discrete mouse wheel notches drive a Chromium-style cubic-bezier impulse
//   curve that inherits instantaneous velocity on mid-flight retargeting.
// - Precision Wayland touchpads track 1:1 during ScrollBegin/ScrollUpdate,
//   estimate release velocity over a rolling 90 ms sample window, respect
//   native ScrollMomentum without doubling inertia, and smoothly coast with
//   exponential friction decay on ScrollEnd (or idle release).
class KineticScroller : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

public:
    explicit KineticScroller(QObject *parent = nullptr);

    bool isActive() const { return m_mode != Mode::Idle; }

    // Handles a wheel/touchpad event and returns the immediate contentY to apply.
    // `phase` matches Qt::ScrollPhase (0=NoPhase, 1=Begin, 2=Update, 3=End, 4=Momentum).
    // `timestampMs` is optional (negative uses internal monotonic clock; tests pass synthetic times).
    Q_INVOKABLE qreal handleWheel(qreal currentY, qreal maxY, qreal pixelDeltaY,
                                  qreal angleDeltaY, int phase, qreal stepPixels,
                                  qreal devicePixelRatio = 1.0,
                                  qint64 timestampMs = -1);

    // Advances active kinetic animation by `dtSeconds`, returning the snapped contentY.
    Q_INVOKABLE qreal advance(qreal dtSeconds, qreal currentY, qreal maxY,
                              qreal devicePixelRatio = 1.0);

    // Stops any running wheel curve or touchpad coast immediately.
    Q_INVOKABLE void cancel();

    // Installs native QWheelEvent filters on both the Flickable and its hosting
    // QQuickWindow so zero-delta Wayland ScrollEnd (wl_pointer.axis_stop) events
    // are never dropped before reaching the physics engine.
    Q_INVOKABLE void attachFlickable(QObject *flickable, qreal stepPixels = 120.0);

    static qreal snapToPixel(qreal y, qreal dpr);

signals:
    void activeChanged();
    void scrolled();
    void zoomRequested(int direction);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum class Mode { Idle, WheelCurve, TouchpadCoast };

    struct Sample {
        qint64 timeMs = 0;
        qreal delta = 0.0;
    };

    void setMode(Mode mode);
    void attachWindow(QQuickWindow *window);
    void recordTouchSample(qreal delta, qint64 nowMs);
    qreal releaseVelocity(qint64 nowMs) const;
    void finishTouchpadGesture(qreal currentY, qint64 nowMs);
    void handleIdleReleaseTimeout();

    void beginCurve(qreal from, qreal to, qreal duration, qreal initialSlope);
    void retargetCurve(qreal newTarget);
    static qreal wheelDuration(qreal distance);

    qreal curveX(qreal s) const;
    qreal curveY(qreal s) const;
    qreal curveDX(qreal s) const;
    qreal curveDY(qreal s) const;
    qreal solveCurve(qreal x) const;

    Mode m_mode = Mode::Idle;

    // Cubic-bezier wheel curve state.
    qreal m_startY = 0.0;
    qreal m_targetY = 0.0;
    qreal m_duration = 0.2;
    qreal m_elapsed = 0.0;
    qreal m_cx1 = 0.42;
    qreal m_cy1 = 0.0;
    static constexpr qreal kCx2 = 0.58;
    static constexpr qreal kCy2 = 1.0;

    // Touchpad gesture tracking & synthetic coast state.
    bool m_touchpadActive = false;
    bool m_sawCompositorMomentum = false;
    std::deque<Sample> m_samples;
    qreal m_coastVelocity = 0.0;
    qreal m_coastY = 0.0;
    QElapsedTimer m_clock;
    QTimer m_idleReleaseTimer;

    QPointer<QObject> m_flickable;
    QPointer<QQuickWindow> m_window;
    qreal m_stepPixels = 120.0;
    bool m_debugScroll = false;
    qreal m_zoomAccum = 0.0;
    qint64 m_lastZoomMs = -1;
};
