/*
    GMCA — a server refusal (HTTP 403), backend-agnostic.

    Another delivery path hits the same refusal, so the player stops retrying.
*/

#pragma once

#include <string>

namespace media {

/// mpv reports "HTTP error 403 Forbidden", the HTTP client "http status 403"
inline bool isForbidden(const std::string& error) {
    return error.find("HTTP error 403") != std::string::npos || error.find("http status 403") != std::string::npos;
}

}  // namespace media
