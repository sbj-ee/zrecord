// Auto-update (docs/PLAN.md §4.10): semver comparison, release JSON fixtures,
// SHA256SUMS verification, the once-a-day / skip rules, and the whole flow
// (check -> dialog -> download -> verify -> install -> restart) against a mock
// GitHub served from a local QTcpServer. Nothing here touches the network or apt.
#include "MinisignTestSigner.hpp"
#include "Update.hpp"
#include "UpdateChecker.hpp"
#include "UpdateDialog.hpp"
#include "UpdateManager.hpp"
#include "zrecord_version.h"

#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QtGlobal>
#include <QTextBrowser>

#include <map>

using namespace zrecord;

namespace {

// A tiny HTTP/1.1 server: one response per connection, then close.
class MockGitHub : public QObject
{
public:
    struct Route {
        int status = 200;
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers;
        bool hang = false; // accept and never answer (timeouts)
    };
    std::map<QString, Route> routes;
    QStringList requests;
    QByteArray lastUserAgent;
    QByteArray lastAccept;
    QTcpServer server;

    MockGitHub()
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                auto *buf = new QByteArray;
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                connect(s, &QObject::destroyed, [buf]() { delete buf; });
                connect(s, &QTcpSocket::readyRead, this, [this, s, buf]() {
                    buf->append(s->readAll());
                    if (!buf->contains("\r\n\r\n")) {
                        return;
                    }
                    handle(s, *buf);
                });
            }
        });
    }
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); }
    QString url(const QString &path) const { return base() + path; }

    void handle(QTcpSocket *s, const QByteArray &req)
    {
        const QList<QByteArray> lines = req.split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        const QString path = QString::fromLatin1(first.value(1));
        requests << path;
        for (const QByteArray &l : lines) {
            const QByteArray low = l.toLower();
            if (low.startsWith("user-agent:")) {
                lastUserAgent = l.mid(11).trimmed();
            } else if (low.startsWith("accept:")) {
                lastAccept = l.mid(7).trimmed();
            }
        }
        Route r;
        if (auto it = routes.find(path); it != routes.end()) {
            r = it->second;
        } else {
            r.status = 404;
            r.body = R"({"message":"Not Found"})";
        }
        if (r.hang) {
            return;
        }
        QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + (r.status == 200 ? " OK" : " Error") + "\r\n";
        out += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
        out += "Connection: close\r\n";
        for (const auto &h : r.headers) {
            out += h.first + ": " + h.second + "\r\n";
        }
        out += "\r\n" + r.body;
        s->write(out);
        s->disconnectFromHost();
    }
};

QByteArray fixture(const QString &name, const QString &base = QStringLiteral("http://127.0.0.1:1"))
{
    QFile f(QStringLiteral(ZRECORD_TEST_DATA "/update/") + name);
    if (!f.open(QIODevice::ReadOnly)) {
        qFatal("missing fixture %s", qPrintable(name));
    }
    return f.readAll().replace("@BASE@", base.toUtf8());
}

QByteArray sha256Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

template <typename T>
T *findTop(const char *name)
{
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (w->isVisible() && w->objectName() == QLatin1String(name)) {
            if (auto *t = qobject_cast<T *>(w)) {
                return t;
            }
        }
    }
    return nullptr;
}

void closeAllTop()
{
    for (const char *n : {"updateDialog", "updateMessage", "updateRestart", "updateProgress"}) {
        while (QWidget *w = findTop<QWidget>(n)) {
            w->close();
            QCoreApplication::processEvents();
        }
    }
}

// Stands in for MainWindow: an open document, maybe with unsaved changes.
class FakeHost : public QWidget, public UpdateHost
{
public:
    bool unsaved = false;
    bool allowClose = true; // false: the user pressed Cancel in the save prompt
    int closeCalls = 0;
    bool hasUnsavedChanges() const override { return unsaved; }
    QStringList restartArguments() const override { return {QStringLiteral("/home/u/draft.odt")}; }
    bool closeForRestart() override
    {
        ++closeCalls;
        return allowClose;
    }
};

} // namespace

class TstUpdate : public QObject
{
    Q_OBJECT
    QTemporaryDir settingsDir;
    const QByteArray debBytes = QByteArray("!<arch>\nfake zrecord package\n").repeated(128);
    MinisignTestKey signKey;  // the "release key" for these tests
    MinisignTestKey otherKey; // someone else's

    // Serve the 0.10.0 release with a correct (or wrong) SHA256SUMS, signed
    // by the release key (or by another key).
    void serveRelease(MockGitHub &gh, bool goodSum = true, bool goodSignature = true)
    {
        gh.routes[QStringLiteral("/latest")].body = fixture(QStringLiteral("release-0.10.0.json"), gh.base());
        gh.routes[QStringLiteral("/download/v0.10.0/zrecord_0.10.0_amd64.deb")].body = debBytes;
        gh.routes[QStringLiteral("/download/v0.10.0/zrecord-0.10.0-Darwin.dmg")].body = debBytes;
        const QByteArray sum = goodSum ? sha256Hex(debBytes) : sha256Hex("something else");
        gh.routes[QStringLiteral("/download/v0.10.0/SHA256SUMS")].body =
            sum + "  zrecord_0.10.0_amd64.deb\n" + sum + "  zrecord-0.10.0-Darwin.dmg\n"
            + QByteArray(64, 'a') + "  other.tar.gz\n";
        const QByteArray &sums = gh.routes[QStringLiteral("/download/v0.10.0/SHA256SUMS")].body;
        gh.routes[QStringLiteral("/download/v0.10.0/SHA256SUMS.minisig")].body =
            goodSignature ? signKey.sign(sums) : otherKey.sign(sums);
    }

    static QString platformPackageName()
    {
#if defined(Q_OS_MACOS)
        return QStringLiteral("zrecord-0.10.0-Darwin.dmg");
#else
        return QStringLiteral("zrecord_0.10.0_amd64.deb");
#endif
    }

    UpdateManager &freshManager(MockGitHub &gh)
    {
        closeAllTop();
        UpdateManager &m = UpdateManager::instance();
        m.resetForTests();
        m.setCurrentVersionForTests(QStringLiteral("0.9.0"));
        m.checker()->setApiUrl(QUrl(gh.url(QStringLiteral("/latest"))));
        m.setInstallerForTests({}, {});
        m.setRestartHandlerForTests({});
        QFile::remove(UpdateState::filePath());
        return m;
    }

private slots:
    void initTestCase()
    {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
        // QSettings (the startup-check switch) in a private folder, never the user's own.
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName(QStringLiteral("sbj-ee-test"));
        QCoreApplication::setApplicationName(QStringLiteral("zrecord-update-test"));
        UpdateManager::setCheckAtStartupEnabled(true);
        setUpdateSigningPublicKeyForTests(signKey.publicKeyBase64());
    }

    void semverCompare_data()
    {
        QTest::addColumn<QString>("a");
        QTest::addColumn<QString>("b");
        QTest::addColumn<int>("cmp");
        QTest::newRow("equal") << "1.2.3" << "1.2.3" << 0;
        QTest::newRow("leading v") << "v0.10.0" << "0.10.0" << 0;
        QTest::newRow("numeric minor, not string") << "0.10.0" << "0.9.0" << 1;
        QTest::newRow("patch") << "0.9.1" << "0.9.0" << 1;
        QTest::newRow("major") << "1.0.0" << "0.99.99" << 1;
        QTest::newRow("short form") << "1.2" << "1.2.0" << 0;
        QTest::newRow("prerelease below release") << "1.0.0-rc1" << "1.0.0" << -1;
        QTest::newRow("prerelease above previous") << "1.0.0-rc.1" << "0.10.0" << 1;
        QTest::newRow("rc.2 > rc.1") << "1.0.0-rc.2" << "1.0.0-rc.1" << 1;
        QTest::newRow("rc.10 > rc.9 numerically") << "1.0.0-rc.10" << "1.0.0-rc.9" << 1;
        QTest::newRow("alpha < beta") << "1.0.0-alpha" << "1.0.0-beta" << -1;
        QTest::newRow("numeric before alnum") << "1.0.0-1" << "1.0.0-alpha" << -1;
        QTest::newRow("longer prerelease wins") << "1.0.0-alpha.1" << "1.0.0-alpha" << 1;
        QTest::newRow("build metadata ignored") << "1.0.0+build.5" << "1.0.0" << 0;
        QTest::newRow("debian tilde") << "1.0.0~rc1" << "1.0.0-rc1" << 0;
    }
    void semverCompare()
    {
        QFETCH(QString, a);
        QFETCH(QString, b);
        QFETCH(int, cmp);
        const SemVer va = SemVer::parse(a);
        const SemVer vb = SemVer::parse(b);
        QVERIFY(va.valid);
        QVERIFY(vb.valid);
        QCOMPARE(SemVer::compare(va, vb), cmp);
        QCOMPARE(SemVer::compare(vb, va), -cmp);
        QCOMPARE(isNewerVersion(a, b), cmp > 0);
    }

    void semverInvalid()
    {
        for (const char *bad : {"", "v", "latest", "1.2.3.4", "1.x.0", "1.0.0-", "1..0", "-1.0.0", "1.0.0-rc..1"}) {
            QVERIFY2(!SemVer::parse(QString::fromLatin1(bad)).valid, bad);
        }
        QVERIFY(!isNewerVersion(QStringLiteral("nightly"), QStringLiteral("0.9.0")));
        QCOMPARE(SemVer::parse(QStringLiteral("v1.0.0-rc.1+abc")).toString(), QStringLiteral("1.0.0-rc.1"));
        // The running build parses (the update check compares against it).
        QVERIFY(SemVer::parse(QStringLiteral(ZRECORD_VERSION)).valid);
        QCOMPARE(SemVer::parse(QStringLiteral(ZRECORD_VERSION)).toString(), QStringLiteral(ZRECORD_EXPECTED_VERSION));
    }

    void releaseJson()
    {
        const auto r = ReleaseInfo::fromJson(fixture(QStringLiteral("release-0.10.0.json")));
        QVERIFY(r);
        QCOMPARE(r->tag, QStringLiteral("v0.10.0"));
        QCOMPARE(r->versionString(), QStringLiteral("0.10.0"));
        QCOMPARE(r->name, QStringLiteral("zrecord 0.10.0"));
        QVERIFY(r->notes.contains(QStringLiteral("Auto-update")));
        QCOMPARE(r->htmlUrl, QStringLiteral("https://github.com/sbj-ee/zrecord/releases/tag/v0.10.0"));
        QVERIFY(r->publishedAt.isValid());
        QCOMPARE(r->assets.size(), 4);
        QVERIFY(r->signatureAsset());
        QCOMPARE(r->signatureAsset()->name, QStringLiteral("SHA256SUMS.minisig"));
        QVERIFY(r->debAsset());
        QCOMPARE(r->debAsset()->name, QStringLiteral("zrecord_0.10.0_amd64.deb"));
        QCOMPARE(r->debAsset()->size, qint64(4096));
        QVERIFY(r->checksumAsset());
        QVERIFY(r->dmgAsset());
        QCOMPARE(r->dmgAsset()->name, QStringLiteral("zrecord-0.10.0-Darwin.dmg"));
        QVERIFY(r->packageAsset());
        QCOMPARE(r->packageAsset()->name, platformPackageName());

        // Darwin .dmg naming used by CPack DragNDrop / macOS updater.
        {
            const auto withDmg = ReleaseInfo::fromJson(fixture(QStringLiteral("release-0.10.0-darwin.json")));
            QVERIFY(withDmg);
            QVERIFY(withDmg->dmgAsset());
            QCOMPARE(withDmg->dmgAsset()->name, QStringLiteral("zrecord-0.10.0-Darwin.dmg"));
            QCOMPARE(withDmg->dmgAsset()->size, qint64(99));
            QVERIFY(withDmg->checksumAsset());
        }

        // Missing .deb for amd64 (an arm64 one doesn't count).
        const auto noDeb = ReleaseInfo::fromJson(fixture(QStringLiteral("release-no-deb.json")));
        QVERIFY(noDeb);
        QVERIFY(!noDeb->debAsset());
        QVERIFY(noDeb->checksumAsset());

        // Prerelease: the package carries the full tag version.
        const auto rc = ReleaseInfo::fromJson(fixture(QStringLiteral("release-rc.json")));
        QVERIFY(rc);
        QVERIFY(rc->prerelease);
        QVERIFY(rc->debAsset());
        QCOMPARE(rc->debAsset()->name, QStringLiteral("zrecord_1.0.0-rc.1_amd64.deb"));

        QString err;
        QVERIFY(!ReleaseInfo::fromJson("<html>rate limited</html>", &err));
        QVERIFY(err.contains(QStringLiteral("could not read")));
        QVERIFY(!ReleaseInfo::fromJson(R"({"message":"Not Found"})", &err));
        QVERIFY(err.contains(QStringLiteral("No published")));
        QVERIFY(!ReleaseInfo::fromJson(R"({"tag_name":"nightly"})", &err));
        QVERIFY(err.contains(QStringLiteral("nightly")));
    }

    // Found by fuzzing: an out-of-range asset size was cast from double (UB).
    void hostileAssetSizes()
    {
        const QByteArray json =
            "{\"tag_name\":\"v9.9.9\",\"assets\":["
            "{\"name\":\"a\",\"browser_download_url\":\"https://x/a\",\"size\":1e300},"
            "{\"name\":\"b\",\"browser_download_url\":\"https://x/b\",\"size\":-5},"
            "{\"name\":\"c\",\"browser_download_url\":\"https://x/c\",\"size\":4096}]}";
        const auto r = ReleaseInfo::fromJson(json);
        QVERIFY(r);
        QCOMPARE(r->assets.size(), 3);
        QCOMPARE(r->assets.at(0).size, qint64(0));
        QCOMPARE(r->assets.at(1).size, qint64(0));
        QCOMPARE(r->assets.at(2).size, qint64(4096));
    }

    void checksum()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("zrecord_0.10.0_amd64.deb"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(debBytes);
        f.close();
        const QByteArray good = sha256Hex(debBytes);
        QCOMPARE(sha256OfFile(path).toLatin1(), good);

        // sha256sum's text and binary ("*") forms, upper-case hex, "./" prefix.
        for (const QByteArray &sums : {good + "  zrecord_0.10.0_amd64.deb\n", good + " *zrecord_0.10.0_amd64.deb\n",
                                        good.toUpper() + "  ./zrecord_0.10.0_amd64.deb\r\n",
                                        QByteArray(64, 'b') + "  other\n" + good + "  zrecord_0.10.0_amd64.deb"}) {
            const ChecksumResult r = verifyChecksum(path, sums, QStringLiteral("zrecord_0.10.0_amd64.deb"));
            QVERIFY2(r.ok(), sums.constData());
        }
        const ChecksumResult bad = verifyChecksum(path, sha256Hex("tampered") + "  zrecord_0.10.0_amd64.deb\n",
                                                  QStringLiteral("zrecord_0.10.0_amd64.deb"));
        QCOMPARE(bad.status, ChecksumResult::Status::Mismatch);
        QVERIFY(bad.message().contains(QStringLiteral("does not match")));
        QVERIFY(bad.message().contains(QString::fromLatin1(good)));

        const ChecksumResult notListed =
            verifyChecksum(path, good + "  zrecord_0.9.0_amd64.deb\n", QStringLiteral("zrecord_0.10.0_amd64.deb"));
        QCOMPARE(notListed.status, ChecksumResult::Status::NotListed);
        QCOMPARE(verifyChecksum(path, "", QStringLiteral("zrecord_0.10.0_amd64.deb")).status,
                 ChecksumResult::Status::BadFile);
        // A short/garbled hash isn't a match for anything.
        QCOMPARE(verifyChecksum(path, good.left(63) + "  zrecord_0.10.0_amd64.deb\n",
                                QStringLiteral("zrecord_0.10.0_amd64.deb"))
                     .status,
                 ChecksumResult::Status::NotListed);
        QCOMPARE(verifyChecksum(dir.filePath(QStringLiteral("gone.deb")), good + "  gone.deb\n", QStringLiteral("gone.deb"))
                     .status,
                 ChecksumResult::Status::Unreadable);
    }

    void dailyLimit()
    {
        const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-04T12:00:00Z"), Qt::ISODate);
        UpdateState st;
        QVERIFY(UpdatePolicy::shouldCheckAtStartup(true, st, now)); // never checked
        QVERIFY(!UpdatePolicy::shouldCheckAtStartup(false, st, now)); // turned off in the Help menu
        st.lastCheck = now.addSecs(-3600);
        QVERIFY(!UpdatePolicy::shouldCheckAtStartup(true, st, now)); // an hour ago
        st.lastCheck = now.addSecs(-(24 * 3600 - 1));
        QVERIFY(!UpdatePolicy::shouldCheckAtStartup(true, st, now));
        st.lastCheck = now.addSecs(-24 * 3600);
        QVERIFY(UpdatePolicy::shouldCheckAtStartup(true, st, now)); // a day
        st.lastCheck = now.addDays(3);
        QVERIFY(UpdatePolicy::shouldCheckAtStartup(true, st, now)); // clock went backwards
    }

    void skipLogic()
    {
        auto rel = [](const char *tag) {
            ReleaseInfo r;
            r.tag = QString::fromLatin1(tag);
            r.version = SemVer::parse(r.tag);
            return r;
        };
        UpdateState none;
        const QString cur = QStringLiteral("0.9.0");
        QVERIFY(UpdatePolicy::shouldOffer(rel("v0.10.0"), cur, none, false));
        QVERIFY(!UpdatePolicy::shouldOffer(rel("v0.9.0"), cur, none, true));  // same
        QVERIFY(!UpdatePolicy::shouldOffer(rel("v0.8.0"), cur, none, true));  // older
        QVERIFY(!UpdatePolicy::shouldOffer(rel("v1.0.0-rc1"), QStringLiteral("1.0.0"), none, true));

        UpdateState skipped;
        skipped.skippedVersion = QStringLiteral("v0.10.0");
        QVERIFY(!UpdatePolicy::shouldOffer(rel("v0.10.0"), cur, skipped, false)); // quiet at startup
        QVERIFY(UpdatePolicy::shouldOffer(rel("v0.10.0"), cur, skipped, true));   // still shown when asked
        QVERIFY(UpdatePolicy::shouldOffer(rel("v0.10.1"), cur, skipped, false));  // newer than the skipped one

        ReleaseInfo draft = rel("v0.10.0");
        draft.draft = true;
        QVERIFY(!UpdatePolicy::shouldOffer(draft, cur, none, true));

        // Persisted next to (not in) zrecord.ini.
        QVERIFY(UpdateState::filePath().endsWith(QStringLiteral("/zrecord/update-state.ini")));
        UpdateState st;
        st.lastCheck = QDateTime::fromString(QStringLiteral("2026-10-04T12:00:00Z"), Qt::ISODate);
        st.skippedVersion = QStringLiteral("v0.10.0");
        st.save();
        const UpdateState back = UpdateState::load();
        QCOMPARE(back.lastCheck, st.lastCheck);
        QCOMPARE(back.skippedVersion, st.skippedVersion);
        QFile::remove(UpdateState::filePath());
    }

    void checkerAgainstMock()
    {
        MockGitHub gh;
        serveRelease(gh);
        UpdateChecker c;
        c.setApiUrl(QUrl(gh.url(QStringLiteral("/latest"))));
        QSignalSpy done(&c, &UpdateChecker::finished);
        c.check();
        QVERIFY(done.wait(5000));
        auto r = done.takeFirst().at(0).value<UpdateChecker::Result>();
        QCOMPARE(r.outcome, UpdateChecker::Outcome::Release);
        QCOMPARE(r.release.tag, QStringLiteral("v0.10.0"));
        QVERIFY(gh.lastUserAgent.startsWith("zrecord/"));
        QCOMPARE(gh.lastAccept, QByteArray("application/vnd.github+json"));
        QVERIFY(UpdateChecker::defaultApiUrl() == QStringLiteral("https://api.github.com/repos/sbj-ee/zrecord/releases/latest"));
    }

    void checkerFailures()
    {
        MockGitHub gh;
        UpdateChecker c;
        c.setTimeoutMs(400);
        QSignalSpy done(&c, &UpdateChecker::finished);
        auto run = [&](const QString &path) {
            c.setApiUrl(QUrl(gh.url(path)));
            c.check();
            if (!done.wait(5000)) {
                qFatal("no result for %s", qPrintable(path));
            }
            return done.takeFirst().at(0).value<UpdateChecker::Result>();
        };

        // 404: no release published yet.
        auto r = run(QStringLiteral("/none"));
        QCOMPARE(r.outcome, UpdateChecker::Outcome::NoRelease);
        QVERIFY(r.error.contains(QStringLiteral("No published")));

        // Rate limited (GitHub: 403 + x-ratelimit-remaining: 0); the reset time is shown.
        const qint64 reset = QDateTime::currentSecsSinceEpoch() + 1800;
        gh.routes[QStringLiteral("/limited")] = {403, R"({"message":"API rate limit exceeded"})",
                                                 {{"x-ratelimit-remaining", "0"},
                                                  {"x-ratelimit-reset", QByteArray::number(reset)}}};
        r = run(QStringLiteral("/limited"));
        QCOMPARE(r.outcome, UpdateChecker::Outcome::Error);
        QVERIFY(r.rateLimited);
        QVERIFY2(r.error.contains(QStringLiteral("rate limit")), qPrintable(r.error));
        QVERIFY(r.error.contains(QDateTime::fromSecsSinceEpoch(reset).toString(QStringLiteral("HH:mm"))));
        gh.routes[QStringLiteral("/429")] = {429, "", {}};
        r = run(QStringLiteral("/429"));
        QVERIFY(r.rateLimited);
        // A 403 that isn't the rate limit.
        gh.routes[QStringLiteral("/forbidden")] = {403, "", {{"x-ratelimit-remaining", "42"}}};
        r = run(QStringLiteral("/forbidden"));
        QVERIFY(!r.rateLimited);
        QVERIFY(r.error.contains(QStringLiteral("403")));
        gh.routes[QStringLiteral("/500")] = {500, "", {}};
        QVERIFY(run(QStringLiteral("/500")).error.contains(QStringLiteral("HTTP 500")));

        // Garbage instead of JSON.
        gh.routes[QStringLiteral("/garbage")] = {200, "<html>captive portal</html>", {}};
        r = run(QStringLiteral("/garbage"));
        QCOMPARE(r.outcome, UpdateChecker::Outcome::Error);
        QVERIFY(r.error.contains(QStringLiteral("could not read")));

        // Server never answers: times out.
        gh.routes[QStringLiteral("/hang")].hang = true;
        r = run(QStringLiteral("/hang"));
        QCOMPARE(r.outcome, UpdateChecker::Outcome::Error);
        QVERIFY2(r.error.contains(QStringLiteral("timed out")), qPrintable(r.error));

        // No network: nothing listening.
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        const quint16 deadPort = probe.serverPort();
        probe.close();
        c.setApiUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/latest").arg(deadPort)));
        c.check();
        QVERIFY(done.wait(5000));
        r = done.takeFirst().at(0).value<UpdateChecker::Result>();
        QCOMPARE(r.outcome, UpdateChecker::Outcome::Error);
        QVERIFY2(r.error.contains(QStringLiteral("Couldn't reach GitHub")), qPrintable(r.error));
    }

    void downloaderVerifies()
    {
        MockGitHub gh;
        serveRelease(gh);
        const auto rel = ReleaseInfo::fromJson(fixture(QStringLiteral("release-0.10.0.json"), gh.base()));
        QVERIFY(rel);
        UpdateDownloader d;
        QSignalSpy done(&d, &UpdateDownloader::finished);
        QSignalSpy prog(&d, &UpdateDownloader::progress);

        QTemporaryDir dir;
        d.start(*rel, dir.path());
        QVERIFY(done.wait(5000));
        auto args = done.takeFirst();
        QVERIFY2(args.at(0).toBool(), qPrintable(args.at(2).toString()));
        const QString deb = args.at(1).toString();
        QCOMPARE(deb, dir.filePath(platformPackageName()));
        QCOMPARE(sha256OfFile(deb).toLatin1(), sha256Hex(debBytes));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("SHA256SUMS"))));
        QVERIFY(QFile::exists(dir.filePath(QStringLiteral("SHA256SUMS.minisig"))));
        QCOMPARE(d.verifiedSha256().toLatin1(), sha256Hex(debBytes));
        QVERIFY(!prog.isEmpty());

        // Item 5: SHA256SUMS signed by another key: refused before the package
        // is even requested.
        {
            serveRelease(gh, /*goodSum=*/true, /*goodSignature=*/false);
            gh.requests.clear();
            QTemporaryDir d3;
            d.start(*rel, d3.path());
            QVERIFY(done.wait(5000));
            args = done.takeFirst();
            QVERIFY(!args.at(0).toBool());
            QVERIFY2(args.at(2).toString().contains(QStringLiteral("signature is not valid")), qPrintable(args.at(2).toString()));
            for (const QString &req : gh.requests) {
                QVERIFY2(!req.contains(platformPackageName()), qPrintable(req));
            }
            QVERIFY(!QFile::exists(d3.filePath(platformPackageName())));
        }
        // A SHA256SUMS swapped after signing (attacker rewrote the hashes).
        {
            serveRelease(gh);
            gh.routes[QStringLiteral("/download/v0.10.0/SHA256SUMS")].body.replace("other.tar.gz", "other.tar.gx");
            QTemporaryDir d4;
            d.start(*rel, d4.path());
            QVERIFY(done.wait(5000));
            args = done.takeFirst();
            QVERIFY(!args.at(0).toBool());
            QVERIFY(args.at(2).toString().contains(QStringLiteral("does not match the file")));
        }
        // Unsigned release (no SHA256SUMS.minisig asset): refused, nothing fetched.
        {
            QByteArray json = fixture(QStringLiteral("release-0.10.0.json"), gh.base());
            json.replace("\"SHA256SUMS.minisig\"", "\"SHA256SUMS.asc\"");
            const auto unsigned_ = ReleaseInfo::fromJson(json);
            QVERIFY(unsigned_ && !unsigned_->signatureAsset());
            const int before = gh.requests.size();
            QTemporaryDir d5;
            d.start(*unsigned_, d5.path());
            QVERIFY(done.wait(5000));
            args = done.takeFirst();
            QVERIFY(!args.at(0).toBool());
            QVERIFY(args.at(2).toString().contains(QStringLiteral("SHA256SUMS.minisig")));
            QCOMPARE(gh.requests.size(), before);
        }
        // A build without a signing key never installs.
        {
            setUpdateSigningPublicKeyForTests(QString());
            const bool builtInKey = !updateSigningPublicKey().isEmpty();
            serveRelease(gh);
            const int before = gh.requests.size();
            QTemporaryDir d6;
            d.start(*rel, d6.path());
            QVERIFY(done.wait(5000));
            args = done.takeFirst();
            setUpdateSigningPublicKeyForTests(signKey.publicKeyBase64());
            if (!builtInKey) {
                QVERIFY(!args.at(0).toBool());
                QVERIFY(args.at(2).toString().contains(QStringLiteral("no update-signing key")));
                QCOMPARE(gh.requests.size(), before);
            } else {
                QVERIFY(!args.at(0).toBool()); // the real key didn't sign this test release
            }
        }
        serveRelease(gh);

        // Mismatch: refused, and the unverified package is deleted.
        serveRelease(gh, /*goodSum=*/false);
        QTemporaryDir dir2;
        d.start(*rel, dir2.path());
        QVERIFY(done.wait(5000));
        args = done.takeFirst();
        QVERIFY(!args.at(0).toBool());
        QVERIFY(args.at(2).toString().contains(QStringLiteral("does not match")));
        QVERIFY(!QFile::exists(dir2.filePath(platformPackageName())));

        // SHA256SUMS vanished from the server (404).
        gh.routes.erase(QStringLiteral("/download/v0.10.0/SHA256SUMS"));
        d.start(*rel, dir2.path());
        QVERIFY(done.wait(5000));
        args = done.takeFirst();
        QVERIFY(!args.at(0).toBool());
        QVERIFY2(args.at(2).toString().contains(QStringLiteral("SHA256SUMS is no longer")), qPrintable(args.at(2).toString()));

        // Asset missing from the release JSON: refused before any download.
        const auto noDeb = ReleaseInfo::fromJson(fixture(QStringLiteral("release-no-deb.json"), gh.base()));
        const int before = gh.requests.size();
        d.start(*noDeb, dir2.path());
        QVERIFY(done.wait(5000));
        args = done.takeFirst();
        QVERIFY(!args.at(0).toBool());
#if defined(Q_OS_MACOS)
        QVERIFY(args.at(2).toString().contains(QStringLiteral("missing zrecord-0.11.0-Darwin.dmg")));
#else
        QVERIFY(args.at(2).toString().contains(QStringLiteral("missing zrecord_0.11.0_amd64.deb")));
#endif
        QCOMPARE(gh.requests.size(), before);
    }

    void dialogButtons()
    {
        const auto rel = ReleaseInfo::fromJson(fixture(QStringLiteral("release-0.10.0.json")));
        UpdateDialog dlg(*rel, QStringLiteral("0.9.0"));
        QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("updateHeading"))->text().contains(QStringLiteral("v0.10.0")));
        QVERIFY(dlg.findChild<QLabel *>(QStringLiteral("updateSubheading"))->text().contains(QStringLiteral("You have 0.9.0")));
        QVERIFY(dlg.notes()->toPlainText().contains(QStringLiteral("Auto-update")));
        QVERIFY(dlg.installButton()->isEnabled());
        QVERIFY(dlg.installButton()->isDefault());
        dlg.open();
        dlg.skipButton()->click();
        QCOMPARE(dlg.choice(), UpdateDialog::Skip);
        QCOMPARE(dlg.result(), int(UpdateDialog::Skip));
        dlg.open();
        dlg.laterButton()->click();
        QCOMPARE(dlg.result(), int(UpdateDialog::Later));
        dlg.open();
        dlg.installButton()->click();
        QCOMPARE(dlg.result(), int(UpdateDialog::Install));

        // No amd64 .deb: Install is disabled and says why.
        const auto noDeb = ReleaseInfo::fromJson(fixture(QStringLiteral("release-no-deb.json")));
        UpdateDialog d2(*noDeb, QStringLiteral("0.9.0"));
        QVERIFY(!d2.installButton()->isEnabled());
        QVERIFY(d2.findChild<QLabel *>(QStringLiteral("updateInstallNote"))->text().contains(QStringLiteral("can't be")));
    }

    void installCommandLine()
    {
        QCOMPARE(UpdateManager::installCommand(QStringLiteral("/tmp/zrecord-install-x/zrecord_0.10.0_amd64.deb")),
                 (QStringList{"/usr/bin/pkexec", "/usr/bin/apt", "install", "-y",
                              "/tmp/zrecord-install-x/zrecord_0.10.0_amd64.deb"}));
        // Relative names become absolute (apt must see a file, not a package name).
        QVERIFY(QFileInfo(UpdateManager::installCommand(QStringLiteral("z.deb")).last()).isAbsolute());
    }

    // The installer gets a private copy, re-hashed right before it runs.
    void stagingRechecksHash()
    {
        QTemporaryDir dl;
        const QString pkg = dl.filePath(QStringLiteral("zrecord_0.10.0_amd64.deb"));
        {
            QFile f(pkg);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(debBytes);
        }
        const QString sha = QString::fromLatin1(sha256Hex(debBytes));
        QTemporaryDir into(QDir::tempPath() + QStringLiteral("/zrecord-install-XXXXXX"));
        QString err;
        const QString staged = UpdateManager::stageVerifiedPackage(pkg, sha, into, &err);
        QVERIFY2(!staged.isEmpty(), qPrintable(err));
        QVERIFY(QFileInfo(staged).isAbsolute());
        QVERIFY(staged.startsWith(into.path()));
        QCOMPARE(QFileInfo(into.path()).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther
                                                         | QFileDevice::WriteGroup | QFileDevice::WriteOther
                                                         | QFileDevice::ExeGroup | QFileDevice::ExeOther),
                 QFileDevice::Permissions());
        // Swapped after verification: refused, no copy left.
        {
            QFile f(pkg);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("evil");
        }
        QTemporaryDir into2;
        QVERIFY(UpdateManager::stageVerifiedPackage(pkg, sha, into2, &err).isEmpty());
        QVERIFY(err.contains(QStringLiteral("changed after it was verified")));
        QVERIFY(QDir(into2.path()).isEmpty());
        QVERIFY(UpdateManager::stageVerifiedPackage(pkg, QString(), into2, &err).isEmpty());
    }

    void releasePageUrlIsChecked()
    {
        QVERIFY(isTrustedReleasePageUrl(QStringLiteral("https://github.com/sbj-ee/zrecord/releases/tag/v1.0.0")));
        QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("http://github.com/sbj-ee/zrecord/releases/tag/v1.0.0")));
        QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("https://github.com.evil/sbj-ee/zrecord/releases/x")));
        QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("https://evil@github.com/sbj-ee/zrecord/releases/x")));
        QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("https://github.com/other/zrecord/releases/x")));
        QVERIFY(!isTrustedReleasePageUrl(QStringLiteral("file:///etc/passwd")));
        QVERIFY(!isTrustedReleasePageUrl(QString()));
        // An untrusted html_url gets no link in the dialog.
        QByteArray json = fixture(QStringLiteral("release-0.10.0.json"));
        json.replace("https://github.com/sbj-ee/zrecord/releases/tag/v0.10.0", "file:///etc/passwd");
        const auto rel = ReleaseInfo::fromJson(json);
        QVERIFY(rel);
        UpdateDialog dlg(*rel, QStringLiteral("0.9.0"));
        QVERIFY(!dlg.findChild<QLabel *>(QStringLiteral("updateSubheading"))->text().contains(QStringLiteral("href")));
    }

    void updateUrlOverrideOnlyInDevBuilds()
    {
        qputenv("ZRECORD_UPDATE_URL", "http://127.0.0.1:1/evil");
        UpdateChecker c;
#if defined(ZRECORD_DEV_OVERRIDES)
        QCOMPARE(c.apiUrl(), QUrl(QStringLiteral("http://127.0.0.1:1/evil")));
#else
        QCOMPARE(c.apiUrl(), QUrl(UpdateChecker::defaultApiUrl()));
#endif
        qunsetenv("ZRECORD_UPDATE_URL");
        QCOMPARE(UpdateChecker::defaultApiUrl(),
                 QStringLiteral("https://api.github.com/repos/sbj-ee/zrecord/releases/latest"));
    }

    // Help > Check for Updates when up to date / failing reports it; Skip is remembered.
    void manualCheckAndSkip()
    {
        MockGitHub gh;
        serveRelease(gh);
        UpdateManager &m = freshManager(gh);
        QSignalSpy checked(&m, &UpdateManager::checkFinished);

        m.setCurrentVersionForTests(QStringLiteral("0.10.0"));
        m.checkNow(nullptr);
        QVERIFY(checked.wait(5000));
        QCOMPARE(checked.takeFirst().at(0).toBool(), false);
        QTRY_VERIFY(findTop<QMessageBox>("updateMessage"));
        QVERIFY(findTop<QMessageBox>("updateMessage")->text().contains(QStringLiteral("latest version")));
        closeAllTop();
        QVERIFY(!QFile::exists(UpdateState::filePath())); // manual checks don't move the daily clock

        m.setCurrentVersionForTests(QStringLiteral("0.9.0"));
        m.checkNow(nullptr);
        QVERIFY(checked.wait(5000));
        QCOMPARE(checked.takeFirst().at(0).toBool(), true);
        QTRY_VERIFY(findTop<UpdateDialog>("updateDialog"));
        findTop<UpdateDialog>("updateDialog")->skipButton()->click();
        QTRY_COMPARE(m.stage(), UpdateManager::Stage::Idle);
        QCOMPARE(UpdateState::load().skippedVersion, QStringLiteral("v0.10.0"));

        // Startup check: quiet for the skipped version, and records the check time.
        QVERIFY(m.startupCheckIfDue(nullptr));
        QVERIFY(checked.wait(5000));
        QCOMPARE(checked.takeFirst().at(0).toBool(), false);
        QTest::qWait(50);
        QVERIFY(!findTop<UpdateDialog>("updateDialog"));
        QVERIFY(!findTop<QMessageBox>("updateMessage"));
        QVERIFY(UpdateState::load().lastCheck.isValid());
        // ...and not again the same day.
        QVERIFY(!m.startupCheckIfDue(nullptr));

        // A manual check still offers the skipped version.
        m.checkNow(nullptr);
        QVERIFY(checked.wait(5000));
        QTRY_VERIFY(findTop<UpdateDialog>("updateDialog"));
        findTop<UpdateDialog>("updateDialog")->laterButton()->click();
        QTRY_COMPARE(m.stage(), UpdateManager::Stage::Idle);
        QCOMPARE(UpdateState::load().skippedVersion, QStringLiteral("v0.10.0")); // Later doesn't skip

        // Errors are reported for manual checks only.
        gh.routes[QStringLiteral("/latest")] = {403, "", {{"x-ratelimit-remaining", "0"}}};
        m.checkNow(nullptr);
        QVERIFY(checked.wait(5000));
        QTRY_VERIFY(findTop<QMessageBox>("updateMessage"));
        QVERIFY(findTop<QMessageBox>("updateMessage")->text().contains(QStringLiteral("rate limit")));
        closeAllTop();
        QFile::remove(UpdateState::filePath());
        QVERIFY(m.startupCheckIfDue(nullptr));
        QVERIFY(checked.wait(5000));
        QTest::qWait(50);
        QVERIFY(!findTop<QMessageBox>("updateMessage"));
        QVERIFY(UpdateState::load().lastCheck.isValid()); // a failed attempt still counts for the day

        // Turned off in the Help menu: no startup check at all.
        QFile::remove(UpdateState::filePath());
        UpdateManager::setCheckAtStartupEnabled(false);
        const int before = gh.requests.size();
        QVERIFY(!m.startupCheckIfDue(nullptr));
        QTest::qWait(50);
        QCOMPARE(gh.requests.size(), before);
        UpdateManager::setCheckAtStartupEnabled(true);
    }

    // Install: download -> verify -> (mock) pkexec apt install -> restart offer,
    // warning about unsaved changes.
    void installFlow()
    {
        MockGitHub gh;
        serveRelease(gh);
        UpdateManager &m = freshManager(gh);
        QTemporaryDir out;
        const QString marker = out.filePath(QStringLiteral("installed"));
        // "$0" is the staged package's absolute path (appended); cwd is its private dir.
        m.setInstallerForTests(QStringLiteral("/bin/sh"),
                               {QStringLiteral("-c"),
                                QStringLiteral("test -f \"$0\" && pwd > '%1' && echo \"$0\" >> '%1' && "
                                               "ls -ld . | cut -c1-10 >> '%1'")
                                    .arg(marker)});
        int restarts = 0;
        m.setRestartHandlerForTests([&restarts]() { ++restarts; });
        QSignalSpy installed(&m, &UpdateManager::installFinished);

        // A window with a document that has unsaved changes, for the restart warning.
        FakeHost w;
        w.unsaved = true;
        m.setHost(&w);
        w.show();
        QVERIFY(QTest::qWaitForWindowExposed(&w));

        m.checkNow(&w);
        QTRY_VERIFY_WITH_TIMEOUT(findTop<UpdateDialog>("updateDialog"), 5000);
        findTop<UpdateDialog>("updateDialog")->installButton()->click();
        QVERIFY(installed.wait(10000));
        auto args = installed.takeFirst();
        QVERIFY2(args.at(0).toBool(), qPrintable(args.at(1).toString()));
        QFile mk(marker);
        QVERIFY(mk.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(mk.readAll()).trimmed().split(QLatin1Char('\n'));
        QVERIFY2(lines.value(1).startsWith(QLatin1Char('/')), qPrintable(lines.value(1)));
        QVERIFY(lines.value(1).endsWith(QLatin1Char('/') + platformPackageName()));
        QVERIFY(lines.value(0).contains(QStringLiteral("zrecord-install-")));
        QCOMPARE(lines.value(2), QStringLiteral("drwx------"));
        QTRY_VERIFY(findTop<QMessageBox>("updateRestart"));
        QMessageBox *box = findTop<QMessageBox>("updateRestart");
        QVERIFY2(box->text().contains(QStringLiteral("unsaved changes")), qPrintable(box->text()));
        QCOMPARE(box->defaultButton()->objectName(), QStringLiteral("restartLater"));
        // The user cancels the "save changes?" prompt: nothing restarts.
        w.allowClose = false;
        box->findChild<QPushButton *>(QStringLiteral("restartNow"))->click();
        QCOMPARE(w.closeCalls, 1);
        QCOMPARE(restarts, 0);
        QTRY_VERIFY(!QDir(lines.value(0)).exists()); // download dir removed after a successful install
        closeAllTop();
        // Restart again (a clean document this time): the window closes, then the new copy starts.
        w.unsaved = false;
        w.allowClose = true;
        m.checkNow(&w);
        QTRY_VERIFY_WITH_TIMEOUT(findTop<UpdateDialog>("updateDialog"), 5000);
        findTop<UpdateDialog>("updateDialog")->installButton()->click();
        QVERIFY(installed.wait(10000));
        QVERIFY(installed.takeFirst().at(0).toBool());
        QTRY_VERIFY(findTop<QMessageBox>("updateRestart"));
        box = findTop<QMessageBox>("updateRestart");
        QVERIFY(!box->text().contains(QStringLiteral("unsaved")));
        QCOMPARE(box->defaultButton()->objectName(), QStringLiteral("restartNow"));
        box->findChild<QPushButton *>(QStringLiteral("restartNow"))->click();
        QCOMPARE(w.closeCalls, 2);
        QCOMPARE(restarts, 1);

        // Checksum mismatch: refused with a message; the installer never runs.
        QFile::remove(marker);
        serveRelease(gh, /*goodSum=*/false);
        m.checkNow(&w);
        QTRY_VERIFY_WITH_TIMEOUT(findTop<UpdateDialog>("updateDialog"), 5000);
        findTop<UpdateDialog>("updateDialog")->installButton()->click();
        QVERIFY(installed.wait(10000));
        args = installed.takeFirst();
        QVERIFY(!args.at(0).toBool());
        QVERIFY(args.at(1).toString().contains(QStringLiteral("does not match")));
        QTRY_VERIFY(findTop<QMessageBox>("updateMessage"));
        QVERIFY(!QFile::exists(marker));
        closeAllTop();

        // apt fails (or the polkit prompt is dismissed): falls back, keeps the verified .deb.
        serveRelease(gh);
        m.setInstallerForTests(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("exit 126")});
        m.checkNow(&w);
        QTRY_VERIFY_WITH_TIMEOUT(findTop<UpdateDialog>("updateDialog"), 5000);
        findTop<UpdateDialog>("updateDialog")->installButton()->click();
        QVERIFY(installed.wait(10000));
        args = installed.takeFirst();
        QVERIFY(!args.at(0).toBool());
        const QString msg = args.at(1).toString();
        QVERIFY2(msg.contains(QStringLiteral("password prompt was dismissed")), qPrintable(msg));
        const QString kept = msg.section(QStringLiteral("The verified package is at:\n"), 1).section(QLatin1Char('\n'), 0, 0);
        QVERIFY2(kept.endsWith(platformPackageName()), qPrintable(kept));
        QVERIFY(QFile::exists(kept));
        QDir(QFileInfo(kept).absolutePath()).removeRecursively();
        closeAllTop();

        // Cancelling the download from the progress dialog.
        gh.routes[QStringLiteral("/download/v0.10.0/") + platformPackageName()].hang = true;
        m.checkNow(&w);
        QTRY_VERIFY_WITH_TIMEOUT(findTop<UpdateDialog>("updateDialog"), 5000);
        findTop<UpdateDialog>("updateDialog")->installButton()->click();
        QTRY_VERIFY(findTop<QProgressDialog>("updateProgress"));
        QTRY_COMPARE(m.stage(), UpdateManager::Stage::Downloading);
        findTop<QProgressDialog>("updateProgress")->findChild<QPushButton *>()->click(); // "Cancel"
        QTRY_COMPARE(installed.count(), 1); // abort() reports synchronously
        QVERIFY(installed.takeFirst().at(1).toString().contains(QStringLiteral("cancelled")));
        QCOMPARE(m.stage(), UpdateManager::Stage::Idle);
        QVERIFY(!findTop<QMessageBox>("updateMessage"));
    }
};

QTEST_MAIN(TstUpdate)
#include "update_test.moc"
