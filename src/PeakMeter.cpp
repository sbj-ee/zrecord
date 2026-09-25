#include "PeakMeter.h"

#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace zrecord {

namespace {

float toDb(float linear) {
    return linear > 0.0f ? 20.0f * std::log10(linear) : PeakMeter::kFloorDb;
}

constexpr int kScaleHeight = 12; // tick labels under the bar

} // namespace

PeakMeter::PeakMeter(QWidget* parent) : QWidget(parent) {
    clock_.start();
    setToolTip("Peak level (dBFS). The marker holds the recent peak; the CLIP light stays on until clicked.");
}

QSize PeakMeter::sizeHint() const {
    return {420, 30};
}

QSize PeakMeter::minimumSizeHint() const {
    return {160, 30};
}

void PeakMeter::setPeak(float linearPeak) {
    setPeakAt(linearPeak, clock_.elapsed());
}

void PeakMeter::setPeakAt(float linearPeak, qint64 nowMs) {
    const float db = std::max(kFloorDb, toDb(std::fabs(linearPeak)));
    const qint64 elapsed = lastMs_ < 0 ? 0 : std::max<qint64>(0, nowMs - lastMs_);
    lastMs_ = nowMs;

    // Instant attack, steady fall.
    const float fallen = levelDb_ - kFallDbPerSecond * static_cast<float>(elapsed) / 1000.0f;
    levelDb_ = std::max({db, fallen, kFloorDb});

    if (db >= holdDb_) {
        holdDb_ = db;
        holdSinceMs_ = nowMs;
    } else if (nowMs - holdSinceMs_ > kHoldMs) {
        // After the hold time the marker falls with the bar.
        holdDb_ = std::max(levelDb_, holdDb_ - kFallDbPerSecond * static_cast<float>(elapsed) / 1000.0f);
    }

    if (std::fabs(linearPeak) >= 1.0f) {
        clipLit_ = true;
    }
    update();
}

void PeakMeter::resetClip() {
    if (clipLit_) {
        clipLit_ = false;
        update();
        emit clipReset();
    }
}

void PeakMeter::mousePressEvent(QMouseEvent* event) {
    // Clicking anywhere on the meter clears the LED and the held peak, as on
    // most hardware meters.
    if (event->button() == Qt::LeftButton) {
        holdDb_ = levelDb_;
        resetClip();
        update();
    }
}

int PeakMeter::xForDb(float db, int barLeft, int barWidth) const {
    const float t = (std::clamp(db, kFloorDb, kCeilingDb) - kFloorDb) / (kCeilingDb - kFloorDb);
    return barLeft + static_cast<int>(std::lround(t * static_cast<float>(barWidth)));
}

void PeakMeter::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const int readoutWidth = 70;
    const int barLeft = 0;
    const int barWidth = std::max(10, width() - kLedWidth - readoutWidth - 8);
    const int barHeight = std::max(8, height() - kScaleHeight);
    const QRect bar(barLeft, 0, barWidth, barHeight);

    painter.fillRect(bar, QColor(30, 30, 30));

    // Zones: green to -18, amber to -6, red above (drawn as the lit part only).
    const int xLevel = xForDb(levelDb_, barLeft, barWidth);
    struct Zone { float fromDb, toDb; QColor color; };
    const Zone zones[] = {{kFloorDb, -18.0f, QColor(60, 200, 90)},
                          {-18.0f, -6.0f, QColor(230, 200, 40)},
                          {-6.0f, kCeilingDb, QColor(235, 60, 50)}};
    for (const Zone& zone : zones) {
        const int x0 = xForDb(zone.fromDb, barLeft, barWidth);
        const int x1 = std::min(xForDb(zone.toDb, barLeft, barWidth), xLevel);
        if (x1 > x0) {
            painter.fillRect(QRect(x0, 1, x1 - x0, barHeight - 2), zone.color);
        }
    }

    // 0 dBFS line.
    const int xZero = xForDb(0.0f, barLeft, barWidth);
    painter.setPen(QColor(255, 255, 255, 120));
    painter.drawLine(xZero, 0, xZero, barHeight - 1);

    // Peak-hold marker.
    if (holdDb_ > kFloorDb) {
        const int xHold = xForDb(holdDb_, barLeft, barWidth);
        // Blue below full scale (white read as a gap in the bar), red at or over it.
        painter.fillRect(QRect(xHold - 1, 0, 3, barHeight), holdDb_ >= 0.0f ? QColor(255, 80, 80) : QColor(70, 160, 255));
    }

    // Scale.
    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() * 0.75));
    painter.setFont(small);
    painter.setPen(palette().color(QPalette::WindowText));
    for (int db : {-60, -48, -36, -24, -18, -12, -6, -3, 0}) {
        const int x = xForDb(static_cast<float>(db), barLeft, barWidth);
        painter.drawLine(x, barHeight, x, barHeight + 2);
        const QString label = QString::number(db);
        const int textWidth = painter.fontMetrics().horizontalAdvance(label);
        const int tx = std::clamp(x - textWidth / 2, 0, barLeft + barWidth - textWidth);
        painter.drawText(tx, height() - 1, label);
    }

    // Readout of the held peak.
    const QRect readout(barLeft + barWidth + 4, 0, readoutWidth, barHeight);
    const QString text = holdDb_ <= kFloorDb ? QString::fromUtf8("\xE2\x88\x92\xE2\x88\x9E dB")
                                             : QString("%1 dB").arg(static_cast<double>(holdDb_), 0, 'f', 1);
    QFont bold = font();
    bold.setBold(true);
    painter.setFont(bold);
    painter.setPen(holdDb_ >= 0.0f ? QColor(235, 60, 50) : palette().color(QPalette::WindowText));
    painter.drawText(readout, Qt::AlignRight | Qt::AlignVCenter, text);

    // Clip LED.
    const QRect led(width() - kLedWidth, 0, kLedWidth, barHeight);
    painter.fillRect(led, clipLit_ ? QColor(255, 30, 30) : QColor(70, 20, 20));
    painter.setPen(clipLit_ ? QColor(255, 255, 255) : QColor(140, 90, 90));
    QFont ledFont = small;
    ledFont.setBold(true);
    painter.setFont(ledFont);
    painter.drawText(led, Qt::AlignCenter, "CLIP");
}

} // namespace zrecord
