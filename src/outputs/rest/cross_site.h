// SPDX-License-Identifier: MIT
//
// Tells a request the bridge's own page sent from one a browser sent on behalf of ANOTHER site.
//
// HTTP Basic is the only credential this API has, and a browser attaches it by itself: once an
// admin has typed the password into the browser's own prompt (every 401 offers one), any page
// they visit later can POST to the bridge and the browser adds the header. A form or a
// `fetch(..., {mode: "no-cors"})` needs no CORS permission to be SENT -- only to read the
// answer -- and a `text/plain` body is a "simple" request with no preflight. So a page on the
// internet could factory-reset the bridge, move the DRM relays with control allowed, or restore
// a backup of its own making, which sets the admin password and opens every gate.
//
// The browser says where a request came from, and that is what this reads. The same rule as Go's
// net/http CrossOriginProtection (Go 1.25), deliberately, because it is small and well argued:
//
//   1. Sec-Fetch-Site, which a page cannot set: allowed only when `same-origin` (our own page)
//      or `none` (typed in the address bar, a bookmark). Browsers send it only to a
//      "potentially trustworthy" origin (https, localhost), so this applies behind a TLS reverse
//      proxy or an ssh tunnel; on the bridge's own plain-HTTP address it is absent and (2)
//      decides.
//   2. Origin, which every current browser sends on a POST/PATCH: its host part must equal the
//      Host header. A cross-site page cannot suppress it -- a `no-referrer` policy, a sandboxed
//      frame, a data: or file:// page or a cross-origin redirect turns it into `null`, never
//      into nothing -- and `null` is refused. The bridge's own pages set no Referrer-Policy, so
//      their same-origin writes carry their real origin; tools/check_web_js.py keeps it so.
//   3. Neither header: not a browser -- curl, Home Assistant's rest_command, a script. Those
//      carry no ambient credential to abuse, so Basic auth stays their only gate, as before.
//
// Not covered: DNS rebinding. A page on evil.example re-pointed at the bridge's address has an
// Origin equal to its Host. It holds no credential for the bridge (the browser keys those to the
// bridge's own name), so it reaches only what needs none: the GETs, and the setup portal's two
// routes while no admin password exists -- which anyone on the setup AP can reach anyway.
//
// Pure and header-only so the native suite tests it; the web server reads the headers.

#pragma once

#include <cstddef>
#include <string_view>

namespace heliograph::rest {

/// True when a browser sent this request from a page of another site. An empty view means the
/// header was absent.
inline bool isCrossSiteRequest(std::string_view secFetchSite, std::string_view origin,
                               std::string_view host) {
    if (!secFetchSite.empty()) {
        return !(secFetchSite == "same-origin" || secFetchSite == "none");
    }
    if (origin.empty()) {
        return false;
    }
    // scheme://host[:port]. The scheme is not compared: behind a TLS reverse proxy the page is
    // https:// while the bridge itself only ever speaks http. (Such a proxy also brings
    // Sec-Fetch-Site, so a proxy that rewrites Host never gets this far with a browser.)
    const std::size_t sep = origin.find("://");
    if (sep == std::string_view::npos) {
        return true;  // "null", or anything else that is not an origin
    }
    return origin.substr(sep + 3) != host;
}

}  // namespace heliograph::rest
