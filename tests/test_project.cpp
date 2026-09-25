#include <QtTest>

#include "Project.h"

using namespace zrecord;

namespace {

// A mono clip whose samples are 1,2,3,... so any misplacement shows up as a
// wrong value rather than as plausible-looking audio.
Clip makeRamp(int64_t startFrame, int64_t frameCount, float firstValue = 1.0f, int channels = 1) {
    Clip clip;
    clip.channels = channels;
    clip.startFrame = startFrame;
    std::vector<float> samples(static_cast<size_t>(frameCount) * static_cast<size_t>(channels));
    for (int64_t f = 0; f < frameCount; ++f) {
        for (int c = 0; c < channels; ++c) {
            samples[static_cast<size_t>(f) * channels + c] = firstValue + static_cast<float>(f);
        }
    }
    clip.samples = samples;
    clip.peaks.build(clip.samples, channels);
    return clip;
}

std::vector<float> toVector(std::initializer_list<float> values) {
    return std::vector<float>(values);
}

} // namespace

class TestProject : public QObject {
    Q_OBJECT

private slots:
    void splitClipAt_splitsInsideOnly();
    void splitClipAt_ignoresBoundaries();
    void removeRange_ripplesLaterClipsLeft();
    void removeRange_returnsRemovedAudio();
    void removeRange_partialClipKeepsRemainder();
    void insertRange_ripplesLaterClipsRight();
    void copyRange_readsWithoutModifying();
    void copyRange_fillsGapsWithSilence();
    void writeRange_onlyWritesWhereClipsExist();
    void silenceRange_zeroesOverlapOnly();
    void appendClip_placesAtTrackEnd();
    void readMix_appliesGain();
    void readMix_respectsMuteAndSolo();
    void peakCache_reportsBlockMinMax();
    void envelope_isUnityWhenEmpty();
    void envelope_interpolatesAndHoldsAtTheEnds();
    void envelope_replacesAPointAtTheSameFrame();
    void readMix_appliesTheEnvelope();
    void crossfadeClips_mergesAndShortensTrack();
    void crossfadeClips_holdsEqualPowerAcrossTheJoin();
    void crossfadeClips_rejectsBadInputWithoutMutating();
};

void TestProject::splitClipAt_splitsInsideOnly() {
    Track track;
    track.clips.push_back(makeRamp(0, 10));

    Project::splitClipAt(track, 4, 1);

    QCOMPARE(track.clips.size(), size_t(2));
    QCOMPARE(track.clips[0].startFrame, int64_t(0));
    QCOMPARE(track.clips[0].frameCount(), int64_t(4));
    QCOMPARE(track.clips[1].startFrame, int64_t(4));
    QCOMPARE(track.clips[1].frameCount(), int64_t(6));
    // The audio must survive the split unchanged.
    QCOMPARE(track.clips[0].samples.front(), 1.0f);
    QCOMPARE(track.clips[1].samples.front(), 5.0f);
    QCOMPARE(track.clips[1].samples.back(), 10.0f);
}

void TestProject::splitClipAt_ignoresBoundaries() {
    Track track;
    track.clips.push_back(makeRamp(0, 10));

    Project::splitClipAt(track, 0, 1);   // exactly the start
    Project::splitClipAt(track, 10, 1);  // exactly the end
    Project::splitClipAt(track, 50, 1);  // past the end

    QCOMPARE(track.clips.size(), size_t(1));
}

void TestProject::removeRange_ripplesLaterClipsLeft() {
    Track track;
    track.clips.push_back(makeRamp(0, 10));
    track.clips.push_back(makeRamp(20, 10, 100.0f));

    Project::removeRange(track, 0, 10, 1);

    // The first clip is gone and the second slides left by the removed length.
    QCOMPARE(track.clips.size(), size_t(1));
    QCOMPARE(track.clips[0].startFrame, int64_t(10));
    QCOMPARE(track.clips[0].samples.front(), 100.0f);
}

void TestProject::removeRange_returnsRemovedAudio() {
    Track track;
    track.clips.push_back(makeRamp(0, 4)); // 1,2,3,4

    std::vector<float> removed = Project::removeRange(track, 1, 3, 1);

    QCOMPARE(removed, toVector({2.0f, 3.0f}));
    QCOMPARE(track.clips.size(), size_t(2));
    QCOMPARE(track.clips[0].samples, toVector({1.0f}));
    QCOMPARE(track.clips[1].samples, toVector({4.0f}));
    QCOMPARE(track.clips[1].startFrame, int64_t(1)); // rippled left by 2
}

void TestProject::removeRange_partialClipKeepsRemainder() {
    Track track;
    track.clips.push_back(makeRamp(0, 10));

    // Remove a span that starts inside the clip and runs past its end.
    Project::removeRange(track, 6, 20, 1);

    QCOMPARE(track.clips.size(), size_t(1));
    QCOMPARE(track.clips[0].frameCount(), int64_t(6));
    QCOMPARE(track.clips[0].samples.back(), 6.0f);
    QCOMPARE(track.endFrame(), int64_t(6));
}

void TestProject::insertRange_ripplesLaterClipsRight() {
    Track track;
    track.clips.push_back(makeRamp(0, 4)); // 1,2,3,4

    Project::insertRange(track, 2, toVector({99.0f, 98.0f}), 1);

    QCOMPARE(track.endFrame(), int64_t(6));
    // Reading the whole track back should show the inserted audio in the middle.
    std::vector<float> all = Project::copyRange(track, 0, 6, 1);
    QCOMPARE(all, toVector({1.0f, 2.0f, 99.0f, 98.0f, 3.0f, 4.0f}));
}

void TestProject::copyRange_readsWithoutModifying() {
    Track track;
    track.clips.push_back(makeRamp(0, 6));
    const size_t clipCountBefore = track.clips.size();
    const std::vector<float> samplesBefore = track.clips[0].samples.toVector();

    std::vector<float> copied = Project::copyRange(track, 2, 5, 1);

    QCOMPARE(copied, toVector({3.0f, 4.0f, 5.0f}));
    QCOMPARE(track.clips.size(), clipCountBefore);
    QCOMPARE(track.clips[0].samples, samplesBefore);
}

void TestProject::copyRange_fillsGapsWithSilence() {
    Track track;
    track.clips.push_back(makeRamp(4, 2)); // frames 4..5 hold 1,2

    std::vector<float> copied = Project::copyRange(track, 2, 8, 1);

    QCOMPARE(copied, toVector({0.0f, 0.0f, 1.0f, 2.0f, 0.0f, 0.0f}));
}

void TestProject::writeRange_onlyWritesWhereClipsExist() {
    Track track;
    track.clips.push_back(makeRamp(2, 2)); // frames 2..3

    // Hand it a span wider than the clip; the frames outside have no clip to
    // land in and must be dropped rather than inventing audio.
    Project::writeRange(track, 0, 6, toVector({7.0f, 7.0f, 7.0f, 7.0f, 7.0f, 7.0f}), 1);

    QCOMPARE(track.clips.size(), size_t(1));
    QCOMPARE(track.clips[0].startFrame, int64_t(2));
    QCOMPARE(track.clips[0].samples, toVector({7.0f, 7.0f}));
    QCOMPARE(track.endFrame(), int64_t(4));
}

void TestProject::silenceRange_zeroesOverlapOnly() {
    Track track;
    track.clips.push_back(makeRamp(0, 6)); // 1..6

    Project::silenceRange(track, 2, 4, 1);

    QCOMPARE(track.clips[0].samples, toVector({1.0f, 2.0f, 0.0f, 0.0f, 5.0f, 6.0f}));
    QCOMPARE(track.endFrame(), int64_t(6)); // length unchanged
}

void TestProject::appendClip_placesAtTrackEnd() {
    Track track;
    track.clips.push_back(makeRamp(0, 3));

    Project::appendClip(track, toVector({9.0f, 9.0f}), 1);

    QCOMPARE(track.clips.size(), size_t(2));
    QCOMPARE(track.clips[1].startFrame, int64_t(3));
    QCOMPARE(track.endFrame(), int64_t(5));
}

void TestProject::readMix_appliesGain() {
    Project project;
    project.channels = 1;
    Track track;
    track.gainDb = -6.0;
    track.clips.push_back(makeRamp(0, 2, 0.5f)); // 0.5, 1.5
    project.tracks.push_back(std::move(track));

    std::vector<float> out(2, 0.0f);
    project.readMix(0, 2, out);

    // -6 dB is a little under half; the second sample also clamps at 1.0.
    QVERIFY(out[0] > 0.24f && out[0] < 0.26f);
    QVERIFY(out[1] <= 1.0f);
}

void TestProject::readMix_respectsMuteAndSolo() {
    Project project;
    project.channels = 1;

    Track loud;
    loud.clips.push_back(makeRamp(0, 1, 0.5f));
    project.tracks.push_back(std::move(loud));

    Track other;
    other.clips.push_back(makeRamp(0, 1, 0.25f));
    project.tracks.push_back(std::move(other));

    std::vector<float> out(1, 0.0f);

    project.readMix(0, 1, out);
    QCOMPARE(out[0], 0.75f); // both audible, summed

    project.tracks[1].muted = true;
    project.readMix(0, 1, out);
    QCOMPARE(out[0], 0.5f); // muted track drops out

    // A solo anywhere silences every non-soloed track, including unmuted ones.
    project.tracks[1].muted = false;
    project.tracks[1].soloed = true;
    project.readMix(0, 1, out);
    QCOMPARE(out[0], 0.25f);
}

void TestProject::peakCache_reportsBlockMinMax() {
    std::vector<float> samples(PeakCache::kBlockFrames * 2, 0.0f);
    samples[5] = 0.8f;
    samples[7] = -0.4f;
    samples[static_cast<size_t>(PeakCache::kBlockFrames) + 3] = 0.2f;

    PeakCache cache;
    cache.build(samples, 1);

    QVERIFY(cache.isBuilt());
    QCOMPARE(cache.blockCount(), int64_t(2));
    QCOMPARE(cache.blockAt(0).maxValue, 0.8f);
    QCOMPARE(cache.blockAt(0).minValue, -0.4f);
    QCOMPARE(cache.blockAt(1).maxValue, 0.2f);
    // Out-of-range blocks must read as silence rather than run off the end.
    QCOMPARE(cache.blockAt(99).maxValue, 0.0f);
}

void TestProject::envelope_isUnityWhenEmpty() {
    Track track;
    QCOMPARE(track.envelopeGainAt(0), 1.0f);
    QCOMPARE(track.envelopeGainAt(100000), 1.0f);
}

void TestProject::envelope_interpolatesAndHoldsAtTheEnds() {
    Track track;
    track.insertEnvelopePoint({100, 0.0f});
    track.insertEnvelopePoint({200, 1.0f});

    // Flat outside the outermost points rather than extrapolating to
    // nonsense gains.
    QCOMPARE(track.envelopeGainAt(0), 0.0f);
    QCOMPARE(track.envelopeGainAt(100), 0.0f);
    QCOMPARE(track.envelopeGainAt(200), 1.0f);
    QCOMPARE(track.envelopeGainAt(5000), 1.0f);
    // Linear in between.
    QVERIFY(std::fabs(track.envelopeGainAt(150) - 0.5f) < 1e-5f);
    QVERIFY(std::fabs(track.envelopeGainAt(125) - 0.25f) < 1e-5f);
}

void TestProject::envelope_replacesAPointAtTheSameFrame() {
    Track track;
    // Inserted out of order; the list must end up sorted.
    track.insertEnvelopePoint({200, 0.5f});
    track.insertEnvelopePoint({100, 0.25f});
    QCOMPARE(track.envelope.size(), size_t(2));
    QCOMPARE(track.envelope[0].frame, int64_t(100));

    // A second point on the same frame updates rather than duplicating,
    // otherwise the curve would have two values at one instant.
    int index = track.insertEnvelopePoint({100, 0.75f});
    QCOMPARE(index, 0);
    QCOMPARE(track.envelope.size(), size_t(2));
    QCOMPARE(track.envelope[0].gain, 0.75f);
}

void TestProject::readMix_appliesTheEnvelope() {
    Project project;
    project.channels = 1;
    Track track;
    Clip clip;
    clip.channels = 1;
    clip.startFrame = 0;
    clip.samples.assign(4, 1.0f);
    track.clips.push_back(std::move(clip));
    // Ramp from silence to unity across the clip.
    track.insertEnvelopePoint({0, 0.0f});
    track.insertEnvelopePoint({3, 1.0f});
    project.tracks.push_back(std::move(track));

    std::vector<float> out(4, 0.0f);
    project.readMix(0, 4, out);

    QCOMPARE(out[0], 0.0f);
    QVERIFY(std::fabs(out[3] - 1.0f) < 1e-5f);
    // Strictly increasing, i.e. sampled per frame rather than per block.
    for (size_t i = 1; i < out.size(); ++i) {
        QVERIFY2(out[i] > out[i - 1], qPrintable(QString("frame %1 did not rise").arg(i)));
    }
}

void TestProject::crossfadeClips_mergesAndShortensTrack() {
    Track track;
    track.clips.push_back(makeRamp(0, 10));            // frames 0..9
    track.clips.push_back(makeRamp(10, 10, 100.0f));   // frames 10..19, adjacent
    track.clips.push_back(makeRamp(30, 4, 500.0f));    // a later clip that must ripple

    QVERIFY(Project::crossfadeClips(track, 0, 4, 1));

    // The pair becomes one clip, 4 frames shorter than the two combined.
    QCOMPARE(track.clips.size(), size_t(2));
    QCOMPARE(track.clips[0].startFrame, int64_t(0));
    QCOMPARE(track.clips[0].frameCount(), int64_t(16));
    // Material outside the overlap is untouched on both sides.
    QCOMPARE(track.clips[0].samples.front(), 1.0f);          // A's head
    QCOMPARE(track.clips[0].samples.back(), 109.0f);         // B's tail
    // Everything after the join slides left by the crossfade length.
    QCOMPARE(track.clips[1].startFrame, int64_t(26));
    QCOMPARE(track.endFrame(), int64_t(30));
}

void TestProject::crossfadeClips_holdsEqualPowerAcrossTheJoin() {
    // Measure the two gain ramps separately: crossfading a unit signal against
    // silence leaves the outgoing ramp, and silence against a unit signal
    // leaves the incoming one.
    auto rampFor = [](float aValue, float bValue) {
        Track track;
        Clip a; a.channels = 1; a.startFrame = 0; a.samples.assign(6, aValue);
        Clip b; b.channels = 1; b.startFrame = 6; b.samples.assign(6, bValue);
        track.clips.push_back(std::move(a));
        track.clips.push_back(std::move(b));
        Project::crossfadeClips(track, 0, 6, 1);
        return track.clips[0].samples.toVector();
    };

    const std::vector<float> outgoing = rampFor(1.0f, 0.0f);
    const std::vector<float> incoming = rampFor(0.0f, 1.0f);
    QCOMPARE(outgoing.size(), size_t(6));
    QCOMPARE(incoming.size(), size_t(6));

    // Equal power means the two gains satisfy out^2 + in^2 == 1 at every step.
    // (A linear crossfade would instead satisfy out + in == 1, and lose ~3 dB
    // in the middle for uncorrelated material.)
    for (size_t i = 0; i < outgoing.size(); ++i) {
        float power = outgoing[i] * outgoing[i] + incoming[i] * incoming[i];
        QVERIFY2(std::fabs(power - 1.0f) < 1e-4f,
                 qPrintable(QString("power %1 at frame %2").arg(power).arg(i)));
    }
    // And the ramps actually run in opposite directions.
    QVERIFY(outgoing.front() > outgoing.back());
    QVERIFY(incoming.front() < incoming.back());
}

void TestProject::crossfadeClips_rejectsBadInputWithoutMutating() {
    Track track;
    track.clips.push_back(makeRamp(0, 10));
    track.clips.push_back(makeRamp(20, 10, 100.0f)); // gap: not adjacent
    const auto before = track.clips;

    QVERIFY(!Project::crossfadeClips(track, 0, 4, 1));   // gap between clips
    QVERIFY(!Project::crossfadeClips(track, 1, 4, 1));   // no clip after the last
    QVERIFY(!Project::crossfadeClips(track, 0, 0, 1));   // zero-length crossfade

    // Close the gap, then ask for more frames than a clip holds.
    track.clips[1].startFrame = 10;
    QVERIFY(!Project::crossfadeClips(track, 0, 50, 1));

    QCOMPARE(track.clips.size(), before.size());
    QCOMPARE(track.clips[0].samples, before[0].samples);
    QCOMPARE(track.clips[1].samples, before[1].samples);
}

QTEST_GUILESS_MAIN(TestProject)
#include "test_project.moc"
