#pragma once

#include <QDialog>
#include <QString>

#include <vector>

#include "Effects.h"

class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QToolButton;
class QVBoxLayout;

namespace zrecord {

// Edits one track's effect stack: add, remove, reorder, bypass (the check
// box on each row), and each effect's parameters. Every edit is emitted at
// once (effectsChanged) so playback can follow it live; the caller turns the
// final result into one undo step, or restores the original on Cancel.
// "Apply to Audio" closes the dialog with kBakeResult: keep the stack and
// bake it into the clips.
class TrackEffectsDialog : public QDialog {
    Q_OBJECT

public:
    static constexpr int kBakeResult = 2; // done() code for "Apply to Audio"

    TrackEffectsDialog(const QString& trackName, std::vector<Effect> effects, QWidget* parent = nullptr);

    const std::vector<Effect>& effects() const { return effects_; }
    int currentRow() const;

    // The dialog's own operations (also what its buttons call).
    void addEffect(EffectType type);
    void removeCurrent();
    void moveCurrent(int delta); // -1 up, +1 down
    void setCurrentRow(int row);
    void setBypassed(int row, bool bypassed);
    void setParameter(int paramIndex, double value); // of the current effect

signals:
    void effectsChanged();

private:
    void rebuildList();
    void rebuildParams();
    void changed();

    std::vector<Effect> effects_;
    QListWidget* list_ = nullptr;
    QToolButton* addButton_ = nullptr;
    QToolButton* removeButton_ = nullptr;
    QToolButton* upButton_ = nullptr;
    QToolButton* downButton_ = nullptr;
    QPushButton* bakeButton_ = nullptr;
    QGroupBox* paramsBox_ = nullptr;
    QVBoxLayout* paramsLayout_ = nullptr;
    std::vector<QDoubleSpinBox*> spins_;
    std::vector<QSlider*> sliders_;
    bool updating_ = false;
};

} // namespace zrecord
