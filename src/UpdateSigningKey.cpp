// The public key that release SHA256SUMS files are signed with (minisign).
// Not secret. Release builds refuse to install an update unless
// SHA256SUMS.minisig verifies against this key.
//
// Release key ID DBC46B23C3AF0EF3 (minisign -G on the maintainer's machine,
// October 2026): the shared release key also used by zwriter and zedit, so all
// of Stephen's apps verify against one public key. The matching secret key
// lives only on the maintainer's machine and in this repository's MINISIGN_SECRET_KEY /
// MINISIGN_PASSWORD Actions secrets. To rotate it, see docs/RELEASING.md,
// "Signing key".
#include "Update.hpp"

namespace zrecord {

namespace {
constexpr const char kUpdateSigningPublicKey[] = "RWTzDq/DI2vE2xMeF01QWTxP2lwwlxGL9M1UxQE4EFV5kVmTF84wwyi2";

QString &testKey()
{
    static QString k;
    return k;
}
} // namespace

QString updateSigningPublicKey()
{
    if (!testKey().isEmpty()) {
        return testKey();
    }
    return QString::fromLatin1(kUpdateSigningPublicKey);
}

void setUpdateSigningPublicKeyForTests(const QString &base64)
{
    testKey() = base64;
}

} // namespace zrecord
