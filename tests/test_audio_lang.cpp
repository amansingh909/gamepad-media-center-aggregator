// Standalone logic test: the audio language setting as mpv's alang list (2-letter and 3-letter
// codes). langs.cpp has no borealis or curl in it, so the test compiles it in.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_audio_lang.cpp -o /tmp/t && /tmp/t
//   in Docker, from the repo root: docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src
//   streamio/native-builder:ubuntu-24.04 bash -c '<the command above>'

#include <cstdio>
#include <string>
#include "../app/src/api/media/langs.cpp"

static int failures = 0;
#define CHECK_EQ(got, want)                                                                         \
    do {                                                                                            \
        std::string g = (got), w = (want);                                                          \
        if (g != w) {                                                                               \
            printf("FAIL: line %d\n  got:  %s\n  want: %s\n", __LINE__, g.c_str(), w.c_str());      \
            ++failures;                                                                             \
        }                                                                                           \
    } while (0)

int main() {
    using media::audioLangList;
    CHECK_EQ(audioLangList("en", "fr-FR"), "en,eng");
    CHECK_EQ(audioLangList("auto", "en-US"), "en,eng");
    CHECK_EQ(audioLangList("auto", "fr"), "fr,fre,fra");
    CHECK_EQ(audioLangList("de", "en-US"), "de,ger,deu");
    CHECK_EQ(audioLangList("auto", "zh-Hans"), "zh,chi,zho,zht,zhe");
    CHECK_EQ(audioLangList("pt", "en-US"), "pt,por,pob");
    CHECK_EQ(audioLangList("ja", "en-US"), "ja,jpn");
    CHECK_EQ(audioLangList("auto", ""), "");
    CHECK_EQ(audioLangList("xx", "en-US"), "");
    // "File default": no alang at all, whatever the app language is
    CHECK_EQ(audioLangList("file", "en-US"), "");
    CHECK_EQ(audioLangList("file", "fr"), "");
    CHECK_EQ(audioLangList("file", ""), "");
    if (failures == 0) printf("test_audio_lang: all passed\n");
    return failures ? 1 : 0;
}
