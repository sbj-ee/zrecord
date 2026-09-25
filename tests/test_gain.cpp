#include <QtTest>

#include "Commands.h"
#include "Gain.h"

using namespace zrecord;

namespace {

Clip makeClip(int64_t start, std::vector<float> samples) {
    Clip clip;
    clip.channels = 1;
    clip.startFrame = start;
    clip.samples = SampleBuffer(samples);
    clip.peaks.build(clip.samples, 1);
    return clip;
}

// Project has a mutex, so it's filled in place.
void twoTracks(Project& project) {
    project.channels = 1;
    project.sampleRate = 1000.0;
    Track a;
    a.clips.push_back(makeClip(0, {0.1f, -0.25f, 0.2f, 0.1f}));
    a.clips.push_back(makeClip(10, {0.5f, -0.5f}));
    Track b;
    b.clips.push_back(makeClip(0, {0.05f, -0.4f}));
    project.tracks.push_back(a);
    project.tracks.push_back(b);
}

} // namespace

class TestGain : public QObject {
    Q_OBJECT

private slots:
    void decibelConversions();
    void plans();
    void measurePeakCoversOnlyTheTargets();
    void applyGainScalesOnlyTheTargets();
    void gainCommandIsOneUndoStepAcrossTracks();
    void gainLeavesUntouchedChunksShared();
};

void TestGain::decibelConversions() {
    QVERIFY(std::fabs(dbToLinear(-6.0206f) - 0.5f) < 1e-4f);
    QVERIFY(std::fabs(linearToDb(0.5f) + 6.0206f) < 1e-3f);
    QVERIFY(std::isinf(linearToDb(0.0f)));
}

void TestGain::plans() {
    GainPlan n = planNormalize(0.5f, -1.0f);
    QVERIFY(std::fabs(linearToDb(n.resultingPeak) + 1.0f) < 1e-4f);
    QVERIFY(!n.clips());
    QVERIFY(planNormalize(0.5f, 2.0f).clips());
    QCOMPARE(planNormalize(0.0f, -1.0f).gain, 1.0f); // silence isn't scaled

    GainPlan a = planAmplify(0.5f, 6.0206f);
    QVERIFY(std::fabs(a.resultingPeak - 1.0f) < 1e-4f);
    QVERIFY(!a.clips());
    QVERIFY(planAmplify(0.5f, 7.0f).clips());
}

void TestGain::measurePeakCoversOnlyTheTargets() {
    Project project;
    twoTracks(project);
    QCOMPARE(measurePeak(project, {GainTarget{0, 0, 4}}), 0.25f);
    QCOMPARE(measurePeak(project, {GainTarget{0, 2, 11}}), 0.5f); // gap, then half the 2nd clip
    QCOMPARE(measurePeak(project, {GainTarget{0, 2, 4}, GainTarget{1, 0, 2}}), 0.4f);
    QCOMPARE(measurePeak(project, {GainTarget{0, 4, 10}}), 0.0f); // the gap is silent
    QCOMPARE(measurePeak(project, {GainTarget{7, 0, 4}}), 0.0f);  // no such track
}

void TestGain::applyGainScalesOnlyTheTargets() {
    Project project;
    twoTracks(project);
    applyGain(project, {GainTarget{0, 1, 11}}, 2.0f);
    QCOMPARE(project.tracks[0].clips[0].samples.toVector(), (std::vector<float>{0.1f, -0.5f, 0.4f, 0.2f}));
    QCOMPARE(project.tracks[0].clips[1].samples.toVector(), (std::vector<float>{1.0f, -0.5f}));
    QCOMPARE(project.tracks[1].clips[0].samples.toVector(), (std::vector<float>{0.05f, -0.4f}));
    // The waveform overview follows the new audio.
    QCOMPARE(project.tracks[0].clips[1].peaks.blockAt(0).maxValue, 1.0f);
}

void TestGain::gainCommandIsOneUndoStepAcrossTracks() {
    Project project;
    twoTracks(project);
    QUndoStack stack;
    const std::vector<GainTarget> clips = {GainTarget{0, 10, 12}, GainTarget{1, 0, 2}};
    const float gain = planNormalize(measurePeak(project, clips), 0.0f).gain; // 0.5 -> 1.0
    stack.push(new GainCommand(project, clips, gain, "Normalize"));
    QCOMPARE(stack.count(), 1);
    QCOMPARE(stack.undoText(), QString("Normalize"));
    QCOMPARE(project.tracks[0].clips[1].samples.toVector(), (std::vector<float>{1.0f, -1.0f}));
    QCOMPARE(project.tracks[1].clips[0].samples.toVector(), (std::vector<float>{0.1f, -0.8f}));
    QCOMPARE(project.tracks[0].clips[0].samples.toVector(), (std::vector<float>{0.1f, -0.25f, 0.2f, 0.1f}));

    stack.undo();
    QCOMPARE(project.tracks[0].clips[1].samples.toVector(), (std::vector<float>{0.5f, -0.5f}));
    QCOMPARE(project.tracks[1].clips[0].samples.toVector(), (std::vector<float>{0.05f, -0.4f}));
    stack.redo();
    QCOMPARE(project.tracks[1].clips[0].samples.toVector(), (std::vector<float>{0.1f, -0.8f}));
}

void TestGain::gainLeavesUntouchedChunksShared() {
    // Amplifying a short span of a long take duplicates only the chunk it
    // touches; the undo snapshot shares everything else.
    Project project;
    project.channels = 1;
    Track track;
    Clip clip;
    clip.channels = 1;
    clip.samples = SampleBuffer(SampleBuffer::kChunkSize * 8, 0.25f);
    track.clips.push_back(clip);
    project.tracks.push_back(track);
    const SampleBuffer before = project.tracks[0].clips[0].samples;

    QUndoStack stack;
    stack.push(new GainCommand(project, {GainTarget{0, 10, 20}}, 2.0f, "Amplify"));
    const SampleBuffer& after = project.tracks[0].clips[0].samples;
    QCOMPARE(after[10], 0.5f);
    QCOMPARE(after[20], 0.25f);
    QVERIFY(!after.sharesChunkAt(before, 0));
    for (size_t chunk = 1; chunk < 8; ++chunk) {
        QVERIFY(after.sharesChunkAt(before, chunk * SampleBuffer::kChunkSize));
    }
}

QTEST_GUILESS_MAIN(TestGain)
#include "test_gain.moc"
