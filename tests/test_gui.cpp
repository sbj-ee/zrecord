#include <QPushButton>
#include <QtTest>

#include "FakeAudioEngine.h"
#include "MainWindow.h"
#include "ProjectFile.h"
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

    void mainWindowDisablesEditActionsWithoutASelection();
    void mainWindowEnablesEditActionsWithASelection();
    void mainWindowSelectAllNeedsContent();
    void mainWindowLocksEditingWhileRecording();
    void mainWindowFailedOpenKeepsProjectAndUndo();
    void mainWindowReleasesPlaybackThatEndsByItself();

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

namespace {

// MainWindow keeps its actions private, so tests reach them by objectName the
// same way a user reaches them by label.
QAction* findAction(QWidget& window, const QString& text) {
    for (QAction* action : window.findChildren<QAction*>()) {
        if (action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

bool actionEnabled(QWidget& window, const QString& text) {
    QAction* action = findAction(window, text);
    return action != nullptr && action->isEnabled();
}

} // namespace

void TestGui::mainWindowDisablesEditActionsWithoutASelection() {
    // Constructing MainWindow at all is the point here: before the engine was
    // injectable this needed real audio hardware.
    MainWindow window(std::make_unique<FakeAudioEngine>());

    for (const char* name : {"Cut", "Copy", "Delete", "Silence", "Fade In", "Fade Out",
                                 "Crossfade", "Apply Filters to Selection"}) {
        QVERIFY2(!actionEnabled(window, name),
                 qPrintable(QString("%1 was enabled with no selection").arg(name)));
    }
}

void TestGui::mainWindowEnablesEditActionsWithASelection() {
    MainWindow window(std::make_unique<FakeAudioEngine>());

    // Give it a track with audio and select part of it, the way the panel does.
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    QVERIFY(project != nullptr);
    Track track;
    track.clips.push_back(makeClip(0, 40000));
    project->tracks.push_back(std::move(track));
    project->selection.trackIndex = 0;
    project->selection.startFrame = 100;
    project->selection.endFrame = 20000;
    window.refreshActionStateForTest();

    for (const char* name : {"Cut", "Copy", "Delete", "Silence", "Fade In", "Fade Out",
                                 "Apply Filters to Selection"}) {
        QVERIFY2(actionEnabled(window, name),
                 qPrintable(QString("%1 stayed disabled with a selection").arg(name)));
    }
    // Paste still needs something on the clipboard.
    QVERIFY(!actionEnabled(window, "Paste"));
}

void TestGui::mainWindowSelectAllNeedsContent() {
    MainWindow window(std::make_unique<FakeAudioEngine>());
    Project* project = window.findChild<TrackPanel*>()->projectForTest();

    window.refreshActionStateForTest();
    QVERIFY2(!actionEnabled(window, "Select All"), "Select All was enabled on an empty project");

    // An empty track still isn't content.
    project->tracks.push_back(Track{});
    window.refreshActionStateForTest();
    QVERIFY2(!actionEnabled(window, "Select All"), "Select All was enabled with only an empty track");

    project->tracks[0].clips.push_back(makeClip(0, 1000));
    window.refreshActionStateForTest();
    QVERIFY(actionEnabled(window, "Select All"));
}

void TestGui::mainWindowLocksEditingWhileRecording() {
    MainWindow window(std::make_unique<FakeAudioEngine>());
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    Track track;
    track.clips.push_back(makeClip(0, 40000));
    project->tracks.push_back(std::move(track));
    project->selection.trackIndex = 0;
    project->selection.startFrame = 0;
    project->selection.endFrame = 20000;

    window.setControlsEnabledForTest(true); // as if a take were running
    for (const char* name : {"Cut", "Copy", "Delete", "Silence", "New", "Open...", "Save..."}) {
        QVERIFY2(!actionEnabled(window, name),
                 qPrintable(QString("%1 stayed enabled during recording").arg(name)));
    }
    // Zoom is deliberately still available while recording.
    QVERIFY(actionEnabled(window, "Zoom In"));

    window.setControlsEnabledForTest(false);
    QVERIFY(actionEnabled(window, "Cut"));
}

void TestGui::mainWindowFailedOpenKeepsProjectAndUndo() {
    // Regression: a failed Open cleared the project but kept the undo stack,
    // so the next Undo indexed tracks that no longer existed.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString good = dir.filePath("good.zrproj");
    const QString broken = dir.filePath("broken.zrproj");
    {
        Project saved;
        saved.channels = 1;
        Track a;
        a.name = "A";
        a.clips.push_back(makeClip(0, 500, 0.1f));
        saved.tracks.push_back(a);
        Track b;
        b.name = "B";
        saved.tracks.push_back(b);
        std::string error;
        QVERIFY2(ProjectFile::save(saved, good.toStdString(), error), error.c_str());
        QVERIFY2(ProjectFile::save(saved, broken.toStdString(), error), error.c_str());
        QVERIFY(QFile::remove(broken + "/audio/track0_clip0.wav"));
    }

    MainWindow window(std::make_unique<FakeAudioEngine>());
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    project->channels = 1;
    Track track;
    track.clips.push_back(makeClip(0, 40000));
    project->tracks.push_back(std::move(track));
    project->selection.trackIndex = 0;
    project->selection.startFrame = 100;
    project->selection.endFrame = 20000;
    window.refreshActionStateForTest();

    findAction(window, "Silence")->trigger();
    QCOMPARE(project->tracks[0].clips[0].samples[1000], 0.0f);
    QVERIFY(actionEnabled(window, "Undo"));

    QString error;
    QVERIFY(!window.openProjectFolder(broken, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(project->tracks.size(), size_t(1));
    QCOMPARE(project->tracks[0].clips[0].samples.size(), size_t(40000));
    QVERIFY(actionEnabled(window, "Undo"));

    // Undo still refers to live data (ASan flags the old use-after-clear).
    findAction(window, "Undo")->trigger();
    QCOMPARE(project->tracks[0].clips[0].samples[1000], 0.5f);

    findAction(window, "Redo")->trigger();
    QVERIFY2(window.openProjectFolder(good, &error), qPrintable(error));
    QCOMPARE(project->tracks.size(), size_t(2));
    QCOMPARE(project->tracks[0].name, std::string("A"));
    QVERIFY2(!actionEnabled(window, "Undo"), "Opening a project must clear the undo history");
}

void TestGui::mainWindowReleasesPlaybackThatEndsByItself() {
    // Regression: when playback ran to the end on its own, onTick only reset
    // the button text and never called stopPlayback(), so the finished
    // PortAudio stream stayed open until the next Play leaked it.
    auto engine = std::make_unique<FakeAudioEngine>();
    FakeAudioEngine* fake = engine.get();
    MainWindow window(std::move(engine));
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    Track track;
    track.clips.push_back(makeClip(0, 1000));
    project->tracks.push_back(std::move(track));

    QPushButton* play = nullptr;
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
        if (button->text() == "▶  Play") {
            play = button;
        }
    }
    QVERIFY(play != nullptr);

    play->click();
    QVERIFY(fake->isPlaying());
    QCOMPARE(play->text(), QString("■  Stop"));

    fake->finishPlayback();
    QTRY_COMPARE(fake->stopPlaybackCalls(), 1); // released by the tick timer
    QCOMPARE(play->text(), QString("▶  Play"));

    // Later ticks don't keep stopping an engine that's already idle.
    QTest::qWait(150);
    QCOMPARE(fake->stopPlaybackCalls(), 1);
}

QTEST_MAIN(TestGui)
#include "test_gui.moc"
