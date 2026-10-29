// Minisign signature verification (SHA256SUMS.minisig): real minisign output,
// tampering, wrong keys, malformed files.
#include "Minisign.hpp"
#include "MinisignTestSigner.hpp"
#include "Update.hpp"

#include <QFile>
#include <QTest>

#include <cstring>

using namespace zrecord;

namespace {
QByteArray data(const QString &name)
{
    QFile f(QStringLiteral(ZRECORD_TEST_DATA "/minisign/") + name);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// zterminal's release public key (key ID 32960BBA77B36F1F). Not zrecord's release key: it is here
// only because tests/data/minisign/release-key/ holds the real signature zterminal's pipeline
// published, which this key verifies.
constexpr const char kZterminalReleasePublicKey[] = "RWQfb7N3uguWMhJ7jsE/Z0hZ7pDWHQ9OEeWSBTlN/R5bxQ8HWSMxp/l9";
} // namespace

class TstMinisign : public QObject
{
    Q_OBJECT
private slots:
    void realMinisignOutput()
    {
        const auto key = MinisignPublicKey::parse(QString::fromLatin1(data(QStringLiteral("test.pub"))));
        QVERIFY(key);
        QCOMPARE(key->keyIdHex(), QStringLiteral("452E2115C846828B")); // as in the .pub comment
        const QByteArray sums = data(QStringLiteral("SHA256SUMS"));
        QVERIFY(!sums.isEmpty());
        const MinisignResult r = verifyMinisign(sums, data(QStringLiteral("SHA256SUMS.minisig")), *key);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.trustedComment, QStringLiteral("timestamp:0 file:SHA256SUMS hashed"));
        // Legacy (non-prehashed "Ed") signatures too.
        const MinisignResult l =
            verifyMinisign(data(QStringLiteral("SHA256SUMS.legacy")), data(QStringLiteral("SHA256SUMS.legacy.minisig")), *key);
        QVERIFY2(l.ok, qPrintable(l.error));
        // One changed byte in the file.
        QByteArray evil = sums;
        evil[0] = evil[0] == 'a' ? 'c' : 'a';
        QVERIFY(!verifyMinisign(evil, data(QStringLiteral("SHA256SUMS.minisig")), *key).ok);
        // The bare base64 line works as a key as well.
        QVERIFY(MinisignPublicKey::parse(QStringLiteral("RWSLgkbIFSEuRS5xAEv9bP6Ma6NWSf7wuuDwLKeT++2GUiloG7OAZjVS")));
    }

    void generatedKey()
    {
        MinisignTestKey k;
        const auto key = MinisignPublicKey::parse(k.publicKeyBase64());
        QVERIFY(key);
        const QByteArray msg = "0123  zrecord_1.2.0_amd64.deb\n";
        QVERIFY(verifyMinisign(msg, k.sign(msg), *key).ok);
        QVERIFY(verifyMinisign(msg, k.sign(msg, "legacy", false), *key).ok);
        QVERIFY(verifyMinisign(msg, k.sign(msg).replace('\n', "\r\n"), *key).ok); // CRLF tolerated
    }

    void tamperingIsRejected()
    {
        MinisignTestKey k;
        MinisignTestKey other;
        const auto key = *MinisignPublicKey::parse(k.publicKeyBase64());
        const QByteArray msg = "abc  zrecord_1.2.0_amd64.deb\n";
        const QByteArray sig = k.sign(msg);
        QList<QByteArray> lines = sig.split('\n');

        // Different file.
        QCOMPARE(verifyMinisign(msg + "x", sig, key).error, QStringLiteral("the signature does not match the file"));
        // Trusted comment edited (the global signature covers it).
        QByteArray edited = sig;
        edited.replace("file:SHA256SUMS", "file:SHA256SUMX");
        QCOMPARE(verifyMinisign(msg, edited, key).error, QStringLiteral("the signature's trusted comment was altered"));
        // Untrusted comment may change freely.
        QByteArray untrusted = sig;
        untrusted.replace("signature from minisign secret key", "anything");
        QVERIFY(verifyMinisign(msg, untrusted, key).ok);
        // Signed by another key (different key id).
        QCOMPARE(verifyMinisign(msg, other.sign(msg), key).error,
                 QStringLiteral("the signature was made with a different key"));
        // Another key that claims our key id.
        std::memcpy(other.keyId, k.keyId, 8);
        QCOMPARE(verifyMinisign(msg, other.sign(msg), key).error, QStringLiteral("the signature does not match the file"));
        // Prehash flag flipped (ED <-> Ed) breaks the signature.
        QByteArray bin = QByteArray::fromBase64(lines.at(1));
        bin[1] = 'd';
        QByteArray flipped = lines.at(0) + "\n" + bin.toBase64() + "\n" + lines.at(2) + "\n" + lines.at(3) + "\n";
        QVERIFY(!verifyMinisign(msg, flipped, key).ok);
    }

    void malformed_data()
    {
        QTest::addColumn<QByteArray>("sig");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("garbage") << QByteArray("hello\nworld\n");
        QTest::newRow("three lines") << QByteArray("untrusted comment: x\nRUQ=\ntrusted comment: y\n");
        QTest::newRow("bad base64") << QByteArray("untrusted comment: x\n!!!!\ntrusted comment: y\nAAAA\n");
        QTest::newRow("short sig") << QByteArray("untrusted comment: x\nRUQAAAA=\ntrusted comment: y\nAAAA\n");
        QTest::newRow("no trusted prefix")
            << QByteArray("untrusted comment: x\n" + QByteArray(74, 'E').toBase64() + "\ncomment: y\nAAAA\n");
    }
    void malformed()
    {
        QFETCH(QByteArray, sig);
        MinisignTestKey k;
        const auto key = *MinisignPublicKey::parse(k.publicKeyBase64());
        const MinisignResult r = verifyMinisign("x", sig, key);
        QVERIFY(!r.ok);
        QVERIFY(!r.error.isEmpty());
    }

    void compiledReleaseKeyIsValid()
    {
        // The key compiled into core/UpdateSigningKey.cpp must be a real
        // minisign Ed25519 public key, or every in-app update is refused.
        setUpdateSigningPublicKeyForTests(QString());
        const QString key = updateSigningPublicKey();
        QVERIFY2(!key.isEmpty(), "core/UpdateSigningKey.cpp has no release key");
        QString error;
        const auto parsed = MinisignPublicKey::parse(key, &error);
        QVERIFY2(parsed.has_value(), qPrintable(error));
        QCOMPARE(parsed->keyIdHex(), QStringLiteral("DBC46B23C3AF0EF3"));
        // zrecord's own key, not zterminal's.
        QCOMPARE(key, QStringLiteral("RWTzDq/DI2vE2xMeF01QWTxP2lwwlxGL9M1UxQE4EFV5kVmTF84wwyi2"));
        QVERIFY(parsed->keyIdHex() != QStringLiteral("32960BBA77B36F1F"));
    }

    void realPublishedSignatureVerifies()
    {
        // zterminal's published v1.6.0 SHA256SUMS: a real signature made by real minisign with a
        // real release key, checked against that key's public half (a constant above, not the key
        // compiled into zrecord). Proves the verifier reads real-world minisign output end to end.
        const auto key = MinisignPublicKey::parse(QString::fromLatin1(kZterminalReleasePublicKey));
        QVERIFY(key);
        const QByteArray sums = data(QStringLiteral("release-key/SHA256SUMS"));
        const QByteArray sig = data(QStringLiteral("release-key/SHA256SUMS.minisig"));
        QVERIFY(!sums.isEmpty());
        const MinisignResult r = verifyMinisign(sums, sig, *key);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.trustedComment, QStringLiteral("zterminal v1.6.0 SHA256SUMS"));
        QByteArray evil = sums;
        evil[0] = evil[0] == 'a' ? 'b' : 'a';
        QVERIFY(!verifyMinisign(evil, sig, *key).ok);
        // ... and zrecord's own release key does not accept it.
        setUpdateSigningPublicKeyForTests(QString());
        const auto own = MinisignPublicKey::parse(updateSigningPublicKey());
        QVERIFY(own);
        QVERIFY(!verifyMinisign(sums, sig, *own).ok);
    }

    void badPublicKeys()
    {
        QVERIFY(!MinisignPublicKey::parse(QString()));
        QVERIFY(!MinisignPublicKey::parse(QStringLiteral("not base64 !!")));
        QVERIFY(!MinisignPublicKey::parse(QString::fromLatin1(QByteArray(42, 'x').toBase64()))); // no "Ed"
        QVERIFY(!MinisignPublicKey::parse(QString::fromLatin1(("Ed" + QByteArray(30, 'x')).toBase64()))); // short
    }
};

QTEST_GUILESS_MAIN(TstMinisign)
#include "minisign_test.moc"
