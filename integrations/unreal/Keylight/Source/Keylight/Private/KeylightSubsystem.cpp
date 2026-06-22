// Copyright Keylight. All Rights Reserved.
// KeylightSubsystem.cpp — UGameInstanceSubsystem wrapping keylight::Client.

#include "KeylightSubsystem.h"
#include "FHttpTransport.h"

// SDK public headers (include path set in Keylight.Build.cs)
#include "keylight/client.hpp"
#include "keylight/config.hpp"
#include "keylight/store.hpp"
#include "keylight/result.hpp"

#include "Async/Async.h"              // AsyncTask(ENamedThreads::...)
#include "Misc/Paths.h"               // FPaths::ProjectSavedDir()
#include "Misc/App.h"                 // FApp::GetProjectName()

#include <string>
#include <map>

// ---------------------------------------------------------------------------
// UELicenseStore — LicenseStore that persists the lease blob to UE Saved/
// ---------------------------------------------------------------------------
namespace
{

/**
 * Concrete LicenseStore that reads/writes a single file under
 * FPaths::ProjectSavedDir() / "Keylight" / "<tenant>-<product>.lease".
 * Uses the SDK's FileStore internally to get atomic-rename semantics.
 */
class UELicenseStore final : public keylight::LicenseStore
{
public:
    explicit UELicenseStore(const FString& TenantId, const FString& ProductId)
    {
        FString Filename = FString::Printf(TEXT("%s-%s.lease"), *TenantId, *ProductId);
        FString Dir      = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Keylight"));
        FString FullPath = FPaths::Combine(Dir, Filename);

        // Build the underlying FileStore with the resolved path.
        std::string StdPath(TCHAR_TO_UTF8(*FullPath));
        Inner_ = std::make_unique<keylight::FileStore>(std::move(StdPath));
    }

    keylight::Result<std::string> load() override { return Inner_->load(); }
    keylight::Result<void>        save(const std::string& data) override { return Inner_->save(data); }
    keylight::Result<void>        clear() override { return Inner_->clear(); }

private:
    std::unique_ptr<keylight::FileStore> Inner_;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// UKeylightSubsystem implementation
// ---------------------------------------------------------------------------

void UKeylightSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    // Client_ is not created until Configure() is called.
}

void UKeylightSubsystem::Deinitialize()
{
    // Stop background auto-validation before tearing down the client.
    if (Client_)
    {
        Client_->stopAutoValidation();
        Client_.Reset();
    }
    Store_.Reset();
    Transport_.Reset();
    bConfigured_ = false;

    Super::Deinitialize();
}

// ---------------------------------------------------------------------------

void UKeylightSubsystem::Configure(
    const FString&              TenantId,
    const FString&              ProductId,
    const TMap<FString, FString>& TrustedKeys)
{
    // Rebuild transport / store / client on each Configure() call so the
    // subsystem can be reconfigured if needed (e.g. multi-step onboarding).
    if (Client_)
    {
        Client_->stopAutoValidation();
        Client_.Reset();
    }
    Store_.Reset();
    Transport_.Reset();

    // ── Build SDK Config ──────────────────────────────────────────────────
    keylight::Config Cfg;
    Cfg.tenantId  = std::string(TCHAR_TO_UTF8(*TenantId));
    Cfg.productId = std::string(TCHAR_TO_UTF8(*ProductId));

    for (const auto& Pair : TrustedKeys)
    {
        Cfg.trustedKeys.emplace(
            std::string(TCHAR_TO_UTF8(*Pair.Key)),
            std::string(TCHAR_TO_UTF8(*Pair.Value)));
    }

    Cfg.appVersion = std::string(TCHAR_TO_UTF8(FApp::GetBuildVersion()));

    // ── Construct dependencies ────────────────────────────────────────────
    Transport_ = MakeUnique<FHttpTransport>();
    Store_     = MakeUnique<UELicenseStore>(TenantId, ProductId);
    Client_    = MakeUnique<keylight::Client>(std::move(Cfg), *Transport_, *Store_);

    bConfigured_ = true;
}

// ---------------------------------------------------------------------------
// Activate
// ---------------------------------------------------------------------------

void UKeylightSubsystem::Activate(const FString& Key)
{
    if (!bConfigured_ || !Client_)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("UKeylightSubsystem::Activate called before Configure()"));
        OnActivateComplete.Broadcast(false, EKeylightState::Invalid,
                                     TEXT("Not configured. Call Configure() first."));
        return;
    }

    // Capture key as std::string for the lambda (FString is not thread-safe).
    std::string StdKey(TCHAR_TO_UTF8(*Key));
    // Raw pointer is safe: the subsystem outlives the async task (Deinitialize
    // calls stopAutoValidation which joins the background thread, and the
    // UGameInstance lifetime gates everything).
    keylight::Client* ClientPtr = Client_.Get();

    // Capture 'this' for the game-thread dispatch; the subsystem is a UObject
    // and will be alive when the game-thread task runs (UE GC is cooperative).
    UKeylightSubsystem* Self = this;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
    [ClientPtr, StdKey, Self]()
    {
        auto Result = ClientPtr->activate(StdKey);

        bool       bSuccess = Result.is_ok();
        int        StateInt = bSuccess
                                ? static_cast<int>(Result.value())
                                : static_cast<int>(keylight::State::Invalid);
        FString    Msg      = bSuccess
                                ? TEXT("")
                                : FString(UTF8_TO_TCHAR(Result.error().message.c_str()));

        // Marshal to game thread before firing the Blueprint delegate.
        AsyncTask(ENamedThreads::GameThread,
        [Self, bSuccess, StateInt, Msg]()
        {
            Self->OnActivateComplete.Broadcast(
                bSuccess,
                UKeylightSubsystem::ToUEState(StateInt),
                Msg);
        });
    });
}

// ---------------------------------------------------------------------------
// Validate
// ---------------------------------------------------------------------------

void UKeylightSubsystem::Validate()
{
    if (!bConfigured_ || !Client_)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("UKeylightSubsystem::Validate called before Configure()"));
        OnValidateComplete.Broadcast(false, EKeylightState::Invalid,
                                     TEXT("Not configured. Call Configure() first."));
        return;
    }

    keylight::Client*    ClientPtr = Client_.Get();
    UKeylightSubsystem*  Self      = this;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
    [ClientPtr, Self]()
    {
        auto Result = ClientPtr->validate();

        bool    bSuccess = Result.is_ok();
        int     StateInt = bSuccess
                             ? static_cast<int>(Result.value())
                             : static_cast<int>(keylight::State::Invalid);
        FString Msg      = bSuccess
                             ? TEXT("")
                             : FString(UTF8_TO_TCHAR(Result.error().message.c_str()));

        AsyncTask(ENamedThreads::GameThread,
        [Self, bSuccess, StateInt, Msg]()
        {
            Self->OnValidateComplete.Broadcast(
                bSuccess,
                UKeylightSubsystem::ToUEState(StateInt),
                Msg);
        });
    });
}

// ---------------------------------------------------------------------------
// Deactivate
// ---------------------------------------------------------------------------

void UKeylightSubsystem::Deactivate()
{
    if (!bConfigured_ || !Client_)
    {
        UE_LOG(LogTemp, Warning,
               TEXT("UKeylightSubsystem::Deactivate called before Configure()"));
        OnDeactivateComplete.Broadcast(false, EKeylightState::Invalid,
                                       TEXT("Not configured. Call Configure() first."));
        return;
    }

    keylight::Client*    ClientPtr = Client_.Get();
    UKeylightSubsystem*  Self      = this;

    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
    [ClientPtr, Self]()
    {
        auto Result = ClientPtr->deactivate();

        bool    bSuccess = Result.is_ok();
        // After deactivate the state is always Invalid.
        int     StateInt = static_cast<int>(keylight::State::Invalid);
        FString Msg      = bSuccess
                             ? TEXT("")
                             : FString(UTF8_TO_TCHAR(Result.error().message.c_str()));

        AsyncTask(ENamedThreads::GameThread,
        [Self, bSuccess, StateInt, Msg]()
        {
            Self->OnDeactivateComplete.Broadcast(
                bSuccess,
                UKeylightSubsystem::ToUEState(StateInt),
                Msg);
        });
    });
}

// ---------------------------------------------------------------------------
// HasEntitlement / GetState (sync, game-thread safe)
// ---------------------------------------------------------------------------

bool UKeylightSubsystem::HasEntitlement(const FString& Feature) const
{
    if (!Client_) return false;
    std::string StdFeature(TCHAR_TO_UTF8(*Feature));
    return Client_->hasEntitlement(StdFeature);
}

EKeylightState UKeylightSubsystem::GetState() const
{
    if (!Client_) return EKeylightState::Invalid;
    return ToUEState(static_cast<int>(Client_->state()));
}

// ---------------------------------------------------------------------------
// ToUEState — static helper
// ---------------------------------------------------------------------------

/*static*/ EKeylightState UKeylightSubsystem::ToUEState(int NativeState)
{
    // keylight::State enum values in declaration order:
    //   0 = Licensed, 1 = Trial, 2 = Expired, 3 = Invalid
    switch (static_cast<keylight::State>(NativeState))
    {
        case keylight::State::Licensed: return EKeylightState::Licensed;
        case keylight::State::Trial:    return EKeylightState::Trial;
        case keylight::State::Expired:  return EKeylightState::Expired;
        default:                        return EKeylightState::Invalid;
    }
}
