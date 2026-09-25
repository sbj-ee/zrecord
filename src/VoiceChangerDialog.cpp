#include "VoiceChangerDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <cmath>

namespace zrecord {

namespace {

constexpr VoicePreset kPresets[] = {VoicePreset::Deeper, VoicePreset::Higher, VoicePreset::Robot,
                                    VoicePreset::Chipmunk, VoicePreset::Custom};

QDoubleSpinBox* makeSemitoneSpin(QWidget* parent) {
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(-12.0, 12.0);
    spin->setDecimals(1);
    spin->setSingleStep(0.5);
    spin->setSuffix(" st");
    return spin;
}

QString signedSt(double v) {
    return QString("%1%2 st").arg(v > 0.0 ? "+" : "").arg(v, 0, 'f', 1);
}

} // namespace

VoiceChangerDialog::VoiceChangerDialog(const QString& scope, QWidget* parent) : QDialog(parent) {
    setWindowTitle("Voice Changer");
    auto* layout = new QVBoxLayout(this);

    auto* scopeLabel = new QLabel(QString("Applies to: %1").arg(scope), this);
    layout->addWidget(scopeLabel);

    auto* form = new QFormLayout;
    presetCombo_ = new QComboBox(this);
    for (VoicePreset preset : kPresets) {
        presetCombo_->addItem(voicePresetName(preset), static_cast<int>(preset));
    }
    form->addRow("Preset:", presetCombo_);

    pitchSpin_ = makeSemitoneSpin(this);
    pitchSpin_->setToolTip("Pitch shift in semitones; the length of the audio is kept");
    form->addRow("Pitch:", pitchSpin_);

    formantSpin_ = makeSemitoneSpin(this);
    formantSpin_->setToolTip("Moves the vocal-tract resonances. 0 keeps them where they were "
                             "(natural); equal to Pitch moves them along (cartoon)");
    form->addRow("Formant:", formantSpin_);

    robotCheck_ = new QCheckBox("Robot (monotone buzz)", this);
    form->addRow(QString(), robotCheck_);
    layout->addLayout(form);

    summaryLabel_ = new QLabel(this);
    summaryLabel_->setStyleSheet("QLabel { color: palette(mid); }");
    summaryLabel_->setWordWrap(true);
    layout->addWidget(summaryLabel_);

    auto* bottom = new QHBoxLayout;
    previewButton_ = new QPushButton(this);
    previewButton_->setAutoDefault(false);
    previewButton_->setToolTip("Play the selection with these settings, without changing it");
    bottom->addWidget(previewButton_);
    bottom->addStretch(1);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText("Apply");
    bottom->addWidget(buttons_);
    layout->addLayout(bottom);

    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(presetCombo_, &QComboBox::currentIndexChanged, this, &VoiceChangerDialog::onPresetChanged);
    connect(pitchSpin_, &QDoubleSpinBox::valueChanged, this, &VoiceChangerDialog::onValueEdited);
    connect(formantSpin_, &QDoubleSpinBox::valueChanged, this, &VoiceChangerDialog::onValueEdited);
    connect(robotCheck_, &QCheckBox::toggled, this, &VoiceChangerDialog::onValueEdited);
    connect(previewButton_, &QPushButton::clicked, this, [this] {
        if (previewPlaying_) {
            emit previewStopRequested();
        } else {
            emit previewRequested(settings());
        }
    });

    onPresetChanged(presetCombo_->currentIndex()); // Deeper
    setPreviewPlaying(false);
}

VoiceSettings VoiceChangerDialog::settings() const {
    VoiceSettings s;
    s.pitchSemitones = static_cast<float>(pitchSpin_->value());
    s.formantSemitones = static_cast<float>(formantSpin_->value());
    s.robot = robotCheck_->isChecked();
    return s;
}

VoicePreset VoiceChangerDialog::preset() const {
    return static_cast<VoicePreset>(presetCombo_->currentData().toInt());
}

void VoiceChangerDialog::setPreset(VoicePreset preset) {
    presetCombo_->setCurrentIndex(presetCombo_->findData(static_cast<int>(preset)));
}

void VoiceChangerDialog::setPitch(double semitones) { pitchSpin_->setValue(semitones); }
void VoiceChangerDialog::setFormant(double semitones) { formantSpin_->setValue(semitones); }
void VoiceChangerDialog::setRobot(bool robot) { robotCheck_->setChecked(robot); }

QPushButton* VoiceChangerDialog::applyButton() const { return buttons_->button(QDialogButtonBox::Ok); }

void VoiceChangerDialog::setPreviewPlaying(bool playing) {
    previewPlaying_ = playing;
    previewButton_->setText(playing ? QString::fromUtf8("\xE2\x96\xA0  Stop Preview")
                                    : QString::fromUtf8("\xE2\x96\xB6  Preview"));
    updateState();
}

void VoiceChangerDialog::onPresetChanged(int) {
    const VoicePreset p = preset();
    if (p != VoicePreset::Custom) {
        // Loading a preset's values is not a user edit: keep the combo on it.
        loadingPreset_ = true;
        const VoiceSettings s = voicePresetSettings(p);
        pitchSpin_->setValue(s.pitchSemitones);
        formantSpin_->setValue(s.formantSemitones);
        robotCheck_->setChecked(s.robot);
        loadingPreset_ = false;
    }
    updateState();
}

void VoiceChangerDialog::onValueEdited() {
    if (!loadingPreset_ && preset() != VoicePreset::Custom) {
        const QSignalBlocker block(presetCombo_);
        setPreset(VoicePreset::Custom);
    }
    updateState();
}

void VoiceChangerDialog::updateState() {
    const VoiceSettings s = settings();
    QString text;
    if (s.isIdentity()) {
        text = "No change: every control is at zero.";
    } else if (s.robot) {
        text = QString("Robot: a monotone buzz, pitch %1, formants %2. Length is unchanged.")
                   .arg(signedSt(s.pitchSemitones), signedSt(s.formantSemitones));
    } else {
        text = QString("Pitch %1 (x%2), formants %3. Length is unchanged.")
                   .arg(signedSt(s.pitchSemitones))
                   .arg(std::pow(2.0, s.pitchSemitones / 12.0), 0, 'f', 3)
                   .arg(signedSt(s.formantSemitones));
    }
    summaryLabel_->setText(text);
    applyButton()->setEnabled(!s.isIdentity());
    previewButton_->setEnabled(!s.isIdentity() || previewPlaying_);
}

} // namespace zrecord
