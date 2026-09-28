#include "TrackEffectsDialog.h"

#include <QAction>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace zrecord {

namespace {
constexpr int kSliderSteps = 1000;

int toSlider(const EffectParamInfo& info, double value) {
    const double t = (value - info.minValue) / (info.maxValue - info.minValue);
    return static_cast<int>(std::lround(std::clamp(t, 0.0, 1.0) * kSliderSteps));
}
double fromSlider(const EffectParamInfo& info, int position) {
    const double v = info.minValue + (info.maxValue - info.minValue) * position / double(kSliderSteps);
    const double scale = std::pow(10.0, info.decimals);
    return std::round(v * scale) / scale;
}
} // namespace

TrackEffectsDialog::TrackEffectsDialog(const QString& trackName, std::vector<Effect> effects, QWidget* parent)
    : QDialog(parent), effects_(std::move(effects)) {
    setWindowTitle(QString("Effects \u2014 %1").arg(trackName));
    setObjectName("trackEffectsDialog");
    auto* root = new QVBoxLayout(this);

    auto* hint = new QLabel("Effects play in order, top to bottom, on playback and in exports. The recording "
                            "itself stays raw until you apply them to the audio. Untick an effect to bypass it.");
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* body = new QHBoxLayout();
    auto* left = new QVBoxLayout();
    list_ = new QListWidget();
    list_->setObjectName("effectList");
    list_->setMinimumWidth(200);
    left->addWidget(list_);

    auto* tools = new QHBoxLayout();
    addButton_ = new QToolButton();
    addButton_->setObjectName("addEffect");
    addButton_->setText("Add \u25be");
    addButton_->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(addButton_);
    for (int i = 0; i < kEffectTypeCount; ++i) {
        const auto type = static_cast<EffectType>(i);
        QAction* action = menu->addAction(effectInfo(type).name);
        action->setObjectName(QString("add_%1").arg(effectInfo(type).key));
        connect(action, &QAction::triggered, this, [this, type] { addEffect(type); });
    }
    addButton_->setMenu(menu);
    tools->addWidget(addButton_);
    removeButton_ = new QToolButton();
    removeButton_->setObjectName("removeEffect");
    removeButton_->setText("Remove");
    connect(removeButton_, &QToolButton::clicked, this, &TrackEffectsDialog::removeCurrent);
    tools->addWidget(removeButton_);
    upButton_ = new QToolButton();
    upButton_->setObjectName("moveEffectUp");
    upButton_->setText("\u25b2");
    upButton_->setToolTip("Move up (earlier in the chain)");
    connect(upButton_, &QToolButton::clicked, this, [this] { moveCurrent(-1); });
    tools->addWidget(upButton_);
    downButton_ = new QToolButton();
    downButton_->setObjectName("moveEffectDown");
    downButton_->setText("\u25bc");
    downButton_->setToolTip("Move down (later in the chain)");
    connect(downButton_, &QToolButton::clicked, this, [this] { moveCurrent(+1); });
    tools->addWidget(downButton_);
    tools->addStretch();
    left->addLayout(tools);
    body->addLayout(left);

    paramsBox_ = new QGroupBox();
    paramsBox_->setObjectName("effectParams");
    paramsBox_->setMinimumWidth(320);
    paramsLayout_ = new QVBoxLayout(paramsBox_);
    body->addWidget(paramsBox_, 1);
    root->addLayout(body, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bakeButton_ = buttons->addButton("Apply to Audio", QDialogButtonBox::ActionRole);
    bakeButton_->setObjectName("bakeEffects");
    bakeButton_->setToolTip("Render these effects into the track's clips (one undo step) and clear the stack");
    connect(bakeButton_, &QPushButton::clicked, this, [this] { done(kBakeResult); });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int) {
        if (!updating_) rebuildParams();
    });
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (updating_) return;
        setBypassed(list_->row(item), item->checkState() != Qt::Checked);
    });

    rebuildList();
    setCurrentRow(effects_.empty() ? -1 : 0);
    resize(620, 360);
}

int TrackEffectsDialog::currentRow() const {
    return list_->currentRow();
}

void TrackEffectsDialog::rebuildList() {
    updating_ = true;
    const int row = list_->currentRow();
    list_->clear();
    for (size_t i = 0; i < effects_.size(); ++i) {
        auto* item = new QListWidgetItem(QString("%1. %2").arg(i + 1).arg(effectInfo(effects_[i].type).name));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(effects_[i].bypassed ? Qt::Unchecked : Qt::Checked);
        if (effects_[i].bypassed) {
            item->setForeground(Qt::gray);
        }
        list_->addItem(item);
    }
    list_->setCurrentRow(std::min(row, static_cast<int>(effects_.size()) - 1));
    updating_ = false;
    const bool has = !effects_.empty();
    removeButton_->setEnabled(has);
    upButton_->setEnabled(has && list_->currentRow() > 0);
    downButton_->setEnabled(has && list_->currentRow() >= 0 &&
                            list_->currentRow() < static_cast<int>(effects_.size()) - 1);
    bakeButton_->setEnabled(anyEffectActive(effects_));
}

void TrackEffectsDialog::rebuildParams() {
    while (QLayoutItem* item = paramsLayout_->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        if (QLayout* l = item->layout()) {
            while (QLayoutItem* inner = l->takeAt(0)) {
                if (QWidget* w = inner->widget()) w->deleteLater();
                delete inner;
            }
        }
        delete item;
    }
    spins_.clear();
    sliders_.clear();
    const int row = list_->currentRow();
    upButton_->setEnabled(row > 0);
    downButton_->setEnabled(row >= 0 && row < static_cast<int>(effects_.size()) - 1);
    if (row < 0 || row >= static_cast<int>(effects_.size())) {
        paramsBox_->setTitle(QString());
        auto* empty = new QLabel(effects_.empty() ? "No effects yet. Use Add to put one on this track."
                                                  : "Select an effect to edit it.");
        empty->setWordWrap(true);
        paramsLayout_->addWidget(empty);
        paramsLayout_->addStretch();
        return;
    }
    const Effect& effect = effects_[static_cast<size_t>(row)];
    const EffectInfo& info = effectInfo(effect.type);
    paramsBox_->setTitle(info.name);
    auto* grid = new QGridLayout();
    for (int p = 0; p < info.paramCount; ++p) {
        const EffectParamInfo& pi = info.params[p];
        auto* label = new QLabel(pi.label);
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, kSliderSteps);
        slider->setObjectName(QString("slider_%1").arg(pi.key));
        auto* spin = new QDoubleSpinBox();
        spin->setObjectName(QString("param_%1").arg(pi.key));
        spin->setRange(pi.minValue, pi.maxValue);
        spin->setDecimals(pi.decimals);
        spin->setSingleStep(pi.decimals == 0 ? 1.0 : std::pow(10.0, -pi.decimals) * 5.0);
        if (pi.unit[0] != '\0') spin->setSuffix(QString(" %1").arg(pi.unit));
        spin->setValue(effect.param(p));
        slider->setValue(toSlider(pi, effect.param(p)));
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, p](double v) {
            if (!updating_) setParameter(p, v);
        });
        connect(slider, &QSlider::valueChanged, this, [this, p, pi](int pos) {
            if (!updating_) setParameter(p, fromSlider(pi, pos));
        });
        grid->addWidget(label, p, 0);
        grid->addWidget(slider, p, 1);
        grid->addWidget(spin, p, 2);
        spins_.push_back(spin);
        sliders_.push_back(slider);
    }
    paramsLayout_->addLayout(grid);
    if (info.paramCount == 0) {
        paramsLayout_->addWidget(new QLabel("This effect has no settings."));
    }
    paramsLayout_->addStretch();
}

void TrackEffectsDialog::changed() {
    emit effectsChanged();
}

void TrackEffectsDialog::addEffect(EffectType type) {
    // After the current effect, or at the end.
    const int row = list_->currentRow();
    const size_t at = row >= 0 ? static_cast<size_t>(row) + 1 : effects_.size();
    effects_.insert(effects_.begin() + static_cast<long>(at), Effect::make(type));
    rebuildList();
    setCurrentRow(static_cast<int>(at));
    changed();
}

void TrackEffectsDialog::removeCurrent() {
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(effects_.size())) return;
    effects_.erase(effects_.begin() + row);
    rebuildList();
    setCurrentRow(std::min(row, static_cast<int>(effects_.size()) - 1));
    changed();
}

void TrackEffectsDialog::moveCurrent(int delta) {
    const int row = list_->currentRow();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= static_cast<int>(effects_.size())) return;
    std::swap(effects_[static_cast<size_t>(row)], effects_[static_cast<size_t>(to)]);
    rebuildList();
    setCurrentRow(to);
    changed();
}

void TrackEffectsDialog::setCurrentRow(int row) {
    updating_ = true;
    list_->setCurrentRow(row);
    updating_ = false;
    rebuildParams();
}

void TrackEffectsDialog::setBypassed(int row, bool bypassed) {
    if (row < 0 || row >= static_cast<int>(effects_.size())) return;
    if (effects_[static_cast<size_t>(row)].bypassed == bypassed) return;
    effects_[static_cast<size_t>(row)].bypassed = bypassed;
    // Update the row in place: this can run from the list's own itemChanged
    // signal, where rebuilding would delete the item being reported.
    if (QListWidgetItem* item = list_->item(row)) {
        updating_ = true;
        item->setCheckState(bypassed ? Qt::Unchecked : Qt::Checked);
        item->setForeground(bypassed ? QBrush(Qt::gray) : QBrush());
        updating_ = false;
    }
    bakeButton_->setEnabled(anyEffectActive(effects_));
    changed();
}

void TrackEffectsDialog::setParameter(int paramIndex, double value) {
    const int row = list_->currentRow();
    if (row < 0 || row >= static_cast<int>(effects_.size())) return;
    Effect& effect = effects_[static_cast<size_t>(row)];
    const EffectInfo& info = effectInfo(effect.type);
    if (paramIndex < 0 || paramIndex >= info.paramCount) return;
    effect.params[static_cast<size_t>(paramIndex)] = value;
    effect.clampParams();
    const double v = effect.param(paramIndex);
    // Keep the slider and the spin box in step without feeding back.
    updating_ = true;
    if (static_cast<size_t>(paramIndex) < spins_.size()) {
        spins_[static_cast<size_t>(paramIndex)]->setValue(v);
        sliders_[static_cast<size_t>(paramIndex)]->setValue(toSlider(info.params[paramIndex], v));
    }
    updating_ = false;
    changed();
}

} // namespace zrecord
