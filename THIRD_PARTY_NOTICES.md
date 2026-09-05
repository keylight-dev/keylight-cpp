# Third-Party Notices

The Keylight C++ SDK vendors the following third-party components. Their licenses are
reproduced below to satisfy attribution requirements.

One entry — ChaCha20-Poly1305 — is listed even though it is *not* third-party.
Vendored-looking crypto invites the assumption that it was copied from
somewhere; recording that it was not is cheaper than having someone re-derive
the answer during a license audit.

---

## TweetNaCl — Ed25519 verification + SHA-512

**Source:** https://tweetnacl.cr.yp.to/  
**Used in:** `include/keylight/ed25519.hpp` (verification path only; signing and
key-generation are not included)  
**License:** Public domain

> Original authors: Daniel J. Bernstein, Bernard van Gastel, Wesley Janssen, Tanja Lange,
> Peter Schwabe, Sjaak Smetsers.
>
> TweetNaCl is in the public domain. The authors make no copyright claim.

---

## SHA-256 (Brad Conte)

**Source:** https://github.com/B-Con/crypto-algorithms  
**Used in:** `include/keylight/sha256.hpp`  
**License:** CC0 / Public domain

> Brad Conte's SHA-256 implementation is released under CC0 (Creative Commons Zero),
> effectively placing it in the public domain with no attribution required.
> Attribution retained for clarity.

---

## ChaCha20-Poly1305 — no third-party code

**Used in:** `include/keylight/chacha20poly1305.hpp`
**License:** n/a — original implementation

This one is listed precisely because it is *not* third-party, so nobody has to
re-derive that later. `chacha20poly1305.hpp` is an original implementation of
RFC 8439 (ChaCha20, Poly1305, and the AEAD construction), written against the
specification. No code was copied from any implementation, so there is nothing
to attribute and no license obligation attaches.

Conformance is pinned to the RFC's own published test vectors in
`tests/test_chacha20poly1305.cpp`.

RFC 8439 itself is an IETF specification. IETF documents carry no restriction
on implementing what they describe.

---

## cpp-httplib (Yuji Hirose)

**Source:** https://github.com/yhirose/cpp-httplib  
**Used in:** `third_party/httplib.h` and `include/keylight/transport/httplib.hpp`
(opt-in only; not compiled unless `KEYLIGHT_BUILD_HTTPLIB_TRANSPORT=ON`)  
**License:** MIT

```
Copyright (c) 2024 Yuji Hirose. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## doctest (Viktor Kirilov)

**Source:** https://github.com/doctest/doctest  
**Used in:** test files only; not compiled into the SDK library  
**License:** MIT

```
The MIT License (MIT)

Copyright (c) 2016-2023 Viktor Kirilov

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
