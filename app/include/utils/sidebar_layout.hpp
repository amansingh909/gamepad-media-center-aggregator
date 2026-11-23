#pragma once

/*
    Pure logic of the per-server sidebar layout (order + hidden tab ids).

    Header-only and dependency-free (STL) so it can be unit-tested without
    Borealis/config. MainTabFrame reads and writes the saved layout through it.
*/

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace sidebar {

/// Layout of the present tabs: saved ids first (present only, deduped), then
/// new present ids in natural order; hidden ids are kept only when present.
inline void resolve(const std::vector<std::string>& natural, const std::vector<std::string>& savedOrder,
    const std::set<std::string>& savedHidden, std::vector<std::string>& order, std::set<std::string>& hidden) {
    order.clear();
    hidden.clear();
    std::set<std::string> present(natural.begin(), natural.end());

    std::set<std::string> placed;
    for (auto& id : savedOrder)
        if (present.count(id) && placed.insert(id).second) order.push_back(id);
    for (auto& id : natural)
        if (placed.insert(id).second) order.push_back(id);

    for (auto& id : savedHidden)
        if (present.count(id)) hidden.insert(id);
}

/// Saved layout after editing the present tabs: ids absent right now (a library
/// not downloaded while offline, a section the server did not return) keep
/// their place, right after the id that preceded them, and their hidden state.
inline void merge(const std::vector<std::string>& savedOrder, const std::set<std::string>& savedHidden,
    const std::vector<std::string>& order, const std::set<std::string>& hidden, std::vector<std::string>& outOrder,
    std::set<std::string>& outHidden) {
    std::set<std::string> present(order.begin(), order.end());
    outOrder = order;

    std::string prev;
    for (auto& id : savedOrder) {
        if (std::find(outOrder.begin(), outOrder.end(), id) == outOrder.end()) {
            auto at = prev.empty() ? outOrder.begin() : std::find(outOrder.begin(), outOrder.end(), prev) + 1;
            outOrder.insert(at, id);
        }
        prev = id;
    }

    outHidden = hidden;
    for (auto& id : savedHidden)
        if (!present.count(id)) outHidden.insert(id);
}

}  // namespace sidebar
