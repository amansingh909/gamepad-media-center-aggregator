#pragma once

/*
    An item id names a folder directly under the downloads folder. It comes from a
    server (ratingKey), so before anything deletes that folder it must be a single,
    non-empty path component: an empty id would name the downloads folder itself, and
    "..", "/" or "\" would reach outside it. STL only, so tests/test_download_path.cpp
    can exercise it.
*/

#include <string>

inline bool isSafeDownloadId(const std::string& id) {
    if (id.empty() || id == "." || id == "..") return false;
    return id.find_first_of("/\\") == std::string::npos;
}
