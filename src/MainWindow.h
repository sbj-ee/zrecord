#pragma once

#include <QMainWindow>
#include <functional>
#include <memory>

#include "AudioEngineInterface.h"
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
    // Injecting the engine is what lets a test build a MainWindow without
    // touching real audio hardware.
    explicit MainWindow(std::unique_ptr<AudioEngineInterface> engine, QWidget* parent = nullptr);
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
    void onFadeIn();
    void onFadeOut();
    void onCrossfade();
    void onApplyEffect();
    void onSelectAll();
    void onEnvelopeEdited(int trackIndex, const std::vector<EnvelopePoint>& before,
                          const std::vector<EnvelopePoint>& after, const QString& what);
    void onSelectionChanged();
    void onClipsMoveRequested(const std::vector<ClipMove>& moves);
    void onAddLabel();
    void onLabelActivated(int labelIndex);
    void onLabelContextMenu(int labelIndex, const QPoint& globalPos);

public:
    // Test hooks: these drive the same enabled-state paths the UI does, so a
    // test can assert the action matrix without simulating a whole session.
    void refreshActionStateForTest() { onSelectionChanged(); }
    void setControlsEnabledForTest(bool recording) { setControlsEnabled(recording); }

    // Opens a .zrproj folder (what File > Open does after its dialog). On
    // failure the current project and its undo history are left untouched
    // and false is returned with the reason in `error`.
    bool openProjectFolder(const QString& path, QString* error = nullptr);

    // Imports an audio file onto the selected (or last) track, converting it
    // to the project's sample rate if needed (what File > Import does after
    // its dialog). `note` gets a short description of any conversion.
    bool importAudioFile(const QString& path, QString* error = nullptr, QString* note = nullptr);

    // Saves to a .zrproj folder and marks the project saved (what File > Save
    // does after its dialog).
    bool saveProjectTo(const QString& path, QString* error = nullptr);
    bool hasUnsavedChanges() const;

    // What the "unsaved changes" question returns: Save, Discard or Cancel.
    // Tests replace the dialog with a function.
    using UnsavedChangesPrompt = std::function<int(const QString& action)>;
    void setUnsavedChangesPromptForTest(UnsavedChangesPrompt prompt) { unsavedPrompt_ = std::move(prompt); }

protected:
    void closeEvent(QCloseEvent* event) override;

public:

private:
    void buildUi();
    // Menus reuse the same QActions the toolbar shows, so the two can't drift
    // apart and the menu advertises each shortcut for free.
    void buildMenus();
    FilterSettings filterSettingsFromUi() const;
    void applyFilterSettingsFromUi();
    void setControlsEnabled(bool recording);
    void queryInitialMicVolume();
    int findArmedTrackIndex() const;
    bool projectHasAnyContent() const;
    // Stops playback (if any) and resets the Play button; used before the
    // project it reads from is replaced.
    void stopPlaybackNow();
    // Asks before `action` would discard unsaved changes. True to go ahead
    // (nothing unsaved, saved, or discarded), false if the user cancelled.
    bool confirmDiscardChanges(const QString& action);
    bool saveProjectInteractive();
    void markSaved(const QString& path);
    void updateWindowTitle();

    Project project_;
    std::unique_ptr<AudioEngineInterface> engine_;
    QUndoStack* undoStack_ = nullptr;
    int recordingArmedTrackIndex_ = -1;
    // True from a successful startPlayback() until the stream is released,
    // including after it ends by itself (onTick releases it then).
    bool playbackActive_ = false;
    QString projectPath_;          // last saved/opened .zrproj, if any
    bool settingsDirty_ = false;   // header changes outside the undo stack
    UnsavedChangesPrompt unsavedPrompt_;

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
    QAction* fadeInAction_ = nullptr;
    QAction* fadeOutAction_ = nullptr;
    QAction* crossfadeAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QAction* zoomFitAction_ = nullptr;

    // Mutually exclusive tool selection for the timeline.
    QAction* selectToolAction_ = nullptr;
    QAction* moveToolAction_ = nullptr;
    QAction* envelopeToolAction_ = nullptr;
    QAction* snapAction_ = nullptr;
    QAction* addLabelAction_ = nullptr;

    // Transport/export keep their custom-styled QPushButtons, so these
    // actions exist only to carry the shortcut; their enabled state is kept
    // in sync with the buttons in setControlsEnabled().
    QAction* recordAction_ = nullptr;
    QAction* playAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* applyEffectAction_ = nullptr;
    QAction* quitAction_ = nullptr;
    QAction* selectAllAction_ = nullptr;

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
