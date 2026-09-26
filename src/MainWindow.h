#pragma once

#include <QMainWindow>
#include <functional>
#include <memory>

#include "AudioEngineInterface.h"
#include "Project.h"
#include "TrackPanel.h"
#include "VoiceChanger.h"

class QAction;
class QComboBox;
class QPushButton;
class QToolButton;
class QLabel;
class QMenu;
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
    void onSeekRequested(int64_t frame);
    void refreshDevices();
    void onMicVolumeChanged(int value);

    void onNewProject();
    void onOpenProject();
    void onSaveProject();
    void onSaveProjectAs();
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
    void onNormalize();
    void onVoiceChanger();
    void onAbout();
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
    // and Save As do once they have a path).
    bool saveProjectTo(const QString& path, QString* error = nullptr);

    // File > Open Recent: the most recently opened or saved project folders,
    // newest first, kept in QSettings. Opening one asks about unsaved
    // changes first; one that fails to open is dropped from the list.
    static QStringList recentProjects();
    bool openRecentProject(const QString& path);
    void setQuietRecentFailuresForTest(bool quiet) { quietRecentFailuresForTest_ = quiet; }
    bool hasUnsavedChanges() const;

    // What the "unsaved changes" question returns: Save, Discard or Cancel.
    // Tests replace the dialog with a function.
    using UnsavedChangesPrompt = std::function<int(const QString& action)>;
    void setUnsavedChangesPromptForTest(UnsavedChangesPrompt prompt) { unsavedPrompt_ = std::move(prompt); }

    // Replaces exec() of the Normalize/Amplify dialog: the function sets it
    // up and returns whether it was accepted.
    using NormalizeDialogDriver = std::function<bool(class NormalizeDialog&)>;
    // The About box text (includes the version, dev suffix and all).
    static QString aboutText();

    void setNormalizeDialogDriverForTest(NormalizeDialogDriver driver) { normalizeDriver_ = std::move(driver); }

    // Same for the Voice Changer dialog. The driver may click Preview; the
    // preview is stopped when the dialog closes, as with exec().
    using VoiceChangerDialogDriver = std::function<bool(class VoiceChangerDialog&)>;
    void setVoiceChangerDialogDriverForTest(VoiceChangerDialogDriver driver) { voiceDriver_ = std::move(driver); }
    // The temporary project a running preview plays (null when none), and a
    // way to run the timer tick without waiting for it.
    const Project* previewProjectForTest() const { return previewActive_ ? previewProject_.get() : nullptr; }
    void tickForTest() { onTick(); }

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
    void onInputDeviceChanged();
    int selectedDeviceMaxChannels() const;
    static constexpr int kMaxChannelsRole = Qt::UserRole + 1;
    // Asks before `action` would discard unsaved changes. True to go ahead
    // (nothing unsaved, saved, or discarded), false if the user cancelled.
    bool confirmDiscardChanges(const QString& action);
    // Save writes back to the project's own folder, asking for one only if
    // it has none yet; Save As always asks. Both report failures in a box.
    bool saveProject();
    bool saveProjectAs();
    // What Normalize and the Voice Changer act on: the time selection, or
    // else the clips picked with the Move tool. `scope` describes it.
    std::vector<GainTarget> editTargets(QString& scope) const;
    // Voice Changer preview: plays a copy of the targets' audio, processed,
    // through the normal playback engine, without touching project_.
    void startVoicePreview(const std::vector<GainTarget>& targets, const VoiceSettings& settings);
    void stopVoicePreview();
    void markSaved(const QString& path);
    void addRecentProject(const QString& path);
    void clearRecentProjects();
    void rebuildRecentMenu();
    // Where Open and Save start browsing: the current project's folder, else
    // the most recent one's, else home.
    QString lastProjectDir() const;
    static constexpr int kMaxRecentProjects = 8;
    static constexpr const char* kRecentProjectsKey = "recentProjects";
    static constexpr const char* kGeometryKey = "mainWindow/geometry";
    void updateWindowTitle();

    Project project_;
    std::unique_ptr<AudioEngineInterface> engine_;
    QUndoStack* undoStack_ = nullptr;
    int recordingArmedTrackIndex_ = -1;
    // True from a successful startPlayback() until the stream is released,
    // including after it ends by itself (onTick releases it then).
    int64_t playbackStartFrame_ = 0; // where the playhead returns when playback runs out
    bool playbackActive_ = false;
    int recordingChannels_ = 2; // channels the current take is captured with
    QString projectPath_;          // last saved/opened .zrproj, if any
    bool settingsDirty_ = false;   // header changes outside the undo stack
    UnsavedChangesPrompt unsavedPrompt_;
    NormalizeDialogDriver normalizeDriver_;
    VoiceChangerDialogDriver voiceDriver_;
    // The engine reads the project it plays from the audio thread, so the
    // preview's project lives here until the preview is stopped.
    std::unique_ptr<Project> previewProject_;
    bool previewActive_ = false;
    class VoiceChangerDialog* voiceDialog_ = nullptr; // open dialog, for preview state

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
    QAction* saveProjectAsAction_ = nullptr;
    QMenu* recentMenu_ = nullptr;
    bool quietRecentFailuresForTest_ = false;
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
    QAction* normalizeAction_ = nullptr;
    QAction* voiceChangerAction_ = nullptr;
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
    // Playhead movement; the shortcuts work while the timeline has focus.
    QAction* goToStartAction_ = nullptr;
    QAction* goToEndAction_ = nullptr;
    QAction* backOneSecondAction_ = nullptr;
    QAction* forwardOneSecondAction_ = nullptr;
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

    class PeakMeter* levelMeter_ = nullptr;
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
