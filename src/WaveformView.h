#pragma once

#include <QWidget>
#include <QString>
#include <deque>

namespace zrecord {

// Scrolling min/max waveform display with a status text overlay, used to
// show a live timeline of the audio being recorded.
class WaveformView : public QWidget {
public:
    explicit WaveformView(QWidget* parent = nullptr);

    void clear();
    void pushColumn(float minValue, float maxValue);
    void setStatusText(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    struct Column {
        float minValue;
        float maxValue;
    };

    std::deque<Column> columns_;
    QString statusText_;
    float peakReference_ = kMinPeakReference;

    static constexpr int kMaxColumns = 4000;
    static constexpr float kMinPeakReference = 0.002f;
    static constexpr float kPeakDecay = 0.998f;
};

} // namespace zrecord
