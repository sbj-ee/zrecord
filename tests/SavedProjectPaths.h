#pragma once

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

// Where a saved project keeps a given clip's audio. Each save writes into a
// fresh audio folder, so tests look the path up instead of assuming one.
inline QString savedClipFile(const QString& projectPath, int track = 0, int clip = 0) {
    QFile json(projectPath + "/project.json");
    if (!json.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QJsonObject root = QJsonDocument::fromJson(json.readAll()).object();
    const QString file = root["tracks"].toArray()[track].toObject()["clips"].toArray()[clip].toObject()["file"].toString();
    return file.isEmpty() ? QString() : projectPath + "/" + file;
}
