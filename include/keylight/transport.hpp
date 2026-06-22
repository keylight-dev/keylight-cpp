#pragma once
// ---------------------------------------------------------------------------
// keylight/transport.hpp — zero-dependency HTTP transport interface
//
// This header is part of the core library and has NO external dependencies.
// Do NOT include httplib.h, OpenSSL, or any platform SDK here.
// ---------------------------------------------------------------------------
#include <map>
#include <string>
#include "result.hpp"

namespace keylight {

// ---------------------------------------------------------------------------
// HttpResponse
// ---------------------------------------------------------------------------
struct HttpResponse {
    int         status = 0;
    std::string body;
};

// ---------------------------------------------------------------------------
// Transport — abstract HTTP seam
// ---------------------------------------------------------------------------
class Transport {
public:
    virtual ~Transport() = default;

    /// Perform an HTTP request.
    /// @param method   HTTP verb ("GET", "POST", …)
    /// @param url      Fully-qualified URL, e.g. "https://api.keylight.dev/v1/…"
    /// @param headers  Request headers (including Content-Type, Authorization, …)
    /// @param body     Request body (may be empty for GET/DELETE)
    /// @returns        Result<HttpResponse> — err(ErrorCode::Network) on I/O failure
    virtual Result<HttpResponse> request(
        const std::string&                        method,
        const std::string&                        url,
        const std::map<std::string, std::string>& headers,
        const std::string&                        body
    ) = 0;
};

} // namespace keylight
