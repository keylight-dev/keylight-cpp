// Copyright Keylight. All Rights Reserved.
// KeylightSubsystem.h — Blueprint-callable UGameInstanceSubsystem wrapping the
//                        Keylight C++ SDK client.
//
// Lifetime: one instance per UGameInstance (created/destroyed by UE automatically).
//
// Threading contract:
//   • Activate/Validate/Deactivate each dispatch a background async task via
//     AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, ...) so the SDK's
//     synchronous Transport::request() never blocks the game thread.
//   • Results are always delivered to the GAME THREAD via
//     AsyncTask(ENamedThreads::GameThread, ...) before the delegate fires.
//   • HasEntitlement / GetState are lightweight reads (one mutex lock / one
//     atomic load in the SDK) and are safe to call from the game thread directly.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

// Forward-declare the SDK types so consumers who include only this header
// do not need to pull in <keylight/client.hpp> directly.
namespace keylight { class Client; class Transport; class LicenseStore; }

#include "KeylightSubsystem.generated.h"

// ---------------------------------------------------------------------------
// EKeylightState — mirrors keylight::State (used in Blueprint)
// ---------------------------------------------------------------------------
UENUM(BlueprintType)
enum class EKeylightState : uint8
{
    Licensed  UMETA(DisplayName = "Licensed"),
    Trial     UMETA(DisplayName = "Trial"),
    Expired   UMETA(DisplayName = "Expired"),
    Invalid   UMETA(DisplayName = "Invalid"),
};

// ---------------------------------------------------------------------------
// FOnKeylightResult — async completion delegate
//   bSuccess  true if the SDK call completed without a transport error
//   State     resulting license state
//   Message   human-readable error message (empty on success)
// ---------------------------------------------------------------------------
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
    FOnKeylightResult,
    bool,            bSuccess,
    EKeylightState,  State,
    const FString&,  Message
);

// ---------------------------------------------------------------------------
// UKeylightSubsystem
// ---------------------------------------------------------------------------
UCLASS(ClassGroup = "Keylight", meta = (DisplayName = "Keylight Subsystem"))
class KEYLIGHT_API UKeylightSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    /**
     * Destructor — declared here but defined (defaulted) out-of-line in the
     * .cpp so the TUniquePtr members below (which hold forward-declared SDK
     * types) are destroyed where their full definitions are visible. Without
     * this, the implicit destructor could be instantiated against incomplete
     * types, silently skipping ~Client() (which joins the auto-validation
     * thread) or failing the build under warnings-as-errors.
     */
    virtual ~UKeylightSubsystem() override;

    // ── UGameInstanceSubsystem ────────────────────────────────────────────

    /** Called by UE when the subsystem is created (game instance starts). */
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    /** Called by UE when the subsystem is torn down (game instance ends). */
    virtual void Deinitialize() override;

    // ── Configuration ─────────────────────────────────────────────────────

    /**
     * Configure the subsystem before calling Activate/Validate.
     * Must be called once (e.g. in BeginPlay or at startup) with your
     * Keylight tenant and product identifiers from the dashboard.
     *
     * @param TenantId    Your Keylight tenant ID.
     * @param ProductId   Your product ID within that tenant.
     * @param TrustedKeys Map of keyId → base64-encoded Ed25519 public key.
     *                    Obtain from the Keylight dashboard.  Pass an empty
     *                    map during initial integration testing (signature
     *                    verification will still run but no key is trusted →
     *                    state will be Invalid even on a valid activation).
     */
    UFUNCTION(BlueprintCallable, Category = "Keylight",
              meta = (DisplayName = "Configure Keylight"))
    void Configure(const FString& TenantId,
                   const FString& ProductId,
                   const TMap<FString, FString>& TrustedKeys);

    // ── Async operations ──────────────────────────────────────────────────

    /**
     * Activate a license key for this device asynchronously.
     * Fires OnActivateComplete on the game thread when done.
     * @param Key  The license key entered by the user.
     */
    UFUNCTION(BlueprintCallable, Category = "Keylight",
              meta = (DisplayName = "Activate License"))
    void Activate(const FString& Key);

    /**
     * Validate the stored license online asynchronously.
     * Fires OnValidateComplete on the game thread when done.
     */
    UFUNCTION(BlueprintCallable, Category = "Keylight",
              meta = (DisplayName = "Validate License"))
    void Validate();

    /**
     * Deactivate this device asynchronously (best-effort network call, then
     * clears local state regardless).
     * Fires OnDeactivateComplete on the game thread when done.
     */
    UFUNCTION(BlueprintCallable, Category = "Keylight",
              meta = (DisplayName = "Deactivate License"))
    void Deactivate();

    // ── Sync queries (safe on game thread) ───────────────────────────────

    /**
     * Returns true if the current verified lease contains the named
     * entitlement feature.  Reads cached state — does NOT hit the network.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Keylight",
              meta = (DisplayName = "Has Entitlement"))
    bool HasEntitlement(const FString& Feature) const;

    /**
     * Returns the current license state.  Reads an atomic — audio-thread safe.
     */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Keylight",
              meta = (DisplayName = "Get License State"))
    EKeylightState GetState() const;

    // ── Delegates ─────────────────────────────────────────────────────────

    /** Fired on the game thread when Activate() completes. */
    UPROPERTY(BlueprintAssignable, Category = "Keylight")
    FOnKeylightResult OnActivateComplete;

    /** Fired on the game thread when Validate() completes. */
    UPROPERTY(BlueprintAssignable, Category = "Keylight")
    FOnKeylightResult OnValidateComplete;

    /** Fired on the game thread when Deactivate() completes. */
    UPROPERTY(BlueprintAssignable, Category = "Keylight")
    FOnKeylightResult OnDeactivateComplete;

private:
    // The transport and store are heap-allocated with new/delete so we can
    // control their lifetimes independently of UObject GC.
    // (They are plain C++ objects, not UObjects.)
    TUniquePtr<keylight::Transport>     Transport_;
    TUniquePtr<keylight::LicenseStore>  Store_;
    TUniquePtr<keylight::Client>        Client_;

    // Guard against calling Activate/Validate before Configure().
    bool bConfigured_ = false;

    /** Helper: convert SDK keylight::State → EKeylightState. */
    static EKeylightState ToUEState(int NativeState);
};
