// The source a Stremio title last played from, remembered per movie or episode so that
// Continue Watching and the Play button start from it again (PlayerView). Kept locally
// (AppConfig::STREMIO_LAST_SOURCES): a JSON array of {"id": item id, "source": source key}, the
// most recent first, at most sourceMemorySize entries. Pure: tests/test_source_memory.cpp.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace source_memory {

constexpr size_t sourceMemorySize = 200;

/// What identifies a source from one fetch of the addons to the next: a torrent's local stream
/// URL without its query (its info hash and file; the trackers may come in another order), any
/// other part key as it is (a link's URL). `localStreamBase` is the local torrent server's
/// address ("" in builds without one).
inline std::string sourceKey(const std::string& partKey, const std::string& localStreamBase) {
    if (!localStreamBase.empty() && partKey.compare(0, localStreamBase.size(), localStreamBase) == 0)
        return partKey.substr(0, partKey.find('?'));
    return partKey;
}

/// The store with `id`'s source set to `key`, first, without its older entry, cut to `size`.
inline nlohmann::json remember(const nlohmann::json& store, const std::string& id, const std::string& key,
                               size_t size = sourceMemorySize) {
    nlohmann::json out = nlohmann::json::array();
    if (id.empty() || key.empty()) return store.is_array() ? store : out;
    out.push_back({{"id", id}, {"source", key}});
    if (store.is_array())
        for (const auto& e : store) {
            if (out.size() >= size) break;
            if (!e.is_object() || !e.contains("id") || !e["id"].is_string() || e["id"].get<std::string>() == id) continue;
            if (!e.contains("source") || !e["source"].is_string()) continue;
            out.push_back(e);
        }
    return out;
}

/// `id`'s remembered source key, "" when there's none.
inline std::string recall(const nlohmann::json& store, const std::string& id) {
    if (!store.is_array() || id.empty()) return "";
    for (const auto& e : store)
        if (e.is_object() && e.contains("id") && e["id"].is_string() && e["id"].get<std::string>() == id &&
            e.contains("source") && e["source"].is_string())
            return e["source"].get<std::string>();
    return "";
}

/// The index of the source with key `key` among `keys` (one per source, "" for one that can't
/// play), -1 when it's not among them.
inline int indexOf(const std::vector<std::string>& keys, const std::string& key) {
    if (key.empty()) return -1;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key) return int(i);
    return -1;
}

}  // namespace source_memory
