// Standalone logic test: what the torrent engine may delete in its temp folder, which
// temp folders it accepts, and the POSIX tree removal and listing it uses (the PS4's
// libc has no working lstat, so no std::filesystem::remove_all either).
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_torrent_folders.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <torrent/posix_tree.hpp>
#include <torrent/temp_folder.hpp>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: line %d: %s\n", __LINE__, #cond);      \
            ++failures;                                          \
        }                                                        \
    } while (0)

// The PS4 build's libc (OpenOrbis musl) has fstatat and unlinkat as stubs that fail with
// ENOSYS, and its lstat goes through that fstatat, and the PS4's FreeBSD kernel answers unlink
// on a folder with EPERM where Linux says EISDIR. While `ps4Libc` is set these calls behave
// like that here too, so the tree code is held to what works on the console. `refuseUnlink`
// makes unlink fail with EPERM for one path.
static bool ps4Libc = false;
static std::string refuseUnlink;

template <class F>
static F nextSymbol(const char* name) {
    return reinterpret_cast<F>(dlsym(RTLD_NEXT, name));
}

extern "C" int lstat(const char* path, struct stat* st) noexcept {
    if (ps4Libc) {
        errno = ENOSYS;
        return -1;
    }
    return nextSymbol<int (*)(const char*, struct stat*)>("lstat")(path, st);
}

extern "C" int fstatat(int fd, const char* path, struct stat* st, int flags) noexcept {
    if (ps4Libc) {
        errno = ENOSYS;
        return -1;
    }
    return nextSymbol<int (*)(int, const char*, struct stat*, int)>("fstatat")(fd, path, st, flags);
}

extern "C" int unlinkat(int fd, const char* path, int flags) noexcept {
    if (ps4Libc) {
        errno = ENOSYS;
        return -1;
    }
    return nextSymbol<int (*)(int, const char*, int)>("unlinkat")(fd, path, flags);
}

extern "C" int unlink(const char* path) noexcept {
    if (ps4Libc) {
        struct stat st;
        const auto realFstatat = nextSymbol<int (*)(int, const char*, struct stat*, int)>("fstatat");
        if (path == refuseUnlink ||
            (realFstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode))) {
            errno = EPERM;
            return -1;
        }
    }
    return nextSymbol<int (*)(const char*)>("unlink")(path);
}

template <class F>
static bool withPs4Libc(F f) {
    ps4Libc = true;
    const bool result = f();
    ps4Libc = false;
    return result;
}

static void touch(const std::string& path) {
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fputs("data", f);
        fclose(f);
    }
}

static bool exists(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

static void testTree() {
    using namespace torrent;
    char tmpl[] = "/tmp/test_torrent_folders.XXXXXX";
    char* base = mkdtemp(tmpl);
    CHECK(base != nullptr);
    if (!base) return;
    const std::string root = base;

    // a file outside the tree that a symlink inside it points at
    const std::string outside = root + "/outside.txt";
    touch(outside);
    const std::string outsideDir = root + "/outside-dir";
    mkdir(outsideDir.c_str(), 0755);
    touch(outsideDir + "/keep.txt");

    const std::string tree = root + "/tree";
    mkdir(tree.c_str(), 0755);
    touch(tree + "/a.txt");
    mkdir((tree + "/sub").c_str(), 0755);
    touch(tree + "/sub/b.txt");
    mkdir((tree + "/sub/deeper").c_str(), 0755);
    touch(tree + "/sub/deeper/c.txt");
    mkdir((tree + "/empty").c_str(), 0755);
    CHECK(symlink(outside.c_str(), (tree + "/link-file").c_str()) == 0);
    CHECK(symlink(outsideDir.c_str(), (tree + "/sub/link-dir").c_str()) == 0);

    // listFolder: exactly the entries, no "." or ".."
    std::vector<std::string> names;
    std::string error;
    CHECK(listFolder(tree, names, error));
    std::sort(names.begin(), names.end());
    CHECK((names == std::vector<std::string>{"a.txt", "empty", "link-file", "sub"}));
    CHECK(error.empty());

    // listFolder on a missing folder fails with a message naming the call and the path
    names.clear();
    CHECK(!listFolder(root + "/nope", names, error));
    CHECK(error == "opendir " + root + "/nope: " + strerror(ENOENT));
    CHECK(names.empty());

    // removeTree: the whole tree goes, what the symlinks point at stays
    error.clear();
    CHECK(removeTree(tree, error));
    CHECK(error.empty());
    CHECK(!exists(tree));
    CHECK(exists(outside));
    CHECK(exists(outsideDir + "/keep.txt"));

    // a missing path is already gone
    CHECK(removeTree(tree, error));
    CHECK(removeTree(root + "/never/was/here", error));

    // a single file, and a symlink on its own
    touch(root + "/single.txt");
    CHECK(removeTree(root + "/single.txt", error));
    CHECK(!exists(root + "/single.txt"));
    CHECK(symlink(outside.c_str(), (root + "/solo-link").c_str()) == 0);
    CHECK(removeTree(root + "/solo-link", error));
    CHECK(!exists(root + "/solo-link"));
    CHECK(exists(outside));

    // a folder that can't be emptied: false, the error names the path, the rest still goes
    const std::string locked = root + "/locked";
    mkdir(locked.c_str(), 0755);
    mkdir((locked + "/inner").c_str(), 0755);
    touch(locked + "/inner/x.txt");
    touch(locked + "/y.txt");
    chmod((locked + "/inner").c_str(), 0555);  // no write: x.txt can't be unlinked
    if (geteuid() != 0) {
        error.clear();
        CHECK(!removeTree(locked, error));
        CHECK(error == "unlink " + locked + "/inner/x.txt: " + strerror(EACCES));  // the first failure
        CHECK(!exists(locked + "/y.txt"));  // kept deleting what it could
        chmod((locked + "/inner").c_str(), 0755);
    }
    std::string ignore;
    removeTree(locked, ignore);

    removeTree(root, ignore);
    CHECK(!exists(root));
}

// The same work with the PS4's libc and kernel answers (see ps4Libc above).
static void testTreeWithPs4Libc() {
    using namespace torrent;
    char tmpl[] = "/tmp/test_torrent_folders_ps4.XXXXXX";
    char* base = mkdtemp(tmpl);
    CHECK(base != nullptr);
    if (!base) return;
    const std::string root = base;

    const std::string outside = root + "/outside.txt";
    touch(outside);
    const std::string outsideDir = root + "/outside-dir";
    mkdir(outsideDir.c_str(), 0755);
    touch(outsideDir + "/keep.txt");

    const std::string tree = root + "/tree";
    mkdir(tree.c_str(), 0755);
    touch(tree + "/a.txt");
    mkdir((tree + "/sub").c_str(), 0755);
    touch(tree + "/sub/b.txt");
    mkdir((tree + "/sub/deeper").c_str(), 0755);
    touch(tree + "/sub/deeper/c.txt");
    mkdir((tree + "/empty").c_str(), 0755);
    CHECK(symlink(outside.c_str(), (tree + "/link-file").c_str()) == 0);
    CHECK(symlink(outsideDir.c_str(), (tree + "/sub/link-dir").c_str()) == 0);

    std::vector<std::string> names;
    std::string error;
    CHECK(withPs4Libc([&] { return listFolder(tree, names, error); }));
    std::sort(names.begin(), names.end());
    CHECK((names == std::vector<std::string>{"a.txt", "empty", "link-file", "sub"}));

    // folders are found by unlink's EPERM, never by a stat call
    error.clear();
    CHECK(withPs4Libc([&] { return removeTree(tree, error); }));
    CHECK(error.empty());
    CHECK(!exists(tree));
    CHECK(exists(outside));
    CHECK(exists(outsideDir + "/keep.txt"));
    CHECK(withPs4Libc([&] { return removeTree(root + "/never/was/here", error); }));
    CHECK(error.empty());

    // a link that can't be unlinked is never entered: what it points at stays
    const std::string held = root + "/held";
    mkdir(held.c_str(), 0755);
    CHECK(symlink(outsideDir.c_str(), (held + "/link-dir").c_str()) == 0);
    refuseUnlink = held + "/link-dir";
    CHECK(!withPs4Libc([&] { return removeTree(held, error); }));
    refuseUnlink.clear();
    CHECK(error == "unlink " + held + "/link-dir: " + strerror(EPERM));
    CHECK(exists(held + "/link-dir"));
    CHECK(exists(outsideDir + "/keep.txt"));

    std::string ignore;
    removeTree(root, ignore);
    CHECK(!exists(root));
}

int main() {
    using namespace torrent;
    testTree();
    testTreeWithPs4Libc();

    const std::string hash = "01c137287d6f0ed05a56742dae794f632c79ff3d";

    // the engine's own save folders: <info hash>-<n>
    CHECK(isEngineFolderName(hash + "-1"));
    CHECK(isEngineFolderName(hash + "-12345"));
    CHECK(isEngineFolderName("0000000000000000000000000000000000000002-7"));

    // anything else stays
    CHECK(!isEngineFolderName(""));
    CHECK(!isEngineFolderName("."));
    CHECK(!isEngineFolderName(".."));
    CHECK(!isEngineFolderName("Movies"));
    CHECK(!isEngineFolderName("config.json"));
    CHECK(!isEngineFolderName(hash));                // no counter
    CHECK(!isEngineFolderName(hash + "-"));          // empty counter
    CHECK(!isEngineFolderName(hash + "-1a"));        // counter not all digits
    CHECK(!isEngineFolderName(hash + "-1 "));
    CHECK(!isEngineFolderName(hash + "-1.parts"));
    CHECK(!isEngineFolderName(hash + "_1"));
    CHECK(!isEngineFolderName(hash + "--1"));
    CHECK(!isEngineFolderName(hash.substr(1) + "-1"));  // 39 hex characters
    CHECK(!isEngineFolderName(hash + "0-1"));           // 41
    CHECK(!isEngineFolderName("01C137287D6F0ED05A56742DAE794F632C79FF3D-1"));  // the engine writes lowercase
    CHECK(!isEngineFolderName("g1c137287d6f0ed05a56742dae794f632c79ff3d-1"));  // not hex
    CHECK(!isEngineFolderName("-1"));

    // the engine only works in an absolute folder
    CHECK(isUsableTempDir("/tmp/torrent-serve"));
    CHECK(isUsableTempDir("/data/GMCA/torrents"));
    CHECK(!isUsableTempDir(""));
    CHECK(!isUsableTempDir("torrents"));
    CHECK(!isUsableTempDir("./torrents"));
    CHECK(!isUsableTempDir("../torrents"));

    if (failures == 0) printf("test_torrent_folders: all passed\n");
    return failures ? 1 : 0;
}
