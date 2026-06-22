// Copyright Keylight. All Rights Reserved.
// FHttpTransport.h — keylight::Transport adapter over UE's FHttpModule.
//
// Threading model:
//   The SDK's Client calls Transport::request() synchronously on a background
//   worker thread (dispatched by UKeylightSubsystem via AsyncTask).  FHttpModule
//   works asynchronously (fire → UE HTTP thread → delegate callback).
//
//   We bridge the gap with an FEvent* (a UE manual-reset event, effectively a
//   binary semaphore):
//
//     Worker thread               UE HTTP thread
//     ─────────────               ──────────────
//     SendRequest()
//     Event->Wait()   ─────────► OnProcessRequestComplete()
//                                 copies status + body
//                                 Event->Trigger()
//     (unblocks)
//     return Result
//
//   The game thread is NEVER blocked.  The worker thread is one of UE's
//   AnyBackgroundThreadNormalTask slots, so blocking it is acceptable.
//
// Error mapping:
//   • Network failure / timeout → Result::err({ErrorCode::Network, ...})
//   • HTTP 4xx / 5xx           → Result::ok({status, body})  (caller decides)

#pragma once

// SDK transport interface (header-only; include path set in Build.cs)
#include "keylight/transport.hpp"

#include "CoreMinimal.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#include <string>
#include <map>

// ---------------------------------------------------------------------------
// FHttpTransport
// ---------------------------------------------------------------------------
class FHttpTransport final : public keylight::Transport
{
public:
    FHttpTransport() = default;
    ~FHttpTransport() override = default;

    // Non-copyable, non-movable (we hold a raw pointer in Client)
    FHttpTransport(const FHttpTransport&)            = delete;
    FHttpTransport& operator=(const FHttpTransport&) = delete;

    /**
     * Perform a synchronous HTTP request on whichever thread calls this
     * method.  MUST NOT be called from the game thread (will block).
     *
     * Exact override of keylight::Transport::request():
     *   virtual Result<HttpResponse> request(
     *       const std::string& method,
     *       const std::string& url,
     *       const std::map<std::string,std::string>& headers,
     *       const std::string& body) = 0;
     */
    keylight::Result<keylight::HttpResponse> request(
        const std::string&                        method,
        const std::string&                        url,
        const std::map<std::string, std::string>& headers,
        const std::string&                        body) override;
};
