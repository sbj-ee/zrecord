#include <QUndoStack>
#include <QtTest>

#include "Commands.h"

using namespace zrecord;

namespace {

Clip makeRamp(int64_t startFrame, int64_t frameCount, float firstValue = 1.0f) {
    Clip clip;
    clip.channels = 1;
    clip.startFrame = startFrame;
    clip.samples.resize(static_cast<size_t>(frameCount));
    for (int64_t f = 0; f < frameCount; ++f) {
        clip.samples[static_cast<size_t>(f)] = firstValue + static_cast<float>(f);
    }
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

    void deleteSelection_undoRestoresAudio();
    void silenceSelection_undoRestoresAudio();
    void fade_undoRestoresAudio();
    void paste_undoRestoresAudio();
    void moveClip_undoRestoresPosition();
    void moveClip_acrossTracksUndoRestoresTrack();
    void addAndRemoveTrack_roundTrip();
    void undoRedo_isRepeatable();

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

QTEST_GUILESS_MAIN(TestCommands)
#include "test_commands.moc"
