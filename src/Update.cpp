#include "Update.hpp"

#include <QUrl>

#include <QtGlobal>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace zrecord {

namespace {

bool isNumeric(const QString &s)
{
    if (s.isEmpty()) {
        return false;
    }
    for (QChar c : s) {
        if (!c.isDigit()) {
            return false;
        }
    }
    return true;
}

} // namespace

SemVer SemVer::parse(const QString &text)
{
    SemVer v;
    QString s = text.trimmed();
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V'))) {
        s.remove(0, 1);
    }
    if (const qsizetype plus = s.indexOf(QLatin1Char('+')); plus >= 0) {
        s.truncate(plus); // build metadata: ignored
    }
    QString pre;
    // Debian spells prereleases with '~' (1.0.0~rc1).
    qsizetype dash = s.indexOf(QLatin1Char('-'));
    if (dash < 0) {
        dash = s.indexOf(QLatin1Char('~'));
    }
    if (dash >= 0) {
        pre = s.mid(dash + 1);
        s.truncate(dash);
    }
    const QStringList parts = s.split(QLatin1Char('.'));
    if (parts.isEmpty() || parts.size() > 3) {
        return v;
    }
    int nums[3] = {0, 0, 0};
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (!isNumeric(parts[i]) || parts[i].size() > 9) {
            return v;
        }
        nums[i] = parts[i].toInt();
    }
    v.major = nums[0];
    v.minor = nums[1];
    v.patch = nums[2];
    if (!pre.isEmpty()) {
        v.prerelease = pre.split(QLatin1Char('.'));
        for (const QString &p : v.prerelease) {
            if (p.isEmpty()) {
                return SemVer{};
            }
        }
    } else if (dash >= 0) {
        return SemVer{}; // "1.0.0-"
    }
    v.valid = true;
    return v;
}

QString SemVer::toString() const
{
    QString s = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
    if (!prerelease.isEmpty()) {
        s += QLatin1Char('-') + prerelease.join(QLatin1Char('.'));
    }
    return s;
}

int SemVer::compare(const SemVer &a, const SemVer &b)
{
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }
    // A release ranks above any of its prereleases.
    if (a.prerelease.isEmpty() || b.prerelease.isEmpty()) {
        return a.prerelease.isEmpty() == b.prerelease.isEmpty() ? 0 : (a.prerelease.isEmpty() ? 1 : -1);
    }
    for (qsizetype i = 0; i < std::min(a.prerelease.size(), b.prerelease.size()); ++i) {
        const QString &x = a.prerelease[i];
        const QString &y = b.prerelease[i];
        const bool nx = isNumeric(x);
        const bool ny = isNumeric(y);
        if (nx && ny) {
            const qint64 ix = x.toLongLong();
            const qint64 iy = y.toLongLong();
            if (ix != iy) {
                return ix < iy ? -1 : 1;
            }
        } else if (nx != ny) {
            return nx ? -1 : 1; // numeric identifiers sort first
        } else if (const int c = QString::compare(x, y); c != 0) {
            return c < 0 ? -1 : 1;
        }
    }
    if (a.prerelease.size() != b.prerelease.size()) {
        return a.prerelease.size() < b.prerelease.size() ? -1 : 1;
    }
    return 0;
}

bool isNewerVersion(const QString &candidate, const QString &current)
{
    const SemVer c = SemVer::parse(candidate);
    const SemVer r = SemVer::parse(current);
    return c.valid && r.valid && SemVer::compare(c, r) > 0;
}

std::optional<ReleaseInfo> ReleaseInfo::fromJson(const QByteArray &json, QString *error)
{
    auto fail = [error](const QString &m) -> std::optional<ReleaseInfo> {
        if (error) {
            *error = m;
        }
        return std::nullopt;
    };
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(QStringLiteral("GitHub sent a reply zrecord could not read."));
    }
    const QJsonObject o = doc.object();
    ReleaseInfo r;
    r.tag = o.value(QStringLiteral("tag_name")).toString();
    if (r.tag.isEmpty()) {
        return fail(QStringLiteral("No published zrecord release was found on GitHub."));
    }
    r.version = SemVer::parse(r.tag);
    if (!r.version.valid) {
        return fail(QStringLiteral("The latest release has a tag zrecord doesn't understand (%1).").arg(r.tag));
    }
    r.name = o.value(QStringLiteral("name")).toString();
    r.notes = o.value(QStringLiteral("body")).toString();
    r.htmlUrl = o.value(QStringLiteral("html_url")).toString();
    r.publishedAt = QDateTime::fromString(o.value(QStringLiteral("published_at")).toString(), Qt::ISODate);
    r.draft = o.value(QStringLiteral("draft")).toBool();
    r.prerelease = o.value(QStringLiteral("prerelease")).toBool();
    for (const QJsonValue &v : o.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject a = v.toObject();
        ReleaseAsset asset;
        asset.name = a.value(QStringLiteral("name")).toString();
        asset.url = a.value(QStringLiteral("browser_download_url")).toString();
        // toInteger: 0 for anything that isn't a whole number in qint64 range
        // (a double cast would be undefined behaviour; found by fuzzing).
        const qint64 size = a.value(QStringLiteral("size")).toInteger(0);
        asset.size = size < 0 ? 0 : size;
        if (asset.isValid()) {
            r.assets << asset;
        }
    }
    return r;
}

std::optional<ReleaseAsset> ReleaseInfo::debAsset() const
{
    const QString name = QStringLiteral("zrecord_%1_amd64.deb").arg(version.toString());
    for (const ReleaseAsset &a : assets) {
        if (a.name == name) {
            return a;
        }
    }
    return std::nullopt;
}

std::optional<ReleaseAsset> ReleaseInfo::dmgAsset() const
{
    const QStringList names{QStringLiteral("zrecord-%1-Darwin.dmg").arg(version.toString())};
    for (const ReleaseAsset &a : assets) {
        if (names.contains(a.name)) {
            return a;
        }
    }
    return std::nullopt;
}

std::optional<ReleaseAsset> ReleaseInfo::packageAsset() const
{
#if defined(Q_OS_MACOS)
    return dmgAsset();
#else
    return debAsset();
#endif
}
std::optional<ReleaseAsset> ReleaseInfo::checksumAsset() const
{
    for (const ReleaseAsset &a : assets) {
        if (a.name == QLatin1String("SHA256SUMS")) {
            return a;
        }
    }
    return std::nullopt;
}

std::optional<ReleaseAsset> ReleaseInfo::signatureAsset() const
{
    for (const ReleaseAsset &a : assets) {
        if (a.name == QLatin1String("SHA256SUMS.minisig")) {
            return a;
        }
    }
    return std::nullopt;
}

bool isTrustedReleasePageUrl(const QString &url)
{
    const QUrl u(url, QUrl::StrictMode);
    return u.isValid() && u.scheme() == QLatin1String("https") && u.host() == QLatin1String("github.com")
        && u.port() == -1 && u.userInfo().isEmpty()
        && u.path().startsWith(QLatin1String("/sbj-ee/zrecord/releases/"));
}

QString checksumFor(const QByteArray &sha256sums, const QString &fileName)
{
    static const QRegularExpression line(QStringLiteral("^([0-9a-fA-F]{64}) [ *](.+)$"));
    for (const QByteArray &raw : sha256sums.split('\n')) {
        const QString l = QString::fromUtf8(raw).trimmed();
        const auto m = line.match(l);
        if (!m.hasMatch()) {
            continue;
        }
        QString name = m.captured(2).trimmed();
        if (name.startsWith(QLatin1String("./"))) {
            name.remove(0, 2);
        }
        if (name == fileName) {
            return m.captured(1).toLower();
        }
    }
    return {};
}

QString sha256OfFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) {
        return {};
    }
    return QString::fromLatin1(h.result().toHex());
}

ChecksumResult verifyChecksum(const QString &filePath, const QByteArray &sha256sums, const QString &fileName)
{
    ChecksumResult r;
    r.expected = checksumFor(sha256sums, fileName);
    if (r.expected.isEmpty()) {
        r.status = sha256sums.trimmed().isEmpty() ? ChecksumResult::Status::BadFile : ChecksumResult::Status::NotListed;
        return r;
    }
    r.actual = sha256OfFile(filePath);
    if (r.actual.isEmpty()) {
        r.status = ChecksumResult::Status::Unreadable;
        return r;
    }
    r.status = r.actual == r.expected ? ChecksumResult::Status::Ok : ChecksumResult::Status::Mismatch;
    return r;
}

QString ChecksumResult::message() const
{
    switch (status) {
    case Status::Ok:
        return QStringLiteral("SHA256 verified (%1).").arg(actual);
    case Status::Mismatch:
        return QStringLiteral("The downloaded package does not match its published SHA256 checksum, so it was "
                              "not installed.\nExpected: %1\nGot:      %2")
            .arg(expected, actual);
    case Status::NotListed:
        return QStringLiteral("SHA256SUMS does not list the package, so it can't be verified and was not installed.");
    case Status::BadFile:
        return QStringLiteral("The release's SHA256SUMS file is empty or unreadable, so the package was not installed.");
    case Status::Unreadable:
        break;
    }
    return QStringLiteral("The downloaded package could not be read, so it was not installed.");
}

QString UpdateState::filePath()
{
    // $XDG_CONFIG_HOME wins when set (Linux, and the tests); otherwise the
    // platform's generic config folder. Its own file, so writing the last-check
    // time never touches zrecord's regular QSettings.
    QString root;
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    if (!xdg.isEmpty()) {
        root = QFile::decodeName(xdg);
    } else {
        root = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    }
    return root + QStringLiteral("/zrecord/update-state.ini");
}

UpdateState UpdateState::load()
{
    QSettings s(filePath(), QSettings::IniFormat);
    UpdateState st;
    st.lastCheck = QDateTime::fromString(s.value(QStringLiteral("updates/lastCheck")).toString(), Qt::ISODate);
    st.skippedVersion = s.value(QStringLiteral("updates/skippedVersion")).toString();
    return st;
}

void UpdateState::save() const
{
    QDir().mkpath(QFileInfo(filePath()).absolutePath());
    QSettings s(filePath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("updates/lastCheck"), lastCheck.isValid() ? lastCheck.toUTC().toString(Qt::ISODate) : QString());
    s.setValue(QStringLiteral("updates/skippedVersion"), skippedVersion);
    s.sync();
}

bool UpdatePolicy::shouldCheckAtStartup(bool enabled, const UpdateState &state, const QDateTime &now)
{
    if (!enabled) {
        return false;
    }
    if (!state.lastCheck.isValid()) {
        return true;
    }
    const qint64 age = state.lastCheck.secsTo(now);
    // A clock set back (negative age) also allows a check rather than blocking it for days.
    return age < 0 || age >= kMinSecondsBetweenChecks;
}

bool UpdatePolicy::shouldOffer(const ReleaseInfo &release, const QString &currentVersion, const UpdateState &state,
                               bool manual)
{
    if (release.draft || !isNewerVersion(release.tag, currentVersion)) {
        return false;
    }
    if (manual || state.skippedVersion.isEmpty()) {
        return true;
    }
    // Skipped: quiet for that version (and anything older), offered again for newer ones.
    const SemVer skipped = SemVer::parse(state.skippedVersion);
    return skipped.valid ? SemVer::compare(release.version, skipped) > 0 : release.tag != state.skippedVersion;
}

} // namespace zrecord
