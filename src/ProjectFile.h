#pragma once

#include <QDir>
#include <QJsonObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "Project.h"

namespace zrecord {

// Where each clip's audio lives on disk, keyed by the clip's
// SampleBuffer::contentId(): lets the autosave journal refer to audio that
// is already in a file instead of writing it again.
using ClipFileMap = std::map<uint64_t, QString>;

// Saves/loads a Project as a folder (conventionally named "*.zrproj"):
// a project.json manifest plus one WAV file per clip under audio/. Mirrors
// Audacity's own approach of metadata + referenced audio blocks, without a
// custom binary container.
class ProjectFile {
public:
    // `savedFiles`, if given, gets the absolute path each clip was written to.
    static bool save(const Project& project, const std::string& folderPath, std::string& errorMessage,
                     ClipFileMap* savedFiles = nullptr);
    // All-or-nothing: on failure `project` is left exactly as it was.
    // `loadedFiles`, if given, gets the file each clip was read from (clips
    // that had to be resampled are left out: their audio isn't the file's).
    static bool load(Project& project, const std::string& folderPath, std::string& errorMessage,
                     ClipFileMap* loadedFiles = nullptr);

    // The manifest (project.json's contents) for `project`, with each clip's
    // "file" entry supplied by `clipFile(trackIndex, clipIndex, clip)`.
    // Doesn't lock the project: the caller holds its mutex, or is the UI
    // thread (the only one that changes it).
    using ClipFileNamer = std::function<QString(size_t, size_t, const Clip&)>;
    static QJsonObject manifest(const Project& project, const ClipFileNamer& clipFile);
    // Parses a manifest, reading each clip's audio from its "file" (relative
    // to `base`, or absolute), into `loaded`, which must be empty.
    static bool parseManifest(const QJsonObject& root, const QDir& base, Project& loaded, std::string& errorMessage,
                              ClipFileMap* loadedFiles = nullptr);
    // Moves a parsed project into `project` (under its mutex), the way load() does.
    static void replace(Project& project, Project& loaded);
};

} // namespace zrecord
