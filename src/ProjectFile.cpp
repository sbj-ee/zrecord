#include "ProjectFile.h"

#include "Resampler.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QUuid>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

#include "AudioFileReader.h"
#include "AudioFileWriter.h"

namespace zrecord {

bool ProjectFile::save(const Project& project, const std::string& folderPath, std::string& errorMessage) {
    QDir dir(QString::fromStdString(folderPath));
    if (!dir.exists() && !dir.mkpath(".")) {
        errorMessage = "Could not create project folder";
        return false;
    }

    // Saving is all-or-nothing. Clip audio goes into a fresh directory that
    // nothing references yet, project.json is replaced atomically
    // (QSaveFile: write a temp file, then rename), and only then are the
    // previous save's audio directories removed. A save that fails anywhere
    // leaves the previous project exactly as it was, and clips deleted since
    // the last save don't linger as stale WAVs.
    const QString audioDirName = "audio-" + QUuid::createUuid().toString(QUuid::Id128).left(12);
    if (!dir.mkdir(audioDirName)) {
        errorMessage = "Could not create the project's audio folder";
        return false;
    }
    auto abandon = [&dir, &audioDirName]() { QDir(dir.filePath(audioDirName)).removeRecursively(); };

    std::lock_guard<std::mutex> lock(project.mutex);

    QJsonObject root;
    root["sampleRate"] = project.sampleRate;
    root["channels"] = project.channels;

    QJsonArray tracksArray;
    for (size_t t = 0; t < project.tracks.size(); ++t) {
        const Track& track = project.tracks[t];
        QJsonObject trackObj;
        trackObj["name"] = QString::fromStdString(track.name);
        trackObj["muted"] = track.muted;
        trackObj["soloed"] = track.soloed;
        trackObj["gainDb"] = track.gainDb;
        QJsonArray envelopeArray;
        for (const EnvelopePoint& point : track.envelope) {
            QJsonObject pointObj;
            pointObj["frame"] = QString::number(point.frame);
            pointObj["gain"] = point.gain;
            envelopeArray.append(pointObj);
        }
        trackObj["envelope"] = envelopeArray;
        trackObj["display"] = track.display == TrackDisplay::Spectrogram ? "spectrogram" : "waveform";
        // The effect stack, in order. Parameters are saved by name, so adding
        // one later (or reordering them in the table) keeps old files valid.
        QJsonArray effectsArray;
        for (const Effect& effect : track.effects) {
            const EffectInfo& info = effectInfo(effect.type);
            QJsonObject effectObj;
            effectObj["type"] = info.key;
            effectObj["bypassed"] = effect.bypassed;
            QJsonObject params;
            for (int p = 0; p < info.paramCount; ++p) {
                params[info.params[p].key] = effect.param(p);
            }
            effectObj["params"] = params;
            effectsArray.append(effectObj);
        }
        trackObj["effects"] = effectsArray;

        QJsonArray clipsArray;
        for (size_t c = 0; c < track.clips.size(); ++c) {
            const Clip& clip = track.clips[c];
            QString relativeFile = QString("%1/track%2_clip%3.wav").arg(audioDirName).arg(t).arg(c);
            QString absoluteFile = dir.filePath(relativeFile);

            // Float, not 24-bit PCM: a project must reopen exactly as saved,
            // and clips can legitimately exceed +/-1.0 (gain, echo) -- only
            // the final mixdown is clamped. Older projects saved as 24-bit
            // still load; the reader takes whatever the file holds.
            if (!AudioFileWriter::writeFloatWav(absoluteFile.toStdString(), clip.samples,
                                                 static_cast<int>(project.sampleRate), clip.channels,
                                                 errorMessage)) {
                abandon();
                return false;
            }

            QJsonObject clipObj;
            clipObj["file"] = relativeFile;
            clipObj["startFrame"] = QString::number(clip.startFrame);
            clipsArray.append(clipObj);
        }
        trackObj["clips"] = clipsArray;
        tracksArray.append(trackObj);
    }
    root["tracks"] = tracksArray;

    QJsonArray labelsArray;
    for (const Label& label : project.labels) {
        QJsonObject labelObj;
        labelObj["text"] = QString::fromStdString(label.text);
        // Frame counts can exceed what a JSON double represents exactly, so
        // they're written as strings here and in the clip entries above.
        labelObj["startFrame"] = QString::number(label.startFrame);
        labelObj["endFrame"] = QString::number(label.endFrame);
        labelsArray.append(labelObj);
    }
    root["labels"] = labelsArray;

    const QByteArray json = QJsonDocument(root).toJson();
    QSaveFile jsonFile(dir.filePath("project.json"));
    if (!jsonFile.open(QIODevice::WriteOnly) || jsonFile.write(json) != json.size() || !jsonFile.commit()) {
        errorMessage = "Could not write project.json: " + jsonFile.errorString().toStdString();
        abandon();
        return false;
    }

    // Committed: the previous save's audio (and the pre-0.2 "audio" folder)
    // is now unreferenced. Only our own folder names are touched.
    const QStringList audioDirs = dir.entryList({"audio", "audio-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& name : audioDirs) {
        if (name != audioDirName) {
            QDir(dir.filePath(name)).removeRecursively();
        }
    }
    return true;
}

bool ProjectFile::load(Project& project, const std::string& folderPath, std::string& errorMessage) {
    QDir dir(QString::fromStdString(folderPath));
    QFile jsonFile(dir.filePath("project.json"));
    if (!jsonFile.open(QIODevice::ReadOnly)) {
        errorMessage = "Could not open project.json";
        return false;
    }
    QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
    if (!doc.isObject()) {
        errorMessage = "Invalid project file";
        return false;
    }
    QJsonObject root = doc.object();

    // Parse everything into a scratch project first. A missing or unreadable
    // clip used to abort half-way through, leaving `project` wiped (and the
    // caller's undo history pointing at tracks that no longer existed).
    Project loaded;
    loaded.sampleRate = root["sampleRate"].toDouble(44100.0);
    loaded.channels = root["channels"].toInt(2);

    for (const QJsonValue& trackValue : root["tracks"].toArray()) {
        QJsonObject trackObj = trackValue.toObject();
        Track track;
        track.name = trackObj["name"].toString().toStdString();
        track.muted = trackObj["muted"].toBool();
        track.soloed = trackObj["soloed"].toBool();
        track.gainDb = trackObj["gainDb"].toDouble();
        for (const QJsonValue& pointValue : trackObj["envelope"].toArray()) {
            QJsonObject pointObj = pointValue.toObject();
            EnvelopePoint point;
            point.frame = pointObj["frame"].toString().toLongLong();
            point.gain = static_cast<float>(pointObj["gain"].toDouble(1.0));
            track.envelope.push_back(point);
        }
        std::sort(track.envelope.begin(), track.envelope.end(),
                  [](const EnvelopePoint& a, const EnvelopePoint& b) { return a.frame < b.frame; });
        track.display = trackObj["display"].toString() == "spectrogram" ? TrackDisplay::Spectrogram
                                                                        : TrackDisplay::Waveform;
        // Projects from before track effects have no "effects": an empty
        // stack. An effect this version doesn't know is skipped (the rest of
        // the stack still loads); a missing parameter takes its default.
        for (const QJsonValue& effectValue : trackObj["effects"].toArray()) {
            const QJsonObject effectObj = effectValue.toObject();
            EffectType type;
            if (!effectTypeFromKey(effectObj["type"].toString().toStdString(), type)) {
                continue;
            }
            Effect effect = Effect::make(type);
            effect.bypassed = effectObj["bypassed"].toBool(false);
            const QJsonObject params = effectObj["params"].toObject();
            const EffectInfo& info = effectInfo(type);
            for (int p = 0; p < info.paramCount; ++p) {
                effect.params[static_cast<size_t>(p)] = params[info.params[p].key].toDouble(info.params[p].defaultValue);
            }
            effect.clampParams();
            track.effects.push_back(effect);
        }

        for (const QJsonValue& clipValue : trackObj["clips"].toArray()) {
            QJsonObject clipObj = clipValue.toObject();
            QString relativeFile = clipObj["file"].toString();
            int64_t startFrame = clipObj["startFrame"].toString().toLongLong();

            Clip clip;
            clip.startFrame = startFrame;
            int sampleRate = 0;
            std::vector<float> samples;
            if (!AudioFileReader::read(dir.filePath(relativeFile).toStdString(), samples,
                                        sampleRate, clip.channels, errorMessage)) {
                return false;
            }
            if (sampleRate != static_cast<int>(loaded.sampleRate)) {
                // zrecord always writes clips at the project rate; a clip that
                // isn't (hand-assembled project) would play at the wrong speed.
                std::vector<float> converted;
                if (!Resampler::convert(samples, clip.channels, sampleRate, loaded.sampleRate, converted,
                                        errorMessage)) {
                    return false;
                }
                samples = std::move(converted);
            }
            clip.samples = SampleBuffer(samples);
            clip.peaks.build(clip.samples, clip.channels);
            track.clips.push_back(std::move(clip));
        }
        std::sort(track.clips.begin(), track.clips.end(),
                  [](const Clip& a, const Clip& b) { return a.startFrame < b.startFrame; });
        loaded.tracks.push_back(std::move(track));
    }

    for (const QJsonValue& labelValue : root["labels"].toArray()) {
        QJsonObject labelObj = labelValue.toObject();
        Label label;
        label.text = labelObj["text"].toString().toStdString();
        label.startFrame = labelObj["startFrame"].toString().toLongLong();
        label.endFrame = labelObj["endFrame"].toString().toLongLong();
        loaded.labels.push_back(std::move(label));
    }
    std::sort(loaded.labels.begin(), loaded.labels.end(),
              [](const Label& a, const Label& b) { return a.startFrame < b.startFrame; });

    // Everything parsed and every clip read: only now replace the target.
    std::lock_guard<std::mutex> lock(project.mutex);
    if (loaded.channels != project.channels) {
        // The clipboard is interleaved at the old width; pasting it into a
        // project with a different channel count would scramble it.
        project.clipboard.clear();
    }
    project.sampleRate = loaded.sampleRate;
    project.channels = loaded.channels;
    project.tracks = std::move(loaded.tracks);
    project.labels = std::move(loaded.labels);
    project.selection.clear();
    project.playheadFrame = 0;
    return true;
}

} // namespace zrecord
