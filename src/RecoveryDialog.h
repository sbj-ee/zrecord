#pragma once

#include <QDateTime>
#include <QDialog>
#include <QString>
#include <vector>

class QListWidget;
class QPushButton;

namespace zrecord {

// Offered at launch when an earlier zrecord didn't exit cleanly and left
// unsaved work behind: restore it, discard it, or decide later.
class RecoveryDialog : public QDialog {
    Q_OBJECT

public:
    // done() codes: Restore and Discard; Decide Later is Rejected.
    enum Result { Later = QDialog::Rejected, Restore = QDialog::Accepted, Discard = 2 };

    struct Entry {
        QString summary;     // "song, 2 tracks, 3 clips, and a partial take of 0:12"
        QDateTime lastSaved; // its newest autosave
    };

    explicit RecoveryDialog(const std::vector<Entry>& entries, QWidget* parent = nullptr);

    // Which session Restore/Discard applies to.
    int selectedIndex() const;
    void setSelectedIndex(int index);

private:
    QListWidget* list_ = nullptr;
};

} // namespace zrecord
