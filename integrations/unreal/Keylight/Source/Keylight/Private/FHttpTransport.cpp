// Copyright Keylight. All Rights Reserved.
// FHttpTransport.cpp — UE FHttpModule adapter for keylight::Transport.

#include "FHttpTransport.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/Event.h"                 // FEvent / FPlatformProcess::GetSynchEventFromPool
#include "Misc/ScopedEvent.h"

#include "keylight/result.hpp"         // keylight::Result, ErrorCode

#include <string>
#include <map>

keylight::Result<keylight::HttpResponse> FHttpTransport::request(
    const std::string&                        method,
    const std::string&                        url,
    const std::map<std::string, std::string>& headers,
    const std::string&                        body)
{
    // ── Build the UE HTTP request ─────────────────────────────────────────

    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest =
        FHttpModule::Get().CreateRequest();

    HttpRequest->SetVerb(FString(UTF8_TO_TCHAR(method.c_str())));
    HttpRequest->SetURL(FString(UTF8_TO_TCHAR(url.c_str())));

    // Copy request headers from the SDK's std::map
    for (const auto& [key, value] : headers)
    {
        HttpRequest->SetHeader(
            FString(UTF8_TO_TCHAR(key.c_str())),
            FString(UTF8_TO_TCHAR(value.c_str())));
    }

    if (!body.empty())
    {
        // SetContentAsString expects a UTF-8 FString, but Content-Type is
        // already set by the SDK (application/json).
        TArray<uint8> BodyBytes;
        BodyBytes.Append(
            reinterpret_cast<const uint8*>(body.data()),
            static_cast<int32>(body.size()));
        HttpRequest->SetContent(BodyBytes);
    }

    // ── Bridge: async UE → blocking worker thread ─────────────────────────
    //
    // FEvent is a manual-reset platform event (thin wrapper around
    // POSIX pthread_cond or Win32 CreateEvent).  We borrow one from the pool
    // to avoid repeated allocation.

    FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset=*/false);

    // Shared state written by the UE HTTP thread, read by us after Wait().
    // No mutex needed: the Wait() establishes a happens-before edge.
    bool    bNetworkError = false;
    int32   ResponseCode  = 0;
    FString ResponseBody;

    HttpRequest->OnProcessRequestComplete().BindLambda(
        [DoneEvent, &bNetworkError, &ResponseCode, &ResponseBody]
        (FHttpRequestPtr /*Request*/, FHttpResponsePtr Response, bool bConnectedSuccessfully)
        {
            if (!bConnectedSuccessfully || !Response.IsValid())
            {
                bNetworkError = true;
            }
            else
            {
                ResponseCode = Response->GetResponseCode();
                ResponseBody = Response->GetContentAsString();
            }
            // Unblock the waiting worker thread.
            DoneEvent->Trigger();
        });

    // Fire the request.  UE processes it on the HTTP worker thread pool and
    // calls the delegate above when complete (or on timeout/failure).
    HttpRequest->ProcessRequest();

    // Block this (background) worker thread until the HTTP round-trip finishes.
    // UE's HTTP module has its own internal timeout (default 60 s); we rely on
    // that rather than imposing a secondary timeout here.
    DoneEvent->Wait();

    // Return the event to the pool for reuse.
    FPlatformProcess::ReturnSynchEventToPool(DoneEvent);

    // ── Map result ────────────────────────────────────────────────────────

    if (bNetworkError)
    {
        return keylight::Result<keylight::HttpResponse>::err(
            {keylight::ErrorCode::Network, "FHttpTransport: network error or timeout"});
    }

    // Convert UE FString body to std::string (UTF-8).
    // TCHAR_TO_UTF8 returns a const char* valid for the lifetime of the macro
    // scope; copy it into a std::string immediately.
    std::string StdBody(TCHAR_TO_UTF8(*ResponseBody));

    keylight::HttpResponse Resp;
    Resp.status = static_cast<int>(ResponseCode);
    Resp.body   = std::move(StdBody);

    return keylight::Result<keylight::HttpResponse>::ok(std::move(Resp));
}
