// Listing a folder with only the calls that work on the PS4 (see torrent/posix_tree.hpp: OpenOrbis
// musl's fstatat is an ENOSYS stub, so std::filesystem's directory_iterator, is_directory,
// file_size and last_write_time fail there). Names come from opendir/readdir; a name is a folder
// if it opens as one and reads without error, else a file whose size is lseek(SEEK_END) on an
// open fd. No modification time: no stat call that works there fills one in a known layout (0
// keeps the "Date" sort stable). Builds and behaves the same on Linux, which is how it is tested.
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "torrent/posix_tree.hpp"

namespace remote {
namespace posix_list {

struct Entry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
};

/// True when `path` opens as a folder and reads to its first entry (or its end) without an error:
/// on the PS4 a file can pass opendir, and then its first readdir fails.
inline bool isFolder(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    errno = 0;
    const struct dirent* e = readdir(d);
    bool ok = e != nullptr || errno == 0;
    closedir(d);
    return ok;
}

/// A file's size in bytes, 0 when it can't be opened.
inline uint64_t fileSize(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    off_t end = lseek(fd, 0, SEEK_END);
    close(fd);
    return end > 0 ? (uint64_t)end : 0;
}

/// The entries of `dir` ("." and ".." left out). Never throws: a folder that can't be read lists
/// what was read before the failure, and `error` says why ("" when it all read).
inline std::vector<Entry> list(const std::string& dir, std::string& error) {
    std::vector<std::string> names;
    torrent::listFolder(dir, names, error);
    std::vector<Entry> out;
    out.reserve(names.size());
    const std::string base = dir.empty() || dir.back() == '/' ? dir : dir + "/";
    for (auto& n : names) {
        Entry e;
        e.name = n;
        std::string full = base + n;
        e.dir = isFolder(full);
        if (!e.dir) e.size = fileSize(full);
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace posix_list
}  // namespace remote
