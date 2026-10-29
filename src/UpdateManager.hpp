#pragma once

#include "Update.hpp"
#include "UpdateChecker.hpp"
#include "UpdateHost.hpp"

#include <QObject>
#include <QPointer>
#include <QStringList>

#include <functional>
#include <memory>

class QProcess;
class QProgressDialog;
#include <QTemporaryDir>
class QWidget;

namespace zrecord {

class UpdateDialog;

// One per process: Help > Check for Updates, the automatic check shortly
// after startup (Help menu, at most once a day), the dialog and the install:
// download package + SHA256SUMS -> verify -> install (Linux: pkexec apt; macOS: open .dmg) -> restart.
// Every dialog is opened non-modally-blocking (open(), not exec()) so the flow
// is testable; objectNames: updateDialog, updateProgress, updateMessage,
// updateRestart.
class UpdateManager : public QObject
{
    Q_OBJECT
public:
    enum class Stage { Idle, Checking, Offering, Downloading, Installing, Done };
    Q_ENUM(Stage)

    static UpdateManager &instance();

    // Help > Check for Updates: always reports (latest / error / dialog).
    void checkNow(QWidget *parent);
    // The "check at startup" choice (Help menu), default on: QSettings updates/checkOnStartup.
    static bool checkAtStartupEnabled();
    static void setCheckAtStartupEnabled(bool on);
    // Called once from main(): after delayMs, checks quietly if enabled and due.
    void scheduleStartupCheck(QWidget *parent, int delayMs = 3000);
    // The startup decision, without the timer (tests).
    bool startupCheckIfDue(QWidget *parent);

    // The window that owns the update flow (set once by MainWindow).
    void setHost(UpdateHost *host) { m_host = host; }

    Stage stage() const { return m_stage; }
    UpdateChecker *checker() const { return m_checker; }
    const ReleaseInfo &lastRelease() const { return m_release; }
    QString lastMessage() const { return m_lastMessage; }

    // Tests: the version we pretend to run (default kVersionString).
    void setCurrentVersionForTests(const QString &v) { m_currentVersion = v; }
    // Tests: run this program instead of `pkexec apt install -y <deb>`; the
    // staged package's absolute path is appended to args. Also skips the
    // /usr/bin check.
    void setInstallerForTests(const QString &program, const QStringList &args);
    // Tests: called instead of relaunching + quitting.
    void setRestartHandlerForTests(std::function<void()> fn) { m_restartHandler = std::move(fn); }
    void resetForTests();

    // The install command for a downloaded package (program, args); exposed for tests.
    // Absolute paths only: {"/usr/bin/pkexec", "/usr/bin/apt", "install", "-y", <absolute .deb path>}.
    static QStringList installCommand(const QString &debPath);
    // Copies the verified package into a fresh private (0700) directory and
    // re-checks its SHA-256 against `sha256` right before it is handed to the
    // installer. Returns the copy's absolute path, or empty (with *error).
    static QString stageVerifiedPackage(const QString &path, const QString &sha256, QTemporaryDir &into,
                                        QString *error);

signals:
    void stageChanged(zrecord::UpdateManager::Stage stage);
    void checkFinished(bool offered);
    void installFinished(bool ok, const QString &message);

private:
    explicit UpdateManager(QObject *parent = nullptr);
    void startCheck(QWidget *parent, bool manual);
    void onCheckFinished(const UpdateChecker::Result &r);
    void offer();
    void startDownload();
    void onDownloadFinished(bool ok, const QString &debPath, const QString &error);
    void runInstaller();
    void onInstallerFinished(int exitCode, bool crashed);
    void offerRestart();
    void restart();
    void fallback(const QString &why);
    void message(const QString &title, const QString &text, bool warning);
    void setStage(Stage s);

    UpdateChecker *m_checker;
    UpdateDownloader *m_downloader;
    QPointer<QWidget> m_parent;
    QPointer<QProgressDialog> m_progress;
    QProcess *m_installer = nullptr;
    std::unique_ptr<QTemporaryDir> m_tmp;
    std::unique_ptr<QTemporaryDir> m_installDir; // private copy handed to the installer
    ReleaseInfo m_release;
    QString m_debPath;
    QString m_currentVersion;
    QString m_lastMessage;
    QString m_testProgram;
    QStringList m_testArgs;
    std::function<void()> m_restartHandler;
    UpdateHost *m_host = nullptr;
    Stage m_stage = Stage::Idle;
    bool m_manual = false;
    bool m_startupScheduled = false;
};

} // namespace zrecord
