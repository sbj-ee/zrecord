#include "MainWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

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

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    engine_ = std::make_unique<AudioEngine>();
    buildUi();
    refreshDevices();
    applyFilterSettingsFromUi();
    setControlsEnabled(false);
    queryInitialMicVolume();

    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &MainWindow::onTick);
    timer_->start(50);
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    setWindowTitle("zrecord");

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
    connect(recordButton_, &QPushButton::clicked, this, &MainWindow::onToggleRecord);

    auto* recordRow = new QHBoxLayout();
    recordRow->addStretch();
    recordRow->addWidget(recordButton_);
    recordRow->addStretch();
    rootLayout->addLayout(recordRow);

    // Level meter + status.
    levelMeter_ = new QProgressBar();
    levelMeter_->setRange(0, 100);
    levelMeter_->setTextVisible(false);
    rootLayout->addWidget(levelMeter_);

    waveformView_ = new WaveformView();
    waveformView_->setStatusText("Ready");
    rootLayout->addWidget(waveformView_);

    // Playback / save row.
    auto* secondaryRow = new QHBoxLayout();
    playButton_ = new QPushButton("▶  Play");
    connect(playButton_, &QPushButton::clicked, this, &MainWindow::onTogglePlayback);
    secondaryRow->addWidget(playButton_);

    secondaryRow->addWidget(new QLabel("Save as:"));
    formatCombo_ = new QComboBox();
    formatCombo_->addItem(AudioFileWriter::nameFor(AudioFormat::Wav), static_cast<int>(AudioFormat::Wav));
    formatCombo_->addItem(AudioFileWriter::nameFor(AudioFormat::Flac), static_cast<int>(AudioFormat::Flac));
    formatCombo_->addItem(AudioFileWriter::nameFor(AudioFormat::OggVorbis), static_cast<int>(AudioFormat::OggVorbis));
    formatCombo_->addItem(AudioFileWriter::nameFor(AudioFormat::Mp3), static_cast<int>(AudioFormat::Mp3));
    secondaryRow->addWidget(formatCombo_);

    saveButton_ = new QPushButton("Save As...");
    connect(saveButton_, &QPushButton::clicked, this, &MainWindow::onSaveAs);
    secondaryRow->addWidget(saveButton_);

    rootLayout->addLayout(secondaryRow);

    // Filters panel.
    auto* filterGroup = new QGroupBox("Filters");
    auto* grid = new QGridLayout(filterGroup);

    limiterEnable_ = new QCheckBox("Limiter (prevent clipping)");
    limiterCeilingSlider_ = new QSlider(Qt::Horizontal);
    limiterCeilingSlider_->setRange(-12, 0);
    limiterCeilingSlider_->setValue(-1);
    limiterCeilingValueLabel_ = new QLabel("-1 dB");
    grid->addWidget(limiterEnable_, 0, 0);
    grid->addWidget(limiterCeilingSlider_, 0, 1);
    grid->addWidget(limiterCeilingValueLabel_, 0, 2);

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
    resize(560, 620);
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
        if (d.index == defaultDevice) {
            defaultComboIndex = deviceCombo_->count() - 1;
        }
    }
    if (defaultComboIndex >= 0) {
        deviceCombo_->setCurrentIndex(defaultComboIndex);
    }
}

void MainWindow::applyFilterSettingsFromUi() {
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
    channelsCombo_->setEnabled(!recording);
    sampleRateCombo_->setEnabled(!recording);
    saveButton_->setEnabled(!recording);
    playButton_->setEnabled(!recording);
    formatCombo_->setEnabled(!recording);
}

void MainWindow::onToggleRecord() {
    if (!engine_->isRecording()) {
        if (deviceCombo_->count() == 0) {
            QMessageBox::warning(this, "No input device", "No input device is available.");
            return;
        }
        int deviceIndex = deviceCombo_->currentData().toInt();
        int channels = channelsCombo_->currentData().toInt();
        double sampleRate = sampleRateCombo_->currentData().toDouble();

        std::string error;
        if (engine_->startRecording(deviceIndex, channels, sampleRate, error)) {
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
            waveformView_->clear();
            waveformView_->setStatusText("Recording... 00:00:00");
        } else {
            QMessageBox::warning(this, "Recording failed", QString::fromStdString(error));
        }
    } else {
        engine_->stopRecording();
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
        waveformView_->setStatusText(QString("Stopped - %1 recorded").arg(formatDuration(engine_->recordedSeconds())));
    }
}

void MainWindow::onTogglePlayback() {
    if (!engine_->isPlaying()) {
        std::string error;
        if (engine_->startPlayback(error)) {
            playButton_->setText("■  Stop");
        } else {
            QMessageBox::warning(this, "Playback failed", QString::fromStdString(error));
        }
    } else {
        engine_->stopPlayback();
        playButton_->setText("▶  Play");
    }
}

void MainWindow::onSaveAs() {
    std::vector<float> buffer = engine_->copyRecordedBuffer();
    if (buffer.empty()) {
        QMessageBox::information(this, "Nothing to save", "Record something first.");
        return;
    }

    auto format = static_cast<AudioFormat>(formatCombo_->currentData().toInt());
    QString ext = AudioFileWriter::extensionFor(format);
    QString defaultPath = QDir::homePath() + "/recording." + ext;
    QString path = QFileDialog::getSaveFileName(this, "Save Recording", defaultPath,
                                                 QString("*.%1").arg(ext));
    if (path.isEmpty()) {
        return;
    }

    std::string error;
    bool ok = AudioFileWriter::write(path.toStdString(), buffer, static_cast<int>(engine_->sampleRate()),
                                      engine_->channels(), format, error);
    if (ok) {
        QMessageBox::information(this, "Saved", "Recording saved to:\n" + path);
    } else {
        QMessageBox::warning(this, "Save failed", QString::fromStdString(error));
    }
}

void MainWindow::onTick() {
    float peak = engine_->peakLevel();
    levelMeter_->setValue(static_cast<int>(std::min(1.0f, peak) * 100.0f));

    if (engine_->isRecording()) {
        waveformView_->setStatusText(QString("Recording... %1").arg(formatDuration(engine_->recordedSeconds())));

        std::vector<float> newSamples = engine_->consumeNewSamples();
        if (!newSamples.empty()) {
            float minVal = newSamples.front();
            float maxVal = newSamples.front();
            for (float sample : newSamples) {
                minVal = std::min(minVal, sample);
                maxVal = std::max(maxVal, sample);
            }
            waveformView_->pushColumn(minVal, maxVal);
        }
    }

    if (!engine_->isPlaying() && playButton_->text() != "▶  Play" && !engine_->isRecording()) {
        playButton_->setText("▶  Play");
    }
}

} // namespace zrecord
