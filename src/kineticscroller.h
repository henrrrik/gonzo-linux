#pragma once

#include <QElapsedTimer>
#include <QObject>

// Kinetic scroll physics for Flickable:
// - Discrete mouse wheel notches drive a Chromium-style cubic-bezier impulse
//   curve that inherits instantaneous velocity on mid-flight retargeting.
// - Precision Wayland touchpads track 1:1 during ScrollBegin/ScrollUpdate,
//   respect native ScrollMomentum without doubling inertia, and smoothly coast
//   with exponential friction decay on ScrollEnd when the compositor does not
//   emit momentum events.
class KineticScroller : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)

public:
    explicit KineticScroller(QObject *parent = nullptr);

    bool isActive() const { return m_mode != Mode::Idle; }

    // Handles a wheel/touchpad event and returns the immediate contentY to apply.
    // `phase` matches Qt::ScrollPhase (0=NoPhase, 1=Begin, 2=Update, 3=End, 4=Momentum).
    Q_INVOKABLE qreal handleWheel(qreal currentY, qreal maxY, qreal pixelDeltaY,
                                  qreal angleDeltaY, int phase, qreal stepPixels,
                                  qreal devicePixelRatio = 1.0);

    // Advances active kinetic animation by `dtSeconds`, returning the snapped contentY.
    Q_INVOKABLE qreal advance(qreal dtSeconds, qreal currentY, qreal maxY,
                              qreal devicePixelRatio = 1.0);

    // Stops any running wheel curve or touchpad coast immediately.
    Q_INVOKABLE void cancel();

    // Installs a native QWheelEvent filter on a QML Flickable so true Wayland
    // scroll phases (ScrollBegin/Update/End/Momentum) reach the physics engine.
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

    void setMode(Mode mode);
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
    qreal m_touchVelocity = 0.0;
    qreal m_coastVelocity = 0.0;
    qreal m_coastY = 0.0;
    QElapsedTimer m_gestureClock;
    QObject *m_flickable = nullptr;
    qreal m_stepPixels = 120.0;
};
