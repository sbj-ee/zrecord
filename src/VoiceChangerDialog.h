#pragma once

#include <QDialog>

#include "VoiceChanger.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace zrecord {

// Voice Changer: pitch (semitones, duration kept), formant shift and robot,
// with presets. Preview asks the owner to play the processed selection
// (previewRequested / previewStopRequested); the owner reports back through
// setPreviewPlaying() so the button reads Preview or Stop. OK is "Apply".
class VoiceChangerDialog : public QDialog {
    Q_OBJECT

public:
    explicit VoiceChangerDialog(const QString& scope, QWidget* parent = nullptr);

    VoiceSettings settings() const;
    VoicePreset preset() const;

    // For tests and scripted screenshots.
    void setPreset(VoicePreset preset);
    void setPitch(double semitones);
    void setFormant(double semitones);
    void setRobot(bool robot);
    QPushButton* previewButton() const { return previewButton_; }
    QPushButton* applyButton() const;
    bool previewPlaying() const { return previewPlaying_; }

public slots:
    void setPreviewPlaying(bool playing);

signals:
    void previewRequested(const zrecord::VoiceSettings& settings);
    void previewStopRequested();

private:
    void onPresetChanged(int index);
    void onValueEdited();
    void updateState();

    QComboBox* presetCombo_ = nullptr;
    QDoubleSpinBox* pitchSpin_ = nullptr;
    QDoubleSpinBox* formantSpin_ = nullptr;
    QCheckBox* robotCheck_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QPushButton* previewButton_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    bool previewPlaying_ = false;
    bool loadingPreset_ = false;
};

} // namespace zrecord
