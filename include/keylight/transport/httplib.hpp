#pragma once
// ---------------------------------------------------------------------------
// keylight/transport/httplib.hpp — opt-in cpp-httplib + OpenSSL transport
//
// This header is NOT included by the core umbrella <keylight/keylight.hpp>.
// Include it explicitly only in translation units that need it, and only
// when building with -DKEYLIGHT_BUILD_HTTPLIB_TRANSPORT=ON (which arranges
// CPPHTTPLIB_OPENSSL_SUPPORT + the OpenSSL link).
//
// Zero OpenSSL dependency leaks into the core library.
// ---------------------------------------------------------------------------
#include "../transport.hpp"

// cpp-httplib — vendored single header in third_party/
// CPPHTTPLIB_OPENSSL_SUPPORT is defined by the CMake option target.
#include "../../../third_party/httplib.h"

#include <string>
#include <map>
#include <stdexcept>

namespace keylight {

// ---------------------------------------------------------------------------
// URL parser — splits "https://host[:port]/path?query" into parts.
// Only https:// (port 443) and http:// (port 80) are handled.
// ---------------------------------------------------------------------------
namespace detail {

struct ParsedUrl {
    bool        is_https = true;
    std::string host;
    int         port     = 443;
    std::string path;   // includes leading slash + query string
};

inline ParsedUrl parse_url(const std::string& url) {
    ParsedUrl p;

    std::string rest = url;
    if (rest.rfind("https://", 0) == 0) {
        p.is_https = true;
        p.port     = 443;
        rest       = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        p.is_https = false;
        p.port     = 80;
        rest       = rest.substr(7);
    } else {
        // Default to HTTPS
        p.is_https = true;
        p.port     = 443;
    }

    // Split host+optional-port from path
    auto slash = rest.find('/');
    std::string authority;
    if (slash == std::string::npos) {
        authority = rest;
        p.path    = "/";
    } else {
        authority = rest.substr(0, slash);
        p.path    = rest.substr(slash);   // keeps the leading '/'
    }

    // Check for explicit port in authority (host:port)
    auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        p.host = authority.substr(0, colon);
        try {
            p.port = std::stoi(authority.substr(colon + 1));
        } catch (...) {
            p.host = authority;  // not a port — treat whole thing as host
        }
    } else {
        p.host = authority;
    }

    if (p.path.empty()) p.path = "/";
    return p;
}

} // namespace detail

// ---------------------------------------------------------------------------
// HttplibTransport
// ---------------------------------------------------------------------------
class HttplibTransport : public Transport {
public:
    Result<HttpResponse> request(
        const std::string&                        method,
        const std::string&                        url,
        const std::map<std::string, std::string>& headers,
        const std::string&                        body
    ) override {
        detail::ParsedUrl pu;
        try {
            pu = detail::parse_url(url);
        } catch (...) {
            return Result<HttpResponse>::err(
                {ErrorCode::Network, "failed to parse URL: " + url});
        }

        // Build httplib headers
        httplib::Headers hl_headers;
        for (const auto& kv : headers) {
            hl_headers.emplace(kv.first, kv.second);
        }

        // Determine Content-Type for body
        std::string content_type;
        auto ct_it = headers.find("Content-Type");
        if (ct_it != headers.end()) {
            content_type = ct_it->second;
        } else {
            content_type = "application/json";
        }

        httplib::Result hl_res;

        try {
            if (pu.is_https) {
                httplib::SSLClient cli(pu.host, pu.port);
                cli.set_follow_location(true);
                cli.set_connection_timeout(10);
                cli.set_read_timeout(30);

                if (method == "GET" || method == "DELETE" || method == "HEAD") {
                    if (method == "GET")
                        hl_res = cli.Get(pu.path, hl_headers);
                    else if (method == "DELETE")
                        hl_res = cli.Delete(pu.path, hl_headers);
                    else
                        hl_res = cli.Head(pu.path, hl_headers);
                } else {
                    // POST, PUT, PATCH …
                    hl_res = cli.Post(pu.path, hl_headers, body, content_type);
                    if (method == "PUT")
                        hl_res = cli.Put(pu.path, hl_headers, body, content_type);
                    else if (method == "PATCH")
                        hl_res = cli.Patch(pu.path, hl_headers, body, content_type);
                }
            } else {
                httplib::Client cli(pu.host, pu.port);
                cli.set_follow_location(true);
                cli.set_connection_timeout(10);
                cli.set_read_timeout(30);

                if (method == "GET" || method == "DELETE" || method == "HEAD") {
                    if (method == "GET")
                        hl_res = cli.Get(pu.path, hl_headers);
                    else if (method == "DELETE")
                        hl_res = cli.Delete(pu.path, hl_headers);
                    else
                        hl_res = cli.Head(pu.path, hl_headers);
                } else {
                    hl_res = cli.Post(pu.path, hl_headers, body, content_type);
                    if (method == "PUT")
                        hl_res = cli.Put(pu.path, hl_headers, body, content_type);
                    else if (method == "PATCH")
                        hl_res = cli.Patch(pu.path, hl_headers, body, content_type);
                }
            }
        } catch (const std::exception& ex) {
            return Result<HttpResponse>::err(
                {ErrorCode::Network, std::string("httplib exception: ") + ex.what()});
        } catch (...) {
            return Result<HttpResponse>::err(
                {ErrorCode::Network, "httplib unknown exception"});
        }

        if (!hl_res) {
            auto ec = hl_res.error();
            std::string msg = "httplib error: ";
            msg += httplib::to_string(ec);
            return Result<HttpResponse>::err({ErrorCode::Network, std::move(msg)});
        }

        HttpResponse resp;
        resp.status = hl_res->status;
        resp.body   = hl_res->body;
        return Result<HttpResponse>::ok(std::move(resp));
    }
};

} // namespace keylight
