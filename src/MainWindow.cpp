#include "MainWindow.h"

#include "NormalizeDialog.h"
#include "PeakMeter.h"
#include "zrecord_version.h"

#include <portaudio.h>

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QFileInfo>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QKeySequence>
#include <QFileDialog>
#include <QGridLayout>
#include <QInputDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cmath>

#include "AudioEngine.h"
#include "AudioFileReader.h"
#include "AudioFileWriter.h"
#include "Commands.h"
#include "ProjectFile.h"
#include "Resampler.h"

namespace zrecord {

namespace {
QString formatDuration(double seconds) {
    int total = static_cast<int>(seconds);
    int hh = total / 3600;
    int mm = (total % 3600) / 60;
    int ss = total % 60;
    return QString("%1:%2:%3")
        .arg(hh, 2, 10, QChar('0'))
        .arg(mm, 2, 10, QChar('0'))
        .arg(ss, 2, 10, QChar('0'));
}
} // namespace

MainWindow::MainWindow(QWidget* parent) : MainWindow(std::make_unique<AudioEngine>(), parent) {}

MainWindow::MainWindow(std::unique_ptr<AudioEngineInterface> engine, QWidget* parent)
    : QMainWindow(parent) {
    engine_ = std::move(engine);
    undoStack_ = new QUndoStack(this);
    buildUi();
    refreshDevices();
    applyFilterSettingsFromUi();
    setControlsEnabled(false);
    queryInitialMicVolume();
    onSelectionChanged();

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::onTick);
    timer_->start(50);

    connect(undoStack_, &QUndoStack::cleanChanged, this, [this](bool) { updateWindowTitle(); });
    connect(trackPanel_, &TrackPanel::trackSettingsChanged, this, [this] {
        settingsDirty_ = true;
        updateWindowTitle();
    });
    updateWindowTitle();
}

MainWindow::~MainWindow() {
    // Child QObjects are deleted only after this class's members are gone.
    // Left to that, the undo stack's destructor clears its commands and emits
    // indexChanged into a lambda that touches trackPanel_ and project_, both
    // already destroyed by then. Tear it down while everything is still alive.
    timer_->stop();
    undoStack_->disconnect(this);
    delete undoStack_;
    undoStack_ = nullptr;
    engine_->stopPlayback();
}

void MainWindow::buildUi() {

    auto* central = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(central);

    // Device / channel / sample-rate row.
    auto* deviceRow = new QHBoxLayout();
    deviceRow->addWidget(new QLabel("Input device:"));
    deviceCombo_ = new QComboBox();
    deviceRow->addWidget(deviceCombo_, 1);

    deviceRow->addWidget(new QLabel("Channels:"));
    channelsCombo_ = new QComboBox();
    channelsCombo_->addItem("Mono", 1);
    channelsCombo_->addItem("Stereo", 2);
    channelsCombo_->setCurrentIndex(1);
    deviceRow->addWidget(channelsCombo_);

    deviceRow->addWidget(new QLabel("Sample rate:"));
    sampleRateCombo_ = new QComboBox();
    sampleRateCombo_->addItem("44100 Hz", 44100);
    sampleRateCombo_->addItem("48000 Hz", 48000);
    sampleRateCombo_->addItem("96000 Hz", 96000);
    deviceRow->addWidget(sampleRateCombo_);

    rootLayout->addLayout(deviceRow);

    // Mic input volume (system-level source volume, via PipeWire/pactl).
    auto* micRow = new QHBoxLayout();
    micRow->addWidget(new QLabel("Mic input volume:"));
    micVolumeSlider_ = new QSlider(Qt::Horizontal);
    micVolumeSlider_->setRange(0, 150);
    micVolumeSlider_->setValue(50);
    micRow->addWidget(micVolumeSlider_, 1);
    micVolumeValueLabel_ = new QLabel("50%");
    micRow->addWidget(micVolumeValueLabel_);
    rootLayout->addLayout(micRow);
    connect(micVolumeSlider_, &QSlider::valueChanged, this, &MainWindow::onMicVolumeChanged);
    connect(deviceCombo_, &QComboBox::currentIndexChanged, this, [this](int) { onInputDeviceChanged(); });

    // A real QToolBar rather than a row of buttons in a layout: when the
    // window is too narrow for every entry, QToolBar folds the overflow into
    // a "»" popup instead of forcing the window wider than the screen.
    auto* toolBar = addToolBar("Main");
    toolBar->setObjectName("mainToolBar");
    toolBar->setMovable(false);
    toolBar->setToolButtonStyle(Qt::ToolButtonTextOnly);

    // Each entry is a QAction, so the toolbar button and the menu entry below
    // are two views of one object -- same label, tooltip, shortcut and
    // enabled state, and a shortcut can never fire while the button is
    // disabled.
    auto addTool = [&](const QString& text, const QString& tip, const QKeySequence& shortcut) {
        auto* action = new QAction(text, this);
        // QToolButton labels itself from iconText(), which strips the "..."
        // that signals "this opens a dialog"; set it so the label survives.
        action->setIconText(text);
        action->setShortcut(shortcut);
        action->setToolTip(shortcut.isEmpty()
                               ? tip
                               : QString("%1 (%2)").arg(tip, shortcut.toString(QKeySequence::NativeText)));
        toolBar->addAction(action);
        if (auto* button = qobject_cast<QToolButton*>(toolBar->widgetForAction(action))) {
            button->setFocusPolicy(Qt::NoFocus); // keep Space/R reaching the shortcuts
        }
        return action;
    };
    // Tool picker: Select drags out a time range, Move time-shifts clips.
    // F1/F5 match Audacity's bindings for the same two tools.
    selectToolAction_ = addTool("Select", "Select a time range", QKeySequence("F1"));
    moveToolAction_ = addTool("Move", "Drag clips along the timeline and between tracks", QKeySequence("F5"));
    envelopeToolAction_ = addTool("Envelope", "Draw a volume curve on a track: click to add a point, "
                                              "drag to move it, right-click to remove it",
                                   QKeySequence("F2"));
    selectToolAction_->setCheckable(true);
    moveToolAction_->setCheckable(true);
    envelopeToolAction_->setCheckable(true);
    selectToolAction_->setChecked(true);
    auto* toolGroup = new QActionGroup(this);
    toolGroup->setExclusive(true);
    toolGroup->addAction(selectToolAction_);
    toolGroup->addAction(moveToolAction_);
    toolGroup->addAction(envelopeToolAction_);

    snapAction_ = addTool("Snap", "Snap dragged clips to clip edges, the playhead and zero (hold Alt to bypass)",
                           QKeySequence());
    snapAction_->setCheckable(true);
    snapAction_->setChecked(true);
    toolBar->addSeparator();

    newProjectAction_ = addTool("New", "New project", QKeySequence::New);
    openProjectAction_ = addTool("Open...", "Open project", QKeySequence::Open);
    saveProjectAction_ = addTool("Save...", "Save project", QKeySequence::Save);
    toolBar->addSeparator();
    addTrackAction_ = addTool("+Track", "Add track", QKeySequence("Ctrl+Shift+N"));
    removeTrackAction_ = addTool("-Track", "Remove selected (or last) track", QKeySequence("Ctrl+Shift+W"));
    importAction_ = addTool("Import...", "Import audio file into selected track", QKeySequence("Ctrl+I"));
    addLabelAction_ = addTool("Label", "Label the selection, or the playhead if nothing is selected",
                               QKeySequence("Ctrl+B"));
    toolBar->addSeparator();
    cutAction_ = addTool("Cut", "Cut selection", QKeySequence::Cut);
    copyAction_ = addTool("Copy", "Copy selection", QKeySequence::Copy);
    pasteAction_ = addTool("Paste", "Paste at playhead", QKeySequence::Paste);
    deleteAction_ = addTool("Delete", "Delete selection", QKeySequence::Delete);
    silenceAction_ = addTool("Silence", "Silence selection", QKeySequence("Ctrl+L"));
    fadeInAction_ = addTool("Fade In", "Ramp the selection up from silence", QKeySequence());
    fadeOutAction_ = addTool("Fade Out", "Ramp the selection down to silence", QKeySequence());
    crossfadeAction_ = addTool("Crossfade",
                                "Crossfade two adjacent clips; select a region spanning the join, "
                                "its length sets the crossfade duration",
                                QKeySequence());
    toolBar->addSeparator();
    undoAction_ = addTool("Undo", "Undo", QKeySequence::Undo);
    redoAction_ = addTool("Redo", "Redo", QKeySequence::Redo);
    toolBar->addSeparator();
    zoomInAction_ = addTool("Zoom In", "Zoom in", QKeySequence("Ctrl+1"));
    zoomOutAction_ = addTool("Zoom Out", "Zoom out", QKeySequence("Ctrl+3"));
    zoomFitAction_ = addTool("Zoom Fit", "Zoom to fit whole project", QKeySequence("Ctrl+F"));

    // Expanding spacer pins the recording cluster to the right-hand end.
    auto* toolBarSpacer = new QWidget();
    toolBarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolBar->addWidget(toolBarSpacer);

    recordingBar_ = new QWidget();
    auto* recordingBarLayout = new QHBoxLayout(recordingBar_);
    recordingBarLayout->setContentsMargins(0, 0, 0, 0);
    recordingBarLayout->setSpacing(6);

    recordingIndicator_ = new QLabel("●  REC  00:00:00");
    recordingIndicator_->setToolTip("A take is being recorded");
    recordingBarLayout->addWidget(recordingIndicator_);

    recordingMuteButton_ = new QToolButton();
    recordingMuteButton_->setText("Mute");
    recordingMuteButton_->setCheckable(true);
    recordingMuteButton_->setFocusPolicy(Qt::NoFocus);
    recordingMuteButton_->setToolTip("Record silence until unmuted (the take keeps running)");
    recordingBarLayout->addWidget(recordingMuteButton_);

    recordingStopButton_ = new QToolButton();
    recordingStopButton_->setText("■ Stop");
    recordingStopButton_->setFocusPolicy(Qt::NoFocus);
    recordingStopButton_->setToolTip("Stop recording (R)");
    recordingBarLayout->addWidget(recordingStopButton_);

    connect(recordingMuteButton_, &QToolButton::toggled, this, [this](bool muted) {
        engine_->setInputMuted(muted);
    });
    connect(recordingStopButton_, &QToolButton::clicked, this, &MainWindow::onToggleRecord);

    recordingBar_->hide();
    toolBar->addWidget(recordingBar_);

    connect(newProjectAction_, &QAction::triggered, this, &MainWindow::onNewProject);
    connect(openProjectAction_, &QAction::triggered, this, &MainWindow::onOpenProject);
    connect(saveProjectAction_, &QAction::triggered, this, &MainWindow::onSaveProject);
    connect(addTrackAction_, &QAction::triggered, this, &MainWindow::onAddTrack);
    connect(removeTrackAction_, &QAction::triggered, this, &MainWindow::onRemoveTrack);
    connect(importAction_, &QAction::triggered, this, &MainWindow::onImportAudio);
    connect(addLabelAction_, &QAction::triggered, this, &MainWindow::onAddLabel);
    connect(cutAction_, &QAction::triggered, this, &MainWindow::onCut);
    connect(copyAction_, &QAction::triggered, this, &MainWindow::onCopy);
    connect(pasteAction_, &QAction::triggered, this, &MainWindow::onPaste);
    connect(deleteAction_, &QAction::triggered, this, &MainWindow::onDeleteSelection);
    connect(silenceAction_, &QAction::triggered, this, &MainWindow::onSilenceSelection);
    connect(fadeInAction_, &QAction::triggered, this, &MainWindow::onFadeIn);
    connect(fadeOutAction_, &QAction::triggered, this, &MainWindow::onFadeOut);
    // Menu-only, like Select All: the toolbar is already full.
    normalizeAction_ = new QAction("Normalize / Amplify...", this);
    normalizeAction_->setStatusTip("Scale the selection or selected clips to a peak level, or by a gain");
    connect(normalizeAction_, &QAction::triggered, this, &MainWindow::onNormalize);
    connect(crossfadeAction_, &QAction::triggered, this, &MainWindow::onCrossfade);
    connect(undoAction_, &QAction::triggered, undoStack_, &QUndoStack::undo);
    connect(redoAction_, &QAction::triggered, undoStack_, &QUndoStack::redo);
    connect(undoStack_, &QUndoStack::canUndoChanged, undoAction_, &QAction::setEnabled);
    connect(undoStack_, &QUndoStack::canRedoChanged, redoAction_, &QAction::setEnabled);
    connect(undoStack_, &QUndoStack::indexChanged, this, [this](int) {
        trackPanel_->refresh();
        // Undo/redo can add or remove audio, so every enabled state that
        // depends on content has to be re-evaluated, not just the selection's.
        onSelectionChanged();
    });
    undoAction_->setEnabled(false);
    redoAction_->setEnabled(false);

    // Big red record/stop button.
    recordButton_ = new QPushButton("●  RECORD");
    recordButton_->setMinimumHeight(90);
    recordButton_->setMinimumWidth(240);
    recordButton_->setCursor(Qt::PointingHandCursor);
    recordButton_->setStyleSheet(
        "QPushButton {"
        "  background-color: #d32f2f;"
        "  color: white;"
        "  font-size: 22px;"
        "  font-weight: bold;"
        "  border: 4px solid #8e0000;"
        "  border-radius: 45px;"
        "}"
        "QPushButton:hover { background-color: #e53935; }"
        "QPushButton:pressed { background-color: #b71c1c; }");
    recordButton_->setFocusPolicy(Qt::NoFocus);
    connect(recordButton_, &QPushButton::clicked, this, &MainWindow::onToggleRecord);

    recordAction_ = new QAction("Record / Stop", this);
    recordAction_->setShortcut(QKeySequence("R"));
    connect(recordAction_, &QAction::triggered, this, &MainWindow::onToggleRecord);
    recordButton_->setToolTip("Start/stop recording (R)");

    auto* recordRow = new QHBoxLayout();
    recordRow->addStretch();
    recordRow->addWidget(recordButton_);
    recordRow->addStretch();
    rootLayout->addLayout(recordRow);

    // Level meter + track timeline.
    auto* meterRow = new QHBoxLayout();
    levelMeter_ = new PeakMeter();
    meterRow->addWidget(levelMeter_, 1);
    statusLabel_ = new QLabel("Ready");
    meterRow->addWidget(statusLabel_);
    rootLayout->addLayout(meterRow);

    trackPanel_ = new TrackPanel();
    trackPanel_->setProject(&project_);
    trackPanel_->setMinimumHeight(260);
    rootLayout->addWidget(trackPanel_, 1);
    connect(trackPanel_, &TrackPanel::selectionChanged, this, &MainWindow::onSelectionChanged);
    connect(trackPanel_, &TrackPanel::seekRequested, this, &MainWindow::onSeekRequested);
    connect(trackPanel_, &TrackPanel::clipsMoveRequested, this, &MainWindow::onClipsMoveRequested);
    connect(trackPanel_, &TrackPanel::envelopeEdited, this, &MainWindow::onEnvelopeEdited);
    connect(trackPanel_, &TrackPanel::labelActivated, this, &MainWindow::onLabelActivated);
    connect(trackPanel_, &TrackPanel::labelContextMenuRequested, this, &MainWindow::onLabelContextMenu);
    connect(selectToolAction_, &QAction::triggered, this, [this] {
        trackPanel_->setTool(TrackPanel::Tool::Select);
    });
    connect(moveToolAction_, &QAction::triggered, this, [this] {
        trackPanel_->setTool(TrackPanel::Tool::Move);
    });
    connect(envelopeToolAction_, &QAction::triggered, this, [this] {
        trackPanel_->setTool(TrackPanel::Tool::Envelope);
    });
    connect(snapAction_, &QAction::toggled, this, [this](bool enabled) {
        trackPanel_->setSnapEnabled(enabled);
    });
    connect(zoomInAction_, &QAction::triggered, trackPanel_, &TrackPanel::zoomIn);
    connect(zoomOutAction_, &QAction::triggered, trackPanel_, &TrackPanel::zoomOut);
    connect(zoomFitAction_, &QAction::triggered, trackPanel_, &TrackPanel::zoomToFit);

    // Playback / export row.
    auto* secondaryRow = new QHBoxLayout();
    playButton_ = new QPushButton("▶  Play");
    playButton_->setFocusPolicy(Qt::NoFocus);
    playButton_->setToolTip("Play/stop from the playhead (Space)");
    connect(playButton_, &QPushButton::clicked, this, &MainWindow::onTogglePlayback);
    secondaryRow->addWidget(playButton_);

    playAction_ = new QAction("Play / Stop", this);
    playAction_->setShortcut(QKeySequence(Qt::Key_Space));
    connect(playAction_, &QAction::triggered, this, &MainWindow::onTogglePlayback);

    secondaryRow->addWidget(new QLabel("Export as:"));
    formatCombo_ = new QComboBox();
    formatCombo_->addItem(AudioFileWriter::nameFor(AudioFormat::Wav), static_cast<int>(AudioFormat::Wav));
    formatCombo_->addItem(AudioFileWriter::nameFor(AudioFormat::Flac), static_cast<int>(AudioFormat::Flac));
    formatCombo_->addItem(AudioFileWriter::nameFor(AudioFormat::OggVorbis), static_cast<int>(AudioFormat::OggVorbis));
    formatCombo_->addItem(AudioFileWriter::nameFor(AudioFormat::Mp3), static_cast<int>(AudioFormat::Mp3));
    secondaryRow->addWidget(formatCombo_);

    exportButton_ = new QPushButton("Export Mixdown...");
    exportButton_->setFocusPolicy(Qt::NoFocus);
    exportButton_->setToolTip("Export the mixed project to a file (Ctrl+E)");
    connect(exportButton_, &QPushButton::clicked, this, &MainWindow::onExport);
    secondaryRow->addWidget(exportButton_);

    exportAction_ = new QAction("Export Mixdown...", this);
    exportAction_->setShortcut(QKeySequence("Ctrl+E"));
    connect(exportAction_, &QAction::triggered, this, &MainWindow::onExport);

    rootLayout->addLayout(secondaryRow);

    // Filters panel: applied live to the armed track's input while recording.
    auto* filterGroup = new QGroupBox("Filters (live while recording, or apply to selection)");
    auto* grid = new QGridLayout(filterGroup);

    limiterEnable_ = new QCheckBox("Limiter (prevent clipping)");
    limiterCeilingSlider_ = new QSlider(Qt::Horizontal);
    limiterCeilingSlider_->setRange(-12, 0);
    limiterCeilingSlider_->setValue(-1);
    limiterCeilingValueLabel_ = new QLabel("-1 dB");

    gainEnable_ = new QCheckBox("Gain");
    gainSlider_ = new QSlider(Qt::Horizontal);
    gainSlider_->setRange(-24, 24);
    gainSlider_->setValue(0);
    gainValueLabel_ = new QLabel("0 dB");
    grid->addWidget(gainEnable_, 1, 0);
    grid->addWidget(gainSlider_, 1, 1);
    grid->addWidget(gainValueLabel_, 1, 2);

    highPassEnable_ = new QCheckBox("High-pass");
    highPassSlider_ = new QSlider(Qt::Horizontal);
    highPassSlider_->setRange(20, 2000);
    highPassSlider_->setValue(100);
    highPassValueLabel_ = new QLabel("100 Hz");
    grid->addWidget(highPassEnable_, 2, 0);
    grid->addWidget(highPassSlider_, 2, 1);
    grid->addWidget(highPassValueLabel_, 2, 2);

    lowPassEnable_ = new QCheckBox("Low-pass");
    lowPassSlider_ = new QSlider(Qt::Horizontal);
    lowPassSlider_->setRange(200, 20000);
    lowPassSlider_->setValue(8000);
    lowPassValueLabel_ = new QLabel("8000 Hz");
    grid->addWidget(lowPassEnable_, 3, 0);
    grid->addWidget(lowPassSlider_, 3, 1);
    grid->addWidget(lowPassValueLabel_, 3, 2);

    noiseGateEnable_ = new QCheckBox("Noise gate");
    noiseGateSlider_ = new QSlider(Qt::Horizontal);
    noiseGateSlider_->setRange(-80, 0);
    noiseGateSlider_->setValue(-40);
    noiseGateValueLabel_ = new QLabel("-40 dB");
    grid->addWidget(noiseGateEnable_, 4, 0);
    grid->addWidget(noiseGateSlider_, 4, 1);
    grid->addWidget(noiseGateValueLabel_, 4, 2);

    noiseGateAttackSlider_ = new QSlider(Qt::Horizontal);
    noiseGateAttackSlider_->setRange(1, 200);
    noiseGateAttackSlider_->setValue(5);
    noiseGateAttackValueLabel_ = new QLabel("5 ms");
    grid->addWidget(new QLabel("  Attack"), 5, 0);
    grid->addWidget(noiseGateAttackSlider_, 5, 1);
    grid->addWidget(noiseGateAttackValueLabel_, 5, 2);

    noiseGateReleaseSlider_ = new QSlider(Qt::Horizontal);
    noiseGateReleaseSlider_->setRange(10, 1000);
    noiseGateReleaseSlider_->setValue(80);
    noiseGateReleaseValueLabel_ = new QLabel("80 ms");
    grid->addWidget(new QLabel("  Release"), 6, 0);
    grid->addWidget(noiseGateReleaseSlider_, 6, 1);
    grid->addWidget(noiseGateReleaseValueLabel_, 6, 2);

    compressorEnable_ = new QCheckBox("Compressor");
    compressorThresholdSlider_ = new QSlider(Qt::Horizontal);
    compressorThresholdSlider_->setRange(-60, 0);
    compressorThresholdSlider_->setValue(-20);
    compressorThresholdValueLabel_ = new QLabel("-20 dB");
    grid->addWidget(compressorEnable_, 7, 0);
    grid->addWidget(compressorThresholdSlider_, 7, 1);
    grid->addWidget(compressorThresholdValueLabel_, 7, 2);

    compressorRatioSlider_ = new QSlider(Qt::Horizontal);
    compressorRatioSlider_->setRange(1, 10);
    compressorRatioSlider_->setValue(3);
    compressorRatioValueLabel_ = new QLabel("3:1");
    grid->addWidget(new QLabel("  Ratio"), 8, 0);
    grid->addWidget(compressorRatioSlider_, 8, 1);
    grid->addWidget(compressorRatioValueLabel_, 8, 2);

    grid->addWidget(new QLabel("Voice effect"), 9, 0);
    voiceEffectCombo_ = new QComboBox();
    voiceEffectCombo_->addItem("None", static_cast<int>(VoiceEffect::None));
    voiceEffectCombo_->addItem("Robot Voice", static_cast<int>(VoiceEffect::Robot));
    voiceEffectCombo_->addItem("Echo", static_cast<int>(VoiceEffect::Echo));
    voiceEffectCombo_->addItem("Deep Voice", static_cast<int>(VoiceEffect::DeepVoice));
    voiceEffectCombo_->addItem("Chipmunk", static_cast<int>(VoiceEffect::Chipmunk));
    voiceEffectCombo_->addItem("Distortion", static_cast<int>(VoiceEffect::Distortion));
    grid->addWidget(voiceEffectCombo_, 9, 1, 1, 2);

    // The limiter sits at the end of the chain (it caps whatever the stages
    // above produce), so its row sits at the bottom of the panel too.
    grid->addWidget(limiterEnable_, 10, 0);
    grid->addWidget(limiterCeilingSlider_, 10, 1);
    grid->addWidget(limiterCeilingValueLabel_, 10, 2);

    applyEffectButton_ = new QPushButton("Apply to Selection");
    applyEffectButton_->setFocusPolicy(Qt::NoFocus);
    applyEffectButton_->setToolTip("Destructively apply these filter settings to the current selection (Ctrl+R)");
    grid->addWidget(applyEffectButton_, 11, 0, 1, 3);
    connect(applyEffectButton_, &QPushButton::clicked, this, &MainWindow::onApplyEffect);

    // Ctrl+R mirrors Audacity's "repeat/apply last effect" muscle memory.
    applyEffectAction_ = new QAction("Apply Filters to Selection", this);
    applyEffectAction_->setShortcut(QKeySequence("Ctrl+R"));
    connect(applyEffectAction_, &QAction::triggered, this, &MainWindow::onApplyEffect);

    rootLayout->addWidget(filterGroup);

    connect(limiterEnable_, &QCheckBox::toggled, this, &MainWindow::onFiltersChanged);
    connect(limiterCeilingSlider_, &QSlider::valueChanged, this, &MainWindow::onFiltersChanged);
    connect(gainEnable_, &QCheckBox::toggled, this, &MainWindow::onFiltersChanged);
    connect(gainSlider_, &QSlider::valueChanged, this, &MainWindow::onFiltersChanged);
    connect(highPassEnable_, &QCheckBox::toggled, this, &MainWindow::onFiltersChanged);
    connect(highPassSlider_, &QSlider::valueChanged, this, &MainWindow::onFiltersChanged);
    connect(lowPassEnable_, &QCheckBox::toggled, this, &MainWindow::onFiltersChanged);
    connect(lowPassSlider_, &QSlider::valueChanged, this, &MainWindow::onFiltersChanged);
    connect(noiseGateEnable_, &QCheckBox::toggled, this, &MainWindow::onFiltersChanged);
    connect(noiseGateSlider_, &QSlider::valueChanged, this, &MainWindow::onFiltersChanged);
    connect(noiseGateAttackSlider_, &QSlider::valueChanged, this, &MainWindow::onFiltersChanged);
    connect(noiseGateReleaseSlider_, &QSlider::valueChanged, this, &MainWindow::onFiltersChanged);
    connect(compressorEnable_, &QCheckBox::toggled, this, &MainWindow::onFiltersChanged);
    connect(compressorThresholdSlider_, &QSlider::valueChanged, this, &MainWindow::onFiltersChanged);
    connect(compressorRatioSlider_, &QSlider::valueChanged, this, &MainWindow::onFiltersChanged);
    connect(voiceEffectCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onFiltersChanged);

    setCentralWidget(central);
    buildMenus();
    resize(900, 820);
}

void MainWindow::buildMenus() {
    quitAction_ = new QAction("Quit", this);
    quitAction_->setShortcut(QKeySequence::Quit);
    connect(quitAction_, &QAction::triggered, this, &MainWindow::close);

    QMenu* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction(newProjectAction_);
    fileMenu->addAction(openProjectAction_);
    fileMenu->addAction(saveProjectAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(importAction_);
    fileMenu->addAction(exportAction_);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAction_);

    // Menu-only rather than another toolbar button: the toolbar already
    // overflows on a narrow window, and this is a keyboard action in practice.
    selectAllAction_ = new QAction("Select All", this);
    selectAllAction_->setShortcut(QKeySequence::SelectAll);
    connect(selectAllAction_, &QAction::triggered, this, &MainWindow::onSelectAll);

    QMenu* editMenu = menuBar()->addMenu("&Edit");
    editMenu->addAction(undoAction_);
    editMenu->addAction(redoAction_);
    editMenu->addSeparator();
    editMenu->addAction(selectAllAction_);
    editMenu->addSeparator();
    editMenu->addAction(cutAction_);
    editMenu->addAction(copyAction_);
    editMenu->addAction(pasteAction_);
    editMenu->addAction(deleteAction_);
    editMenu->addSeparator();
    editMenu->addAction(silenceAction_);
    editMenu->addAction(fadeInAction_);
    editMenu->addAction(fadeOutAction_);
    editMenu->addAction(normalizeAction_);
    editMenu->addAction(crossfadeAction_);
    editMenu->addAction(applyEffectAction_);

    QMenu* trackMenu = menuBar()->addMenu("&Tracks");
    trackMenu->addAction(addTrackAction_);
    trackMenu->addAction(removeTrackAction_);
    trackMenu->addSeparator();
    trackMenu->addAction(addLabelAction_);

    QMenu* transportMenu = menuBar()->addMenu("Trans&port");
    transportMenu->addAction(recordAction_);
    transportMenu->addAction(playAction_);

    QMenu* viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction(selectToolAction_);
    viewMenu->addAction(moveToolAction_);
    viewMenu->addAction(envelopeToolAction_);
    viewMenu->addAction(snapAction_);
    viewMenu->addSeparator();
    viewMenu->addAction(zoomInAction_);
    viewMenu->addAction(zoomOutAction_);
    viewMenu->addAction(zoomFitAction_);

    QMenu* helpMenu = menuBar()->addMenu("&Help");
    QAction* aboutAction = helpMenu->addAction("About zrecord");
    connect(aboutAction, &QAction::triggered, this, &MainWindow::onAbout);
}

QString MainWindow::aboutText() {
    return QString("<b>zrecord %1</b><br>"
                   "Multi-track audio recorder and editor.<br>"
                   "Qt %2, %3.")
        .arg(ZRECORD_VERSION, qVersion(), QString::fromUtf8(Pa_GetVersionInfo()->versionText));
}

void MainWindow::onAbout() {
    QMessageBox::about(this, "About zrecord", aboutText());
}

void MainWindow::queryInitialMicVolume() {
    QProcess process;
    process.start("pactl", {"get-source-volume", "@DEFAULT_SOURCE@"});
    if (!process.waitForFinished(1000)) {
        return;
    }
    QString output = QString::fromUtf8(process.readAllStandardOutput());
    QRegularExpression re("(\\d+)%");
    QRegularExpressionMatch match = re.match(output);
    if (match.hasMatch()) {
        int percent = match.captured(1).toInt();
        micVolumeSlider_->blockSignals(true);
        micVolumeSlider_->setValue(percent);
        micVolumeSlider_->blockSignals(false);
        micVolumeValueLabel_->setText(QString("%1%").arg(percent));
    }
}

void MainWindow::onMicVolumeChanged(int value) {
    micVolumeValueLabel_->setText(QString("%1%").arg(value));
    QProcess::startDetached("pactl", {"set-source-volume", "@DEFAULT_SOURCE@", QString("%1%").arg(value)});
}

void MainWindow::refreshDevices() {
    deviceCombo_->clear();
    int defaultDevice = engine_->defaultInputDeviceIndex();
    int defaultComboIndex = -1;
    for (const auto& d : engine_->listInputDevices()) {
        deviceCombo_->addItem(QString::fromStdString(d.name), d.index);
        deviceCombo_->setItemData(deviceCombo_->count() - 1, d.maxInputChannels, kMaxChannelsRole);
        if (d.index == defaultDevice) {
            defaultComboIndex = deviceCombo_->count() - 1;
        }
    }
    if (defaultComboIndex >= 0) {
        deviceCombo_->setCurrentIndex(defaultComboIndex);
    }
    onInputDeviceChanged();
}

int MainWindow::selectedDeviceMaxChannels() const {
    bool ok = false;
    int channels = deviceCombo_->currentData(kMaxChannelsRole).toInt(&ok);
    return ok && channels > 0 ? channels : 2;
}

void MainWindow::onInputDeviceChanged() {
    // Default the new project's channel count from what the device can
    // actually deliver: a mono-only device opened directly (ALSA hw) refuses
    // a stereo stream outright.
    if (!projectHasAnyContent()) {
        int wanted = std::min(2, selectedDeviceMaxChannels());
        int index = channelsCombo_->findData(wanted);
        if (index >= 0) {
            channelsCombo_->setCurrentIndex(index);
        }
    }
}

FilterSettings MainWindow::filterSettingsFromUi() const {
    FilterSettings settings;
    settings.limiterEnabled = limiterEnable_->isChecked();
    settings.limiterCeilingDb = limiterCeilingSlider_->value();
    settings.gainEnabled = gainEnable_->isChecked();
    settings.gainDb = gainSlider_->value();
    settings.highPassEnabled = highPassEnable_->isChecked();
    settings.highPassHz = highPassSlider_->value();
    settings.lowPassEnabled = lowPassEnable_->isChecked();
    settings.lowPassHz = lowPassSlider_->value();
    settings.noiseGateEnabled = noiseGateEnable_->isChecked();
    settings.noiseGateThresholdDb = noiseGateSlider_->value();
    settings.noiseGateAttackMs = noiseGateAttackSlider_->value();
    settings.noiseGateReleaseMs = noiseGateReleaseSlider_->value();
    settings.compressorEnabled = compressorEnable_->isChecked();
    settings.compressorThresholdDb = compressorThresholdSlider_->value();
    settings.compressorRatio = compressorRatioSlider_->value();
    settings.voiceEffect = static_cast<VoiceEffect>(voiceEffectCombo_->currentData().toInt());
    return settings;
}

void MainWindow::applyFilterSettingsFromUi() {
    FilterSettings settings = filterSettingsFromUi();
    engine_->setFilterSettings(settings);

    limiterCeilingValueLabel_->setText(QString("%1 dB").arg(limiterCeilingSlider_->value()));
    gainValueLabel_->setText(QString("%1 dB").arg(gainSlider_->value()));
    highPassValueLabel_->setText(QString("%1 Hz").arg(highPassSlider_->value()));
    lowPassValueLabel_->setText(QString("%1 Hz").arg(lowPassSlider_->value()));
    noiseGateValueLabel_->setText(QString("%1 dB").arg(noiseGateSlider_->value()));
    noiseGateAttackValueLabel_->setText(QString("%1 ms").arg(noiseGateAttackSlider_->value()));
    noiseGateReleaseValueLabel_->setText(QString("%1 ms").arg(noiseGateReleaseSlider_->value()));
    compressorThresholdValueLabel_->setText(QString("%1 dB").arg(compressorThresholdSlider_->value()));
    compressorRatioValueLabel_->setText(QString("%1:1").arg(compressorRatioSlider_->value()));
}

void MainWindow::onFiltersChanged() {
    applyFilterSettingsFromUi();
}

void MainWindow::setControlsEnabled(bool recording) {
    deviceCombo_->setEnabled(!recording);
    exportButton_->setEnabled(!recording);
    playButton_->setEnabled(!recording);
    formatCombo_->setEnabled(!recording);

    // Keep the shortcut-only actions in lockstep with their buttons, so e.g.
    // Space can't start playback in the middle of a take.
    exportAction_->setEnabled(!recording);
    playAction_->setEnabled(!recording);

    // Editing the timeline while the input stream is live would race the
    // take that's being captured, so gate those on recording too.
    for (QAction* action : {newProjectAction_, openProjectAction_, saveProjectAction_,
                             addTrackAction_, removeTrackAction_, importAction_,
                             cutAction_, copyAction_, pasteAction_, deleteAction_,
                             silenceAction_, fadeInAction_, fadeOutAction_, crossfadeAction_,
                             applyEffectAction_, addLabelAction_, selectAllAction_, normalizeAction_}) {
        action->setEnabled(!recording);
    }
    if (!recording) {
        onSelectionChanged(); // restore selection-dependent enablement
    }
    undoAction_->setEnabled(!recording && undoStack_->canUndo());
    redoAction_->setEnabled(!recording && undoStack_->canRedo());

    bool hasContent = projectHasAnyContent();
    channelsCombo_->setEnabled(!recording && !hasContent);
    sampleRateCombo_->setEnabled(!recording && !hasContent);
}

int MainWindow::findArmedTrackIndex() const {
    for (size_t i = 0; i < project_.tracks.size(); ++i) {
        if (project_.tracks[i].recordArmed) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool MainWindow::projectHasAnyContent() const {
    for (const auto& track : project_.tracks) {
        if (!track.clips.empty()) {
            return true;
        }
    }
    return false;
}

void MainWindow::onToggleRecord() {
    if (!engine_->isRecording()) {
        if (deviceCombo_->count() == 0) {
            QMessageBox::warning(this, "No input device", "No input device is available.");
            return;
        }

        int armedIndex = findArmedTrackIndex();
        if (armedIndex < 0) {
            if (project_.tracks.empty()) {
                undoStack_->push(new AddTrackCommand(project_, "Track 1"));
                trackPanel_->refresh();
                armedIndex = 0;
                project_.tracks[0].recordArmed = true;
                trackPanel_->refresh();
            } else {
                QMessageBox::information(this, "No track armed",
                                          "Arm a track for recording first (the ● button in its header).");
                return;
            }
        }

        if (!projectHasAnyContent()) {
            project_.sampleRate = sampleRateCombo_->currentData().toDouble();
            project_.channels = channelsCombo_->currentData().toInt();
        }

        recordingArmedTrackIndex_ = armedIndex;
        int deviceIndex = deviceCombo_->currentData().toInt();
        // Never ask the device for more channels than it has; a mono take
        // for a stereo project is upmixed when it's added.
        recordingChannels_ = std::max(1, std::min(project_.channels, selectedDeviceMaxChannels()));

        // Playback and capture would otherwise fight over the device (and
        // playback's stream kept running under the take, unstoppable once
        // the controls were disabled).
        if (playbackActive_ || engine_->isPlaying()) {
            stopPlaybackNow();
        }

        std::string error;
        if (engine_->startRecording(deviceIndex, recordingChannels_, project_.sampleRate, error)) {
            recordButton_->setText("■  STOP");
            recordButton_->setStyleSheet(
                "QPushButton {"
                "  background-color: #ff1744;"
                "  color: white;"
                "  font-size: 22px;"
                "  font-weight: bold;"
                "  border: 4px solid #ffd600;"
                "  border-radius: 45px;"
                "}"
                "QPushButton:hover { background-color: #ff4569; }"
                "QPushButton:pressed { background-color: #c4001d; }");
            setControlsEnabled(true);
            trackPanel_->beginLiveCapture(armedIndex);
            recordingMuteButton_->setChecked(false); // startRecording() clears the engine's mute
            recordingBar_->show();
        } else {
            QMessageBox::warning(this, "Recording failed", QString::fromStdString(error));
        }
    } else {
        engine_->stopRecording();
        std::vector<float> captured = engine_->copyCapturedBuffer();
        trackPanel_->endLiveCapture();
        recordingBar_->hide();

        if (recordingChannels_ == 1 && project_.channels == 2) {
            std::vector<float> stereo(captured.size() * 2);
            for (size_t i = 0; i < captured.size(); ++i) {
                stereo[2 * i] = stereo[2 * i + 1] = captured[i];
            }
            captured = std::move(stereo);
        }
        if (!captured.empty() && recordingArmedTrackIndex_ >= 0) {
            undoStack_->push(new AppendClipCommand(project_, recordingArmedTrackIndex_, std::move(captured),
                                                   project_.channels, "Record"));
        }
        recordingArmedTrackIndex_ = -1;
        trackPanel_->refresh();
        statusLabel_->setText("Stopped");

        recordButton_->setText("●  RECORD");
        recordButton_->setStyleSheet(
            "QPushButton {"
            "  background-color: #d32f2f;"
            "  color: white;"
            "  font-size: 22px;"
            "  font-weight: bold;"
            "  border: 4px solid #8e0000;"
            "  border-radius: 45px;"
            "}"
            "QPushButton:hover { background-color: #e53935; }"
            "QPushButton:pressed { background-color: #b71c1c; }");
        setControlsEnabled(false);
    }
}

void MainWindow::onTogglePlayback() {
    if (!engine_->isPlaying()) {
        std::string error;
        playbackStartFrame_ = project_.playheadFrame;
        if (engine_->startPlayback(project_, error)) {
            playbackActive_ = true;
            playButton_->setText("■  Stop");
        } else {
            QMessageBox::warning(this, "Playback failed", QString::fromStdString(error));
        }
    } else {
        stopPlaybackNow();
    }
}

void MainWindow::onSeekRequested(int64_t frame) {
    project_.playheadFrame = frame;
    if (playbackActive_ && engine_->isPlaying()) {
        engine_->seekPlayback(frame);
        playbackStartFrame_ = frame; // running out now returns here
    }
    trackPanel_->update();
}

void MainWindow::stopPlaybackNow() {
    engine_->stopPlayback();
    playbackActive_ = false;
    playButton_->setText("▶  Play");
}

void MainWindow::onExport() {
    std::vector<float> buffer = project_.renderMixdown();
    if (buffer.empty()) {
        QMessageBox::information(this, "Nothing to export", "Record or import something first.");
        return;
    }

    auto format = static_cast<AudioFormat>(formatCombo_->currentData().toInt());
    QString ext = AudioFileWriter::extensionFor(format);
    QString defaultPath = QDir::homePath() + "/mixdown." + ext;
    QString path = QFileDialog::getSaveFileName(this, "Export Mixdown", defaultPath, QString("*.%1").arg(ext));
    if (path.isEmpty()) {
        return;
    }

    std::string error;
    bool ok = AudioFileWriter::write(path.toStdString(), buffer, static_cast<int>(project_.sampleRate),
                                      project_.channels, format, error);
    if (ok) {
        QMessageBox::information(this, "Exported", "Mixdown exported to:\n" + path);
    } else {
        QMessageBox::warning(this, "Export failed", QString::fromStdString(error));
    }
}

void MainWindow::onNewProject() {
    if (!confirmDiscardChanges("starting a new project")) {
        return;
    }
    stopPlaybackNow();
    project_.reset();
    undoStack_->clear();
    trackPanel_->refresh();
    setControlsEnabled(false);
    markSaved(QString()); // an empty project has nothing to lose
}

bool MainWindow::hasUnsavedChanges() const {
    return !undoStack_->isClean() || settingsDirty_;
}

void MainWindow::updateWindowTitle() {
    QString name = projectPath_.isEmpty() ? QString("Untitled") : QFileInfo(projectPath_).completeBaseName();
    setWindowTitle(QString("%1[*] \u2014 zrecord").arg(name));
    setWindowModified(hasUnsavedChanges());
}

void MainWindow::markSaved(const QString& path) {
    projectPath_ = path;
    settingsDirty_ = false;
    undoStack_->setClean();
    updateWindowTitle();
}

bool MainWindow::confirmDiscardChanges(const QString& action) {
    if (!hasUnsavedChanges()) {
        return true;
    }
    int answer = 0;
    if (unsavedPrompt_) {
        answer = unsavedPrompt_(action);
    } else {
        answer = QMessageBox::question(
            this, "Unsaved changes",
            QString("The project has unsaved changes. Save them before %1?").arg(action),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    }
    if (answer == QMessageBox::Save) {
        return saveProjectInteractive();
    }
    return answer == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (engine_->isRecording()) {
        onToggleRecord(); // keep the take: stop and add it, then ask as usual
    }
    if (!confirmDiscardChanges("quitting")) {
        event->ignore();
        return;
    }
    stopPlaybackNow();
    event->accept();
}

void MainWindow::onOpenProject() {
    if (!confirmDiscardChanges("opening another project")) {
        return;
    }
    QString dirPath = QFileDialog::getExistingDirectory(this, "Open Project (select a .zrproj folder)", QDir::homePath());
    if (dirPath.isEmpty()) {
        return;
    }
    QString error;
    if (!openProjectFolder(dirPath, &error)) {
        QMessageBox::warning(this, "Open failed", error);
    }
}

bool MainWindow::openProjectFolder(const QString& path, QString* error) {
    // The audio thread reads project_ while playing; don't swap it underneath.
    stopPlaybackNow();
    std::string message;
    if (!ProjectFile::load(project_, path.toStdString(), message)) {
        // load() is all-or-nothing, so the project -- and the undo commands
        // that refer into it -- are still intact.
        if (error != nullptr) {
            *error = QString::fromStdString(message);
        }
        return false;
    }
    undoStack_->clear();
    trackPanel_->refresh();
    trackPanel_->zoomToFit();
    setControlsEnabled(false);
    markSaved(path);
    return true;
}

void MainWindow::onSaveProject() {
    saveProjectInteractive();
}

bool MainWindow::saveProjectInteractive() {
    QString defaultPath = projectPath_.isEmpty() ? QDir::homePath() + "/untitled.zrproj" : projectPath_;
    QString path = QFileDialog::getSaveFileName(this, "Save Project", defaultPath, "zrecord Project (*.zrproj)");
    if (path.isEmpty()) {
        return false;
    }
    if (!path.endsWith(".zrproj")) {
        path += ".zrproj";
    }
    QString error;
    if (!saveProjectTo(path, &error)) {
        QMessageBox::warning(this, "Save failed", error);
        return false;
    }
    statusLabel_->setText("Saved to " + path);
    return true;
}

bool MainWindow::saveProjectTo(const QString& path, QString* error) {
    std::string message;
    if (!ProjectFile::save(project_, path.toStdString(), message)) {
        if (error != nullptr) {
            *error = QString::fromStdString(message);
        }
        return false;
    }
    markSaved(path);
    return true;
}

void MainWindow::onAddTrack() {
    int n = static_cast<int>(project_.tracks.size()) + 1;
    undoStack_->push(new AddTrackCommand(project_, "Track " + std::to_string(n)));
    trackPanel_->refresh();
}

void MainWindow::onRemoveTrack() {
    if (project_.tracks.empty()) {
        return;
    }
    int index = project_.selection.trackIndex >= 0 ? project_.selection.trackIndex
                                                    : static_cast<int>(project_.tracks.size()) - 1;
    undoStack_->push(new RemoveTrackCommand(project_, index));
    trackPanel_->refresh();
}

void MainWindow::onImportAudio() {
    QString path = QFileDialog::getOpenFileName(this, "Import Audio", QDir::homePath(),
                                                 "Audio Files (*.wav *.flac *.ogg *.aiff *.aif)");
    if (path.isEmpty()) {
        return;
    }
    QString error;
    QString note;
    if (!importAudioFile(path, &error, &note)) {
        QMessageBox::warning(this, "Import failed", error);
        return;
    }
    if (!note.isEmpty()) {
        statusLabel_->setText(note);
    }
}

bool MainWindow::importAudioFile(const QString& path, QString* error, QString* note) {
    auto fail = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    std::vector<float> samples;
    int sampleRate = 0;
    int channels = 0;
    std::string readError;
    if (!AudioFileReader::read(path.toStdString(), samples, sampleRate, channels, readError)) {
        return fail(QString::fromStdString(readError));
    }

    // Validate before touching the project, so a rejected file doesn't leave
    // a stray empty "Imported" track behind.
    const bool adoptFileFormat = !projectHasAnyContent();
    if (!adoptFileFormat && channels != project_.channels) {
        return fail(QString("This project is %1-channel; the imported file is %2-channel.")
                        .arg(project_.channels)
                        .arg(channels));
    }
    if (!adoptFileFormat && sampleRate != static_cast<int>(project_.sampleRate)) {
        // Added as-is, a 48 kHz file in a 44.1 kHz project would play back
        // ~9% slow and flat.
        std::vector<float> converted;
        std::string convertError;
        if (!Resampler::convert(samples, channels, sampleRate, project_.sampleRate, converted, convertError)) {
            return fail(QString("Could not convert %1 Hz to %2 Hz: %3")
                            .arg(sampleRate)
                            .arg(static_cast<int>(project_.sampleRate))
                            .arg(QString::fromStdString(convertError)));
        }
        samples = std::move(converted);
        if (note != nullptr) {
            *note = QString("Imported and resampled from %1 Hz to %2 Hz")
                        .arg(sampleRate)
                        .arg(static_cast<int>(project_.sampleRate));
        }
    }

    if (project_.tracks.empty()) {
        undoStack_->push(new AddTrackCommand(project_, "Imported"));
        trackPanel_->refresh();
    }
    if (adoptFileFormat) {
        project_.sampleRate = sampleRate;
        project_.channels = channels;
    }

    int trackIndex = project_.selection.trackIndex >= 0 ? project_.selection.trackIndex
                                                          : static_cast<int>(project_.tracks.size()) - 1;
    undoStack_->push(new AppendClipCommand(project_, trackIndex, std::move(samples), channels, "Import"));
    trackPanel_->refresh();
    setControlsEnabled(false);
    return true;
}

void MainWindow::onCut() {
    if (project_.selection.isEmpty()) {
        return;
    }
    onCopy();
    const Selection sel = project_.selection;
    undoStack_->push(new DeleteSelectionCommand(project_, sel.trackIndex, sel.startFrame, sel.endFrame));
    trackPanel_->refresh();
}

void MainWindow::onCopy() {
    if (project_.selection.isEmpty()) {
        return;
    }
    const Selection sel = project_.selection;
    std::lock_guard<std::mutex> lock(project_.mutex);
    project_.clipboard = Project::copyRange(project_.tracks[static_cast<size_t>(sel.trackIndex)],
                                             sel.startFrame, sel.endFrame, project_.channels);
    pasteAction_->setEnabled(!project_.clipboard.empty());
}

void MainWindow::onPaste() {
    if (project_.clipboard.empty() || project_.tracks.empty()) {
        return;
    }
    int trackIndex = project_.selection.trackIndex >= 0 ? project_.selection.trackIndex : 0;
    undoStack_->push(new PasteCommand(project_, trackIndex, project_.playheadFrame, project_.clipboard, project_.channels));
    trackPanel_->refresh();
}

void MainWindow::onDeleteSelection() {
    if (project_.selection.isEmpty()) {
        return;
    }
    const Selection sel = project_.selection;
    undoStack_->push(new DeleteSelectionCommand(project_, sel.trackIndex, sel.startFrame, sel.endFrame));
    trackPanel_->refresh();
}

void MainWindow::onSilenceSelection() {
    if (project_.selection.isEmpty()) {
        return;
    }
    const Selection sel = project_.selection;
    undoStack_->push(new SilenceSelectionCommand(project_, sel.trackIndex, sel.startFrame, sel.endFrame));
    trackPanel_->refresh();
}

void MainWindow::onFadeIn() {
    if (project_.selection.isEmpty()) {
        return;
    }
    const Selection sel = project_.selection;
    undoStack_->push(new FadeCommand(project_, sel.trackIndex, sel.startFrame, sel.endFrame,
                                      FadeShape::In, project_.channels));
    trackPanel_->refresh();
}

void MainWindow::onFadeOut() {
    if (project_.selection.isEmpty()) {
        return;
    }
    const Selection sel = project_.selection;
    undoStack_->push(new FadeCommand(project_, sel.trackIndex, sel.startFrame, sel.endFrame,
                                      FadeShape::Out, project_.channels));
    trackPanel_->refresh();
}

void MainWindow::onCrossfade() {
    if (project_.selection.isEmpty()) {
        return;
    }
    const Selection sel = project_.selection;
    const int64_t frames = sel.endFrame - sel.startFrame;

    // The selection picks the join and sets the duration: whichever pair of
    // adjacent clips meet inside it get crossfaded over `frames`.
    int firstClipIndex = -1;
    bool tooShort = false;
    {
        std::lock_guard<std::mutex> lock(project_.mutex);
        if (sel.trackIndex < 0 || sel.trackIndex >= static_cast<int>(project_.tracks.size())) {
            return;
        }
        const Track& track = project_.tracks[static_cast<size_t>(sel.trackIndex)];
        for (size_t i = 0; i + 1 < track.clips.size(); ++i) {
            int64_t join = track.clips[i].endFrame();
            if (join != track.clips[i + 1].startFrame) {
                continue; // a gap, not a join
            }
            if (join >= sel.startFrame && join <= sel.endFrame) {
                if (track.clips[i].frameCount() < frames || track.clips[i + 1].frameCount() < frames) {
                    tooShort = true;
                } else {
                    firstClipIndex = static_cast<int>(i);
                }
                break;
            }
        }
    }

    if (tooShort) {
        QMessageBox::information(this, "Crossfade",
                                  "The selection is longer than one of the clips being joined.\n"
                                  "Select a shorter region across the join.");
        return;
    }
    if (firstClipIndex < 0) {
        QMessageBox::information(this, "Crossfade",
                                  "Select a region spanning the join between two adjacent clips "
                                  "on one track.");
        return;
    }

    undoStack_->push(new CrossfadeCommand(project_, sel.trackIndex, firstClipIndex, frames,
                                           project_.channels));
    trackPanel_->refresh();
    onSelectionChanged();
}

void MainWindow::onNormalize() {
    // A time selection wins; otherwise whole clips picked with the Move tool.
    std::vector<GainTarget> targets;
    QString scope;
    if (!project_.selection.isEmpty()) {
        const Selection& sel = project_.selection;
        targets.push_back(GainTarget{sel.trackIndex, sel.startFrame, sel.endFrame});
        scope = QString("the selection (%1 s)").arg(double(sel.endFrame - sel.startFrame) / project_.sampleRate, 0, 'f', 2);
    } else {
        for (const auto& [trackIndex, clipIndex] : trackPanel_->selectedClips()) {
            if (trackIndex < 0 || trackIndex >= static_cast<int>(project_.tracks.size())) {
                continue;
            }
            const auto& clips = project_.tracks[static_cast<size_t>(trackIndex)].clips;
            if (clipIndex >= 0 && clipIndex < static_cast<int>(clips.size())) {
                const Clip& clip = clips[static_cast<size_t>(clipIndex)];
                targets.push_back(GainTarget{trackIndex, clip.startFrame, clip.endFrame()});
            }
        }
        scope = targets.size() == 1 ? QString("1 clip") : QString("%1 clips").arg(targets.size());
    }
    if (targets.empty()) {
        QMessageBox::information(this, "Normalize / Amplify",
                                  "Select a time range, or pick clips with the Move tool, first.");
        return;
    }

    float peak = 0.0f;
    {
        std::lock_guard<std::mutex> lock(project_.mutex);
        peak = measurePeak(project_, targets);
    }
    if (peak <= 0.0f) {
        QMessageBox::information(this, "Normalize / Amplify", "That audio is silent; there is nothing to scale.");
        return;
    }

    NormalizeDialog dialog(peak, scope, this);
    const bool accepted = normalizeDriver_ ? normalizeDriver_(dialog) : dialog.exec() == QDialog::Accepted;
    if (!accepted) {
        return;
    }
    const GainPlan plan = dialog.plan();
    if (std::fabs(plan.gain - 1.0f) < 1e-6f) {
        return; // no change, no undo step
    }
    undoStack_->push(new GainCommand(project_, targets, plan.gain, dialog.actionName()));
    trackPanel_->refresh();
}

void MainWindow::onApplyEffect() {
    if (project_.selection.isEmpty()) {
        return;
    }
    const Selection sel = project_.selection;
    undoStack_->push(new ApplyEffectCommand(project_, sel.trackIndex, sel.startFrame, sel.endFrame,
                                             filterSettingsFromUi(), project_.sampleRate, project_.channels));
    trackPanel_->refresh();
}

void MainWindow::onClipsMoveRequested(const std::vector<ClipMove>& moves) {
    if (moves.empty()) {
        return;
    }
    undoStack_->push(new MoveClipsCommand(project_, moves));
    trackPanel_->refresh();
    onSelectionChanged(); // the move clears the selection
}

void MainWindow::onAddLabel() {
    Label label;
    if (!project_.selection.isEmpty()) {
        label.startFrame = project_.selection.startFrame;
        label.endFrame = project_.selection.endFrame;
    } else {
        label.startFrame = project_.playheadFrame;
        label.endFrame = project_.playheadFrame; // a point marker
    }

    bool accepted = false;
    QString text = QInputDialog::getText(this, "Add Label", "Label text:", QLineEdit::Normal,
                                          QString(), &accepted);
    if (!accepted || text.trimmed().isEmpty()) {
        return;
    }
    label.text = text.trimmed().toStdString();

    undoStack_->push(new AddLabelCommand(project_, label));
    trackPanel_->update();
}

void MainWindow::onLabelActivated(int labelIndex) {
    if (labelIndex < 0 || labelIndex >= static_cast<int>(project_.labels.size())) {
        return;
    }
    const QString current = QString::fromStdString(project_.labels[static_cast<size_t>(labelIndex)].text);

    bool accepted = false;
    QString text = QInputDialog::getText(this, "Rename Label", "Label text:", QLineEdit::Normal,
                                          current, &accepted);
    if (!accepted) {
        return;
    }
    if (text.trimmed().isEmpty()) {
        // Clearing the text is the natural way to say "remove this".
        undoStack_->push(new RemoveLabelCommand(project_, labelIndex));
    } else if (text.trimmed() != current) {
        undoStack_->push(new RenameLabelCommand(project_, labelIndex, text.trimmed().toStdString()));
    }
    trackPanel_->update();
}

void MainWindow::onLabelContextMenu(int labelIndex, const QPoint& globalPos) {
    if (labelIndex < 0 || labelIndex >= static_cast<int>(project_.labels.size())) {
        return;
    }
    QMenu menu(this);
    QAction* rename = menu.addAction("Rename...");
    QAction* remove = menu.addAction("Delete");
    QAction* chosen = menu.exec(globalPos);

    if (chosen == rename) {
        onLabelActivated(labelIndex);
    } else if (chosen == remove) {
        undoStack_->push(new RemoveLabelCommand(project_, labelIndex));
        trackPanel_->update();
    }
}

void MainWindow::onSelectAll() {
    // Whichever track the user is working on: the selected one, else the armed
    // one, else the first. Selection is per-track, so "all" means all of one
    // track rather than the whole project.
    int trackIndex = project_.selection.trackIndex;
    if (trackIndex < 0) {
        trackIndex = findArmedTrackIndex();
    }
    if (trackIndex < 0 && !project_.tracks.empty()) {
        trackIndex = 0;
    }
    if (trackIndex < 0 || trackIndex >= static_cast<int>(project_.tracks.size())) {
        return;
    }

    int64_t end = 0;
    {
        std::lock_guard<std::mutex> lock(project_.mutex);
        end = project_.tracks[static_cast<size_t>(trackIndex)].endFrame();
    }
    if (end <= 0) {
        return; // an empty track has no extent to select
    }

    project_.selection.trackIndex = trackIndex;
    project_.selection.startFrame = 0;
    project_.selection.endFrame = end;
    trackPanel_->update();
    onSelectionChanged();
}

void MainWindow::onEnvelopeEdited(int trackIndex, const std::vector<EnvelopePoint>& before,
                                   const std::vector<EnvelopePoint>& after, const QString& what) {
    undoStack_->push(new EnvelopeEditCommand(project_, trackIndex, before, after, what));
    trackPanel_->update();
}

void MainWindow::onSelectionChanged() {
    bool hasSelection = !project_.selection.isEmpty();
    cutAction_->setEnabled(hasSelection);
    copyAction_->setEnabled(hasSelection);
    deleteAction_->setEnabled(hasSelection);
    silenceAction_->setEnabled(hasSelection);
    fadeInAction_->setEnabled(hasSelection);
    fadeOutAction_->setEnabled(hasSelection);
    crossfadeAction_->setEnabled(hasSelection);
    // The button is a plain QPushButton (it isn't driven by the action), so
    // both need setting or the menu entry advertises itself as available
    // while the button is greyed out.
    applyEffectAction_->setEnabled(hasSelection);
    applyEffectButton_->setEnabled(hasSelection);
    if (normalizeAction_ != nullptr) {
        normalizeAction_->setEnabled(projectHasAnyContent());
    }
    if (selectAllAction_ != nullptr) {
        selectAllAction_->setEnabled(projectHasAnyContent());
    }
    pasteAction_->setEnabled(!project_.clipboard.empty());
}

void MainWindow::onTick() {
    // Input while recording, output while playing; otherwise it falls away.
    levelMeter_->setPeak(engine_->takeMeterPeak());

    if (engine_->isRecording()) {
        double captured = engine_->capturedSeconds();
        statusLabel_->setText(QString("Recording... %1").arg(formatDuration(captured)));

        // Blink at 1 Hz off the take's own clock, so the indicator can't
        // drift from the elapsed time it sits next to. A muted take shows a
        // steady amber instead, to read clearly as "running but capturing
        // nothing" rather than as a stopped recording.
        bool muted = engine_->isInputMuted();
        bool blinkOn = std::fmod(captured, 1.0) < 0.5;
        recordingIndicator_->setText(QString("%1  %2  %3")
                                          .arg(QString::fromUtf8("\xE2\x97\x8F"),
                                               muted ? "MUTED" : "REC",
                                               formatDuration(captured)));
        recordingIndicator_->setStyleSheet(
            QString("font-weight: bold; color: %1;")
                .arg(muted ? "#ffa000" : (blinkOn ? "#ff1744" : "#7a1226")));

        std::vector<float> newSamples = engine_->consumeNewSamples();
        if (!newSamples.empty()) {
            float minVal = newSamples.front();
            float maxVal = newSamples.front();
            for (float sample : newSamples) {
                minVal = std::min(minVal, sample);
                maxVal = std::max(maxVal, sample);
            }
            trackPanel_->pushLiveColumn(minVal, maxVal);
        }
    }

    // Playback that reached the end on its own: release the finished stream
    // (it used to stay open until the next Play overwrote and leaked it).
    // Played to the end, the playhead goes back to where playback started,
    // ready to play the same passage again.
    if (playbackActive_ && !engine_->isPlaying()) {
        stopPlaybackNow();
        project_.playheadFrame = playbackStartFrame_;
        trackPanel_->update();
    } else if (playbackActive_) {
        // Let playback hear edits made since the last tick.
        engine_->refreshPlayback();
        // The playhead follows what's being heard, and the view follows it.
        project_.playheadFrame = engine_->playbackFrame();
        trackPanel_->followPlayhead(project_.playheadFrame);
    }
}

} // namespace zrecord
