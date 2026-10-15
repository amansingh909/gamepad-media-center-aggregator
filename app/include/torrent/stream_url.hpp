// Stremio-style torrent stream addresses, as served by Stremio's streaming server:
//   /<info hash, 40 hex>/<file index, or -1 for the largest file>?tr=<tracker>&f=<name hint>
#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace torrent {

struct StreamRequest {
    std::string infoHash;  // lowercase hex
    int fileIndex = -1;
    std::vector<std::string> trackers;
    std::vector<std::string> fileHints;
};

/// 40 lowercase hex characters: the form the engine accepts and names its folders by.
inline bool isInfoHash(const std::string& h) {
    if (h.size() != 40) return false;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out += char(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

inline std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

/// Parses "/<hash>/<index>[?query]"; anything else is rejected.
inline bool parseStreamPath(const std::string& target, StreamRequest& out) {
    StreamRequest r;
    size_t q = target.find('?');
    std::string path = target.substr(0, q);
    if (path.size() < 2 || path[0] != '/') return false;
    size_t slash = path.find('/', 1);
    if (slash == std::string::npos) return false;
    std::string hash = path.substr(1, slash - 1);
    std::string index = path.substr(slash + 1);
    if (hash.size() != 40) return false;
    for (char& c : hash) {
        if (hexValue(c) < 0) return false;
        c = char(std::tolower((unsigned char)c));
    }
    if (index == "-1") {
        r.fileIndex = -1;
    } else {
        if (index.empty() || index.size() > 6) return false;
        for (char c : index)
            if (c < '0' || c > '9') return false;
        r.fileIndex = std::stoi(index);
    }
    r.infoHash = hash;

    if (q != std::string::npos) {
        std::string query = target.substr(q + 1);
        size_t pos = 0;
        while (pos <= query.size()) {
            size_t amp = query.find('&', pos);
            if (amp == std::string::npos) amp = query.size();
            std::string kv = query.substr(pos, amp - pos);
            size_t eq = kv.find('=');
            if (eq != std::string::npos) {
                std::string key = kv.substr(0, eq), value = percentDecode(kv.substr(eq + 1));
                if (key == "tr" && !value.empty()) r.trackers.push_back(value);
                else if (key == "f" && !value.empty()) r.fileHints.push_back(value);
            }
            pos = amp + 1;
        }
    }
    out = r;
    return true;
}

inline std::string streamPath(const StreamRequest& r) {
    std::string out = "/" + r.infoHash + "/" + std::to_string(r.fileIndex);
    char sep = '?';
    for (const auto& t : r.trackers) {
        out += sep;
        out += "tr=" + percentEncode(t);
        sep = '&';
    }
    for (const auto& f : r.fileHints) {
        out += sep;
        out += "f=" + percentEncode(f);
        sep = '&';
    }
    return out;
}

inline std::string magnetUri(const StreamRequest& r) {
    std::string m = "magnet:?xt=urn:btih:" + r.infoHash;
    for (const auto& t : r.trackers) m += "&tr=" + percentEncode(t);
    return m;
}

}  // namespace torrent
