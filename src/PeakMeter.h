#pragma once

#include <QElapsedTimer>
#include <QWidget>

#include <vector>

#include "Meter.h"

class QAction;
class QActionGroup;

namespace zrecord {

// The level meter: one horizontal bar per channel (L/R for stereo), in dBFS
// from a selectable floor (-48, -60 or -96) to +3:
// - a solid bar for the RMS level, with the peak level as a lighter bar
//   running on past it;
// - a peak-hold tick per channel that holds for kHoldMs, then falls at the
//   decay rate, and a numeric readout of the highest held peak;
// - an optional thin cyan tick for the raw input level before input gain
//   (recording only), so the effect of the gain is visible;
// - the CLIP LED, latched until clicked, for audio that clipped (runs of
//   full-scale samples, or beyond full scale).
// Right-click for range, decay rate and the input tick. Fed by addBlocks()
// from a UI timer with the blocks the audio thread measured.
class PeakMeter : public QWidget {
    Q_OBJECT

public:
    static constexpr float kCeilingDb = 3.0f;
    static constexpr int kHoldMs = MeterBallistics::kDefaultHoldMs;
    static constexpr int kLedWidth = 38;
    static constexpr int kReadoutWidth = 70;
    static constexpr int kLabelWidth = 12;
    static constexpr int kScaleHeight = 12;

    explicit PeakMeter(QWidget* parent = nullptr);

    // One UI tick's worth of blocks (may be empty: silence).
    void addBlocks(const std::vector<MeterBlock>& blocks);
    void addBlocksAt(const std::vector<MeterBlock>& blocks, qint64 nowMs); // tests: explicit clock
    // Convenience: a steady mono signal peaking at `linearPeak` (RMS equal to
    // the peak, like a square wave).
    void setPeak(float linearPeak);
    void setPeakAt(float linearPeak, qint64 nowMs);

    // Settings (also reachable from the context menu, which emits
    // settingsChanged()).
    void setFloorDb(float db);
    float floorDb() const { return ballistics_.floorDb(); }
    void setDecayDbPerSecond(float dbPerSecond);
    float decayDbPerSecond() const { return ballistics_.decayDbPerSecond(); }
    void setShowInputTick(bool show);
    bool showInputTick() const { return showInputTick_; }

    const MeterBallistics& ballistics() const { return ballistics_; }
    int channelCount() const { return ballistics_.channels(); }
    float levelDb() const { return ballistics_.maxPeakDb(); }
    float holdDb() const { return ballistics_.maxHoldDb(); }
    float peakDb(int channel) const { return ballistics_.channel(channel).peakDb; }
    float rmsDb(int channel) const { return ballistics_.channel(channel).rmsDb; }
    float channelHoldDb(int channel) const { return ballistics_.channel(channel).holdDb; }
    float inputDb(int channel) const { return ballistics_.channel(channel).inputDb; }
    bool clipLit() const { return ballistics_.clipLatched(); }
    void resetClip();

    // Geometry, for tests (and anything overlaying the meter).
    int xForDb(float db) const;
    QRect channelRect(int channel) const;
    // The colours bars are painted in at a given level: solid for RMS,
    // lighter for the peak part.
    static QColor zoneColor(float db, bool peakPart);
    static std::vector<int> scaleMarks(float floorDb);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void clipReset();
    void settingsChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void syncActions();
    int barLeft() const { return kLabelWidth; }
    int barWidth() const;
    int barsHeight() const;

    QElapsedTimer clock_;
    MeterBallistics ballistics_;
    bool showInputTick_ = true;

    QActionGroup* rangeGroup_ = nullptr;
    QActionGroup* decayGroup_ = nullptr;
    QAction* inputTickAction_ = nullptr;
    QAction* resetAction_ = nullptr;
};

} // namespace zrecord
