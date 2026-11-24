#include "kineticscroller.h"

#include <QDebug>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>
#include <Qt>
#include <algorithm>
#include <cmath>

namespace {
// Tuned to match macOS NSScrollView / GTK4 kinetic glide feel.
constexpr qreal kCoastDamping = 2.6;          // Exponential decay rate (1/s)
constexpr qreal kMinCoastVelocity = 10.0;     // px/s threshold to settle cleanly
constexpr qreal kMaxCoastVelocity = 10000.0;  // px/s cap for fast multi-page flicks
constexpr qreal kStartCoastThreshold = 35.0;  // px/s minimum release velocity to coast
constexpr qint64 kVelocityWindowMs = 95;      // Rolling window for flick release speed
constexpr qint64 kStopBeforeLiftMs = 110;     // Pause before lift cancels coast
constexpr qreal kTouchAngleToPixels = 0.5;    // Fallback scale when touchpad reports angleDelta only
} // namespace

KineticScroller::KineticScroller(QObject *parent)
    : QObject(parent),
      m_debugScroll(!qEnvironmentVariableIsEmpty("GONZO_DEBUG_SCROLL")) {
    m_clock.start();
    m_idleReleaseTimer.setSingleShot(true);
    connect(&m_idleReleaseTimer, &QTimer::timeout, this, &KineticScroller::handleIdleReleaseTimeout);
}

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
    m_idleReleaseTimer.stop();
    m_touchpadActive = false;
    m_samples.clear();
    m_coastVelocity = 0.0;
    setMode(Mode::Idle);
}

void KineticScroller::attachWindow(QQuickWindow *window) {
    if (m_window == window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);
    m_window = window;
    if (m_window)
        m_window->installEventFilter(this);
}

void KineticScroller::attachFlickable(QObject *flickable, qreal stepPixels) {
    if (m_flickable) {
        m_flickable->removeEventFilter(this);
        disconnect(m_flickable, nullptr, this, nullptr);
    }
    attachWindow(nullptr);
    m_flickable = flickable;
    m_stepPixels = stepPixels > 0.0 ? stepPixels : 120.0;
    if (m_flickable) {
        m_flickable->installEventFilter(this);
        if (auto *item = qobject_cast<QQuickItem *>(m_flickable)) {
            attachWindow(item->window());
            connect(item, &QQuickItem::windowChanged, this, &KineticScroller::attachWindow);
        }
    }
}

void KineticScroller::recordTouchSample(qreal delta, qint64 nowMs) {
    // Discard stale samples from a prior gesture or a pause-on-pad.
    while (!m_samples.empty() && nowMs - m_samples.front().timeMs > kVelocityWindowMs)
        m_samples.pop_front();
    // Direction reversal starts a fresh velocity window.
    if (!m_samples.empty() && ((m_samples.back().delta > 0.0 && delta < 0.0)
                               || (m_samples.back().delta < 0.0 && delta > 0.0))) {
        m_samples.clear();
    }
    m_samples.push_back(Sample{nowMs, delta});
    while (m_samples.size() > 16)
        m_samples.pop_front();
}

qreal KineticScroller::releaseVelocity(qint64 nowMs) const {
    if (m_samples.empty())
        return 0.0;
    // If the user held their fingers still before lifting, do not coast.
    if (nowMs - m_samples.back().timeMs > kStopBeforeLiftMs)
        return 0.0;

    // Aggregate recent displacement over the rolling window so a single tiny
    // lift-off frame cannot crush a genuine flick's velocity.
    qreal totalDelta = 0.0;
    qint64 firstTime = m_samples.back().timeMs;
    qint64 lastTime = m_samples.back().timeMs;
    int count = 0;

    for (auto it = m_samples.rbegin(); it != m_samples.rend(); ++it) {
        if (nowMs - it->timeMs > kVelocityWindowMs)
            break;
        totalDelta += it->delta;
        firstTime = it->timeMs;
        ++count;
    }

    if (count == 0)
        return 0.0;

    // For a single quick flick sample, assume one 16 ms frame interval.
    const qreal spanSec = count >= 2
        ? std::max<qreal>(0.012, (lastTime - firstTime) / 1000.0)
        : 0.016;
    return totalDelta / spanSec;
}

void KineticScroller::finishTouchpadGesture(qreal currentY, qint64 nowMs) {
    m_idleReleaseTimer.stop();
    if (!m_touchpadActive)
        return;

    const qreal vel = releaseVelocity(nowMs);
    m_touchpadActive = false;
    m_samples.clear();

    if (!m_sawCompositorMomentum && std::abs(vel) >= kStartCoastThreshold) {
        m_coastVelocity = std::clamp(vel, -kMaxCoastVelocity, kMaxCoastVelocity);
        m_coastY = currentY;
        if (m_debugScroll)
            qWarning() << "[gonzo scroll] coast start v=" << m_coastVelocity << "y=" << currentY;
        setMode(Mode::TouchpadCoast);
    } else {
        m_coastVelocity = 0.0;
        setMode(Mode::Idle);
    }
}

void KineticScroller::handleIdleReleaseTimeout() {
    if (!m_touchpadActive || !m_flickable)
        return;
    const qreal currentY = m_flickable->property("contentY").toReal();
    finishTouchpadGesture(currentY, m_clock.elapsed());
}

bool KineticScroller::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_window && event->type() == QEvent::MouseButtonPress && isActive()) {
        cancel();
        return false;
    }
    if (event->type() != QEvent::Wheel)
        return QObject::eventFilter(watched, event);

    auto *wheel = static_cast<QWheelEvent *>(event);

    // Wayland's wl_pointer.axis_stop arrives as a zero-delta Qt::ScrollEnd
    // wheel event, which Qt Quick's delivery agent can drop before child items
    // see it. Intercept it on the hosting QQuickWindow to trigger coasting.
    if (watched == m_window) {
        if (m_touchpadActive && wheel->phase() == Qt::ScrollEnd && m_flickable) {
            if (m_debugScroll)
                qWarning() << "[gonzo scroll] window caught ScrollEnd";
            const qreal currentY = m_flickable->property("contentY").toReal();
            finishTouchpadGesture(currentY, m_clock.elapsed());
            emit scrolled();
        }
        return false;
    }

    if (watched != m_flickable)
        return QObject::eventFilter(watched, event);

    if (wheel->modifiers() & Qt::ControlModifier) {
        const qint64 nowMs = m_clock.elapsed();
        if (wheel->phase() == Qt::ScrollBegin || wheel->phase() == Qt::ScrollEnd
            || (m_lastZoomMs >= 0 && nowMs - m_lastZoomMs > 250)) {
            m_zoomAccum = 0.0;
        }
        m_lastZoomMs = nowMs;

        // Discrete mouse wheel reports ±120 angleDelta per notch; precision
        // touchpads emit small per-frame pixelDelta values. Accumulate both so
        // one touchpad gesture steps smoothly instead of jumping per frame.
        const qreal delta = wheel->pixelDelta().y() != 0
            ? wheel->pixelDelta().y() * 2.0
            : static_cast<qreal>(wheel->angleDelta().y());
        m_zoomAccum += delta;
        while (m_zoomAccum >= 120.0) {
            m_zoomAccum -= 120.0;
            emit zoomRequested(1);
        }
        while (m_zoomAccum <= -120.0) {
            m_zoomAccum += 120.0;
            emit zoomRequested(-1);
        }
        wheel->accept();
        return true;
    }

    if (m_debugScroll) {
        qWarning() << "[gonzo scroll] phase=" << wheel->phase()
                   << "pixelDelta=" << wheel->pixelDelta()
                   << "angleDelta=" << wheel->angleDelta();
    }

    const qreal currentY = m_flickable->property("contentY").toReal();
    const qreal contentH = m_flickable->property("contentHeight").toReal();
    const qreal viewH = m_flickable->property("height").toReal();
    const qreal maxY = std::max<qreal>(0.0, contentH - viewH);

    qreal dpr = 1.0;
    if (m_window)
        dpr = m_window->effectiveDevicePixelRatio();

    const qreal nextY = handleWheel(currentY, maxY,
                                    wheel->pixelDelta().y(),
                                    wheel->angleDelta().y(),
                                    static_cast<int>(wheel->phase()),
                                    m_stepPixels,
                                    dpr);
    if (nextY != currentY)
        m_flickable->setProperty("contentY", nextY);

    // Arm a fallback release timer in case the compositor sends pixel-delta
    // touchpad events with NoScrollPhase or drops ScrollEnd.
    if (m_touchpadActive) {
        const int fallbackMs = wheel->phase() == Qt::NoScrollPhase ? 55 : 110;
        m_idleReleaseTimer.start(fallbackMs);
    }

    emit scrolled();
    wheel->accept();
    return true;
}

qreal KineticScroller::wheelDuration(qreal distance) {
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
    if (velocity != 0.0 && (delta / velocity) > 0.0)
        dur = std::min(dur, (delta / velocity) * 2.5);
    beginCurve(pos, newTarget, dur, velocity * dur / delta);
}

qreal KineticScroller::handleWheel(qreal currentY, qreal maxY, qreal pixelDeltaY,
                                   qreal angleDeltaY, int phase, qreal stepPixels,
                                   qreal devicePixelRatio, qint64 timestampMs) {
    maxY = std::max<qreal>(0.0, maxY);
    currentY = std::clamp(currentY, 0.0, maxY);
    const qint64 nowMs = timestampMs >= 0 ? timestampMs : m_clock.elapsed();

    if (phase == Qt::ScrollMomentum) {
        // Compositor/libinput already delivers momentum events: apply 1:1 and
        // never stack synthetic coasting on top.
        m_idleReleaseTimer.stop();
        m_sawCompositorMomentum = true;
        m_touchpadActive = false;
        m_samples.clear();
        setMode(Mode::Idle);
        const qreal delta = pixelDeltaY != 0.0 ? -pixelDeltaY : -(angleDeltaY / 120.0) * stepPixels;
        return snapToPixel(std::clamp(currentY + delta, 0.0, maxY), devicePixelRatio);
    }

    if (phase == Qt::ScrollEnd) {
        finishTouchpadGesture(currentY, nowMs);
        return currentY;
    }

    if (phase == Qt::ScrollBegin) {
        // Qt Wayland often attaches the first non-zero scroll delta directly to
        // ScrollBegin; reset gesture state first, then fall through to apply it.
        cancel();
        m_touchpadActive = true;
        m_sawCompositorMomentum = false;
        if (pixelDeltaY == 0.0 && angleDeltaY == 0.0)
            return currentY;
    }

    // Precision touchpad / continuous scroll: either explicit pixelDelta or a
    // phased ScrollBegin/ScrollUpdate gesture carrying angleDelta.
    const bool phasedTouch = (phase == Qt::ScrollBegin || phase == Qt::ScrollUpdate);
    if (pixelDeltaY != 0.0 || (phasedTouch && angleDeltaY != 0.0)) {
        setMode(Mode::Idle);
        m_sawCompositorMomentum = false;
        m_touchpadActive = true;
        const qreal delta = pixelDeltaY != 0.0
            ? -pixelDeltaY
            : -angleDeltaY * kTouchAngleToPixels;
        recordTouchSample(delta, nowMs);
        const qreal nextY = std::clamp(currentY + delta, 0.0, maxY);
        return snapToPixel(nextY, devicePixelRatio);
    }

    // Discrete mouse wheel notch (NoScrollPhase + angleDeltaY only).
    if (angleDeltaY != 0.0) {
        m_idleReleaseTimer.stop();
        m_touchpadActive = false;
        m_samples.clear();
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
