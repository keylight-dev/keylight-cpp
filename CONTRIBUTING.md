# Contributing to the Keylight C++ SDK

This document covers how to build, test, and release the SDK. For the public API and
integration guides see [README.md](README.md).

## Build & Test

### Default build (no external dependencies)

The core library is header-only and has no mandatory external dependencies.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

This runs the full test suite including the conformance vectors.

### With the optional httplib transport

The bundled [cpp-httplib](https://github.com/yhirose/cpp-httplib) + OpenSSL transport is
opt-in. Requires OpenSSL ≥ 3 installed (e.g. `brew install openssl@3` on macOS).

```bash
cmake -B build \
  -DKEYLIGHT_BUILD_HTTPLIB_TRANSPORT=ON \
  -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3
cmake --build build
ctest --test-dir build --output-on-failure
```

### Live integration tests

Live tests hit the public Keylight demo tenant and require a network connection.
They are skipped by default and must be opted in explicitly:

```bash
KEYLIGHT_LIVE=1 ctest --test-dir build --output-on-failure -R live
```

## Regenerating the single-header amalgamation

`keylight_single.hpp` is the pre-generated amalgamation of all headers under
`include/keylight/`. Regenerate it after any header change:

```bash
python3 tools/amalgamate.py
```

The CI `amalgamation` job checks that `keylight_single.hpp` matches the regenerated output
and fails if it's out of sync. Always commit the updated `keylight_single.hpp` alongside
header changes.

## Syncing conformance vectors

The cross-SDK conformance vectors are canonical — they are shared with the Swift, Rust,
JavaScript, and C# SDKs and must never be modified unilaterally.

To pull in the latest frozen vectors from the canonical source:

```bash
./scripts/sync-vectors.sh
```

After syncing, rebuild and run the conformance tests to confirm the C++ verifier still
passes all vectors before committing.

## Release process

### Version bump checklist

Before tagging a release, update the version in all of these places:

1. `include/keylight/version.hpp` — `KEYLIGHT_SDK_VERSION` macro
2. `vcpkg.json` — `"version"` field
3. `conanfile.py` — `version` attribute
4. `CHANGELOG.md` — add a `## [x.y.z] - YYYY-MM-DD` entry describing the changes

Commit everything with a message like `chore: bump to v0.1.1`.

### Tagging

Push a tag matching `v*` to trigger the release workflow:

```bash
git tag v0.1.1
git push origin v0.1.1
```

### What the release workflow does

The `.github/workflows/release.yml` workflow:

1. Runs the full build + conformance test matrix on Ubuntu, macOS, and Windows.
2. Regenerates `keylight_single.hpp` and verifies it matches the committed copy.
3. Creates a GitHub Release named after the tag and attaches `keylight_single.hpp`
   as a release artifact.

> C++ has no central package registry equivalent to crates.io, npm, or NuGet. Distribution
> is via **git tag + CMake FetchContent** and the `keylight_single.hpp` release artifact.
> vcpkg and Conan registry submissions are out of scope for v1.

### CHANGELOG entry format

Follow the [Keep a Changelog](https://keepachangelog.com/en/1.0.0/) conventions. Keep entries
SDK-side: factory parameters, lease/state machine behavior, verifier semantics, public API
surface, adapter changes, migration notes. Do **not** include Keylight platform/operator/billing
changes that have no impact on SDK code.

## Code style

- C++17. No exceptions in the core library (errors surface as `Result<T>`).
- Header-only core: no `.cpp` files under `include/`. All implementation stays in headers.
- Zero external dependencies in the core. Optional dependencies (httplib, OpenSSL) are
  strictly gated behind `KEYLIGHT_BUILD_HTTPLIB_TRANSPORT`.
- Follow existing naming: `camelCase` for methods, `snake_case` for local variables and
  private fields with trailing `_`.
