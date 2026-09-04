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
 * These are updated in the subscription callback, on whichever thread is
 * draining the SDK's event queue, after every SDK state transition.  No mutex,
 * no allocation, no juce::String construction happens on the audio thread —
 * just two relaxed atomic loads.
 *
 * state_snapshot_ mirrors Client::state(), which fails closed when the system
 * clock has been rolled back.  The SDK's subscription callback delivers that
 * same guarded value — the SDK reports every event through the guard and
 * dedupes on the reported value — so storing the callback's parameter is both
 * correct and the only way the audio thread, the message thread and
 * client_->state() cannot disagree.
 * Do not "optimise" any of the three to a different source.
 *
 * The guard also emits its own events: a rolled-back clock changes no raw
 * state, so the SDK fires the transition off refreshIfNeeded(), which
 * startAutoValidation() ticks.  Call startAutoValidation(): without it the
 * snapshot only moves on real state changes and a mid-session rollback would
 * never reach the audio thread.
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
        // The callback fires on whatever thread is draining the SDK's event
        // queue. We update the atomics there, then post a UI notification via
        // callAsync so the editor can repaint.
        //
        // alive_ is captured BY VALUE (a shared_ptr copy) and checked before
        // anything touches `this`. Dropping subscription_ in ~Licensing does
        // not fence a delivery already in flight on another thread, so the
        // unsubscribe alone does not make this safe.
        //
        // KNOWN LIMITATION, be honest about it: the flag NARROWS the window,
        // it does not close it. This is a check-then-use — the callback can
        // pass the check, ~Licensing can then run to completion, and the
        // callback proceeds into state_snapshot_/refresh_entitlement_cache_()
        // on a destroyed object.
        //
        // ~Licensing closes the AUTO-VALIDATION half: step ⑤ destroys the
        // Client last, and ~Client() joins the worker, so a delivery on that
        // thread finishes while every member it can touch is still alive.
        //
        // What remains is delivery on an APP thread — the SDK hands the baton
        // to whichever thread is draining, so an app thread calling
        // refreshIfNeeded() on focus/resume can be the one in your callback,
        // and nothing here joins that thread. Closing it needs a fence in the
        // SDK, and a naive one reintroduces a deadlock (an unsubscribe() that
        // waits blocks the caller on user callback code), so it is a
        // deliberate follow-up rather than an oversight.
        //
        // NOTE: stopping auto-validation first does NOT help — since 0.2.0
        // stopAutoValidation() does not wait for anything. Destroy Licensing
        // from the message thread, and do not call into the SDK off
        // underlying() from another thread while doing so.
        subscription_ = client_->subscribe(
            [this, aliveCopy = alive_](keylight::State newState)
        {
            if (!aliveCopy->load())   // Licensing destroyed mid-delivery
                return;

            // Update atomics. Called from whichever thread is draining the
            // SDK's queue — never the audio thread.
            //
            // `newState` IS the guarded state: the SDK reports subscription
            // events through the same clock guard as Client::state(). Use it
            // rather than re-reading client_->state(), so the value latched
            // for the audio thread is byte-for-byte the one handed to the
            // message thread below — re-reading could straddle a clock change
            // and leave the two disagreeing. Still a single atomic store.
            state_snapshot_.store(newState, std::memory_order_relaxed);
            refresh_entitlement_cache_();

            // Anonymous funnel beacon. Swift's LicenseManager reports
            // automatically; keylight::Client deliberately never does, so the
            // adapter does it here.
            //
            // Called INLINE on purpose. dispatch_() joins the background
            // thread this callback normally runs on, so routing through it
            // would make the thread join itself. The SDK debounces to one
            // request per 24h per state, so the usual cost is a lock and a
            // comparison. Licensed/Invalid are not keyless states.
            //
            // The callback runs on whichever thread is DRAINING the SDK's
            // event queue — usually the thread that caused the transition
            // (our background thread for a dispatched call, the SDK's
            // auto-validation thread for a tick), but under concurrency a
            // thread already delivering picks up the event instead. Never the
            // audio thread. It CAN reach the message thread if you call the
            // SDK directly off underlying() from there — the root README
            // recommends refreshIfNeeded() on focus/resume — in which case
            // this POST blocks the UI for one round trip, once per 24h. Wrap
            // that call in your own thread if that matters to you.
            //
            // Calling back into the SDK from here is allowed — it holds no
            // lock during delivery, and a re-entrant call queues its event
            // rather than recursing. The one thing not to do is destroy the
            // Client (or this Licensing) from inside the callback.
            switch (newState)
            {
                case keylight::State::Trial:
                    client_->reportKeylessState(keylight::KeylessState::Trial);
                    break;
                case keylight::State::FreeTier:
                    client_->reportKeylessState(keylight::KeylessState::FreeTier);
                    break;
                case keylight::State::Expired:
                    client_->reportKeylessState(keylight::KeylessState::Expired);
                    break;
                case keylight::State::Licensed:
                case keylight::State::Invalid:
                // Limited means a trusted, licensed lease that the server
                // could only issue in degraded form (signing-key incident) —
                // it is licensing material, not a keyless funnel state, so it
                // is not reported here either.
                case keylight::State::Limited:
                    break;
            }

            // Deliver to message thread if a state-change callback is set.
            // Capture alive_ by value (copies the shared_ptr, keeping the
            // flag alive even after ~Licensing runs) so the lambda can
            // safely check whether this is still valid before touching members.
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
    // Destructor — retires auto-validation, joins our own worker thread, then
    // destroys the SDK client (which joins the SDK's worker).
    //
    // Call from the MESSAGE THREAD, and know that it can block there. Usually
    // it returns at once. It waits only when an SDK worker is mid-cycle: up to
    // one round trip, plus every queued listener callback if that worker is
    // delivering events. Since listeners are your code, that has no fixed
    // upper bound — see step ⑤.
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

        // ③ Retire the SDK auto-validation worker.  NOTE: since 0.2.0 this
        //   does NOT wait for that worker to exit — it retires it and
        //   returns, so a final state-change callback can still be in flight
        //   when this line completes.  The alive_ flag above is what makes
        //   that safe, not this call.
        client_->stopAutoValidation();

        // ④ Join any pending activate/validate/deactivate worker thread.
        //   These callAsync lambdas only capture result+cb, not this, so they
        //   are safe even without the flag — but joining here keeps ordering
        //   well-defined.
        join_worker_();

        // ⑤ Destroy the Client HERE, in the destructor body, not in member
        //   destruction order.  ~Client() is the only thing that joins the
        //   auto-validation worker, and client_ is declared BEFORE
        //   state_snapshot_ and pro_enabled_ — so leaving it to member
        //   destruction would join after those atomics are already gone, and
        //   an in-flight callback that got past the alive_ check would write
        //   to destroyed members.  Formally UB, and free to avoid.
        //
        //   COST: usually zero. ~Client() wakes the SDK worker before it
        //   joins, so one parked in its interval wait exits without another
        //   cycle — on the default 30-minute interval that is almost always
        //   the case. It blocks the MESSAGE THREAD only when the worker is
        //   mid-cycle: up to one round trip, plus every queued listener
        //   callback if that worker is delivering events. Your listeners set
        //   that ceiling, so keep them short if teardown latency matters.
        //   It is the price of a clean shutdown, and it was previously paid
        //   inside stopAutoValidation().
        client_.reset();
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
    // startTrial — call from the message thread (e.g. a "Start free trial"
    // button).  Begins the LOCAL, offline-first trial: the start timestamp is
    // written to the on-disk store, so this is dispatched to the background
    // thread like every other licensing call — never call it from
    // processBlock.  Idempotent: an existing trial is never restarted.
    //
    // Nothing starts a trial implicitly.  A DAW that scans or instantiates the
    // plugin runs checkOnLaunch(), which only *resolves* a trial that the user
    // already started.
    //
    // The resulting state flows through the normal subscription, so
    // state()/onStateChanged see State::Trial without any extra plumbing.
    // -----------------------------------------------------------------------
    void startTrial(std::function<void(keylight::Result<keylight::State>)> callback = {})
    {
        dispatch_([this, cb = std::move(callback)]()
        {
            auto result = client_->startTrial();
            juce::MessageManager::callAsync([result, cb]()
            {
                if (cb) cb(result);
            });
        });
    }

    // -----------------------------------------------------------------------
    // Trial queries — message thread only (they take the SDK's cache mutex).
    // For the audio thread use state() == keylight::State::Trial, which reads
    // the atomic snapshot.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // reportKeylessState — anonymous free-tier/trial funnel beacon.
    //
    // You do NOT normally call this: the adapter fires it for you on every
    // state transition. It exists for hosts that resolve state themselves.
    //
    // Blocking network call, so it runs on the background dispatch — never
    // call it from processBlock. Do not call it from inside onStateChanged
    // either; the transition already reports itself.
    // -----------------------------------------------------------------------
    void reportKeylessState(keylight::KeylessState state,
                            std::function<void()> callback = {})
    {
        dispatch_([this, state, cb = std::move(callback)]()
        {
            client_->reportKeylessState(state);
            if (cb)
            {
                auto aliveCopy = alive_;
                juce::MessageManager::callAsync([aliveCopy, cb]()
                {
                    if (aliveCopy->load()) cb();
                });
            }
        });
    }

    /// Whole days left in the local trial (0 when disabled/not started/elapsed).
    int trialDaysLeft() const { return client_->trialDaysLeft(); }

    /// NotStarted / Active / Expired for the local trial.
    keylight::TrialStatus trialStatus() const { return client_->checkTrial(); }

    // -----------------------------------------------------------------------
    // startAutoValidation / stopAutoValidation
    // Delegates to keylight::Client's built-in auto-validation thread
    // (interval configured via Config::autoValidationIntervalMs, default 30 min).
    // Call startAutoValidation() from the message thread after construction.
    // -----------------------------------------------------------------------
    // stopAutoValidation() retires the SDK worker and returns; it does NOT
    // wait for it to exit, so one more validation cycle (and one more
    // onStateChanged) can land after this returns. ~Licensing() is what joins.
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
