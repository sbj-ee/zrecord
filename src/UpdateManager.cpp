#include "UpdateManager.hpp"

#include <QtGlobal>

#include "UpdateDialog.hpp"
#include "zrecord_version.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QSettings>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

namespace zrecord {

namespace {
const QString kInstalledBinary = QStringLiteral("/usr/bin/zrecord");
// Absolute paths: never whatever "pkexec"/"apt" $PATH finds first.
const QString kPkexec = QStringLiteral("/usr/bin/pkexec");
const QString kApt = QStringLiteral("/usr/bin/apt");
} // namespace

bool UpdateManager::checkAtStartupEnabled()
{
    return QSettings().value(QStringLiteral("updates/checkOnStartup"), true).toBool();
}

void UpdateManager::setCheckAtStartupEnabled(bool on)
{
    QSettings().setValue(QStringLiteral("updates/checkOnStartup"), on);
}

UpdateManager &UpdateManager::instance()
{
    static UpdateManager *m = new UpdateManager(qApp);
    return *m;
}

UpdateManager::UpdateManager(QObject *parent)
    : QObject(parent)
    , m_checker(new UpdateChecker(this))
    , m_downloader(new UpdateDownloader(this))
    , m_currentVersion(QStringLiteral(ZRECORD_VERSION))
{
    connect(m_checker, &UpdateChecker::finished, this, &UpdateManager::onCheckFinished);
    connect(m_downloader, &UpdateDownloader::finished, this, &UpdateManager::onDownloadFinished);
    connect(m_downloader, &UpdateDownloader::progress, this, [this](const QString &name, qint64 got, qint64 total) {
        if (!m_progress) {
            return;
        }
        m_progress->setLabelText(QStringLiteral("Downloading %1\u2026").arg(name));
        if (total > 0) {
            m_progress->setMaximum(1000);
            m_progress->setValue(int(got * 1000 / total));
        } else {
            m_progress->setMaximum(0); // busy
        }
    });
}

void UpdateManager::setStage(Stage s)
{
    if (m_stage != s) {
        m_stage = s;
        emit stageChanged(s);
    }
}

void UpdateManager::setInstallerForTests(const QString &program, const QStringList &args)
{
    m_testProgram = program;
    m_testArgs = args;
}

void UpdateManager::resetForTests()
{
    m_checker->abort();
    m_downloader->cancel();
    m_tmp.reset();
    m_installDir.reset();
    m_release = {};
    m_debPath.clear();
    m_lastMessage.clear();
    m_startupScheduled = false;
    m_stage = Stage::Idle;
}

QStringList UpdateManager::installCommand(const QString &debPath)
{
    // An absolute path makes apt treat the argument as a file, not a package name.
    return {kPkexec, kApt, QStringLiteral("install"), QStringLiteral("-y"), QFileInfo(debPath).absoluteFilePath()};
}

QString UpdateManager::stageVerifiedPackage(const QString &path, const QString &sha256, QTemporaryDir &into,
                                            QString *error)
{
    auto fail = [error](const QString &why) {
        if (error) {
            *error = why;
        }
        return QString();
    };
    if (!into.isValid()) {
        return fail(QStringLiteral("Couldn't create a private folder for the package."));
    }
    // QTemporaryDir creates it 0700; make sure.
    QFile::setPermissions(into.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString dest = QDir(into.path()).filePath(QFileInfo(path).fileName());
    if (!QFile::copy(path, dest)) {
        return fail(QStringLiteral("Couldn't copy the package to %1.").arg(into.path()));
    }
    QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup
                                    | QFileDevice::ReadOther);
    if (sha256.isEmpty() || sha256OfFile(dest) != sha256.toLower()) {
        QFile::remove(dest);
        return fail(QStringLiteral("The package changed after it was verified, so it was not installed."));
    }
    return QFileInfo(dest).absoluteFilePath();
}

void UpdateManager::checkNow(QWidget *parent)
{
    startCheck(parent, true);
}

void UpdateManager::scheduleStartupCheck(QWidget *parent, int delayMs)
{
    if (m_startupScheduled) {
        return; // once per process, however many windows open
    }
    m_startupScheduled = true;
    QPointer<QWidget> p(parent);
    QTimer::singleShot(delayMs, this, [this, p]() {
        if (p) {
            startupCheckIfDue(p);
        }
    });
}

bool UpdateManager::startupCheckIfDue(QWidget *parent)
{
    const UpdateState state = UpdateState::load();
    if (!UpdatePolicy::shouldCheckAtStartup(checkAtStartupEnabled(), state, QDateTime::currentDateTimeUtc())) {
        return false;
    }
    startCheck(parent, false);
    return true;
}

void UpdateManager::startCheck(QWidget *parent, bool manual)
{
    if (m_stage != Stage::Idle && m_stage != Stage::Done) {
        if (manual && m_stage == Stage::Checking) {
            m_manual = true; // a quiet startup check becomes a reported one
            m_parent = parent;
        }
        return;
    }
    m_parent = parent;
    m_manual = manual;
    setStage(Stage::Checking);
    m_checker->check();
}

void UpdateManager::onCheckFinished(const UpdateChecker::Result &r)
{
    if (!m_manual) {
        // Count the attempt even if it failed, so a flaky network or a rate
        // limit doesn't turn into a request at every startup.
        UpdateState st = UpdateState::load();
        st.lastCheck = QDateTime::currentDateTimeUtc();
        st.save();
    }
    setStage(Stage::Idle);
    if (r.outcome != UpdateChecker::Outcome::Release) {
        m_lastMessage = r.error;
        if (m_manual) {
            message(QStringLiteral("Check for Updates"), r.error, r.outcome == UpdateChecker::Outcome::Error);
        }
        emit checkFinished(false);
        return;
    }
    m_release = r.release;
    if (!UpdatePolicy::shouldOffer(m_release, m_currentVersion, UpdateState::load(), m_manual)) {
        m_lastMessage = QStringLiteral("You're running the latest version of zrecord (%1).").arg(m_currentVersion);
        if (m_manual) {
            if (isNewerVersion(m_currentVersion, m_release.tag)) {
                m_lastMessage = QStringLiteral("You're running zrecord %1, newer than the latest release (%2).")
                                    .arg(m_currentVersion, m_release.tag);
            }
            message(QStringLiteral("Check for Updates"), m_lastMessage, false);
        }
        emit checkFinished(false);
        return;
    }
    offer();
    emit checkFinished(true);
}

void UpdateManager::offer()
{
    setStage(Stage::Offering);
    auto *dlg = new UpdateDialog(m_release, m_currentVersion, m_parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::finished, this, [this](int result) {
        if (result == UpdateDialog::Skip) {
            UpdateState st = UpdateState::load();
            st.skippedVersion = m_release.tag;
            st.save();
            setStage(Stage::Idle);
        } else if (result == UpdateDialog::Install) {
            startDownload();
        } else {
            setStage(Stage::Idle);
        }
    });
    dlg->open();
}

void UpdateManager::startDownload()
{
    m_tmp = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/zrecord-update-XXXXXX"));
    if (!m_tmp->isValid()) {
        message(QStringLiteral("Update"), QStringLiteral("Couldn't create a temporary folder for the download."), true);
        setStage(Stage::Idle);
        return;
    }
    setStage(Stage::Downloading);
    m_progress = new QProgressDialog(QStringLiteral("Downloading\u2026"), QStringLiteral("Cancel"), 0, 0, m_parent);
    m_progress->setObjectName(QStringLiteral("updateProgress"));
    m_progress->setWindowTitle(QStringLiteral("Updating zrecord"));
    m_progress->setAttribute(Qt::WA_DeleteOnClose);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    m_progress->setMinimumDuration(0);
    connect(m_progress, &QProgressDialog::canceled, m_downloader, &UpdateDownloader::cancel);
    m_progress->open();
    m_downloader->start(m_release, m_tmp->path());
}

void UpdateManager::onDownloadFinished(bool ok, const QString &debPath, const QString &error)
{
    if (m_progress) {
        m_progress->disconnect(m_downloader);
        m_progress->close();
    }
    if (!ok) {
        m_tmp.reset();
        setStage(Stage::Idle);
        m_lastMessage = error;
        if (error != QLatin1String("Download cancelled.")) {
            message(QStringLiteral("Update Not Installed"), error, true);
        }
        emit installFinished(false, error);
        return;
    }
    m_debPath = debPath;
    runInstaller();
}

void UpdateManager::runInstaller()
{
    const QString pkgName = QFileInfo(m_debPath).fileName();
    // Hand the installer a private copy, re-verified against the signed hash
    // at the last moment.
    m_installDir = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/zrecord-install-XXXXXX"));
    QString stageError;
    const QString staged = stageVerifiedPackage(m_debPath, m_downloader->verifiedSha256(), *m_installDir, &stageError);
    if (staged.isEmpty()) {
        m_installDir.reset();
        m_tmp.reset();
        setStage(Stage::Idle);
        m_lastMessage = stageError;
        message(QStringLiteral("Update Not Installed"), stageError, true);
        emit installFinished(false, stageError);
        return;
    }
    QString program;
    QStringList args;
    if (!m_testProgram.isEmpty()) {
        program = m_testProgram;
        args = m_testArgs;
        args << staged;
    } else {
#if defined(Q_OS_MACOS)
        // macOS: open the verified .dmg so the user can drag zrecord.app to
        // Applications. Automated replacement of a signed app bundle is out of
        // scope (no pkexec/apt equivalent here).
        setStage(Stage::Installing);
        const QUrl dmgUrl = QUrl::fromLocalFile(staged);
        if (!QDesktopServices::openUrl(dmgUrl)) {
            fallback(QStringLiteral("Couldn't open %1. Open it from Finder and drag zrecord.app to Applications.")
                         .arg(m_debPath));
            return;
        }
        setStage(Stage::Done);
        m_lastMessage = QStringLiteral(
            "zrecord %1 was downloaded and verified. The disk image is open — drag "
            "zrecord.app to Applications, then quit and reopen zrecord.")
                            .arg(m_release.tag);
        message(QStringLiteral("Update Ready"), m_lastMessage, false);
        emit installFinished(true, m_lastMessage);
        return;
#else
        // Only replace a packaged install; a build-tree or /usr/local binary
        // wouldn't be the one apt updates.
        if (QCoreApplication::applicationFilePath() != kInstalledBinary) {
            fallback(QStringLiteral("This zrecord (%1) wasn't installed from the .deb, so the package can't be "
                                    "installed in its place automatically.")
                         .arg(QCoreApplication::applicationFilePath()));
            return;
        }
        const QStringList cmd = installCommand(staged);
        if (!QFileInfo(cmd.at(0)).isExecutable() || !QFileInfo(cmd.at(1)).isExecutable()) {
            fallback(QStringLiteral("pkexec or apt isn't available, so the package can't be installed from here."));
            return;
        }
        program = cmd.at(0);
        args = cmd.mid(1);
#endif
    }
    setStage(Stage::Installing);
    m_progress = new QProgressDialog(
        QStringLiteral("Installing %1\u2026\nEnter your password in the system prompt.").arg(pkgName), QString(), 0, 0,
        m_parent);
    m_progress->setObjectName(QStringLiteral("updateProgress"));
    m_progress->setWindowTitle(QStringLiteral("Updating zrecord"));
    m_progress->setAttribute(Qt::WA_DeleteOnClose);
    m_progress->setMinimumDuration(0);
    m_progress->setCancelButton(nullptr);
    m_progress->open();

    m_installer = new QProcess(this);
    m_installer->setWorkingDirectory(m_installDir->path());
    m_installer->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_installer, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        onInstallerFinished(code, st == QProcess::CrashExit);
    });
    connect(m_installer, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            onInstallerFinished(127, false);
        }
    });
    m_installer->start(program, args); // argv list, no shell
}

void UpdateManager::onInstallerFinished(int exitCode, bool crashed)
{
    if (!m_installer) {
        return;
    }
    const QString output = QString::fromLocal8Bit(m_installer->readAll()).trimmed();
    m_installer->deleteLater();
    m_installer = nullptr;
    if (m_progress) {
        m_progress->close();
    }
    if (crashed || exitCode != 0) {
        QString why;
        if (exitCode == 126) {
            why = QStringLiteral("The password prompt was dismissed, so nothing was installed.");
        } else if (exitCode == 127) {
            why = QStringLiteral("Not authorised to install packages (pkexec exit 127).");
        } else {
            why = QStringLiteral("apt couldn't install the package (exit %1).").arg(exitCode);
            if (!output.isEmpty()) {
                why += QLatin1Char('\n') + output.right(600);
            }
        }
        fallback(why);
        return;
    }
    setStage(Stage::Done);
    m_lastMessage = QStringLiteral("zrecord %1 was installed.").arg(m_release.tag);
    m_tmp.reset(); // remove the downloaded package
    m_installDir.reset();
    emit installFinished(true, m_lastMessage);
    offerRestart();
}

void UpdateManager::offerRestart()
{
    const bool unsaved = m_host && m_host->hasUnsavedChanges();
    QString text = QStringLiteral("zrecord %1 was installed. Restart zrecord now to use it?").arg(m_release.tag);
    if (unsaved) {
        text += QStringLiteral("\n\nThe open document has unsaved changes. You will be asked whether to save "
                               "them before zrecord closes.");
    }
    auto *box = new QMessageBox(unsaved ? QMessageBox::Warning : QMessageBox::Question,
                                QStringLiteral("Restart zrecord"), text, QMessageBox::NoButton, m_parent);
    box->setObjectName(QStringLiteral("updateRestart"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton *restartBtn = box->addButton(QStringLiteral("&Restart Now"), QMessageBox::AcceptRole);
    restartBtn->setObjectName(QStringLiteral("restartNow"));
    QPushButton *laterBtn = box->addButton(QStringLiteral("&Later"), QMessageBox::RejectRole);
    laterBtn->setObjectName(QStringLiteral("restartLater"));
    box->setDefaultButton(unsaved ? laterBtn : restartBtn);
    connect(box, &QMessageBox::buttonClicked, this, [this, restartBtn](QAbstractButton *b) {
        if (b == restartBtn) {
            restart();
        }
    });
    box->open();
}

void UpdateManager::restart()
{
    // Close first, the way the user would (the window asks to save unsaved
    // work, and "Cancel" there keeps zrecord running); only then start the new
    // copy with the same document.
    if (m_host && !m_host->closeForRestart()) {
        return;
    }
    if (m_restartHandler) {
        m_restartHandler();
        return;
    }
    QProcess::startDetached(kInstalledBinary, m_host ? m_host->restartArguments() : QStringList());
    QCoreApplication::exit(0);
}

void UpdateManager::fallback(const QString &why)
{
    setStage(Stage::Idle);
    if (m_tmp) {
        m_tmp->setAutoRemove(false); // keep the verified package for a manual install
    }
#if defined(Q_OS_MACOS)
    const QString text =
        QStringLiteral("%1\n\nThe verified package is at:\n%2\n\nOpen the .dmg and drag zrecord.app to "
                       "Applications.\n\nOpening the release page.")
            .arg(why, m_debPath);
#else
    const QString text =
        QStringLiteral("%1\n\nThe verified package is at:\n%2\n\nInstall it with:\nsudo apt install %2\n\n"
                       "Opening the release page.")
            .arg(why, m_debPath);
#endif
    m_lastMessage = text;
    if (isTrustedReleasePageUrl(m_release.htmlUrl) && m_testProgram.isEmpty()) {
        QDesktopServices::openUrl(QUrl(m_release.htmlUrl));
    }
    message(QStringLiteral("Update Not Installed"), text, true);
    emit installFinished(false, text);
}

void UpdateManager::message(const QString &title, const QString &text, bool warning)
{
    m_lastMessage = text;
    auto *box = new QMessageBox(warning ? QMessageBox::Warning : QMessageBox::Information, title, text,
                                QMessageBox::Ok, m_parent);
    box->setObjectName(QStringLiteral("updateMessage"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setTextInteractionFlags(Qt::TextSelectableByMouse);
    box->open();
}

} // namespace zrecord
