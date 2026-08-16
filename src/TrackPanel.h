#pragma once

#include <QWidget>
#include <deque>
#include <vector>

#include "Project.h"

class QScrollBar;
class QLabel;
class QToolButton;
class QSlider;

namespace zrecord {

// The multi-track editing surface: a shared, zoomable/scrollable timeline
// with one waveform lane per track, drag-to-select, a playhead, and a small
// header (name/mute/solo/arm/gain) per track.
class TrackPanel : public QWidget {
    Q_OBJECT
public:
    explicit TrackPanel(QWidget* parent = nullptr);

    // `project` must outlive the TrackPanel or be cleared via setProject(nullptr).
    void setProject(Project* project);

    // Rebuilds per-track header widgets and repaints; call after any command
    // that changes the number of tracks or their audio content.
    void refresh();

    void zoomIn();
    void zoomOut();
    void zoomToFit();

    // While `trackIndex` is being recorded into, its lane shows a simple
    // scrolling live strip (like a VU-style capture view) instead of the
    // zoomed timeline, since the audio isn't committed to the project yet.
    void beginLiveCapture(int trackIndex);
    void pushLiveColumn(float minValue, float maxValue);
    void endLiveCapture();

signals:
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct TrackHeader {
        QWidget* container = nullptr;
        QLabel* nameLabel = nullptr;
        QToolButton* muteButton = nullptr;
        QToolButton* soloButton = nullptr;
        QToolButton* armButton = nullptr;
        QSlider* gainSlider = nullptr;
    };

    void rebuildHeaders();
    void layoutHeaders();
    int64_t frameAtX(int x) const;
    int xAtFrame(int64_t frame) const;
    int laneIndexAtY(int y) const;
    PeakCache::MinMax computeColumn(const Track& track, int64_t frameStart, int64_t frameEnd) const;
    void drawRuler(class QPainter& painter, int w);
    void updateScrollBarRange();

    Project* project_ = nullptr;
    std::vector<TrackHeader> headers_;
    QScrollBar* hScroll_ = nullptr;

    double framesPerPixel_ = 44.1; // ~1 pixel per millisecond at 44.1kHz by default
    int64_t viewStartFrame_ = 0;

    bool dragging_ = false;
    int dragTrackIndex_ = -1;
    int64_t dragAnchorFrame_ = 0;

    struct LiveCapture {
        int trackIndex = -1;
        std::deque<PeakCache::MinMax> columns;
    };
    LiveCapture liveCapture_;

    static constexpr int kHeaderWidth = 150;
    static constexpr int kRulerHeight = 24;
    static constexpr int kLaneHeight = 90;
    static constexpr int kScrollBarHeight = 16;
    static constexpr int kMaxLiveColumns = 2000;
};

} // namespace zrecord
