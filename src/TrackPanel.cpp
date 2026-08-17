#include "TrackPanel.h"

#include "Snap.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QContextMenuEvent>
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
    setMinimumHeight(lanesTop() + kLaneHeight + kScrollBarHeight);
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

void TrackPanel::setTool(Tool tool) {
    if (tool_ == tool) {
        return;
    }
    tool_ = tool;
    clipDrag_ = ClipDrag{};
    setCursor(tool_ == Tool::Move ? Qt::OpenHandCursor : Qt::ArrowCursor);
    update();
}

void TrackPanel::setSnapEnabled(bool enabled) {
    snapEnabled_ = enabled;
}

int64_t TrackPanel::applySnap(int64_t rawStart, int64_t clipLength, const Clip* excludeClip,
                               bool& snappedOut, int64_t& snapFrameOut) const {
    snappedOut = false;
    if (!snapEnabled_ || project_ == nullptr) {
        return rawStart;
    }
    // The radius is defined in pixels so the feel stays the same at every zoom
    // level; the arithmetic itself lives in Snap so it can be tested headless.
    int64_t threshold = std::max<int64_t>(1, static_cast<int64_t>(kSnapPixels * framesPerPixel_));
    SnapResult result = snapToTargets(rawStart, clipLength,
                                       collectSnapTargets(*project_, excludeClip), threshold);
    snappedOut = result.snapped;
    snapFrameOut = result.targetFrame;
    return result.startFrame;
}

int TrackPanel::clipIndexAt(int trackIndex, int64_t frame) const {
    if (project_ == nullptr || trackIndex < 0 || trackIndex >= static_cast<int>(project_->tracks.size())) {
        return -1;
    }
    const Track& track = project_->tracks[static_cast<size_t>(trackIndex)];
    for (size_t i = 0; i < track.clips.size(); ++i) {
        if (frame >= track.clips[i].startFrame && frame < track.clips[i].endFrame()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool TrackPanel::isClipSelected(int trackIndex, int clipIndex) const {
    for (const ClipRef& ref : selectedClips_) {
        if (ref.track == trackIndex && ref.index == clipIndex) {
            return true;
        }
    }
    return false;
}

std::vector<ClipMove> TrackPanel::computeMoves(int64_t deltaFrames, int deltaTracks) const {
    std::vector<ClipMove> moves;
    if (project_ == nullptr) {
        return moves;
    }
    for (const ClipRef& ref : selectedClips_) {
        if (ref.track < 0 || ref.track >= static_cast<int>(project_->tracks.size())) {
            return {};
        }
        const Track& track = project_->tracks[static_cast<size_t>(ref.track)];
        if (ref.index < 0 || ref.index >= static_cast<int>(track.clips.size())) {
            return {};
        }
        ClipMove move;
        move.fromTrack = ref.track;
        move.clipIndex = ref.index;
        move.toTrack = ref.track + deltaTracks;
        move.newStartFrame = track.clips[static_cast<size_t>(ref.index)].startFrame + deltaFrames;
        // The whole set moves rigidly, so one clip falling off the start or
        // past the last track invalidates the entire drag.
        if (move.toTrack < 0 || move.toTrack >= static_cast<int>(project_->tracks.size()) ||
            move.newStartFrame < 0) {
            return {};
        }
        moves.push_back(move);
    }
    return moves;
}

bool TrackPanel::movesAreValid(const std::vector<ClipMove>& moves) const {
    if (project_ == nullptr || moves.empty()) {
        return false;
    }
    for (const ClipMove& move : moves) {
        const Clip& moving =
            project_->tracks[static_cast<size_t>(move.fromTrack)].clips[static_cast<size_t>(move.clipIndex)];
        int64_t start = move.newStartFrame;
        int64_t end = start + moving.frameCount();

        const Track& target = project_->tracks[static_cast<size_t>(move.toTrack)];
        for (size_t i = 0; i < target.clips.size(); ++i) {
            // Clips that are themselves moving vacate their old spot, so they
            // can't block the drag.
            if (isClipSelected(move.toTrack, static_cast<int>(i))) {
                continue;
            }
            const Clip& other = target.clips[i];
            if (start < other.endFrame() && other.startFrame < end) {
                return false;
            }
        }
    }
    return true;
}

bool TrackPanel::canPlaceClip(int trackIndex, int64_t start, int64_t length, const Clip* excludeClip) const {
    if (project_ == nullptr || trackIndex < 0 || trackIndex >= static_cast<int>(project_->tracks.size())) {
        return false;
    }
    if (start < 0) {
        return false;
    }
    int64_t end = start + length;
    for (const auto& clip : project_->tracks[static_cast<size_t>(trackIndex)].clips) {
        if (&clip == excludeClip) continue;
        if (start < clip.endFrame() && clip.startFrame < end) {
            return false;
        }
    }
    return true;
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
        int y = lanesTop() + static_cast<int>(i) * kLaneHeight;
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
    if (y < lanesTop()) return -1;
    int index = (y - lanesTop()) / kLaneHeight;
    if (project_ == nullptr || index < 0 || index >= static_cast<int>(project_->tracks.size())) {
        return -1;
    }
    return index;
}

namespace {
// Folds one clip's peaks over [frameStart, frameEnd) into min/max, treating
// the clip as if it began at clip.startFrame + shiftFrames. The shift is what
// lets a clip being dragged be drawn at its preview position without touching
// the model.
void accumulateClip(const Clip& clip, int64_t frameStart, int64_t frameEnd, int64_t shiftFrames,
                     float& minValue, float& maxValue, bool& first) {
    int64_t clipStart = clip.startFrame + shiftFrames;
    int64_t overlapStart = std::max(clipStart, frameStart);
    int64_t overlapEnd = std::min(clipStart + clip.frameCount(), frameEnd);
    if (overlapStart >= overlapEnd) {
        return;
    }
    int64_t localStart = overlapStart - clipStart;
    int64_t localEnd = overlapEnd - clipStart;

    auto accumulate = [&](float v) {
        if (first) {
            minValue = maxValue = v;
            first = false;
        } else {
            minValue = std::min(minValue, v);
            maxValue = std::max(maxValue, v);
        }
    };

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
} // namespace

PeakCache::MinMax TrackPanel::computeColumn(const Track& track, int64_t frameStart, int64_t frameEnd,
                                             const std::vector<const Clip*>& skipClips) const {
    float minValue = 0.0f;
    float maxValue = 0.0f;
    bool first = true;
    for (const auto& clip : track.clips) {
        if (std::find(skipClips.begin(), skipClips.end(), &clip) != skipClips.end()) continue;
        accumulateClip(clip, frameStart, frameEnd, 0, minValue, maxValue, first);
    }
    return {minValue, maxValue};
}

PeakCache::MinMax TrackPanel::computeClipColumn(const Clip& clip, int64_t frameStart, int64_t frameEnd,
                                                 int64_t shiftFrames) const {
    float minValue = 0.0f;
    float maxValue = 0.0f;
    bool first = true;
    accumulateClip(clip, frameStart, frameEnd, shiftFrames, minValue, maxValue, first);
    return {minValue, maxValue};
}

void TrackPanel::drawClipDragPreview(QPainter& painter, int w) {
    if (!clipDrag_.active || project_ == nullptr || clipDrag_.preview.empty()) {
        return;
    }

    QColor tint = clipDrag_.valid ? QColor(80, 210, 130, 40) : QColor(220, 70, 70, 50);
    QColor wave = clipDrag_.valid ? QColor(150, 245, 190) : QColor(255, 140, 140);

    for (const ClipMove& move : clipDrag_.preview) {
        const Clip& clip =
            project_->tracks[static_cast<size_t>(move.fromTrack)].clips[static_cast<size_t>(move.clipIndex)];
        int laneTop = lanesTop() + move.toTrack * kLaneHeight;
        int midY = laneTop + kLaneHeight / 2;
        int usableHalfHeight = kLaneHeight / 2 - 6;

        int xStart = std::max(kHeaderWidth, xAtFrame(move.newStartFrame));
        int xEnd = std::min(w, xAtFrame(move.newStartFrame + clip.frameCount()));
        if (xEnd > xStart) {
            painter.fillRect(xStart, laneTop, xEnd - xStart, kLaneHeight, tint);
            painter.setPen(wave.darker(140));
            painter.drawRect(xStart, laneTop, xEnd - xStart - 1, kLaneHeight - 1);
        }

        int64_t shift = move.newStartFrame - clip.startFrame;
        painter.setPen(wave);
        for (int x = std::max(kHeaderWidth, xStart); x < xEnd; ++x) {
            int64_t frameStart = frameAtX(x);
            int64_t frameEnd = frameAtX(x + 1);
            if (frameEnd <= frameStart) frameEnd = frameStart + 1;
            auto mm = computeClipColumn(clip, frameStart, frameEnd, shift);
            int yTop = midY - static_cast<int>(std::clamp(mm.maxValue, -1.0f, 1.0f) * usableHalfHeight);
            int yBottom = midY - static_cast<int>(std::clamp(mm.minValue, -1.0f, 1.0f) * usableHalfHeight);
            painter.drawLine(x, yTop, x, yBottom);
        }
    }

    // Guide line marking what the drag latched onto.
    if (clipDrag_.snapped) {
        int snapX = xAtFrame(clipDrag_.snapFrame);
        if (snapX >= kHeaderWidth && snapX <= w) {
            int lanesBottom = lanesTop() + static_cast<int>(project_->tracks.size()) * kLaneHeight;
            painter.setPen(QPen(QColor(255, 214, 0), 1, Qt::DashLine));
            painter.drawLine(snapX, lanesTop(), snapX, lanesBottom);
        }
    }
}

const Clip* TrackPanel::draggedClip() const {
    if (!clipDrag_.active || project_ == nullptr) {
        return nullptr;
    }
    if (clipDrag_.sourceTrack < 0 || clipDrag_.sourceTrack >= static_cast<int>(project_->tracks.size())) {
        return nullptr;
    }
    const Track& track = project_->tracks[static_cast<size_t>(clipDrag_.sourceTrack)];
    if (clipDrag_.clipIndex < 0 || clipDrag_.clipIndex >= static_cast<int>(track.clips.size())) {
        return nullptr;
    }
    return &track.clips[static_cast<size_t>(clipDrag_.clipIndex)];
}

void TrackPanel::drawLaneWaveform(QPainter& painter, const Track& track, int laneTop, int w,
                                   const QColor& waveColor, int trackIndex) {
    int midY = laneTop + kLaneHeight / 2;
    int usableHalfHeight = kLaneHeight / 2 - 6;

    // Clips being dragged are lifted out of their own lane and redrawn at the
    // preview position, so every one of them is skipped here.
    std::vector<const Clip*> lifted;
    if (clipDrag_.active) {
        for (const ClipMove& move : clipDrag_.preview) {
            if (move.fromTrack == trackIndex) {
                lifted.push_back(&track.clips[static_cast<size_t>(move.clipIndex)]);
            }
        }
    }

    painter.setPen(waveColor);
    for (int x = kHeaderWidth; x < w; ++x) {
        int64_t frameStart = frameAtX(x);
        int64_t frameEnd = frameAtX(x + 1);
        if (frameEnd <= frameStart) frameEnd = frameStart + 1;
        auto mm = computeColumn(track, frameStart, frameEnd, lifted);
        int yTop = midY - static_cast<int>(std::clamp(mm.maxValue, -1.0f, 1.0f) * usableHalfHeight);
        int yBottom = midY - static_cast<int>(std::clamp(mm.minValue, -1.0f, 1.0f) * usableHalfHeight);
        painter.drawLine(x, yTop, x, yBottom);
    }
}

namespace {
// Width reserved for a label's text, and the marker drawn at its start.
constexpr int kLabelTextWidth = 130;
constexpr int kLabelMarkerWidth = 7;
} // namespace

void TrackPanel::drawLabelStrip(QPainter& painter, int w) {
    painter.fillRect(0, kRulerHeight, w, kLabelStripHeight, QColor(28, 28, 34));
    painter.setPen(QColor(52, 52, 60));
    painter.drawLine(kHeaderWidth, kRulerHeight + kLabelStripHeight - 1, w, kRulerHeight + kLabelStripHeight - 1);

    if (project_ == nullptr) {
        return;
    }

    painter.setPen(QColor(150, 150, 160));
    painter.drawText(QRect(6, kRulerHeight, kHeaderWidth - 12, kLabelStripHeight),
                      Qt::AlignLeft | Qt::AlignVCenter, "Labels");

    for (const auto& label : project_->labels) {
        int xStart = xAtFrame(label.startFrame);
        int xEnd = label.isRange() ? xAtFrame(label.endFrame) : xStart;
        if (xEnd < kHeaderWidth || xStart > w) {
            continue;
        }

        // Markers occupy the top of the strip and text the bottom half, so a
        // range's connector doesn't strike through its own caption.
        const int markerTop = kRulerHeight + 2;
        const int markerBottom = kRulerHeight + 7;

        painter.setPen(QColor(255, 214, 0));
        if (label.isRange()) {
            // A span reads as a bracket so its extent is obvious even when the
            // text runs past the end of the region.
            int clampedStart = std::max(kHeaderWidth, xStart);
            int clampedEnd = std::min(w, xEnd);
            painter.drawLine(clampedStart, markerTop, clampedEnd, markerTop);
            painter.drawLine(clampedStart, markerTop, clampedStart, markerBottom);
            painter.drawLine(clampedEnd, markerTop, clampedEnd, markerBottom);
        } else if (xStart >= kHeaderWidth) {
            painter.drawLine(xStart, markerTop, xStart, markerBottom);
            painter.drawLine(xStart, markerTop, xStart + kLabelMarkerWidth / 2, markerTop + 3);
            painter.drawLine(xStart, markerTop, xStart - kLabelMarkerWidth / 2, markerTop + 3);
        }

        int textX = std::max(kHeaderWidth + 2, xStart + 4);
        QRect textRect(textX, markerBottom, std::min(kLabelTextWidth, w - textX),
                        kRulerHeight + kLabelStripHeight - markerBottom);
        painter.setPen(QColor(240, 226, 160));
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter.fontMetrics().elidedText(QString::fromStdString(label.text),
                                                            Qt::ElideRight, textRect.width()));
    }
}

int TrackPanel::labelIndexAt(const QPoint& pos) const {
    if (project_ == nullptr || pos.y() < kRulerHeight || pos.y() >= kRulerHeight + kLabelStripHeight) {
        return -1;
    }
    // Later labels are drawn over earlier ones, so hit-test back to front.
    for (int i = static_cast<int>(project_->labels.size()) - 1; i >= 0; --i) {
        const Label& label = project_->labels[static_cast<size_t>(i)];
        int xStart = xAtFrame(label.startFrame);
        int xEnd = label.isRange() ? xAtFrame(label.endFrame) : xStart;
        // The clickable area covers the marker/bracket plus its text.
        int hitStart = xStart - kLabelMarkerWidth;
        int hitEnd = std::max(xEnd, xStart + kLabelTextWidth);
        if (pos.x() >= hitStart && pos.x() <= hitEnd) {
            return i;
        }
    }
    return -1;
}

void TrackPanel::mouseDoubleClickEvent(QMouseEvent* event) {
    int index = labelIndexAt(event->pos());
    if (index >= 0) {
        emit labelActivated(index);
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TrackPanel::contextMenuEvent(QContextMenuEvent* event) {
    int index = labelIndexAt(event->pos());
    if (index >= 0) {
        emit labelContextMenuRequested(index, event->globalPos());
        event->accept();
        return;
    }
    QWidget::contextMenuEvent(event);
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
    drawLabelStrip(painter, w);

    if (project_ == nullptr) return;

    std::lock_guard<std::mutex> lock(project_->mutex);

    for (size_t i = 0; i < project_->tracks.size(); ++i) {
        const Track& track = project_->tracks[i];
        int laneTop = lanesTop() + static_cast<int>(i) * kLaneHeight;
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
            // A clip mid-drag is lifted out of its own lane and redrawn at the
            // preview position below, so it appears to follow the cursor.
            drawLaneWaveform(painter, track, laneTop, w, waveColor, static_cast<int>(i));

            // Outline clips picked out with the Move tool, so it's visible
            // which ones a drag will carry.
            for (size_t c = 0; c < track.clips.size(); ++c) {
                if (!isClipSelected(static_cast<int>(i), static_cast<int>(c))) {
                    continue;
                }
                const Clip& sel = track.clips[c];
                int xStart = std::max(kHeaderWidth, xAtFrame(sel.startFrame));
                int xEnd = std::min(w, xAtFrame(sel.endFrame()));
                if (xEnd > xStart) {
                    painter.fillRect(xStart, laneTop, xEnd - xStart, kLaneHeight, QColor(120, 180, 255, 30));
                    painter.setPen(QPen(QColor(120, 180, 255), 1));
                    painter.drawRect(xStart, laneTop, xEnd - xStart - 1, kLaneHeight - 1);
                }
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

    drawClipDragPreview(painter, w);

    int laneAreaBottom = lanesTop() + static_cast<int>(project_->tracks.size()) * kLaneHeight;
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

    if (tool_ == Tool::Move) {
        int64_t frame = frameAtX(event->pos().x());
        int clipIndex = clipIndexAt(lane, frame);
        if (clipIndex < 0) {
            selectedClips_.clear(); // clicking empty space drops the selection
            update();
            return;
        }
        if (event->modifiers() & Qt::ControlModifier) {
            // Ctrl toggles membership so a set can be built up or trimmed.
            auto it = std::find_if(selectedClips_.begin(), selectedClips_.end(),
                                    [&](const ClipRef& r) { return r.track == lane && r.index == clipIndex; });
            if (it != selectedClips_.end()) {
                selectedClips_.erase(it);
                update();
                return;
            }
            selectedClips_.push_back(ClipRef{lane, clipIndex});
        } else if (!isClipSelected(lane, clipIndex)) {
            // Grabbing an unselected clip starts a fresh single-clip selection;
            // grabbing one already in the set keeps the set so it drags whole.
            selectedClips_.assign(1, ClipRef{lane, clipIndex});
        }

        const Clip& clip = project_->tracks[static_cast<size_t>(lane)].clips[static_cast<size_t>(clipIndex)];
        clipDrag_ = ClipDrag{};
        clipDrag_.active = true;
        clipDrag_.sourceTrack = lane;
        clipDrag_.clipIndex = clipIndex;
        clipDrag_.grabFrame = frame;
        clipDrag_.origStartFrame = clip.startFrame;
        clipDrag_.targetTrack = lane;
        clipDrag_.previewStartFrame = clip.startFrame;
        clipDrag_.preview = computeMoves(0, 0);
        setCursor(Qt::ClosedHandCursor);
        update();
        return;
    }

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
    if (clipDrag_.active && project_ != nullptr) {
        const Clip& clip = project_->tracks[static_cast<size_t>(clipDrag_.sourceTrack)]
                               .clips[static_cast<size_t>(clipDrag_.clipIndex)];
        int64_t delta = frameAtX(event->pos().x()) - clipDrag_.grabFrame;
        int64_t rawStart = std::max<int64_t>(0, clipDrag_.origStartFrame + delta);

        clipDrag_.snapped = false;
        if (event->modifiers() & Qt::AltModifier) {
            clipDrag_.previewStartFrame = rawStart; // Alt bypasses snapping
        } else {
            clipDrag_.previewStartFrame = applySnap(rawStart, clip.frameCount(), &clip,
                                                     clipDrag_.snapped, clipDrag_.snapFrame);
        }

        int lane = laneIndexAtY(event->pos().y());
        clipDrag_.targetTrack = lane >= 0 ? lane : clipDrag_.sourceTrack;

        // The grabbed clip defines the shift; every selected clip follows it.
        int64_t deltaFrames = clipDrag_.previewStartFrame - clipDrag_.origStartFrame;
        // Clamp so the earliest clip in the set stops at zero rather than the
        // whole drag becoming invalid and appearing to do nothing.
        int64_t earliest = std::numeric_limits<int64_t>::max();
        for (const ClipRef& ref : selectedClips_) {
            if (ref.track >= 0 && ref.track < static_cast<int>(project_->tracks.size())) {
                const Track& t = project_->tracks[static_cast<size_t>(ref.track)];
                if (ref.index >= 0 && ref.index < static_cast<int>(t.clips.size())) {
                    earliest = std::min(earliest, t.clips[static_cast<size_t>(ref.index)].startFrame);
                }
            }
        }
        if (earliest != std::numeric_limits<int64_t>::max()) {
            deltaFrames = std::max(deltaFrames, -earliest);
        }
        int deltaTracks = clipDrag_.targetTrack - clipDrag_.sourceTrack;
        clipDrag_.preview = computeMoves(deltaFrames, deltaTracks);
        clipDrag_.valid = movesAreValid(clipDrag_.preview);
        update();
        return;
    }

    if (!dragging_ || project_ == nullptr) return;
    int64_t frame = std::max<int64_t>(0, frameAtX(event->pos().x()));
    project_->selection.trackIndex = dragTrackIndex_;
    project_->selection.startFrame = std::min(dragAnchorFrame_, frame);
    project_->selection.endFrame = std::max(dragAnchorFrame_, frame);
    update();
}

void TrackPanel::mouseReleaseEvent(QMouseEvent*) {
    if (clipDrag_.active) {
        ClipDrag drag = clipDrag_;
        clipDrag_ = ClipDrag{};
        setCursor(tool_ == Tool::Move ? Qt::OpenHandCursor : Qt::ArrowCursor);

        bool moved = drag.targetTrack != drag.sourceTrack || drag.previewStartFrame != drag.origStartFrame;
        if (drag.valid && moved && !drag.preview.empty()) {
            emit clipsMoveRequested(drag.preview);
            selectedClips_.clear(); // indices no longer mean anything after the move
        }
        update();
        return;
    }

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
