#include "RecoveryDialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace zrecord {

RecoveryDialog::RecoveryDialog(const std::vector<Entry>& entries, QWidget* parent) : QDialog(parent) {
    setWindowTitle("Recover Unsaved Work");
    setObjectName("recoveryDialog");

    auto* layout = new QVBoxLayout(this);
    auto* header = new QHBoxLayout;
    auto* icon = new QLabel;
    icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(48, 48));
    header->addWidget(icon, 0, Qt::AlignTop);
    auto* text = new QLabel(entries.size() == 1
                                ? "zrecord didn't shut down properly last time, and left unsaved work behind."
                                : "zrecord didn't shut down properly, and left unsaved work behind from more "
                                  "than one session.");
    text->setWordWrap(true);
    QFont bold = text->font();
    bold.setBold(true);
    text->setFont(bold);
    header->addWidget(text, 1);
    layout->addLayout(header);

    list_ = new QListWidget;
    list_->setObjectName("recoverySessions");
    for (const Entry& entry : entries) {
        const QString when = QLocale().toString(entry.lastSaved, QLocale::ShortFormat);
        auto* item = new QListWidgetItem(QString("%1\nLast autosaved %2").arg(entry.summary, when), list_);
        item->setToolTip(entry.summary);
    }
    list_->setCurrentRow(0);
    list_->setMinimumWidth(460);
    list_->setMaximumHeight(28 + 44 * std::min<int>(4, static_cast<int>(entries.size())));
    layout->addWidget(list_);

    auto* info = new QLabel("<b>Restore</b> opens it as an unsaved project: save it to keep it. (The undo history "
                            "isn't restored.)<br><b>Discard</b> deletes it for good.<br><b>Decide Later</b> leaves "
                            "it for the next time zrecord starts.");
    info->setWordWrap(true);
    layout->addWidget(info);

    auto* buttons = new QDialogButtonBox;
    QPushButton* restore = buttons->addButton("Restore", QDialogButtonBox::AcceptRole);
    restore->setObjectName("restoreButton");
    restore->setDefault(true);
    QPushButton* discard = buttons->addButton("Discard", QDialogButtonBox::DestructiveRole);
    discard->setObjectName("discardButton");
    QPushButton* later = buttons->addButton("Decide Later", QDialogButtonBox::RejectRole);
    later->setObjectName("laterButton");
    connect(restore, &QPushButton::clicked, this, [this] { done(Restore); });
    connect(discard, &QPushButton::clicked, this, [this] { done(Discard); });
    connect(later, &QPushButton::clicked, this, [this] { done(Later); });
    layout->addWidget(buttons);
}

int RecoveryDialog::selectedIndex() const {
    return list_->currentRow();
}

void RecoveryDialog::setSelectedIndex(int index) {
    list_->setCurrentRow(index);
}

} // namespace zrecord
