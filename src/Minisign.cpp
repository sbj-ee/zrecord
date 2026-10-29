#include "Minisign.hpp"

#include <QList>

#include <sodium.h>

#include <cstring>

namespace zrecord {

namespace {

QByteArray decodeBase64Strict(const QByteArray &text)
{
    const auto r = QByteArray::fromBase64Encoding(text.trimmed(), QByteArray::AbortOnBase64DecodingErrors);
    return r ? r.decoded : QByteArray();
}

} // namespace

std::optional<MinisignPublicKey> MinisignPublicKey::parse(const QString &text, QString *error)
{
    auto fail = [error](const QString &why) -> std::optional<MinisignPublicKey> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    QByteArray b64;
    for (const QByteArray &raw : text.toUtf8().split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith("untrusted comment:")) {
            continue;
        }
        b64 = line;
        break;
    }
    const QByteArray bin = decodeBase64Strict(b64);
    if (bin.size() != 42 || bin.at(0) != 'E' || bin.at(1) != 'd') {
        return fail(QStringLiteral("not a minisign Ed25519 public key"));
    }
    MinisignPublicKey k;
    std::memcpy(k.keyId.data(), bin.constData() + 2, 8);
    std::memcpy(k.key.data(), bin.constData() + 10, 32);
    return k;
}

QString MinisignPublicKey::keyIdHex() const
{
    QString s;
    for (int i = 7; i >= 0; --i) {
        s += QStringLiteral("%1").arg(keyId[std::size_t(i)], 2, 16, QLatin1Char('0')).toUpper();
    }
    return s;
}

MinisignResult verifyMinisign(const QByteArray &message, const QByteArray &signatureFile, const MinisignPublicKey &key)
{
    MinisignResult r;
    if (sodium_init() < 0) {
        r.error = QStringLiteral("libsodium could not be initialised");
        return r;
    }
    QList<QByteArray> lines = signatureFile.split('\n');
    for (QByteArray &l : lines) {
        if (l.endsWith('\r')) {
            l.chop(1);
        }
    }
    while (!lines.isEmpty() && lines.back().isEmpty()) {
        lines.removeLast();
    }
    static const QByteArray kTrusted("trusted comment: ");
    if (lines.size() != 4 || !lines.at(0).startsWith("untrusted comment:") || !lines.at(2).startsWith(kTrusted)) {
        r.error = QStringLiteral("the signature file is not in minisign format");
        return r;
    }
    const QByteArray sig = decodeBase64Strict(lines.at(1));
    if (sig.size() != 74 || sig.at(0) != 'E' || (sig.at(1) != 'D' && sig.at(1) != 'd')) {
        r.error = QStringLiteral("the signature file is not in minisign format");
        return r;
    }
    if (std::memcmp(sig.constData() + 2, key.keyId.data(), 8) != 0) {
        r.error = QStringLiteral("the signature was made with a different key");
        return r;
    }
    const auto *sigBytes = reinterpret_cast<const unsigned char *>(sig.constData() + 10);
    int bad;
    if (sig.at(1) == 'D') {
        unsigned char hash[crypto_generichash_BYTES_MAX];
        crypto_generichash(hash, sizeof hash, reinterpret_cast<const unsigned char *>(message.constData()),
                           std::size_t(message.size()), nullptr, 0);
        bad = crypto_sign_verify_detached(sigBytes, hash, sizeof hash, key.key.data());
    } else {
        bad = crypto_sign_verify_detached(sigBytes, reinterpret_cast<const unsigned char *>(message.constData()),
                                          std::size_t(message.size()), key.key.data());
    }
    if (bad != 0) {
        r.error = QStringLiteral("the signature does not match the file");
        return r;
    }
    const QByteArray trusted = lines.at(2).mid(kTrusted.size());
    const QByteArray global = decodeBase64Strict(lines.at(3));
    if (global.size() != crypto_sign_BYTES) {
        r.error = QStringLiteral("the signature file is not in minisign format");
        return r;
    }
    const QByteArray signedPart = QByteArray(reinterpret_cast<const char *>(sigBytes), 64) + trusted;
    if (crypto_sign_verify_detached(reinterpret_cast<const unsigned char *>(global.constData()),
                                    reinterpret_cast<const unsigned char *>(signedPart.constData()),
                                    std::size_t(signedPart.size()), key.key.data())
        != 0) {
        r.error = QStringLiteral("the signature's trusted comment was altered");
        return r;
    }
    r.ok = true;
    r.trustedComment = QString::fromUtf8(trusted);
    return r;
}

} // namespace zrecord
