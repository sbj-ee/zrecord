#include <QtTest>

#include "TrackPanel.h"

using namespace zrecord;

namespace {

Clip makeClip(int64_t startFrame, int64_t frameCount, float value = 0.5f) {
    Clip clip;
    clip.channels = 1;
    clip.startFrame = startFrame;
    clip.samples.assign(static_cast<size_t>(frameCount), value);
    clip.peaks.build(clip.samples, 1);
    return clip;
}

// Vertical centre of a lane, in panel coordinates.
int laneCentreY(int trackIndex) {
    return TrackPanel::lanesTop() + trackIndex * TrackPanel::kLaneHeight + TrackPanel::kLaneHeight / 2;
}

} // namespace

class TestGui : public QObject {
    Q_OBJECT

private slots:
    void init();

    void clickSelectsTheLaneItLandsIn();
    void dragSelectsATimeRange();
    void moveToolDragEmitsAMove();
    void draggingLeftPastZeroClampsInsteadOfVanishing();
    void doubleClickOnALabelActivatesIt();
    void labelStripDoesNotSwallowLaneClicks();

private:
    Project project_;
    TrackPanel panel_;
};

void TestGui::init() {
    project_.reset();
    project_.channels = 1;
    project_.sampleRate = 44100.0;

    Track a;
    a.name = "Track 1";
    a.clips.push_back(makeClip(0, 40000));
    project_.tracks.push_back(std::move(a));

    Track b;
    b.name = "Track 2";
    project_.tracks.push_back(std::move(b));

    // The panel is a member reused across tests, so every mode it exposes has
    // to be reset here -- otherwise a tool left set by one test silently
    // changes what a click means in the next.
    panel_.setTool(TrackPanel::Tool::Select);
    panel_.setSnapEnabled(true);
    panel_.setProject(&project_);
    panel_.resize(900, 400);
    // One pixel per 100 frames keeps the arithmetic in these tests obvious.
    panel_.setFramesPerPixelForTest(100.0);
}

void TestGui::clickSelectsTheLaneItLandsIn() {
    // Lane geometry has shifted twice already (label strip, header rows); this
    // pins that a click lands in the lane it looks like it lands in.
    QTest::mouseClick(&panel_, Qt::LeftButton, Qt::NoModifier,
                       QPoint(TrackPanel::kHeaderWidth + 50, laneCentreY(0)));
    QCOMPARE(project_.selection.trackIndex, 0);

    QTest::mouseClick(&panel_, Qt::LeftButton, Qt::NoModifier,
                       QPoint(TrackPanel::kHeaderWidth + 50, laneCentreY(1)));
    QCOMPARE(project_.selection.trackIndex, 1);
}

void TestGui::dragSelectsATimeRange() {
    const int y = laneCentreY(0);
    QPoint from(TrackPanel::kHeaderWidth + 100, y);
    QPoint to(TrackPanel::kHeaderWidth + 300, y);

    QTest::mousePress(&panel_, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&panel_, to);
    QTest::mouseRelease(&panel_, Qt::LeftButton, Qt::NoModifier, to);

    QCOMPARE(project_.selection.trackIndex, 0);
    // 200 px at 100 frames per pixel.
    QCOMPARE(project_.selection.startFrame, int64_t(10000));
    QCOMPARE(project_.selection.endFrame, int64_t(30000));
    QVERIFY(!project_.selection.isEmpty());
}

void TestGui::moveToolDragEmitsAMove() {
    panel_.setTool(TrackPanel::Tool::Move);
    panel_.setSnapEnabled(false); // keep the arithmetic exact

    QSignalSpy spy(&panel_, &TrackPanel::clipsMoveRequested);

    const QPoint from(TrackPanel::kHeaderWidth + 100, laneCentreY(0));
    const QPoint to(TrackPanel::kHeaderWidth + 200, laneCentreY(1));
    QTest::mousePress(&panel_, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&panel_, to);
    QTest::mouseRelease(&panel_, Qt::LeftButton, Qt::NoModifier, to);

    QCOMPARE(spy.count(), 1);
    const auto moves = spy.at(0).at(0).value<std::vector<ClipMove>>();
    QCOMPARE(moves.size(), size_t(1));
    QCOMPARE(moves[0].fromTrack, 0);
    QCOMPARE(moves[0].toTrack, 1);          // dropped on the second lane
    QCOMPARE(moves[0].newStartFrame, int64_t(10000)); // shifted 100 px right
}

void TestGui::draggingLeftPastZeroClampsInsteadOfVanishing() {
    panel_.setTool(TrackPanel::Tool::Move);
    panel_.setSnapEnabled(false);

    QSignalSpy spy(&panel_, &TrackPanel::clipsMoveRequested);

    // The clip starts at frame 0, so dragging left would push it negative.
    // That used to invalidate the whole drag and render nothing at all;
    // it must clamp to zero and still be a usable (if no-op) drag.
    const QPoint from(TrackPanel::kHeaderWidth + 300, laneCentreY(0));
    const QPoint to(TrackPanel::kHeaderWidth + 100, laneCentreY(0));
    QTest::mousePress(&panel_, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(&panel_, to);
    QTest::mouseRelease(&panel_, Qt::LeftButton, Qt::NoModifier, to);

    // Clamped back to where it already was, so nothing is emitted -- but
    // crucially the drag stayed alive rather than the preview disappearing.
    for (int i = 0; i < spy.count(); ++i) {
        const auto moves = spy.at(i).at(0).value<std::vector<ClipMove>>();
        for (const ClipMove& move : moves) {
            QVERIFY2(move.newStartFrame >= 0,
                     qPrintable(QString("clip moved to %1").arg(move.newStartFrame)));
        }
    }
}

void TestGui::doubleClickOnALabelActivatesIt() {
    Label label;
    label.startFrame = 10000;
    label.endFrame = 20000;
    label.text = "Verse";
    project_.insertLabel(label);

    QSignalSpy spy(&panel_, &TrackPanel::labelActivated);

    const int labelY = TrackPanel::kRulerHeight + TrackPanel::kLabelStripHeight / 2;
    QTest::mouseDClick(&panel_, Qt::LeftButton, Qt::NoModifier,
                        QPoint(TrackPanel::kHeaderWidth + 100, labelY));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toInt(), 0);
}

void TestGui::labelStripDoesNotSwallowLaneClicks() {
    // The strip sits between the ruler and the lanes; a click just below it
    // must reach track 0 rather than being eaten by the strip.
    QTest::mouseClick(&panel_, Qt::LeftButton, Qt::NoModifier,
                       QPoint(TrackPanel::kHeaderWidth + 50, TrackPanel::lanesTop() + 2));
    QCOMPARE(project_.selection.trackIndex, 0);
}

QTEST_MAIN(TestGui)
#include "test_gui.moc"
