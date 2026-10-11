// Deleting and listing folders with plain POSIX calls. The PS4's libc and kernel don't
// provide what libc++'s std::filesystem::remove_all needs (it fails with ENOSYS), and
// directory_iterator is untested there, so the engine's delete and list paths use only
// lstat, opendir/readdir/closedir, unlink and rmdir.
#pragma once

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace torrent {

/// Lists the names in `path` (no "." and ".."). False, with `error` filled, if it can't be opened.
inline bool listFolder(const std::string& path, std::vector<std::string>& names, std::string& error) {
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        error = path + ": " + strerror(errno);
        return false;
    }
    while (struct dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    closedir(dir);
    return true;
}

/// Deletes a file or a folder tree. Never follows a symlink: a link is removed, not what it
/// points at. True when the path no longer exists (including when it never did). On failure
/// `error` holds the first strerror and path, and everything else that can go still goes.
inline bool removeTree(const std::string& path, std::string& error) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        if (error.empty()) error = path + ": " + strerror(errno);
        return false;
    }
    bool ok = true;
    auto fail = [&](const std::string& what) {
        if (error.empty()) error = what + ": " + strerror(errno);
        ok = false;
    };
    if (S_ISDIR(st.st_mode)) {
        std::vector<std::string> names;
        std::string listError;
        if (listFolder(path, names, listError)) {
            for (const auto& name : names)
                if (!removeTree(path + "/" + name, error)) ok = false;
        } else {
            if (error.empty()) error = listError;
            ok = false;
        }
        if (rmdir(path.c_str()) != 0 && errno != ENOENT) fail(path);
    } else if (unlink(path.c_str()) != 0 && errno != ENOENT) {
        fail(path);
    }
    return ok;
}

}  // namespace torrent
