#pragma once

#include <QMainWindow>
#include <memory>

#include "AudioEngine.h"
#include "AudioFileWriter.h"
#include "WaveformView.h"

class QComboBox;
class QPushButton;
class QProgressBar;
class QLabel;
class QCheckBox;
class QSlider;
class QTimer;

namespace zrecord {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onToggleRecord();
    void onTogglePlayback();
    void onSaveAs();
    void onFiltersChanged();
    void onTick();
    void refreshDevices();
    void onMicVolumeChanged(int value);

private:
    void buildUi();
    void applyFilterSettingsFromUi();
    void setControlsEnabled(bool recording);
    void queryInitialMicVolume();

    std::unique_ptr<AudioEngine> engine_;

    QComboBox* deviceCombo_ = nullptr;
    QComboBox* channelsCombo_ = nullptr;
    QComboBox* sampleRateCombo_ = nullptr;
    QComboBox* formatCombo_ = nullptr;

    QSlider* micVolumeSlider_ = nullptr;
    QLabel* micVolumeValueLabel_ = nullptr;

    QPushButton* recordButton_ = nullptr;
    QPushButton* playButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;

    QProgressBar* levelMeter_ = nullptr;
    WaveformView* waveformView_ = nullptr;

    QCheckBox* gainEnable_ = nullptr;
    QSlider* gainSlider_ = nullptr;
    QLabel* gainValueLabel_ = nullptr;

    QCheckBox* highPassEnable_ = nullptr;
    QSlider* highPassSlider_ = nullptr;
    QLabel* highPassValueLabel_ = nullptr;

    QCheckBox* lowPassEnable_ = nullptr;
    QSlider* lowPassSlider_ = nullptr;
    QLabel* lowPassValueLabel_ = nullptr;

    QCheckBox* noiseGateEnable_ = nullptr;
    QSlider* noiseGateSlider_ = nullptr;
    QLabel* noiseGateValueLabel_ = nullptr;
    QSlider* noiseGateAttackSlider_ = nullptr;
    QLabel* noiseGateAttackValueLabel_ = nullptr;
    QSlider* noiseGateReleaseSlider_ = nullptr;
    QLabel* noiseGateReleaseValueLabel_ = nullptr;

    QComboBox* voiceEffectCombo_ = nullptr;

    QTimer* timer_ = nullptr;
};

} // namespace zrecord
