#include <QtTest>

#include <memory>

#include "AudioEngine.h"

using namespace zrecord;

namespace {

// Open file descriptors in this process; an unreleased PortAudio/ALSA stream
// keeps its device handles open, so a leak shows up here.
int openFdCount() {
    return static_cast<int>(QDir("/proc/self/fd").entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size());
}

void fill(Project& project, int64_t frames) {
    project.channels = 1;
    project.sampleRate = 44100.0;
    Track track;
    Clip clip;
    clip.channels = 1;
    clip.samples.assign(static_cast<size_t>(frames), 0.1f);
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));
}

bool waitUntilIdle(const AudioEngine& engine, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (engine.isPlaying()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QThread::msleep(10);
    }
    return true;
}

} // namespace

// Drives the real PortAudio engine. CI machines have no sound card, so the
// test points ALSA's default device at the "null" plugin through a private
// HOME; where even that isn't available the tests skip rather than fail.
class TestAudioEngine : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void naturalEndDoesNotLeakTheStream();
    void naturalEndReleasesTheProject();

private:
    QTemporaryDir home_;
    std::unique_ptr<AudioEngine> engine_;
};

void TestAudioEngine::initTestCase() {
    QVERIFY(home_.isValid());
    QFile asoundrc(home_.filePath(".asoundrc"));
    QVERIFY(asoundrc.open(QIODevice::WriteOnly));
    asoundrc.write("pcm.!default { type null }\n");
    asoundrc.close();
    qputenv("HOME", home_.path().toUtf8());
    engine_ = std::make_unique<AudioEngine>(); // Pa_Initialize reads the config
}

void TestAudioEngine::naturalEndDoesNotLeakTheStream() {
    // Regression: when playback reached the end by itself the stream was
    // never closed, and the next startPlayback overwrote (leaked) it -- one
    // set of device handles per play.
    Project project;
    fill(project, 2205); // 50 ms
    std::string error;
    if (!engine_->startPlayback(project, error)) {
        QSKIP(qPrintable(QString("no usable output device: %1").arg(QString::fromStdString(error))));
    }
    QVERIFY(waitUntilIdle(*engine_));
    const int baseline = openFdCount();

    for (int i = 0; i < 5; ++i) {
        QVERIFY2(engine_->startPlayback(project, error), error.c_str());
        QVERIFY(waitUntilIdle(*engine_));
    }
    QCOMPARE(openFdCount(), baseline);

    engine_->stopPlayback();
    // A natural end leaves the playhead where playback started.
    QCOMPARE(project.playheadFrame, int64_t(0));
}

void TestAudioEngine::naturalEndReleasesTheProject() {
    // The engine must not write through its project pointer once playback
    // has finished: the project may be gone by the time it's stopped.
    std::string error;
    {
        auto project = std::make_unique<Project>();
        fill(*project, 2205);
        if (!engine_->startPlayback(*project, error)) {
            QSKIP(qPrintable(QString("no usable output device: %1").arg(QString::fromStdString(error))));
        }
        QVERIFY(waitUntilIdle(*engine_));
    }
    engine_->stopPlayback(); // ASan: no write into the freed project
    QVERIFY(!engine_->isPlaying());
}

QTEST_GUILESS_MAIN(TestAudioEngine)
#include "test_audioengine.moc"
