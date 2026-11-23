/*
    GMCA — subtitle language helpers (backend-agnostic).

    Stremio subtitles carry a `lang` that is an ISO 639-2 code (3 letters, /B or
    /T variants), sometimes an ISO 639-1 code (2 letters) or a plain-text name, or
    an OpenSubtitles-specific variant ("pob" = Brazilian Portuguese, "zht" =
    Traditional Chinese…). To dedupe tracks by language, to label them in the
    picker, and to match the user's preferred-subtitle-language setting, we
    normalize any of these to a canonical code and an endonym display label: an
    ISO 639-1 (2-letter) code, or a regional variant the addons list apart
    ("pt-BR", Brazilian Portuguese, next to "pt"). Unknown codes fall back to the
    raw value (the SDK allows lang to be free text), so a language we don't
    tabulate still shows up, just unlabeled.

    subtitleLangCatalog() also drives the settings picker (Automatique / Désactivé
    are prepended by the settings tab, not listed here).
*/

#pragma once

#include <string>
#include <vector>

namespace media {

struct LangOption {
    std::string code;     // canonical ISO 639-1 (2-letter, lowercase), or a variant ("pt-BR")
    std::string display;  // endonym label for menus / the preference picker
};

/// Canonical code for a subtitle language string (ISO 639-1, 639-2/B or /T, or a known
/// variant): 2 letters, or a regional variant such as "pt-BR". Empty when unrecognized.
std::string subtitleLangCode(const std::string& raw);

/// Human display label (endonym) for a subtitle language string; falls back to
/// the raw value when unrecognized.
std::string subtitleLangDisplay(const std::string& raw);

/// Ordered catalog of common subtitle languages for the preference picker.
const std::vector<LangOption>& subtitleLangCatalog();

/// mpv's `alang` for the audio language setting: "auto" follows the app language
/// (`appLocale`, such as "en-US" or "zh-Hant", its variant when the catalog has one), "file"
/// sets none (the file's default track), anything else is a code of the catalog. The code and
/// the catalog's 3-letter ones, as files tag their tracks either way ("fr,fre,fra"); for a
/// regional variant, its own codes first and then its language's ("pt-BR,pob,pt,por"). "" for
/// "file" and for a language the catalog doesn't have.
std::string audioLangList(const std::string& setting, const std::string& appLocale);

/// Of a video's subtitle tracks, by their canonical codes (`tags`, "" for a track without one),
/// the one to select for the preferred language `pref`: the first with that exact code, else the
/// first in the same language ("pt-BR" for "pt", and the other way round). npos for none, or an
/// empty `pref`.
size_t preferredSubtitle(const std::vector<std::string>& tags, const std::string& pref);

}  // namespace media
