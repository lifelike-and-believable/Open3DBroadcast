# Open3DBroadcast: handoff to the next session

Written 2026-09-30 by the cloud session that drove M1, M2 and the start of M3 of the plugin hardening roadmap. Updated 2026-10-01 with the control-channel work (ADR 0011, CTL-1..5) that landed afterwards; ADR 0011 accepted and WP-CTL added to the roadmap the same day. Updated 2026-10-03 when the cloud session finished WP-A1 (#305–#314) and WP-A2a to A2c (#316–#318) and handed the work over to Claude on desktop (§0); updated the same day when Claude on desktop finished WP-A2d (#319) and WP-A2e. Read this first, then the files it points to.

## 0a. State at the end of the desktop session (2026-10-04, updated 2026-10-06; read this first)

This section supersedes the "Start here" line in §0 where they disagree.

**Mid-project review (2026-10-06):** `docs/roadmap/mid-project-review-2026-10-06.md` records how the plan evolved, learnings, plan-document drift and code defects no finding or WP covers, with suggested work packages WP-R1 to R3. Read it before planning the next batch; its §1 lists the most important items (a CI security setting reported to the maintainer directly, a use-after-free in the receiver source, an incomplete RCV-6 fix).

**2026-10-05 and 2026-10-06 (newest):**
- **Merged:**
  - #383 (HANDOFF) and #384 (issue cleanup).
  - **WP-U1:** #385, project settings (UX-2, RCV-18).
  - **WP-U2:** #386, the sender Blueprint API (SND-26, UX-3, DOC-4); #387, the receiver Blueprint library (UX-3).
  - **CI:** #388, every PR workflow posts "✅ … completed successfully!" on the PR.
  - **WP-U3, complete:** #389, the sender Details panel (SND-24, SND-25, SND-27, SND-30, SND-35); #390, receiver status, Create Source validation and live concealment settings (RCV-15, RCV-16, RCV-17); #391, transport options and logs (TRF-30, TRB-43 and TRB-21's UI part; the rest of TRB-21 is WP-U6's).
  - **Findings:** #392, `Status: closed in #N` markers for the 193 findings M1 to M3 fixed. Before it, only the M4 PRs had added any. Its description lists 66 findings left open, 16 of them for the maintainer to decide.
  - **Docs:** #394, this section and the roadmap's progress lines.
  - **WP-U4, complete:**
    - #393, the remote audio component lifecycle (RCV-22, RCV-23, RCV-24): it keeps the parent the user chose, Auto Activate off works with new Play and Stop functions, and EndPlay destroys the internal audio component;
    - #395, inactive LiveLink subjects (RCV-6): a per-source Inactive Subject Timeout (default 5 s, 0 = never); on timeout the subject's frames are cleared, and the subject and its settings are kept;
    - #396, the remote audio jitter buffer (RCV-20): `FO3DAudioJitterBuffer`, pulled on the audio thread by `UO3DJitterSoundWave`; a Target Latency property (default 60 ms), trimming above target + 60 ms. Open3DReceiver now depends on AudioExtensions.
    - #398, remote audio stream routing (RCV-21): one stream at a time (source and label) until it is idle for 1 s, a new Any Stream mode and a Stream Label Filter; Mix stays strict on `o3ds:mix`, now `O3DS::MixAudioStreamLabel`. The USER_GUIDE audio property table is corrected.
  - **Docs:** #397, this section at the pause before RCV-21; #399, WP-U4 done; #400, the mid-project review.
  - **WP-R1, complete** (correctness follow-ups from the mid-project review; maintainer, 2026-10-06):
    - #401, receiver:
      - RR-1: the source holds LiveLink's settings weakly and stops ticking after shutdown (a use-after-free);
      - RR-2: inactive subjects are cleared before concealment runs (#395 was wrong that `ClearFrames` drops the static data, so cleared subjects came back frozen);
      - RR-3: an "Unreadable data" or "update this receiver" status;
      - RR-4: "Parse failed" is throttled.
    - #402, sender:
      - SR-3: capture stops when its transport unloads;
      - SR-2: audio is sent only at rates receivers play.
    - #403, transports:
      - TR-1/SR-1: frames dropped after acceptance lead to a full sync, through the new `IOpen3DSender::SetFramesDroppedCallback`, and the send queue never discards a full sync;
      - TR-2: NNG subscription topics removed (maintainer, 2026-10-06);
      - TR-3: NNG `Poll` is bounded for every kind of message.
  - **Docs:** #404, WP-R1 done; #405, BC-1 resolved (the fork-PR approval setting).
  - **WP-R2, complete** (CI and release):
    - #406: each CI workflow ends in one aggregator check, "Plugin CI result" and "Core tests result", the two to require on `develop`. Pushes to `develop` are no longer cancelled by the next merge.
    - #407: the fuzz job reads its targets from `test/fuzz/CMakeLists.txt`, so all 8 run. `Build/automation-test-floors.json` sets the minimum number of UE tests per run (`Run-AutomationTests.ps1 -MinTestsKey`). Raise a floor in the PR that adds tests.
    - #408: a red scheduled nightly opens or comments on a "Nightly build failing" issue labelled `nightly-failure`; the next green one closes it. The Linux exclusion check runs only when the runner has the toolchain, and shows as skipped otherwise.
    - #415: gated release.
      - A tag `open3dbroadcast-vX.Y.Z` needs a `## [X.Y.Z]` CHANGELOG section naming the protocol, transport API and core versions.
      - The tag's version goes into both `.uplugin` files: VersionName X.Y.Z and Version X×10000 + Y×100 + Z (`Build/Scripts/release-version.py`).
      - The release runs PR CI's Fab zip and UE jobs; the UE job moved into the reusable `open3dbroadcast-ue-build-test.yml`.
      - It publishes the package the tests ran against. The Fab zip is a 90-day artifact, uploaded to Fab by hand.
      - A manual run is a dry run. One on #415's branch passed (run 37513123559); the publish job has not run yet (Build/README.md, "Releases").
    - BC-11: `.gitattributes` stores every text file with LF, and 127 CRLF or mixed files were renormalized, with content otherwise unchanged. `.sh` files are LF in every checkout, `.patch` files keep their bytes, and binaries are marked. A branch from before it may show line-ending conflicts when it merges `develop`; merge, then run `git add --renormalize .`.
  - **WP-R3, complete** (transport consistency):
    - #409, TR-5/SR-4: the sender pipeline records frames captured, bytes serialized and refused frames once, for every transport; transports record bytes sent and frames dropped after acceptance, and count `FramesSent` when a frame is sent, not queued.
    - #410, TR-6: `FO3DLogThrottle` (`Open3DShared/Public/O3DLogThrottle.h`) gives one line per 2 s, with a count of the lines it held back, for the UDP, NNG and WebRTC log sites a peer or a failing socket can repeat per packet.
    - #411, TR-9: the UDP and Loopback receivers guard their stats with a lock; conformance case `Stats.ReceiverReadFromAnyThread`.
    - #412, TR-8: the MoQ receiver no longer shows Delivery Mode and Queue Capacity.
    - #413, TR-10: the MoQ receiver drops what a lost session had queued.
    - #414, TR-4: not reproduced. NNG runs `Lifecycle.RestartAfterStop` and five control stop cycles on one port, and both pass repeatedly.
    - #416, TR-7: an item larger than the send queue's byte hard cap is `TooLarge`, not backpressure, and the pipeline requests no full sync after it. The conformance backpressure case now fills the queue, with the sender's worker held: new `HoldSenderWorker`, and a TCP pause hook.
  - **Branch protection and auto-merge (2026-10-06):** #419. `develop` has a ruleset: it requires the two result checks, allows squash merges only, and has no bypass. Auto-merge was tried on #419, #431 and #432 and dropped from #433 on: see Working practice.
  - **WP-D2, complete** (the end-user docs, P0 for Fab):
    - #420: plugin README (DOC-1, DOC-2, DOC-6);
    - #421: USER_GUIDE quick start and transport setup (DOC-5, part of DOC-10);
    - #422: transport READMEs (new for Sockets and Loopback) and the WebRTC add-on guide (TRB-44, TRF-36);
    - #423: public header docs (SND-37);
    - #424: USER_GUIDE Blueprint API and receiver references, troubleshooting, console variables, FAQ, limitations and privacy, and the transport comparison (DOC-10, RCV-32, DOC-5).
  - **WP-U6, complete** (UDP features and network defaults; maintainer decisions 2026-10-06):
    - #427, Loopback (TRB-31, TRB-32): a receiver no longer resets the channel's queue limits, a stopped receiver stops consuming, and a full queue warns (throttled);
    - #428, TRB-25: the dead `audio.port`, `audio.host` and `audio.bind` options are gone; audio and control share the data socket;
    - #429, TRB-22: no SO_REUSEADDR on the UDP receiver, the UDP sender or the TCP listener;
    - #430, TRB-16: the UDP sender splits every message above the MTU (header included, minimum 280). Control stays one datagram, and the sender's Max Datagram Bytes is a hidden ceiling. No wire change;
    - #431, TRB-28: the socket defaults live in `SocketsTransportCommon.h`;
    - #432, TRB-29: listening ends (TCP sender, UDP receiver, NNG listeners) default to `127.0.0.1` and warn once per Start on a non-loopback bind; USER_GUIDE "Network Exposure". **Breaking** for setups between machines that relied on `0.0.0.0`;
    - #433, TRB-21: UDP multicast (receiver **Multicast Group**, sender **Multicast TTL** and **Multicast Loopback**), **Allowed Senders**, and an opt-in **Share Port**; the receiver's **Accept Broadcast Packets** is removed.
    - Not verified: TCP, UDP and NNG between two machines with `0.0.0.0`, and multicast through a real switch or Wi-Fi (desk checks).
  - **Found while writing WP-D2:** all fixed: the Port tooltip and the dead audio options (#428), the MTU tooltips (#430), the Loopback queue-limit reset (#427), and in #435 the MoQ defaults (both ends now use `mocap/default`, track `primary`; maintainer decision 2026-10-07; **breaking** for a receiver set to a sender's Subject Name), the MoQ audio namespace for an unprefixed custom namespace, and the WebRTC reconnect default (the 5.0 initializer was overwritten; one constant now).
  - **Docs and cleanup batch (2026-10-07):**
    - #436, WP-D1: the root README checked against the code (DOC-3).
    - #437, WP-D3: the Copilot agent files point to `AGENTS.md` and `.claude/rules/`; the unused `Doxyfile` is removed (DOC-7, DOC-9; DOC-8 marked closed by #339).
    - #438, WP-D4: `Build/Scripts/check-markdown-links.py` and the CI job "Markdown links resolve" on every PR (it found and fixed 9 broken links); `.github/pull_request_template.md` with the section 7 checklist.
    - #439, WP-D3: `Source/Open3DShared/README.md` with each class's threading contract; the audio payload byte by byte in `docs/wire-format.md` (SHR-37).
    - #440, WP-Q1 core: `Matrix::Transpose` compiles; dead code and `o3ds.cpp` removed (CORE-25, partly).
    - #441, WP-Q1 plugin: dead code, `LogO3DSenderCurves` and `LogO3DSenderTransport`, no logs in Slate getters, exported log categories, header hygiene (SND-31, SND-33, TRF-33, SHR-28; SHR-29 partly).
    - WP-Q1's survey found 19 of its 37 sub-items already fixed by earlier work packages.
    - Flaky test: `core.repeater_tests` `Repeater_OversizeMessageClosesOnlyThatSender` failed once in CI (#440): the NNG push socket reconnects between the test's polls. Offered as a separate task; not fixed yet.
  - **UE 5.8 (2026-10-07):** the maintainer chose UE 5.7 and 5.8, without 5.6. The compile spike (WP-V1) is in `docs/roadmap/engine-version-5.8-spike.md`: the main plugin builds on 5.8 with 2 deprecation warnings (`OnPostEngineInit`) and passes 493 of 494 tests (a test bug: a relative path read with `std::ifstream`); the add-on stops on `PLATFORM_64BITS` deprecation warnings.
  - **UE 5.8 support, decided and built (2026-10-07):**
    - #444, ADR 0014 (supported engines: UE 5.7 and 5.8, Win64; two at a time; one source tree; `EngineVersion` stamped at packaging; a reduced 5.8 CI job; one Fab listing with a zip per engine). The plan is `docs/roadmap/engine-version-5.8-plan.md`.
    - #446, WP-V3: builds cleanly on 5.8 (`O3DEngineCompat.h` for `OnPostEngineInit`; `PLATFORM_WINDOWS` instead of the deprecated `PLATFORM_64BITS`; the capture test reads its full path).
    - #448, WP-V2: Target.cs pick V7/`Unreal5_8` on 5.8 and V6/`Unreal5_7` on 5.7; no `EngineVersion` in the source descriptors (5.8 skipped the add-on because of it); `fab-package.py --engine-version`; `Run-AutomationTests.ps1` fails on a plugin that names another engine. Verified locally: 5.8 and 5.7 each 549/549, every build without warnings.
    - #451, WP-V4: PR CI runs the UE job twice, the full job on 5.7 and a reduced one on 5.8 (build with warnings as errors, the tests, the add-on and its tests); `Setup-UE.ps1 -EngineVersion` checks the engine at the path. UE 5.8 is on the runner (maintainer).
    - #452, WP-V5: the release and the nightly run both engines in full (the nightly's flag builds, Linux check and Shipping game on 5.7 only); one zip per engine for each plugin (`Open3DBroadcast-Plugin-X.Y.Z-UE<engine>-Win64.zip`), made and checked by `release-version.py archives`; a Fab zip per engine; the manual test workflow takes `ue-version`. A release dry run (run 37651080532, version 0.0.1) passed on both engines; running `archives` on its packages found that the add-on package had no `EngineVersion`, which #452 fixed in `Build-WebRTCAddOn.ps1`.
    - #454, WP-V7: the READMEs, user guides, `.uplugin` descriptions, ProjectSandbox README, PR template and Copilot agent files name both engines. The two "one LiveLink client per process" sentences wait for WP-V6.
    - Not done: WP-V6 (LiveLink EngineTime and Timecode on 5.8, a live take) and WP-V8b (Fab: confirm the multi-version rules, upload a zip per engine) need the maintainer.
  - **Nightly fixed (#453, issue #443):** the 2026-10-07 nightly failed in three flag-combination builds because the TCP and NNG conformance fixtures called testing helpers that are compiled out with their transport; the fixtures are now guarded like MoQ's. A nightly run on the branch passed all four combinations and, on 5.8, the Shipping BuildPlugin, the strict build and the Fab zip build. #443 closes itself on the next green scheduled nightly.
  - **Runner notes (maintainer, 2026-10-07):** the self-hosted UE runner is a separate machine from the one the agent works on; the Linux and Mac runners are GitHub-hosted. `LINUX_MULTIARCH_ROOT` was set on the runner on 2026-10-06 and not on 2026-10-07, so the nightly's Linux exclusion check is skipped.
  - **WP-Q1 leftovers (maintainer took the recommendations, 2026-10-07):** #445, CORE-25: `ConcealmentEngine` without a predictor falls back to `HoldPredictor`; `o3ds.h` removed. SHR-29's exported `FUnifiedHeader` field names stay as they are.
  - **WebRTC reconnect (ADR 0015, 2026-10-07):** #447 (the ADR) and #449 (TRF-6, TRF-26). The FFI's `lk_set_reconnect_backoff` is a no-op and `lk_client_is_ready` is not a health signal, so both ends use LiveKit's connection state and `FO3DReconnectPolicy` (1 s to 30 s, no limit) after LiveKit gives up or a connect fails. The receiver's no-data watchdog is off by default and counts only while connected; the sender reconnects instead of staying `Failed`, closing the room livekit_ffi still holds and using fresh audio tracks. Not verified against a real LiveKit server.
- **Maintainer decisions:**
  - **WP-U2 (2026-10-05):**
    - `bAutoCreateTransport` stays false, with a warning;
    - Blueprint gets state events only, not per-frame ones;
    - the receiver library is a separate PR;
    - the sample map is WP-U5's.
  - **WP-U4 (2026-10-06):**
    - **RCV-6:** a source setting for the inactive-subject timeout, default 5 s, 0 meaning never. On timeout, clear the subject's frames but keep the subject and its LiveLink settings.
    - **RCV-20:** a Target Latency property, default 60 ms. Pre-roll before playback, and trim the oldest audio above target + 60 ms. No drift resampling.
    - **RCV-21:** a stream-label filter, and lock to the first matching stream until it goes idle. `o3ds:mix` becomes a shared constant with the same value, so the wire format is unchanged. Revised later the same day:
      - **Mix** stays strict on `o3ds:mix`, and a new **Any Stream** mode plays any stream;
      - no source filter, because Context Name already separates sources.
- **External:**
  - The LiveKit FFI request (`docs/livekit_ffi_feature_request.md`) is filed as `lifelike-and-believable/livekit-ffi` issues #13 to #17; the maintainer's agent is implementing them.
  - The rules-for-robots adoption findings are rules-for-robots issues #23 to #38.
- **Working practice:**
  - Every PR gets Auto-fix in the desktop app. Merge by hand when the success comment arrives (`gh pr merge <n> --squash`, after checking the checks). Auto-merge is allowed (maintainer, 2026-10-06) but hides the green signal: the app relays nothing for a PR that is already merged when it reads it. Events also arrive late during a long turn: the app holds them until the turn ends, so check a PR yourself before relying on its event. Parallel PRs that each add a CHANGELOG entry at the top of a section conflict after every merge; merge `develop` into the next one at once.
  - A workflow's success comment is the cue that CI is green; check the checks before merging.
  - Delete `ProjectSandbox/Intermediate/Build/Win64/x64/ProjectSandboxEditor/Development/Makefile.bin` before building whenever files were added, removed, merged in from `develop`, or changed by a branch switch. UBT otherwise reuses its makefile, which can:
    - skip new or merged-in files, so a local run lacks their tests;
    - still include a deleted file, so the build fails;
    - and after a failed build, the automation run uses the old binaries and can report green.

    Check that the build succeeded and compare the test count: 553 with the WebRTC add-on (494 without) on `develop` as of #449. CI fails below the floors in `Build/automation-test-floors.json`. When several PRs add tests, each raises the floors by its own tests; after one merges, merge `develop` into the others and add their tests to `develop`'s floors.
  - The test editor runs with `-NoSound`, so audible playback can't be tested automatically. Audio tests cover the component's state and the jitter buffer; listening is a desk check.
  - `close_findings.py`-style helpers must put the Status line after a finding's last top-level bullet, not inside nested bullets.
- **Next, after the maintainer's go-ahead:**
  1. UE 5.8 (ADR 0014; plan `docs/roadmap/engine-version-5.8-plan.md`): WP-V6 (LiveLink EngineTime and Timecode on 5.8, one LiveLink client per process, a live take by the maintainer) and WP-V8b (Fab, desk). Check the first scheduled two-engine nightly (the first with tests and the add-on on 5.8). The runner's Linux toolchain (`LINUX_MULTIARCH_ROOT`) needs the maintainer.
  2. Desk checks: WP-U6 between two machines and multicast on real network gear; MoQ against a real relay (the new defaults); WebRTC through a network drop of more than 45 s (#449).
  3. The flaky core test `Repeater_OversizeMessageClosesOnlyThatSender` (the NNG push socket reconnects between the test's polls; it failed once in CI on #440): offered as a separate task.
  4. The names-in-use picker: not specified yet; ask the maintainer what it should do.
  5. WP-U5: sample content. It also carries WP-U2's acceptance, and it needs the maintainer at the editor.

  The desk items below are unchanged. Also for the maintainer:
  - (done 2026-10-06) branch protection: a `develop` ruleset requires "Plugin CI result" and "Core tests result", allows only squash merges, blocks deletion and force pushes, and has no bypass (`Build/README.md`);
  - (done 2026-10-06) the fork-PR approval policy is `all_external_contributors`, so fork PRs can't run code on the self-hosted runner unapproved. Read a fork PR's workflow, Build.cs and script changes before approving its run (mid-project review BC-1);
  - listen to a received audio stream at the default 60 ms Target Latency (#396), pause the sender, and check that playback resumes;
  - set a translator on a LiveLink subject, stop the sender for longer than the Inactive Subject Timeout, restart it, and check the translator is still there (#395). During the pause, also check that the subject shows no data rather than a frozen pose; the mid-project review's RR-2 expects a frozen pose while concealment is on;
  - over TCP with a slow receiver, or NNG with its buffer full, check that a residual-coded subject recovers at once rather than after the periodic full sync (#403);
  - with two receiver sources sending audio, check that a component in Any Stream mode plays one of them cleanly and switches to the other about a second after that sender stops (#398);
  - drop a live MoQ relay session while receiving, and check that no stale frames play after the reconnect (#413; tested only against the fake moq-ffi);
  - the first real release: add the `## [X.Y.Z] - date` CHANGELOG section in a PR, merge it, then tag that merge commit. The `.uplugin` files still say 1.0, and the last release was 0.9.6, so the version number is the maintainer's call. That first tag is also the first run of the release's publish job (#415);
  - follow the USER_GUIDE quick start on a clean machine with UE 5.7 (WP-D2's acceptance, #421). Check these too:
    - the UI names it uses: the **O3D Sender** Add-menu entry, **Window → Virtual Production → Live Link**, **Add Source**;
    - the Third Person template paths;
    - typing a subject name into **Live Link Pose** before the subject exists;
    - that an editor-created LiveLink source receives Loopback frames from Play In Editor;
  - also check: several UDP senders feeding one LiveLink source; UDP broadcast to a subnet address; the Retargeting section's Control Rig options (#424).

**Merged this session (#335–#355; later PRs under "Open at hand-over"):**
- **WP-A4 protocol versioning (ADR 0009):** #335 (frame word, identifier, protocol 2, core 1.1.0), #336 (UDP fragment header v2), #337 (envelope v2, LE PCM), #338 (length-prefixed name hash), #339 (`docs/wire-format.md`, one changelog), #340 (no compatibility with formats before protocol 2; maintainer: there are no users of old receivers).
- **ADR 0005, resync and loss contract:** #341 (sender stamping, `O3DS::StreamWriter`, one per subject), #342 (`SubjectUpdate.ref_seq`, receiver resync contract; residual streams recover only at a full Subject), #343 (residual only on `ReliableOrdered` transports, UI warning), #344 (peer-joined trigger, TCP and NNG). ADR 0005 is implemented except the WebRTC peer join (needs the LiveKit FFI's participant events verified, Q4).
- **Other WP-A4 findings:** #345 (CORE-15, a forged far-ahead `tx_seq` no longer blackholes a gated stream), #346 (CORE-11, updates carry scale), #349 (the residual fallback sends full snapshots, never quantized frames; maintainer: quantization only when enabled), #350 (CORE-13, residual rotations normalized identically on both ends and hemisphere-aligned; curve histories agree), #352 (CORE-12 measurement harness `apps/QuantEval`, and a full sync now counts as sent: before, a value returning near one sent before a full sync was skipped, up to 1 degree off, in every delta encoding).
- **SHR-38:** ADR 0012 accepted (#347), revised to do the runtime context now while transport API 5 is unreleased.
- **WP-A7:** #348 archived `plugins/maya`, `plugins/mobu`, `python/` and `sphinx/` (tag `archive/dcc-plugins-python-sphinx`).

**Open at hand-over:**
- **Also merged:** #353 (CORE-14: `Open3DStreamCore` builds with `FPSemantics = Precise`; both ends round the predicted reference to float32), #354 (this section), #355 (`o3d.Sender.Capture.Start/Stop`). #353's first CI runs failed because the self-hosted runner had stopped; it passed after the runner machine was restarted.
- #351: ADR 0013, timecode on LiveLink frames (RCV-8), **Accepted** 2026-10-04 with its open questions' defaults; not implemented yet.
- **ADR 0012 (SHR-38) implemented, 2026-10-04:** #357 (PR 1: `FO3DRuntimeContext`, default context behind the statics), #358 (PR 2: `FO3DTransportConfig::Context`; NNG, MoQ and WebRTC record into it; CI `Build/Scripts/check-transport-metrics.py`), #359 (PR 3a: per-receiver metrics handles), #360 (PR 3b: per-sender metrics handles through `FO3DTransportConfig::SenderMetrics`; only transports record sender metrics, so the component passes its handle down), #361 (PR 4a: `UO3DRuntimeSubsystem`, `ContextName` on the receiver source config and the remote audio and control components, `o3d.DumpMetrics` per context), #364 (PR 4b: `ContextName` on the sender component), #365 (PR 5: `docs/dev/runtime-services.md`). Open follow-up: the names-in-use picker (Next, item 3).
- **ADR 0013 (RCV-8) implemented, 2026-10-04:** #366 (PR 1: optional `SubjectList.scene_time`, protocol 2, `O3DS::SceneTime` and `IsValidSceneTime`, reset on every parse), #367 (PR 2: the sender stamps `FApp::GetCurrentFrameTime()`), #368 (PR 3a: `FO3DSceneTimeMapper` sets every LiveLink frame's SceneTime from the sender's timecode, the sender's timeline continued, or WorldTime on the engine timecode; the per-frame `CurveHash` and `SubjectListTime` string metadata are removed, RCV-11), #369 (PR 3b: control alignment in Timecode mode). ADR Q3 resolved from engine source. Not done yet: the manual test at the desk.
- **CI:** #363, documentation-only changes (Markdown, `docs/`) start no builds: `core-tests.yml` ignores them and the plugin CI's change filter excludes Markdown.
- **HANDOFF:** #362 recorded the ADR 0012 progress and the names-in-use follow-up.
- **Updated NNG Repeater (WP-A7), 2026-10-04:** #371 (rebuilt on raw NNG, pull to pub: `apps/Repeater/relay.h`/`.cpp` as a library, a receive size limit (`--max-message-mb`, default 64), send buffering (`--send-buffer`), backoff on receive errors, a stats line every `--stats-seconds` instead of a log line per message, clean stop on SIGINT/SIGTERM; `test/repeater_tests.cpp`), #372 (`docker/Dockerfile.repeater` builds only the Repeater from the pinned submodules and ships `nngcat`; `.dockerignore`; the disabled publish workflow `repeater-image.yml` has narrowed paths and a real relay smoke test; the new **Repeater image test** workflow builds the image and runs that smoke test plus a clean `docker stop` check on PRs touching the image, never pushing). The image is verified only by that CI workflow (no Docker on the dev machine). It is still not published: enabling `repeater-image.yml` (GHCR pushes) is the maintainer's call (§5).
- **Legacy apps and connectors (WP-A7, CORE-27, CORE-28), 2026-10-04:** #375 removed `apps/FbxStream`, `Test1`, `SubscribeTest` and `XSensTest` (tag `archive/legacy-apps`) and moved the legacy connectors into an optional `open3dstream_legacy` library behind `O3DS_BUILD_LEGACY` (default OFF). A default core build needs neither NNG nor libdatachannel; the WebRTC options apply only with legacy ON. Windows CI builds with `-DO3DS_BUILD_LEGACY=ON`; nothing links the legacy library, so link errors inside it would not show.
- **LiveKit FFI request and findings, 2026-10-04:** #374 and #376 extend `docs/livekit_ffi_feature_request.md` with section 11 (participant joined/left events; the SDK has them but both FFI event loops drop them, and participants present at connect must be reported from `remote_participants()`), 12 (delivery: every send is a reliable ordered byte stream, `LkLossy` and the `ordered` flag are ignored, and `lk_send_data_ex` blocks the caller, holding the client lock, during a reconnect) and 13 (the async-connect loop calls only the unlabeled data callback). Checked against `lifelike-and-believable/livekit-ffi` `main` (50e6663) and the `livekit` 0.7.24 crate, cloned at `U:\o3dwt\livekit-ffi` (the `E:\OtherProjects\livekit-ffi-ue` path in `.claude/CLAUDE.md` does not exist on this machine). The maintainer forwards the request to the FFI developers.
- **WebRTC receiver received no data (since #271, TRF-16):** the receiver connects with `lk_connect_with_role_async` and registered only the labeled data callback, which that loop never calls. #377 registers both callbacks (no FFI version calls both for one packet; the `default` label from the unlabeled one reaches logs only, since streams are keyed by the packet's subject names, RCV-5). Found by reading the source; needs the WebRTC manual test (cases 2 and 9) at the desk.
- **Required checks possible (2026-10-05):** #379 makes `core-tests.yml` start on every PR, with a "Detect core-relevant changes" job skipping the core jobs for Markdown/`docs/`-only changes, so every core check reports and can be required on `develop`. `Build/README.md` lists the ten checks to require; turning on branch protection is the maintainer's setting (§5).
- **Agent instructions (2026-10-05):** #380 makes `AGENTS.md` the one instruction file (root `CLAUDE.md` imports it; `.github/copilot-instructions.md` points to it; `.claude/claude.md` and the tracked `.claude/settings.local.json` are gone). `.claude/rules/` holds the rules-for-robots core and `unreal-plugin` rules, waivers for UE-006, UE-007, FAB-001, FAB-002, FAB-003, and project rules O3D-001 to O3D-005; `.claude/settings.json` enables the `rfr-core` hooks. rfr's lint flags the `O3D-*` files only because it does not support project prefixes yet (rules-for-robots #27). Mechanical fixes and rule proposals found while adopting it are rules-for-robots issues #23 to #38.
- **Copyright year (2026-10-05):** #382: plugin sources start with `// Copyright 2026 Lifelike & Believable. All Rights Reserved.`; Open3DStream-derived files add `// Portions Copyright (c) Open3DStream Contributors` as the second line (Fab TR 4.3.6.1.b). Hyphenated names (`moq-ffi`, `*-LICENSE.txt`) stay until Fab review objects: #381.

**Maintainer decisions (2026-10-03):**
- No users run old receivers; compatibility with formats before protocol 2 is not kept.
- Quantization: go carefully, because the July 2026 attempt showed idle-animation jitter (#247). Measure first (QuantEval), change defaults only after real takes and a live test at the desk. Quantization is never switched on implicitly.
- CORE-13 and CORE-14: the recommended approaches (done in #350, #353).
- ADR 0012: accepted, both stages now.
- RCV-8: sender timecode on the wire with a receiver-derived fallback (ADR 0013, accepted 2026-10-04).
- WP-A7: archive Maya, MotionBuilder, `python/`, `sphinx/` (done). The Repeater image is not deployed; an updated NNG Repeater is wanted (done, #371, #372).
- Agent and repository conventions (2026-10-05): PRs are squash-merged into `develop`; release tags are `open3dbroadcast-vX.Y.Z`; the rules-for-robots waivers above are approved.
- WebRTC add-on distribution (2026-10-05): from the Open3DBroadcast website to registered plugin users, not Fab, so Fab rules (no dependency on user-made plugins) do not apply to it. Third-party code stays in per-module `Source/<Module>/ThirdParty` folders unless Fab review rejects it.
- Repeater late joiners (2026-10-04): no cache. A receiver that joins behind the Repeater waits for the sender's next full frame, because the sender's peer-joined trigger cannot reach it; `apps/Repeater/README.md` says so.

**Next, in order:**
1. **At the desk (maintainer):**
   - record takes with `o3d.Sender.Capture.Start` on the default (legacy) encoding, above all the idle animation that jittered in July, and run `QuantEval --capture <take>`;
   - the RCV-9 live check (narrowed: LiveLink evaluates on the pushed `WorldTime` after a source-wide offset estimate; the fixed-timestep question is answered in ADR 0013 Q3);
   - the ADR 0013 manual test: `USystemTimeTimecodeProvider` on both machines (turn off *Generate Full Frame*, or turn on the LiveLink source's *Generate Sub Frame*), a LiveLink source in Timecode mode, and check that frames are selected by the sender's timecode (user guide, "Timecode Mode");
   - WP-A5 (reconnect, needs live servers);
   - the WebRTC manual test, cases 2 and 9 at least, to confirm #377 (receiver data).
2. **CORE-12 follow-up, after real numbers:** rotations use the 16-bit tier only (QuantEval's synthetic suite: idle-bone jitter 0.40/0.86 degrees p95/max at the UE defaults, 0.0096/0.020 without the Byte tier, for 5 to 10% more bytes), normalize before quantizing, record the dequantized value as last sent. Quantization stays off by default until the maintainer's live test passes.
3. **ADR 0012 follow-up:**
   - **Names-in-use picker (gap against ADR 0012 accepted default Q3, "the panels show the names in use"):** `ContextName` is a plain text field. UE 5.7's `GetOptions` meta uses `SPropertyEditorCombo`, which only picks from a list (no free text), so showing names in use needs a small details customization in Open3DBroadcastEditor: a text field plus a list of the names in use (`UO3DRuntimeSubsystem::GetNamedContexts`). Maintainer, 2026-10-04: record it here; not yet scheduled.
   - Q4 is resolved: UE 5.7 has one `FLiveLinkClient` per process (`LiveLinkModule.h:60`, registered in `LiveLinkModule.cpp:57-61`), so LiveLink subject names stay process-wide; the user guide says so (#361).
4. **ADR 0013:** implemented (#366 to #369); only the manual test at the desk remains (item 1).
5. **Updated NNG Repeater:** done (#371, #372). Late joiners: no cache (maintainer decision above). Open: whether to publish the image (§5).
6. **Later:** WebRTC peer join (ADR 0005 Q4; waits on FFI request section 11); the sender blocking in `lk_send_data_ex` during a reconnect (FFI request section 12; consider sending off the game thread meanwhile); the rest of WP-A7; M4; Fab F0 and F5. Done: archiving the four legacy apps and CORE-27 (#375).

**Working notes from this session:**
- Never run two UBT builds at once. While a strict build uses the main checkout, do other branches in git worktrees under `U:\o3dwt\` (`git worktree add`), and build the core there with `U:\o3dcore\build-core-at.cmd <repo dir> <build dir>`, one build dir per checkout path (CMake refuses a build dir made for another source path).
- `sync_o3ds_core.py` needs the submodules, which only the main checkout has. After merging `develop` into a branch elsewhere, take `develop`'s `SYNC_STAMP.txt` and regenerate it in the main checkout before pushing.
- Every PR that edited the long "Start here" line of §0 conflicted with the others; edit this section, not that line, and keep status changes to one docs PR at the end of a batch.
- Line endings: since WP-R2's BC-11, `.gitattributes` stores every text file with LF. The CRLF staging rules of earlier sessions no longer apply.
- The self-hosted UE runner once failed with `FileLoadException` loading its compiled build-rules assembly; it is not a code failure, and a new run passes.
- Docs-only PRs skip the plugin jobs and get no bot comment, so check their checks directly before merging.
- The maintainer's Pushover notification when Claude stops is a user-level Claude Code hook (`~/.claude/settings.json`), not part of this repository. `.claude/settings.local.json` is ignored (personal settings); shared settings are in `.claude/settings.json` (#380).
- `nngcat --ascii` prints messages without a newline, and `docker logs` holds back a partial line while the container runs; use `--quoted` in container tests (#372).
- `Build/Scripts/Run-AutomationTests.ps1` writes its report under the current directory's `Artifacts/`; run it from the repo root, or delete a stray `Artifacts/` inside the source tree before committing.
- One-shot local check for a plugin change: build, WebRTC add-on build, then all tests on the add-on host project (469 tests with both plugins as of #361).

## 0. Handover to Claude on desktop (2026-10-03)

The cloud session stops after WP-A2c (#318). The next session runs on the maintainer's machine.

- **State of `develop`:** WP-A1 complete; WP-A2a (#316), A2b (#317) and A2c (#318) merged. WP-A2d (audio clock, cached device enumeration) merged as #319. WP-A2e (core CRC and builder reuse) done on desktop as #320, built and tested locally (UE 5.7 and the core with MSVC). `O3D_TRANSPORT_API_VERSION` is **5**, unreleased (last tag v0.9.6); everything since c98c92c is under Unreleased in the CHANGELOG. No open PRs from the cloud session.
- **Start here:** WP-A3 (god classes) is complete; its plan (eight steps, rules) is in the roadmap's WP-A3 section. Step 1 (`FO3DReceiverFrameDecoder`) is #325; step 2 (`FO3DLiveLinkPublisher`) is #326; step 3 (stream scheduler and concealment) is #327; step 4 (control router, header without `o3ds/` includes, `Open3DStreamCore` private) is #328; step 5 (RCV-13: doubles kept, `InvalidPosesDropped` metric) is #329; step 6 (sender pose sampler) is #330; step 7 (sender audio binding) is #331; step 8 (sender transport options and secrets, restart properties) is #332, the last of the plan. WP-A6 (global state, CVars) is complete (#333, #334). WP-A4 (protocol versioning, ADR 0009) is in progress: PR 1 (D8 core: frame word, identifier, protocol 2, core 1.1.0) is #335; PR 2 (UDP fragment header v2, datagram classification) is #336; PR 3 (envelope v2, LE PCM) is #337; PR 4 (length-prefixed name hash, item 8) is #338; PR 5 (`docs/wire-format.md`, one changelog, ADR 0009 implementation notes) is #339, which completes ADR 0009 except the Fab changelog packaging (Q4). No users run old receivers, so compatibility with formats from before protocol 2 is not kept: the follow-up #340 removes envelope v1, the v1 keepalive, the pre-D8 frame check, the golden fixtures and the old-reader CI step. ADR 0005 is in progress in four PRs: A, sender stamping (`O3DS::StreamWriter`, one per subject in the UE serializer; SND-15, CORE-29), is #341 (UE senders' frames now take the receiver's gated path, so LiveLink gets the mapped `WorldTime` instead of arrival time: RCV-9 needs a live check at the desk); B, `ref_seq` and the receiver resync contract (CORE-5, CORE-6; residual streams recover only at a full Subject), is #342; C, the delivery-guarantee residual fallback with a UI warning (ADR 0005 (iii)), is #343; D, the peer-joined trigger (TCP and NNG; WebRTC waits for LiveKit FFI verification, Q4), is #344. CORE-15 (a forged far-ahead `tx_seq` blackholed a gated stream; live since #341) is fixed in #345. CORE-11 (scale was never sent in updates) is fixed in #346. CORE-13 (residual rotations drifted from unit length; q/-q resends; curve history mismatch) is fixed in #350. The residual fallback on unreliable transports sends full snapshots, not quantized frames (maintainer, 2026-10-03: quantization only when enabled), in #349. SHR-38: ADR 0012 is accepted (#347): a runtime context (metrics, audio bus, control bus) with a process default, passed to transports through `FO3DTransportConfig` while transport API 5 is unreleased, per-instance metrics handles, and named contexts hosted by an engine subsystem; five PRs, not started. WP-A5 (reconnect; its acceptance needs live servers) waits until the maintainer is back at the desk. WP-A2 is complete except removing `o3d.Sender.AsyncPipeline` one release later (§2a item 6). WP-A7: the MotionBuilder and Maya plugins, `python/` and `sphinx/` are archived at the tag `archive/dcc-plugins-python-sphinx` (#348); an updated NNG Repeater is wanted (its image is not deployed).
- **Local core build:** the core and its CTest suite build with MSVC like core-tests.yml's Windows job (vcvars64, Ninja, NNG/CRCpp/FlatBuffers installed to a prefix, `-DO3DS_BUILD_TESTS=ON`). `sync_o3ds_core.py` needs git to trust the submodule checkouts on this machine ("dubious ownership"); pass it for one process with `GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0=*` rather than changing the global git config.
- **What a local UE 5.7 build can now do that the cloud session could not** (pitfall 25: the cloud session reached neither the UE source mirror nor the Epic docs, so every engine API added in WP-A2 was first checked by the CI compile):
  - Record the **Unreal Insights numbers** ADR 0008 Verification asks for (before/after with `o3d.Sender.AsyncPipeline` 0 and 1). The capture steps, timers (`O3D.Sender.Sample`, `O3D.Sender.Pipeline.Serialize`, `O3D.Sender.Pipeline.Send`), `o3d.Sender.DumpPipelineStats` and the budgets are in the ADR 0008 addendum "implementation notes (WP-A2c)". Record them on #318 or in a follow-up PR.
  - Check the unverified engine APIs listed in the ADR 0008 addenda (WP-A2b tick APIs; WP-A2c `UE::Tasks::Launch`, `ETaskPriority::BackgroundHigh`, `TRACE_CPUPROFILER_EVENT_SCOPE_STR`) and ADR 0008 open questions 1 and 2 against the engine source.
  - Add a small skeletal mesh test asset so the ADR 0008 root-bone pose-equality test can be written (§2a item 2).
- **Follow-ups found on the way (not started):** the WebRTC add-on's `SendSerialized` should take the lifetime gate `SendControl` uses (§2 leftovers); MoQ's own backoff, the UDP pause hook not waiting (pitfall 15) and the unused schema features (§2 leftovers).
- **How the cloud session worked each step:** branch from `develop`; local checks (§4); push; open the PR as a draft, then mark it ready, which starts the self-hosted UE job (build with `-FailOnWarnings`, automation tests with and without the WebRTC add-on, strict non-unity/no-PCH build, Fab zip) beside the core-tests workflow; squash-merge only when every check on the head commit is green. `Run-AutomationTests.ps1` reports only the first failed assertion per test (CI annotations show it); the full `Automation.log` is in the job artifacts. Runner-side failures seen so far (App Control 0x800711C7, the editor exiting at startup) cleared on one re-run.
- **Worktrees:** the cloud session's agent worktrees under `.claude/worktrees/` are local to its container and are not in the repository.

## 1. Where things stand

The plan is `docs/roadmap/plugin-hardening-and-fab-readiness.md`. Design decisions are in `docs/adr/0001`–`0013`, all **Accepted**; their open questions were accepted with the recommended defaults. Work is organised as work packages (WPs), one PR each (or a PR series for large ones), squash-merged into `develop`.

| Milestone | Status |
|---|---|
| M0 Decisions (ADRs 0001–0010) | Done (#261–#263) |
| M1 Safety and correctness: WP-S1..S11, WP-T1, WP-T2 | Done (#264–#279) |
| M2 Fab-buildable package: WP-F1..F4, F6..F9, F11 | Done (#274–#286). **F0 and F5 wait on the maintainer** (see §5) |
| M3 Architecture: WP-A1..A7 | **In progress.** WP-A1, A2 (except removing `o3d.Sender.AsyncPipeline` one release later), A3 and A6 done; WP-A4 nearly done (ADR 0009 and ADR 0005 implemented, CORE-11/13/15 fixed, CORE-12 harness merged, CORE-14 in #353, RCV-8 in ADR 0013, accepted); WP-A5 waits for the maintainer at the desk; WP-A7 partly done (#348). SHR-38: ADR 0012 implemented (#357–#365). See §0a. |
| M4 Usability and docs: WP-U1..U6, WP-D1..D4, WP-Q1 | **In progress.** WP-U1, U2 and U3 done (#385–#391); WP-U4 done (#393, #395, #396, #398); WP-U6 done (#427–#433); WP-D1 to D4 done (#420–#424, #436–#439); WP-Q1 done (#440, #441, #445, #449). UE 5.8: ADR 0014, WP-V2 to V5 and V7 done (#444, #446, #448, #451, #452, #454); WP-V6 and V8b open. Open: WP-U5 (needs the maintainer at the editor). See §0a. |
| M5 Fab submission: WP-F10 | Not started; needs F0, F5 and the listing details in §5 |
| WP-CTL control channel (D11, ADR 0011) | CTL-1..7 done (#290–#295, CTL-6, CTL-7); live-server checks remain (see §2b) |

Recent merges on `develop`: WP-A1 PR 4a–4f (#305–#310), PR 5a–5c (#311–#313), step 6 (#314, 5ee5ac6), review-docs note (#315), WP-A2a (#316, fc147b7), WP-A2b (#317, aadc41f), WP-A2c (#318). Earlier: F7 editor split (#285), F11 WebRTC add-on (#286), WP-A1 PR 1 (#289), ADR 0011 (#290), CTL-1..5 (#291–#295).

## 2. The work in flight: WP-A1 (transport core consolidation)

Design: `docs/adr/0007-transport-abstraction-and-registry.md`, section "Implementation outline". WP-A1 is a series of PRs; each must keep every transport working and the conformance suite green.

1. **Interfaces and one registry** (SHR-12, SND-23, RCV-27, RCV-28, SHR-24). **Done: #289, merged as d365c34.**
   - `IOpen3DSender`, `IOpen3DReceiver`, their audio sinks, `ISerializedFrameConsumer` and `FO3DTransportConfig` now live in `Open3DShared/Public/Transport/`, exported by Open3DShared.
   - `FO3DTransportRegistry` (`Transport/O3DTransportRegistry.h`): each transport registers one immutable `FO3DTransportDescriptor` and holds a move-only `FO3DTransportRegistration`. `Find` returns `TSharedPtr<const FO3DTransportDescriptor>`; `GetNames(Role)` feeds the pickers from the same entries `CreateSender`/`CreateReceiver` use; `OnTransportsChanged` fires on every change. Duplicate names and API-version mismatches are refused.
   - Old Sender/Receiver headers are deprecated forwarding shims (comments only, no `UE_DEPRECATED`), removed next minor release with an API-version bump. Register/unregister were documented as game-thread only; step 2 added the `check`.
   - Tests: `Open3DBroadcast.Shared.TransportRegistry.*` (7 cases).
2. **Lifetime** (SHR-13, TRF-14). **Done (WP-A1 PR 2).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 2)".
   - The registry keeps a live list of the instances `CreateSender`/`CreateReceiver` hand out; `GetNumLiveInstances(Name)` counts them, leaks included.
   - Unregistering drains: the name goes, `OnTransportUnregistering(FName)` fires, the sender transport controller (through the component's `TeardownTransport`) and the receiver source stop and release their instance and its sinks, then the registry stops what is left and logs an Error naming the transport.
   - `FO3DFfiLibrary` asks the registry (`FO3DFfiLibraryDesc::TransportNames`) instead of tracking instances; `TrackInstance`/`StopLiveInstances` are gone. MoQ and the WebRTC add-on reset their registration, then unload.
   - Register/unregister `check(IsInGameThread())`. `O3D_TRANSPORT_API_VERSION` is now **3** (FFI library layout).
   - Tests: `Open3DBroadcast.Shared.TransportLifetime.*` (7 cases) and the reworked `Open3DBroadcast.Shared.FfiLibrary.*`.
3. **Results, state, capabilities** (SHR-14). **Done (WP-A1 PR 3).** Details and deviations from ADR 0007 item 3 in the ADR 0007 addendum "implementation notes (WP-A1 PR 3)".
   - `Initialize`/`Start` return `FO3DTransportResult` (code and message; explicit `operator bool`). `SendSerialized(FO3DSendPayload&&)` is pure virtual and returns `EO3DSendResult`, as does `SendControl`. Same failure, same code on every transport (`NotRunning`, `InvalidConfig`, `NoConsumer`, `AddressInUse`, `NotConnected`, ...).
   - `EO3DConnectionState` with `GetConnectionState()`/`SetStateChangedCallback()` on both interfaces, implemented by `FO3DConnectionStateTracker` (Open3DShared): lock-free reads, callback on the changing thread, nothing after `Stop`.
   - `FO3DTransportCapabilities` on both interfaces and on the descriptor (`FO3DTransportRegistry::GetCapabilities`); it holds ADR 0005's delivery guarantee and ADR 0011's control flag. `SupportsAudio()`/`SupportsControl()` became non-virtual forwarders (removed in step 6). `SetPeerJoinedCallback` is deferred to WP-A4b (`bPeerJoinSignal` false everywhere).
   - `O3D_TRANSPORT_API_VERSION` is now **4**. `FO3DTransportStats` gained `State`, `SendErrors`, `ReceiveErrors`, `PendingFrames`, `PendingBytes` (mostly zero until step 4's shared queue fills them).
   - Tests: `Open3DBroadcast.Shared.TransportResult.*`, `.ConnectionState.*`, `.TransportCapabilities.*`, `Open3DBroadcast.Transport.Results.*`, five new conformance cases, and the WebRTC add-on's `Results`/`State`/`Capabilities` tests.
4. **Shared building blocks, one transport per PR**, in order Loopback, TCP, UDP, NNG, MoQ, then WebRTC (in the add-on). Each PR deletes that transport's own queue, demux, sink and option-parsing copies and drops its Build.cs dependency on Open3DSender/Open3DReceiver. Control landed before this step, so each migration must also carry that transport's control path: the shared send queue has the "control" item type that is never dropped for mocap (ADR 0007 item 7), and the shared demux routes control through `TryGetControlPayload` (CTL-2/3).
   - **Building blocks + Loopback done (WP-A1 PR 4a).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4a)".
     - In `Open3DShared/Public/Transport/`: `FO3DSendQueue` (per-kind limits and atomic accounting; mocap `DropOldest` or `RefuseNewest`; audio and control never evicted; control cap 1,024; optional age limit), `FO3DTransportWorker` and `FO3DReconnectPolicy`, `FO3DUnifiedReceiveDemux`, `FO3DAudioPublishState` + `FO3DQueuedSenderAudioSink` (and the moved `FO3DSenderAudioSinkBase`/`FO3DGatedSenderAudioSink`; Open3DSender keeps a forwarding header), `O3DTransportOptions` (typed getters, `ParseHostPort`, `ResolveHostPort`). `O3DAudio::TryGetAudioPayloadCodec` added; `NormalizeTcpUrlHostPort` deleted (SHR-9).
     - Loopback: one shared queue per channel (`RefuseNewest`), demux in `Poll`, the shared sink; no Open3DSender/Open3DReceiver dependency. Net transport lines -311.
     - `O3D_TRANSPORT_API_VERSION` stays **4** (standalone types only).
     - Tests: `Open3DBroadcast.Shared.SendQueue.*`, `.TransportWorker.*`, `.ReconnectPolicy.*`, `.ReceiveDemux.*`, `.AudioSinkBase.*`, `.HostPort.*`, `.TransportOptions.TypedGetters`; `Open3DBroadcast.Transport.Loopback.Audio.IndependentOfFrameQueue`, `.Lifetime.StopWhileSending`.
   - **TCP done (WP-A1 PR 4b).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4b)".
     - Sender: frames, audio and control are items on one `FO3DSendQueue` (`RefuseNewest`, `tcp.maxqueue` per kind, `tcp.maxqueueage` for frames and audio); an `FO3DTransportWorker` writes each item as one TCP frame (header written on the worker, no copy); the sink is `FO3DQueuedSenderAudioSink`, refused without a client through the new `FO3DAudioPublishState::SetPeerReady`. The bind address must be an IP literal or a wildcard (no DNS on the game thread), as before.
     - Receiver: an `FO3DTransportWorker` resolves (`ResolveHostPort`), connects, reconnects with `FO3DReconnectPolicy` (jitter added), reads and frames; payloads go through a bounded hand-off queue (8 MiB, at least one `tcp.maxframe`) to `Poll`, which feeds `FO3DUnifiedReceiveDemux`. A full hand-off queue stops reading, so TCP flow control holds the sender back. The receiver holds the consumer strongly and releases it in `Stop` (TRF-38).
     - Config: `ParseTcpEndpoint` (strict, bracketed IPv6) and the configure functions read only the config. `SocketsTcpAudio.*` and `BuildTcpUri` are gone. The module still names Open3DSender/Open3DReceiver for UDP's configure functions.
     - `O3D_TRANSPORT_API_VERSION` stays **4**.
     - Tests: `Open3DBroadcast.Transport.Sockets.Tcp.AudioIndependentOfFrameQueue`, `.ReceiverBacksOffWithoutSender`, `.StopWhileSending` (1,000 cycles, 50 with a client); `SocketsTesting.h` gained `TcpReceiverGetFailedConnectAttempts`.
   - **UDP done (WP-A1 PR 4c).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4c)".
     - Sender: frames, audio and control are items on one `FO3DSendQueue` (`DropOldest`, soft cap 4 frames / 16 MiB, refused at twice that; audio 1 MiB; control 1,024); an `FO3DTransportWorker` sends each item, fragmenting above `udp.maxdatagram` (wire format unchanged), so no caller calls `SendTo` (TRB-20). A host name resolves on the worker (`ResolveHostPort`, retried with `FO3DReconnectPolicy`; `Connecting` until then); an IP literal still resolves in `Initialize`. The sink is `FO3DQueuedSenderAudioSink`, refused while there is no socket.
     - Receiver: still `Poll`-driven (ADR 0007 leaves it open); complete messages go to `FO3DUnifiedReceiveDemux`. Consumer held strongly, released in `Stop`. Bind must be an IP literal (IPv6 now works) or a wildcard/`localhost` (`O3DTransportOptions::IsIpLiteral`, new).
     - Config: `O3DSockets::ParseEndpoint` (strict, shared with TCP) and the configure functions read only the config. **`Open3DTransportSockets` no longer depends on Open3DSender/Open3DReceiver.**
     - `O3D_TRANSPORT_API_VERSION` stays **4**.
     - Tests: `Open3DBroadcast.Transport.Sockets.Udp.QueueDropsOldestUnderBackpressure`, `.AudioIndependentOfFrameQueue`, `.StopWhileSending` (1,000 cycles); `Open3DBroadcast.Shared.HostPort.IsIpLiteral`; `SocketsTesting.h` gained `UdpSenderSetWorkerPaused`.
   - **NNG done (WP-A1 PR 4d).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4d)".
     - Sender: frames, audio and control are items on one `FO3DSendQueue` (`RefuseNewest`, `nng.qmax` per kind; control 1,024) that an `FO3DTransportWorker` hands to `nng_send(NONBLOCK)`; `NNG_EAGAIN` still drops the oldest at the worker, so the policy is right for pub/sub and for pair/push. `FramesSent` is still counted after `nng_send` (#301), frames only. NNG keeps redialing dropped connections itself; `FO3DReconnectPolicy` only paces reopening a socket that failed or was closed. The sink is `FO3DQueuedSenderAudioSink`.
     - Receiver: still `Poll`-driven (NNG's threads do the I/O); messages go to `FO3DUnifiedReceiveDemux`. Consumer held strongly, released in `Stop`. Rejects count in `ReceiveErrors`.
     - Config: the NNG Uri shape is unchanged, but hosts, ports and `nng.qmax` are parsed strictly with `O3DTransportOptions`; the configure functions read only the config. **`Open3DTransportNNG` no longer depends on Open3DSender/Open3DReceiver.**
     - `O3D_TRANSPORT_API_VERSION` stays **4**.
     - Tests: `Open3DBroadcast.Transport.NNG.QueueRefusesNewestUnderBackpressure`, `.AudioIndependentOfFrameQueue`, `.StopWhileSending` (1,000 cycles, every 50th connected); `NngTesting.h` gained `SenderSetWorkerPaused`.
   - **MoQ done (WP-A1 PR 4e).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4e)".
     - Sender: frames, audio and control are items on one `FO3DSendQueue` (`RefuseNewest`, `queue_bytes` for frames, audio 1 MiB, control 1,024) that an `FO3DTransportWorker` publishes per track; items whose publisher is not ready are dropped at the worker. The sink is `FO3DQueuedSenderAudioSink` (`AudioPayload`). Reconnect stays MoQ's own (game thread, jitter only downward, pinned by tests); `FO3DReconnectPolicy` is not used.
     - Receiver: data callbacks reach the game thread through the dispatcher, then an `FO3DSendQueue` hand-off that `Poll` drains into `FO3DUnifiedReceiveDemux`. Consumer held strongly, released in `Stop`.
     - Config: `O3DTransportOptions` for every key, strict numbers. **`Open3DTransportMoQ` no longer depends on Open3DReceiver**; it keeps Open3DSender only for `UO3DSenderComponent::SubjectName` (the default stream id) until step 5.
     - `O3D_TRANSPORT_API_VERSION` stays **4**.
     - Tests: `Open3DBroadcast.Transport.MoQ.QueueRefusesNewestUnderBackpressure`, `.AudioIndependentOfFrameQueue`, `.StopWhileSending` (1,000 cycles), all on the fake moq-ffi; `MoQTesting.h` gained `SenderSetWorkerPaused`.
   - **WebRTC add-on done (WP-A1 PR 4f); step 4 is complete.** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 4f)".
     - Receiver: frames and control go from LiveKit's data callback to `Poll` through two `FO3DSendQueue` hand-offs (frames `RefuseNewest` 16 MiB, newly bounded; control 1,024), and `Poll` delivers them through `FO3DUnifiedReceiveDemux`. Audio still goes straight from LiveKit's audio callback to the sink (LiveKit decodes Opus).
     - Sender: unchanged and synchronous. LiveKit's data channel buffers and refuses, its refusal is the backpressure, and the add-on's tests pin synchronous results. The gated PCM16 audio sink stays (LiveKit encodes Opus). LiveKit reconnects by itself; `FO3DReconnectPolicy` is not used.
     - Config: `O3DTransportOptions` (strict booleans and numbers); the configure functions read only the config, so no runtime file includes an Open3DSender or Open3DReceiver header.
     - `O3D_TRANSPORT_API_VERSION` stays **4**.
     - Tests: `Open3DBroadcast.Transport.WebRTC.SharedBlocks.ReceiverQueuePolicy`, `.ReceiverAudioIndependentOfFrameQueue`, `.SenderAudioIndependentOfDataChannel`, `.SenderStopWhileSending`, `.ReceiverStopWhileReceiving` (200 cycles each), on a fake LiveKit.
   - **Leftovers from step 4:**
     - MoQ keeps its own backoff (jitter only downward, pinned by its tests); TCP and UDP use `FO3DReconnectPolicy`.
     - The WebRTC sender has no send queue (see above); revisit if `lk_send_data_ex` turns out to block.
     - `UdpSenderSetWorkerPaused` does not wait for the worker to see the flag (pitfall 15).
     - (Closed by PR 5a: MoQ's Open3DSender dependency and the add-on's test-only Open3DSender/Open3DReceiver dependencies.)
   - **Step 5 is split:** 5a typed config (done), 5b consumer API (done; `SubmitFrame` forms, delete `Send(SubjectList)`), 5c (done) the rest of item 8 (TRB-27 `FName Transport`/`EO3DTransportRole Role`, Secret entries with their env var, schema `Float`/`bRestartOnChange`/`Validate`). Why: see the ADR 0007 addendum "implementation notes (WP-A1 PR 5a)".
   - **Typed config done (WP-A1 PR 5a).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 5a)".
     - `FO3DTransportOptionsView` (non-owning; schema defaults, `VisibleWhen`); the `O3DTransportOptions` getters take it (a map converts).
     - The configure functions are `void(const FO3DTransportOptionsView&, FO3DTransportConfig&)` for both roles. The deprecated customizations adapt and still get the component or the settings.
     - `FO3DTransportConfig` lost `Token`, `bPersistToken`, `bUseAutoTokenFetch`, `TokenEndpointUrl`, `TokenRefreshLeadTimeSec`, `Backend`, and gained `SubjectName`, `OptionSchema` and `GetOptions()`. `AdvancedParams` kept its name.
     - WebRTC reads its token settings from the options and `Secrets` (`WebRTCUtils::ReadTokenSettings`).
     - MoQ takes `Config.SubjectName` and no longer depends on Open3DSender.
     - The add-on depends only on Open3DShared (the lifetime test helpers moved there as `Testing/O3DTransportLifetimeTestUtils.h`).
     - SND-35: switching transports keeps each transport's options in `InactiveTransportOptions` (sender component, receiver settings) instead of clearing. Keys are not renamed (TCP, UDP and NNG share `host`/`port`), secrets are never kept, and connection strings leave them out.
     - Saved data needed no migration: the config was never saved, and the `webrtc.*` keys already were the saved form.
     - `O3D_TRANSPORT_API_VERSION` is **5**.
     - Tests: `Open3DBroadcast.Shared.TransportOptionsView.*`, `.TransportOptions.SwitchKeepsOtherTransportsOptions`, `.TransportApiVersion.TypedConfigIsVersion5`, `Open3DBroadcast.Sender.TypedConfig.*`, `.Sender.TransportSwitch.*`, `Open3DBroadcast.Receiver.TypedConfig.*`, `.Receiver.TransportSwitch.*`, `Open3DBroadcast.Transport.WebRTC.TypedConfig.SavedOptionsReachTransport`.
   - **Consumer API done (WP-A1 PR 5b).** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 5b)".
     - `ISerializedFrameConsumer::SubmitFrame(const FString&, TConstArrayView<uint8>, double)` (view, valid for the call) and `SubmitFrameOwned(..., TArray<uint8>&&, ...)` (owned; defaults to the view form). The subject stays a case-sensitive `FString`.
     - The demux delivers mocap as a view (its scratch copy is gone); Loopback, MoQ and WebRTC hand over their queue items' buffers with `DeliverMocapOwned`. The LiveLink source implements both forms and moves an owned buffer across the game-thread hop.
     - `IOpen3DSender::Send(SubjectList)` is deleted, with every implementation and the sender component's dead `OnSubjectListReady` handler. Tests use `O3DTests::SendSubjectList` / `WebRTCSubjectListTest::SendSubjectList`.
     - `O3D_TRANSPORT_API_VERSION` stays **5**: 5a and 5b ship in the same release (no tag contains 5a).
     - Tests: `Open3DBroadcast.Shared.FrameConsumer.*`, `Open3DBroadcast.Transport.Loopback.FrameReachesConsumerWithoutCopy`.
   - **Rest of item 8 done (WP-A1 PR 5c); step 5 is complete.** Details in the ADR 0007 addendum "implementation notes (WP-A1 PR 5c)" and the ADR 0004 addendum.
     - TRB-27: `FO3DTransportConfig::Transport` is the registered `FName`, `Role` an `EO3DTransportRole`; constructor `FO3DTransportConfig(Name, Role)`. The hosts and every configure function set both; tests use "TCP"/"UDP"/"NNG"/"Loopback", not "sockets.tcp" or lower case. NNG's socket side stays in the `nng.role` option.
     - Secrets: a `Secret` schema entry declares its key and carries `SecretEnvVar` (the old lists were kept as deprecated inputs in 5c and removed in step 6). WebRTC declares its secrets in the schema only. ADR 0004 behaviour unchanged.
     - Schema: `Float` (appended), `Min`/`Max` now `double` (Int and Float; `GetDouble` clamps a Float), `bRestartOnChange` (panel calls `IO3DOptionTarget::RestartTransport`; the sender target restarts a capturing component in a game world), `Validate` (`TFunction<bool(const FString&, FText&)>`; `O3DTransportOptions::ValidateOptions` at the sender controller's and receiver source's start gives `InvalidConfig`; the panel shows the error and refuses the write). No built-in field uses the three yet.
     - `UO3DSenderComponent::GetLastTransportResult()` (C++); `FO3DSenderSerializer::OnSubjectListReady` deleted.
     - `O3D_TRANSPORT_API_VERSION` stays **5**: still no tag after v0.9.6, so 5a–5c are one release.
     - Tests: `Open3DBroadcast.Shared.TypedConfig.ConfigCarriesTransportAndRole`, `Open3DBroadcast.Shared.OptionSchema.*`, `Open3DBroadcast.Editor.OptionsPanel.RestartOnChangeRestartsTransport`, `.ValidateShowsErrorAndRefuses`, `.FloatIsClampedToRange`, `Open3DBroadcast.Transport.WebRTC.Secrets.SchemaEntriesCarryEnvVars`.
5. **Typed config and consumer API** (SHR-36, TRB-27, SHR-16, TRF-38): removes the LiveKit string fields from `FO3DTransportConfig`, deletes `Send(SubjectList)`. Split into 5a (typed config), 5b (consumer API) and 5c (rest of item 8). **Done**, interface version 5.
6. **Shims removed (WP-A1 step 6). Done; WP-A1 is complete.** Details in the ADR 0007 addendum "implementation notes (WP-A1 step 6)" and the CHANGELOG's "Removed" list.
   - Maintainer decision: removed now instead of one release later (ADR 0007 item 9 and open question 5), because no release carried the shims and no third-party add-on or project builds from this codebase.
   - `O3D_TRANSPORT_API_VERSION` stays **5**: still no tag after v0.9.6, and no tag contains 5a's merge (c98c92c), so 5a, 5b, 5c and step 6 are one version.
   - Gone: `O3DTransport::RegisterSender`/`RegisterReceiver` and the rest of the old registries; the sender and receiver transport customizations (`RegisterTransportCustomization`, `FindTransportCustomization`, the list, secret and schema forwarders); the `FScopedConfiguring*` scopes and customization caches; `FO3DTransportRegistry::EditLegacyDescriptor`; `FO3DTransportRoleOptions::SecretOptionKeys`/`SecretEnvVars` (a `Secret` schema entry is the only declaration); `SupportsAudio()`/`SupportsControl()` (use `GetCapabilities()`); and the forwarding headers (`O3DSenderInterface.h`, `O3DReceiverInterface.h`, `O3DSenderAudioSinkBase.h` and `Testing/O3DLifetimeTestUtils.h` in Open3DSender/Open3DReceiver; `O3DTransportTypes.h` and `SerializedFrameConsumerRegistry.h` at the Open3DShared root).
   - `O3DReceiverTransportCustomization.h` keeps its name for the receiver's secret and switching helpers.
   - Tests that tested only removed API were deleted (`TransportRegistry.DeprecatedFunctionsForward`, `Sender.TypedConfig.DeprecatedConfigureGetsComponent`, `Receiver.TypedConfig.DeprecatedConfigureGetsSettings`); tests that used the shims as setup now register a descriptor with a fake factory.
   - **Leftovers after WP-A1:**
     - MoQ keeps its own reconnect backoff (jitter only downward, pinned by its tests); TCP and UDP use `FO3DReconnectPolicy`.
     - The WebRTC sender has no send queue and stays synchronous; revisit if `lk_send_data_ex` turns out to block. Since WP-A2c it is called from the pose pipeline's worker. Its `SendSerialized` reads `ClientHandle` outside the lifetime gate `SendControl` uses, so it is not safe against a concurrent `Stop`; the sender controller detaches it from the pipeline (waiting for a send in flight) before `Stop`, so the pipeline cannot race, but the gate belongs in `SendSerialized` (add-on change).
     - `UdpSenderSetWorkerPaused` does not wait for the worker to see the flag (pitfall 15).
     - Schema features no built-in transport uses yet: `Validate` for host/port fields (`ParseHostPort`), `Float` for `webrtc.reconnect_timeout`, `bRestartOnChange` where a running transport ignores a change.
     - `docs/review/2026-09-plugin-review/*` still names the removed APIs; they are dated review findings with file:line evidence and are left as written.

**WP-A2 (async sender, ADR 0008) is in progress; see §2a.**

WP-A1 acceptance (roadmap): conformance suite green after each migration, net transport LOC goes down, no transport keeps its own queue/demux/sink. ADR 0007 "Verification / acceptance" lists the extra test cases.

After WP-A1, the M3 order in the roadmap is WP-A2 (async sender, ADR 0008) → WP-A3 (god classes); WP-A4 (protocol, ADR 0009), WP-A5, WP-A6, WP-A7 can go in parallel where files don't overlap.

## 2a. The work in flight: WP-A2 (asynchronous sender pipeline)

Design: `docs/adr/0008-sender-pipeline-threading.md`, "Implementation outline" items 2 to 7. One PR per sub-step (A2a to A2e); each must leave the WP-S3 sender tests passing unchanged.

1. **WP-A2a: settings snapshot, serializer without component, frame pool, sampling clock. Done (#316).** Details and deviations in the ADR 0008 addendum "implementation notes (WP-A2a)".
   - `FO3DSenderEncodingSettings` holds the encoding and the curve filtering settings (pattern lists as shared immutable arrays); the component refreshes it per sampled frame (`UpdateEncodingSnapshot`) and copies a list only when it changed. Frames carry it by value (deviation 1: the WP-S3 tests use value semantics).
   - `FO3DSenderSerializer` has no `Attach`/`Detach` and no component pointer; the component calls `SerializePoseFrame` after filtering. `SetStatsLabel` names it for `o3ds.Sender.DumpStats`.
   - Curve capture (`FO3DSenderCurveProcessor`, raw values against a shared `FO3DSCurveList`) and filtering (`FO3DSenderCurveFilter`, on the frame) are split. Filtering still runs before `OnPoseFrameReady` (deviation 3).
   - `FO3DSPoseFramePool` (public header): bounded (default 4), reuses allocations, lock only around pointer moves.
   - The wire time is `FO3DSPoseFrame::CaptureTimeSec` (sampling time), not the serialization time.
   - Still synchronous on the game thread. `O3D_TRANSPORT_API_VERSION` stays **5** (no transport interface change).
   - Tests: `Open3DBroadcast.Sender.EncodingSnapshot.SerializerWorksWithoutComponent`, `.FramePool.ReusesFramesAndIsBounded`, `.CurveFilter.AfterSamplingMatchesBefore`, `.Wire.SerializedTimeIsSamplingTime`.
2. **WP-A2b: tick group and prerequisite. Done (#317).** Details and deviations in the ADR 0008 addendum "implementation notes (WP-A2b)".
   - The sender ticks in `TG_PostUpdateWork` (was the default `TG_PrePhysics`). `BindToTarget`/`UnbindFromTarget` add and remove the target mesh's tick as a prerequisite through `SetTickPrerequisiteMesh` (one mesh at a time, no double add). `CanCaptureThisFrame` moves the prerequisite when `TargetMesh` is written during capture and removes it when the mesh is destroyed (deviation 1); an entry left by a mesh that was already collected is pruned (deviation 2).
   - The transport `Tick` and `TickControl` moved to `TG_PostUpdateWork` with the rest of the tick.
   - `RegisterOnBoneTransformsFinalizedDelegate` is not used; the old "removed in 5.4" comments are gone. Open question 2 is answered only from knowledge of the engine's tick design (addendum); the UE 5.7 source and the Epic API pages were unreachable (pitfall 25, now also the docs sites), so every tick API used is unverified and first compiled by CI.
   - **ADR 0008's root-bone acceptance test is not written:** it needs a skeletal mesh asset and the plugin has none (the CI host project holds only the packaged plugin). `SamplesAfterTargetMeshEachFrame` checks the ordering it rests on in a ticking game world instead (deviation 3). Adding a small skeletal mesh asset to a test content folder would allow the pose-equality test.
   - Still synchronous. `O3D_TRANSPORT_API_VERSION` stays **5**.
   - Tests: `Open3DBroadcast.Sender.TickOrder.TickGroupIsPostUpdateWork`, `.PrerequisiteFollowsTargetMesh`, `.SamplesAfterTargetMeshEachFrame` (the first test in the repo that creates and ticks a `UWorld`; its first CI runs failed until the test advanced `GFrameCounter` before each world tick, pitfall 28).
3. **WP-A2c: the pose pipeline. Done (#318).** Details and deviations in the ADR 0008 addendum "implementation notes (WP-A2c)".
   - `FO3DSenderPipeline` (`Open3DSender/Private/O3DSenderPipeline.h/.cpp`), one per component, shared-owned so a task in flight keeps it alive: frame pool, curve filter, serializer, one FIFO of frames and control items (`Start`, `Stop`, `RemoveSubject`), and the transport sender. The game thread samples and hands the frame over; a `UE::Tasks::Launch` task (`BackgroundHigh`) filters, serializes and calls `SendSerialized` with the serializer's buffer moved in. The ADR's "drain task scheduled" flag allows one task per sender at a time; `UE::Tasks::FPipe` is not used (deviation 1: pipe lifetime when the task holds the last reference).
   - Depth `o3d.Sender.PipelineDepth` (default 2, 1 to 8), drop oldest before serialization; control items never dropped. `o3d.Sender.AsyncPipeline 0` (read at `StartCapture`) keeps the WP-A2b synchronous order, for one release.
   - `StopCapture` discards waiting frames and does not wait for the network; removing the `OnSerializedFrame` listener and detaching the sender (the transport controller's `Stop`, before `IOpen3DSender::Stop`) wait for the one item the worker is processing (deviation 3). The component destructor detaches both. `ShutdownModule` waits up to 1 s for drain tasks.
   - `OnSerializedFrame` fires on the worker; `OnPoseFrameReady` fires on the game thread with raw curves (filtering moved to the worker). A full sync refused with `DroppedBackpressure` is followed by a full sync (`FO3DSenderSerializer::RequestFullSync`); `bFullSync` is set on the payload.
   - `o3ds.Sender.DumpStats` reads every serializer under its new `StateLock` (A2a deviation 6 closed). New `UO3DSenderComponent::GetPipelineStats()`, `o3d.Sender.DumpPipelineStats` and trace scopes `O3D.Sender.Sample`, `O3D.Sender.Pipeline.Serialize`, `O3D.Sender.Pipeline.Send`.
   - **Not done here:** `tx_seq` on the wire (done since in ADR 0005 PR A: the serializer stamps through one `O3DS::StreamWriter` per subject; `LastSendSequence` still counts every subject's payloads together), the new-peer flag (WP-A4b), and **the Unreal Insights numbers** ADR 0008 Verification asks for: no engine in the cloud session. The manual capture (what to record, which timers and stats, the budgets) is in the addendum; record the numbers in the A2c PR or a follow-up.
   - Every new engine API (`UE::Tasks::Launch`, `ETaskPriority::BackgroundHigh`, `TRACE_CPUPROFILER_EVENT_SCOPE_STR`, and in tests `CollectGarbage`, `IConsoleVariable::Set`) is unverified against 5.7 (pitfall 25) and first compiled by CI.
   - Still `O3D_TRANSPORT_API_VERSION` **5**: the interface did not change; the worker relies on the documented "any thread" contract of `SendSerialized`.
   - Tests: `Open3DBroadcast.Sender.Pipeline.SlowTransportDropsOldest`, `.RefusedFullSyncIsSentAgain`, `.StopDiscardsQueuedFrames`, `.StopStartAndRenameUnderLoad` and `.QuantizationChangeForcesOneFullSync` (both with the console variable at 0 and 1), `.OwnerReleasedWithTaskInFlight` (1,000 cycles), `.ComponentDestroyedWithTaskInFlight` (200 components, garbage-collected every 50). New hooks: `FO3DSenderPipelineProbe`, `FO3DSenderComponentTestAccess::SubmitSampledFrame`/`WaitForPipelineIdle`.
4. **WP-A2d: audio clock and cached device enumeration. Done (#319).** Details and deviations in the ADR 0008 addendum "implementation notes (WP-A2d)".
   - `FO3DAudioClockMapper` (public, header-only): the submix tap maps `AudioClock` and the microphone `StreamTimeSec` onto `FPlatformTime::Seconds()`; offset from the first buffer, low-pass (5 s), reset on a jump over 100 ms (at once when the source goes backwards or ahead; after 0.5 s when it falls behind, so a hitch does not move the stamps). `PushFrames` is not mapped: its callers pass sender-clock times.
   - `FO3DAudioInputDevices` (public, exported): one cached device list, enumerated once per Input `StartCapture`, once in the editor after engine init, and on request (`UO3DSenderComponent::RefreshAudioInputDevices`, `o3d.Sender.Audio.RefreshDevices`). Pickers and name lookups read the cache.
   - The device is opened once per start: `InitializeTransport` no longer calls `UpdateAudioCaptureBinding` (it ran twice per start, reopening the microphone), and the capture component's `BeginPlay` opens the device only with a sink bound.
   - `O3D_TRANSPORT_API_VERSION` stays **5**. Every engine API used was checked against the local UE 5.7 headers (addendum).
   - Tests: `Open3DBroadcast.Sender.AudioClock.*` (4), `Open3DBroadcast.Sender.AudioDevices.LookupsReadTheCache`, `.StartEnumeratesAndOpensOnce`, `.SinkBindOpensAtMostOnce`; new hook `FO3DSenderAudioCaptureTestAccess`.
5. **WP-A2e: core CRC and builder reuse. Done (#320).** Details and numbers in the ADR 0008 addendum "implementation notes (WP-A2e)".
   - `O3DS::Crc32` (`src/o3ds/crc32.h/.cpp`): slicing-by-8 CRC-32 with CRCpp's `CRC_32()` parameters, so the value and the wire format are unchanged. `finalize()` and `Parse()` use it; the core no longer includes `CRC.h`, so the plugin mirror drops it and the CRC++ licence (`sync_o3ds_core.py` copies that licence only while a `crccpp/` file is mirrored), and `Open3DStreamCore.Build.cs` drops the include path.
   - The six whole-buffer `Serialize*` functions reuse a `thread_local` `FlatBufferBuilder`; `finalize()` writes in place.
   - MSVC Release, one 250-bone, 250-curve subject: Serialize 499 to 96 µs, Parse 406 to 162 µs, Serialize + Parse 3.5x (the acceptance asks for 3x).
   - Tests: `core.crc32_tests`; benchmark `o3ds_core_bench` (`core.bench.serialize`, label `bench`).
   - Follow-up done separately (#321): `Parse` reuses subjects, transforms and curve names on a full sync with exactly the result of a fresh parse (`core.parse_reuse_tests`); Parse 160 to about 85 µs. Still open: skipping the second Verifier pass after `PeekMeta`.
6. **Next: remove `o3d.Sender.AsyncPipeline` one release after WP-A2c ships** (deletes `FO3DSenderPipeline::FilterFrameInline` and the synchronous branch of `Push`).

## 2b. Control channel (WP-CTL, ADR 0011)

Done outside the cloud session, after the first version of this doc. Design: `docs/adr/0011-control-channel.md` ("Implementation outline" and "Verification / acceptance"). A one-way sender-to-receivers stream of **values** (keyed, last-writer-wins, periodic snapshots) and **events** (fire-once, de-duplicated, TTL, redundant copies), as envelope kind `Control = 2` with its own FlatBuffers root (`src/o3ds_control.fbs`, `"O3DC"`).

| Step | Status |
|---|---|
| CTL-1 core: schema, codec, publisher/receiver state machines, CTest + fuzz | Done (#291) |
| CTL-2 envelope kind, `SendControl`/`SetControlSink` on the interfaces, `FO3DControlBus`; API version 1 → 2 | Done (#292) |
| CTL-3 TCP, UDP, NNG, Loopback | Done (#293) |
| CTL-4 sender component API, receiver `FControlSink` with mocap alignment, `UO3DControlSettings`, `UO3DRemoteControlComponent` | Done (#294) |
| CTL-5 MoQ (`control/<session>` track) | Done (#295). The live-relay test case is not written (needs a relay) |
| CTL-6 WebRTC add-on: `__o3d.ctl` send path and receive classification, tests, manual test step | Done (CTL-6). Tests `Open3DBroadcast.Transport.WebRTC.Control.*` (fake FFI); manual case 12 in `docs/testing/webrtc-manual-test.md` not yet run against a live LiveKit server |
| CTL-7 docs: USER_GUIDE Control section, transport comparison row, CHANGELOG protocol entry | Done (CTL-7). The wire layout goes into `docs/wire-format.md` with WP-D3 |

Open items:
- ADR 0011 was accepted on 2026-10-01 (open questions with their defaults), and WP-CTL is in the roadmap as decision D11 with its own work package section.
- The WebRTC transport supports control since CTL-6. Its conformance profile is still deferred (WP-T2e: no add-on test module), so the control conformance cases are mirrored in the add-on's `Open3DBroadcast.Transport.WebRTC.Control.*` tests; when WP-T2e adds the profile, give it the control cases too. ADR 0011 open question 4 (is the reliable LiveKit data channel ordered; does the callback expose the participant) is still unverified; the manual case 12 checks ordering, and `source_id` in the payload covers sender identity.
- The packaged Shipping test ADR 0011 asks of CTL-4 (control enabled in a Shipping build through the project setting and the runtime call) is not in CI yet; the nightly Shipping build (`Build-ShippingGame.ps1`) is the natural place for it.
- New-peer snapshots wait for ADR 0005 (vi); until then recovery is bounded by the snapshot interval.
- CTL and WP-A1 overlap: see the notes on steps 3 and 4 in §2.

## 3. Repository map (what changed during M1/M2)

- Main plugin: `ProjectSandbox/Plugins/Open3DBroadcast`. Modules: `Open3DShared`, `Open3DSender`, `Open3DReceiver`, the transports (`Open3DTransportSockets`, NNG, MoQ, Loopback), `Open3DStreamCore`, `Open3DBroadcastEditor` (editor-only UI, ADR 0010), `Open3DBroadcastTests` (editor-only, ADR 0006).
- WebRTC add-on: `ProjectSandbox/Plugins/Open3DBroadcastWebRTC` (WP-F11, ADR 0002). Depends on Open3DBroadcast; checks the transport API version at startup. Publishing it waits on counsel question L1; the release workflow attaches it only when the repo variable `O3D_PUBLISH_WEBRTC_ADDON` is `true`.
- The o3ds core (`src/o3ds`) is compiled inside the plugin from a generated copy in `Source/ThirdParty/Open3DStreamCore` (ADR 0003). After touching `src/o3ds`, `src/o3ds_generated.h` or the flatbuffers/crccpp pins: run `python3 Build/Scripts/sync_o3ds_core.py` and commit the result. Never edit the copy by hand.
- Control channel: core in `src/o3ds/control.{h,cpp}` and `src/o3ds_control.fbs` (the second generated header is in the core manifest, so `sync_o3ds_core.py` mirrors it); UE types in `Open3DShared/Public/O3DControl*.h`; receiver-side `O3DControlSettings.h` and `O3DRemoteControlComponent.h` in `Open3DReceiver/Public`.
- Rider IDE settings are committed under `.idea/`.
- Rules for agents: `AGENTS.md` → `.github/copilot-instructions.md` (authoritative). `Build/README.md` documents every build script.

## 4. How to work in this repo

### Conventions
- Branch from `develop`; one WP (or one WP-A1 step) per PR; squash-merge. Before merging a later PR, merge `develop` into it and let CI re-run.
- Commit/PR titles start with the WP id and list finding ids, e.g. `WP-F7: editor module split (ADR 0010; FAB-7, SND-34)`.
- New source files start with `// Copyright <current year> Lifelike & Believable. All Rights Reserved.` and a blank line. Open3DStream-derived files carry `// Portions Copyright (c) Open3DStream Contributors` as the second line (#382). The full conventions for agents are in `AGENTS.md`.
- Every test name starts with `Open3DBroadcast.` so the CI filter finds it. Network tests register only with `O3DB_NETWORK_TESTS=1`.
- Update `CHANGELOG.md` and the relevant docs in the same PR.

### Local checks (run before every push)
```
python3 Build/Scripts/fab-package.py --out-dir <scratch>     # Fab tree/package checks
python3 Build/Scripts/check-copyright-headers.py
python3 Build/Scripts/check-runtime-editor-deps.py
bash    Build/Scripts/check-no-video-codecs.sh                # main plugin must pass; --addon fails by design
python3 Build/Scripts/sync_o3ds_core.py --check               # needs submodules initialised
```
With UE 5.7 installed locally you can also run the real build and tests: `Build/Scripts/Build-Plugin.ps1`, `Build-WebRTCAddOn.ps1`, `Run-AutomationTests.ps1 -TestFilter Open3DBroadcast` (usage in `Build/README.md`). The cloud session had no UE, so every C++ change was first compiled by CI; a local build before pushing will save cycles.

### CI
- `open3dbroadcast-plugin-ci.yml` (PRs): GitHub-hosted jobs (path filter, Fab source zip, copyright headers, runtime-editor deps) plus **one self-hosted Windows UE job**:
  1. BuildPlugin;
  2. automation tests with the main plugin only;
  3. build the WebRTC add-on against that package;
  4. automation tests with both plugins;
  5. a strict build (non-unity, no PCH, warnings as errors);
  6. BuildPlugin on the Fab zip.
- **Draft PRs skip the UE job**; mark the PR ready to get it.
- `core-tests.yml`: Linux ASan/UBSan, MSVC, libFuzzer, warning ratchet (baseline 8), core mirror sync check.
- `repeater-image-test.yml` (PRs touching the Repeater image's files): builds `docker/Dockerfile.repeater`, pushes through the relay with `nngcat`, checks `docker stop` exits 0; never pushes to a registry. `repeater-image.yml` (publish to GHCR) is disabled.
- PR CI's UE job runs twice (ADR 0014): the full job on UE 5.7 and a reduced one on UE 5.8 (no strict build, no Fab zip build); 5.8 artifacts end in `-UE5.8`.
- Nightly (`open3dbroadcast-plugin-nightly.yml`): once per engine, UE 5.7 and 5.8; flag-combination builds, Linux exclusion check and Shipping game build on 5.7 only. PR CI builds with every transport on, so only the nightly's flag builds catch code that needs an `O3D_WITH_TRANSPORT_*` guard (#453).
- Release (`open3dbroadcast-plugin-release.yml`): both engines in full; a dry run (Actions > Run workflow, with a version) exercises everything but Publish. To check Publish, download the dry run's packages and run `release-version.py archives` on them.
- When a test fails, `Run-AutomationTests.ps1` prints the last 150 lines of `Automation.log` into the job log.
- Known runner infrastructure failures (re-run once, only if the job died before any test ran): Windows Application Control blocking a UBT rules DLL (`0x800711C7`), and an occasional editor exit at startup with no report. A second failure is real.

### UE 5.7 compile pitfalls hit in this repo
1. Friend declarations must be unconditional (not inside `#if WITH_...`).
2. `*_API` classes with non-copyable members need `= delete` copy operations.
3. No `MakeShared` with a forward-declared type.
4. `NewObject<T>(GetTransientPackage())` needs `UObject/Package.h`.
5. Include `Widgets/Input/SComboBox.h`; never forward-declare `SComboBox<...>`.
6. Include what you use: the strict build has no PCH and no unity.
7. Anything tests or another module/plugin uses must be `*_API`-exported in a Public header.
8. `TStrongObjectPtr<T>` members need the complete type in the header.
9. `TSlateDelegates<int32>::FOnValueCommitted` does not exist in 5.7; use the SLATE_EVENT shorthand `.OnValueCommitted(this, &Method, Payload)`.
10. Windows PowerShell 5.1 ignores `-Include` together with `-LiteralPath`; filter with `Where-Object`.
11. Helpers in anonymous namespaces need names unique within their module: unity builds merge a module's .cpp files, and CI's strict build (non-unity) will not catch a collision. A local `ProjectSandboxEditor` build does (see #287, `GetPipeContextRegistry` in NNG).
12. Never name a free function, local or helper after a UE global namespace (`Audio`, `UE`, `Chaos`, ...). MSVC reports C2872 "ambiguous symbol" when that namespace is visible in the module; `O3DSendQueueTests.cpp` had helpers called `Audio()` (#305; renamed `MocapItem`/`AudioItem`/`ControlItem`).
13. The copyright line must be followed by a blank line; a bare `//` continuation line right after it fails `Build/Scripts/check-copyright-headers.py`. Run the script before pushing.
14. A test that needs a transport's queue to back up must not rely on the network being slow: on loopback a worker drains faster than a test can fill. Pause the worker with a test hook instead (`O3DSocketsTesting::UdpSenderSetWorkerPaused`, WP-A1 PR 4c), so the drop policy is deterministic.
15. A test pause hook must return only once the worker has seen the flag: an iteration that started before the flag was set can still dequeue the next item. `FO3DNngSender::SetWorkerPausedForTesting` waits for a paused iteration (WP-A1 PR 4d); `UdpSenderSetWorkerPaused` does not yet, so a UDP test should leave the queue empty for one worker wait before relying on it.
16. Never build a counter that reserves first and rolls back on overflow when another thread can read it: a reader sees the over-limit value for a moment. `FO3DSendQueue::Enqueue` did that and `Open3DBroadcast.Shared.SendQueue.ConcurrentAccounting` failed intermittently on #308; it now reserves with a compare-exchange that only succeeds when the result fits (af7cfcc). The same applies to any limit a test or `GetStats` observes.
17. A transport's existing tests may pin its own timing (MoQ's backoff jitter, connect timeout, subscribe retry on a manual clock). Before replacing such logic with a shared block, check that the shared block reproduces those exact values; if not, keep the transport's logic and say why, rather than editing the tests (WP-A1 PR 4e kept MoQ's backoff).
18. A fake FFI used for a Stop test must keep the real library's callback contract. LiveKit promises no callback after the call that clears it (or `lk_client_destroy`) returns, so the WebRTC fakes hold a lock around each callback and take it in the setters (`WebRTCSharedBlocksTests.cpp`, WP-A1 PR 4f). A fake that calls back without it reports races the real library cannot produce, and can hide the ones it can.
19. Before planning a saved-data migration, check what is actually serialized. `FO3DTransportConfig` is a plain struct built per start, so removing its LiveKit fields lost nothing; the saved form was the `webrtc.*` keys of a UPROPERTY option map (WP-A1 PR 5a). A new UPROPERTY loads with its default from older assets, ini files and `ImportText` strings, so adding one needs no migration either.
20. When an interface gains a second form of a virtual, give it its own name (`SubmitFrameOwned`, not a second `SubmitFrame` overload). A derived class that overrides one overload hides the others, so a call through the derived type silently picks the wrong one or fails to compile (WP-A1 PR 5b).
21. A local must not reuse the name of a parameter or of a local in an enclosing scope. MSVC reports C4457 ("declaration hides function parameter") or C4456, and CI builds with `-FailOnWarnings`, so it fails the build. The 5c options panel declared `double Value` inside a function whose parameter was `Value` (fixed in c5fb457). Check every new local against the enclosing function's parameters.
22. A sender-component test that expects its transport to start must set `bAutoCreateTransport = true`: it defaults to false, and `StartCapture` then never reaches the transport controller. It must also declare every warning the path logs with `AddExpectedError` (log warnings fail a test): a control-only `StartCapture` without a mesh logs "No TargetMesh set" once per call. `ValidateGivesInvalidConfig` needed both (fixed in 9bb6b3f); `O3DTransportLifetimeTests` shows the pattern.
23. Mixed line endings in the sources (WP-A2a): resolved by `.gitattributes` (WP-R2, BC-11), which stores every text file with LF.
24. `FO3DSenderCurveConfig` holds raw pointers to the include/exclude pattern arrays. Build it only from a frame's settings snapshot (`FO3DSenderEncodingSettings`, whose lists are shared and immutable), as `FO3DSenderCurveFilter::FilterFrame` does, never from the component's `IncludeCurvePatterns`/`ExcludeCurvePatterns`: once filtering runs on the WP-A2c worker those would be read while the game thread edits them (WP-A2a).
25. The GitHub connector in a cloud session may refuse the UE 5.7 source mirror (`lifelike-and-believable/UnrealEngine`). If it does, use only UE APIs this plugin already compiles in CI, in the same call form, and say so in the ADR addendum; a new engine API then needs a session that can reach the mirror (WP-A2a).
26. A test that needs real engine ticking creates its own world: `UWorld::CreateWorld(EWorldType::Game, false)` plus a world context, `InitializeActorsForPlay(FURL())`, then `World->Tick(LEVELTICK_All, Dt)`. Such a world has no game mode, so actors never begin play by themselves and component ticks are not registered: call `Actor->DispatchBeginPlay()` after registering the components. Destroy the world in a scope guard (`DestroyWorldContext`, `DestroyWorld(false)`) so a failed check does not leak it. `O3DSenderTickOrderTests.cpp` (WP-A2b) shows the pattern; it was written without a local UE build, so check its first CI run.
27. A sender test with a skeletal mesh component but no mesh asset samples frames without a skeleton descriptor, and the serializer drops them with a Warning ("no skeleton descriptor on the frame", rate-limited to one per 5 s). Declare it with `AddExpectedError(..., -1)`: `0` means "at least once" (`AutomationTest.h`), and since WP-A2c the drop happens on the pipeline's worker, so a short test may end before any frame was dropped; `SamplesAfterTargetMeshEachFrame` used `0` and failed intermittently until it was changed to `-1` (WP-A2b, fixed in the WP-A2 worker-cost follow-up). The cloud session could not reach `dev.epicgames.com` or `docs.unrealengine.com` either (network proxy), so API checks fell back to search snippets.
28. A test that ticks a world with `World->Tick` must advance `GFrameCounter` before each tick (`++GFrameCounter;`). The engine loop advances it once per frame, and a tick function that already ran in the current `GFrameCounter` frame is not queued again, so without it only the first `World->Tick` runs the component ticks (found in #317; `O3DSenderTickOrderTests.cpp` shows it).
29. Since WP-A2c the sender serializes and sends on a worker. A test that checks what a sender component sent must wait for its pipeline (`FO3DSenderComponentTestAccess::WaitForPipelineIdle`, an event with a timeout) before reading the transport or the serializer stats; a test that needs the queue to back up holds the worker inside `SendSerialized` with a scripted transport and waits for an "entered" event (pitfalls 14 and 15), as `O3DSenderPipelineTests.cpp` does. Warnings the worker logs (the serializer's drop warning) arrive on the worker thread; `StopCapture` waits for the item in flight, so stop the component before the test ends if a late warning could otherwise land in the next test.
30. Do not own a `UE::Tasks::FPipe` in an object whose last reference a piped task may drop: the pipe's destructor checks it has no work, and the pipe is touched after the task body and its captures are destroyed (as far as is known; unverified for 5.7). `FO3DSenderPipeline` uses plain tasks plus a "drain scheduled" flag for that reason (ADR 0008 addendum WP-A2c, deviation 1).
31. Adding a source file to a module reshuffles its unity blobs, so a latent name collision between two other files can surface in your PR (pitfall 11). WP-A2d's two new test files put `SocketsLifetimeTests.cpp` (an anonymous-namespace `FindFreeTcpPort`) in one blob with `SocketsTcpSharedBlocksTests.cpp` (its own `FindFreeTcpPort` in a named namespace it `using`s): C2668, ambiguous call. Renamed to `FindFreeLifetimeTcpPort`. Give file-local test helpers names unique within the test module.

## 5. Waiting on the maintainer

- **WP-F0:** check the live Fab technical requirements page against ADR 0001/0002 assumptions.
- **Repeater image publishing:** enable `.github/workflows/repeater-image.yml` (disabled manually; pushes `ghcr.io/lifelike-and-believable/open3dstream-repeater` on `main`/`develop` pushes and nightly), which `compose/` and `cloud-init/` pull. The image builds and passes its relay test (#372).
- **LiveKit FFI request:** forward `docs/livekit_ffi_feature_request.md` (sections 11 to 13 are new) to the LiveKit FFI developers.
- **Branch protection on `develop`:** require the ten checks listed in `Build/README.md` ("Required checks"); leave "require branches to be up to date" off; keep admin bypass for runner outages.
- **WP-F5:** counsel questions L1–L5 (ADR 0002). L1 gates publishing the WebRTC add-on before the codec-free `livekit_ffi` rebuild.
- **Listing details:** `CreatedByURL` (still `https://open3dstream.com/` in both `.uplugin` files); real `SupportURL` and `DocsURL` (stand-ins today); `MarketplaceURL` once the Fab listing exists; the WebRTC add-on download link (a marked placeholder in the user guides). Decided 2026-10-01: `CreatedBy` is "Lifelike & Believable and Open3DStream Contributors" in both descriptors.
- **Naming:** decided 2026-10-01: editor categories (`Category = "Open3DBroadcast|..."`, the Sender details customization) and `ClassGroup = (Open3DBroadcast)` use Open3DBroadcast. Still open: the LiveLink source name (factory display name "Open3DStream Receiver" and tooltip in `O3DReceiverSourceFactory.cpp`, source type "Open3D Stream" in `O3DReceiverSource.cpp`), and whether anything saved in assets, presets or config (property names, config section names, ini keys) should ever be renamed. None of those contain "Open3DStream" today; renaming them would need redirects.
- **ADR 0002 Q8(b):** confirm on a real Fab install that the installed plugin ships the import libraries (`UnrealEditor-Open3DShared.lib` etc.) a source build of the add-on needs.
- **Red-gate branches:** five throwaway branches proving each CI gate fails when it should (unused local with `-FailOnWarnings`, failing test, include only the PCH provided, dev notes in `Private/`, WebRTC in the Fab zip). They were never pushed. The patches are in `docs/roadmap/handoff/redgate-patches/`. To use one: branch from `develop`, `git am` the patch, open a **draft** PR, mark it ready, confirm the named check goes red, then close the PR and delete the branch. They predate F1/F7/F11, so a patch may need a small path fix.

## 6. Decisions already made (don't reopen)

- Rights holder: Lifelike & Believable. Open3DStream Contributors notices stay on files that had them.
- FriendlyName "Open3DBroadcast"; the add-on is "Open3DBroadcast WebRTC". Whole plugin is Beta for v1; MoQ ships as Experimental.
- WebRTC ships only as the separate free add-on, not in the Fab package.
- v1 platform scope and the rest: see ADRs 0001–0011.
