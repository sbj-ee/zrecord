#pragma once

#include <QByteArray>
#include <QString>

#include <array>
#include <optional>

namespace zrecord {

// Verification of minisign (https://jedisct1.github.io/minisign/) detached
// signatures with libsodium's Ed25519, as used for the release SHA256SUMS
// (SHA256SUMS.minisig). Verification only: zrecord never holds a secret key.
//
// Public key: base64("Ed" | key id (8) | Ed25519 public key (32)), optionally
// after an "untrusted comment:" line (the .pub file as minisign writes it).
// Signature file, four lines:
//   untrusted comment: <anything, not authenticated>
//   base64("ED" or "Ed" | key id (8) | signature (64))
//   trusted comment: <text>
//   base64(global signature (64))
// "ED" (minisign's default) signs BLAKE2b-512(file); legacy "Ed" signs the file.
// The global signature covers signature (64) | <text>, so the trusted comment
// (minisign puts a timestamp and the file name there) can't be swapped.
struct MinisignPublicKey {
    std::array<unsigned char, 8> keyId{};
    std::array<unsigned char, 32> key{};

    static std::optional<MinisignPublicKey> parse(const QString &text, QString *error = nullptr);
    QString keyIdHex() const; // as minisign prints it (upper-case, little-endian order)
};

struct MinisignResult {
    bool ok = false;
    QString error;          // why not, for the user
    QString trustedComment; // when ok
};

MinisignResult verifyMinisign(const QByteArray &message, const QByteArray &signatureFile,
                              const MinisignPublicKey &key);

} // namespace zrecord
