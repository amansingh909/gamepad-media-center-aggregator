// What the torrent engine may delete in its temp folder: only the save folders it
// creates there (<info hash>-<n>), so a wrong folder setting can't take the user's
// files with it.
#pragma once

#include <filesystem>
#include <string>

namespace torrent {

/// True for the engine's own save folder names: 40 lowercase hex characters, a dash
/// and one or more digits.
inline bool isEngineFolderName(const std::string& name) {
    const size_t hashLength = 40;
    if (name.size() < hashLength + 2 || name[hashLength] != '-') return false;
    for (size_t i = 0; i < hashLength; ++i) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    for (size_t i = hashLength + 1; i < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

/// The engine only works in an absolute folder: an empty or relative one would point
/// wherever the current directory happens to be.
inline bool isUsableTempDir(const std::string& dir) { return !dir.empty() && std::filesystem::path(dir).is_absolute(); }

}  // namespace torrent
