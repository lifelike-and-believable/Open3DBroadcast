# Mid-project review: plugin hardening and Fab readiness (2026-10-06)

This is a snapshot taken at `develop` 496a064c, after #399, eight days and about 140 PRs (#260–#399) into `plugin-hardening-and-fab-readiness.md`. The maintainer asked for it at the end of WP-U4.

It records:
- how the plan has changed;
- what the work has taught us;
- what is wrong with the plan documents;
- defects in the code that no open finding or planned work package covers.

HANDOFF.md stays the source for current status and next steps. Read this document when you plan the next batch or turn its items into work packages.

**How it was made.**
- Five read-only reviews ran in parallel: how the plan evolved; Open3DShared and Open3DSender; Open3DReceiver and the editor module; the transports; build, CI, tests and packaging.
- They were combined with this session's notes.
- Each review was told to leave out open findings, unstarted WPs and the HANDOFF Next items.
- Items marked **(verified)** were re-checked in the code by the session that wrote this report. The rest come from one reviewer's reading of the code, with file:line evidence.
- Nothing was built or run for this review.
- Item IDs (`BC-`, `SR-`, `RR-`, `TR-`) are local to this document. They are not finding IDs from `docs/review/`.

## 1. Most important, in order

1. **BC-1, fork PRs could run code on the self-hosted build machine (verified; resolved 2026-10-06).**
   - The repository is public. Its fork-PR approval policy was `first_time_contributors_new_to_github`.
   - The "UE build and tests" job in `open3dbroadcast-plugin-ci.yml` runs on `pull_request` on `[self-hosted, ue5, windows]`. On `pull_request`, GitHub runs the workflow from the PR's merge commit, so a fork could rewrite that job and run its code on the build machine, which other projects share.
   - The maintainer set the policy to `all_external_contributors` (checked with `gh api repos/lifelike-and-believable/Open3DBroadcast/actions/permissions/fork-pr-contributor-approval`). Outside contributors' workflows now wait for approval.
   - **Rule for whoever approves a fork PR's workflows:** read its changes to `.github/workflows/`, Build.cs files and scripts first. Approving runs them on the build machine.
   - **Don't add a guard that skips the job for fork PRs.** A skipped job counts as passing a required check, so the PR would look green without a build.
2. **RR-1, use-after-free (verified).** `FO3DReceiverSource` holds LiveLink's settings object as a raw `ULiveLinkSourceSettings*` (`O3DReceiverSource.h:232`), and `IsTickable()` always returns true.
   - LiveLink owns that object.
   - A Blueprint that keeps the handle from Create Open3DStream LiveLink Source (#387) after Remove Source keeps the source alive and ticking after GC frees the settings.
   - `Tick` has no guard, and `RequestSourceShutdown` sets `bIsValid` false but leaves `Settings` pointing at the freed object.
   - Fix: `TWeakObjectPtr`, and tick only while the source is valid.
3. **RR-2, incomplete RCV-6 fix (#395, verified).** #395 says `ClearFrames` drops the static data. That's wrong:
   - `FLiveLinkSubject` keeps its own `StaticData` member (`LiveLinkSubject.h:231`), and `ClearFrames` resets only the snapshot's copy.
   - `Tick` runs concealment (`O3DReceiverSource.cpp:342`) before clearing inactive subjects (`:353`).
   - LiveLink queues frames but clears immediately, so a held pose queued earlier in the same tick is applied after the clear.
   - Result: a "cleared" subject becomes valid again with a frozen pose.
   - Confirmed in the engine: `PushSubjectFrameData_Internal` accepts a frame whenever `HasStaticData()` is true, and `HasStaticData()` reads the member that `ClearFrames` leaves alone.
   - Fix: clear inactive subjects, and forget their concealment state, before the concealment tick. Correct the comment in `O3DLiveLinkPublisher.cpp`. The HANDOFF desk check for #395 now also asks whether the subject shows no data, rather than a frozen pose, during the pause.
4. **TR-1 / SR-1, reliable transports lose frames silently (verified).** A queued full sync or residual frame can be dropped after `SendSerialized` returned `Queued`:
   - TCP ages out queued frames after `tcp.maxqueueage` (1 s);
   - NNG drops on `EAGAIN`;
   - the soft-cap DropOldest policy drops too.

   The pipeline asks for an early full sync only when a send is refused synchronously (`O3DSenderPipeline.cpp:388-413`), and no transport reads `FO3DSendItem::bFullSync`. So on a congested TCP or NNG link, a residual-coded subject stalls until the next periodic full sync. That breaks ADR 0005's ReliableOrdered assumption. Fix: never age out or drop full-sync items, and report post-accept mocap drops back to the pipeline.
5. **TR-2, an NNG subscription topic blocks all data (verified).**
   - The receiver subscribes with the topic as a prefix (`NngReceiver.cpp:346-354`).
   - The topic can come from the schema field, the URI, or any `/` in StreamId.
   - The sender never writes a topic, and every message starts with the envelope magic.
   - So any topic makes the subscriber receive nothing, with no error.
   - Fix: the sender prepends the topic and the receiver strips it, or drop the option. Add a round-trip test.
6. **SR-3, a stopped sender still reports capturing (verified).** When its transport unregisters, `TeardownTransport` disables the tick but leaves `bIsCapturing` true (`O3DSenderComponent.cpp:445-451, 480-491`). `IsCapturing()` stays true, nothing is sampled, and `StartCapture` returns early until Stop Capture is called.
7. **Release path (BC-4, BC-5).** No release has gone through the release workflow under the `open3dbroadcast-vX.Y.Z` scheme. The workflow:
   - builds no Fab zip;
   - runs no tests and doesn't build with `-FailOnWarnings`;
   - computes the plugin version as committed + 1 and never writes it back.

   Both `.uplugin` files say VersionName 1.0, and the last release was 0.9.6. CHANGELOG has one "Unreleased" section of about 2,550 lines, and nothing maps plugin versions to transport API 5, protocol 2 or core 1.1.0. Fix this before the next release.
8. **Gates that don't enforce (BC-2, BC-3, BC-7).**
   - `develop` is unprotected: protection returns 404, and there are no rulesets.
   - "Fab source zip" and "Third-party binaries match their READMEs" are missing from the required-checks list in `Build/README.md`.
   - Nothing catches the UE test count going down. A smaller run that is green looks fine (see §3, the stale makefile).

## 2. How the plan evolved

**Scope added beyond the 2026-09-29 plan:**
- **WP-F11:** the WebRTC add-on plugin (#286).
- **WP-CTL:** the control channel, ADR 0011 (#290–#302).
- **ADR 0012:** runtime contexts, with metrics, audio bus and control bus per context (#347, #357–#365). WP-A3 had excluded it.
- **ADR 0013:** sender timecode on LiveLink frames (#351, #366–#369).
- **QuantEval and take recording** (#352, #355).
- **A2 follow-ups:** Insights numbers and a root-bone test using a mesh built in code (#321–#324).
- **NNG Repeater rebuilt, with an image test** (#371, #372).
- **Legacy apps archived** (#348, #375).
- **LiveKit FFI feature request** (#374, #376), which led to a receiver bug fix (#377).
- **Process changes:**
  - docs-only changes skip the builds (#363);
  - required-check readiness (#379);
  - `AGENTS.md` and the rules-for-robots rules (#380);
  - copyright year (#382);
  - every workflow posts a success comment (#388);
  - findings markers backfilled (#392).

Roadmap §3 has no D12 or D13 for the two new ADRs, and §4 still reads "D1–D11".

**Maintainer decisions that changed or narrowed the plan** (recorded in HANDOFF §0a):
- No compatibility with formats from before protocol 2 (#340). This makes WP-A4's "old-reader compatibility in both directions" criterion unmeetable.
- Quantization is never on implicitly (#349). This reverses part of ADR 0005's Verification text.
- PRs are squash-merged.
- The WebRTC add-on is distributed from the website, not through Fab. ADR 0002, roadmap D2 and WP-F11 still say "linked from the Fab listing".
- WP-U2's sample-map acceptance moved to WP-U5.
- WP-U4:
  - RCV-6 clears subjects instead of removing them;
  - RCV-20 has no drift resampling;
  - RCV-21 plays one stream at a time, adds an Any Stream mode, and keeps Mix strict on `o3ds:mix`, so a magic label remains.
- Repeater late joiners get no cache.

**Sequencing:**
- M0–M2 took two days, not the planned two weeks.
- M3 overlapped with WP-CTL, and M4 began while M3 was still open (A4 remainder, A5, A7).
- WP-F0, at the head of the plan's Fab lane and part of the "Fab-ready" definition, never started. `docs/fab/` doesn't exist.

**Blocked on the maintainer:**
- **Infrastructure and legal:** WP-A5 (needs live servers); WP-F0 and WP-F5 (counsel questions L1–L5); branch protection; publishing the Repeater image; the listing URLs; ADR 0002 Q8(b).
- **Desk tests** (about ten items, growing): QuantEval takes; RCV-9; the ADR 0013 manual test; WebRTC manual cases 2, 9 and 12; the three WP-U4 audio and LiveLink checks.

**Actual status:**

| Milestone | Status |
|---|---|
| M0 | Done; 13 ADRs accepted. |
| M1 | Done, with gaps: WP-S7's live-LiveKit acceptance was never met (the WebRTC receiver received no data from #271 until #377); WP-S5's ASan run isn't evidenced; WP-T2's WebRTC conformance profile went to a "WP-T2e" that no section defines. |
| M2 | Mostly done. F8's "a deliberately broken commit turns each job red" was never shown; F11 lacks a conformance profile and a clean-project test; F9's URLs are open; F0 and F5 not started. |
| M3 | A1, A2, A3 and A6 done. A4 partial (CORE-10, CORE-12, RCV-8/9 desk tests, SHR-7, WebRTC peer join). A5 not started. A7 partial. |
| WP-CTL | Done. Live checks remain: the Shipping-build test isn't in CI, the MoQ relay case isn't written, WebRTC manual case 12 hasn't been run. |
| M4 | U1, U3 and U4 done. U2 done except its acceptance, which moved to U5. D1 and D3 partly done by other PRs (DOC-3, DOC-7, DOC-9 not marked). U5, U6, D2, D4 and Q1 not started. |
| M5 | Not started. |

## 3. Learnings

**About process (this project's agents):**
- **UBT's cached makefile gives false greens.** After a merge, a branch switch or a new file, a built worktree compiled without the new or merged files. The automation run then:
  - lacked their tests (491 run where 493 were expected; 5 where 7 were expected);
  - or, after a failed build, ran the old binaries and reported green.

  This happened four times this session. Delete `Makefile.bin`, check that the build succeeded, and compare the test count every time (HANDOFF §0a). BC-7 would make the count check automatic.
- **Red runs earn their cost.** Every TDD red run in WP-U3 and WP-U4 failed for the intended reason, and one exposed an edge case in a test before the code existed: a 0 ms jitter target trimmed to zero. The earlier PRs that said "no red run" (#389–#391) skipped that check.
- **Reasoning from engine source isn't enough when the engine defers work.** RR-2 slipped through because LiveLink queues frames but clears synchronously. Behaviour that depends on a deferred engine operation needs a test against the real client (latent if necessary), not hooks.
- **A fresh review finds bugs in fresh code.** The receiver review found RR-1 and RR-2 in code merged hours earlier. Run a review pass on a work package before marking it done, not only at milestones.
- **Bookkeeping that nothing enforces decays.**
  - Findings markers: 193 findings fixed in M1–M3 had none until #392.
  - Roadmap Done lines: M1 and M2 have none.
  - HANDOFF §1 rows and the roadmap header: stale.

  Closing a finding should be part of the PR checklist, with a committed helper that places the line after the last top-level bullet. The old ad-hoc helper put it inside nested bullets.
- **Squash merges with stacked branches conflict.** A branch built on an unmerged PR's branch conflicts when `develop` gets that PR as a squash commit. Resolving takes the branch side, then a check that the diff against `develop` is exactly the branch's own commit (as for #398). Better: branch from `develop` after the base PR merges.
- **Line endings are fragile without `.gitattributes`.**
  - Staging a file whose blob is LF with `core.autocrlf=false` rewrote seven finding files (caught before push).
  - Five source files have mixed endings.
  - The flatc step rewrites `o3ds_generated.h` from CRLF to LF.

  See BC-11.
- **The test editor runs with `-NoSound`, so audio can't be heard in CI.** Design audio code so its logic sits in testable plain classes; `FO3DAudioJitterBuffer` is the example.
- **What worked:**
  - Asking the maintainer with a recommended option.
  - Merging only when every check passes, with Auto-fix waking the session.
  - Docs-only PRs skipping the builds.

  Decisions were sometimes revised the same day (RCV-21), so record them in HANDOFF straight away.
- **Desk tests pile up.** Every live-only acceptance becomes a desk item, and nothing in CI runs any of them. Ten of them, spread across milestones, now stand between the code and a release.

**About the code:**
- The structure work held up: the registry and conformance suite, the shared send queue and worker, the lifetime gates, the runtime contexts and the frame pool. Reviewers found no threading defect in the shared blocks.
- What WP-A1 left uneven is behaviour, not structure. "Queued" means different things per transport (TR-1), and so do the stats counters (TR-5), the `Poll` work bounds (TR-3) and the logging (TR-6). Each fix should come with a conformance case, so a sixth transport can't drift again.

## 4. Plan-document deficiencies

**Roadmap (`plugin-hardening-and-fab-readiness.md`):**
- **Header:** still "Status: Ready for delegation · 2026-09-29 · Baseline 7aea235".
- **Stale references:**
  - §0.1, §0.2 and WP-U2 cite `.github/copilot-instructions.md` §0/§5, now a pointer to AGENTS.md.
  - WP-D3 cites `.claude/claude.md` (deleted) and "merge the two CHANGELOGs" (done in #339).
- **§4 ranges miss work packages:** "WP-S1..S10" omits S11, and "F0..F9" omits F11.
- **WP-A2:** says the root-bone test and the Insights numbers are pending. Both were done in #322–#324.
- **WP-CTL:**
  - waits for `docs/wire-format.md`, which exists since #339;
  - waits for ADR 0005 (vi), done in #344 except WebRTC;
  - says A1 "must absorb" the control paths, which is done.
- **WP-T2e** is referenced but never defined.
- **Unmeetable or unmet acceptance criteria:** WP-A4's old-reader test; WP-U2's sample map, now WP-U5's; WP-F8's red-commit check.
- **Findings owned by one WP in §8 but closed by another** (§8 allows this). Fine, but §8 should say who actually closed them: DOC-4, TRF-35, SHR-19, SND-35, DOC-8, and TRB-4–6, which appear in WP-A5's text.
- **Done markers:** M1 and M2 have none in the roadmap; their status lives only in HANDOFF §1.

**HANDOFF:**
- **§0's "Start here" line** says ADR 0012 is "not started". AGENTS.md forbids editing that line, and §0a only supersedes it.
- **§0a contradicts itself:**
  - ADR 0013 is "not implemented yet" at line 72 and implemented at line 74;
  - there are two "Next" lists;
  - "M4" is listed under "Later".
- **§1:**
  - the M0 row says "ADRs 0001–0010";
  - "Recent merges" stops at #318.
- **§2 and §2a** call WP-A1 and WP-A2 "in flight".
- **§3** says AGENTS.md points to `copilot-instructions.md`, the reverse of #380.
- **§4** says the nightly has never reported a run. It has been green since 10-04.

HANDOFF has grown to about 370 lines of layered history. **Recommendation:** move §0, §0a's 10-04 block and §2–§2b to a `handoff/HISTORY.md`, and keep HANDOFF to state, decisions, next steps and desk items.

**Other docs:**
- `resilient-streaming-and-motion-prediction.md` §0.2 says the core is "checked in twice" (resolved by ADR 0003).
- `c3-go-no-go.md` has no takes yet.
- CHANGELOG's oldest Schema/Protocol section still describes the v1 keepalive and "until `ref_seq` lands".

**Agent instructions (BC-10):** `.github/agents/*.md` (2,860 lines, read by Copilot) contradicts AGENTS.md:
- it says "feature/" branches, rebasing, and keeping up with `main`;
- it documents `/ue` bot commands with no workflow behind them;
- it points to a `poll-steer.sh`.

Rule TEST-001 cites a CI "test-change-guard" that doesn't exist. Cut `.github/agents` down to pointers to AGENTS.md.

## 5. Code deficiencies not covered by the plan

The items already in §1 aren't repeated here. Severity follows the reviewer, adjusted where the item was verified.

**Receiver and audio:**

| ID | Severity | Item |
|---|---|---|
| RR-3 | medium | Incomplete RCV-16 fix (#390). "Receiving via X" is set before the packet is validated (`O3DReceiverSource.cpp:759-771`), so unreadable or too-new senders show Receiving, then the status flaps with "No data received". The version-too-new reason is never shown. |
| RR-4 | medium | "Parse failed" logs a Warning on every frame (`:949`); unthrottled on the hot path. |
| RR-6 | medium | Attenuation, spatialization and other `BlueprintReadWrite` audio properties: `bAC_OverrideAttenuation` hides the asset and offers no overrides (they are commented out). Most `AC_` properties apply only at BeginPlay, so setting them from Blueprint at runtime does nothing. |
| RR-7 | medium | The remote control component's source-name filter misses values set before it started. Its `SourceNames` map is keyed by network ids and never pruned. |
| RR-8 | low-medium | The Blueprint Create LiveLink Source node skips `ValidateNewSource`. |
| RR-9 | low | `SetVolumeMultiplier`, and `Play` retries, on every audio packet. |
| RR-10 | low | USER_GUIDE errors outside RCV-32: `AudioStreamLabel`; the `AudioCodec` default; a hidden Transport Options field; "follows subject's position"; "Audio Bus singleton". |
| RR-11 | low | Dead code: `bStalled`, `bLoggedActiveState`, `LastClockOffsetEstimateUs`, an unreachable off-thread hop in `HandleSerializedFrame`, a shared `LastGateSubjectLabel`. |
| RR-12 | medium | Untested: `UO3DJitterSoundWave` through `GeneratePCMData` (possible without a device); `ClearSubjectsFrames` against a real client; malformed-packet status; a source outliving its removal. |

**Sender and shared:**

| ID | Severity | Item |
|---|---|---|
| SR-2 | medium | Incomplete SND-27 fix. The sender accepts any audio rate from 8 to 48 kHz, but receivers drop rates outside `IsSupportedSampleRate`, and the sink still reports success. |
| SR-4 / TR-5 | medium | Per-sender and transport metrics are zero for Loopback, TCP and UDP, and counters mean different things per transport (TCP counts frames as sent when it queues them). Record the capture and serialize counts once, in the pipeline, and define each counter. |
| SR-5 | medium | The USER_GUIDE quick start can't produce a stream: it never turns on Auto Create Transport, and it uses option keys no transport declares. WP-D2 will rewrite it; listed so it isn't forgotten. |
| SR-6 | low-medium | Two senders on one actor share one audio capture component, and each start overwrites its configuration. |
| SR-7 | low | Open3DSender exposes the core and six Win32 libraries publicly, and keeps `bEnableExceptions`; incomplete BUILD-5 fix. |
| SR-8 | low | `GetSerializer()` crashes outside a capture session. There is a leftover second `OnSerializedFrame` delegate. |
| SR-9 | low | `FO3DGatedSenderAudioSink` is dead exported API. Delete it while transport API 5 is unreleased. |
| SR-10 | low | `o3d.Sender.DumpPipelineStats` is registered through a function-local static; incomplete SND-32 fix. |
| SR-11 | low | Frames are still serialized with no transport and no listener; SND-25 only half done. |
| SR-12 | low | Console names mix `o3ds.Sender.*` and `o3d.Sender.*`. |

The sender reviewer also notes that SHR-7 and SHR-35 look largely resolved in code but are still open. The maintainer should check them alongside #392's list.

**Transports:**

| ID | Severity | Item |
|---|---|---|
| TR-3 | medium | NNG `Poll` counts only mocap and audio toward its 16-message bound, so a peer can pin the game thread with control or garbage messages. The bounds also differ by transport, and Loopback has none. |
| TR-4 | medium | An NNG listener can't restart after Stop, because the address stays in use and nothing retries. The conformance profile skips the case, and nothing tracks it. Reviewer's reading, not reproduced. |
| TR-6 | medium | Hot paths log every time on WebRTC (`TooLarge` Error per frame), on UDP and on the NNG receiver. MoQ's `ClaimLogSlot` throttle should move to Open3DShared and be used at every site. |
| TR-7 | low-medium | A payload above the queue's hard cap returns `DroppedBackpressure` instead of `TooLarge`, so the pipeline requests full syncs forever. The conformance backpressure case never fills a queue. |
| TR-8 | low | MoQ's receiver shows Delivery Mode and Queue Capacity options it ignores. |
| TR-9 | low | UDP and Loopback receivers read stats without synchronisation. |
| TR-10 | low | MoQ's receiver delivers stale frames after a reconnect. |
| TR-11 | low | WebRTC can't Start after Stop or re-Initialize. The future WP-T2e profile will catch it. |
| TR-12 | low | Duplicated, already diverging code: the WebRTC token and connect logic, the NNG socket wrappers, MoQ's per-track functions. |
| TR-13 | low | Incomplete TRB-19 fix. The UDP receiver still allocates an `FString` per fragment and copies every datagram. |

**Build, CI, tests:**

| ID | Severity | Item |
|---|---|---|
| BC-6 | medium | Every UE job runs on one self-hosted runner shared with other projects, and `windows.yml` runs `choco install` on it. Needs an outage runbook. |
| BC-8 | medium | Fuzz targets `tcp_stream` and `control` never get the 60 s libFuzzer run, although ADR 0011 says `fuzz_control` does. Build the list from CMake. |
| BC-9 | medium | Nightly failures notify nobody: there were eight consecutive red runs on 9/18–9/26. Its "Linux target excludes the plugin" step always passes. |
| BC-11 | medium | No `.gitattributes`; mixed line endings (see §3). |
| BC-12 | low | The plugin CI concurrency group cancels `develop` push runs when a docs PR merges, so some code merges get no post-merge UE run. Cancel in progress only for pull requests. |
| BC-13 | low | `Build/README.md` refers to a removed script and a non-existent workflow, and its CI table is incomplete. |
| BC-14 | low | Wall-clock assertions and `FPlatformProcess::Sleep` in tests, against ADR 0006 §9. `bEnableExceptions` in six modules that don't use it. |
| BC-15 | low | Leftovers: `.idea/`, `scripts/agent/poll-steer.sh`, and public GitHub releases built from `claude/...` branches. |

## 6. Suggested next work packages

Each is to be scheduled by the maintainer. None of them is in the roadmap yet.

1. **WP-R1, correctness follow-ups (before WP-U5):**
   - RR-1, RR-2, SR-3, TR-1/SR-1, TR-2, RR-3, RR-4, TR-3, SR-2;
   - each with a test, using a real LiveLink client where the bug involves one.
2. **WP-R2, CI and release:**
   - BC-1 is resolved by the repository setting (see §1); no workflow change is needed.
   - BC-2 and BC-3: an always-running aggregator job to require;
   - BC-4 and BC-5: release from the tested Fab zip, version taken from the tag, a CHANGELOG version section;
   - BC-7: minimum test count;
   - BC-8, BC-9, BC-11, BC-12.
3. **WP-R3, transport consistency:** TR-5/SR-4 counters, TR-6 shared throttle, TR-7, TR-9, TR-10, TR-8, TR-4, each with a conformance case. Fold TR-11 into WP-T2e, and define WP-T2e in the roadmap.
4. **Docs:**
   - fold §4 into WP-D3/D4;
   - fold RR-10 and SR-5 into WP-D2;
   - the HANDOFF/HISTORY split;
   - add D12 and D13 to roadmap §3;
   - update ADR 0002 and WP-F11 for website distribution;
   - trim `.github/agents`.
5. **Small cleanups into WP-Q1:** RR-6, RR-7, RR-8, RR-9, RR-11, SR-6 to SR-12, TR-12, TR-13, BC-13 to BC-15.
6. **Decisions for the maintainer:**
   - **RR-5, Mix as the default mode.** On 2026-10-06 the maintainer kept Mix strict and accepted that "existing components behave as today (silent for labelled streams) until users switch mode". The reviewer's evidence:
     - senders label audio with the subject name, or `o3ds:audio` when there is none (`O3DSenderAudioCaptureComponent.cpp:347`);
     - `o3ds:mix` appears only when the receiver source has no StreamId, but `BuildTransportConfig` copies the URI into StreamId;
     - so a component left at its defaults plays nothing for any real sender;
     - Mix also matches by prefix, case-insensitively, not exactly.

     Revisit the default for Fab users?
   - SHR-7 and SHR-35 status, alongside #392's 16 open questions;
   - whether to schedule a desk-test session before more live-only work lands.
