#include "TrackPanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

namespace zrecord {

namespace {
QString formatTimecode(double seconds, double stepSeconds) {
    int total = static_cast<int>(seconds);
    int mm = total / 60;
    int ss = total % 60;
    QString base = QString("%1:%2").arg(mm, 2, 10, QChar('0')).arg(ss, 2, 10, QChar('0'));
    if (stepSeconds < 1.0) {
        double fraction = seconds - std::floor(seconds);
        base += QString(".%1").arg(static_cast<int>(fraction * 10.0));
    }
    return base;
}
} // namespace

TrackPanel::TrackPanel(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(kRulerHeight + kLaneHeight + kScrollBarHeight);
    setMouseTracking(true);

    hScroll_ = new QScrollBar(Qt::Horizontal, this);
    connect(hScroll_, &QScrollBar::valueChanged, this, [this](int value) {
        viewStartFrame_ = value;
        update();
    });
}

void TrackPanel::setProject(Project* project) {
    project_ = project;
    refresh();
}

void TrackPanel::refresh() {
    rebuildHeaders();
    updateScrollBarRange();
    update();
}

void TrackPanel::rebuildHeaders() {
    for (auto& header : headers_) {
        header.container->deleteLater();
    }
    headers_.clear();
    if (project_ == nullptr) {
        return;
    }

    for (size_t i = 0; i < project_->tracks.size(); ++i) {
        int index = static_cast<int>(i);
        TrackHeader header;
        header.container = new QWidget(this);
        auto* layout = new QVBoxLayout(header.container);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(2);

        header.nameLabel = new QLabel(QString::fromStdString(project_->tracks[i].name), header.container);
        layout->addWidget(header.nameLabel);

        auto* buttonRow = new QHBoxLayout();
        header.muteButton = new QToolButton(header.container);
        header.muteButton->setText("M");
        header.muteButton->setCheckable(true);
        header.muteButton->setChecked(project_->tracks[i].muted);
        buttonRow->addWidget(header.muteButton);

        header.soloButton = new QToolButton(header.container);
        header.soloButton->setText("S");
        header.soloButton->setCheckable(true);
        header.soloButton->setChecked(project_->tracks[i].soloed);
        buttonRow->addWidget(header.soloButton);

        header.armButton = new QToolButton(header.container);
        header.armButton->setText("●");
        header.armButton->setCheckable(true);
        header.armButton->setChecked(project_->tracks[i].recordArmed);
        header.armButton->setToolTip("Arm for recording");
        buttonRow->addWidget(header.armButton);
        layout->addLayout(buttonRow);

        header.gainSlider = new QSlider(Qt::Horizontal, header.container);
        header.gainSlider->setRange(-24, 24);
        header.gainSlider->setValue(static_cast<int>(project_->tracks[i].gainDb));
        header.gainSlider->setToolTip("Track gain (dB)");
        layout->addWidget(header.gainSlider);

        connect(header.muteButton, &QToolButton::toggled, this, [this, index](bool checked) {
            if (project_ == nullptr) return;
            project_->tracks[static_cast<size_t>(index)].muted = checked;
            update();
        });
        connect(header.soloButton, &QToolButton::toggled, this, [this, index](bool checked) {
            if (project_ == nullptr) return;
            project_->tracks[static_cast<size_t>(index)].soloed = checked;
            update();
        });
        connect(header.armButton, &QToolButton::toggled, this, [this, index](bool checked) {
            if (project_ == nullptr) return;
            if (checked) {
                for (size_t j = 0; j < headers_.size(); ++j) {
                    if (static_cast<int>(j) == index) continue;
                    headers_[j].armButton->blockSignals(true);
                    headers_[j].armButton->setChecked(false);
                    headers_[j].armButton->blockSignals(false);
                    project_->tracks[j].recordArmed = false;
                }
            }
            project_->tracks[static_cast<size_t>(index)].recordArmed = checked;
        });
        connect(header.gainSlider, &QSlider::valueChanged, this, [this, index](int value) {
            if (project_ == nullptr) return;
            project_->tracks[static_cast<size_t>(index)].gainDb = value;
            update();
        });

        header.container->show();
        headers_.push_back(header);
    }
    layoutHeaders();
}

void TrackPanel::layoutHeaders() {
    for (size_t i = 0; i < headers_.size(); ++i) {
        int y = kRulerHeight + static_cast<int>(i) * kLaneHeight;
        headers_[i].container->setGeometry(0, y, kHeaderWidth, kLaneHeight);
    }
}

void TrackPanel::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    hScroll_->setGeometry(kHeaderWidth, height() - kScrollBarHeight, width() - kHeaderWidth, kScrollBarHeight);
    layoutHeaders();
    updateScrollBarRange();
}

void TrackPanel::updateScrollBarRange() {
    if (project_ == nullptr) {
        hScroll_->setRange(0, 0);
        return;
    }
    int64_t length = project_->lengthFrames();
    int visibleFrames = static_cast<int>((width() - kHeaderWidth) * framesPerPixel_);
    int64_t maxStart = std::max<int64_t>(0, length - visibleFrames);

    hScroll_->blockSignals(true);
    hScroll_->setRange(0, static_cast<int>(std::min<int64_t>(maxStart, std::numeric_limits<int>::max())));
    hScroll_->setPageStep(std::max(1, visibleFrames));
    hScroll_->setValue(static_cast<int>(std::clamp<int64_t>(viewStartFrame_, 0, maxStart)));
    hScroll_->blockSignals(false);
}

void TrackPanel::zoomIn() {
    framesPerPixel_ = std::max(1.0, framesPerPixel_ / 1.5);
    updateScrollBarRange();
    update();
}

void TrackPanel::zoomOut() {
    framesPerPixel_ = std::min(1.0e7, framesPerPixel_ * 1.5);
    updateScrollBarRange();
    update();
}

void TrackPanel::zoomToFit() {
    if (project_ == nullptr) return;
    int64_t length = std::max<int64_t>(1, project_->lengthFrames());
    int visibleWidth = std::max(1, width() - kHeaderWidth);
    framesPerPixel_ = std::max(1.0, static_cast<double>(length) / visibleWidth);
    viewStartFrame_ = 0;
    updateScrollBarRange();
    update();
}

void TrackPanel::beginLiveCapture(int trackIndex) {
    liveCapture_.trackIndex = trackIndex;
    liveCapture_.columns.clear();
    update();
}

void TrackPanel::pushLiveColumn(float minValue, float maxValue) {
    liveCapture_.columns.push_back({minValue, maxValue});
    while (static_cast<int>(liveCapture_.columns.size()) > kMaxLiveColumns) {
        liveCapture_.columns.pop_front();
    }
    update();
}

void TrackPanel::endLiveCapture() {
    liveCapture_.trackIndex = -1;
    liveCapture_.columns.clear();
    update();
}

int64_t TrackPanel::frameAtX(int x) const {
    return viewStartFrame_ + static_cast<int64_t>((x - kHeaderWidth) * framesPerPixel_);
}

int TrackPanel::xAtFrame(int64_t frame) const {
    return kHeaderWidth + static_cast<int>(static_cast<double>(frame - viewStartFrame_) / framesPerPixel_);
}

int TrackPanel::laneIndexAtY(int y) const {
    if (y < kRulerHeight) return -1;
    int index = (y - kRulerHeight) / kLaneHeight;
    if (project_ == nullptr || index < 0 || index >= static_cast<int>(project_->tracks.size())) {
        return -1;
    }
    return index;
}

PeakCache::MinMax TrackPanel::computeColumn(const Track& track, int64_t frameStart, int64_t frameEnd) const {
    float minValue = 0.0f;
    float maxValue = 0.0f;
    bool first = true;
    auto accumulate = [&](float v) {
        if (first) {
            minValue = maxValue = v;
            first = false;
        } else {
            minValue = std::min(minValue, v);
            maxValue = std::max(maxValue, v);
        }
    };

    for (const auto& clip : track.clips) {
        int64_t overlapStart = std::max(clip.startFrame, frameStart);
        int64_t overlapEnd = std::min(clip.endFrame(), frameEnd);
        if (overlapStart >= overlapEnd) continue;
        int64_t localStart = overlapStart - clip.startFrame;
        int64_t localEnd = overlapEnd - clip.startFrame;

        if (frameEnd - frameStart >= PeakCache::kBlockFrames && clip.peaks.isBuilt()) {
            int64_t blockStart = localStart / PeakCache::kBlockFrames;
            int64_t blockEnd = (localEnd - 1) / PeakCache::kBlockFrames;
            for (int64_t b = blockStart; b <= blockEnd; ++b) {
                auto mm = clip.peaks.blockAt(b);
                accumulate(mm.minValue);
                accumulate(mm.maxValue);
            }
        } else {
            for (int64_t f = localStart; f < localEnd; ++f) {
                for (int c = 0; c < clip.channels; ++c) {
                    size_t idx = static_cast<size_t>(f) * static_cast<size_t>(clip.channels) + static_cast<size_t>(c);
                    if (idx < clip.samples.size()) {
                        accumulate(clip.samples[idx]);
                    }
                }
            }
        }
    }
    return {minValue, maxValue};
}

void TrackPanel::drawRuler(QPainter& painter, int w) {
    painter.fillRect(0, 0, w, kRulerHeight, QColor(32, 32, 38));
    if (project_ == nullptr || project_->sampleRate <= 0.0) return;

    double pixelsPerSecond = project_->sampleRate / framesPerPixel_;
    static const double candidates[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 30, 60, 120, 300, 600, 1800, 3600};
    double step = candidates[sizeof(candidates) / sizeof(candidates[0]) - 1];
    for (double c : candidates) {
        if (c * pixelsPerSecond >= 80.0) {
            step = c;
            break;
        }
    }

    double viewStartSeconds = viewStartFrame_ / project_->sampleRate;
    double firstTick = std::floor(viewStartSeconds / step) * step;

    painter.setPen(QColor(150, 150, 160));
    for (double t = firstTick; ; t += step) {
        int64_t frame = static_cast<int64_t>(t * project_->sampleRate);
        int x = xAtFrame(frame);
        if (x > w) break;
        if (x >= kHeaderWidth) {
            painter.drawLine(x, kRulerHeight - 6, x, kRulerHeight);
            painter.drawText(x + 2, kRulerHeight - 8, formatTimecode(t, step));
        }
    }
}

void TrackPanel::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(18, 18, 22));

    int w = width();
    drawRuler(painter, w);

    if (project_ == nullptr) return;

    std::lock_guard<std::mutex> lock(project_->mutex);

    for (size_t i = 0; i < project_->tracks.size(); ++i) {
        const Track& track = project_->tracks[i];
        int laneTop = kRulerHeight + static_cast<int>(i) * kLaneHeight;
        int midY = laneTop + kLaneHeight / 2;
        int usableHalfHeight = kLaneHeight / 2 - 6;

        QColor bg = (static_cast<int>(i) % 2 == 0) ? QColor(24, 24, 28) : QColor(20, 20, 24);
        painter.fillRect(kHeaderWidth, laneTop, w - kHeaderWidth, kLaneHeight, bg);
        painter.setPen(QColor(64, 64, 72));
        painter.drawLine(kHeaderWidth, midY, w, midY);

        bool isLive = liveCapture_.trackIndex == static_cast<int>(i);
        QColor waveColor = track.muted && !isLive ? QColor(110, 110, 118) : QColor(80, 210, 130);
        painter.setPen(waveColor);

        if (isLive) {
            int visibleColumns = std::min<int>(w - kHeaderWidth, static_cast<int>(liveCapture_.columns.size()));
            int startIndex = static_cast<int>(liveCapture_.columns.size()) - visibleColumns;
            for (int x = 0; x < visibleColumns; ++x) {
                const auto& c = liveCapture_.columns[static_cast<size_t>(startIndex + x)];
                int yTop = midY - static_cast<int>(std::clamp(c.maxValue, -1.0f, 1.0f) * usableHalfHeight);
                int yBottom = midY - static_cast<int>(std::clamp(c.minValue, -1.0f, 1.0f) * usableHalfHeight);
                int xPos = w - visibleColumns + x;
                painter.drawLine(xPos, yTop, xPos, yBottom);
            }
        } else {
            for (int x = kHeaderWidth; x < w; ++x) {
                int64_t frameStart = frameAtX(x);
                int64_t frameEnd = frameAtX(x + 1);
                if (frameEnd <= frameStart) frameEnd = frameStart + 1;
                auto mm = computeColumn(track, frameStart, frameEnd);
                int yTop = midY - static_cast<int>(std::clamp(mm.maxValue, -1.0f, 1.0f) * usableHalfHeight);
                int yBottom = midY - static_cast<int>(std::clamp(mm.minValue, -1.0f, 1.0f) * usableHalfHeight);
                painter.drawLine(x, yTop, x, yBottom);
            }
        }

        if (!project_->selection.isEmpty() && project_->selection.trackIndex == static_cast<int>(i)) {
            int xStart = std::max(kHeaderWidth, xAtFrame(project_->selection.startFrame));
            int xEnd = std::min(w, xAtFrame(project_->selection.endFrame));
            if (xEnd > xStart) {
                painter.fillRect(xStart, laneTop, xEnd - xStart, kLaneHeight, QColor(255, 255, 255, 40));
            }
        }
    }

    int laneAreaBottom = kRulerHeight + static_cast<int>(project_->tracks.size()) * kLaneHeight;
    int playheadX = xAtFrame(project_->playheadFrame);
    if (playheadX >= kHeaderWidth && playheadX <= w) {
        painter.setPen(QColor(255, 80, 80));
        painter.drawLine(playheadX, kRulerHeight, playheadX, laneAreaBottom);
    }
}

void TrackPanel::mousePressEvent(QMouseEvent* event) {
    if (project_ == nullptr || event->pos().x() < kHeaderWidth) return;
    int lane = laneIndexAtY(event->pos().y());
    if (lane < 0) return;

    dragging_ = true;
    dragTrackIndex_ = lane;
    dragAnchorFrame_ = std::max<int64_t>(0, frameAtX(event->pos().x()));
    project_->selection.trackIndex = lane;
    project_->selection.startFrame = dragAnchorFrame_;
    project_->selection.endFrame = dragAnchorFrame_;
    project_->playheadFrame = dragAnchorFrame_;
    update();
}

void TrackPanel::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_ || project_ == nullptr) return;
    int64_t frame = std::max<int64_t>(0, frameAtX(event->pos().x()));
    project_->selection.trackIndex = dragTrackIndex_;
    project_->selection.startFrame = std::min(dragAnchorFrame_, frame);
    project_->selection.endFrame = std::max(dragAnchorFrame_, frame);
    update();
}

void TrackPanel::mouseReleaseEvent(QMouseEvent*) {
    if (!dragging_) return;
    dragging_ = false;
    emit selectionChanged();
}

void TrackPanel::wheelEvent(QWheelEvent* event) {
    if (project_ == nullptr) return;
    if (event->modifiers() & Qt::ControlModifier) {
        int64_t anchorFrame = frameAtX(event->position().toPoint().x());
        if (event->angleDelta().y() > 0) {
            framesPerPixel_ = std::max(1.0, framesPerPixel_ / 1.2);
        } else {
            framesPerPixel_ = std::min(1.0e7, framesPerPixel_ * 1.2);
        }
        viewStartFrame_ = std::max<int64_t>(0, anchorFrame - static_cast<int64_t>((event->position().x() - kHeaderWidth) * framesPerPixel_));
    } else {
        int64_t shift = static_cast<int64_t>(event->angleDelta().y() * framesPerPixel_);
        viewStartFrame_ = std::max<int64_t>(0, viewStartFrame_ - shift);
    }
    updateScrollBarRange();
    update();
    event->accept();
}

} // namespace zrecord
