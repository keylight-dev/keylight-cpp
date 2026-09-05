#!/usr/bin/env python3
"""Regenerate the signed-config test fixtures in tests/test_client.cpp.

The SDK verifies Ed25519 but never signs — signing is the server's job — so the
fixtures for "a valid signature is trusted" cannot be produced by the test suite
itself. This script produces them.

    python3 tools/gen_config_fixtures.py

Requires `cryptography` (pip install cryptography). It is NOT a build or test
dependency: the generated constants are checked into tests/test_client.cpp, and
CI never runs this.

THE KEY BELOW IS TEST-ONLY. It is generated from the fixed seed 0x00..07, it is
published here on purpose so the fixtures are reproducible, and it signs nothing
real. It must never appear in a tenant keyset. A private key in a public repo is
only safe because this one guards nothing — do not follow the pattern with a key
that does.

Run this after changing config_canonical_payload()'s format, and paste the
output over the CFG_* constants in tests/test_client.cpp. The literal format
assertions in that file are what catch a format change; these signatures are
what catch a change that also breaks verification.
"""

import base64

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

# Test-only seed. See the module docstring.
SEED = bytes.fromhex("00" * 31 + "07")

# Must match config_signing_cfg() in tests/test_client.cpp — tenant and product
# are inside the signed payload, so fixtures are only valid for these values.
KID = "kcfg"
TENANT = "testco"
PRODUCT = "testapp"


def canonical(kid, tenant, product, issued, expires, trial_days, free_tier):
    """Mirror of keylight::config_canonical_payload (config_payload.hpp)."""
    return (
        f"cfg1|{kid}|{tenant}|{product}|{issued}|{expires}|{trial_days}|"
        f"{'true' if free_tier else 'false'}"
    )


def main():
    sk = Ed25519PrivateKey.from_private_bytes(SEED)
    pk = sk.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw
    )

    print("// CFG_PUBKEY")
    print(base64.b64encode(pk).decode())
    print()

    cases = [
        ("CFG_SIG_VALID", KID, TENANT, PRODUCT, 1781076246, 1781680000, 14, True),
        # A real signature for the WRONG product: proves tenant/product being
        # inside the payload is what stops a cross-product replay.
        ("CFG_SIG_OTHER_PRODUCT", KID, TENANT, "otherapp", 1781076246, 1781680000, 14, True),
        # Genuinely signed but long expired: proves freshness is enforced on
        # the wire, so an old longer-trial config cannot be replayed forever.
        ("CFG_SIG_EXPIRED", KID, TENANT, PRODUCT, 1700000000, 1700600000, 30, True),
    ]

    for name, *args in cases:
        payload = canonical(*args)
        sig = base64.b64encode(sk.sign(payload.encode())).decode()
        print(f"// {name}")
        print(f"//   payload = {payload}")
        print(f"{sig}")
        print()


if __name__ == "__main__":
    main()
