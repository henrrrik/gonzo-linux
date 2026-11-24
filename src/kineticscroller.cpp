#include "kineticscroller.h"

#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>
#include <Qt>
#include <algorithm>
#include <cmath>

namespace {
constexpr qreal kCoastDamping = 5.5;       // Exponential decay rate (1/s)
constexpr qreal kMinCoastVelocity = 25.0;  // px/s threshold to settle cleanly
constexpr qreal kMaxCoastVelocity = 8000.0;
constexpr qreal kStartCoastThreshold = 60.0;
} // namespace

KineticScroller::KineticScroller(QObject *parent) : QObject(parent) {}

qreal KineticScroller::snapToPixel(qreal y, qreal dpr) {
    const qreal scale = dpr > 0.0 ? dpr : 1.0;
    return std::round(y * scale) / scale;
}

void KineticScroller::setMode(Mode mode) {
    if (m_mode == mode)
        return;
    const bool wasActive = isActive();
    m_mode = mode;
    if (isActive() != wasActive)
        emit activeChanged();
}

void KineticScroller::cancel() {
    m_touchpadActive = false;
    m_touchVelocity = 0.0;
    m_coastVelocity = 0.0;
    setMode(Mode::Idle);
}

void KineticScroller::attachFlickable(QObject *flickable, qreal stepPixels) {
    if (m_flickable)
        m_flickable->removeEventFilter(this);
    m_flickable = flickable;
    m_stepPixels = stepPixels > 0.0 ? stepPixels : 120.0;
    if (m_flickable)
        m_flickable->installEventFilter(this);
}

bool KineticScroller::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_flickable || event->type() != QEvent::Wheel)
        return QObject::eventFilter(watched, event);

    auto *wheel = static_cast<QWheelEvent *>(event);
    if (wheel->modifiers() & Qt::ControlModifier) {
        const qreal delta = wheel->angleDelta().y() != 0
            ? wheel->angleDelta().y()
            : wheel->pixelDelta().y();
        if (delta != 0.0)
            emit zoomRequested(delta > 0.0 ? 1 : -1);
        wheel->accept();
        return true;
    }

    const qreal currentY = m_flickable->property("contentY").toReal();
    const qreal contentH = m_flickable->property("contentHeight").toReal();
    const qreal viewH = m_flickable->property("height").toReal();
    const qreal maxY = std::max<qreal>(0.0, contentH - viewH);

    qreal dpr = 1.0;
    if (auto *item = qobject_cast<QQuickItem *>(m_flickable)) {
        if (item->window())
            dpr = item->window()->effectiveDevicePixelRatio();
    }

    const qreal nextY = handleWheel(currentY, maxY,
                                    wheel->pixelDelta().y(),
                                    wheel->angleDelta().y(),
                                    static_cast<int>(wheel->phase()),
                                    m_stepPixels,
                                    dpr);
    if (nextY != currentY)
        m_flickable->setProperty("contentY", nextY);
    emit scrolled();
    wheel->accept();
    return true;
}

qreal KineticScroller::wheelDuration(qreal distance) {
    // Shorter duration for rapid/distant scrolling so sustained wheel flicks
    // stay crisp and never feel sluggish.
    const qreal d = std::abs(distance);
    return std::clamp(0.12 + 0.08 * std::exp(-d / 600.0), 0.10, 0.22);
}

qreal KineticScroller::curveX(qreal s) const {
    return 3.0 * s * (1.0 - s) * ((1.0 - s) * m_cx1 + s * kCx2) + s * s * s;
}

qreal KineticScroller::curveY(qreal s) const {
    return 3.0 * s * (1.0 - s) * ((1.0 - s) * m_cy1 + s * kCy2) + s * s * s;
}

qreal KineticScroller::curveDX(qreal s) const {
    return 3.0 * (1.0 - s) * (1.0 - s) * m_cx1
         + 6.0 * (1.0 - s) * s * (kCx2 - m_cx1)
         + 3.0 * s * s * (1.0 - kCx2);
}

qreal KineticScroller::curveDY(qreal s) const {
    return 3.0 * (1.0 - s) * (1.0 - s) * m_cy1
         + 6.0 * (1.0 - s) * s * (kCy2 - m_cy1)
         + 3.0 * s * s * (1.0 - kCy2);
}

qreal KineticScroller::solveCurve(qreal x) const {
    x = std::clamp(x, 0.0, 1.0);
    qreal s = x;
    for (int i = 0; i < 6; ++i) {
        const qreal err = curveX(s) - x;
        const qreal dx = curveDX(s);
        if (std::abs(err) < 1e-5 || std::abs(dx) < 1e-6)
            break;
        s = std::clamp(s - err / dx, 0.0, 1.0);
    }
    return s;
}

void KineticScroller::beginCurve(qreal from, qreal to, qreal duration, qreal initialSlope) {
    m_startY = from;
    m_targetY = to;
    m_duration = std::max<qreal>(0.016, duration);
    m_elapsed = 0.0;
    m_cx1 = 0.42;
    m_cy1 = 0.42 * std::clamp(initialSlope, -1000.0, 1000.0);
    setMode(Mode::WheelCurve);
}

void KineticScroller::retargetCurve(qreal newTarget) {
    const qreal progress = std::clamp(m_elapsed / std::max<qreal>(1e-6, m_duration), 0.0, 1.0);
    const qreal s = solveCurve(progress);
    const qreal pos = m_startY + (m_targetY - m_startY) * curveY(s);
    const qreal delta = newTarget - pos;
    if (std::abs(delta) < 0.5) {
        m_targetY = newTarget;
        setMode(Mode::Idle);
        return;
    }

    const qreal velocity = (curveDY(s) / std::max<qreal>(1e-6, curveDX(s)))
                         * (m_targetY - m_startY) / m_duration;
    qreal dur = wheelDuration(delta);
    // When already moving faster in the same direction, bound duration by time
    // to target at current velocity so sustained flicks keep picking up speed.
    if (velocity != 0.0 && (delta / velocity) > 0.0)
        dur = std::min(dur, (delta / velocity) * 2.5);
    beginCurve(pos, newTarget, dur, velocity * dur / delta);
}

qreal KineticScroller::handleWheel(qreal currentY, qreal maxY, qreal pixelDeltaY,
                                   qreal angleDeltaY, int phase, qreal stepPixels,
                                   qreal devicePixelRatio) {
    maxY = std::max<qreal>(0.0, maxY);
    currentY = std::clamp(currentY, 0.0, maxY);

    if (phase == Qt::ScrollBegin) {
        cancel();
        m_touchpadActive = true;
        m_sawCompositorMomentum = false;
        m_touchVelocity = 0.0;
        m_gestureClock.restart();
        return currentY;
    }

    if (phase == Qt::ScrollMomentum) {
        // Compositor/libinput already delivers momentum events: apply 1:1 and
        // never stack synthetic coasting on top.
        m_sawCompositorMomentum = true;
        m_touchpadActive = false;
        m_touchVelocity = 0.0;
        setMode(Mode::Idle);
        const qreal delta = pixelDeltaY != 0.0 ? -pixelDeltaY : -(angleDeltaY / 120.0) * stepPixels;
        return snapToPixel(std::clamp(currentY + delta, 0.0, maxY), devicePixelRatio);
    }

    if (phase == Qt::ScrollEnd) {
        const bool shouldCoast = m_touchpadActive && !m_sawCompositorMomentum
                              && std::abs(m_touchVelocity) >= kStartCoastThreshold;
        m_touchpadActive = false;
        if (shouldCoast) {
            m_coastVelocity = std::clamp(m_touchVelocity, -kMaxCoastVelocity, kMaxCoastVelocity);
            m_coastY = currentY;
            setMode(Mode::TouchpadCoast);
        } else {
            m_touchVelocity = 0.0;
            setMode(Mode::Idle);
        }
        return currentY;
    }

    // Precision touchpad / high-resolution pixel delta tracking.
    if (pixelDeltaY != 0.0) {
        setMode(Mode::Idle);
        m_sawCompositorMomentum = false;
        const qreal delta = -pixelDeltaY;
        const qreal nextY = std::clamp(currentY + delta, 0.0, maxY);

        if (m_gestureClock.isValid()) {
            const qint64 ms = m_gestureClock.restart();
            if (ms > 120) {
                m_touchVelocity = 0.0;
            } else {
                const qreal dt = std::clamp(ms / 1000.0, 0.004, 0.08);
                const qreal instVel = delta / dt;
                // Exponential smoothing suppresses jitter between libinput frames.
                m_touchVelocity = m_touchpadActive
                    ? (0.65 * instVel + 0.35 * m_touchVelocity)
                    : instVel;
            }
        } else {
            m_gestureClock.restart();
            m_touchVelocity = delta / 0.016;
        }
        m_touchpadActive = true;
        return snapToPixel(nextY, devicePixelRatio);
    }

    // Discrete mouse wheel notch (angleDeltaY only).
    if (angleDeltaY != 0.0) {
        m_touchpadActive = false;
        m_touchVelocity = 0.0;
        const qreal step = stepPixels > 0.0 ? stepPixels : 120.0;
        const qreal delta = -(angleDeltaY / 120.0) * step;

        if (m_mode == Mode::WheelCurve) {
            const qreal existingDir = m_targetY - m_startY;
            const bool reversing = (existingDir > 0.0 && delta < 0.0)
                                || (existingDir < 0.0 && delta > 0.0);
            const qreal baseTarget = reversing ? currentY : m_targetY;
            const qreal newTarget = std::clamp(baseTarget + delta, 0.0, maxY);
            if (reversing)
                beginCurve(currentY, newTarget, wheelDuration(newTarget - currentY), 0.0);
            else
                retargetCurve(newTarget);
        } else {
            const qreal newTarget = std::clamp(currentY + delta, 0.0, maxY);
            if (std::abs(newTarget - currentY) >= 0.5)
                beginCurve(currentY, newTarget, wheelDuration(newTarget - currentY), 0.0);
            else
                return snapToPixel(newTarget, devicePixelRatio);
        }
    }

    return currentY;
}

qreal KineticScroller::advance(qreal dtSeconds, qreal currentY, qreal maxY, qreal devicePixelRatio) {
    maxY = std::max<qreal>(0.0, maxY);
    if (m_mode == Mode::Idle)
        return std::clamp(currentY, 0.0, maxY);

    const qreal dt = std::clamp(dtSeconds, 0.001, 0.05);

    if (m_mode == Mode::WheelCurve) {
        m_elapsed += dt;
        const qreal x = m_elapsed / m_duration;
        if (x >= 1.0) {
            const qreal finalY = snapToPixel(std::clamp(m_targetY, 0.0, maxY), devicePixelRatio);
            setMode(Mode::Idle);
            return finalY;
        }
        const qreal s = solveCurve(x);
        const qreal pos = m_startY + (m_targetY - m_startY) * curveY(s);
        const qreal clamped = std::clamp(pos, 0.0, maxY);
        if ((pos <= 0.0 && m_targetY <= 0.0) || (pos >= maxY && m_targetY >= maxY))
            setMode(Mode::Idle);
        return snapToPixel(clamped, devicePixelRatio);
    }

    // Mode::TouchpadCoast: exact analytical integral of v(t) = v0 * exp(-k * t).
    const qreal decay = std::exp(-kCoastDamping * dt);
    const qreal displacement = m_coastVelocity * (1.0 - decay) / kCoastDamping;
    m_coastY += displacement;
    m_coastVelocity *= decay;

    if (m_coastY <= 0.0) {
        m_coastY = 0.0;
        m_coastVelocity = 0.0;
        setMode(Mode::Idle);
        return 0.0;
    }
    if (m_coastY >= maxY) {
        m_coastY = maxY;
        m_coastVelocity = 0.0;
        setMode(Mode::Idle);
        return snapToPixel(maxY, devicePixelRatio);
    }
    if (std::abs(m_coastVelocity) < kMinCoastVelocity) {
        m_coastVelocity = 0.0;
        setMode(Mode::Idle);
    }
    return snapToPixel(m_coastY, devicePixelRatio);
}
