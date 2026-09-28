#include <QUndoStack>
#include <QtTest>

#include <cmath>
#include <fstream>

#include "Commands.h"
#include "PlaybackMixer.h"

using namespace zrecord;

namespace {

Clip makeRamp(int64_t startFrame, int64_t frameCount, float firstValue = 1.0f) {
    Clip clip;
    clip.channels = 1;
    clip.startFrame = startFrame;
    std::vector<float> samples(static_cast<size_t>(frameCount));
    for (int64_t f = 0; f < frameCount; ++f) {
        samples[static_cast<size_t>(f)] = firstValue + static_cast<float>(f);
    }
    clip.samples = samples;
    clip.peaks.build(clip.samples, 1);
    return clip;
}

// Flattens a track to a plain buffer so a whole edit can be compared in one go.
std::vector<float> snapshot(const Project& project, int trackIndex, int64_t length) {
    return Project::copyRange(project.tracks[static_cast<size_t>(trackIndex)], 0, length, project.channels);
}

} // namespace

class TestCommands : public QObject {
    Q_OBJECT

private slots:
    void init();

    void smallEditsDontDuplicateTheTake();
    void setTrackEffects_undoAndRedo();
    void bakeTrackEffects_equalsPlaybackAndIsOneUndoStep();
    void bakeTrackEffects_keepsClipBoundaries();

    void deleteSelection_undoRestoresAudio();
    void silenceSelection_undoRestoresAudio();
    void fade_undoRestoresAudio();
    void paste_undoRestoresAudio();
    void moveClip_undoRestoresPosition();
    void moveClip_acrossTracksUndoRestoresTrack();
    void addAndRemoveTrack_roundTrip();
    void undoRedo_isRepeatable();

    void appendClip_leavesPlayheadAtStartOfTheNewClip();
    void undo_restoresThePlayhead();

    void moveClips_movesWholeSetAndUndoRestoresAll();
    void moveClips_acrossTracksKeepsTargetSorted();

    void addLabel_keepsLabelsSortedByStart();
    void addLabel_undoRemovesTheRightOne();
    void removeLabel_undoRestoresAtSameIndex();
    void renameLabel_undoRestoresOldText();

private:
    Project project_;
    QUndoStack stack_;
};

void TestCommands::init() {
    stack_.clear();
    project_.reset();
    project_.channels = 1;
    Track track;
    track.name = "Track 1";
    track.clips.push_back(makeRamp(0, 6)); // 1,2,3,4,5,6
    project_.tracks.push_back(std::move(track));
    project_.tracks.push_back(Track{});
}

void TestCommands::deleteSelection_undoRestoresAudio() {
    const std::vector<float> before = snapshot(project_, 0, 6);

    stack_.push(new DeleteSelectionCommand(project_, 0, 2, 4));
    QCOMPARE(snapshot(project_, 0, 4), (std::vector<float>{1.0f, 2.0f, 5.0f, 6.0f}));

    stack_.undo();
    QCOMPARE(snapshot(project_, 0, 6), before);
}

void TestCommands::silenceSelection_undoRestoresAudio() {
    const std::vector<float> before = snapshot(project_, 0, 6);

    stack_.push(new SilenceSelectionCommand(project_, 0, 1, 3));
    QCOMPARE(snapshot(project_, 0, 6), (std::vector<float>{1.0f, 0.0f, 0.0f, 4.0f, 5.0f, 6.0f}));

    stack_.undo();
    QCOMPARE(snapshot(project_, 0, 6), before);
}

void TestCommands::fade_undoRestoresAudio() {
    const std::vector<float> before = snapshot(project_, 0, 6);

    stack_.push(new FadeCommand(project_, 0, 0, 6, FadeShape::In, 1));
    const std::vector<float> faded = snapshot(project_, 0, 6);
    QCOMPARE(faded.front(), 0.0f);
    QCOMPARE(faded.back(), 6.0f); // unity at the end of the ramp

    stack_.undo();
    QCOMPARE(snapshot(project_, 0, 6), before);
}

void TestCommands::paste_undoRestoresAudio() {
    const std::vector<float> before = snapshot(project_, 0, 6);

    stack_.push(new PasteCommand(project_, 0, 2, std::vector<float>{99.0f}, 1));
    QCOMPARE(project_.tracks[0].endFrame(), int64_t(7));
    QCOMPARE(snapshot(project_, 0, 7), (std::vector<float>{1.0f, 2.0f, 99.0f, 3.0f, 4.0f, 5.0f, 6.0f}));

    stack_.undo();
    QCOMPARE(project_.tracks[0].endFrame(), int64_t(6));
    QCOMPARE(snapshot(project_, 0, 6), before);
}

void TestCommands::moveClip_undoRestoresPosition() {
    stack_.push(new MoveClipCommand(project_, 0, 0, 0, 10));
    QCOMPARE(project_.tracks[0].clips[0].startFrame, int64_t(10));

    stack_.undo();
    QCOMPARE(project_.tracks[0].clips[0].startFrame, int64_t(0));
}

void TestCommands::moveClip_acrossTracksUndoRestoresTrack() {
    stack_.push(new MoveClipCommand(project_, 0, 0, 1, 4));

    QCOMPARE(project_.tracks[0].clips.size(), size_t(0));
    QCOMPARE(project_.tracks[1].clips.size(), size_t(1));
    QCOMPARE(project_.tracks[1].clips[0].startFrame, int64_t(4));
    QCOMPARE(project_.tracks[1].clips[0].samples.front(), 1.0f);

    stack_.undo();
    QCOMPARE(project_.tracks[0].clips.size(), size_t(1));
    QCOMPARE(project_.tracks[1].clips.size(), size_t(0));
    QCOMPARE(project_.tracks[0].clips[0].startFrame, int64_t(0));
}

void TestCommands::addAndRemoveTrack_roundTrip() {
    const size_t before = project_.tracks.size();

    stack_.push(new AddTrackCommand(project_, "Track 3"));
    QCOMPARE(project_.tracks.size(), before + 1);
    QCOMPARE(project_.tracks.back().name, std::string("Track 3"));

    stack_.undo();
    QCOMPARE(project_.tracks.size(), before);

    stack_.push(new RemoveTrackCommand(project_, 0));
    QCOMPARE(project_.tracks.size(), before - 1);

    stack_.undo();
    QCOMPARE(project_.tracks.size(), before);
    // The removed track must come back with its audio, in its old slot.
    QCOMPARE(project_.tracks[0].clips.size(), size_t(1));
    QCOMPARE(project_.tracks[0].clips[0].samples.front(), 1.0f);
}

void TestCommands::undoRedo_isRepeatable() {
    const std::vector<float> before = snapshot(project_, 0, 6);

    stack_.push(new DeleteSelectionCommand(project_, 0, 2, 4));
    const std::vector<float> after = snapshot(project_, 0, 4);

    // Cycling must be stable -- a redo that re-runs the edit on already-edited
    // state would drift further each time.
    for (int i = 0; i < 3; ++i) {
        stack_.undo();
        QCOMPARE(snapshot(project_, 0, 6), before);
        stack_.redo();
        QCOMPARE(snapshot(project_, 0, 4), after);
    }
}

void TestCommands::appendClip_leavesPlayheadAtStartOfTheNewClip() {
    // Track 1 already holds frames 0..5 from init().
    project_.playheadFrame = 99;

    stack_.push(new AppendClipCommand(project_, 0, std::vector<float>{7.0f, 8.0f}, 1, "Record"));

    // The playhead lands on the take just added, not past it, so Play
    // auditions the new audio rather than appearing to do nothing.
    QCOMPARE(project_.playheadFrame, int64_t(6));
    QCOMPARE(project_.tracks[0].endFrame(), int64_t(8));

    // A second take parks at its own start, not at the first one's.
    stack_.push(new AppendClipCommand(project_, 0, std::vector<float>{9.0f}, 1, "Record"));
    QCOMPARE(project_.playheadFrame, int64_t(8));
}

void TestCommands::moveClips_movesWholeSetAndUndoRestoresAll() {
    // Two more clips after the one init() puts at 0..5.
    project_.tracks[0].clips.push_back(makeRamp(10, 4, 100.0f));
    project_.tracks[0].clips.push_back(makeRamp(20, 4, 200.0f));

    std::vector<ClipMove> moves{
        ClipMove{0, 1, 0, 15},  // 10 -> 15
        ClipMove{0, 2, 0, 25},  // 20 -> 25
    };
    stack_.push(new MoveClipsCommand(project_, moves));

    QCOMPARE(project_.tracks[0].clips.size(), size_t(3));
    QCOMPARE(project_.tracks[0].clips[0].startFrame, int64_t(0));
    QCOMPARE(project_.tracks[0].clips[1].startFrame, int64_t(15));
    QCOMPARE(project_.tracks[0].clips[2].startFrame, int64_t(25));
    // The audio must travel with each clip, not just the positions.
    QCOMPARE(project_.tracks[0].clips[1].samples.front(), 100.0f);
    QCOMPARE(project_.tracks[0].clips[2].samples.front(), 200.0f);

    stack_.undo();
    QCOMPARE(project_.tracks[0].clips[1].startFrame, int64_t(10));
    QCOMPARE(project_.tracks[0].clips[2].startFrame, int64_t(20));
    QCOMPARE(project_.tracks[0].clips[1].samples.front(), 100.0f);
}

void TestCommands::moveClips_acrossTracksKeepsTargetSorted() {
    project_.tracks[0].clips.push_back(makeRamp(10, 4, 100.0f));
    // Track 2 already holds something the arrivals must sort around.
    project_.tracks[1].clips.push_back(makeRamp(50, 4, 900.0f));

    std::vector<ClipMove> moves{
        ClipMove{0, 0, 1, 100}, // lands after the sitting clip
        ClipMove{0, 1, 1, 20},  // lands before it
    };
    stack_.push(new MoveClipsCommand(project_, moves));

    QCOMPARE(project_.tracks[0].clips.size(), size_t(0));
    QCOMPARE(project_.tracks[1].clips.size(), size_t(3));
    // Arrivals are spliced into sorted order, not appended.
    QCOMPARE(project_.tracks[1].clips[0].startFrame, int64_t(20));
    QCOMPARE(project_.tracks[1].clips[1].startFrame, int64_t(50));
    QCOMPARE(project_.tracks[1].clips[2].startFrame, int64_t(100));
    QCOMPARE(project_.tracks[1].clips[1].samples.front(), 900.0f); // the sitting clip

    stack_.undo();
    QCOMPARE(project_.tracks[0].clips.size(), size_t(2));
    QCOMPARE(project_.tracks[1].clips.size(), size_t(1));
    QCOMPARE(project_.tracks[0].clips[0].startFrame, int64_t(0));
    QCOMPARE(project_.tracks[1].clips[0].startFrame, int64_t(50));
}

void TestCommands::undo_restoresThePlayhead() {
    project_.playheadFrame = 5;

    // Delete pulls the playhead to the start of the cut.
    stack_.push(new DeleteSelectionCommand(project_, 0, 2, 4));
    QCOMPARE(project_.playheadFrame, int64_t(2));

    stack_.undo();
    QCOMPARE(project_.playheadFrame, int64_t(5));

    stack_.redo();
    QCOMPARE(project_.playheadFrame, int64_t(2));

    // Same for an edit that pushes it forward.
    stack_.undo();
    stack_.push(new PasteCommand(project_, 0, 1, std::vector<float>{9.0f, 9.0f}, 1));
    QCOMPARE(project_.playheadFrame, int64_t(3));
    stack_.undo();
    QCOMPARE(project_.playheadFrame, int64_t(5));
}

namespace {
Label makeLabel(int64_t start, int64_t end, const char* text) {
    Label label;
    label.startFrame = start;
    label.endFrame = end;
    label.text = text;
    return label;
}
} // namespace

void TestCommands::addLabel_keepsLabelsSortedByStart() {
    // Pushed out of order; the list must end up ordered by start frame.
    stack_.push(new AddLabelCommand(project_, makeLabel(50, 50, "third")));
    stack_.push(new AddLabelCommand(project_, makeLabel(10, 20, "first")));
    stack_.push(new AddLabelCommand(project_, makeLabel(30, 30, "second")));

    QCOMPARE(project_.labels.size(), size_t(3));
    QCOMPARE(project_.labels[0].text, std::string("first"));
    QCOMPARE(project_.labels[1].text, std::string("second"));
    QCOMPARE(project_.labels[2].text, std::string("third"));
    QVERIFY(project_.labels[0].isRange());
    QVERIFY(!project_.labels[1].isRange()); // start == end is a point marker
}

void TestCommands::addLabel_undoRemovesTheRightOne() {
    stack_.push(new AddLabelCommand(project_, makeLabel(10, 10, "a")));
    stack_.push(new AddLabelCommand(project_, makeLabel(50, 50, "c")));
    // Inserting in the middle shifts "c" along; undo must still remove "b".
    stack_.push(new AddLabelCommand(project_, makeLabel(30, 30, "b")));

    stack_.undo();

    QCOMPARE(project_.labels.size(), size_t(2));
    QCOMPARE(project_.labels[0].text, std::string("a"));
    QCOMPARE(project_.labels[1].text, std::string("c"));
}

void TestCommands::removeLabel_undoRestoresAtSameIndex() {
    stack_.push(new AddLabelCommand(project_, makeLabel(10, 10, "a")));
    stack_.push(new AddLabelCommand(project_, makeLabel(30, 30, "b")));
    stack_.push(new AddLabelCommand(project_, makeLabel(50, 50, "c")));

    stack_.push(new RemoveLabelCommand(project_, 1));
    QCOMPARE(project_.labels.size(), size_t(2));
    QCOMPARE(project_.labels[1].text, std::string("c"));

    stack_.undo();
    QCOMPARE(project_.labels.size(), size_t(3));
    QCOMPARE(project_.labels[1].text, std::string("b"));
    QCOMPARE(project_.labels[1].startFrame, int64_t(30));
}

void TestCommands::renameLabel_undoRestoresOldText() {
    stack_.push(new AddLabelCommand(project_, makeLabel(10, 10, "before")));

    stack_.push(new RenameLabelCommand(project_, 0, "after"));
    QCOMPARE(project_.labels[0].text, std::string("after"));

    stack_.undo();
    QCOMPARE(project_.labels[0].text, std::string("before"));

    stack_.redo();
    QCOMPARE(project_.labels[0].text, std::string("after"));
}

namespace {
long residentKb() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            long kb = 0;
            status >> kb;
            return kb;
        }
    }
    return -1;
}
} // namespace

void TestCommands::smallEditsDontDuplicateTheTake() {
    // Regression: every edit snapshotted the whole track twice (before and
    // after), deep-copying all of its audio. A 10-minute stereo take grew by
    // ~440 MB per 0.1 s Silence. Scaled down here: a 2-minute stereo take
    // (~44 MB) used to grow ~88 MB per edit; now each edit may copy a chunk
    // or two.
    Project project;
    project.channels = 2;
    project.sampleRate = 48000.0;
    QUndoStack stack;
    stack.push(new AddTrackCommand(project, "Take"));
    {
        std::vector<float> take(size_t(48000) * 2 * 120, 0.1f);
        stack.push(new AppendClipCommand(project, 0, std::move(take), 2, "Record"));
    }
    const SampleBuffer recorded = project.tracks[0].clips[0].samples;

    const long before = residentKb();
    QVERIFY(before > 0);
    for (int i = 1; i <= 5; ++i) {
        stack.push(new SilenceSelectionCommand(project, 0, 48000 * i, 48000 * i + 4800));
    }
    const long grownKb = residentKb() - before;
    qInfo("RSS growth over 5 small edits on a 44 MB take: %ld KB", grownKb);
    QVERIFY2(grownKb < 16 * 1024, qPrintable(QString("grew %1 KB").arg(grownKb)));

    // Structurally: audio far from the edits is still the recorded chunk.
    const SampleBuffer& now = project.tracks[0].clips[0].samples;
    QVERIFY(now.sharesChunkAt(recorded, now.size() - 1));
    QCOMPARE(now[size_t(48000 * 1 + 100) * 2], 0.0f);

    stack.undo();
    stack.undo();
    stack.undo();
    stack.undo();
    stack.undo();
    QVERIFY(project.tracks[0].clips[0].samples == recorded);
}

namespace {
// Stereo project with one track: a decaying two-tone clip starting at 0.
void fillToneProject(Project& project, int64_t frames) {
    project.reset();
    project.channels = 2;
    project.sampleRate = 44100.0;
    std::vector<float> samples(static_cast<size_t>(frames * 2));
    for (int64_t f = 0; f < frames; ++f) {
        const double t = double(f) / 44100.0;
        const float env = float(std::exp(-t * 3.0));
        samples[size_t(2 * f)] = 0.6f * env * float(std::sin(2 * M_PI * 330 * t));
        samples[size_t(2 * f + 1)] = 0.5f * env * float(std::sin(2 * M_PI * 550 * t));
    }
    Track track;
    Clip clip;
    clip.channels = 2;
    clip.samples = samples;
    clip.peaks.build(clip.samples, 2);
    track.clips.push_back(clip);
    project.tracks.push_back(track);
}

// Playback from the start through the real mixer, in 512-frame callbacks.
std::vector<float> playFromStart(Project& project) {
    PlaybackMixer mixer;
    mixer.publish(PlaybackSnapshot::capture(project));
    const int64_t length = project.lengthFrames();
    std::vector<float> out(size_t(length * 2));
    std::vector<float> block(1024);
    for (int64_t pos = 0; pos < length; pos += 512) {
        mixer.render(pos, block.data(), 512, 2);
        const int64_t n = std::min<int64_t>(512, length - pos);
        std::copy(block.begin(), block.begin() + n * 2, out.begin() + pos * 2);
    }
    mixer.clear();
    return out;
}
} // namespace

void TestCommands::setTrackEffects_undoAndRedo() {
    const std::vector<Effect> before = {Effect::make(EffectType::Gain)};
    std::vector<Effect> after = {Effect::make(EffectType::Echo), Effect::make(EffectType::Gain)};
    after[1].bypassed = true;
    project_.tracks[0].effects = before;
    stack_.push(new SetTrackEffectsCommand(project_, 0, before, after));
    QVERIFY(project_.tracks[0].effects == after);
    QCOMPARE(stack_.undoText(), QString("Track Effects"));
    stack_.undo();
    QVERIFY(project_.tracks[0].effects == before);
    stack_.redo();
    QVERIFY(project_.tracks[0].effects == after);
    QVERIFY(project_.tracks[1].effects.empty());
}

void TestCommands::bakeTrackEffects_equalsPlaybackAndIsOneUndoStep() {
    Project project;
    fillToneProject(project, 44100);
    Effect gate = Effect::make(EffectType::NoiseGate);
    gate.params[0] = -35.0;
    project.tracks[0].effects = {Effect::make(EffectType::Compressor), Effect::make(EffectType::Echo), gate,
                                 Effect::make(EffectType::DeepVoice), Effect::make(EffectType::LowPass)};
    const std::vector<Effect> stackBefore = project.tracks[0].effects;
    const std::vector<float> raw = Project::copyRange(project.tracks[0], 0, 44100, 2);
    const std::vector<float> heard = playFromStart(project);
    QVERIFY(heard != raw);

    QUndoStack stack;
    stack.push(new BakeTrackEffectsCommand(project, 0));
    QCOMPARE(stack.count(), 1);
    QCOMPARE(stack.undoText(), QString("Apply Track Effects"));
    QVERIFY(project.tracks[0].effects.empty());
    // The baked clip, played dry, is exactly what the stack made of it.
    QCOMPARE(Project::copyRange(project.tracks[0], 0, 44100, 2), heard);
    QCOMPARE(playFromStart(project), heard);

    stack.undo(); // raw audio and the stack, both back
    QCOMPARE(Project::copyRange(project.tracks[0], 0, 44100, 2), raw);
    QVERIFY(project.tracks[0].effects == stackBefore);
    QCOMPARE(playFromStart(project), heard);
    stack.redo();
    QVERIFY(project.tracks[0].effects.empty());
    QCOMPARE(Project::copyRange(project.tracks[0], 0, 44100, 2), heard);
}

void TestCommands::bakeTrackEffects_keepsClipBoundaries() {
    // Two clips with a gap: each keeps its place and length, and gets the
    // stack's output for its span (rendered continuously from the start).
    Project project;
    project.channels = 1;
    Track track;
    track.clips.push_back(makeRamp(100, 50, 0.01f));
    track.clips.push_back(makeRamp(400, 50, 0.02f));
    Effect gain = Effect::make(EffectType::Gain);
    gain.params[0] = 20.0 * std::log10(2.0); // x2
    track.effects = {gain};
    project.tracks.push_back(track);
    QUndoStack stack;
    stack.push(new BakeTrackEffectsCommand(project, 0));
    const Track& baked = project.tracks[0];
    QCOMPARE(baked.clips.size(), size_t(2));
    QCOMPARE(baked.clips[0].startFrame, int64_t(100));
    QCOMPARE(baked.clips[1].startFrame, int64_t(400));
    QCOMPARE(baked.clips[0].frameCount(), int64_t(50));
    QVERIFY(std::fabs(baked.clips[0].samples[0] - 0.02f) < 1e-6f);
    QVERIFY(std::fabs(baked.clips[1].samples[49] - 2.0f * (0.02f + 49.0f)) < 1e-3f); // not clamped
}

QTEST_GUILESS_MAIN(TestCommands)
#include "test_commands.moc"
