#include <QtTest>

#include "Snap.h"

using namespace zrecord;

class TestSnap : public QObject {
    Q_OBJECT

private slots:
    void snapsLeadingEdgeToNearestTarget();
    void snapsTrailingEdgeWhenItIsCloser();
    void leavesPositionAloneBeyondThreshold();
    void neverSnapsBeforeZero();
    void collectsZeroPlayheadAndClipEdges();
    void excludesTheDraggedClipsOwnEdges();
};

void TestSnap::snapsLeadingEdgeToNearestTarget() {
    const std::vector<int64_t> targets{0, 1000, 5000};

    SnapResult result = snapToTargets(1005, 100, targets, 50);

    QVERIFY(result.snapped);
    QCOMPARE(result.startFrame, int64_t(1000));
    QCOMPARE(result.targetFrame, int64_t(1000));
}

void TestSnap::snapsTrailingEdgeWhenItIsCloser() {
    const std::vector<int64_t> targets{5000};

    // A 100-frame clip starting at 4890 ends at 4990 -- 10 frames from the
    // target, while its start is 110 away. The end edge should win, placing
    // the clip so it butts up against the target.
    SnapResult result = snapToTargets(4890, 100, targets, 50);

    QVERIFY(result.snapped);
    QCOMPARE(result.startFrame, int64_t(4900));
    QCOMPARE(result.targetFrame, int64_t(5000));
}

void TestSnap::leavesPositionAloneBeyondThreshold() {
    const std::vector<int64_t> targets{0, 5000};

    SnapResult result = snapToTargets(2000, 100, targets, 50);

    QVERIFY(!result.snapped);
    QCOMPARE(result.startFrame, int64_t(2000)); // untouched
}

void TestSnap::neverSnapsBeforeZero() {
    // Snapping this clip's end to zero would place its start at -100.
    const std::vector<int64_t> targets{0};

    SnapResult result = snapToTargets(20, 100, targets, 50);

    // The start edge is still a legal match, so it snaps to zero rather than
    // to a negative position.
    QVERIFY(result.snapped);
    QCOMPARE(result.startFrame, int64_t(0));
    QVERIFY(result.startFrame >= 0);
}

void TestSnap::collectsZeroPlayheadAndClipEdges() {
    Project project;
    project.playheadFrame = 777;
    Track track;
    Clip clip;
    clip.channels = 1;
    clip.startFrame = 100;
    clip.samples.assign(50, 0.0f);
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));

    std::vector<int64_t> targets = collectSnapTargets(project, nullptr);

    QVERIFY(targets.size() >= 4);
    QVERIFY(std::find(targets.begin(), targets.end(), int64_t(0)) != targets.end());
    QVERIFY(std::find(targets.begin(), targets.end(), int64_t(777)) != targets.end());
    QVERIFY(std::find(targets.begin(), targets.end(), int64_t(100)) != targets.end());
    QVERIFY(std::find(targets.begin(), targets.end(), int64_t(150)) != targets.end());
}

void TestSnap::excludesTheDraggedClipsOwnEdges() {
    Project project;
    Track track;
    Clip clip;
    clip.channels = 1;
    clip.startFrame = 100;
    clip.samples.assign(50, 0.0f);
    track.clips.push_back(std::move(clip));
    project.tracks.push_back(std::move(track));

    const Clip* dragged = &project.tracks[0].clips[0];
    std::vector<int64_t> targets = collectSnapTargets(project, dragged);

    // A clip must not snap to where it already is, or it could never be moved.
    QVERIFY(std::find(targets.begin(), targets.end(), int64_t(100)) == targets.end());
    QVERIFY(std::find(targets.begin(), targets.end(), int64_t(150)) == targets.end());
}

QTEST_GUILESS_MAIN(TestSnap)
#include "test_snap.moc"
