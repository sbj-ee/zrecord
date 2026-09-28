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
    void cleanup();
    void naturalEndDoesNotLeakTheStream();
    void naturalEndReleasesTheProject();
    void playbackRunsWhileTheUiHoldsTheProjectMutex();
    void seekJumpsRunningPlayback();
    void playbackFeedsTheMeter();

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

void TestAudioEngine::cleanup() {
    // A failed check returns early; don't leave playback running into the next
    // test (it would skip with "Already playing").
    engine_->stopPlayback();
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

void TestAudioEngine::playbackRunsWhileTheUiHoldsTheProjectMutex() {
    // Regression: the output callback blocked on project.mutex, so anything
    // holding it on the UI thread (a spectrogram repaint took ~390 ms) stalled
    // playback. Playback now renders from a snapshot and must run to the end
    // with the mutex held throughout.
    Project project;
    fill(project, 4410); // 100 ms
    std::string error;
    if (!engine_->startPlayback(project, error)) {
        QSKIP(qPrintable(QString("no usable output device: %1").arg(QString::fromStdString(error))));
    }
    {
        std::lock_guard<std::mutex> uiHoldsIt(project.mutex);
        QVERIFY2(waitUntilIdle(*engine_, 3000), "playback stalled while the project mutex was held");
    }
    QVERIFY(engine_->playbackFrame() >= 4410);
    engine_->stopPlayback();
}

void TestAudioEngine::seekJumpsRunningPlayback() {
    // Click-to-seek during playback moves the running stream without
    // restarting it. (ALSA's null device doesn't run in real time, so this
    // pins that a seek is taken up and playback still ends cleanly past it,
    // not how quickly.)
    Project project;
    fill(project, 441000);
    std::string error;
    if (!engine_->startPlayback(project, error)) {
        QSKIP(qPrintable(QString("no usable output device: %1").arg(QString::fromStdString(error))));
    }
    engine_->seekPlayback(436590);
    // No check of playbackFrame() right here: on the null device the callback
    // runs flat out, and a block that started just before the seek can still
    // report its own position once before it takes the seek up.
    QVERIFY2(waitUntilIdle(*engine_, 3000), "playback didn't jump to the seek point");
    QVERIFY(engine_->playbackFrame() >= 441000);
    engine_->stopPlayback();
}

void TestAudioEngine::playbackFeedsTheMeter() {
    // The meter shows playback as well as recording input.
    Project project;
    fill(project, 4410); // 0.1 amplitude
    project.tracks[0].clips[0].samples.fill(100, 1, -0.8f);
    std::string error;
    if (!engine_->startPlayback(project, error)) {
        QSKIP(qPrintable(QString("no usable output device: %1").arg(QString::fromStdString(error))));
    }
    QVERIFY(waitUntilIdle(*engine_));
    engine_->stopPlayback();
    std::vector<MeterBlock> blocks;
    engine_->drainMeterBlocks(blocks);
    QVERIFY(!blocks.empty());
    MeterBlock all;
    for (const MeterBlock& b : blocks) all.merge(b);
    QCOMPARE(all.peak[0], 0.8f); // the loudest sample, even if brief
    QVERIFY(!all.clipped[0]);    // one sample at 0.8 is nowhere near
    blocks.clear();
    QCOMPARE(engine_->drainMeterBlocks(blocks), size_t(0)); // taken
}

QTEST_GUILESS_MAIN(TestAudioEngine)
#include "test_audioengine.moc"
