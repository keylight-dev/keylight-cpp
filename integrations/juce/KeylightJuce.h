/*
 * KeylightJuce.h — single-header Keylight adapter for JUCE audio plugins
 *
 * Drop this file anywhere in your JUCE plugin project; add the Keylight C++
 * SDK include path, and #include "KeylightJuce.h".  No extra dependencies
 * beyond JUCE's own modules (juce_core) and the Keylight SDK headers.
 *
 * AUDIO-THREAD CONTRACT
 * ─────────────────────
 * The audio render thread (processBlock) MUST NOT block, allocate, or lock.
 * All licensing network I/O runs on a background std::thread and delivers
 * results to the message thread via juce::MessageManager::callAsync.
 * The only data the audio thread ever touches are two std::atomic fields:
 *
 *   std::atomic<keylight::State>  state_snapshot_   — mirrors Client::state()
 *   std::atomic<bool>             pro_enabled_       — mirrors hasEntitlement("pro")
 *
 * These are updated from the message thread (in the subscription callback)
 * after every SDK state transition.  No mutex, no allocation, no juce::String
 * construction happens on the audio thread — just two relaxed atomic loads.
 *
 * JUCE version compatibility: JUCE 7 and JUCE 8.
 *
 * Manual verification pending: compile in a real JUCE plugin project.
 * (No JUCE toolchain is available in keylight-cpp CI.)
 */

#pragma once

// ---------------------------------------------------------------------------
// Keylight C++ SDK headers (header-only; add include/keylight to your paths)
// ---------------------------------------------------------------------------
#include <keylight/client.hpp>   // keylight::Client, State, Subscription
#include <keylight/config.hpp>   // keylight::Config
#include <keylight/store.hpp>    // keylight::FileStore
#include <keylight/transport.hpp>// keylight::Transport, HttpResponse, Result

// ---------------------------------------------------------------------------
// JUCE headers (pulled in via AppConfig.h or module includes in your project)
// ---------------------------------------------------------------------------
#include <juce_core/juce_core.h>     // juce::URL, juce::File, juce::Thread,
                                      // juce::MemoryBlock, juce::String
#include <juce_events/juce_events.h>  // juce::MessageManager::callAsync
                                      // (MessageManager lives in juce_events,
                                      //  not juce_core — include it explicitly
                                      //  so this header is self-contained)

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>

namespace keylight {
namespace juce_integration {

// ===========================================================================
// JuceUrlTransport
//
// Implements keylight::Transport over juce::URL.  This class MUST only be
// called from a background thread (not the audio thread, not the message
// thread).  keylight::Client dispatches all networking synchronously from
// whichever thread calls activate()/validate()/deactivate(); the Licensing
// wrapper below routes those calls to a std::thread.
// ===========================================================================
class JuceUrlTransport final : public keylight::Transport
{
public:
    JuceUrlTransport() = default;
    ~JuceUrlTransport() override = default;

    // -----------------------------------------------------------------------
    // request() — exact signature from keylight::Transport
    // Called from a background thread; blocks until the HTTP round-trip
    // completes.  Never called from the audio thread.
    // -----------------------------------------------------------------------
    keylight::Result<keylight::HttpResponse> request(
        const std::string&                        method,
        const std::string&                        url,
        const std::map<std::string, std::string>& headers,
        const std::string&                        body) override
    {
        // Build juce::URL
        juce::URL juceUrl(juce::String(url.c_str()));
        if (juceUrl.isEmpty())
        {
            return keylight::Result<keylight::HttpResponse>::err(
                { keylight::ErrorCode::Network, "JuceUrlTransport: malformed URL" });
        }

        // Build extra-headers string (one "Key: Value\r\n" per header).
        // juce::URL::InputStreamOptions accepts a header block.
        juce::String extraHeaders;
        for (const auto& [key, value] : headers)
        {
            extraHeaders += juce::String(key.c_str())
                          + ": "
                          + juce::String(value.c_str())
                          + "\r\n";
        }

        // Attach POST body if present.
        // juce::URL::withPOSTData takes a juce::MemoryBlock for binary safety.
        if (!body.empty())
        {
            juce::MemoryBlock bodyBlock(body.data(), body.size());
            juceUrl = juceUrl.withPOSTData(bodyBlock);
        }

        // Configure the input stream request.
        // numRedirectsToFollow = 5 (sensible default; api.keylight.dev doesn't
        // redirect, but guard against CDN/proxy chains).
        //
        // NOTE: juce::URL::InputStreamOptions is NOT copy-assignable (its
        // parameterHandling field is const), so the whole thing must be built
        // in one chained expression — each withXxx() returns a fresh value.
        int statusCode = 0;
        juce::StringPairArray responseHeaders;
        const juce::URL::InputStreamOptions opts =
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withExtraHeaders(extraHeaders)
                .withConnectionTimeoutMs(15000) // 15 s connect timeout
                .withNumRedirectsToFollow(5)
                .withHttpRequestCmd(juce::String(method.c_str()))
                .withStatusCode(&statusCode)
                .withResponseHeaders(&responseHeaders);

        // Open the stream (blocks the background thread).
        std::unique_ptr<juce::InputStream> stream(
            juceUrl.createInputStream(opts));

        if (stream == nullptr)
        {
            // Network-level failure (connection refused, DNS, timeout, etc.)
            return keylight::Result<keylight::HttpResponse>::err(
                { keylight::ErrorCode::Network,
                  "JuceUrlTransport: createInputStream returned null" });
        }

        // Read the entire response body.
        juce::MemoryBlock responseBlock;
        stream->readIntoMemoryBlock(responseBlock);

        keylight::HttpResponse resp;
        resp.status = statusCode;
        resp.body   = std::string(
            static_cast<const char*>(responseBlock.getData()),
            responseBlock.getSize());

        return keylight::Result<keylight::HttpResponse>::ok(std::move(resp));
    }
};


// ===========================================================================
// Licensing
//
// Owns a keylight::Client + JuceUrlTransport + FileStore.  Exposes:
//
//   activate(key, callback)    — off the message thread; callback on message thread
//   validate(callback)         — off the message thread; callback on message thread
//   deactivate(callback)       — off the message thread; callback on message thread
//
//   bool hasFeature(feature)   — audio-thread safe (atomic read)
//   State state()              — audio-thread safe (atomic read)
//
// One instance per plugin instance (processor).  NO global / static mutable
// state — safe when multiple instances run in the same process.
//
// Lifecycle: construct on the message thread; destroy after the processor is
// destroyed (typical ownership: member of your AudioProcessor subclass).
// The destructor joins any in-flight background thread and stops
// auto-validation before returning.
// ===========================================================================
class Licensing
{
public:
    // -----------------------------------------------------------------------
    // Constructor
    //
    // cfg         — your keylight::Config (tenantId, productId, trustedKeys…)
    // storePath   — path to the on-disk .lease file.
    //               If empty, a sensible default is derived automatically:
    //               <juce::File::userApplicationDataDirectory>/Keylight/
    //                   <tenantId>-<productId>.lease
    //
    // Construct on the message thread (or at least before audio starts).
    // -----------------------------------------------------------------------
    explicit Licensing(keylight::Config cfg,
                       juce::String    storePath = {})
        : transport_(std::make_unique<JuceUrlTransport>())
        , store_(resolve_store_path_(cfg, storePath))
        , client_(std::make_unique<keylight::Client>(
              cfg, *transport_, store_))
    {
        // Seed the atomic snapshots from the client's initial state
        // (loaded from the on-disk store by Client's constructor).
        state_snapshot_.store(client_->state(), std::memory_order_relaxed);
        refresh_entitlement_cache_();

        // Subscribe to SDK state-change events.
        // The callback fires on whatever thread changes the state (i.e. the
        // background network thread managed by this class). We update the
        // atomics there, then post a UI notification via callAsync so the
        // editor can repaint.
        subscription_ = client_->subscribe([this](keylight::State newState)
        {
            // Update atomics (may be called from any thread, but always from
            // our own background thread — never from the audio thread).
            state_snapshot_.store(newState, std::memory_order_relaxed);
            refresh_entitlement_cache_();

            // Deliver to message thread if a state-change callback is set.
            // Capture alive_ by value (copies the shared_ptr, keeping the
            // flag alive even after ~Licensing runs) so the lambda can
            // safely check whether this is still valid before touching members.
            auto aliveCopy = alive_;
            juce::MessageManager::callAsync([this, aliveCopy, newState]()
            {
                if (!aliveCopy->load())  // Licensing destroyed; drop safely
                    return;
                if (onStateChanged)
                    onStateChanged(newState);
            });
        });
    }

    // -----------------------------------------------------------------------
    // Destructor — joins any running background thread; stops auto-validation.
    // Safe to call from the message thread (join is quick; background threads
    // do only one SDK round-trip then exit).
    // -----------------------------------------------------------------------
    ~Licensing()
    {
        // ① Signal immediately: any callAsync lambdas still queued on the
        //   message thread will see alive_ == false and early-return without
        //   touching this.  Must happen BEFORE we release any other resource.
        alive_->store(false);

        // ② Drop the subscription so the background SDK thread stops firing
        //   our state-change callback (and enqueueing new callAsync lambdas).
        subscription_ = keylight::Subscription{};

        // ③ Stop SDK auto-validation thread (may fire a final callback; the
        //   alive_ flag above makes any resulting callAsync a safe no-op).
        client_->stopAutoValidation();

        // ④ Join any pending activate/validate/deactivate worker thread.
        //   These callAsync lambdas only capture result+cb, not this, so they
        //   are safe even without the flag — but joining here keeps ordering
        //   well-defined.
        join_worker_();
    }

    // Move-only (owns a thread).
    Licensing(const Licensing&)            = delete;
    Licensing& operator=(const Licensing&) = delete;
    Licensing(Licensing&&)                 = delete;
    Licensing& operator=(Licensing&&)      = delete;

    // -----------------------------------------------------------------------
    // activate — call from the message thread (e.g. a button handler in your
    // AudioProcessorEditor).  Runs the SDK round-trip on a background thread;
    // calls |callback| on the message thread when done.
    //
    // Signature: void callback(keylight::Result<keylight::State>)
    // -----------------------------------------------------------------------
    void activate(const juce::String&                                  key,
                  std::function<void(keylight::Result<keylight::State>)> callback)
    {
        std::string keyStd = key.toStdString();
        dispatch_([this, keyStd, cb = std::move(callback)]()
        {
            auto result = client_->activate(keyStd);
            juce::MessageManager::callAsync([result, cb]()
            {
                if (cb) cb(result);
            });
        });
    }

    // -----------------------------------------------------------------------
    // validate — call from the message thread.
    // -----------------------------------------------------------------------
    void validate(std::function<void(keylight::Result<keylight::State>)> callback)
    {
        dispatch_([this, cb = std::move(callback)]()
        {
            auto result = client_->validate();
            juce::MessageManager::callAsync([result, cb]()
            {
                if (cb) cb(result);
            });
        });
    }

    // -----------------------------------------------------------------------
    // deactivate — call from the message thread.
    // -----------------------------------------------------------------------
    void deactivate(std::function<void(keylight::Result<void>)> callback)
    {
        dispatch_([this, cb = std::move(callback)]()
        {
            auto result = client_->deactivate();
            juce::MessageManager::callAsync([result, cb]()
            {
                if (cb) cb(result);
            });
        });
    }

    // -----------------------------------------------------------------------
    // checkOnLaunch — call once from the message thread after construction
    // to refresh the cached state against the server (stale / near-expiry
    // policy).  Optional but recommended.
    // -----------------------------------------------------------------------
    void checkOnLaunch(std::function<void(keylight::Result<keylight::State>)> callback = {})
    {
        dispatch_([this, cb = std::move(callback)]()
        {
            auto result = client_->checkOnLaunch();
            juce::MessageManager::callAsync([result, cb]()
            {
                if (cb) cb(result);
            });
        });
    }

    // -----------------------------------------------------------------------
    // startAutoValidation / stopAutoValidation
    // Delegates to keylight::Client's built-in auto-validation thread
    // (interval configured via Config::autoValidationIntervalMs, default 30 min).
    // Call startAutoValidation() from the message thread after construction.
    // -----------------------------------------------------------------------
    void startAutoValidation() { client_->startAutoValidation(); }
    void stopAutoValidation()  { client_->stopAutoValidation();  }

    // -----------------------------------------------------------------------
    // AUDIO-THREAD-SAFE QUERY API
    //
    // These two methods are the ONLY ones safe to call from processBlock.
    // They read std::atomic fields — no lock, no allocation, no heap touch.
    // -----------------------------------------------------------------------

    /// Returns the current license state.  Audio-thread safe.
    keylight::State state() const noexcept
    {
        return state_snapshot_.load(std::memory_order_relaxed);
    }

    /// Returns true iff the named entitlement/feature is currently enabled.
    ///
    /// IMPORTANT: to remain audio-thread safe, this method uses a pre-cached
    /// std::atomic<bool> per-entitlement.  The per-feature atomics are
    /// refreshed from the message/background thread on every state change via
    /// the SDK subscription.
    ///
    /// For the common "pro" entitlement: call hasFeature("pro") — reads
    /// pro_enabled_ atomically.  For other entitlements, reads the generic
    /// snapshot (which caches the last hasEntitlement result for exactly the
    /// feature key last subscribed).
    ///
    /// If you gate on multiple entitlements, pre-cache each one in a separate
    /// std::atomic<bool> member via the subscription callback (see README).
    // NOTE: noexcept / lock-free guarantee is for RELEASE builds.  In JUCE
    // debug builds, juce::String comparison may invoke debug instrumentation
    // that allocates; only the atomics themselves are unconditionally lock-free.
    bool hasFeature(const juce::String& feature) const noexcept
    {
        // Fast path for the canonical "pro" entitlement — atomic bool.
        // This covers the vast majority of plugins that have a single pro tier.
        if (feature == "pro")
            return pro_enabled_.load(std::memory_order_relaxed);

        // Fallback: read generic cached entitlement flag.
        // Updated by refresh_entitlement_cache_ on state transitions.
        // Still lock-free (atomic bool).
        return generic_entitlement_enabled_.load(std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // Optional state-change callback.  Set this BEFORE calling checkOnLaunch
    // or activate.  Always fires on the message thread.
    //
    //   licensing.onStateChanged = [this](keylight::State s) { repaint(); };
    // -----------------------------------------------------------------------
    std::function<void(keylight::State)> onStateChanged;

    // -----------------------------------------------------------------------
    // hasEntitlement — message-thread version (goes to SDK, acquires mutex).
    // Use this for UI updates.  Do NOT call from processBlock.
    // -----------------------------------------------------------------------
    bool hasEntitlement(const std::string& feature) const
    {
        return client_->hasEntitlement(feature);
    }

    // -----------------------------------------------------------------------
    // underlying — access the raw Client for advanced usage.
    // Do NOT call SDK mutex-guarded methods from the audio thread.
    // -----------------------------------------------------------------------
    keylight::Client& underlying() { return *client_; }

private:
    // ── Alive flag (shared_ptr-to-atomic) ────────────────────────────────
    // Set to false in the destructor BEFORE unsubscribing or joining threads
    // so that any callAsync lambdas still queued on the message thread will
    // no-op rather than dereference a dangling this.
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);

    // ── Owned resources ───────────────────────────────────────────────────
    std::unique_ptr<JuceUrlTransport> transport_;
    keylight::FileStore               store_;
    std::unique_ptr<keylight::Client> client_;
    keylight::Subscription            subscription_;

    // ── Audio-thread-safe atomic snapshots ───────────────────────────────
    std::atomic<keylight::State> state_snapshot_{ keylight::State::Invalid };
    std::atomic<bool>            pro_enabled_{ false };
    std::atomic<bool>            generic_entitlement_enabled_{ false };

    // ── Background worker thread ──────────────────────────────────────────
    // One thread at a time for activate/validate/deactivate.
    // (The SDK's auto-validation thread is separate, owned by Client.)
    std::thread worker_thread_;

    // ── Helpers ───────────────────────────────────────────────────────────

    // Refresh entitlement atomics from the SDK (must NOT be called from the
    // audio thread — goes through the SDK's cache_mutex_ internally).
    void refresh_entitlement_cache_()
    {
        bool pro = client_->hasEntitlement("pro");
        pro_enabled_.store(pro, std::memory_order_relaxed);
        // generic_entitlement_enabled_ is not meaningful without a target feature;
        // it defaults to false until callers explicitly populate it.
        // (Advanced users: extend this pattern with their own atomics.)
    }

    // Join any running worker thread before launching a new one.
    void join_worker_()
    {
        if (worker_thread_.joinable())
            worker_thread_.join();
    }

    // Dispatch a callable to a background std::thread.
    // Joins the previous thread first (our operations are short, sequential).
    template <typename Fn>
    void dispatch_(Fn&& fn)
    {
        join_worker_();
        worker_thread_ = std::thread(std::forward<Fn>(fn));
    }

    // Derive the on-disk lease file path.
    static keylight::FileStore resolve_store_path_(const keylight::Config& cfg,
                                                   const juce::String&     override_path)
    {
        if (override_path.isNotEmpty())
            return keylight::FileStore(override_path.toStdString());

        // Default: <userApplicationDataDirectory>/Keylight/<tenantId>-<productId>.lease
        juce::File appData =
            juce::File::getSpecialLocation(
                juce::File::SpecialLocationType::userApplicationDataDirectory);

        juce::File storeDir = appData.getChildFile("Keylight");
        juce::String filename =
            juce::String(cfg.tenantId.c_str())
            + "-"
            + juce::String(cfg.productId.c_str())
            + ".lease";

        juce::File storeFile = storeDir.getChildFile(filename);
        return keylight::FileStore(storeFile.getFullPathName().toStdString());
    }
};

} // namespace juce_integration
} // namespace keylight
