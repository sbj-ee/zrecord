#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

namespace zrecord {

// Auto-update: version comparison, the GitHub release
// JSON, SHA256SUMS verification and the "when to check / whom to tell" rules.
// No networking and no widgets here, so all of it is unit-tested with fixtures.

// Semantic version (https://semver.org): MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD],
// a leading 'v' allowed (git tags). Missing parts count as 0 ("1.2" = 1.2.0).
struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    QStringList prerelease; // "rc.1" -> {"rc", "1"}; empty = a release
    bool valid = false;

    static SemVer parse(const QString &text);
    QString toString() const;
    // <0, 0, >0. Build metadata is ignored; 1.0.0-rc.1 < 1.0.0; numeric
    // prerelease parts compare numerically and sort before alphanumeric ones.
    static int compare(const SemVer &a, const SemVer &b);
    bool operator<(const SemVer &o) const { return compare(*this, o) < 0; }
    bool operator==(const SemVer &o) const { return compare(*this, o) == 0; }
};

// True when `candidate` is a newer valid version than `current`.
bool isNewerVersion(const QString &candidate, const QString &current);

struct ReleaseAsset {
    QString name;
    QString url; // browser_download_url
    qint64 size = 0;
    bool isValid() const { return !name.isEmpty() && !url.isEmpty(); }
};

// GET /repos/sbj-ee/zrecord/releases/latest, the parts we use.
struct ReleaseInfo {
    QString tag;     // "v0.10.0"
    SemVer version;  // parsed tag
    QString name;    // release title
    QString notes;   // body (Markdown)
    QString htmlUrl; // release page
    QDateTime publishedAt;
    bool draft = false;
    bool prerelease = false;
    QList<ReleaseAsset> assets;

    // nullopt (with *error) for JSON that isn't a usable release.
    static std::optional<ReleaseInfo> fromJson(const QByteArray &json, QString *error = nullptr);
    // zrecord_<version>_amd64.deb for this release (the CPack DEB-DEFAULT name); nullopt when missing.
    std::optional<ReleaseAsset> debAsset() const;
    // zrecord-<version>-Darwin.dmg (Apple Silicon DragNDrop package).
    std::optional<ReleaseAsset> dmgAsset() const;
    // Platform package: .deb on Linux, .dmg on macOS.
    std::optional<ReleaseAsset> packageAsset() const;
    std::optional<ReleaseAsset> checksumAsset() const;  // "SHA256SUMS"
    std::optional<ReleaseAsset> signatureAsset() const; // "SHA256SUMS.minisig"
    QString versionString() const { return version.toString(); }
};

// SHA256SUMS ("<64 hex>  <name>" or "<64 hex> *<name>" per line, as written by sha256sum).
struct ChecksumResult {
    enum class Status { Ok, Mismatch, NotListed, BadFile, Unreadable };
    Status status = Status::Unreadable;
    QString expected; // lower-case hex from SHA256SUMS
    QString actual;   // lower-case hex of the file
    QString message() const;
    bool ok() const { return status == Status::Ok; }
};
// Expected hash for fileName, or empty.
QString checksumFor(const QByteArray &sha256sums, const QString &fileName);
QString sha256OfFile(const QString &path); // lower-case hex, empty if unreadable
ChecksumResult verifyChecksum(const QString &filePath, const QByteArray &sha256sums, const QString &fileName);

// The release page we may open in a browser: https://github.com/sbj-ee/zrecord/releases/...
// (the URL comes from the API response, so don't open anything else).
bool isTrustedReleasePageUrl(const QString &url);

// Updates are only installed when SHA256SUMS carries a valid minisign
// signature (SHA256SUMS.minisig) by the release key: base64 public key line,
// compiled in (core/UpdateSigningKey.cpp). Empty: this build can't verify
// updates, and Install refuses.
QString updateSigningPublicKey();
void setUpdateSigningPublicKeyForTests(const QString &base64); // empty: back to the built-in key

// Persistent state: ~/.config/zrecord/update-state.ini (not zrecord.ini, so
// writing the last-check time doesn't make every window reload its settings).
struct UpdateState {
    QDateTime lastCheck;    // last automatic check (UTC)
    QString skippedVersion; // "Skip this version": the tag, e.g. "v0.10.0"

    static QString filePath();
    static UpdateState load();
    void save() const;
};

struct UpdatePolicy {
    static constexpr qint64 kMinSecondsBetweenChecks = 24 * 60 * 60; // at most once a day
    // Automatic check at startup: enabled in Preferences and the last one is a day old.
    static bool shouldCheckAtStartup(bool enabled, const UpdateState &state, const QDateTime &now);
    // Whether to show the dialog for `release`: it must be newer than the
    // running version; a skipped tag only stays quiet for automatic checks
    // (anything newer than the skipped one is offered again).
    static bool shouldOffer(const ReleaseInfo &release, const QString &currentVersion, const UpdateState &state,
                            bool manual);
};

} // namespace zrecord
