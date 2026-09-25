#pragma once

#include <string>

#include "Project.h"

namespace zrecord {

// Saves/loads a Project as a folder (conventionally named "*.zrproj"):
// a project.json manifest plus one WAV file per clip under audio/. Mirrors
// Audacity's own approach of metadata + referenced audio blocks, without a
// custom binary container.
class ProjectFile {
public:
    static bool save(const Project& project, const std::string& folderPath, std::string& errorMessage);
    // All-or-nothing: on failure `project` is left exactly as it was.
    static bool load(Project& project, const std::string& folderPath, std::string& errorMessage);
};

} // namespace zrecord
