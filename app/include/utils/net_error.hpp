#pragma once

#include "api/http.hpp"
#include "utils/hostname.hpp"

#include <borealis/core/i18n.hpp>
#include <fmt/format.h>

#include <string>

namespace net {

inline std::string dnsHelp(const std::string& url) {
    using namespace brls::literals;
    return fmt::format(fmt::runtime("main/server/dns_unresolved"_i18n), hostFromUrl(url));
}

/// libnx and SceNet have no mDNS resolver: a `.local` server can never be reached.
inline bool unresolvableHost(const std::string& url) {
#if defined(__SWITCH__) || defined(__PSV__)
    return isMdnsHost(url);
#else
    (void)url;
    return false;
#endif
}

inline std::string errorText(const HTTP::Error& ex, const std::string& url) {
    return ex.kind == HTTP::ErrorKind::ResolveFailed ? dnsHelp(url) : ex.what();
}

}  // namespace net
