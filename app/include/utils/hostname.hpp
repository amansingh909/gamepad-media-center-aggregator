#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace net {

/// Best-effort host of a URL.
inline std::string hostFromUrl(const std::string& url) {
    std::string s = url;
    auto scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    s = s.substr(0, s.find_first_of("/?#"));
    auto at = s.rfind('@');
    if (at != std::string::npos) s = s.substr(at + 1);
    if (!s.empty() && s.front() == '[') {
        auto close = s.find(']');
        return close == std::string::npos ? s.substr(1) : s.substr(1, close - 1);
    }
    auto colon = s.rfind(':');
    if (colon != std::string::npos) s = s.substr(0, colon);
    return s;
}

inline bool isMdnsHost(const std::string& url) {
    std::string host = hostFromUrl(url);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    if (!host.empty() && host.back() == '.') host.pop_back();
    const std::string suffix = ".local";
    return host.size() > suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace net
