// Standalone logic test — file type from its name.
//
//   c++ -std=gnu++17 -arch x86_64 -Iapp/include tests/test_dir_type.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <client/dir_entry.hpp>

using remote::EntryType;
using remote::typeFromName;

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

int main() {
    CHECK(typeFromName("clip.mp4") == EntryType::VIDEO);
    CHECK(typeFromName("Movie.2020.1080p.MKV") == EntryType::VIDEO);
    CHECK(typeFromName("tone.flac") == EntryType::AUDIO);
    CHECK(typeFromName("poster.JPG") == EntryType::IMAGE);
    CHECK(typeFromName("movie.fr.srt") == EntryType::SUBTITLE);
    CHECK(typeFromName("list.m3u8") == EntryType::PLAYLIST);

    // no known extension: a folder in an FTP listing without MLSD
    CHECK(typeFromName("Movies") == EntryType::FILE);
    CHECK(typeFromName("TV Shows") == EntryType::FILE);
    CHECK(typeFromName("Show.S01.1080p") == EntryType::FILE);
    CHECK(typeFromName("readme.txt") == EntryType::FILE);
    CHECK(typeFromName("") == EntryType::FILE);

    if (failures == 0) {
        printf("test_dir_type: OK\n");
        return 0;
    }
    printf("test_dir_type: %d FAILURE(S)\n", failures);
    return 1;
}
