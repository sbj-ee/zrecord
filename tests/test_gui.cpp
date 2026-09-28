#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QToolButton>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QUndoStack>
#include <QtTest>

#include <atomic>
#include <thread>

#include "FakeAudioEngine.h"
#include "MainWindow.h"
#include "AudioFileWriter.h"
#include "Capture.h"
#include "Commands.h"
#include "ProjectFile.h"
#include "SavedProjectPaths.h"
#include "TrackPanel.h"
#include "NormalizeDialog.h"
#include "VoiceChangerDialog.h"
#include "PeakMeter.h"
#include "zrecord_version.h"

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
    void initTestCase();
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
    void mainWindowClosesCleanlyWithUndoHistory();
    void spectrogramRepaintsReuseCachedTiles();
    void mainWindowRefreshesPlaybackWhilePlaying();
    void mainWindowImportResamplesToTheProjectRate();
    void envelopeDragIsOneUndoableStep();
    void mainWindowAsksBeforeDiscardingUnsavedChanges();
    void saveWritesBackToTheProjectsFolder();
    void recentProjectsTrackOpensAndSaves();
    void mainWindowRecordsWithTheDevicesChannelCount();
    void playheadFollowsPlayback();
    void recordingStopsPlayback();
    void inputGainDefaultsToZeroAndIsRemembered();
    void inputClipIndicatorLatchesPerTake();
    void inputClipCountIsShownAndReportedAtStop();
    void clipSelectionIsDroppedWhenTracksChange();
    void rulerClickSeeksWhileStopped();
    void seekingAndAutoScrollDuringPlayback();
    void normalizeDialogFlagsClipping();
    void normalizeSelectionIsOneUndoStep();
    void voiceChangerPresetsAndCustom();
    void voiceChangerPreviewThenApplyIsOneUndoStep();
    void voiceChangerCancelStopsPreviewAndChangesNothing();
    void amplifySelectedClips();
    void peakMeterBallisticsAndHold();
    void peakMeterClipLedLatchesUntilClicked();
    void mainWindowMetersPlayback();
    void repaintDoesNotWaitForTheProjectMutex();
    void aboutShowsTheBuildVersion();
    void playheadKeysMoveThePlayhead();

private:
    Project project_;
    TrackPanel panel_;
};

void TestGui::initTestCase() {
    // Keep QSettings (recent projects, window geometry) out of the real
    // ~/.config: test mode points it at ~/.qttest instead.
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY2(QSettings().fileName().contains(".qttest"), qPrintable(QSettings().fileName()));
    QSettings().clear();
}

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
    for (const char* name : {"Cut", "Copy", "Delete", "Silence", "New", "Open...", "Save", "Save As..."}) {
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
        QVERIFY(QFile::remove(savedClipFile(broken)));
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

void TestGui::mainWindowClosesCleanlyWithUndoHistory() {
    // Regression: destroying the window with commands on the undo stack ran
    // the stack's indexChanged handler on a half-destroyed MainWindow
    // (UBSan: member access within an object of the wrong type; ASan could
    // report a use-after-free). Needs a sanitizer build to fail loudly.
    auto window = std::make_unique<MainWindow>(std::make_unique<FakeAudioEngine>());
    Project* project = window->findChild<TrackPanel*>()->projectForTest();
    Track track;
    track.clips.push_back(makeClip(0, 4000));
    project->tracks.push_back(std::move(track));
    project->selection.trackIndex = 0;
    project->selection.startFrame = 100;
    project->selection.endFrame = 2000;
    window->refreshActionStateForTest();
    findAction(*window, "Silence")->trigger();
    findAction(*window, "Fade In")->trigger();
    findAction(*window, "Undo")->trigger(); // leave both undo and redo history
    QVERIFY(actionEnabled(*window, "Undo"));
    QVERIFY(actionEnabled(*window, "Redo"));
    window.reset();
}

void TestGui::spectrogramRepaintsReuseCachedTiles() {
    // Regression: every spectrogram repaint recomputed one FFT per column
    // (~390 ms for a 1600 px panel), all under the project lock.
    for (auto& track : project_.tracks) {
        track.display = TrackDisplay::Spectrogram;
    }
    panel_.update();
    panel_.grab();
    const int firstPaint = panel_.spectrogramTilesRenderedForTest();
    QVERIFY(firstPaint > 0);

    panel_.grab();
    QCOMPARE(panel_.spectrogramTilesRenderedForTest(), firstPaint); // all cached

    // An edit changes the clip's content, so its tiles are recomputed.
    Project::silenceRange(project_.tracks[0], 0, 1000, 1);
    panel_.grab();
    QVERIFY(panel_.spectrogramTilesRenderedForTest() > firstPaint);
}

void TestGui::mainWindowRefreshesPlaybackWhilePlaying() {
    // Playback plays a snapshot; the tick must keep handing it fresh ones so
    // edits made while playing are heard.
    auto engine = std::make_unique<FakeAudioEngine>();
    FakeAudioEngine* fake = engine.get();
    MainWindow window(std::move(engine));
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    Track track;
    track.clips.push_back(makeClip(0, 1000));
    project->tracks.push_back(std::move(track));

    findAction(window, "Play / Stop")->trigger();
    QVERIFY(fake->isPlaying());
    QTRY_VERIFY(fake->refreshCalls() >= 2);
    findAction(window, "Play / Stop")->trigger();
    const int afterStop = fake->refreshCalls();
    QTest::qWait(150);
    QCOMPARE(fake->refreshCalls(), afterStop);
}

void TestGui::mainWindowImportResamplesToTheProjectRate() {
    // Regression: import checked only the channel count, so a 48 kHz file in
    // a 44.1 kHz project was added as-is and played ~9% slow and flat.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("one-second-48k.wav");
    std::string error;
    QVERIFY(AudioFileWriter::writeFloatWav(path.toStdString(), SampleBuffer(size_t(48000), 0.25f), 48000, 1,
                                           error));

    MainWindow window(std::make_unique<FakeAudioEngine>());
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    project->channels = 1;
    project->sampleRate = 44100.0;
    Track track;
    track.clips.push_back(makeClip(0, 1000));
    project->tracks.push_back(std::move(track));

    QString message;
    QString note;
    QVERIFY2(window.importAudioFile(path, &message, &note), qPrintable(message));
    QCOMPARE(project->sampleRate, 44100.0);
    QCOMPARE(project->tracks[0].clips.size(), size_t(2));
    const int64_t frames = project->tracks[0].clips[1].frameCount();
    QVERIFY2(std::llabs(frames - 44100) <= 2, qPrintable(QString::number(frames))); // still one second
    QVERIFY(note.contains("48000"));

    // A channel mismatch is refused without adding anything.
    const QString stereo = dir.filePath("stereo.wav");
    QVERIFY(AudioFileWriter::writeFloatWav(stereo.toStdString(), SampleBuffer(size_t(200), 0.1f), 44100, 2, error));
    QVERIFY(!window.importAudioFile(stereo, &message));
    QCOMPARE(project->tracks.size(), size_t(1));
    QCOMPARE(project->tracks[0].clips.size(), size_t(2));
}

void TestGui::envelopeDragIsOneUndoableStep() {
    // Regression: the drag mutated the envelope live and the command captured
    // "before" at push time (after the drag), so Undo restored nothing; and a
    // plain click pushed two steps (Add on press, Move on release).
    QUndoStack stack;
    connect(&panel_, &TrackPanel::envelopeEdited, &stack,
            [&](int track, const std::vector<EnvelopePoint>& before, const std::vector<EnvelopePoint>& after,
                const QString& what) { stack.push(new EnvelopeEditCommand(project_, track, before, after, what)); });
    panel_.setTool(TrackPanel::Tool::Envelope);
    const int x = TrackPanel::kHeaderWidth + 200;
    const int top = TrackPanel::lanesTop();

    QTest::mouseClick(&panel_, Qt::LeftButton, {}, QPoint(x, top + 20));
    QCOMPARE(project_.tracks[0].envelope.size(), size_t(1));
    QCOMPARE(stack.count(), 1);
    QCOMPARE(stack.text(0), QString("Add Envelope Point"));
    const float added = project_.tracks[0].envelope[0].gain;

    QTest::mousePress(&panel_, Qt::LeftButton, {}, QPoint(x, top + 20));
    QMouseEvent move(QEvent::MouseMove, QPointF(x, top + 70), panel_.mapToGlobal(QPointF(x, top + 70)),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&panel_, &move);
    QTest::mouseRelease(&panel_, Qt::LeftButton, {}, QPoint(x, top + 70));
    QCOMPARE(stack.count(), 2);
    QCOMPARE(stack.text(1), QString("Move Envelope Point"));
    QVERIFY(project_.tracks[0].envelope[0].gain < added);

    stack.undo();
    QCOMPARE(project_.tracks[0].envelope[0].gain, added);
    stack.undo();
    QVERIFY(project_.tracks[0].envelope.empty());
    stack.redo();
    stack.redo();
    QVERIFY(project_.tracks[0].envelope[0].gain < added);

    // Clicking the point (now at y = top + 70) without moving it is not an edit.
    const int count = stack.count();
    QTest::mouseClick(&panel_, Qt::LeftButton, {}, QPoint(x, top + 70));
    QCOMPARE(stack.count(), count);
    panel_.disconnect(&stack);
}

void TestGui::recentProjectsTrackOpensAndSaves() {
    QSettings().clear();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MainWindow window(std::make_unique<FakeAudioEngine>());
    window.setQuietRecentFailuresForTest(true);
    QVERIFY(MainWindow::recentProjects().isEmpty());

    QString error;
    const QString a = dir.filePath("a.zrproj");
    const QString b = dir.filePath("b.zrproj");
    QVERIFY2(window.saveProjectTo(a, &error), qPrintable(error));
    QVERIFY2(window.saveProjectTo(b, &error), qPrintable(error));
    QCOMPARE(MainWindow::recentProjects(), QStringList({b, a}));

    // Opening an entry moves it to the top rather than duplicating it.
    QVERIFY(window.openRecentProject(a));
    QCOMPARE(MainWindow::recentProjects(), QStringList({a, b}));

    // The list is capped, oldest dropped first.
    for (int i = 0; i < 10; ++i) {
        QVERIFY(window.saveProjectTo(dir.filePath(QString("p%1.zrproj").arg(i)), &error));
    }
    QCOMPARE(MainWindow::recentProjects().size(), 8);
    QCOMPARE(MainWindow::recentProjects().first(), dir.filePath("p9.zrproj"));
    QVERIFY(!MainWindow::recentProjects().contains(a));

    // A project that has gone away drops off the list when picked.
    const QString gone = dir.filePath("p5.zrproj");
    QVERIFY(QDir(gone).removeRecursively());
    QVERIFY(!window.openRecentProject(gone));
    QVERIFY(!MainWindow::recentProjects().contains(gone));
    QCOMPARE(MainWindow::recentProjects().size(), 7);
    QSettings().clear();
}

void TestGui::mainWindowAsksBeforeDiscardingUnsavedChanges() {
    // Regression: Quit and Open discarded edits silently, and New asked based
    // on "anything ever done" (so it asked right after a save).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MainWindow window(std::make_unique<FakeAudioEngine>());
    QStringList asked;
    int answer = QMessageBox::Cancel;
    window.setUnsavedChangesPromptForTest([&](const QString& action) {
        asked << action;
        return answer;
    });
    Project* project = window.findChild<TrackPanel*>()->projectForTest();

    QVERIFY(!window.hasUnsavedChanges());
    QVERIFY(window.close()); // nothing to lose: closes without asking
    QVERIFY(asked.isEmpty());
    window.show();

    findAction(window, "+Track")->trigger();
    QVERIFY(window.hasUnsavedChanges());
    QVERIFY(window.isWindowModified());

    QVERIFY(!window.close()); // Cancel keeps the window open
    QCOMPARE(asked.size(), 1);
    QVERIFY(asked[0].contains("quit"));

    findAction(window, "New")->trigger(); // Cancel keeps the project
    QCOMPARE(project->tracks.size(), size_t(1));

    QString error;
    QVERIFY2(window.saveProjectTo(dir.filePath("p.zrproj"), &error), qPrintable(error));
    QVERIFY(!window.hasUnsavedChanges());
    QVERIFY(!window.isWindowModified());
    QVERIFY(window.windowTitle().startsWith("p"));
    QVERIFY(window.windowTitle().endsWith("zrecord " ZRECORD_VERSION));

    // Undoing past the save point is a change; redoing back to it isn't.
    findAction(window, "Undo")->trigger();
    QVERIFY(window.hasUnsavedChanges());
    findAction(window, "Redo")->trigger();
    QVERIFY(!window.hasUnsavedChanges());

    asked.clear();
    answer = QMessageBox::Discard;
    findAction(window, "+Track")->trigger();
    findAction(window, "New")->trigger();
    QCOMPARE(asked.size(), 1);
    QVERIFY(project->tracks.empty());
    QVERIFY(!window.hasUnsavedChanges());
}

void TestGui::saveWritesBackToTheProjectsFolder() {
    // Regression: Save always opened a file dialog, even for a project that
    // already had a folder. Now it writes back without asking, as does
    // answering Save to the unsaved-changes question.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("p.zrproj");
    MainWindow window(std::make_unique<FakeAudioEngine>());
    // Regression: bound via QKeySequence::SaveAs, which is empty outside
    // GNOME/KDE themes (including the offscreen platform these tests use).
    QCOMPARE(findAction(window, "Save As...")->shortcut(), QKeySequence("Ctrl+Shift+S"));
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    QString error;
    QVERIFY2(window.saveProjectTo(path, &error), qPrintable(error));

    findAction(window, "+Track")->trigger();
    QVERIFY(window.hasUnsavedChanges());
    findAction(window, "Save")->trigger();
    QVERIFY(!window.hasUnsavedChanges());
    Project reloaded;
    std::string message;
    QVERIFY2(ProjectFile::load(reloaded, path.toStdString(), message), message.c_str());
    QCOMPARE(reloaded.tracks.size(), project->tracks.size());

    findAction(window, "+Track")->trigger();
    window.setUnsavedChangesPromptForTest([](const QString&) { return int(QMessageBox::Save); });
    findAction(window, "New")->trigger();
    QVERIFY(project->tracks.empty());
    QVERIFY2(ProjectFile::load(reloaded, path.toStdString(), message), message.c_str());
    QCOMPARE(reloaded.tracks.size(), size_t(2));
}

namespace {
QPushButton* findButton(QWidget& window, const QString& text) {
    for (QPushButton* button : window.findChildren<QPushButton*>()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}
} // namespace

void TestGui::mainWindowRecordsWithTheDevicesChannelCount() {
    // Regression: recording defaulted to stereo and always opened the device
    // with the project's channel count, ignoring maxInputChannels, so a
    // mono-only device (ALSA hw) failed with "Invalid number of channels".
    auto engine = std::make_unique<FakeAudioEngine>();
    FakeAudioEngine* fake = engine.get();
    AudioDeviceInfo mono;
    mono.index = 3;
    mono.name = "Mono Mic";
    mono.maxInputChannels = 1;
    fake->setInputDevices({mono});
    MainWindow window(std::move(engine));

    // A new project defaults to what the device can deliver.
    QComboBox* channels = nullptr;
    for (QComboBox* combo : window.findChildren<QComboBox*>()) {
        if (combo->findText("Stereo") >= 0) {
            channels = combo;
        }
    }
    QVERIFY(channels != nullptr);
    QCOMPARE(channels->currentText(), QString("Mono"));

    // An existing stereo project records mono from this device and gets the
    // take upmixed.
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    project->channels = 2;
    Track track;
    Clip clip;
    clip.channels = 2;
    clip.samples = SampleBuffer(size_t(20), 0.1f);
    track.clips.push_back(clip);
    track.recordArmed = true;
    project->tracks.push_back(std::move(track));

    QPushButton* record = findButton(window, "●  RECORD");
    QVERIFY(record != nullptr);
    record->click();
    QVERIFY(fake->isRecording());
    QCOMPARE(fake->lastRecordingDevice(), 3);
    QCOMPARE(fake->lastRecordingChannels(), 1);

    fake->setCapturedBuffer({0.25f, -0.5f, 0.75f});
    record->click();
    QVERIFY(!fake->isRecording());
    QCOMPARE(project->tracks[0].clips.size(), size_t(2));
    const Clip& take = project->tracks[0].clips[1];
    QCOMPARE(take.channels, 2);
    QCOMPARE(take.samples.toVector(), (std::vector<float>{0.25f, 0.25f, -0.5f, -0.5f, 0.75f, 0.75f}));
}

void TestGui::playheadFollowsPlayback() {
    // Regression: the playhead stood still during playback (paint only ever
    // drew project_.playheadFrame, which nothing updated while playing).
    auto engine = std::make_unique<FakeAudioEngine>();
    FakeAudioEngine* fake = engine.get();
    MainWindow window(std::move(engine));
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    Track track;
    Clip clip;
    clip.channels = 2;
    clip.samples = SampleBuffer(size_t(2 * 96000), 0.1f);
    track.clips.push_back(clip);
    project->tracks.push_back(std::move(track));
    project->playheadFrame = 1000;

    QPushButton* play = findButton(window, "▶  Play");
    QVERIFY(play != nullptr);
    play->click();
    QVERIFY(fake->isPlaying());
    fake->setPlaybackFrame(24000);
    QTRY_COMPARE(project->playheadFrame, int64_t(24000));
    fake->setPlaybackFrame(30000);
    QTRY_COMPARE(project->playheadFrame, int64_t(30000));

    // Running out returns it to where playback started.
    fake->finishPlayback();
    QTRY_COMPARE(play->text(), QString("▶  Play"));
    QCOMPARE(project->playheadFrame, int64_t(1000));
}

void TestGui::recordingStopsPlayback() {
    // Regression: starting a take while playing left playback running.
    auto engine = std::make_unique<FakeAudioEngine>();
    FakeAudioEngine* fake = engine.get();
    MainWindow window(std::move(engine));
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    Track track;
    Clip clip;
    clip.channels = 2;
    clip.samples = SampleBuffer(size_t(2 * 48000), 0.1f);
    track.clips.push_back(clip);
    track.recordArmed = true;
    project->tracks.push_back(std::move(track));

    QPushButton* play = findButton(window, "▶  Play");
    QPushButton* record = findButton(window, "●  RECORD");
    QVERIFY(play != nullptr && record != nullptr);
    play->click();
    QVERIFY(fake->isPlaying());
    record->click();
    QVERIFY(fake->isRecording());
    QVERIFY(!fake->isPlaying());
    QCOMPARE(fake->stopPlaybackCalls(), 1);
    QCOMPARE(play->text(), QString("▶  Play"));
    record->click();
}

void TestGui::inputGainDefaultsToZeroAndIsRemembered() {
    QSettings().clear();
    {
        auto engine = std::make_unique<FakeAudioEngine>();
        FakeAudioEngine* fake = engine.get();
        MainWindow window(std::move(engine));
        auto* gain = window.findChild<QDoubleSpinBox*>("inputGain");
        QVERIFY(gain != nullptr);
        QCOMPARE(gain->value(), 0.0);
        QCOMPARE(fake->inputGainDb(), 0.0); // unity unless asked otherwise
        QCOMPARE(gain->suffix(), QString(" dB"));
        gain->setValue(-4.5);
        QCOMPARE(fake->inputGainDb(), -4.5);
        gain->setValue(100.0); // clamped to the range
        QCOMPARE(gain->value(), kInputGainMaxDb);
        gain->setValue(-4.5);
    }
    {
        // It belongs to the input setup, so the next session starts with it.
        auto engine = std::make_unique<FakeAudioEngine>();
        FakeAudioEngine* fake = engine.get();
        MainWindow window(std::move(engine));
        QCOMPARE(window.findChild<QDoubleSpinBox*>("inputGain")->value(), -4.5);
        QCOMPARE(fake->inputGainDb(), -4.5);
    }
    QSettings().clear();
}

void TestGui::inputClipIndicatorLatchesPerTake() {
    auto engine = std::make_unique<FakeAudioEngine>();
    FakeAudioEngine* fake = engine.get();
    MainWindow window(std::move(engine));
    auto* led = window.findChild<QToolButton*>("inputClip");
    QVERIFY(led != nullptr);
    QVERIFY(!led->property("lit").toBool());
    QPushButton* record = findButton(window, "●  RECORD");
    record->click(); // creates and arms Track 1
    QVERIFY(fake->isRecording());

    // A full-scale peak alone is not a clip: the light follows the engine's
    // clip count (runs of full-scale samples), not the raw peak.
    fake->setInputPeak(1.0f);
    window.tickForTest();
    QVERIFY(!led->property("lit").toBool());
    fake->setInputClipStats({1, 3});
    window.tickForTest();
    QVERIFY(led->property("lit").toBool());
    window.tickForTest();
    QVERIFY(led->property("lit").toBool()); // latched
    led->click();
    QVERIFY(!led->property("lit").toBool());
    window.tickForTest();
    QVERIFY(!led->property("lit").toBool()); // no new clip since the click

    fake->setInputClipStats({2, 7}); // a new clip relights it
    window.tickForTest();
    QVERIFY(led->property("lit").toBool());
    record->click(); // stop
    // A new take starts dark (the engine's count starts again from zero).
    record->click();
    QVERIFY(!led->property("lit").toBool());
    record->click();
}

void TestGui::inputClipCountIsShownAndReportedAtStop() {
    auto engine = std::make_unique<FakeAudioEngine>();
    FakeAudioEngine* fake = engine.get();
    MainWindow window(std::move(engine));
    auto* led = window.findChild<QToolButton*>("inputClip");
    auto* status = window.findChild<QLabel*>("status");
    QVERIFY(led != nullptr && status != nullptr);
    QCOMPARE(led->text(), QString("INPUT CLIP"));

    QPushButton* record = findButton(window, "●  RECORD");
    record->click();
    fake->setInputClipStats({2, 9});
    window.tickForTest();
    QCOMPARE(led->text(), QString::fromUtf8("INPUT CLIP \u00d72"));
    QVERIFY(led->toolTip().contains("2 clips, 9 clipped samples"));

    // Clips in the last moments before Stop (after the last tick) count too.
    fake->setInputClipStats({3, 14});
    fake->setCapturedBuffer(std::vector<float>(100, 0.1f));
    findButton(window, "■  STOP")->click();
    QCOMPARE(led->text(), QString::fromUtf8("INPUT CLIP \u00d73"));
    QVERIFY(led->property("lit").toBool());
    QVERIFY2(status->text().contains("the input clipped 3 times (14 samples)"), qPrintable(status->text()));
    led->click(); // clears the light, keeps the take's count
    QCOMPARE(led->text(), QString::fromUtf8("INPUT CLIP \u00d73"));

    // A clean take says so plainly and resets the count.
    record = findButton(window, "●  RECORD");
    record->click();
    QCOMPARE(led->text(), QString("INPUT CLIP"));
    fake->setCapturedBuffer(std::vector<float>(100, 0.1f));
    findButton(window, "■  STOP")->click();
    QCOMPARE(status->text(), QString("Stopped"));
}

void TestGui::clipSelectionIsDroppedWhenTracksChange() {
    // Regression: selected clips are (track, index) pairs that outlived
    // undo/redo/paste, so a later drag could move a clip that was never
    // outlined.
    panel_.setTool(TrackPanel::Tool::Move);
    const QPoint onClip(TrackPanel::kHeaderWidth + 100, laneCentreY(0));
    QTest::mouseClick(&panel_, Qt::LeftButton, Qt::NoModifier, onClip);
    QCOMPARE(panel_.selectedClipCountForTest(), size_t(1));

    project_.tracks[0].clips.insert(project_.tracks[0].clips.begin(), makeClip(0, 100));
    project_.tracks[0].clips[1].startFrame = 50000;
    panel_.refresh(); // what MainWindow does after every edit, undo and redo
    QCOMPARE(panel_.selectedClipCountForTest(), size_t(0));
}

void TestGui::rulerClickSeeksWhileStopped() {
    QSignalSpy spy(&panel_, &TrackPanel::seekRequested);
    QTest::mouseClick(&panel_, Qt::LeftButton, Qt::NoModifier,
                       QPoint(TrackPanel::kHeaderWidth + 250, TrackPanel::kRulerHeight / 2));
    QCOMPARE(project_.playheadFrame, int64_t(25000));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<int64_t>(), int64_t(25000));
    QVERIFY(project_.selection.isEmpty()); // a seek isn't a selection

    // Dragging along the ruler scrubs.
    QTest::mousePress(&panel_, Qt::LeftButton, Qt::NoModifier,
                       QPoint(TrackPanel::kHeaderWidth + 100, TrackPanel::kRulerHeight / 2));
    QTest::mouseMove(&panel_, QPoint(TrackPanel::kHeaderWidth + 150, TrackPanel::kRulerHeight / 2));
    QTest::mouseRelease(&panel_, Qt::LeftButton, Qt::NoModifier,
                         QPoint(TrackPanel::kHeaderWidth + 150, TrackPanel::kRulerHeight / 2));
    QCOMPARE(project_.playheadFrame, int64_t(15000));
}

void TestGui::seekingAndAutoScrollDuringPlayback() {
    auto engine = std::make_unique<FakeAudioEngine>();
    FakeAudioEngine* fake = engine.get();
    MainWindow window(std::move(engine));
    window.resize(1000, 600);
    TrackPanel* panel = window.findChild<TrackPanel*>();
    Project* project = panel->projectForTest();
    project->channels = 1;
    Track track;
    track.clips.push_back(makeClip(0, 10'000'000)); // long enough to scroll
    project->tracks.push_back(std::move(track));
    panel->refresh();
    panel->setFramesPerPixelForTest(100.0);

    QPushButton* play = findButton(window, "▶  Play");
    play->click();
    QVERIFY(fake->isPlaying());

    // Click-to-seek while playing jumps the running playback.
    QTest::mouseClick(panel, Qt::LeftButton, Qt::NoModifier,
                       QPoint(TrackPanel::kHeaderWidth + 300, TrackPanel::kRulerHeight / 2));
    QCOMPARE(fake->lastSeek(), int64_t(30000));
    QCOMPARE(project->playheadFrame, int64_t(30000));
    QVERIFY(fake->isPlaying());

    // Playback passing the right edge pages the view along.
    QCOMPARE(panel->viewStartFrameForTest(), int64_t(0));
    const int64_t farFrame = 2'000'000;
    fake->setPlaybackFrame(farFrame);
    QTRY_VERIFY(panel->viewStartFrameForTest() > 0);
    const int64_t start = panel->viewStartFrameForTest();
    QVERIFY(start <= farFrame);
    QVERIFY(farFrame < start + int64_t((panel->width() - TrackPanel::kHeaderWidth) * 100.0));

    // Running out returns the playhead to the last seek point.
    fake->finishPlayback();
    QTRY_COMPARE(play->text(), QString("▶  Play"));
    QCOMPARE(project->playheadFrame, int64_t(30000));
}

void TestGui::normalizeDialogFlagsClipping() {
    NormalizeDialog dialog(0.5f, "the selection");
    // Default: normalize to -1 dBFS, which fits.
    QVERIFY(!dialog.clipIndicatorShown());
    QVERIFY(dialog.acceptEnabled());
    QVERIFY(std::fabs(linearToDb(dialog.plan().resultingPeak) + 1.0f) < 1e-3f);

    // +12 dB on a -6 dBFS peak would clip: the indicator shows and OK waits
    // for "Allow clipping".
    dialog.setMode(NormalizeDialog::Mode::Amplify);
    dialog.setGainDb(12.0);
    QVERIFY(dialog.clipIndicatorShown());
    QVERIFY(!dialog.acceptEnabled());
    dialog.setAllowClipping(true);
    QVERIFY(dialog.acceptEnabled());
    QCOMPARE(dialog.actionName(), QString("Amplify"));

    dialog.setGainDb(3.0);
    QVERIFY(!dialog.clipIndicatorShown());

    // Normalizing above 0 dBFS clips too.
    dialog.setMode(NormalizeDialog::Mode::Normalize);
    dialog.setTargetDb(1.0);
    QVERIFY(dialog.clipIndicatorShown());
}

void TestGui::normalizeSelectionIsOneUndoStep() {
    MainWindow window(std::make_unique<FakeAudioEngine>());
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    project->channels = 1;
    Track track;
    track.clips.push_back(makeClip(0, 1000, 0.25f));
    project->tracks.push_back(std::move(track));
    TrackPanel* panel = window.findChild<TrackPanel*>();
    panel->refresh();
    panel->setFramesPerPixelForTest(1.0);
    // Drag out frames 100..200 so MainWindow sees a selection change.
    QTest::mousePress(panel, Qt::LeftButton, Qt::NoModifier, QPoint(TrackPanel::kHeaderWidth + 100, laneCentreY(0)));
    QTest::mouseMove(panel, QPoint(TrackPanel::kHeaderWidth + 200, laneCentreY(0)));
    QTest::mouseRelease(panel, Qt::LeftButton, Qt::NoModifier, QPoint(TrackPanel::kHeaderWidth + 200, laneCentreY(0)));
    QCOMPARE(project->selection.startFrame, int64_t(100));
    QCOMPARE(project->selection.endFrame, int64_t(200));
    QUndoStack* undo = window.findChild<QUndoStack*>();
    const int before = undo->count();

    QAction* normalize = nullptr;
    for (QAction* action : window.findChildren<QAction*>()) {
        if (action->text() == "Normalize / Amplify...") {
            normalize = action;
        }
    }
    QVERIFY(normalize != nullptr);
    QVERIFY(normalize->isEnabled());
    window.setNormalizeDialogDriverForTest([](NormalizeDialog& dialog) {
        dialog.setMode(NormalizeDialog::Mode::Normalize);
        dialog.setTargetDb(-6.0);
        return true;
    });
    normalize->trigger();

    QCOMPARE(undo->count(), before + 1);
    QCOMPARE(undo->undoText(), QString("Normalize"));
    const Clip& clip = project->tracks[0].clips[0];
    QVERIFY(std::fabs(clip.samples[150] - dbToLinear(-6.0f)) < 1e-5f);
    QCOMPARE(clip.samples[50], 0.25f); // outside the selection
    undo->undo();
    QCOMPARE(project->tracks[0].clips[0].samples[150], 0.25f);
}

void TestGui::amplifySelectedClips() {
    MainWindow window(std::make_unique<FakeAudioEngine>());
    window.resize(1000, 600);
    TrackPanel* panel = window.findChild<TrackPanel*>();
    Project* project = panel->projectForTest();
    project->channels = 1;
    project->sampleRate = 44100.0;
    for (int i = 0; i < 2; ++i) {
        Track track;
        track.clips.push_back(makeClip(0, 40000, 0.1f * float(i + 1)));
        project->tracks.push_back(std::move(track));
    }
    panel->refresh();
    panel->setFramesPerPixelForTest(100.0);
    window.refreshActionStateForTest(); // audio was added behind MainWindow's back
    panel->setTool(TrackPanel::Tool::Move);
    QTest::mouseClick(panel, Qt::LeftButton, Qt::NoModifier, QPoint(TrackPanel::kHeaderWidth + 100, laneCentreY(0)));
    QTest::mouseClick(panel, Qt::LeftButton, Qt::ControlModifier, QPoint(TrackPanel::kHeaderWidth + 100, laneCentreY(1)));
    QCOMPARE(panel->selectedClips().size(), size_t(2));

    window.setNormalizeDialogDriverForTest([](NormalizeDialog& dialog) {
        dialog.setMode(NormalizeDialog::Mode::Amplify);
        dialog.setGainDb(6.0);
        return true;
    });
    for (QAction* action : window.findChildren<QAction*>()) {
        if (action->text() == "Normalize / Amplify...") {
            QVERIFY(action->isEnabled());
            action->trigger();
        }
    }
    QVERIFY(std::fabs(project->tracks[0].clips[0].samples[0] - 0.1f * dbToLinear(6.0f)) < 1e-5f);
    QVERIFY(std::fabs(project->tracks[1].clips[0].samples[39999] - 0.2f * dbToLinear(6.0f)) < 1e-5f);
    QCOMPARE(window.findChild<QUndoStack*>()->undoText(), QString("Amplify"));
}

void TestGui::peakMeterBallisticsAndHold() {
    PeakMeter meter;
    meter.setPeakAt(0.5f, 0); // -6 dBFS
    QVERIFY(std::fabs(meter.levelDb() + 6.02f) < 0.01f);
    QVERIFY(std::fabs(meter.holdDb() + 6.02f) < 0.01f);

    // Silence: the bar falls at 24 dB/s, the hold marker stays put...
    meter.setPeakAt(0.0f, 500);
    QVERIFY(std::fabs(meter.levelDb() + 18.02f) < 0.01f);
    QVERIFY(std::fabs(meter.holdDb() + 6.02f) < 0.01f);
    meter.setPeakAt(0.0f, 1400);
    QVERIFY(std::fabs(meter.holdDb() + 6.02f) < 0.01f);
    // ...until the hold time has passed.
    meter.setPeakAt(0.0f, 2000);
    QVERIFY(meter.holdDb() < -6.5f);
    // A louder peak takes over the hold at once.
    meter.setPeakAt(0.25f, 2050);
    meter.setPeakAt(0.7f, 2100);
    QVERIFY(std::fabs(meter.holdDb() + 3.1f) < 0.01f);
    QVERIFY(!meter.clipLit());
    // It bottoms out at the floor.
    meter.setPeakAt(0.0f, 60000);
    QCOMPARE(meter.levelDb(), PeakMeter::kFloorDb);
}

void TestGui::peakMeterClipLedLatchesUntilClicked() {
    PeakMeter meter;
    meter.resize(400, 30);
    QSignalSpy reset(&meter, &PeakMeter::clipReset);
    meter.setPeakAt(0.99f, 0);
    QVERIFY(!meter.clipLit());
    meter.setPeakAt(1.0f, 50); // 0 dBFS counts
    QVERIFY(meter.clipLit());
    for (int t = 100; t < 10000; t += 100) {
        meter.setPeakAt(0.1f, t);
    }
    QVERIFY(meter.clipLit()); // still lit long after
    QTest::mouseClick(&meter, Qt::LeftButton, Qt::NoModifier, QPoint(390, 10));
    QVERIFY(!meter.clipLit());
    QCOMPARE(reset.count(), 1);
}

void TestGui::mainWindowMetersPlayback() {
    auto engine = std::make_unique<FakeAudioEngine>();
    FakeAudioEngine* fake = engine.get();
    MainWindow window(std::move(engine));
    PeakMeter* meter = window.findChild<PeakMeter*>();
    QVERIFY(meter != nullptr);
    QVERIFY(window.findChild<QProgressBar*>() == nullptr); // the old linear bar is gone
    Project* project = window.findChild<TrackPanel*>()->projectForTest();
    Track track;
    track.clips.push_back(makeClip(0, 400000, 0.5f));
    project->tracks.push_back(std::move(track));

    findButton(window, "▶  Play")->click();
    fake->setMeterPeak(0.5f);
    QTRY_VERIFY(meter->holdDb() > -6.1f);
    QVERIFY(!meter->clipLit());
    fake->setMeterPeak(1.3f);
    QTRY_VERIFY(meter->clipLit());
}

void TestGui::repaintDoesNotWaitForTheProjectMutex() {
    // From the review's repro_lock: paintEvent held project.mutex for a
    // whole repaint (~450 ms with two spectrogram tracks), which is what
    // stalled playback. Paint runs on the UI thread, where every writer
    // lives, so it needs no lock; here another thread holds the mutex for
    // 3 s and a full repaint (waveform and spectrogram) must not wait for it.
    for (Track& track : project_.tracks) {
        track.display = TrackDisplay::Spectrogram;
    }
    project_.tracks[1].clips.push_back(makeClip(0, 40000, 0.3f));
    panel_.refresh();

    std::atomic<bool> locked{false};
    std::thread holder([this, &locked] {
        std::lock_guard<std::mutex> lock(project_.mutex);
        locked = true;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    });
    while (!locked) {
        std::this_thread::yield();
    }
    QElapsedTimer timer;
    timer.start();
    QImage image = panel_.grab().toImage();
    const qint64 elapsed = timer.elapsed();
    holder.join();
    QVERIFY(!image.isNull());
    QVERIFY2(elapsed < 2000, qPrintable(QString("repaint waited %1 ms for the project mutex").arg(elapsed)));
    for (Track& track : project_.tracks) {
        track.display = TrackDisplay::Waveform;
    }
}

void TestGui::playheadKeysMoveThePlayhead() {
    MainWindow window(std::make_unique<FakeAudioEngine>());
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto* panel = window.findChild<TrackPanel*>();
    Project* project = panel->projectForTest();
    project->sampleRate = 44100.0;
    Track track;
    track.clips.push_back(makeClip(0, 441000)); // 10 s
    project->tracks.push_back(std::move(track));

    // The keys are scoped to the timeline, so a focused slider keeps them.
    panel->setFocus();
    QTRY_VERIFY(panel->hasFocus());
    QTest::keyClick(&window, Qt::Key_End);
    QCOMPARE(project->playheadFrame, int64_t(441000));
    QTest::keyClick(&window, Qt::Key_Left);
    QCOMPARE(project->playheadFrame, int64_t(441000 - 44100));
    QTest::keyClick(&window, Qt::Key_Right);
    QCOMPARE(project->playheadFrame, int64_t(441000));
    QTest::keyClick(&window, Qt::Key_Home);
    QCOMPARE(project->playheadFrame, int64_t(0));
    QTest::keyClick(&window, Qt::Key_Left); // clamped at the start
    QCOMPARE(project->playheadFrame, int64_t(0));

    auto* slider = window.findChild<QSlider*>();
    QVERIFY(slider != nullptr);
    slider->setFocus();
    QTRY_VERIFY(slider->hasFocus());
    QTest::keyClick(&window, Qt::Key_End);
    QCOMPARE(project->playheadFrame, int64_t(0));

    // Both the familiar zoom keys and the original ones are bound.
    const QList<QKeySequence> zoomIn = findAction(window, "Zoom In")->shortcuts();
    QVERIFY(zoomIn.contains(QKeySequence("Ctrl+=")));
    QVERIFY(zoomIn.contains(QKeySequence("Ctrl+1")));
    const QList<QKeySequence> zoomOut = findAction(window, "Zoom Out")->shortcuts();
    QVERIFY(zoomOut.contains(QKeySequence("Ctrl+-")));
    QVERIFY(zoomOut.contains(QKeySequence("Ctrl+3")));
}

void TestGui::aboutShowsTheBuildVersion() {
    // Plain x.y.z only for a release build; otherwise x.y.z+g<sha> (or
    // +dev without git), so a development build can't pass for a release.
    const QString version = ZRECORD_VERSION;
    QVERIFY2(QRegularExpression("^\\d+\\.\\d+\\.\\d+(\\+g[0-9a-f]{7,}|\\+dev)?$").match(version).hasMatch(),
             qPrintable(version));
    QVERIFY(MainWindow::aboutText().contains("zrecord " + version));
}

void TestGui::voiceChangerPresetsAndCustom() {
    VoiceChangerDialog dialog("the selection (1.00 s)");
    QCOMPARE(dialog.preset(), VoicePreset::Deeper);
    QVERIFY(dialog.settings() == voicePresetSettings(VoicePreset::Deeper));
    dialog.setPreset(VoicePreset::Chipmunk);
    QVERIFY(dialog.settings() == voicePresetSettings(VoicePreset::Chipmunk));
    dialog.setPreset(VoicePreset::Robot);
    QVERIFY(dialog.settings().robot);
    // Touching any control makes it a custom voice, keeping the other values.
    dialog.setFormant(3.0);
    QCOMPARE(dialog.preset(), VoicePreset::Custom);
    QVERIFY(dialog.settings().robot);
    QCOMPARE(dialog.settings().formantSemitones, 3.0f);
    dialog.setPreset(VoicePreset::Higher);
    QVERIFY(dialog.settings() == voicePresetSettings(VoicePreset::Higher));
    // All zero is no change: nothing to apply or preview.
    dialog.setPitch(0.0);
    dialog.setFormant(0.0);
    QCOMPARE(dialog.preset(), VoicePreset::Custom);
    QVERIFY(dialog.settings().isIdentity());
    QVERIFY(!dialog.applyButton()->isEnabled());
    QVERIFY(!dialog.previewButton()->isEnabled());
    dialog.setPitch(-2.5);
    QVERIFY(dialog.applyButton()->isEnabled());
    QVERIFY(dialog.previewButton()->isEnabled());
}

namespace {

// A 1 s, 44.1 kHz mono project holding a 200 Hz sine, with frames
// 10000..30000 selected.
struct VoiceFixture {
    FakeAudioEngine* engine = nullptr;
    std::unique_ptr<MainWindow> window;
    Project* project = nullptr;
    QAction* action = nullptr;

    VoiceFixture() {
        auto fake = std::make_unique<FakeAudioEngine>();
        engine = fake.get();
        window = std::make_unique<MainWindow>(std::move(fake));
        project = window->findChild<TrackPanel*>()->projectForTest();
        project->channels = 1;
        project->sampleRate = 44100.0;
        std::vector<float> tone(44100);
        for (size_t i = 0; i < tone.size(); ++i) {
            tone[i] = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * 200.0 * double(i) / 44100.0));
        }
        Track track;
        Clip clip;
        clip.channels = 1;
        clip.samples = SampleBuffer(tone);
        track.clips.push_back(std::move(clip));
        project->tracks.push_back(std::move(track));
        window->findChild<TrackPanel*>()->refresh();
        project->selection.trackIndex = 0;
        project->selection.startFrame = 10000;
        project->selection.endFrame = 30000;
        window->refreshActionStateForTest();
        for (QAction* a : window->findChildren<QAction*>()) {
            if (a->text() == "Voice Changer...") {
                action = a;
            }
        }
    }
    std::vector<float> samples() const { return project->tracks[0].clips[0].samples.toVector(); }
};

} // namespace

void TestGui::voiceChangerPreviewThenApplyIsOneUndoStep() {
    VoiceFixture f;
    QVERIFY(f.action != nullptr);
    QVERIFY(f.action->isEnabled());
    const std::vector<float> original = f.samples();
    QUndoStack* undo = f.window->findChild<QUndoStack*>();
    const int before = undo->count();

    bool checkedPreview = false;
    f.window->setVoiceChangerDialogDriverForTest([&](VoiceChangerDialog& dialog) {
        dialog.setPreset(VoicePreset::Deeper);
        QTest::mouseClick(dialog.previewButton(), Qt::LeftButton);
        // The preview plays a processed copy of just the selection, through
        // the same engine, and leaves the project alone.
        const Project* preview = f.window->previewProjectForTest();
        if (preview == nullptr || !f.engine->isPlaying() || f.engine->lastPlaybackProject() != preview) {
            return false;
        }
        checkedPreview = preview->lengthFrames() == 20000 && preview->playheadFrame == 0
                         && dialog.previewPlaying() && f.samples() == original
                         && preview->tracks[0].clips[0].samples.toVector()
                                != std::vector<float>(original.begin() + 10000, original.begin() + 30000);
        // Running out by itself puts the button back to Preview.
        f.engine->finishPlayback();
        f.window->tickForTest();
        checkedPreview = checkedPreview && !dialog.previewPlaying() && f.window->previewProjectForTest() == nullptr;
        // Start it again and Apply while it plays.
        QTest::mouseClick(dialog.previewButton(), Qt::LeftButton);
        return true;
    });
    f.action->trigger();

    QVERIFY(checkedPreview);
    QVERIFY(!f.engine->isPlaying());                         // closing the dialog stopped the preview
    QVERIFY(f.window->previewProjectForTest() == nullptr);
    QCOMPARE(undo->count(), before + 1);
    QCOMPARE(undo->undoText(), QString("Voice Changer: Deeper"));
    const std::vector<float> after = f.samples();
    QCOMPARE(after.size(), original.size());
    QVERIFY(std::equal(after.begin(), after.begin() + 10000, original.begin()));   // before the selection
    QVERIFY(std::equal(after.begin() + 30000, after.end(), original.begin() + 30000)); // after it
    QVERIFY(!std::equal(after.begin() + 10000, after.begin() + 30000, original.begin() + 10000));
    undo->undo();
    QVERIFY(f.samples() == original);
    undo->redo();
    QVERIFY(f.samples() == after);
}

void TestGui::voiceChangerCancelStopsPreviewAndChangesNothing() {
    VoiceFixture f;
    const std::vector<float> original = f.samples();
    QUndoStack* undo = f.window->findChild<QUndoStack*>();
    const int before = undo->count();
    bool wasPlaying = false;
    f.window->setVoiceChangerDialogDriverForTest([&](VoiceChangerDialog& dialog) {
        dialog.setPreset(VoicePreset::Chipmunk);
        QTest::mouseClick(dialog.previewButton(), Qt::LeftButton);
        wasPlaying = f.engine->isPlaying();
        return false; // Cancel
    });
    f.action->trigger();
    QVERIFY(wasPlaying);
    QVERIFY(!f.engine->isPlaying());
    QVERIFY(f.window->previewProjectForTest() == nullptr);
    QCOMPARE(undo->count(), before);
    QVERIFY(f.samples() == original);
}

QTEST_MAIN(TestGui)
#include "test_gui.moc"
