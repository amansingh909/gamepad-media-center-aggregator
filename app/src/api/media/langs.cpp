/*
    GMCA — subtitle language helpers (see api/media/langs.hpp).

    A single ordered catalog of common subtitle languages, each with a canonical
    ISO 639-1 code, an endonym label, and every alias we want to recognize (639-1,
    639-2/B and /T, common OpenSubtitles variants, and a few English names). An
    alias -> index map is built once on first use for O(1) lookups.
*/

#include "api/media/langs.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace media {

namespace {

/// One catalog row: canonical code, endonym, and the aliases that map onto it.
struct LangRow {
    const char* code;
    const char* display;
    std::vector<const char*> aliases;
};

// Ordered roughly by global prevalence so the preference picker stays friendly.
// Aliases are lowercase; the 2-letter `code` is matched implicitly too.
const std::vector<LangRow>& rows() {
    static const std::vector<LangRow> table = {
        {"en", "English", {"eng"}},
        {"es", "Español", {"spa", "spl", "spn", "spanish", "es-419"}},
        {"fr", "Français", {"fre", "fra", "fr-ca", "french"}},
        {"de", "Deutsch", {"ger", "deu", "german"}},
        {"it", "Italiano", {"ita", "italian"}},
        {"pt", "Português", {"por", "pob", "pt-br", "pt-pt", "portuguese"}},
        {"ru", "Русский", {"rus", "russian"}},
        {"ar", "العربية", {"ara", "arabic"}},
        {"zh", "中文", {"chi", "zho", "zht", "zhe", "cn", "chinese"}},
        {"ja", "日本語", {"jpn", "japanese"}},
        {"ko", "한국어", {"kor", "korean"}},
        {"nl", "Nederlands", {"dut", "nld", "dutch"}},
        {"pl", "Polski", {"pol", "polish"}},
        {"tr", "Türkçe", {"tur", "turkish"}},
        {"sv", "Svenska", {"swe", "swedish"}},
        {"da", "Dansk", {"dan", "danish"}},
        {"no", "Norsk", {"nor", "nob", "nno", "norwegian"}},
        {"fi", "Suomi", {"fin", "finnish"}},
        {"cs", "Čeština", {"cze", "ces", "czech"}},
        {"el", "Ελληνικά", {"gre", "ell", "greek"}},
        {"he", "עברית", {"heb", "iw", "hebrew"}},
        {"hi", "हिन्दी", {"hin", "hindi"}},
        {"id", "Bahasa Indonesia", {"ind", "in", "indonesian"}},
        {"th", "ไทย", {"tha", "thai"}},
        {"uk", "Українська", {"ukr", "ukrainian"}},
        {"ro", "Română", {"rum", "ron", "romanian"}},
        {"hu", "Magyar", {"hun", "hungarian"}},
        {"vi", "Tiếng Việt", {"vie", "vietnamese"}},
        {"bg", "Български", {"bul", "bulgarian"}},
        {"hr", "Hrvatski", {"hrv", "scr", "croatian"}},
        {"sr", "Српски", {"srp", "scc", "serbian"}},
        {"sk", "Slovenčina", {"slo", "slk", "slovak"}},
    };
    return table;
}

/// alias/code -> row index, built once.
const std::unordered_map<std::string, size_t>& aliasIndex() {
    static const std::unordered_map<std::string, size_t> map = [] {
        std::unordered_map<std::string, size_t> m;
        const auto& t = rows();
        for (size_t i = 0; i < t.size(); ++i) {
            m[t[i].code] = i;
            for (const char* a : t[i].aliases) m[a] = i;
        }
        return m;
    }();
    return map;
}

/// Trim + lowercase.
std::string normalize(const std::string& raw) {
    size_t b = 0, e = raw.size();
    while (b < e && std::isspace((unsigned char)raw[b])) ++b;
    while (e > b && std::isspace((unsigned char)raw[e - 1])) --e;
    std::string s = raw.substr(b, e - b);
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

/// Resolve a raw lang to a catalog index, or npos. Tries the full normalized
/// string, then (for region-tagged codes like "pt-br") the part before '-'.
size_t lookup(const std::string& raw) {
    const auto& idx = aliasIndex();
    std::string s = normalize(raw);
    if (s.empty()) return std::string::npos;
    auto it = idx.find(s);
    if (it != idx.end()) return it->second;
    auto dash = s.find('-');
    if (dash != std::string::npos) {
        it = idx.find(s.substr(0, dash));
        if (it != idx.end()) return it->second;
    }
    return std::string::npos;
}

}  // namespace

std::string subtitleLangCode(const std::string& raw) {
    size_t i = lookup(raw);
    return i == std::string::npos ? std::string() : rows()[i].code;
}

std::string subtitleLangDisplay(const std::string& raw) {
    size_t i = lookup(raw);
    if (i != std::string::npos) return rows()[i].display;
    // Unrecognized: show the raw value (the SDK allows lang to be free text).
    std::string s = raw;
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

const std::vector<LangOption>& subtitleLangCatalog() {
    static const std::vector<LangOption> cat = [] {
        std::vector<LangOption> c;
        for (const auto& r : rows()) c.push_back({r.code, r.display});
        return c;
    }();
    return cat;
}

std::string audioLangList(const std::string& setting, const std::string& appLocale) {
    if (setting == "file") return "";
    const std::string code = setting == "auto" ? appLocale.substr(0, appLocale.find('-')) : setting;
    const size_t i = lookup(code);
    if (i == std::string::npos) return "";
    const LangRow& r = rows()[i];
    std::string out = r.code;
    for (const char* a : r.aliases) {
        const std::string alias = a;
        if (alias.size() == 3 &&
            std::all_of(alias.begin(), alias.end(), [](unsigned char c) { return std::isalpha(c) != 0; }))
            out += "," + alias;
    }
    return out;
}

}  // namespace media
