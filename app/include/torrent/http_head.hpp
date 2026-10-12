// The little HTTP the torrent server speaks: request heads in, status lines out.
#pragma once

#include <cctype>
#include <string>

namespace torrent {

struct RequestHead {
    std::string method, target, range;
};

/// Parses "METHOD target HTTP/x.y" plus headers, keeping only Range.
inline bool parseRequestHead(const std::string& text, RequestHead& out) {
    size_t eol = text.find("\r\n");
    if (eol == std::string::npos) return false;
    std::string line = text.substr(0, eol);
    size_t s1 = line.find(' '), s2 = line.rfind(' ');
    if (s1 == std::string::npos || s2 == s1) return false;
    if (line.compare(s2 + 1, 5, "HTTP/") != 0) return false;
    out.method = line.substr(0, s1);
    out.target = line.substr(s1 + 1, s2 - s1 - 1);
    out.range.clear();

    size_t pos = eol + 2;
    while (pos < text.size()) {
        size_t e = text.find("\r\n", pos);
        if (e == std::string::npos) e = text.size();
        std::string h = text.substr(pos, e - pos);
        if (h.empty()) break;
        size_t colon = h.find(':');
        if (colon != std::string::npos) {
            std::string name = h.substr(0, colon);
            for (char& c : name) c = char(std::tolower((unsigned char)c));
            if (name == "range") {
                std::string v = h.substr(colon + 1);
                size_t a = v.find_first_not_of(" \t"), z = v.find_last_not_of(" \t");
                out.range = a == std::string::npos ? "" : v.substr(a, z - a + 1);
            }
        }
        pos = e + 2;
    }
    return true;
}

inline std::string statusLine(int code) {
    const char* text = "Error";
    switch (code) {
        case 200: text = "OK"; break;
        case 206: text = "Partial Content"; break;
        case 400: text = "Bad Request"; break;
        case 404: text = "Not Found"; break;
        case 405: text = "Method Not Allowed"; break;
        case 416: text = "Range Not Satisfiable"; break;
        case 502: text = "Bad Gateway"; break;
        case 503: text = "Service Unavailable"; break;
        case 504: text = "Gateway Timeout"; break;
        case 507: text = "Insufficient Storage"; break;
    }
    return "HTTP/1.1 " + std::to_string(code) + " " + text;
}

inline std::string contentTypeFor(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = char(std::tolower((unsigned char)c));
    if (ext == "mkv") return "video/x-matroska";
    if (ext == "mp4" || ext == "m4v") return "video/mp4";
    if (ext == "webm") return "video/webm";
    if (ext == "avi") return "video/x-msvideo";
    if (ext == "ts") return "video/mp2t";
    return "application/octet-stream";
}

}  // namespace torrent
