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

    // When on, a dragged clip's nearest edge jumps to nearby clip edges, the
    // playhead, or zero. Holding Alt during a drag bypasses it.
    void setSnapEnabled(bool enabled);
    bool snapEnabled() const { return snapEnabled_; }

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
    // A finished drag of one or more clips. As with labels, the panel doesn't
    // mutate the project itself -- MainWindow turns this into one undoable step.
    void clipsMoveRequested(const std::vector<ClipMove>& moves);

    // Label interactions. As with clip moves, the panel doesn't mutate the
    // project or raise dialogs itself -- MainWindow owns both.
    void labelActivated(int labelIndex);
    void labelContextMenuRequested(int labelIndex, const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(class QContextMenuEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct TrackHeader {
        QWidget* container = nullptr;
        QToolButton* displayButton = nullptr;
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
                                     const std::vector<const Clip*>& skipClips = {}) const;
    PeakCache::MinMax computeClipColumn(const Clip& clip, int64_t frameStart, int64_t frameEnd,
                                         int64_t shiftFrames) const;
    // Index of the clip covering `frame` on `trackIndex`, or -1.
    int clipIndexAt(int trackIndex, int64_t frame) const;
    bool isClipSelected(int trackIndex, int clipIndex) const;
    // Where every selected clip would land for a given shift. Returns an empty
    // list if any of them would fall outside the timeline or tracks.
    std::vector<ClipMove> computeMoves(int64_t deltaFrames, int deltaTracks) const;
    // True when no moved clip would land on top of a clip that isn't moving.
    bool movesAreValid(const std::vector<ClipMove>& moves) const;
    // True if [start, start+length) on `trackIndex` is clear of every clip
    // except `excludeClip`, i.e. the drag can legally land there.
    bool canPlaceClip(int trackIndex, int64_t start, int64_t length, const Clip* excludeClip) const;
    void drawLaneWaveform(class QPainter& painter, const Track& track, int laneTop, int w,
                           const QColor& waveColor, int trackIndex);
    void drawLabelStrip(class QPainter& painter, int w);
    // Spectrogram for one lane: one FFT per pixel column, magnitude mapped to
    // colour. Computed on demand rather than cached -- see the note in the
    // implementation about when that stops being good enough.
    void drawLaneSpectrogram(class QPainter& painter, const Track& track, int laneTop, int w,
                              int trackIndex);
    // Index of the label whose marker or text sits under `pos`, or -1.
    int labelIndexAt(const QPoint& pos) const;
    void drawClipDragPreview(class QPainter& painter, int w);
    // The clip currently being dragged, or nullptr when no drag is in flight.
    const Clip* draggedClip() const;
    // Returns the start frame `rawStart` should snap to, given a clip of
    // `clipLength` frames. Whichever of the clip's two edges lands closest to
    // a target wins. Reports what it snapped to so the drag can draw a guide.
    int64_t applySnap(int64_t rawStart, int64_t clipLength, const Clip* excludeClip,
                       bool& snappedOut, int64_t& snapFrameOut) const;
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
        std::vector<ClipMove> preview; // where each selected clip would land
        bool snapped = false;
        int64_t snapFrame = 0;      // target the drag snapped to, for the guide line
    };
    ClipDrag clipDrag_;

    // Clips picked out with the Move tool; a drag shifts all of them together.
    struct ClipRef {
        int track = -1;
        int index = -1;
    };
    std::vector<ClipRef> selectedClips_;

    bool snapEnabled_ = true;

    struct LiveCapture {
        int trackIndex = -1;
        std::deque<PeakCache::MinMax> columns;
    };
    LiveCapture liveCapture_;

    static constexpr int kHeaderWidth = 150;
    static constexpr int kRulerHeight = 24;
    static constexpr int kLabelStripHeight = 20;
    // Track lanes start below the ruler and the label strip; everything that
    // positions a lane goes through this rather than adding the two up itself.
    static constexpr int lanesTop() { return kRulerHeight + kLabelStripHeight; }
    static constexpr int kLaneHeight = 90;
    static constexpr int kScrollBarHeight = 16;
    static constexpr int kMaxLiveColumns = 2000;
    // Snap radius in pixels, so the feel stays the same at every zoom level.
    static constexpr int kSnapPixels = 8;
    // 512 samples is a deliberate compromise: enough frequency resolution to
    // read as a spectrogram, small enough that a full-width repaint stays
    // interactive when every column needs its own transform.
    static constexpr int kFftSize = 512;
    static constexpr float kSpectrogramFloorDb = -84.0f;
};

} // namespace zrecord
