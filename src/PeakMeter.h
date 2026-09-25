#pragma once

#include <QElapsedTimer>
#include <QWidget>

namespace zrecord {

// A horizontal peak meter in dBFS (-60 to +3), with ballistics suited to
// spotting levels by eye:
// - the bar jumps up to each new peak and falls back at kFallDbPerSecond;
// - a peak-hold marker (and readout) stays at the highest recent peak for
//   kHoldMs, then falls;
// - the clip LED lights at or above 0 dBFS and stays lit until clicked, so
//   a single clipped block can't be missed.
// Feed it with setPeak() from a timer; it shows recording input and
// playback alike.
class PeakMeter : public QWidget {
    Q_OBJECT

public:
    static constexpr float kFloorDb = -60.0f;
    static constexpr float kCeilingDb = 3.0f;
    static constexpr float kFallDbPerSecond = 24.0f;
    static constexpr int kHoldMs = 1500;
    static constexpr int kLedWidth = 38;

    explicit PeakMeter(QWidget* parent = nullptr);

    // The largest |sample| since the previous call (linear, 1.0 = 0 dBFS).
    void setPeak(float linearPeak);
    // Same, at an explicit time (ms on any monotonic clock); tests use this.
    void setPeakAt(float linearPeak, qint64 nowMs);

    float levelDb() const { return levelDb_; }
    float holdDb() const { return holdDb_; }
    bool clipLit() const { return clipLit_; }
    void resetClip();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void clipReset();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    int xForDb(float db, int barLeft, int barWidth) const;

    QElapsedTimer clock_;
    qint64 lastMs_ = -1;
    float levelDb_ = kFloorDb;
    float holdDb_ = kFloorDb;
    qint64 holdSinceMs_ = 0;
    bool clipLit_ = false;
};

} // namespace zrecord
