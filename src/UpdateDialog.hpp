#pragma once

#include "Update.hpp"

#include <QDialog>

class QPushButton;
class QTextBrowser;

namespace zrecord {

// "zrecord vX is available": version, release notes, Install / Later /
// Skip this version. Install is disabled (with the reason) when the release
// lacks the .deb or SHA256SUMS; the release page link is always there.
class UpdateDialog : public QDialog
{
    Q_OBJECT
public:
    enum Choice { Later = 0, Install = 1, Skip = 2 };

    UpdateDialog(const ReleaseInfo &release, const QString &currentVersion, QWidget *parent = nullptr);
    Choice choice() const { return m_choice; }
    QPushButton *installButton() const { return m_install; }
    QPushButton *laterButton() const { return m_later; }
    QPushButton *skipButton() const { return m_skip; }
    QTextBrowser *notes() const { return m_notes; }

private:
    Choice m_choice = Later;
    QPushButton *m_install;
    QPushButton *m_later;
    QPushButton *m_skip;
    QTextBrowser *m_notes;
};

} // namespace zrecord
