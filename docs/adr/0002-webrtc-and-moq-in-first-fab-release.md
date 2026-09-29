# 0002: WebRTC and MoQ in the first Fab release

- **Status:** Proposed (pending maintainer sign-off)
- **Date:** 2026-09-29
- **Plan decision:** D2 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3
- **Related:** [ADR 0001](0001-platform-scope-first-fab-release.md) (D1), [`docs/webrtc-codec-removal-plan.md`](../webrtc-codec-removal-plan.md); feeds WP-F5, WP-F8, WP-F9, WP-S7, WP-S8, WP-D2

**Recommendation in one line:** the Fab package leaves out `Open3DTransportWebRTC`, removed by the Fab packaging job, while the codec-free `livekit_ffi` rebuild proceeds in parallel and brings WebRTC back in a later update. MoQ ships in v1, labelled **Experimental**, but only if four named gates close before WP-F10. If they don't, it is removed by the same mechanism.

This ADR covers the engineering choice only. The licensing questions are listed separately in "Questions for counsel" and are not answered here.

## Context

**WebRTC.**
- `livekit_ffi.dll` (23,449,088 bytes, `Plugin/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/bin/Win64/`) statically links ffmpeg (`libavcodec`, `libavutil`) and OpenH264 (`WelsEnc`), as recorded in `Plugin/THIRD_PARTY_LICENSES.md:24-36`.
- The inventory says this "cannot be closed by documentation" (`:38-39`).
- The transport has no video path. The `livekit_ffi` C ABI exposes none, so removing the codecs costs no function (`docs/webrtc-codec-removal-plan.md` §0; `THIRD_PARTY_LICENSES.md:43-48`).
- The removal plan is "plan only, no build has been attempted" (`webrtc-codec-removal-plan.md:6-8`). Its biggest unknown is the unguarded H.264 decoder references in `webrtc-sys` (§3). Its main cost is standing up and caching a custom libwebrtc build (§4 step 3).
- `Build/Scripts/check-no-video-codecs.sh` "currently fails by design" (`:8-12`). It scans every `.dll/.so/.dylib` under the plugin, or the paths given as arguments (`:27-34`).
- Separately, `livekit_ffi` has no licence text upstream (`THIRD_PARTY_LICENSES.md:12-16`, `:130-138`), and the DLL is a combined Apache-2.0 work that needs NOTICE attribution (`:139-143`) (FAB-11).

**How the WebRTC module is coupled to the rest.**
- WebRTC registers itself at module startup through `O3DSender::RegisterTransportCustomization(TEXT("WebRTC"), ...)` and the receiver equivalent (`Plugin/Source/Open3DTransportWebRTC/Private/Open3DTransportWebRTCModule.cpp:663`, `:708`).
- No other module's Build.cs names `Open3DTransportWebRTC`. A grep of every `*.cs` outside the module finds nothing.
- The only mentions of WebRTC or LiveKit in Shared, Sender and Receiver are comments and the stringly-typed token fields in `Plugin/Source/Open3DShared/Public/O3DTransportTypes.h:66-98` (SHR-36).
- **So the module can be dropped from a package without touching other modules' code.**

**MoQ.**
- `moq_ffi.dll` was scanned and contains no ffmpeg or OpenH264 (`THIRD_PARTY_LICENSES.md:54`, `:170-171`; `Plugin/Source/Open3DTransportMoQ/ThirdParty/moq-ffi/THIRD_PARTY_NOTICES.md` §2 table, "ffmpeg / OpenH264: No"). There is no codec issue.
- Its licensing is heavier than it first appears:
  - **AWS-LC** is `ISC AND (Apache-2.0 OR ISC) AND OpenSSL`, and the OpenSSL and SSLeay terms carry advertising clauses (`THIRD_PARTY_LICENSES.md:152-165`).
  - **mlkem-native** is linked but not attributed (`:166-168`).
  - **ICU crates** are under Unicode-3.0, and **`ring`** is under a conjunctive `Apache-2.0 AND ISC` (`THIRD_PARTY_NOTICES.md` §1 notes).
  - The notices are hand-recovered from the binary and will drift (`THIRD_PARTY_NOTICES.md:15-17`).
- It is built against **draft-ietf-moq-transport-07** (`Plugin/Source/Open3DTransportMoQ/ThirdParty/moq-ffi/README.md:13-14`; `.gitmodules`, moq-rs `branch = draft-ietf-moq-transport-07`).
- The plugin README documents a "Draft-07 Hotfix & Fallback" for a reader panic (`Plugin/README.md:129-132`).
- The descriptor claims neither beta nor experimental status (`Plugin/Open3DBroadcast.uplugin:15-16`) (FAB-12).
- Open functional findings: TRF-8, 9, 11, 13, 20, 29, 37 and 39 (WP-S8); the pdb shipped as a runtime dependency (`Open3DTransportMoQ.Build.cs:92-97`, FAB-4); and a relay test that needs the public internet (UX-4, TRF-34).
- USER_GUIDE does not mention MoQ at all (0 matches), while `Transport_Module_Comparison.md` has 43.

## Decision drivers

1. No copyleft or patent-encumbered bytes in the Fab package without counsel clearance (the Fab checklist in `fab-ci-docs.md`).
2. Don't block the first submission on the libwebrtc rebuild, which has an unknown duration.
3. What ships must be honest: the listing, docs and UI describe exactly the transports in the package, and their maturity.
4. The mechanism must be deterministic and checked by CI, with no manual steps at submission time.
5. Keep one source tree. A second plugin or a fork is a cost that needs a reason.

## Options considered

### Option A: rebuild `livekit_ffi` without ffmpeg and OpenH264, then ship WebRTC in v1
Follow `webrtc-codec-removal-plan.md` steps 1 to 5. Make `check-no-video-codecs.sh` a required CI gate.
- **Pros:** WebRTC stays in v1. It removes the problem rather than working around it. The DLL gets smaller.
- **Cons:** the duration is unknown. It needs a libwebrtc build (hours long, tens of GB, per the plan §4 step 3) and possibly a `webrtc-sys` patch (§3). FAB-11 (the missing licence text upstream) still has to close.
- **Cost / effort:** L. **Risk:** high for the v1 schedule, low for the end state.

### Option B: leave `Open3DTransportWebRTC` out of the Fab package
The ways to exclude it:
- **B1. The Fab packaging job removes the module.** WP-F8's `fab-package` job copies the plugin into a staging folder, deletes `Source/Open3DTransportWebRTC/`, removes its entry from the staged `.uplugin`, and runs BuildPlugin on the result.
  - **Pros:** one source tree; deterministic; the bytes are physically absent, which is what driver 1 needs; the change is small.
  - **Cons:** the Fab descriptor is derived, not hand-written, and the licence index must also be derived (see Decision). The GitHub build and the Fab build differ.
- **B2. `Config/FilterPlugin.ini`.** As far as the review could tell, FilterPlugin *adds* non-standard folders to a package. It is not documented as excluding files under `Source/` (**needs-verification**, Q1). Even if it could, the module entry would remain in the descriptor. Rejected.
- **B3. The build flag `O3D_WITH_TRANSPORT_WEBRTC=0`.** This controls compilation, not distribution. The DLL would still be in the source zip, and Fab's farm cannot set environment variables (BUILD-4). Rejected.
- **B4. A separate plugin (`Open3DBroadcastWebRTC`), distributed outside Fab.** This is clean in the long term, but a second plugin needs the transport interfaces and a single registry exposed as a stable public API first. That is D4 and WP-A1. It is effectively a product decision (a second SKU). Deferred until D4 is decided.
- **Cost / effort (B1):** S to M. **Risk:** low.

### Option C: ship WebRTC as-is with counsel sign-off
- **Pros:** no engineering work.
- **Cons:** the repository's own inventory says the problem needs more than documentation (`THIRD_PARTY_LICENSES.md:38-39`). It depends on a legal outcome this ADR cannot predict, and it keeps 20+ MB of unreachable code.
- **Cost / effort:** S engineering, plus unknown legal work. **Risk:** high. Not recommended; listed for completeness.

### Option D: replace LiveKit with another WebRTC stack
The repo has previous libdatachannel work (`.github/workflows/build-libdatachannel.yml`, `build-webrtc.yml`; CI-6 says the output is unused).
- **Cost / effort:** L, and it is a new transport. **Risk:** high. Out of scope for v1.

### MoQ options
- **M1. Ship MoQ as Experimental in v1, behind gates.**
  - Pros: no codec issue; working code; a differentiator.
  - Cons: open licensing items (FAB-11), P1 functional bugs (WP-S8), and a draft protocol with limited relay interoperability.
- **M2. Leave MoQ out of v1** using B1's mechanism.
  - Pros: the smallest risk surface.
  - Cons: it drops a working transport and delays feedback.
- **M3. MoQ as a separate optional plugin (FAB-12's first recommendation).** Same D4 prerequisite as B4. Deferred.

## Decision

1. **WebRTC: Option B1 for v1, with Option A in parallel.**
   - The Fab package has no `Open3DTransportWebRTC` module and no `livekit_ffi` files.
   - WebRTC returns in a Fab update once all three hold:
     - (a) `check-no-video-codecs.sh` passes on the rebuilt DLL;
     - (b) the upstream licence text and generated notices exist (FAB-11);
     - (c) counsel has signed off on the remaining livekit notices.
   - WP-S7 fixes continue regardless, because the GitHub build still ships WebRTC.
2. **Mechanism for B1.** The WP-F8 `fab-package` job reads a small, committed exclusion manifest (for example `Build/Fab/exclude-modules.txt` listing `Open3DTransportWebRTC`). It deletes the listed module folders from the staged copy and removes the matching `Modules[]` entries from the staged `.uplugin` with a JSON-aware tool that keeps the file's formatting (CI-5 notes that `ConvertTo-Json` rewrites it). Then it runs:
   - BuildPlugin on the staged tree;
   - `Build/Scripts/check-no-video-codecs.sh` on **the staged package**, as a required gate from day one. It passes because the only offending DLL is absent. This closes CI-8 for the Fab path now instead of waiting for Option A.
3. **MoQ: Option M1.** MoQ ships in v1 as **Experimental** if all four gates are closed at WP-F10:
   - **G1:** WP-S8 is merged, and the MoQ parts of WP-S5 (lifetime safety) are merged.
   - **G2:** FAB-4 for MoQ is done: the pdb is gone and `Open3DTransportMoQ.Build.cs:92-97` is removed.
   - **G3:** FAB-11 for MoQ is done:
     - the mlkem-native entry exists;
     - the notices are generated by `cargo about`, not hand-recovered;
     - counsel has decided where the OpenSSL/SSLeay acknowledgement appears.
   - **G4:** the relay test is out of the default test filter (UX-4).

   If any gate is open at WP-F10, `Open3DTransportMoQ` is added to the exclusion manifest and v1 ships without it. The decision is recorded in the WP-F10 report.
4. **Labelling (WP-F9).** The plugin stays `IsBetaVersion: false` and `IsExperimentalVersion: false`, because the default transports (Loopback, Sockets, NNG) are the supported path. MoQ is labelled per transport:
   - its display name in the transport picker becomes "MoQ (Experimental)";
   - it logs one Warning on first use naming the draft version ("MoQ transport draft-07; relays must speak draft-07");
   - it gets a USER_GUIDE section (there is none today) with the same wording;
   - the listing text says the same.

   UE has no per-module "experimental" key as far as could be found (**needs-verification**, Q2). Whether the whole plugin should be Beta for v1 is a maintainer call (Q5).
5. **Licence index.** `THIRD_PARTY_LICENSES.md` in the Fab package must match the shipped binaries exactly (WP-F5 acceptance). The `fab-package` job:
   - assembles it from per-module fragments that already exist next to each binary (the `THIRD_PARTY_NOTICES.md` and LICENSE files under each module's `ThirdParty/`), plus the plugin-level entries;
   - fails if a shipped binary has no entry, or if an entry names a binary that is not shipped.

## Shipped transports, docs and listing

| Transport | GitHub build | Fab v1 | Label |
|---|---|---|---|
| Loopback | yes | yes | stable (testing aid) |
| Sockets (UDP/TCP) | yes | yes | stable |
| NNG | yes | yes | stable |
| MoQ | yes | yes, if G1 to G4 close | Experimental (draft-07) |
| WebRTC (LiveKit) | yes | **no** | "Planned for a later update" in the listing |

- `Plugin/USER_GUIDE.md` (50 WebRTC/LiveKit mentions), `Plugin/README.md` (6) and `Plugin/Transport_Module_Comparison.md` (58) get a short "Available in" line per transport. WebRTC sections say they apply to the GitHub build only. This is WP-D2.
- The Fab listing names only shipped transports, states the MoQ draft version, and carries whatever acknowledgement text counsel requires (G3).
- `DocsURL` (`Plugin/Open3DBroadcast.uplugin:11`) must point at docs that match the Fab edition (WP-F9, FAB-13).

## Consequences

- **Easier:**
  - v1 submission no longer waits on libwebrtc.
  - The codec gate runs in CI immediately, on the Fab zip.
  - WP-F5's acceptance ("`check-no-video-codecs.sh` passes, or WebRTC is excluded") is met by the exclusion.
- **Harder:**
  - There are two package variants. Every WP-F8 package assertion runs on the Fab variant.
  - Docs need edition notes.
  - Users who need browser or LiveKit interop must use the GitHub build until Option A lands.
- **Constrains:**
  - No module may take a hard dependency on `Open3DTransportWebRTC` or `Open3DTransportMoQ`. The review agent checks this in WP-A1.
  - D6 (credentials) should not hard-wire LiveKit fields into shared types (SHR-36), because the Fab edition won't have the transport that uses them.
- **Enables:**
  - B4 and M3 (separate plugins) after D4, if the maintainer later wants SKUs.
  - The same exclusion manifest serves any future experimental module.

## Implementation outline

1. **WP-F8 (CI-5, CI-8):** add `Build/Fab/exclude-modules.txt` and the staging step in the new `fab-package` job in `.github/workflows/open3dbroadcast-plugin-release.yml`, or a dedicated workflow. Run `check-no-video-codecs.sh <staged binaries>` as a required step.
2. **WP-F5:** the licence-index assembly script and the package assertion. Close FAB-11 for MoQ (G3): add mlkem-native, and add `cargo about` to the `moq-ffi` build workflow. Delete the orphaned `Plugin/ThirdParty/Include` (SHR-23).
3. **WP-F3:** remove both pdbs and `Open3DTransportMoQ.Build.cs:92-97` (G2).
4. **WP-S8 and the MoQ part of WP-S5** (G1). **WP-T2:** take the relay test out of the default filter (G4).
5. **WP-F9:** MoQ display name, first-use warning, and listing text.
6. **WP-D2:** "Available in" notes, and a MoQ section in USER_GUIDE.
7. **Parallel track (new WP under WP-F5, owner: FFI maintainer):** `webrtc-codec-removal-plan.md` steps 1 to 5. Once it passes, remove `Open3DTransportWebRTC` from the exclusion manifest in a separate PR, with counsel sign-off recorded.

## Verification / acceptance

- The Fab zip contains no `Source/Open3DTransportWebRTC/` and no file matching `*livekit*`, and its `.uplugin` has no WebRTC module entry (a package-content assertion).
- `check-no-video-codecs.sh` exits 0 on the staged package in CI. A deliberately re-added `livekit_ffi.dll` turns the job red.
- BuildPlugin passes on the staged tree (WP-F1 acceptance).
- The licence index check passes, and fails if an entry is deleted.
- If MoQ ships: the transport picker shows "MoQ (Experimental)"; the first MoQ start logs the Warning; the WP-F10 report lists G1 to G4 as closed with PR links.

## Questions for counsel (legal; no conclusions given here)

- **L1.** Do the ffmpeg (LGPL-2.1) and OpenH264 contents of `livekit_ffi.dll` create obligations for the **GitHub** releases (`open3dbroadcast-plugin-release.yml` ships binaries), independently of Fab?
- **L2.** Where must the OpenSSL and SSLeay acknowledgements for AWS-LC appear: Fab listing, in-editor About, or the shipped notices file?
- **L3.** Are the Unicode-3.0 (ICU) and `ring` notice sets complete as vendored?
- **L4.** Does shipping cryptography (AWS-LC in `moq_ffi.dll`; TLS inside `livekit_ffi.dll`) raise any export-classification or Fab declaration requirement?
- **L5.** Once the rebuilt DLL passes the codec scan, is anything else needed before WebRTC ships on Fab (Apache-2.0 NOTICE for the livekit crates, upstream MIT text)?

## Open questions for the maintainer

1. **needs-verification:** can `Config/FilterPlugin.ini` exclude files under `Source/` from a BuildPlugin package, or does it only add paths? This ADR assumes it only adds.
2. **needs-verification:** does UE 5.7's `.uplugin` schema have any per-module maturity flag? This ADR assumes it has none.
3. **needs-verification (WP-F0):** does Fab accept a descriptor that differs from the public repo's (a derived, reduced module list)? Does Fab review compare against a public source?
4. Is a separate WebRTC plugin or SKU (B4) a direction you want once D4 lands, or should WebRTC return into the main plugin?
5. Should the whole plugin be marked `IsBetaVersion: true` for v1, given the number of P0 fixes landing just before submission?
6. If a MoQ gate is still open near submission, do you prefer to slip v1 or ship without MoQ? This ADR defaults to shipping without it.

## References

- Findings: FAB-1, FAB-11, FAB-12; also FAB-4, CI-5, CI-8, UX-4, TRF-34, SHR-23, SHR-36, BUILD-4.
- Files: `Plugin/THIRD_PARTY_LICENSES.md`; `Plugin/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/`; `Plugin/Source/Open3DTransportMoQ/ThirdParty/moq-ffi/{README.md,THIRD_PARTY_NOTICES.md}`; `Plugin/Open3DBroadcast.uplugin`; `Plugin/README.md`; `Build/Scripts/check-no-video-codecs.sh`; `docs/webrtc-codec-removal-plan.md`; `.gitmodules`.
- External: none fetched specifically for this ADR. The Fab sources and blocked sites are listed in ADR 0001, References.
