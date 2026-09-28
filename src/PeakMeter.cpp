#include "PeakMeter.h"

#include <QAction>
#include <QActionGroup>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace zrecord {

namespace {
constexpr float kRangeChoices[] = {-48.0f, -60.0f, -96.0f};
struct DecayChoice {
    const char* name;
    float dbPerSecond;
};
constexpr DecayChoice kDecayChoices[] = {{"Slow (12 dB/s)", 12.0f}, {"Medium (24 dB/s)", 24.0f}, {"Fast (48 dB/s)", 48.0f}};
} // namespace

PeakMeter::PeakMeter(QWidget* parent) : QWidget(parent) {
    clock_.start();
    setToolTip("Level (dBFS) per channel: solid bar = RMS, lighter bar = peak, tick = held peak (the readout), "
               "thin cyan tick = input before gain. The CLIP light stays on until clicked. Right-click for the "
               "range, decay rate and input tick.");

    rangeGroup_ = new QActionGroup(this);
    for (float db : kRangeChoices) {
        QAction* a = rangeGroup_->addAction(QString("%1 dB").arg(static_cast<int>(db)));
        a->setObjectName(QString("meterRange%1").arg(static_cast<int>(-db)));
        a->setCheckable(true);
        a->setData(db);
        connect(a, &QAction::triggered, this, [this, db] {
            setFloorDb(db);
            emit settingsChanged();
        });
    }
    decayGroup_ = new QActionGroup(this);
    for (const DecayChoice& choice : kDecayChoices) {
        const float rate = choice.dbPerSecond;
        QAction* a = decayGroup_->addAction(choice.name);
        a->setObjectName(QString("meterDecay%1").arg(static_cast<int>(rate)));
        a->setCheckable(true);
        a->setData(rate);
        connect(a, &QAction::triggered, this, [this, rate] {
            setDecayDbPerSecond(rate);
            emit settingsChanged();
        });
    }
    inputTickAction_ = new QAction("Show input level before gain", this);
    inputTickAction_->setObjectName("meterInputTick");
    inputTickAction_->setCheckable(true);
    connect(inputTickAction_, &QAction::triggered, this, [this](bool on) {
        setShowInputTick(on);
        emit settingsChanged();
    });
    resetAction_ = new QAction("Reset peak hold and CLIP", this);
    resetAction_->setObjectName("meterReset");
    connect(resetAction_, &QAction::triggered, this, [this] {
        ballistics_.resetHold();
        resetClip();
        update();
    });
    syncActions();
}

QSize PeakMeter::sizeHint() const {
    return {420, 48};
}

QSize PeakMeter::minimumSizeHint() const {
    return {180, 44};
}

void PeakMeter::addBlocks(const std::vector<MeterBlock>& blocks) {
    addBlocksAt(blocks, clock_.elapsed());
}

void PeakMeter::addBlocksAt(const std::vector<MeterBlock>& blocks, qint64 nowMs) {
    ballistics_.advance(blocks.data(), blocks.size(), nowMs);
    update();
}

void PeakMeter::setPeak(float linearPeak) {
    setPeakAt(linearPeak, clock_.elapsed());
}

void PeakMeter::setPeakAt(float linearPeak, qint64 nowMs) {
    addBlocksAt({steadyMeterBlock(1, std::fabs(linearPeak), std::fabs(linearPeak))}, nowMs);
}

void PeakMeter::setFloorDb(float db) {
    ballistics_.setFloorDb(db);
    syncActions();
    update();
}

void PeakMeter::setDecayDbPerSecond(float dbPerSecond) {
    ballistics_.setDecayDbPerSecond(dbPerSecond);
    syncActions();
}

void PeakMeter::setShowInputTick(bool show) {
    showInputTick_ = show;
    syncActions();
    update();
}

void PeakMeter::syncActions() {
    for (QAction* a : rangeGroup_->actions()) a->setChecked(a->data().toFloat() == ballistics_.floorDb());
    for (QAction* a : decayGroup_->actions()) a->setChecked(a->data().toFloat() == ballistics_.decayDbPerSecond());
    inputTickAction_->setChecked(showInputTick_);
}

void PeakMeter::resetClip() {
    if (ballistics_.clipLatched()) {
        ballistics_.resetClip();
        update();
        emit clipReset();
    }
}

void PeakMeter::mousePressEvent(QMouseEvent* event) {
    // Clicking anywhere on the meter clears the LED and the held peaks, as on
    // most hardware meters.
    if (event->button() == Qt::LeftButton) {
        ballistics_.resetHold();
        resetClip();
        update();
    }
}

void PeakMeter::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    QMenu* range = menu.addMenu("Range");
    range->addActions(rangeGroup_->actions());
    QMenu* decay = menu.addMenu("Decay");
    decay->addActions(decayGroup_->actions());
    menu.addAction(inputTickAction_);
    menu.addSeparator();
    menu.addAction(resetAction_);
    menu.exec(event->globalPos());
}

int PeakMeter::barWidth() const {
    return std::max(10, width() - kLabelWidth - kLedWidth - kReadoutWidth - 8);
}

int PeakMeter::barsHeight() const {
    return std::max(8, height() - kScaleHeight);
}

int PeakMeter::xForDb(float db) const {
    const float floor = ballistics_.floorDb();
    const float t = (std::clamp(db, floor, kCeilingDb) - floor) / (kCeilingDb - floor);
    return barLeft() + static_cast<int>(std::lround(t * static_cast<float>(barWidth())));
}

QRect PeakMeter::channelRect(int channel) const {
    const int n = std::max(1, channelCount());
    const int gap = 2;
    const int rowHeight = std::max(3, (barsHeight() - gap * (n - 1)) / n);
    return QRect(barLeft(), channel * (rowHeight + gap), barWidth(), rowHeight);
}

QColor PeakMeter::zoneColor(float db, bool peakPart) {
    // Green to -18, amber to -6, red above. The peak part is a lighter tint.
    if (db < -18.0f) return peakPart ? QColor(150, 230, 170) : QColor(60, 200, 90);
    if (db < -6.0f) return peakPart ? QColor(245, 230, 150) : QColor(230, 200, 40);
    return peakPart ? QColor(250, 160, 150) : QColor(235, 60, 50);
}

std::vector<int> PeakMeter::scaleMarks(float floorDb) {
    if (floorDb <= -96.0f) return {-96, -72, -60, -48, -36, -24, -12, -6, 0};
    if (floorDb <= -60.0f) return {-60, -48, -36, -24, -18, -12, -6, -3, 0};
    return {-48, -36, -24, -18, -12, -6, -3, 0};
}

void PeakMeter::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const float floor = ballistics_.floorDb();
    const int n = std::max(1, channelCount());
    const int barsH = barsHeight();

    QFont small = font();
    small.setPointSizeF(std::max(6.0, small.pointSizeF() * 0.75));

    // Paints [fromDb, toDb) of a bar in the zone colours.
    auto fillZones = [&](const QRect& row, float toDb, bool peakPart, float fromDb) {
        const float bounds[] = {floor, -18.0f, -6.0f, kCeilingDb};
        for (int z = 0; z < 3; ++z) {
            const float lo = std::max(bounds[z], fromDb);
            const float hi = std::min(bounds[z + 1], toDb);
            if (hi <= lo) continue;
            const int x0 = xForDb(lo);
            const int x1 = xForDb(hi);
            if (x1 > x0) painter.fillRect(QRect(x0, row.top(), x1 - x0, row.height()), zoneColor(lo, peakPart));
        }
    };

    for (int c = 0; c < n; ++c) {
        const QRect row = channelRect(c);
        const MeterBallistics::Channel& ch = ballistics_.channel(c);
        painter.fillRect(row, QColor(30, 30, 30));
        // Peak (lighter) from the RMS level up; RMS (solid) from the floor.
        const float rms = std::min(ch.rmsDb, ch.peakDb);
        fillZones(row, ch.peakDb, true, rms);
        fillZones(row, rms, false, floor);

        // 0 dBFS line.
        painter.setPen(QColor(255, 255, 255, 120));
        const int xZero = xForDb(0.0f);
        painter.drawLine(xZero, row.top(), xZero, row.bottom());

        // Input (before gain) tick: the lower half of the row.
        if (showInputTick_ && ch.inputDb > floor) {
            const int xIn = xForDb(ch.inputDb);
            painter.fillRect(QRect(xIn, row.top() + row.height() / 2, 2, row.height() - row.height() / 2),
                             QColor(0, 230, 255));
        }

        // Peak-hold tick: blue below full scale, red at or over it.
        if (ch.holdDb > floor) {
            const int xHold = xForDb(ch.holdDb);
            painter.fillRect(QRect(xHold - 1, row.top(), 3, row.height()),
                             ch.holdDb >= 0.0f ? QColor(255, 80, 80) : QColor(70, 160, 255));
        }

        // Channel label.
        if (n > 1) {
            painter.setFont(small);
            painter.setPen(palette().color(QPalette::WindowText));
            const QString label = n == 2 ? (c == 0 ? "L" : "R") : QString::number(c + 1);
            painter.drawText(QRect(0, row.top(), kLabelWidth - 2, row.height()), Qt::AlignCenter, label);
        }
    }

    // Scale.
    painter.setFont(small);
    painter.setPen(palette().color(QPalette::WindowText));
    for (int db : scaleMarks(floor)) {
        const int x = xForDb(static_cast<float>(db));
        painter.drawLine(x, barsH, x, barsH + 2);
        const QString label = QString::number(db);
        const int textWidth = painter.fontMetrics().horizontalAdvance(label);
        const int tx = std::clamp(x - textWidth / 2, 0, barLeft() + barWidth() - textWidth);
        painter.drawText(tx, height() - 1, label);
    }

    // Readout of the highest held peak.
    const float hold = ballistics_.maxHoldDb();
    const QRect readout(barLeft() + barWidth() + 4, 0, kReadoutWidth, barsH);
    const QString text = hold <= floor ? QString::fromUtf8("\xE2\x88\x92\xE2\x88\x9E dB")
                                       : QString("%1 dB").arg(static_cast<double>(hold), 0, 'f', 1);
    QFont bold = font();
    bold.setBold(true);
    painter.setFont(bold);
    painter.setPen(hold >= 0.0f ? QColor(235, 60, 50) : palette().color(QPalette::WindowText));
    painter.drawText(readout, Qt::AlignRight | Qt::AlignVCenter, text);

    // Clip LED.
    const bool clip = ballistics_.clipLatched();
    const QRect led(width() - kLedWidth, 0, kLedWidth, barsH);
    painter.fillRect(led, clip ? QColor(255, 30, 30) : QColor(70, 20, 20));
    painter.setPen(clip ? QColor(255, 255, 255) : QColor(140, 90, 90));
    QFont ledFont = small;
    ledFont.setBold(true);
    painter.setFont(ledFont);
    painter.drawText(led, Qt::AlignCenter, "CLIP");
}

} // namespace zrecord
