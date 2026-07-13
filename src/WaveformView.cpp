#include "WaveformView.h"

#include <QPainter>
#include <algorithm>

namespace zrecord {

WaveformView::WaveformView(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(110);
}

void WaveformView::clear() {
    columns_.clear();
    peakReference_ = kMinPeakReference;
    update();
}

void WaveformView::pushColumn(float minValue, float maxValue) {
    float columnPeak = std::max(std::fabs(minValue), std::fabs(maxValue));
    if (columnPeak > peakReference_) {
        peakReference_ = columnPeak;
    } else {
        peakReference_ = std::max(kMinPeakReference, peakReference_ * kPeakDecay);
    }

    columns_.push_back({minValue, maxValue});
    while (static_cast<int>(columns_.size()) > kMaxColumns) {
        columns_.pop_front();
    }
    update();
}

void WaveformView::setStatusText(const QString& text) {
    statusText_ = text;
    update();
}

void WaveformView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(24, 24, 28));

    int w = width();
    int h = height();
    int midY = h / 2;

    painter.setPen(QColor(64, 64, 72));
    painter.drawLine(0, midY, w, midY);

    int visibleColumns = std::min<int>(w, static_cast<int>(columns_.size()));
    int startIndex = static_cast<int>(columns_.size()) - visibleColumns;
    int usableHalfHeight = h / 2 - 6;

    float displayScale = peakReference_ > 0.0001f ? 1.0f / peakReference_ : 1.0f;

    painter.setPen(QColor(80, 210, 130));
    for (int x = 0; x < visibleColumns; ++x) {
        const Column& c = columns_[static_cast<size_t>(startIndex + x)];
        float scaledMax = std::clamp(c.maxValue * displayScale, -1.0f, 1.0f);
        float scaledMin = std::clamp(c.minValue * displayScale, -1.0f, 1.0f);
        int yTop = midY - static_cast<int>(scaledMax * usableHalfHeight);
        int yBottom = midY - static_cast<int>(scaledMin * usableHalfHeight);
        int xPos = w - visibleColumns + x;
        painter.drawLine(xPos, yTop, xPos, yBottom);
    }

    painter.setPen(Qt::white);
    painter.drawText(QRect(8, 4, w - 16, 20), Qt::AlignLeft | Qt::AlignVCenter, statusText_);
}

} // namespace zrecord
