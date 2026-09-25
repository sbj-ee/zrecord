#pragma once

#include <QDialog>

#include "Gain.h"

class QCheckBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QRadioButton;

namespace zrecord {

// Normalize (scale to a target peak) or Amplify (scale by a fixed gain) the
// current selection or selected clips. Shows the current and resulting peak
// in dBFS and a clip indicator when the result would pass full scale; OK
// then needs "Allow clipping", as in Audacity's Amplify.
class NormalizeDialog : public QDialog {
    Q_OBJECT

public:
    enum class Mode { Normalize, Amplify };

    NormalizeDialog(float currentPeak, const QString& scope, QWidget* parent = nullptr);

    GainPlan plan() const;
    Mode mode() const;
    QString actionName() const; // "Normalize" or "Amplify", for the undo entry

    // For tests and scripted screenshots.
    void setMode(Mode mode);
    void setTargetDb(double db);
    void setGainDb(double db);
    void setAllowClipping(bool allow);
    bool clipIndicatorShown() const;
    bool acceptEnabled() const;

private:
    void updatePreview();

    float currentPeak_;
    QRadioButton* normalizeRadio_ = nullptr;
    QRadioButton* amplifyRadio_ = nullptr;
    QDoubleSpinBox* targetSpin_ = nullptr;
    QDoubleSpinBox* gainSpin_ = nullptr;
    QLabel* resultLabel_ = nullptr;
    QLabel* clipLabel_ = nullptr;
    QCheckBox* allowClipping_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

} // namespace zrecord
