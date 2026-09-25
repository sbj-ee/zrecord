#include "NormalizeDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <cmath>

namespace zrecord {

namespace {

QString dbText(float linear) {
    const float db = linearToDb(linear);
    if (std::isinf(db)) {
        return QString::fromUtf8("\xE2\x88\x92\xE2\x88\x9E dBFS"); // −∞
    }
    return QString("%1%2 dBFS").arg(db > 0.0f ? "+" : "").arg(static_cast<double>(db), 0, 'f', 1);
}

} // namespace

NormalizeDialog::NormalizeDialog(float currentPeak, const QString& scope, QWidget* parent)
    : QDialog(parent), currentPeak_(currentPeak) {
    setWindowTitle("Normalize / Amplify");
    auto* layout = new QVBoxLayout(this);

    auto* scopeLabel = new QLabel(QString("Applies to: %1<br>Current peak: <b>%2</b>").arg(scope, dbText(currentPeak)), this);
    layout->addWidget(scopeLabel);

    auto* grid = new QGridLayout;
    normalizeRadio_ = new QRadioButton("Normalize peak to", this);
    amplifyRadio_ = new QRadioButton("Amplify by", this);
    targetSpin_ = new QDoubleSpinBox(this);
    targetSpin_->setRange(-60.0, 12.0);
    targetSpin_->setDecimals(1);
    targetSpin_->setSingleStep(0.5);
    targetSpin_->setSuffix(" dBFS");
    targetSpin_->setValue(-1.0);
    gainSpin_ = new QDoubleSpinBox(this);
    gainSpin_->setRange(-60.0, 40.0);
    gainSpin_->setDecimals(1);
    gainSpin_->setSingleStep(0.5);
    gainSpin_->setSuffix(" dB");
    // Start Amplify at the gain that would reach 0 dBFS, like Audacity.
    gainSpin_->setValue(currentPeak > 0.0f ? std::round(-linearToDb(currentPeak) * 10.0f) / 10.0f : 0.0);
    grid->addWidget(normalizeRadio_, 0, 0);
    grid->addWidget(targetSpin_, 0, 1);
    grid->addWidget(amplifyRadio_, 1, 0);
    grid->addWidget(gainSpin_, 1, 1);
    layout->addLayout(grid);

    resultLabel_ = new QLabel(this);
    layout->addWidget(resultLabel_);

    clipLabel_ = new QLabel(this);
    clipLabel_->setStyleSheet("QLabel { background-color: #d32f2f; color: white; font-weight: bold;"
                              " padding: 4px 8px; border-radius: 3px; }");
    layout->addWidget(clipLabel_);

    allowClipping_ = new QCheckBox("Allow clipping", this);
    layout->addWidget(allowClipping_);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    normalizeRadio_->setChecked(true);
    for (QRadioButton* radio : {normalizeRadio_, amplifyRadio_}) {
        connect(radio, &QRadioButton::toggled, this, &NormalizeDialog::updatePreview);
    }
    connect(targetSpin_, &QDoubleSpinBox::valueChanged, this, &NormalizeDialog::updatePreview);
    connect(gainSpin_, &QDoubleSpinBox::valueChanged, this, &NormalizeDialog::updatePreview);
    connect(allowClipping_, &QCheckBox::toggled, this, &NormalizeDialog::updatePreview);
    updatePreview();
}

GainPlan NormalizeDialog::plan() const {
    if (mode() == Mode::Normalize) {
        return planNormalize(currentPeak_, static_cast<float>(targetSpin_->value()));
    }
    return planAmplify(currentPeak_, static_cast<float>(gainSpin_->value()));
}

NormalizeDialog::Mode NormalizeDialog::mode() const {
    return normalizeRadio_->isChecked() ? Mode::Normalize : Mode::Amplify;
}

QString NormalizeDialog::actionName() const {
    return mode() == Mode::Normalize ? "Normalize" : "Amplify";
}

void NormalizeDialog::setMode(Mode mode) {
    (mode == Mode::Normalize ? normalizeRadio_ : amplifyRadio_)->setChecked(true);
}

void NormalizeDialog::setTargetDb(double db) {
    targetSpin_->setValue(db);
}

void NormalizeDialog::setGainDb(double db) {
    gainSpin_->setValue(db);
}

void NormalizeDialog::setAllowClipping(bool allow) {
    allowClipping_->setChecked(allow);
}

bool NormalizeDialog::clipIndicatorShown() const {
    return !clipLabel_->isHidden();
}

bool NormalizeDialog::acceptEnabled() const {
    return buttons_->button(QDialogButtonBox::Ok)->isEnabled();
}

void NormalizeDialog::updatePreview() {
    targetSpin_->setEnabled(mode() == Mode::Normalize);
    gainSpin_->setEnabled(mode() == Mode::Amplify);

    const GainPlan p = plan();
    const float gainDb = linearToDb(p.gain);
    resultLabel_->setText(QString("Gain: %1%2 dB    New peak: <b>%3</b>")
                              .arg(gainDb > 0.0f ? "+" : "")
                              .arg(static_cast<double>(gainDb), 0, 'f', 1)
                              .arg(dbText(p.resultingPeak)));

    const bool clips = p.clips();
    clipLabel_->setText(QString::fromUtf8("\xE2\x97\x8F CLIP \xE2\x80\x94 the new peak is %1 dB over full scale")
                            .arg(static_cast<double>(linearToDb(p.resultingPeak)), 0, 'f', 1));
    clipLabel_->setVisible(clips);
    allowClipping_->setVisible(clips);
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(currentPeak_ > 0.0f && (!clips || allowClipping_->isChecked()));
}

} // namespace zrecord
