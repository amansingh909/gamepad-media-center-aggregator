// Standalone logic test: subtitle language codes, labels and the preferred track. A regional
// variant the addons list apart (Brazilian Portuguese "pob" next to Portuguese "por", Latin
// American Spanish, Traditional Chinese...) has its own code, so a Stremio subtitle list keeps
// each: it keeps one track per code. langs.cpp has no borealis or curl in it, so the test
// compiles it in.
//
//   c++ -std=gnu++17 -Wall -Iapp/include tests/test_subtitle_lang.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <set>
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

static std::string pick(const std::vector<std::string>& tags, const std::string& pref) {
    const size_t i = media::preferredSubtitle(tags, pref);
    return i == std::string::npos ? "none" : std::to_string(i);
}

int main() {
    using media::subtitleLangCode;
    using media::subtitleLangDisplay;

    // the codes OpenSubtitles and other addons send for each
    for (const char* raw : {"pob", "pt-br", "PT-BR", "pt-BR", "ptbr"}) CHECK_EQ(subtitleLangCode(raw), "pt-BR");
    for (const char* raw : {"por", "pt", "pt-pt", "Portuguese"}) CHECK_EQ(subtitleLangCode(raw), "pt");
    CHECK_EQ(subtitleLangDisplay("pob"), "Português (Brasil)");
    CHECK_EQ(subtitleLangDisplay("por"), "Português");
    // the other variants OpenSubtitles (export_languages.php) and other addons tell apart
    CHECK_EQ(subtitleLangCode("spa"), "es");
    for (const char* raw : {"spn", "es-es", "es-ES"}) CHECK_EQ(subtitleLangCode(raw), "es-ES");
    for (const char* raw : {"spl", "es-419", "es-la", "es-mx"}) CHECK_EQ(subtitleLangCode(raw), "es-419");
    CHECK_EQ(subtitleLangDisplay("spn"), "Español (España)");
    CHECK_EQ(subtitleLangDisplay("spl"), "Español (Latinoamérica)");
    for (const char* raw : {"fre", "fra", "fr"}) CHECK_EQ(subtitleLangCode(raw), "fr");
    CHECK_EQ(subtitleLangCode("fr-ca"), "fr-CA");
    CHECK_EQ(subtitleLangDisplay("fr-CA"), "Français (Canada)");
    for (const char* raw : {"chi", "zho", "zh-cn", "zh-Hans"}) CHECK_EQ(subtitleLangCode(raw), "zh");
    for (const char* raw : {"zht", "zh-tw", "zh-hk", "zh-Hant"}) CHECK_EQ(subtitleLangCode(raw), "zh-Hant");
    CHECK_EQ(subtitleLangCode("zhe"), "zh-x-bilingual");
    CHECK_EQ(subtitleLangDisplay("zht"), "中文 (繁體)");
    CHECK_EQ(subtitleLangDisplay("zhe"), "中文 (双语)");
    for (const char* raw : {"nor", "nob", "nb"}) CHECK_EQ(subtitleLangCode(raw), "no");
    CHECK_EQ(subtitleLangCode("nno"), "nn");
    CHECK_EQ(subtitleLangDisplay("nno"), "Norsk (nynorsk)");
    // every variant has its own code, so a list keeps one track of each
    std::set<std::string> codes;
    for (const char* raw : {"spa", "spn", "spl", "fre", "fr-ca", "chi", "zht", "zhe", "nor", "nno", "por", "pob"})
        codes.insert(subtitleLangCode(raw));
    CHECK_EQ(std::to_string(codes.size()), "12");
    // other languages are as before
    CHECK_EQ(subtitleLangCode("eng"), "en");
    CHECK_EQ(subtitleLangCode("en-US"), "en");
    CHECK_EQ(subtitleLangCode("xyz"), "");

    // the preference picker lists Brazilian Portuguese right after Portuguese
    std::string after;
    const auto& cat = media::subtitleLangCatalog();
    for (size_t i = 0; i + 1 < cat.size(); ++i)
        if (cat[i].code == "pt") after = cat[i + 1].code + " " + cat[i + 1].display;
    CHECK_EQ(after, "pt-BR Português (Brasil)");

    // one track gets selected: the first with the preferred code, else the first in its language
    CHECK_EQ(pick({"en", "pt-BR", "pt"}, "pt"), "2");
    CHECK_EQ(pick({"en", "pt", "pt-BR"}, "pt-BR"), "2");
    CHECK_EQ(pick({"en", "pt-BR"}, "pt"), "1");
    CHECK_EQ(pick({"en", "pt"}, "pt-BR"), "1");
    CHECK_EQ(pick({"pt", "pt"}, "pt"), "0");
    CHECK_EQ(pick({"en", "PT-br"}, "pt-BR"), "1");
    CHECK_EQ(pick({"en", "fr"}, "pt"), "none");
    CHECK_EQ(pick({"en", ""}, ""), "none");
    CHECK_EQ(pick({}, "en"), "none");

    // the audio setting: a Brazilian dub first, then any Portuguese track
    CHECK_EQ(media::audioLangList("pt-BR", "en-US"), "pt-BR,pob,pt,por");
    CHECK_EQ(media::audioLangList("pt", "en-US"), "pt,por,pob");
    CHECK_EQ(media::audioLangList("es-419", "en-US"), "es-419,spl,es,spa,spn");
    CHECK_EQ(media::audioLangList("es", "en-US"), "es,spa,spn,spl");
    // "auto" follows a variant the app language names (Traditional Chinese), else its language
    CHECK_EQ(media::audioLangList("auto", "zh-Hant"), "zh-Hant,zht,zh,chi,zho,zhe");
    CHECK_EQ(media::audioLangList("auto", "zh-Hans"), "zh,chi,zho,zht,zhe");
    CHECK_EQ(media::audioLangList("auto", "en-US"), "en,eng");

    if (failures == 0) printf("test_subtitle_lang: all passed\n");
    return failures ? 1 : 0;
}
