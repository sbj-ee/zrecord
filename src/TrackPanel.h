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
    // Select drags out a time range; Move drags whole clips along the
    // timeline and between tracks (Audacity's "time shift" tool).
    enum class Tool { Select, Move };

    explicit TrackPanel(QWidget* parent = nullptr);

    void setTool(Tool tool);
    Tool tool() const { return tool_; }

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

    // Emitted when a clip drag finishes somewhere valid. The panel does not
    // mutate the project itself -- MainWindow turns this into an undoable
    // command.
    void clipMoveRequested(int fromTrack, int clipIndex, int toTrack, qint64 newStartFrame);

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
    // `skipClip`, when set, is left out -- used to hide a clip from its
    // original lane while it is being dragged.
    PeakCache::MinMax computeColumn(const Track& track, int64_t frameStart, int64_t frameEnd,
                                     const Clip* skipClip = nullptr) const;
    PeakCache::MinMax computeClipColumn(const Clip& clip, int64_t frameStart, int64_t frameEnd,
                                         int64_t shiftFrames) const;
    // Index of the clip covering `frame` on `trackIndex`, or -1.
    int clipIndexAt(int trackIndex, int64_t frame) const;
    // True if [start, start+length) on `trackIndex` is clear of every clip
    // except `excludeClip`, i.e. the drag can legally land there.
    bool canPlaceClip(int trackIndex, int64_t start, int64_t length, const Clip* excludeClip) const;
    void drawLaneWaveform(class QPainter& painter, const Track& track, int laneTop, int w,
                           const QColor& waveColor, const Clip* skipClip);
    void drawClipDragPreview(class QPainter& painter, int w);
    // The clip currently being dragged, or nullptr when no drag is in flight.
    const Clip* draggedClip() const;
    void drawRuler(class QPainter& painter, int w);
    void updateScrollBarRange();

    Project* project_ = nullptr;
    std::vector<TrackHeader> headers_;
    QScrollBar* hScroll_ = nullptr;

    double framesPerPixel_ = 44.1; // ~1 pixel per millisecond at 44.1kHz by default
    int64_t viewStartFrame_ = 0;

    Tool tool_ = Tool::Select;

    bool dragging_ = false;
    int dragTrackIndex_ = -1;
    int64_t dragAnchorFrame_ = 0;

    // In-flight clip drag. Nothing in the project changes until the drop is
    // committed, so an aborted or illegal drag costs nothing.
    struct ClipDrag {
        bool active = false;
        int sourceTrack = -1;
        int clipIndex = -1;
        int64_t grabFrame = 0;      // timeline frame under the cursor at press
        int64_t origStartFrame = 0;
        int targetTrack = -1;
        int64_t previewStartFrame = 0;
        bool valid = true;          // false when the drop would overlap a clip
    };
    ClipDrag clipDrag_;

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
