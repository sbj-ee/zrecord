#pragma once

#include <QMainWindow>
#include <memory>

#include "AudioEngine.h"
#include "Project.h"
#include "TrackPanel.h"

class QComboBox;
class QPushButton;
class QToolButton;
class QProgressBar;
class QLabel;
class QCheckBox;
class QSlider;
class QTimer;
class QUndoStack;

namespace zrecord {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onToggleRecord();
    void onTogglePlayback();
    void onExport();
    void onFiltersChanged();
    void onTick();
    void refreshDevices();
    void onMicVolumeChanged(int value);

    void onNewProject();
    void onOpenProject();
    void onSaveProject();
    void onAddTrack();
    void onRemoveTrack();
    void onImportAudio();

    void onCut();
    void onCopy();
    void onPaste();
    void onDeleteSelection();
    void onSilenceSelection();
    void onApplyEffect();
    void onSelectionChanged();

private:
    void buildUi();
    FilterSettings filterSettingsFromUi() const;
    void applyFilterSettingsFromUi();
    void setControlsEnabled(bool recording);
    void queryInitialMicVolume();
    int findArmedTrackIndex() const;
    bool projectHasAnyContent() const;

    Project project_;
    std::unique_ptr<AudioEngine> engine_;
    QUndoStack* undoStack_ = nullptr;
    int recordingArmedTrackIndex_ = -1;

    QComboBox* deviceCombo_ = nullptr;
    QComboBox* channelsCombo_ = nullptr;
    QComboBox* sampleRateCombo_ = nullptr;
    QComboBox* formatCombo_ = nullptr;

    QSlider* micVolumeSlider_ = nullptr;
    QLabel* micVolumeValueLabel_ = nullptr;

    QPushButton* recordButton_ = nullptr;
    QPushButton* playButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;

    QToolButton* newProjectButton_ = nullptr;
    QToolButton* openProjectButton_ = nullptr;
    QToolButton* saveProjectButton_ = nullptr;
    QToolButton* addTrackButton_ = nullptr;
    QToolButton* removeTrackButton_ = nullptr;
    QToolButton* importButton_ = nullptr;
    QToolButton* cutButton_ = nullptr;
    QToolButton* copyButton_ = nullptr;
    QToolButton* pasteButton_ = nullptr;
    QToolButton* deleteButton_ = nullptr;
    QToolButton* silenceButton_ = nullptr;
    QToolButton* undoButton_ = nullptr;
    QToolButton* redoButton_ = nullptr;
    QToolButton* zoomInButton_ = nullptr;
    QToolButton* zoomOutButton_ = nullptr;
    QToolButton* zoomFitButton_ = nullptr;

    QProgressBar* levelMeter_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    TrackPanel* trackPanel_ = nullptr;

    QCheckBox* limiterEnable_ = nullptr;
    QSlider* limiterCeilingSlider_ = nullptr;
    QLabel* limiterCeilingValueLabel_ = nullptr;

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

    QCheckBox* compressorEnable_ = nullptr;
    QSlider* compressorThresholdSlider_ = nullptr;
    QLabel* compressorThresholdValueLabel_ = nullptr;
    QSlider* compressorRatioSlider_ = nullptr;
    QLabel* compressorRatioValueLabel_ = nullptr;

    QComboBox* voiceEffectCombo_ = nullptr;
    QPushButton* applyEffectButton_ = nullptr;

    QTimer* timer_ = nullptr;
};

} // namespace zrecord
