#pragma once
// Test-only minisign signer (libsodium), so tests can sign fixtures with a
// keypair generated at run time. zrecord itself only verifies.
#include <QByteArray>
#include <QString>

#include <sodium.h>

#include <cstdlib>

struct MinisignTestKey {
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    unsigned char keyId[8];

    MinisignTestKey()
    {
        if (sodium_init() < 0) {
            std::abort();
        }
        crypto_sign_keypair(pk, sk);
        randombytes_buf(keyId, sizeof keyId);
    }
    ~MinisignTestKey() { sodium_memzero(sk, sizeof sk); }

    QString publicKeyBase64() const
    {
        QByteArray b("Ed");
        b += QByteArray(reinterpret_cast<const char *>(keyId), 8);
        b += QByteArray(reinterpret_cast<const char *>(pk), crypto_sign_PUBLICKEYBYTES);
        return QString::fromLatin1(b.toBase64());
    }

    QByteArray sign(const QByteArray &message, const QByteArray &trusted = "timestamp:0 file:SHA256SUMS hashed",
                    bool prehash = true) const
    {
        unsigned char sig[crypto_sign_BYTES];
        if (prehash) {
            unsigned char h[crypto_generichash_BYTES_MAX];
            crypto_generichash(h, sizeof h, reinterpret_cast<const unsigned char *>(message.constData()),
                               std::size_t(message.size()), nullptr, 0);
            crypto_sign_detached(sig, nullptr, h, sizeof h, sk);
        } else {
            crypto_sign_detached(sig, nullptr, reinterpret_cast<const unsigned char *>(message.constData()),
                                 std::size_t(message.size()), sk);
        }
        QByteArray line(prehash ? "ED" : "Ed");
        line += QByteArray(reinterpret_cast<const char *>(keyId), 8);
        line += QByteArray(reinterpret_cast<const char *>(sig), crypto_sign_BYTES);
        const QByteArray globalMsg = QByteArray(reinterpret_cast<const char *>(sig), crypto_sign_BYTES) + trusted;
        unsigned char global[crypto_sign_BYTES];
        crypto_sign_detached(global, nullptr, reinterpret_cast<const unsigned char *>(globalMsg.constData()),
                             std::size_t(globalMsg.size()), sk);
        return "untrusted comment: signature from minisign secret key\n" + line.toBase64() + "\ntrusted comment: "
            + trusted + "\n" + QByteArray(reinterpret_cast<const char *>(global), crypto_sign_BYTES).toBase64()
            + "\n";
    }
};
