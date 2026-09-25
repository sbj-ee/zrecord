#include <QtTest>

#include <future>

#include "PlaybackMixer.h"

using namespace zrecord;

namespace {

void fill(Project& project, int64_t frames, float value = 0.25f) {
    project.channels = 1;
    project.sampleRate = 44100.0;
    Track track;
    Clip clip;
    clip.channels = 1;
    clip.samples = SampleBuffer(static_cast<size_t>(frames), value);
    clip.peaks.build(clip.samples, 1);
    track.clips.push_back(clip);
    project.tracks.push_back(track);
}

} // namespace

class TestPlaybackMixer : public QObject {
    Q_OBJECT

private slots:
    void rendersTheSameMixAsTheProject();
    void renderNeverWaitsForTheProjectMutex();
    void snapshotIsImmuneToLaterEdits();
    void reportsTheEnd();
    void replacedSnapshotsAreFreedOnTheUiThread();
};

void TestPlaybackMixer::rendersTheSameMixAsTheProject() {
    Project project;
    fill(project, 3000, 0.5f);
    project.tracks[0].gainDb = -6.0;
    project.tracks[0].envelope = {{0, 1.0f}, {2000, 0.0f}};

    PlaybackMixer mixer;
    mixer.publish(PlaybackSnapshot::capture(project));
    std::vector<float> rendered(1000);
    QVERIFY(mixer.render(500, rendered.data(), 1000, 1));

    std::vector<float> expected(1000);
    {
        std::lock_guard<std::mutex> lock(project.mutex);
        project.readMix(500, 1000, expected);
    }
    QCOMPARE(rendered, expected);
}

void TestPlaybackMixer::renderNeverWaitsForTheProjectMutex() {
    // Regression: the playback callback took project.mutex with a blocking
    // lock, so a slow repaint (392 ms for a spectrogram) stalled the audio.
    Project project;
    fill(project, 44100);
    PlaybackMixer mixer;
    mixer.publish(PlaybackSnapshot::capture(project));

    std::lock_guard<std::mutex> uiHoldsIt(project.mutex);
    auto rendered = std::async(std::launch::async, [&mixer] {
        std::vector<float> out(512);
        return mixer.render(0, out.data(), 512, 1);
    });
    QVERIFY2(rendered.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
             "render() blocked while the project mutex was held");
    QVERIFY(rendered.get());
}

void TestPlaybackMixer::snapshotIsImmuneToLaterEdits() {
    Project project;
    fill(project, 2000, 0.5f);
    PlaybackMixer mixer;
    mixer.publish(PlaybackSnapshot::capture(project));

    Project::silenceRange(project.tracks[0], 0, 2000, 1); // UI edits the project
    std::vector<float> out(100);
    mixer.render(0, out.data(), 100, 1);
    QCOMPARE(out[0], 0.5f); // still the audio that was published

    mixer.publish(PlaybackSnapshot::capture(project)); // the next tick's refresh
    mixer.render(0, out.data(), 100, 1);
    QCOMPARE(out[0], 0.0f);
}

void TestPlaybackMixer::reportsTheEnd() {
    Project project;
    fill(project, 1000);
    PlaybackMixer mixer;
    std::vector<float> out(512, 1.0f);
    QVERIFY(!mixer.render(0, out.data(), 512, 1)); // nothing published yet
    QCOMPARE(out[0], 0.0f);

    mixer.publish(PlaybackSnapshot::capture(project));
    QVERIFY(mixer.render(0, out.data(), 512, 1));
    QVERIFY(!mixer.render(512, out.data(), 512, 1)); // reaches frame 1024 >= 1000
    std::vector<float> stereo(1024, 1.0f);
    QVERIFY(!mixer.render(0, stereo.data(), 512, 2)); // channel mismatch: silence
    QCOMPARE(stereo[1023], 0.0f);
}

void TestPlaybackMixer::replacedSnapshotsAreFreedOnTheUiThread() {
    Project project;
    fill(project, 1000);
    PlaybackMixer mixer;
    auto first = PlaybackSnapshot::capture(project);
    std::weak_ptr<const PlaybackSnapshot> watch = first;
    mixer.publish(std::move(first));
    mixer.publish(PlaybackSnapshot::capture(project));
    // render() wasn't running, so the replaced snapshot is freed at once.
    QVERIFY(watch.expired());
    QCOMPARE(mixer.retiredCount(), size_t(0));
    mixer.clear();
}

QTEST_GUILESS_MAIN(TestPlaybackMixer)
#include "test_playbackmixer.moc"
