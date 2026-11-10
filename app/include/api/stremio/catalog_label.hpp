#pragma once

#include <string>

namespace stremio {

/// The text on a catalog's tab or row title. `shown` is the catalog's own label (translated for
/// Cinemeta's, else its declared name). An addon may declare "name": "", or only an id as a name:
/// then the addon's manifest name, then the catalog id, then `generic` ("Movies 2"), so a tab
/// never reads blank.
inline std::string pickCatalogLabel(
    const std::string& shown, const std::string& id, const std::string& addonName, const std::string& generic) {
    if (!shown.empty() && shown != id) return shown;
    if (!addonName.empty()) return addonName;
    if (!shown.empty()) return shown;
    if (!id.empty()) return id;
    return generic;
}

/// "Movies 2": the content type's label and the catalog's place in its list (from 1; 0 = none).
inline std::string genericCatalogLabel(const std::string& typeLabel, size_t position) {
    return position > 0 ? typeLabel + " " + std::to_string(position) : typeLabel;
}

}  // namespace stremio
