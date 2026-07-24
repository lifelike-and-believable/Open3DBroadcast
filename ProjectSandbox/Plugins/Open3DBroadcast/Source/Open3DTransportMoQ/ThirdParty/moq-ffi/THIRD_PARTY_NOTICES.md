# Third-Party Notices — moq_ffi.dll

`moq_ffi.dll` is a statically linked Rust `cdylib`. Everything listed here is
compiled **into** the DLL that ships with this plugin; none of it is a runtime
dependency that a user could substitute.

## How this list was produced

Crate versions were recovered by reading the Cargo registry paths that `rustc`
embeds in the binary
(`…\index.crates.io-*\<crate>-<version>\…`), then each version's `license` field
was read from the crates.io API. The native components in §3 were confirmed by
locating their symbols directly in the shipped DLL.

⚠️ **This is a stopgap, not a build product.** It was reconstructed from binary
inspection after the fact, and it will drift silently the next time the DLL is
rebuilt. See "Maintaining this file" at the end.

---

## 1. Rust crates

All 35 crates below were recovered from the shipped DLL and their SPDX
identifiers verified against crates.io. None are yanked.

| Crate | Version | License (SPDX) |
| --- | --- | --- |
| aws-lc-rs | 1.15.1 | **ISC AND (Apache-2.0 OR ISC)** (see §2) |
| aws-lc-sys | 0.34.0 | **ISC AND (Apache-2.0 OR ISC) AND OpenSSL** (see §2) |
| bytes | 1.11.0 | MIT |
| futures-core | 0.3.31 | MIT OR Apache-2.0 |
| futures-util | 0.3.31 | MIT OR Apache-2.0 |
| http | 1.3.1 | MIT OR Apache-2.0 |
| icu_collections | 2.1.1 | **Unicode-3.0** |
| icu_normalizer | 2.1.1 | **Unicode-3.0** |
| idna | 1.1.0 | MIT OR Apache-2.0 |
| lru-slab | 0.1.2 | MIT OR Apache-2.0 OR Zlib |
| mio | 1.1.0 | MIT |
| once_cell | 1.21.3 | MIT OR Apache-2.0 |
| percent-encoding | 2.3.2 | MIT OR Apache-2.0 |
| quinn | 0.11.9 | MIT OR Apache-2.0 |
| quinn-proto | 0.11.13 | MIT OR Apache-2.0 |
| quinn-udp | 0.5.14 | MIT OR Apache-2.0 |
| rand | 0.9.2 | MIT OR Apache-2.0 |
| rand_chacha | 0.9.0 | MIT OR Apache-2.0 |
| ring | 0.17.14 | **Apache-2.0 AND ISC** (see note) |
| rustls | 0.23.35 | Apache-2.0 OR ISC OR MIT |
| rustls-native-certs | 0.8.2 | Apache-2.0 OR ISC OR MIT |
| rustls-pki-types | 1.13.0 | MIT OR Apache-2.0 |
| rustls-webpki | 0.103.8 | ISC |
| schannel | 0.1.28 | MIT |
| slab | 0.4.11 | MIT |
| smallvec | 1.15.1 | MIT OR Apache-2.0 |
| socket2 | 0.6.1 | MIT OR Apache-2.0 |
| tinyvec | 1.10.0 | Zlib OR Apache-2.0 OR MIT |
| tokio | 1.48.0 | MIT |
| tracing-core | 0.1.34 | MIT |
| untrusted | 0.9.0 | ISC |
| url | 2.5.7 | MIT OR Apache-2.0 |
| web-transport | 0.3.1 | MIT |
| web-transport-proto | 0.2.8 | MIT OR Apache-2.0 |
| web-transport-quinn | 0.3.4 | MIT OR Apache-2.0 |

### Note on `ring`

`ring` declares `Apache-2.0 AND ISC` — a **conjunctive** AND, not the usual
`OR`. Both sets of terms apply at once, and the crate's top-level `LICENSE` is a
pointer file rather than a grant: new code is ISC (`LICENSE-other-bits`), code
sourced from BoringSSL is Apache-2.0 (`LICENSE-BoringSSL`), the `once_cell`
polyfill is dual Apache-2.0/MIT, and `third_party/fiat` is Apache-2.0. All five
files should be reproduced.

### Note on `icu_collections` / `icu_normalizer`

Unicode-3.0 is a distinct license (not MIT/Apache) and requires reproducing the
Unicode license and trademark notice. Note this is Unicode-3.0, **not** the
older Unicode-DFS-2016 — it carries its own attribution clause.

---

## 2. AWS-LC — the significant obligation in this binary

`aws-lc-rs` / `aws-lc-sys` provide the cryptographic backend, and they are the
reason this DLL's licensing is materially heavier than the crate table above
suggests. `aws-lc-sys` bundles a full C cryptography toolkit derived from
BoringSSL and OpenSSL, and its SPDX string
`ISC AND (Apache-2.0 OR ISC) AND OpenSSL` is a **conjunction**: the ISC and
OpenSSL terms both apply, and only the middle term is a choice.

The upstream license text is vendored here as
[`AWS-LC-LICENSE.txt`](AWS-LC-LICENSE.txt) (489 lines, copied verbatim from the
published `aws-lc-sys` 0.34.0 crate tarball). It is self-contained and reproduces
the full text of every license it enumerates:

| Component | License | Notes |
| --- | --- | --- |
| OpenSSL toolkit | OpenSSL License | Carries **advertising clauses** (3 and 6) |
| SSLeay (Eric Young / Tim Hudson) | Original SSLeay License | Also carries an advertising clause; applies cumulatively with the above |
| BoringSSL (Google) | ISC | |
| AWS-LC (Amazon) | ISC | |
| — | Apache-2.0 | Full text included |
| `third_party/fiat` (fiat-crypto) | MIT | Compiled into non-test libraries; also vendored as [`AWS-LC-fiat-LICENSE.txt`](AWS-LC-fiat-LICENSE.txt) |
| `third_party/jitterentropy` (Stephan Mueller) | BSD-3-Clause | Amazon expressly elects BSD-3-Clause over GPLv2 |
| Kyber / Keccak / AES reference code | Public domain / CC0 | |

`aws-lc-sys` ships **no `NOTICE` file** — `AWS-LC-LICENSE.txt` is the whole of
the upstream attribution material.

### What is actually linked into *this* Windows DLL

Rather than assume the bundled-source list maps to the shipped binary, symbols
were located directly in `bin/Win64/Release/moq_ffi.dll`:

| Component | Present? | Evidence |
| --- | --- | --- |
| OpenSSL / BoringSSL / AWS-LC core | **Yes** | ~1100 `openssl` / `aws-lc` / BoringSSL markers |
| jitterentropy | **Yes** | `jent_entropy_init`, `jent_read_entropy`, `jent_entropy_collector_alloc`, … |
| mlkem-native | **Yes** | `MLKEM`, `MlKem` |
| s2n-bignum | **No** | The only `BIGNUM` hits are BoringSSL error strings (`BIGNUM_TOO_LONG`, `BIGNUM_OUT_OF_RANGE`), not s2n-bignum routines — consistent with `crypto/fipsmodule/CMakeLists.txt` gating it on `UNIX` |
| ffmpeg / OpenH264 | **No** | 0 hits (unlike `livekit_ffi.dll` — see that directory's notices) |

So the BSD-3-Clause jitterentropy attribution **does** attach on Windows, and
s2n-bignum's `Apache-2.0 OR ISC OR MIT-0` terms do **not** (though they would on
a Linux/macOS build, which is worth remembering when those platforms are
vendored).

### ⚠️ Two open items

1. **mlkem-native is not covered by the vendored LICENSE.** The
   `crypto/fipsmodule/ml_kem/mlkem/` sources are `Apache-2.0 OR ISC OR MIT`,
   © "The mlkem-native project authors" — a distinct upstream project from the
   older pqcrystals Kyber code that `AWS-LC-LICENSE.txt` *does* mention. It is
   built into libcrypto on all platforms and is confirmed present in this DLL.
   An explicit entry for it is needed; the upstream LICENSE omission does not
   discharge the obligation.
2. **The OpenSSL/SSLeay advertising clauses need a decision.** Both require an
   acknowledgement string ("This product includes software developed by the
   OpenSSL Project…" / "This product includes cryptographic software written by
   Eric Young…") in advertising or documentation mentioning the product's
   features. Whether that must surface in the plugin's Fab listing or in-editor
   documentation — as opposed to being satisfied by this file — is a question
   for counsel. It is a genuine obligation here, not a theoretical one, and it
   is the clause most likely to be flagged in a commercial marketplace review.

Windows builds additionally consume `builder/prebuilt-nasm/*.obj` — 30
pre-assembled x86-64 object files checked into the crate. They carry the same
licensing as the assembly they were built from, but they land in the DLL as
prebuilt binaries rather than being compiled from source at plugin build time.

---

## 3. What is *not* in this binary

`moq_ffi.dll` was scanned for the copyleft components that are present in
`livekit_ffi.dll` (ffmpeg, OpenH264) and contains **none of them**. The MoQ
transport does not carry the unresolved blocker described in
`../../../Open3DTransportWebRTC/ThirdParty/livekit_ffi/THIRD_PARTY_NOTICES.md`.

---

## Maintaining this file

Do not hand-edit this after the next DLL rebuild. Add a `cargo about generate`
(or `cargo deny`) step to the `moq-ffi` CI workflow, publish its output as a
release asset alongside the DLL, and copy it in as part of the refresh workflow
in `README.md`. Re-vendor `AWS-LC-LICENSE.txt` at the same time if the
`aws-lc-sys` version changes, and re-run the symbol scan in §2 — the set of
native components linked on Windows is a property of the build, not of the crate
version alone.
