# Keylight — Unreal Engine Plugin

Adds Keylight license management to your UE 5.x game/app with zero extra
dependencies.  Networking goes through UE's built-in `FHttpModule`; no
OpenSSL, no cpp-httplib, nothing extra to vendor.

> **Manual verification pending:** the plugin is written against the UE 5.x
> API and has been self-reviewed for correctness, but it has not yet been
> compiled inside a real UE project.  A manual editor build is required before
> shipping (see [Verification](#verification) below).

---

## Minimum requirements

| Requirement        | Value                        |
|--------------------|------------------------------|
| Unreal Engine      | 5.0 or later                 |
| C++ standard       | C++17 (UE 5.x default)       |
| Keylight C++ SDK   | any version after 0.1.0      |
| Target platforms   | Windows, macOS, Linux (any platform FHttpModule supports) |

---

## Installation

### 1. Get the C++ SDK headers

The plugin uses the Keylight C++ SDK headers to talk to `keylight::Client`.
The SDK is **header-only at this boundary** (no static lib to link):

```
# Clone (or copy) the C++ SDK into your project:
git clone https://github.com/keylight-dev/keylight-cpp \
    <YourProject>/ThirdParty/keylight-cpp
```

Expected layout:

```
<YourProject>/
├── Plugins/
│   └── Keylight/               ← this plugin
└── ThirdParty/
    └── keylight-cpp/
        └── include/
            └── keylight/
                ├── client.hpp
                ├── transport.hpp
                └── ...
```

The `Keylight.Build.cs` file resolves the include path automatically when the
SDK is at `ThirdParty/keylight-cpp` relative to the project root.  To override
(e.g. in CI or a monorepo layout), set:

```
# Environment variable — picked up by Keylight.Build.cs at build time
export KEYLIGHT_SDK_INCLUDE_PATH=/absolute/path/to/keylight-cpp/include
```

Or edit the path directly in `Keylight.Build.cs`:

```csharp
string KeylightSdkInclude = Path.Combine(ModuleDirectory,
    "..", "..", "..", "..", "ThirdParty", "keylight-cpp", "include");
// ↑ adjust the relative path to match your layout
```

### 2. Copy the plugin

```
cp -r integrations/unreal/Keylight  <YourProject>/Plugins/Keylight
```

### 3. Enable the plugin

In your `.uproject` file:

```json
{
  "Plugins": [
    {
      "Name": "Keylight",
      "Enabled": true
    }
  ]
}
```

Or enable it in the **Unreal Editor → Edit → Plugins → Licensing → Keylight**.

### 4. Add the module dependency

In your game module's `Build.cs`:

```csharp
PrivateDependencyModuleNames.AddRange(new string[]
{
    "Keylight",
});
```

---

## Usage

### From Blueprint

1. In any Blueprint, get the **Keylight Subsystem**:  
   `Get Game Instance Subsystem → UKeylightSubsystem`
2. Call **Configure Keylight** once at startup with your Tenant ID, Product
   ID, and trusted public keys (from the Keylight dashboard).
3. Bind to **On Activate Complete** / **On Validate Complete** /
   **On Deactivate Complete** before calling the async functions.
4. Call **Activate License**, **Validate License**, or **Deactivate License**.
5. Query **Get License State** and **Has Entitlement** synchronously whenever
   you need to gate content.

### From C++

```cpp
// GameMode.cpp (or wherever you initialise licensing)

#include "KeylightSubsystem.h"

// On game start:
UKeylightSubsystem* KL = GetGameInstance()
    ->GetSubsystem<UKeylightSubsystem>();

TMap<FString, FString> Keys;
Keys.Add(TEXT("kid-1"), TEXT("<base64-Ed25519-public-key>"));

KL->Configure(TEXT("your-tenant-id"), TEXT("your-product-id"), Keys);

// Bind completion delegate (C++ lambda):
KL->OnActivateComplete.AddLambda(
    [](bool bSuccess, EKeylightState State, const FString& Message)
    {
        if (bSuccess && State == EKeylightState::Licensed)
        {
            UE_LOG(LogTemp, Log, TEXT("License activated!"));
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Activate failed: %s"), *Message);
        }
    });

// Fire activation (call from UI button handler):
KL->Activate(TEXT("XXXX-XXXX-XXXX-XXXX"));

// Later — gate a feature:
if (KL->HasEntitlement(TEXT("pro_tier")))
{
    // unlock pro content
}
```

### Auto-validation

The subsystem does not start background auto-validation automatically.
Call it explicitly after `Configure()`:

```cpp
// Not exposed to Blueprint (runs on the SDK's internal thread):
KL->Client_->startAutoValidation();  // internal — see note below
```

> Note: `Client_` is declared `private` in the header.  If you need
> auto-validation, expose a `StartAutoValidation()` BlueprintCallable method
> — the one-line implementation is `Client_->startAutoValidation()`.

---

## Threading contract

| Method                               | Allowed threads      |
|--------------------------------------|----------------------|
| `Configure()`                        | Game thread only     |
| `Activate()` / `Validate()` / `Deactivate()` | Game thread (returns immediately, dispatches async) |
| `OnActivateComplete` delegate fires  | **Game thread** always |
| `GetState()`                         | Any thread (atomic read) |
| `HasEntitlement()`                   | Any thread (mutex-guarded read) |

`FHttpTransport::request()` is called on a UE background worker thread and
blocks that thread (via `FEvent::Wait()`) until the HTTP response arrives.
The **game thread is never blocked**.

---

## Verification

> **Manual verification pending** — compile in a UE 5.x project.

The plugin code is written against the UE 5.x public API
(`FHttpModule`, `IHttpRequest`, `IHttpResponse`, `FPlatformProcess::GetSynchEventFromPool`,
`AsyncTask(ENamedThreads::...)`, `UGameInstanceSubsystem`, `UFUNCTION`/`UPROPERTY`/`UENUM` macros)
and the Keylight C++ SDK public API (`keylight::Client`, `keylight::Transport`,
`keylight::Result`, `keylight::Config`, `keylight::FileStore`).

No UE toolchain is present in the keylight-cpp CI environment.  Before
shipping to players, a developer with UE 5.x installed must:

1. Follow the installation steps above.
2. Build the project (Development or Shipping) in the UE Editor or via UAT.
3. Confirm: plugin compiles cleanly, `UKeylightSubsystem` appears in the
   Subsystems panel, Blueprint nodes are visible, and a round-trip
   `Activate → GetState` returns `Licensed` against the staging API.

---

## License store location

The lease blob is stored at:

```
<SavedDir>/Keylight/<tenantId>-<productId>.lease
```

`FPaths::ProjectSavedDir()` expands to:
- **Windows:** `%LOCALAPPDATA%\<ProjectName>\Saved\`
- **macOS/Linux:** `~/Library/Application Support/<ProjectName>/Saved/` (macOS)
  or `~/.local/share/<ProjectName>/Saved/` (Linux)

The `.lease` file is written atomically (temp → rename) via the SDK's
`FileStore`.
