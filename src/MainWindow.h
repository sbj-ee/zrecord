#pragma once

#include <QMainWindow>
#include <memory>

#include "AudioEngine.h"
#include "Project.h"
#include "TrackPanel.h"

class QAction;
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
    void onClipMoveRequested(int fromTrack, int clipIndex, int toTrack, qint64 newStartFrame);

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

    // Toolbar items are QActions (the QToolButtons that show them are created
    // in buildUi via setDefaultAction), so each item's shortcut, tooltip and
    // enabled state all live in one place.
    QAction* newProjectAction_ = nullptr;
    QAction* openProjectAction_ = nullptr;
    QAction* saveProjectAction_ = nullptr;
    QAction* addTrackAction_ = nullptr;
    QAction* removeTrackAction_ = nullptr;
    QAction* importAction_ = nullptr;
    QAction* cutAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* pasteAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* silenceAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QAction* zoomFitAction_ = nullptr;

    // Mutually exclusive tool selection for the timeline.
    QAction* selectToolAction_ = nullptr;
    QAction* moveToolAction_ = nullptr;
    QAction* snapAction_ = nullptr;

    // Transport/export keep their custom-styled QPushButtons, so these
    // actions exist only to carry the shortcut; their enabled state is kept
    // in sync with the buttons in setControlsEnabled().
    QAction* recordAction_ = nullptr;
    QAction* playAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* applyEffectAction_ = nullptr;

    // Recording cluster at the right of the toolbar: a blinking
    // "● REC hh:mm:ss" plus Stop and Mute controls. Hidden unless a take is
    // in progress.
    QWidget* recordingBar_ = nullptr;
    QLabel* recordingIndicator_ = nullptr;
    QToolButton* recordingStopButton_ = nullptr;
    QToolButton* recordingMuteButton_ = nullptr;

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
