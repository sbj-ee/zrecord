#include "Recovery.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <set>

#include "AudioFileWriter.h"
#include "TakeFile.h"

namespace zrecord {

namespace {

const char kLock[] = "lock";
const char kSessionInfo[] = "session.json";
const char kJournal[] = "journal.json";
const char kRecording[] = "recording.json";
const char kAudioDir[] = "audio";
const char kTakesDir[] = "takes";
const char kMetaKey[] = "zrecordJournal";

bool writeJsonAtomically(const QString& path, const QJsonObject& object, QString* error = nullptr) {
    QSaveFile file(path);
    const QByteArray json = QJsonDocument(object).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit()) {
        if (error != nullptr) {
            *error = QString("Could not write %1: %2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

QJsonObject readJson(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

QStringList toStringList(const QJsonValue& value) {
    QStringList list;
    for (const QJsonValue& v : value.toArray()) list << v.toString();
    return list;
}

// Frames of a take file, from its size (it may have been cut short).
int64_t takeFileFrames(const QString& path, int* channelsOut = nullptr) {
    std::vector<float> none;
    int channels = 0, rate = 0;
    std::string error;
    if (!readTakeFile(path.toStdString(), none, channels, rate, error, 0)) {
        return -1;
    }
    if (channelsOut != nullptr) *channelsOut = channels;
    return std::max<int64_t>(0, (QFileInfo(path).size() - FloatWavAppender::kHeaderBytes) / (int64_t(channels) * 4));
}

std::unique_ptr<QLockFile> makeLock(const QString& dir) {
    auto lock = std::make_unique<QLockFile>(QDir(dir).filePath(kLock));
    lock->setStaleLockTime(0); // stale only when its process is gone, never by age
    return lock;
}

QString formatLength(int64_t frames, double sampleRate) {
    const int seconds = static_cast<int>(static_cast<double>(frames) / std::max(1.0, sampleRate));
    return QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}

} // namespace

QString RecoverySession::defaultRoot() {
    // ~/.local/share/zrecord/recovery. (AppLocalDataLocation would nest it
    // as zrecord/zrecord, organisation and application both being zrecord.)
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/zrecord/recovery";
}

RecoverySession::RecoverySession(const QString& root) : root_(root) {}

RecoverySession::~RecoverySession() { stopThread(); }

bool RecoverySession::begin(QString* error) {
    if (active()) {
        return true;
    }
    QDir root(root_);
    const QString name = QString("session-%1-%2-%3")
                             .arg(QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss"))
                             .arg(QCoreApplication::applicationPid())
                             .arg(QUuid::createUuid().toString(QUuid::Id128).left(8));
    if (!root.mkpath(name)) {
        if (error != nullptr) *error = "Could not create the recovery folder " + root.filePath(name);
        return false;
    }
    const QString dir = root.filePath(name);
    auto lock = makeLock(dir);
    if (!lock->tryLock(0)) {
        if (error != nullptr) *error = "Could not lock the recovery folder " + dir;
        QDir(dir).removeRecursively();
        return false;
    }
    dir_ = dir;
    lock_ = std::move(lock);
    quit_ = false;
    writeSessionInfo();
    thread_ = std::thread([this] { run(); });
    return true;
}

bool RecoverySession::writeSessionInfo() {
    if (!active()) return false;
    QJsonObject info;
    info["projectPath"] = projectPath_;
    info["takes"] = QJsonArray::fromStringList(takes_);
    info["pid"] = QCoreApplication::applicationPid();
    return writeJsonAtomically(QDir(dir_).filePath(kSessionInfo), info);
}

void RecoverySession::setProjectPath(const QString& path) {
    projectPath_ = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
    writeSessionInfo();
}

QString RecoverySession::newTakePath() {
    if (!active()) return {};
    static int serial = 0;
    const QString name = QString("take-%1-%2.wav").arg(QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss")).arg(++serial);
    QString folder;
    if (!projectPath_.isEmpty() && QDir(projectPath_).mkpath(kTakesDir) &&
        QFileInfo(QDir(projectPath_).filePath(kTakesDir)).isWritable()) {
        folder = QDir(projectPath_).filePath(kTakesDir);
    } else {
        QDir(dir_).mkpath(kTakesDir);
        folder = QDir(dir_).filePath(kTakesDir);
    }
    const QString path = QDir(folder).filePath(name);
    takes_ << path;
    writeSessionInfo(); // before the take exists, so a crash can't orphan it
    return path;
}

bool RecoverySession::beginTake(const TakeInProgress& take, QString* error) {
    if (!active()) return false;
    QJsonObject info;
    info["path"] = take.path;
    info["track"] = take.trackIndex;
    info["startFrame"] = QString::number(take.startFrame);
    info["channels"] = take.channels;
    return writeJsonAtomically(QDir(dir_).filePath(kRecording), info, error);
}

void RecoverySession::endTake() {
    if (active()) QFile::remove(QDir(dir_).filePath(kRecording));
}

void RecoverySession::markTakeCommitted(const QString& path) {
    if (!committedTakes_.contains(path)) committedTakes_ << path;
}

void RecoverySession::setAudioFile(uint64_t contentId, const QString& path) { audioFiles_[contentId] = path; }

void RecoverySession::setAudioFiles(const ClipFileMap& files) {
    for (const auto& [id, path] : files) audioFiles_[id] = path;
}

void RecoverySession::clearAudioFiles() { audioFiles_.clear(); }

void RecoverySession::scheduleJournal(const Project& project) {
    if (!active()) return;
    auto snapshot = std::make_unique<Snapshot>();
    const QDir audioDir(QDir(dir_).filePath(kAudioDir));
    std::set<uint64_t> queued;
    snapshot->journal = ProjectFile::manifest(project, [&](size_t, size_t, const Clip& clip) {
        const uint64_t id = clip.samples.contentId();
        const auto known = audioFiles_.find(id);
        if (known != audioFiles_.end()) {
            return known->second;
        }
        // Named by content: the same audio (a moved clip, an unchanged one)
        // is written once, and only audio that changed is written again.
        const QString name = QString("c%1-%2ch.wav").arg(id).arg(clip.channels);
        if (queued.insert(id).second) {
            snapshot->audio.push_back({audioDir.filePath(name), clip.samples, clip.channels,
                                       static_cast<int>(project.sampleRate)});
            snapshot->referenced << name;
        }
        return audioDir.filePath(name);
    });
    QJsonObject meta;
    meta["version"] = 1;
    meta["projectPath"] = projectPath_;
    meta["time"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    meta["committedTakes"] = QJsonArray::fromStringList(committedTakes_);
    snapshot->journal[kMetaKey] = meta;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(snapshot); // a newer state replaces one not yet written
    }
    wake_.notify_all();
}

void RecoverySession::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return quit_ || pending_ != nullptr; });
        if (quit_) break;
        std::unique_ptr<Snapshot> snapshot = std::move(pending_);
        busy_ = true;
        lock.unlock();
        QString error;
        const bool ok = write(*snapshot, error);
        snapshot.reset(); // drop the audio references outside the lock
        lock.lock();
        busy_ = false;
        lastOk_ = ok;
        lastError_ = error;
        if (ok) ++written_;
        if (pending_ == nullptr) idle_.notify_all();
    }
    busy_ = false;
    idle_.notify_all();
}

bool RecoverySession::write(const Snapshot& snapshot, QString& error) {
    QDir dir(dir_);
    if (!snapshot.audio.empty() && !dir.mkpath(kAudioDir)) {
        error = "Could not create " + dir.filePath(kAudioDir);
        return false;
    }
    for (const AudioJob& job : snapshot.audio) {
        if (QFile::exists(job.file)) {
            continue;
        }
        // Temporary name, then rename: a half-written file never has the
        // name the journal refers to.
        const QString part = job.file + ".part";
        std::string message;
        if (!AudioFileWriter::writeFloatWav(part.toStdString(), job.samples, job.sampleRate, job.channels, message)) {
            QFile::remove(part);
            error = QString::fromStdString(message);
            return false;
        }
        if (!QFile::rename(part, job.file)) {
            QFile::remove(part);
            error = "Could not write " + job.file;
            return false;
        }
    }

    const QString journalPath = dir.filePath(kJournal);
    qint64 failAfter = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::swap(failAfter, failAfterBytes_);
    }
    const QByteArray json = QJsonDocument(snapshot.journal).toJson();
    QSaveFile file(journalPath);
    if (!file.open(QIODevice::WriteOnly)) {
        error = "Could not write " + journalPath + ": " + file.errorString();
        return false;
    }
    if (failAfter >= 0) {
        // Simulated death mid-write: part of the new journal is in the
        // temporary file, which is never renamed over the old one.
        file.write(json.left(failAfter));
        file.flush();
        file.cancelWriting();
        error = "Journal write interrupted (simulated)";
        return false;
    }
    if (file.write(json) != json.size() || !file.commit()) {
        error = "Could not write " + journalPath + ": " + file.errorString();
        return false;
    }

    // Committed. Journal audio nothing refers to any more can go.
    const QDir audioDir(dir.filePath(kAudioDir));
    for (const QString& name : audioDir.entryList({"*.wav", "*.part"}, QDir::Files)) {
        if (!snapshot.referenced.contains(name)) {
            QFile::remove(audioDir.filePath(name));
        }
    }
    return true;
}

bool RecoverySession::flushJournal() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return (!busy_ && pending_ == nullptr) || !thread_.joinable(); });
    return lastOk_;
}

QString RecoverySession::lastJournalError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

int RecoverySession::journalsWritten() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return written_;
}

void RecoverySession::setJournalFailAfterBytesForTesting(qint64 bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    failAfterBytes_ = bytes;
}

void RecoverySession::clear() {
    if (!active()) return;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pending_.reset();
        idle_.wait(lock, [this] { return !busy_ || !thread_.joinable(); });
        lastOk_ = true;
        lastError_.clear();
    }
    QDir dir(dir_);
    QFile::remove(dir.filePath(kJournal));
    QFile::remove(dir.filePath(kRecording));
    QDir(dir.filePath(kAudioDir)).removeRecursively();
    for (const QString& take : takes_) {
        QFile::remove(take);
        // An emptied takes/ folder in the project goes too (rmdir only
        // removes it if nothing else is in it).
        QDir().rmdir(QFileInfo(take).absolutePath());
    }
    QDir(dir.filePath(kTakesDir)).removeRecursively();
    takes_.clear();
    committedTakes_.clear();
    audioFiles_.clear();
    writeSessionInfo();
}

void RecoverySession::stopThread() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void RecoverySession::discard() {
    if (!active()) return;
    clear();
    stopThread();
    lock_->unlock();
    lock_.reset();
    QDir(dir_).removeRecursively();
    dir_.clear();
}

void RecoverySession::abandonForTesting() {
    stopThread();
    if (lock_) {
        lock_->unlock();
        lock_.reset();
    }
    dir_.clear();
}

// ---------------------------------------------------------------------------

std::vector<std::unique_ptr<RecoverableSession>> RecoverableSession::find(const QString& root) {
    std::vector<std::unique_ptr<RecoverableSession>> found;
    const QDir rootDir(root);
    for (const QString& name : rootDir.entryList({"session-*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString dir = rootDir.filePath(name);
        auto lock = makeLock(dir);
        if (!lock->tryLock(0)) {
            continue; // its zrecord is still running
        }
        std::unique_ptr<RecoverableSession> session(new RecoverableSession);
        session->dir_ = dir;
        session->lock_ = std::move(lock);
        const QDir d(dir);
        const QJsonObject info = readJson(d.filePath(kSessionInfo));
        session->projectPath_ = info["projectPath"].toString();
        session->takes_ = toStringList(info["takes"]);
        QDateTime newest = QFileInfo(d.filePath(kSessionInfo)).lastModified();

        QStringList committed;
        if (QFile::exists(d.filePath(kJournal))) {
            const QJsonObject journal = readJson(d.filePath(kJournal));
            if (!journal.isEmpty()) {
                session->hasJournal_ = true;
                session->sampleRate_ = journal["sampleRate"].toDouble(44100.0);
                const QJsonArray tracks = journal["tracks"].toArray();
                session->tracks_ = static_cast<int>(tracks.size());
                for (const QJsonValue& t : tracks) session->clips_ += static_cast<int>(t.toObject()["clips"].toArray().size());
                const QJsonObject meta = journal[kMetaKey].toObject();
                committed = toStringList(meta["committedTakes"]);
                if (!meta["projectPath"].toString().isEmpty()) session->projectPath_ = meta["projectPath"].toString();
                newest = std::max(newest, QFileInfo(d.filePath(kJournal)).lastModified());
            }
        }
        const QJsonObject recording = readJson(d.filePath(kRecording));
        if (!recording.isEmpty()) {
            TakeInProgress& take = session->take_;
            take.path = recording["path"].toString();
            take.trackIndex = recording["track"].toInt();
            take.startFrame = recording["startFrame"].toString().toLongLong();
            int channels = recording["channels"].toInt(1);
            const int64_t frames = takeFileFrames(take.path, &channels);
            take.channels = channels;
            if (frames > 0 && !committed.contains(take.path)) {
                session->hasTake_ = true;
                session->takeFrames_ = frames;
                newest = std::max(newest, QFileInfo(take.path).lastModified());
                if (!session->hasJournal_) {
                    std::vector<float> none;
                    int ch = 0, rate = 0;
                    std::string error;
                    if (readTakeFile(take.path.toStdString(), none, ch, rate, error, 0)) session->sampleRate_ = rate;
                }
            }
        }
        session->lastSaved_ = newest;
        if (!session->hasJournal_ && !session->hasTake_) {
            session->discard(); // a crash with nothing unsaved: nothing to offer
            continue;
        }
        found.push_back(std::move(session));
    }
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a->lastSaved() > b->lastSaved(); });
    return found;
}

RecoverableSession::~RecoverableSession() = default;

QString RecoverableSession::summary() const {
    QString name = projectPath_.isEmpty() ? QString("Untitled project") : QFileInfo(projectPath_).completeBaseName();
    QStringList parts;
    if (hasJournal_) {
        parts << QString("%1 track%2").arg(tracks_).arg(tracks_ == 1 ? "" : "s")
              << QString("%1 clip%2").arg(clips_).arg(clips_ == 1 ? "" : "s");
    }
    QString text = name;
    if (!parts.isEmpty()) text += ", " + parts.join(", ");
    if (hasTake_) text += QString(parts.isEmpty() ? ": " : ", and ") + "a partial take of " + formatLength(takeFrames_, sampleRate_);
    return text;
}

bool RecoverableSession::restore(Project& project, std::string& error, int64_t* takeFrames) const {
    if (takeFrames != nullptr) *takeFrames = 0;
    Project loaded;
    const QDir d(dir_);
    if (hasJournal_) {
        QFile file(d.filePath(kJournal));
        if (!file.open(QIODevice::ReadOnly)) {
            error = "Could not open the recovery journal";
            return false;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject() || !ProjectFile::parseManifest(doc.object(), d, loaded, error)) {
            if (error.empty()) error = "The recovery journal is damaged";
            return false;
        }
    }
    if (hasTake_) {
        std::vector<float> samples;
        int channels = 0, rate = 0;
        if (!readTakeFile(take_.path.toStdString(), samples, channels, rate, error)) {
            return false;
        }
        if (!hasJournal_) {
            loaded.sampleRate = rate;
            loaded.channels = channels;
        }
        // A mono take in a stereo project was upmixed when added; do the same.
        if (channels == 1 && loaded.channels == 2) {
            std::vector<float> stereo(samples.size() * 2);
            for (size_t i = 0; i < samples.size(); ++i) stereo[2 * i] = stereo[2 * i + 1] = samples[i];
            samples = std::move(stereo);
        } else if (channels != loaded.channels) {
            error = "The partial take's channel count doesn't match the project";
            return false;
        }
        size_t trackIndex = 0;
        if (take_.trackIndex >= 0 && take_.trackIndex < static_cast<int>(loaded.tracks.size())) {
            trackIndex = static_cast<size_t>(take_.trackIndex);
        } else {
            Track track; // no journal, or it predates the track: give the take its own
            track.name = "Recovered take";
            loaded.tracks.push_back(std::move(track));
            trackIndex = loaded.tracks.size() - 1;
        }
        Track& track = loaded.tracks[trackIndex];
        Clip clip;
        clip.channels = loaded.channels;
        clip.samples = SampleBuffer(samples);
        clip.peaks.build(clip.samples, clip.channels);
        // Where it was being recorded -- after the track's last clip, unless
        // the journal is older than that and something else is there now.
        clip.startFrame = std::max(take_.startFrame, track.endFrame());
        const int64_t frames = clip.frameCount();
        Label label;
        label.startFrame = clip.startFrame;
        label.endFrame = clip.startFrame + frames;
        label.text = "Recovered take";
        track.clips.push_back(std::move(clip));
        loaded.labels.push_back(label);
        std::sort(loaded.labels.begin(), loaded.labels.end(),
                  [](const Label& a, const Label& b) { return a.startFrame < b.startFrame; });
        if (takeFrames != nullptr) *takeFrames = frames;
    }
    ProjectFile::replace(project, loaded);
    return true;
}

void RecoverableSession::discard() {
    for (const QString& take : takes_) {
        QFile::remove(take);
        QDir().rmdir(QFileInfo(take).absolutePath()); // only if now empty
    }
    if (!take_.path.isEmpty()) QFile::remove(take_.path);
    if (lock_) {
        lock_->unlock();
        lock_.reset();
    }
    QDir(dir_).removeRecursively();
}

} // namespace zrecord
