#pragma once

#include "Update.hpp"

#include <QObject>
#include <QPointer>
#include <QUrl>

class QNetworkAccessManager;
#include <QNetworkReply>
class QFile;

namespace zrecord {

// Asks GitHub for the latest release (public API, no token):
// GET https://api.github.com/repos/sbj-ee/zrecord/releases/latest
// One request per check(); finished() always fires exactly once per check.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    enum class Outcome { Release, NoRelease, Error };
    struct Result {
        Outcome outcome = Outcome::Error;
        ReleaseInfo release; // when outcome == Release
        QString error;       // human-readable, for NoRelease / Error
        bool rateLimited = false;
    };

    static QString defaultApiUrl();
    explicit UpdateChecker(QObject *parent = nullptr);
    ~UpdateChecker() override;

    // Default: defaultApiUrl(). Builds with ZRECORD_DEV_OVERRIDES (debug
    // only) also honour $ZRECORD_UPDATE_URL; release builds never read it.
    // setApiUrl is for tests.
    void setApiUrl(const QUrl &url) { m_url = url; }
    QUrl apiUrl() const { return m_url; }
    void setTimeoutMs(int ms) { m_timeoutMs = ms; }
    bool isRunning() const { return m_reply != nullptr; }

    void check();
    void abort();

    // Message for a failed reply: offline, timeout, rate limit (with the reset
    // time), 404, other HTTP errors. Public for the tests.
    static QString failureReason(QNetworkReply *reply, bool *rateLimited = nullptr);

signals:
    void finished(const zrecord::UpdateChecker::Result &result);

private:
    void onFinished();

    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
    QUrl m_url;
    int m_timeoutMs = 5000;
};

// Downloads a release's SHA256SUMS, SHA256SUMS.minisig and package into
// `dir`. SHA256SUMS must carry a valid minisign signature by the release key
// (updateSigningPublicKey()) before the package is even fetched, and the
// package must match it. Refuses (finished(false, ...)) when the build has no
// key, an asset is missing, a download fails, the signature is invalid or
// the checksum doesn't match.
class UpdateDownloader : public QObject {
    Q_OBJECT
public:
    explicit UpdateDownloader(QObject *parent = nullptr);
    ~UpdateDownloader() override;

    void start(const ReleaseInfo &release, const QString &dir);
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }
    void setStallTimeoutMs(int ms) { m_stallMs = ms; }
    // After a successful finished(): the package's SHA-256 from the signed
    // SHA256SUMS (held in memory, for re-checking right before install).
    QString verifiedSha256() const { return m_verifiedSha; }

signals:
    // bytesTotal is -1 when unknown.
    void progress(const QString &assetName, qint64 bytesReceived, qint64 bytesTotal);
    // ok: debPath is the verified package. !ok: error says why (cancelled too).
    void finished(bool ok, const QString &debPath, const QString &error);

private:
    void fetch(const ReleaseAsset &asset, const QString &path);
    void onReadyRead();
    void onFinished();
    void fail(const QString &error);

    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
    QFile *m_file = nullptr;
    ReleaseAsset m_deb;
    ReleaseAsset m_sums;
    ReleaseAsset m_sig;
    QByteArray m_sumsBytes; // verified SHA256SUMS, in memory
    QString m_verifiedSha;
    QString m_dir;
    QString m_current; // asset being downloaded
    int m_stage = 0;   // 0 idle, 1 SHA256SUMS, 2 SHA256SUMS.minisig, 3 package
    int m_stallMs = 30000;
    bool m_cancelled = false;
};

} // namespace zrecord
