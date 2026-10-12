// Deleting and listing folders with only the calls that work on the PS4.
//
// The PS4 build links OpenOrbis musl (scripts/ps4/musl), whose fstatat and unlinkat are stubs
// that fail with ENOSYS, and its lstat is musl's fstatat(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW)
// wrapper, so lstat fails there too. That is also why libc++'s std::filesystem::remove_all
// fails on the console. What the console has: unlink and rmdir straight from the PS4's libkernel
// (libtorrent deletes files there with unlink), open, which passes the FreeBSD O_DIRECTORY and
// O_NOFOLLOW flags through, and musl's opendir/readdir/closedir on libkernel's getdents with the
// FreeBSD dirent layout (the engine's sweep lists folders there). So nothing here calls the stat family:
// removeTree tries unlink first, which removes a file or a link and never follows the link, and
// only a path that opens as a folder without following a link gets emptied and removed.
//
// Errors read "<call> <path>: <strerror>", so a console log names the call that failed.
#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

namespace torrent {

namespace posix_tree_detail {

inline std::string describe(const char* call, const std::string& path, int err) {
    return std::string(call) + " " + path + ": " + std::strerror(err);
}

// FreeBSD's UFS hands out deleted directory entries with inode 0. Its libc skips them, musl's
// readdir doesn't. (The OpenOrbis header names the field d_fileno, then #defines d_fileno to
// d_ino, a field it doesn't have.)
inline bool isDeletedEntry(const struct dirent* entry) {
#ifdef __PS4__
#pragma push_macro("d_fileno")
#undef d_fileno
    return entry->d_fileno == 0;
#pragma pop_macro("d_fileno")
#else
    return entry->d_ino == 0;
#endif
}

}  // namespace posix_tree_detail

/// Lists the names in `path` (no "." and ".."). False, with `error` filled, if it can't be
/// opened or read to the end (the names read until then are kept).
inline bool listFolder(const std::string& path, std::vector<std::string>& names, std::string& error) {
    using posix_tree_detail::describe;
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        error = describe("opendir", path, errno);
        return false;
    }
    bool ok = true;
    for (;;) {
        errno = 0;
        const struct dirent* entry = readdir(dir);
        if (!entry) {
            // NULL with errno unchanged is the end. On the PS4, musl's readdir reports any
            // getdents failure as EPERM.
            if (errno != 0) {
                error = describe("readdir", path, errno);
                ok = false;
            }
            break;
        }
        if (posix_tree_detail::isDeletedEntry(entry)) continue;
        const std::string name = entry->d_name;
        if (name != "." && name != "..") names.push_back(name);
    }
    closedir(dir);
    return ok;
}

/// Deletes a file or a folder tree. Never follows a symlink: a link is removed, not what it
/// points at. True when the path no longer exists (including when it never did). On failure
/// `error` holds the first failing call with its path, and everything else that can go still goes.
inline bool removeTree(const std::string& path, std::string& error) {
    using posix_tree_detail::describe;
    auto fail = [&](const char* call, const std::string& p, int err) {
        if (error.empty()) error = describe(call, p, err);
        return false;
    };

    // A folder answers EPERM here on the PS4 and FreeBSD, EISDIR on Linux. Whatever the answer,
    // the open below decides whether it is a folder.
    if (unlink(path.c_str()) == 0 || errno == ENOENT) return true;
    const int unlinkError = errno;

    // A folder only if it opens as one without following a link, so a link that couldn't be
    // unlinked is never entered. opendir then reopens the path: the engine's temp folder is its
    // own, nothing swaps a folder for a link in between.
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        const int openError = errno;
        if (openError == ENOENT) return true;  // gone meanwhile
        // a file (ENOTDIR) or a link (ELOOP on Linux, EMLINK on FreeBSD): unlink's answer stands
        if (openError == ENOTDIR || openError == ELOOP || openError == EMLINK)
            return fail("unlink", path, unlinkError);
        return fail("open", path, openError);
    }
    close(fd);

    bool ok = true;
    std::vector<std::string> names;
    std::string listError;
    if (!listFolder(path, names, listError)) {
        if (error.empty()) error = listError;
        ok = false;
    }
    for (const auto& name : names)
        if (!removeTree(path + "/" + name, error)) ok = false;
    if (rmdir(path.c_str()) != 0 && errno != ENOENT) ok = fail("rmdir", path, errno);
    return ok;
}

}  // namespace torrent
