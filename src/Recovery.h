#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Project.h"
#include "ProjectFile.h"
#include "SampleBuffer.h"

class QLockFile;

namespace zrecord {

// Crash recovery (the idea follows Audacity/Tenacity's autosave and
// "recover unsaved projects" prompt: ideas only, no code).
//
// Every running zrecord owns a session folder under a recovery root
// (~/.local/share/zrecord/recovery), held by a lock file for as long as it
// runs. A clean exit removes the folder; a crash leaves it, with a lock
// whose process is gone. The next launch finds such folders and offers to
// restore them.
//
// A session folder holds:
//   lock            - QLockFile: the "still running" marker
//   session.json    - the project's folder (if saved) and every take file
//                     this session created (they may live in the project)
//   journal.json    - the autosave journal: the project's state in the
//                     project.json format, clips referring to audio files
//   audio/          - journal audio not already in a file
//   takes/          - takes of an unsaved project
//   recording.json  - the take being recorded right now, if any
// Every file is written to a temporary name and renamed into place, so a
// crash mid-write leaves the previous version intact.

// The take being recorded, as recording.json describes it.
struct TakeInProgress {
    QString path;           // the streamed take file
    int trackIndex = 0;     // the armed track
    int64_t startFrame = 0; // where on it the take starts
    int channels = 1;       // of the file (a mono take for a stereo project is upmixed)
};

class RecoverySession {
public:
    // The recovery root the app uses: ~/.local/share/zrecord/recovery.
    static QString defaultRoot();

    explicit RecoverySession(const QString& root);
    // Stops the journal thread. Leaves the folder: only discard() removes it,
    // so a session that isn't ended cleanly stays recoverable.
    ~RecoverySession();
    RecoverySession(const RecoverySession&) = delete;
    RecoverySession& operator=(const RecoverySession&) = delete;

    // Creates the session folder and takes its lock.
    bool begin(QString* error = nullptr);
    bool active() const { return !dir_.isEmpty(); }
    QString dir() const { return dir_; }

    // The saved project's folder, if any (where Save goes after a restore,
    // and where takes are written).
    void setProjectPath(const QString& path);
    QString projectPath() const { return projectPath_; }

    // A path for the next take: in the project's takes/ folder when the
    // project is saved (and that folder is writable), else in the session.
    // Remembered, so clear() and discard() can remove it.
    QString newTakePath();
    // recording.json while a take runs; endTake() removes it.
    bool beginTake(const TakeInProgress& take, QString* error = nullptr);
    void endTake();
    // A finished take now in the project: journals written from here on
    // say so, so a restore won't add it twice.
    void markTakeCommitted(const QString& path);

    // Audio already in a file (a take, a saved or loaded project's clip):
    // the journal refers to it instead of writing a copy.
    void setAudioFile(uint64_t contentId, const QString& path);
    void setAudioFiles(const ClipFileMap& files);
    void clearAudioFiles();

    // Snapshots `project` (UI thread, cheap: audio is shared, not copied)
    // and hands it to the journal thread, which writes it in the
    // background. A newer snapshot replaces one not yet started.
    void scheduleJournal(const Project& project);
    // Waits until the journal thread is idle. False if the last write failed.
    bool flushJournal();
    QString lastJournalError() const;
    int journalsWritten() const;

    // After a save, or when unsaved work is discarded: removes the journal,
    // its audio, recording.json and every take file this session created
    // (the project, if saved, now has its own copy). The session goes on.
    void clear();
    // Clean exit: clear() and remove the session folder and its lock.
    void discard();

    // Tests: the next journal write stops after `bytes` bytes of
    // journal.json, as if the process died mid-write (never committed).
    void setJournalFailAfterBytesForTesting(qint64 bytes);
    // Tests: releases the lock without cleaning up -- what a crash leaves.
    void abandonForTesting();

private:
    struct AudioJob {
        QString file; // absolute
        SampleBuffer samples;
        int channels = 1;
        int sampleRate = 44100;
    };
    struct Snapshot {
        QJsonObject journal;
        std::vector<AudioJob> audio;
        QStringList referenced; // file names in audio/ to keep
    };

    void run();
    bool write(const Snapshot& snapshot, QString& error);
    bool writeSessionInfo();
    void stopThread();

    QString root_;
    QString dir_;
    QString projectPath_;
    QStringList takes_;          // every take file this session created
    QStringList committedTakes_; // of those, the ones in the project now
    ClipFileMap audioFiles_;
    std::unique_ptr<QLockFile> lock_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::unique_ptr<Snapshot> pending_;
    bool busy_ = false;
    bool quit_ = false;
    bool lastOk_ = true;
    QString lastError_;
    int written_ = 0;
    qint64 failAfterBytes_ = -1;
};

// A session left by a process that didn't exit cleanly. Holding one holds
// its lock, so two instances can't restore the same session.
class RecoverableSession {
public:
    // Every crashed session under `root` with something to restore, newest
    // first. Crashed sessions with nothing in them are removed on the way.
    static std::vector<std::unique_ptr<RecoverableSession>> find(const QString& root);
    ~RecoverableSession();

    QString dir() const { return dir_; }
    QDateTime lastSaved() const { return lastSaved_; } // of its newest file
    QString projectPath() const { return projectPath_; }
    bool hasJournal() const { return hasJournal_; }
    bool hasPartialTake() const { return hasTake_; }
    int trackCount() const { return tracks_; }
    int clipCount() const { return clips_; }
    int64_t partialTakeFrames() const { return takeFrames_; }
    double sampleRate() const { return sampleRate_; }
    // "Untitled project, 2 tracks, 5 clips, and a partial take of 1:23".
    QString summary() const;

    // Rebuilds the project: the journal, then the partial take (if any, and
    // not already in the journal) as a new clip where it was being recorded.
    // `takeFrames` gets the partial take's length (0 if none).
    bool restore(Project& project, std::string& error, int64_t* takeFrames = nullptr) const;
    // Removes the session folder and every take file it created.
    void discard();

private:
    RecoverableSession() = default;
    QString dir_;
    QDateTime lastSaved_;
    QString projectPath_;
    QStringList takes_;
    bool hasJournal_ = false;
    bool hasTake_ = false;
    int tracks_ = 0;
    int clips_ = 0;
    int64_t takeFrames_ = 0;
    double sampleRate_ = 44100.0;
    TakeInProgress take_;
    std::unique_ptr<QLockFile> lock_;
};

} // namespace zrecord
