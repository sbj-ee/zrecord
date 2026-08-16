#include "ProjectFile.h"

#include <QDir>
#include <QFile>
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
    QString audioDirPath = dir.filePath("audio");
    QDir().mkpath(audioDirPath);

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

        QJsonArray clipsArray;
        for (size_t c = 0; c < track.clips.size(); ++c) {
            const Clip& clip = track.clips[c];
            QString relativeFile = QString("audio/track%1_clip%2.wav").arg(t).arg(c);
            QString absoluteFile = dir.filePath(relativeFile);

            if (!AudioFileWriter::write(absoluteFile.toStdString(), clip.samples,
                                         static_cast<int>(project.sampleRate), clip.channels,
                                         AudioFormat::Wav, errorMessage)) {
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

    QFile jsonFile(dir.filePath("project.json"));
    if (!jsonFile.open(QIODevice::WriteOnly)) {
        errorMessage = "Could not write project.json";
        return false;
    }
    jsonFile.write(QJsonDocument(root).toJson());
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

    std::lock_guard<std::mutex> lock(project.mutex);
    project.sampleRate = root["sampleRate"].toDouble(44100.0);
    project.channels = root["channels"].toInt(2);
    project.tracks.clear();
    project.selection.clear();
    project.playheadFrame = 0;

    for (const QJsonValue& trackValue : root["tracks"].toArray()) {
        QJsonObject trackObj = trackValue.toObject();
        Track track;
        track.name = trackObj["name"].toString().toStdString();
        track.muted = trackObj["muted"].toBool();
        track.soloed = trackObj["soloed"].toBool();
        track.gainDb = trackObj["gainDb"].toDouble();

        for (const QJsonValue& clipValue : trackObj["clips"].toArray()) {
            QJsonObject clipObj = clipValue.toObject();
            QString relativeFile = clipObj["file"].toString();
            int64_t startFrame = clipObj["startFrame"].toString().toLongLong();

            Clip clip;
            clip.startFrame = startFrame;
            int sampleRate = 0;
            if (!AudioFileReader::read(dir.filePath(relativeFile).toStdString(), clip.samples,
                                        sampleRate, clip.channels, errorMessage)) {
                return false;
            }
            clip.peaks.build(clip.samples, clip.channels);
            track.clips.push_back(std::move(clip));
        }
        std::sort(track.clips.begin(), track.clips.end(),
                  [](const Clip& a, const Clip& b) { return a.startFrame < b.startFrame; });
        project.tracks.push_back(std::move(track));
    }
    return true;
}

} // namespace zrecord
