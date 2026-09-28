#include <QtTest>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <future>
#include <new>

#include "PlaybackMixer.h"

using namespace zrecord;

// Counts heap allocations while `g_countAllocations` is set, to prove the
// playback render doesn't allocate. (GCC inlines the replacement delete below
// down to free() and then flags free() on memory from operator new, which is
// exactly how these replacements pair up; the warning is a false positive.)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
namespace {
std::atomic<bool> g_countAllocations{false};
std::atomic<int> g_allocations{0};
} // namespace

void* operator new(std::size_t size) {
    if (g_countAllocations.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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

// A mono track holding a decaying 440 Hz tone (so echoes and filters show).
void fillTone(Project& project, int64_t frames) {
    project.channels = 1;
    project.sampleRate = 44100.0;
    std::vector<float> tone(static_cast<size_t>(frames));
    for (int64_t i = 0; i < frames; ++i) {
        tone[size_t(i)] = 0.5f * float(std::exp(-double(i) / 8000.0) * std::sin(2 * M_PI * 440.0 * double(i) / 44100.0));
    }
    Track track;
    Clip clip;
    clip.channels = 1;
    clip.samples = SampleBuffer(tone);
    track.clips.push_back(clip);
    project.tracks.push_back(track);
}

// Plays the whole project through the mixer in `block`-frame callbacks,
// retaking the snapshot every `refreshEvery` blocks as the UI tick does.
std::vector<float> playAll(Project& project, PlaybackMixer& mixer, size_t block, int refreshEvery,
                           int64_t from = 0) {
    std::vector<float> out;
    std::vector<float> buffer(block);
    int n = 0;
    for (int64_t pos = from; pos < project.lengthFrames(); pos += int64_t(block)) {
        if (refreshEvery > 0 && n++ % refreshEvery == 0) {
            mixer.publish(PlaybackSnapshot::capture(project, mixer.currentEffects()));
        }
        mixer.render(pos, buffer.data(), block, 1);
        out.insert(out.end(), buffer.begin(), buffer.end());
    }
    out.resize(size_t(project.lengthFrames() - from));
    return out;
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
    void trackEffectsChangeTheOutputAndBypassRestoresIt();
    void effectsRunBeforeTrackGainAndEnvelope();
    void parameterChangesKeepTheRackAndItsState();
    void playbackFromTheStartMatchesExport();
    void seekStartsEffectsFromRest();
    void renderWithEffectsDoesNotAllocate();
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

void TestPlaybackMixer::trackEffectsChangeTheOutputAndBypassRestoresIt() {
    Project project;
    fillTone(project, 30000);
    PlaybackMixer mixer;
    mixer.publish(PlaybackSnapshot::capture(project));
    const std::vector<float> dry = playAll(project, mixer, 512, 0);

    project.tracks[0].effects = {Effect::make(EffectType::Echo), Effect::make(EffectType::LowPass)};
    mixer.publish(PlaybackSnapshot::capture(project));
    const std::vector<float> wet = playAll(project, mixer, 512, 0);
    QVERIFY(wet != dry);
    // The echo arrives 280 ms (12348 frames) later: audible where the dry
    // tone has decayed away.
    float echoed = 0.0f;
    for (size_t i = 12348; i < 12348 + 200; ++i) echoed = std::max(echoed, std::fabs(wet[i] - dry[i]));
    QVERIFY2(echoed > 0.1f, qPrintable(QString::number(echoed))); // about 0.25: half the tone's start
    for (size_t i = 0; i < 12348; ++i) QVERIFY(std::fabs(wet[i] - dry[i]) < 0.05f); // before it: just the filter

    for (Effect& e : project.tracks[0].effects) e.bypassed = true;
    mixer.publish(PlaybackSnapshot::capture(project, mixer.currentEffects()));
    QCOMPARE(playAll(project, mixer, 512, 0), dry); // bit-exact
    mixer.clear();
}

void TestPlaybackMixer::effectsRunBeforeTrackGainAndEnvelope() {
    // Gain effect +6 dB, then track gain -6 dB and an envelope: the effect
    // sees the raw clip, the fader and envelope come after it. A limiter at
    // -12 dB proves the order: it caps the clip before the track gain.
    Project project;
    fill(project, 1000, 0.5f);
    project.tracks[0].gainDb = -6.0;
    project.tracks[0].envelope = {{0, 1.0f}, {999, 0.5f}};
    Effect limiter = Effect::make(EffectType::Limiter);
    limiter.params[0] = -12.0;
    project.tracks[0].effects = {limiter};
    PlaybackMixer mixer;
    mixer.publish(PlaybackSnapshot::capture(project));
    const std::vector<float> out = playAll(project, mixer, 256, 0);
    const float ceiling = float(std::pow(10.0, -12.0 / 20.0));
    const float gain = float(std::pow(10.0, -6.0 / 20.0));
    QVERIFY(std::fabs(out[0] - ceiling * gain) < 1e-4f);
    QVERIFY(std::fabs(out[999] - ceiling * gain * 0.5f) < 1e-4f);
    mixer.clear();
}

void TestPlaybackMixer::parameterChangesKeepTheRackAndItsState() {
    Project project;
    fill(project, 20000, 0.25f);
    project.tracks[0].effects = {Effect::make(EffectType::Gain), Effect::make(EffectType::Echo)};
    PlaybackMixer mixer;
    mixer.publish(PlaybackSnapshot::capture(project));
    std::shared_ptr<EffectRack> rack = mixer.currentEffects();
    QVERIFY(rack != nullptr);

    std::vector<float> block(256);
    mixer.render(0, block.data(), 256, 1);
    QVERIFY(std::fabs(block[0] - 0.25f) < 1e-6f);

    // A parameter change: same rack, new value from the next block on.
    project.tracks[0].effects[0].params[0] = 6.0;
    mixer.publish(PlaybackSnapshot::capture(project, mixer.currentEffects()));
    QCOMPARE(mixer.currentEffects(), rack);
    mixer.render(256, block.data(), 256, 1);
    QVERIFY(std::fabs(block[0] - 0.25f * float(std::pow(10.0, 6.0 / 20.0))) < 1e-5f);

    // Bypass is a parameter too.
    project.tracks[0].effects[1].bypassed = true;
    mixer.publish(PlaybackSnapshot::capture(project, mixer.currentEffects()));
    QCOMPARE(mixer.currentEffects(), rack);

    // A structural change (reorder) builds a new rack; the old one goes with
    // the replaced snapshot, on this thread.
    std::weak_ptr<EffectRack> old = rack;
    rack.reset();
    std::swap(project.tracks[0].effects[0], project.tracks[0].effects[1]);
    mixer.publish(PlaybackSnapshot::capture(project, mixer.currentEffects()));
    QVERIFY(mixer.currentEffects() != old.lock());
    project.tracks[0].effects.clear();
    mixer.publish(PlaybackSnapshot::capture(project, mixer.currentEffects()));
    QVERIFY(mixer.currentEffects() == nullptr); // no effects: dry, no rack
    mixer.clear();
    QVERIFY(old.expired());
}

void TestPlaybackMixer::playbackFromTheStartMatchesExport() {
    // Effects are stateful; the snapshot is retaken every few blocks while
    // playing, and that must not interrupt them. Playback from the start
    // equals the export render exactly, whatever the callback size.
    Project project;
    fillTone(project, 40000);
    Effect gate = Effect::make(EffectType::NoiseGate);
    gate.params[0] = -30.0;
    project.tracks[0].effects = {Effect::make(EffectType::Compressor), Effect::make(EffectType::Echo), gate,
                                 Effect::make(EffectType::DeepVoice)};
    const std::vector<float> exported = project.renderMixdown();
    for (size_t block : {size_t(64), size_t(441), size_t(4096)}) {
        PlaybackMixer mixer;
        mixer.publish(PlaybackSnapshot::capture(project));
        QCOMPARE(playAll(project, mixer, block, 3), exported);
        mixer.clear();
    }
}

void TestPlaybackMixer::seekStartsEffectsFromRest() {
    Project project;
    fillTone(project, 30000);
    project.tracks[0].effects = {Effect::make(EffectType::Echo)};
    PlaybackMixer mixer;
    mixer.publish(PlaybackSnapshot::capture(project));
    std::vector<float> block(512);
    for (int64_t pos = 0; pos < 20000; pos += 512) mixer.render(pos, block.data(), 512, 1); // echo line full
    // Jump to 15000: the echo must not replay audio from around 20000.
    const std::vector<float> afterSeek = playAll(project, mixer, 512, 0, 15000);
    PlaybackMixer fresh;
    fresh.publish(PlaybackSnapshot::capture(project));
    QCOMPARE(afterSeek, playAll(project, fresh, 512, 0, 15000));
    mixer.clear();
    fresh.clear();
}

void TestPlaybackMixer::renderWithEffectsDoesNotAllocate() {
    Project project;
    fillTone(project, 30000);
    for (int i = 0; i < kEffectTypeCount; ++i) project.tracks[0].effects.push_back(Effect::make(EffectType(i)));
    PlaybackMixer mixer;
    mixer.publish(PlaybackSnapshot::capture(project));
    // Parameters changed on the UI side, published, then rendered: the render
    // (the callback's whole job) allocates nothing, even across a seek.
    project.tracks[0].effects[0].params[0] = 3.0;
    project.tracks[0].effects[6].params[0] = 1500.0; // echo delay
    mixer.publish(PlaybackSnapshot::capture(project, mixer.currentEffects()));
    std::vector<float> block(4096);
    g_allocations = 0;
    g_countAllocations = true;
    for (int64_t pos = 0; pos < 20000; pos += 1000) mixer.render(pos, block.data(), 1000, 1);
    mixer.render(5, block.data(), 4096, 1);
    g_countAllocations = false;
    QCOMPARE(g_allocations.load(), 0);
    mixer.clear();
}

QTEST_GUILESS_MAIN(TestPlaybackMixer)
#include "test_playbackmixer.moc"
