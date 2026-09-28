#include <QtTest>

#include <csignal>
#include <cstdio>
#include <thread>

#include <sys/types.h>
#include <unistd.h>

#include "AudioFileWriter.h"
#include "Dropouts.h"
#include "ProjectFile.h"
#include "Recovery.h"
#include "RingBuffer.h"
#include "TakeFile.h"

using namespace zrecord;

namespace {

float patternAt(int64_t i) {
    const uint32_t h = static_cast<uint32_t>(i) * 2654435761u;
    return static_cast<float>((h >> 8) & 0xFFFF) / 65536.0f - 0.5f;
}

std::vector<float> pattern(int64_t from, int64_t count) {
    std::vector<float> v(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) v[static_cast<size_t>(i)] = patternAt(from + i);
    return v;
}

Clip makeClip(int64_t start, std::vector<float> samples, int channels = 2) {
    Clip clip;
    clip.channels = channels;
    clip.startFrame = start;
    clip.samples = SampleBuffer(samples);
    clip.peaks.build(clip.samples, channels);
    return clip;
}

// A stereo project with a bit of everything the journal must carry.
void fill(Project& project) {
    project.sampleRate = 44100.0;
    project.channels = 2;
    Track voice;
    voice.name = "Voice";
    voice.gainDb = -3.0;
    voice.muted = true;
    voice.envelope = {{0, 1.0f}, {22050, 0.5f}};
    Effect echo = Effect::make(EffectType::Echo);
    echo.bypassed = true;
    voice.effects = {Effect::make(EffectType::HighPass), echo};
    voice.clips.push_back(makeClip(0, pattern(0, 2 * 22050)));
    voice.clips.push_back(makeClip(30000, pattern(1000000, 2 * 4410)));
    Track guitar;
    guitar.name = "Guitar";
    guitar.clips.push_back(makeClip(100, pattern(5000000, 2 * 8000)));
    project.tracks = {voice, guitar};
    project.labels = {{0, 22050, "Verse"}, {30000, 30529, "Dropout 12 ms"}};
}

void compareProjects(const Project& a, const Project& b) {
    QCOMPARE(a.sampleRate, b.sampleRate);
    QCOMPARE(a.channels, b.channels);
    QCOMPARE(a.tracks.size(), b.tracks.size());
    for (size_t t = 0; t < a.tracks.size(); ++t) {
        const Track& x = a.tracks[t];
        const Track& y = b.tracks[t];
        QCOMPARE(x.name, y.name);
        QCOMPARE(x.gainDb, y.gainDb);
        QCOMPARE(x.muted, y.muted);
        QVERIFY(x.effects == y.effects);
        QCOMPARE(x.envelope.size(), y.envelope.size());
        QCOMPARE(x.clips.size(), y.clips.size());
        for (size_t c = 0; c < x.clips.size(); ++c) {
            QCOMPARE(x.clips[c].startFrame, y.clips[c].startFrame);
            QCOMPARE(x.clips[c].channels, y.clips[c].channels);
            QVERIFY(x.clips[c].samples == y.clips[c].samples);
        }
    }
    QCOMPARE(a.labels.size(), b.labels.size());
    for (size_t l = 0; l < a.labels.size(); ++l) {
        QCOMPARE(a.labels[l].text, b.labels[l].text);
        QCOMPARE(a.labels[l].startFrame, b.labels[l].startFrame);
    }
}

QStringList sessionDirs(const QString& root) {
    return QDir(root).entryList({"session-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
}

constexpr char kChildEnv[] = "ZRECORD_RECOVERY_CHILD";

// The child for the kill test: a session with a journalled one-track
// project, then a take streaming onto that track until it's killed.
int crashChild(const QString& root) {
    RecoverySession session(root);
    if (!session.begin()) return 1;
    Project project;
    project.channels = 2;
    project.sampleRate = 44100.0;
    Track track;
    track.name = "Voice";
    track.clips.push_back(makeClip(0, pattern(0, 2 * 44100)));
    project.tracks.push_back(track);
    session.scheduleJournal(project);
    if (!session.flushJournal()) return 2;

    const QString takePath = session.newTakePath();
    TakeInProgress take;
    take.path = takePath;
    take.trackIndex = 0;
    take.startFrame = 44100;
    take.channels = 2;
    if (!session.beginTake(take)) return 3;
    RingBuffer ring;
    ring.reset(44100 * 2 * 10);
    CaptureWriter capture;
    capture.reset(&ring, 2, 44100.0);
    TakeWriter writer;
    std::string error;
    if (!writer.start(&ring, takePath.toStdString(), 2, 44100, error)) return 4;
    bool said = false;
    std::vector<float> block(441 * 2);
    for (int64_t frame = 0;; frame += 441) {
        while (ring.capacity() - ring.available() < block.size() * 4) std::this_thread::yield();
        for (size_t s = 0; s < block.size(); ++s) block[s] = patternAt(7000000 + frame * 2 + static_cast<int64_t>(s));
        capture.beginCallback(441, false, 0.0);
        capture.write(block.data(), 441);
        if (!said && writer.framesOnDisk() >= 44100) {
            std::printf("READY\n");
            std::fflush(stdout);
            said = true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

} // namespace

class TestRecovery : public QObject {
    Q_OBJECT

private slots:
    void init();
    void journalRoundTripsTheProject();
    void journalWritesOnlyAudioThatChanged();
    void interruptedJournalWriteKeepsThePreviousJournal();
    void killedProcessRecoversJournalAndPartialTake();
    void liveSessionsAreNotOffered();
    void crashedSessionWithNothingUnsavedIsRemoved();
    void clearRemovesJournalAndTakesButKeepsTheSession();
    void committedTakeIsNotRestoredTwice();
    void monoPartialTakeIsUpmixed();

private:
    std::unique_ptr<QTemporaryDir> dir_;
    QString root() const { return dir_->filePath("recovery"); }
};

void TestRecovery::init() {
    dir_ = std::make_unique<QTemporaryDir>();
    QVERIFY(dir_->isValid());
}

void TestRecovery::journalRoundTripsTheProject() {
    Project original;
    fill(original);
    {
        RecoverySession session(root());
        QVERIFY(session.begin());
        session.setProjectPath(dir_->filePath("song.zrproj"));
        session.scheduleJournal(original);
        QVERIFY(session.flushJournal());
        QCOMPARE(session.journalsWritten(), 1);
        session.abandonForTesting(); // a crash: nothing cleaned up
    }
    auto found = RecoverableSession::find(root());
    QCOMPARE(found.size(), size_t(1));
    QVERIFY(found[0]->hasJournal());
    QVERIFY(!found[0]->hasPartialTake());
    QCOMPARE(found[0]->projectPath(), dir_->filePath("song.zrproj"));
    QCOMPARE(found[0]->summary(), QString("song, 2 tracks, 3 clips"));
    Project restored;
    std::string error;
    QVERIFY2(found[0]->restore(restored, error), error.c_str());
    compareProjects(restored, original);
    found[0]->discard();
    QVERIFY(sessionDirs(root()).isEmpty());
}

void TestRecovery::journalWritesOnlyAudioThatChanged() {
    Project project;
    fill(project);
    RecoverySession session(root());
    QVERIFY(session.begin());
    const QDir audio(QDir(session.dir()).filePath("audio"));
    session.scheduleJournal(project);
    QVERIFY(session.flushJournal());
    QCOMPARE(audio.entryList(QDir::Files).size(), 3);
    const QDateTime firstWrite = QFileInfo(audio.filePath(audio.entryList(QDir::Files).first())).lastModified();

    // Moving a clip changes no audio: nothing is rewritten.
    project.tracks[0].clips[1].startFrame = 40000;
    session.scheduleJournal(project);
    QVERIFY(session.flushJournal());
    QCOMPARE(audio.entryList(QDir::Files).size(), 3);
    QCOMPARE(QFileInfo(audio.filePath(audio.entryList(QDir::Files).first())).lastModified(), firstWrite);

    // An edit writes that clip's new audio, and the old file goes.
    const std::vector<float> quiet(200, 0.0f);
    project.tracks[1].clips[0].samples.write(0, quiet.data(), quiet.size());
    session.scheduleJournal(project);
    QVERIFY(session.flushJournal());
    QCOMPARE(audio.entryList(QDir::Files).size(), 3);

    // Audio already in a file (a take, a saved project) isn't copied at all.
    session.setAudioFile(project.tracks[0].clips[0].samples.contentId(), dir_->filePath("elsewhere.wav"));
    std::string error;
    QVERIFY(AudioFileWriter::writeFloatWav(dir_->filePath("elsewhere.wav").toStdString(), project.tracks[0].clips[0].samples,
                                           44100, 2, error));
    session.scheduleJournal(project);
    QVERIFY(session.flushJournal());
    QCOMPARE(audio.entryList(QDir::Files).size(), 2);
    session.abandonForTesting();
    auto found = RecoverableSession::find(root());
    QCOMPARE(found.size(), size_t(1));
    Project restored;
    QVERIFY2(found[0]->restore(restored, error), error.c_str());
    compareProjects(restored, project);
    found[0]->discard();
}

void TestRecovery::interruptedJournalWriteKeepsThePreviousJournal() {
    // The journal is written to a temporary file and renamed over the old
    // one only once complete. A write that dies part-way (simulated: the
    // temporary file gets half the new journal and is never committed)
    // leaves the previous journal exactly as it was, and that's what a
    // restore gets.
    Project first;
    fill(first);
    first.tracks.pop_back();
    RecoverySession session(root());
    QVERIFY(session.begin());
    session.scheduleJournal(first);
    QVERIFY(session.flushJournal());
    const QString journal = QDir(session.dir()).filePath("journal.json");
    QFile before(journal);
    QVERIFY(before.open(QIODevice::ReadOnly));
    const QByteArray committed = before.readAll();
    before.close();

    Project second;
    fill(second);
    session.setJournalFailAfterBytesForTesting(200);
    session.scheduleJournal(second);
    QVERIFY(!session.flushJournal());
    QVERIFY(session.lastJournalError().contains("interrupted"));
    QFile after(journal);
    QVERIFY(after.open(QIODevice::ReadOnly));
    QCOMPARE(after.readAll(), committed);
    after.close();
    // A crash can also leave a stray temporary file behind; it's ignored.
    QFile stray(journal + ".XyZ123");
    QVERIFY(stray.open(QIODevice::WriteOnly));
    stray.write("{\"tracks\": [ {\"name\": \"half");
    stray.close();

    session.abandonForTesting();
    auto found = RecoverableSession::find(root());
    QCOMPARE(found.size(), size_t(1));
    QCOMPARE(found[0]->trackCount(), 1);
    Project restored;
    std::string error;
    QVERIFY2(found[0]->restore(restored, error), error.c_str());
    compareProjects(restored, first);

    // The next successful write replaces it as usual.
    found[0]->discard();
}

void TestRecovery::killedProcessRecoversJournalAndPartialTake() {
    // A real crash: the child journals a project, starts streaming a take
    // onto its track, and is SIGKILLed mid-take. Its session is found (the
    // lock's process is gone) and restores the project with the partial
    // take where it was being recorded: an exact prefix of the take.
    QProcess child;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(kChildEnv, root());
    child.setProcessEnvironment(env);
    child.start(QCoreApplication::applicationFilePath(), {});
    QVERIFY(child.waitForStarted());
    // A live session isn't offered.
    QVERIFY2(child.waitForReadyRead(20000), qPrintable(child.readAllStandardError()));
    QCOMPARE(child.readLine().trimmed(), QByteArray("READY"));
    QVERIFY(RecoverableSession::find(root()).empty());
    QTest::qWait(200);
    ::kill(static_cast<pid_t>(child.processId()), SIGKILL);
    QVERIFY(child.waitForFinished(5000));

    auto found = RecoverableSession::find(root());
    QCOMPARE(found.size(), size_t(1));
    RecoverableSession& session = *found[0];
    QVERIFY(session.hasJournal());
    QVERIFY(session.hasPartialTake());
    QVERIFY(session.partialTakeFrames() >= 44100);
    QVERIFY2(session.summary().startsWith("Untitled project, 1 track, 1 clip, and a partial take of 0:"),
             qPrintable(session.summary()));
    Project restored;
    int64_t takeFrames = 0;
    std::string error;
    QVERIFY2(session.restore(restored, error, &takeFrames), error.c_str());
    QCOMPARE(takeFrames, session.partialTakeFrames());
    QCOMPARE(restored.tracks.size(), size_t(1));
    const Track& track = restored.tracks[0];
    QCOMPARE(track.clips.size(), size_t(2));
    QVERIFY(track.clips[0].samples == pattern(0, 2 * 44100));
    QCOMPARE(track.clips[1].startFrame, int64_t(44100));
    QVERIFY(track.clips[1].samples == pattern(7000000, takeFrames * 2));
    QCOMPARE(restored.labels.size(), size_t(1));
    QCOMPARE(restored.labels[0].text, std::string("Recovered take"));
    QCOMPARE(restored.labels[0].endFrame - restored.labels[0].startFrame, takeFrames);

    // Discarding removes the session and its take file.
    const QStringList takes = QDir(QDir(session.dir()).filePath("takes")).entryList(QDir::Files);
    QCOMPARE(takes.size(), 1);
    session.discard();
    QVERIFY(sessionDirs(root()).isEmpty());
}

void TestRecovery::liveSessionsAreNotOffered() {
    Project project;
    fill(project);
    RecoverySession session(root());
    QVERIFY(session.begin());
    session.scheduleJournal(project);
    QVERIFY(session.flushJournal());
    QVERIFY(RecoverableSession::find(root()).empty()); // it's still running
    session.discard(); // a clean exit leaves nothing behind
    QVERIFY(sessionDirs(root()).isEmpty());
    QVERIFY(RecoverableSession::find(root()).empty());
}

void TestRecovery::crashedSessionWithNothingUnsavedIsRemoved() {
    RecoverySession session(root());
    QVERIFY(session.begin());
    session.abandonForTesting(); // crashed before any change was made
    QCOMPARE(sessionDirs(root()).size(), 1);
    QVERIFY(RecoverableSession::find(root()).empty());
    QVERIFY(sessionDirs(root()).isEmpty());
}

void TestRecovery::clearRemovesJournalAndTakesButKeepsTheSession() {
    // After a save (or discarding unsaved work) the recovery data goes --
    // including take files this session streamed into the project folder --
    // but the session keeps running and journals the next change.
    const QString projectDir = dir_->filePath("song.zrproj");
    QVERIFY(QDir().mkpath(projectDir));
    Project project;
    fill(project);
    RecoverySession session(root());
    QVERIFY(session.begin());
    session.setProjectPath(projectDir);
    const QString take = session.newTakePath();
    QCOMPARE(QFileInfo(take).absolutePath(), QDir(projectDir).filePath("takes"));
    FloatWavAppender file;
    std::string error;
    QVERIFY(file.open(take.toStdString(), 2, 44100, error));
    file.close();
    session.scheduleJournal(project);
    QVERIFY(session.flushJournal());

    session.clear();
    QVERIFY(!QFile::exists(take));
    QVERIFY(!QDir(QDir(projectDir).filePath("takes")).exists());
    QVERIFY(!QFile::exists(QDir(session.dir()).filePath("journal.json")));
    QVERIFY(QDir(session.dir()).exists());
    QVERIFY(RecoverableSession::find(root()).empty());
    session.scheduleJournal(project);
    QVERIFY(session.flushJournal());
    QVERIFY(QFile::exists(QDir(session.dir()).filePath("journal.json")));
    session.discard();
    QVERIFY(sessionDirs(root()).isEmpty());
}

void TestRecovery::committedTakeIsNotRestoredTwice() {
    // A take stopped normally is in the project, and the journal written
    // after it says so. If the crash comes before recording.json is
    // removed, the take isn't added a second time.
    Project project;
    fill(project);
    RecoverySession session(root());
    QVERIFY(session.begin());
    const QString takePath = session.newTakePath();
    TakeInProgress take;
    take.path = takePath;
    take.trackIndex = 1;
    take.startFrame = project.tracks[1].endFrame();
    take.channels = 2;
    QVERIFY(session.beginTake(take));
    FloatWavAppender file;
    std::string error;
    QVERIFY(file.open(takePath.toStdString(), 2, 44100, error));
    const std::vector<float> audio = pattern(9000000, 2 * 1000);
    QVERIFY(file.append(audio.data(), 1000));
    file.close();
    project.tracks[1].clips.push_back(makeClip(take.startFrame, audio));
    session.markTakeCommitted(takePath);
    session.scheduleJournal(project);
    QVERIFY(session.flushJournal());
    session.abandonForTesting();

    auto found = RecoverableSession::find(root());
    QCOMPARE(found.size(), size_t(1));
    QVERIFY(!found[0]->hasPartialTake());
    Project restored;
    QVERIFY(found[0]->restore(restored, error));
    compareProjects(restored, project);
    found[0]->discard();
}

void TestRecovery::monoPartialTakeIsUpmixed() {
    // A mono device recording into a stereo project: the take file is mono
    // and comes back upmixed, as it would have been at Stop.
    Project project;
    fill(project);
    RecoverySession session(root());
    QVERIFY(session.begin());
    session.scheduleJournal(project);
    QVERIFY(session.flushJournal());
    const QString takePath = session.newTakePath();
    TakeInProgress take{takePath, 5 /* no such track */, 0, 1};
    QVERIFY(session.beginTake(take));
    FloatWavAppender file;
    std::string error;
    QVERIFY(file.open(takePath.toStdString(), 1, 44100, error));
    const std::vector<float> mono = pattern(0, 500);
    QVERIFY(file.append(mono.data(), 500));
    // (killed: no close, no final header)
    session.abandonForTesting();
    auto found = RecoverableSession::find(root());
    QCOMPARE(found.size(), size_t(1));
    Project restored;
    QVERIFY2(found[0]->restore(restored, error), error.c_str());
    QCOMPARE(restored.tracks.size(), size_t(3)); // its own track: the journal had no track 5
    QCOMPARE(restored.tracks[2].name, std::string("Recovered take"));
    const std::vector<float> stereo = restored.tracks[2].clips[0].samples.toVector();
    QCOMPARE(stereo.size(), size_t(1000));
    for (size_t i = 0; i < 500; ++i) {
        QCOMPARE(stereo[2 * i], mono[i]);
        QCOMPARE(stereo[2 * i + 1], mono[i]);
    }
    found[0]->discard();
}

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsSet(kChildEnv)) {
        QCoreApplication app(argc, argv);
        return crashChild(qEnvironmentVariable(kChildEnv));
    }
    QCoreApplication app(argc, argv);
    TestRecovery test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_recovery.moc"
