# Core library review: Open3DStream `src/o3ds` (as used by the Open3DBroadcast UE plugin)

**Summary**
1. **Architecture.** `src/o3ds` is one static library, `open3dstreamstatic` (C++17, CMake). Its core is the FlatBuffers schema in `src/o3ds.fbs`, which `flatc` compiles at build time. Around the schema sit an OO model layer (`model.h/.cpp`: `SubjectList` → `Subject` → `TransformList` of raw `Transform*`) and newer, well-separated pure-math modules: sequencing, `ReorderGate`, `ClockOffsetEstimator`, predictors, concealment, the residual codec, channel quantization and UDP fragmentation. It also still carries a large set of legacy transports (nng/tcp/udp/websocket/webrtc connectors, xsens parser, binary_stream).
2. **How it reaches the plugin.** `Build/Scripts/Sync-O3DSCore.ps1` builds the lib (Win64, Release, WebRTC off) and copies the `.lib`, the whole `src/o3ds` tree (including .cpp files), `o3ds.fbs` and the generated header into `ThirdParty/open3dstream`. That folder is gitignored, and every plugin build depends on this script having been run.
3. **What the plugin uses.** Grepping `O3DS::` / `#include "o3ds/` in the plugin shows it only uses `model.h` (`SubjectList`/`Subject` Serialize, Parse and PeekMeta, plus residual and quantization), `udp_fragment.h`, `reorder_gate.h`, `sequencing.h` (`NowUtcMicros` only), `clock_offset.h`, `predict/concealment.h` and `predict/linear_predictor.h`. It uses none of the core connectors, `capture`, `replay`, `channel_model`, `xsens` or `binary_stream`.
4. **Generated header.** `src/o3ds_generated.h` matches fresh `flatc 2.0.6` output byte-for-byte once line endings are normalised (the checked-in file has CRLF endings). The schema's append-only field order is respected: vtable offsets are sequential and new fields are at the end.
5. **Robustness is uneven.** `Parse()` runs the FlatBuffers Verifier and null-checks optional fields, which is good. But untrusted-input bugs remain *after* verification. I confirmed two memory-safety bugs with ASan proof-of-concepts: a remote SEGV in `CalcMatrices` (CORE-1) and a heap overflow in `UdpCombiner` (CORE-2). There are also resource-exhaustion paths: fragment reassembly (CORE-3) and O(N²) hierarchy solving (CORE-8).
6. **Residual coding is lockstep and not loss-tolerant.** C2 residual coding, which the plugin can enable, relies on sender and receiver predictors staying in lockstep, and it does not survive packet loss. In my proof-of-concept, one dropped packet grows the error from 0 to 20 units in 150 frames (CORE-5), and a receiver that joins mid-stream shows about 115 units of persistent error (CORE-6).
7. **Hot path.** CRC-32 is computed bit-by-bit and takes about 80% of Serialize/Parse time (413 µs of about 500 µs for a 250-bone, 250-curve subject; a table-based CRC takes 99 µs). Parse also re-allocates every Transform on every full sync (CORE-7, CORE-18).
8. **Protocol versioning is informal.** There is no `file_identifier` and no protocol version on the wire. `O3DS_VERSION_TAG` is still `1.0.4` despite three schema extensions, `CHANGELOG.md` is empty, and old readers silently misapply residual-coded data (CORE-16).
9. **Tests.** 174 unit tests (one ctest entry), built with GCC 13 in Debug with `-fsanitize=address,undefined` in a scratch dir against pinned submodule commits: **174/174 pass**. They are happy-path and round-trip only: there are no malformed-input, fuzz, loss or reorder-under-residual tests, and no test run on MSVC/Release, which is the configuration the plugin ships. `-Wall -Wextra` produces 68 warnings.
10. **Overall.** The newer modules (reorder gate, clock offset, predictors, quantization) are well documented and mostly careful. The legacy model/fragment code is the weak spot. I recommend fixing CORE-1 through CORE-8 before relying on the UDP receiver or residual mode in production, and splitting the legacy connectors into a separate target.

The proof-of-concept harnesses are in `poc/{poc.cpp,chain.cpp,perf.cpp}` (next to this file). They were built against `open3dstreamstatic` with GCC 13 and `-fsanitize=address,undefined`. I did not modify any repository file.

---

### CORE-1: Remote crash / heap OOB read in `CalcMatrices` via `Component_Matrix` without matching matrices
- Category: security
- Severity: critical
- Location: src/o3ds/model.cpp:1209-1212, src/o3ds/model.cpp:1217-1224, src/o3ds/model.cpp:240-243
- Evidence:
  - `ParseSubject` pushes `O3DS::TMatrix` into `transformOrder` for every `Component_Matrix` entry in the wire `components` vector. It never checks that the `matrix` vector has that many entries.
  - `CalcMatrices()` (called unconditionally for every subject at the end of `Parse`, model.cpp:1058-1063) then does `m = transform->matrices[matrixId++].value * m;` with no bounds check.
  - A sender with a valid CRC (anyone can compute it) sends `components=[Matrix]` and omits `matrix`. The PoC (`poc calc`) produces UBSan "reference binding to null pointer" at model.cpp:242, then ASan SEGV in `operator*` (math.h:389).
  - This is reachable from every plugin receive path, which all call `SubjectList::Parse`.
- Recommendation:
  - In `ParseSubject`, only push `TMatrix` while `matrixCount < inMatrix->size()`, or reject the subject.
  - In `CalcMatrices`, bounds-check `matrixId < transform->matrices.size()` and fail with `mError`.
  - Add a regression test that builds this buffer.
- Effort: S
- Owner: coding
- Status: closed in #265

### CORE-2: Heap buffer overflow in `UdpCombiner::addFragment` when fragments of one message disagree on `fragSize`
- Category: security
- Severity: critical
- Location: src/o3ds/udp_fragment.cpp:113-117, src/o3ds/udp_fragment.cpp:127-140, src/o3ds/udp_fragment.cpp:156-158
- Evidence:
  - `mFound.assign(frames, false)` is sized from the *first* fragment's `bufSz/fragSize`.
  - Later fragments for the same id are only checked for `mBufferSize == bufSz`. Their own `frames` (recomputed from their own `fragSize`) and `seq < frames` are then trusted, so `mFound[seq] = true` writes past the vector.
  - PoC (`poc frag`): fragment (id 7, bufSz 2000, fragSize 1000, seq 0) followed by (id 7, bufSz 2000, fragSize 1, seq 1999). Result: ASan heap-buffer-overflow at `udp_fragment.cpp:156`.
  - Both packets pass the plugin's own pre-filter (`SocketsUdpReceiver.cpp:348-386`: FrameCount 2000 ≤ 4096, payload matches), so this is remotely reachable on the UE UDP receiver.
- Recommendation:
  - Store `fragSize`/`frames` in `UdpCombiner` on first fragment and reject any later fragment whose `fragSize` or `bufSz` differ.
  - Use `mFound.at()` or an explicit `seq < mFound.size()` check.
  - Add a unit test for mixed-fragSize input.
- Effort: S
- Owner: coding
- Status: closed in #264

### CORE-3: Unbounded UDP reassembly state enables memory exhaustion (and `UdpCombiner` is an unsafe copyable owner)
- Category: security
- Severity: high
- Location: src/o3ds/udp_fragment.cpp:110-111, src/o3ds/udp_fragment.cpp:127-131, src/o3ds/udp_fragment.cpp:173-184, src/o3ds/udp_fragment.cpp:204-226, src/o3ds/udp_fragment.h:50-72
- Evidence:
  - Each new message id creates a map entry, and its first fragment `malloc(bufSz)` allocates up to 64 MB (the plugin caps this at 50 MB).
  - There is no limit on concurrent ids and no age-based expiry. Entries are only erased when some *later-or-equal* id completes (`j->first <= i.first`).
  - Stale entries with ids above the current stream therefore live forever; this happens on sender restart or id wraparound.
  - PoC (`poc dos`): 200 single-fragment datagrams reserve 13.4 GB. On Windows, `malloc` commits the memory, so this exhausts the commit charge quickly.
  - `UdpCombiner`/`UdpFragmenter` own raw `malloc` buffers but have implicit copy constructors: a copy after allocation causes a double free.
- Recommendation:
  - Bound in-flight messages (e.g. 8) and evict the oldest.
  - Expire entries after N ms or when a newer id completes. Use serial-number (wrapping) comparison instead of `<=` on raw `uint32_t`.
  - Cap `bufSz` to a caller-configured max (default ≈ 1 MB for mocap).
  - Replace the raw buffers with `std::vector<char>` so the classes follow the rule of zero.
- Effort: M
- Owner: coding
- Status: closed in #264

### CORE-4: `UdpMapper` emits empty frames and discards in-progress messages after a rejected fragment
- Category: bug
- Severity: medium
- Location: src/o3ds/udp_fragment.cpp:164-171, src/o3ds/udp_fragment.cpp:186-202, src/o3ds/udp_fragment.cpp:212-220
- Evidence:
  - `UdpMapper::addFragment` calls `getCombiner(frameId)`, which inserts a combiner even if `UdpCombiner::addFragment` then rejects the data.
  - The return value is ignored, and the function always returns `true`.
  - An empty combiner has `mFound.empty()`, so `isComplete()` returns `true`.
  - `getFrame` then returns a 0-byte frame and erases every entry with a lower id, including legitimate partial messages.
  - PoC (`poc empty`): `addFragment(bad)=1 getFrame=1 size=0 remaining items=0`.
- Recommendation:
  - Propagate the combiner's result.
  - Only insert the entry after the first fragment is accepted.
  - Make `isComplete()` return false when `mBufferSize == 0`.
- Effort: S
- Owner: coding
- Status: closed in #264

### CORE-5: Residual coding (C2) is not loss-tolerant: one lost update drifts until the next keyframe
- Category: architecture
- Severity: high
- Location: src/o3ds/predict/residual_codec.h:87-158, src/o3ds/model.cpp:626-760, src/o3ds/model.cpp:1376-1498, ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DSender/Public/O3DSenderComponent.h:230, ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DReceiver/Private/O3DReceiverSource.cpp:610-619
- Evidence:
  - **How the design works.** The encoder `Commit()`s its "reconstructed" pose and the decoder `EndFrame()`s its own. This keeps sender and receiver in lockstep only if *every* update arrives, in order.
  - **No loss detection.**
    - `ParseUpdateResidual` has no sequence input: it passes `seq` 0 to `ToPoseSample`.
    - The plugin's residual path sends `tx_seq=0` (`SerializeUpdateResidual(Buffer, Count, DeltaThreshold, Now)`, O3DSenderSerializer.cpp:468). So the ReorderGate is bypassed.
    - The legacy receive path parses the buffer (which advances decoder history) *before* `ShouldProcessFrame` drops stale or out-of-order frames.
  - **Measured effect.** The plugin default is `ResidualKeyframeIntervalFrames = 300`. PoC (`poc residual`, Linear, one dropped packet at frame 50): translation error 1.8e-11 at frame 49, 0.18 at frame 51, 6.9 at frame 100, 20.5 at frame 200, still growing.
- Recommendation:
  - Carry `tx_seq` on every residual update, and have the decoder track the expected seq. On a gap, reorder, or out-of-order frame, drop residual updates until `is_keyframe`, and hold or conceal meanwhile.
  - Add a receiver-to-sender keyframe request, or at least a much shorter default keyframe interval.
  - Make the gate/stale decision *before* `Parse()` mutates codec state.
  - Until this is done, document residual mode as reliable-ordered-transport-only (TCP/NNG), and have the UE settings block it on UDP/WebRTC-unreliable.
- Effort: M
- Owner: design
- Status: closed in #342

### CORE-6: `ResidualDecoder` fallback applies residuals as absolute values and poisons history; residual keyframes cannot bootstrap a late joiner
- Category: bug
- Severity: high
- Location: src/o3ds/predict/residual_codec.cpp:104-124, src/o3ds/model.cpp:1401-1415, src/o3ds/model.cpp:1450-1497, src/o3ds/model.cpp:1146
- Evidence:
  - **The fallback.** When the decoder has no history (a new decoder after a full resync, or joining mid-stream), `BeginFrame` sets `mIsKeyframe = true` with an empty reference, even though the wire carries *residuals* relative to the sender's prediction.
  - **Effect on output.** `ParseUpdateResidual` then writes `0 + residual` into the pose. Rotation becomes `Quat(0,0,0,0)` plus a small residual, which is a near-zero quaternion.
  - **Effect on history.** `EndFrame` then observes this garbage pose as history, so subsequent predictions are wrong too, until the sender's next keyframe.
  - The header comment ("degrades to one visibly 'off' frame") is incorrect.
  - PoC (`poc midjoin`): receiver joins at frame 30. Error is 115 at f31, 120 at f60 and 130 at f120 (persistent).
  - Separately, residual keyframes only reset the predictor. The subject topology (`Subject::Serialize`) is only sent on descriptor change, and `ParseUpdateResidual` drops updates for unknown subjects (model.cpp:1384-1386). So a late joiner on a residual stream never sees the subject.
  - There is also an edge case where a NaN channel on a keyframe (model.cpp:671-685) is committed as zero by the encoder but kept at its previous value by the decoder. This is another source of divergence.
- Recommendation:
  - When the decoder cannot predict and the update is not a keyframe, *skip* applying and observing the update and return a "needs keyframe" status.
  - Make residual keyframes carry absolute values only, and optionally embed or resend the full `Subject` descriptor periodically.
  - Normalize or skip NaN channels identically on both sides.
- Effort: M
- Owner: design
- Status: closed in #342

### CORE-7: CRC-32 computed bit-by-bit dominates Serialize/Parse cost
- Category: performance
- Severity: high
- Location: src/o3ds/model.cpp:872, src/o3ds/model.cpp:982
- Evidence:
  - `CRCPP::CRC::Calculate(data, len, CRCPP::CRC::CRC_32())` uses the parameter overload, which computes the remainder bitwise (CRC.h:457-463 in the pinned CRCpp).
  - Benchmark (`perf.cpp`, -O2, 250 bones plus 250 curves, 34.7 KB): Serialize 548 µs, Parse 496 µs, of which bitwise CRC is 413 µs. A static `CRC::Table` brings the CRC down to 99 µs.
  - This runs on both ends, per subject, at 60 Hz. In the default legacy mode every frame is a full snapshot.
- Recommendation:
  - Use a function-static `CRCPP::CRC::Table<uint32_t,32>` (thread-safe init). A better option is a slicing-by-8 or hardware CRC32C.
  - Since FlatBuffers is already verified and transports have their own checksums, consider making the CRC optional through a flags bit.
- Effort: S
- Owner: coding
- Status: closed in #320

### CORE-8: `CalcMatrices` is O(N²), silently accepts parent cycles, and has a dead NaN check
- Category: security
- Severity: high
- Location: src/o3ds/model.cpp:210-222, src/o3ds/model.cpp:272-311
- Evidence:
  - **Complexity.** The world-matrix solve repeats full passes until no progress, which is O(N·depth).
    - A 16 000-node reverse-ordered chain (1.3 MB packet) takes **912 ms** in `Parse` at -O2 (`chain.cpp`: 2k→8 ms, 8k→101 ms, 16k→912 ms).
    - It runs inside `Parse` on the receiver, per packet.
  - **Cycles.** A parent cycle (a→b→a) leaves `done=true` with `bWorldMatrix=false` and returns `true`. PoC `poc cycle`: `parse=1 err=''`.
  - **Dead NaN check.** `if (m.HasNan())` is evaluated right after `m = Matrixd()` (identity), so it can never fire.
- Recommendation:
  - Solve the hierarchy in one topological pass, or require `parent < index` as the wire invariant and reject violations.
  - Detect cycles and fail.
  - Enforce limits: max nodes, curves and subjects per buffer.
  - Move the NaN/Inf check after the local matrix is built.
  - Make world-matrix computation opt-in, since the UE receiver consumes local TRS.
- Effort: M
- Owner: coding
- Status: closed in #265

### CORE-9: No validation of non-finite floats or sizes from the wire
- Category: security
- Severity: medium
- Location: src/o3ds/model.cpp:1249-1258, src/o3ds/model.cpp:1305-1314, src/o3ds/model.cpp:1364-1373, src/o3ds/model.cpp:1175-1208, src/o3ds/model.cpp:189-208
- Evidence:
  - `ParseUpdate`/`ParseSubject` copy wire floats straight into transforms and curves. NaN/Inf translations, rotations, scales and curve values reach the plugin unchanged.
  - `Transform::nan()` only tests `x != x` and so misses Inf. It is only used on the send side.
  - There is no cap on node, curve or subject counts beyond the buffer size. The Verifier permits millions of minimal tables, and each costs a heap `Transform` (~400 B), so a 50 MB datagram can expand into GB-scale allocation.
- Recommendation:
  - Add a `ParseLimits` struct (max subjects, nodes, curves, name length).
  - Reject or zero non-finite values at parse time with a counter.
  - Use `std::isfinite` in `nan()` and rename it `nonFinite()`.
- Effort: S
- Owner: coding
- Status: closed in #265

### CORE-10: Legacy and quantized delta paths are not loss-safe: `sent()` marks delivery at serialize time and there is no periodic resync
- Category: bug
- Severity: medium
- Location: src/o3ds/transform_component.h:70-72, src/o3ds/transform_component.h:99-101, src/o3ds/model.cpp:529, src/o3ds/model.cpp:575, ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DSender/Public/O3DSenderComponent.h:237-239
- Evidence:
  - `SerializeUpdate` sends a channel only when `delta() > deltaThreshold` against `lastSentValue`, and calls `sent()` immediately.
  - If that datagram is lost and the channel then stops moving, it is never resent.
  - The plugin's quantized mode only full-syncs on the first frame or on a descriptor change. Despite that, the plugin header says it is "stateless-across-loss and safe on unreliable transports".
- Recommendation:
  - Add a periodic full-value refresh: for example, a round-robin subset of channels every frame, or a full `SerializeUpdate` every K frames that ignores the threshold.
  - Alternatively, expose a `ForceRefresh()` API and drive it from sequencing and loss feedback.
  - Correct the plugin documentation.
- Effort: M
- Owner: design

### CORE-11: Scale is never transmitted in any update path
- Category: bug
- Severity: medium
- Location: src/o3ds/model.cpp:579-587, src/o3ds/model.cpp:645, src/o3ds/model.cpp:1479-1481
- Evidence:
  - The legacy `SerializeUpdate` has scale emission commented out. The residual path explicitly skips scale, and `ParseUpdateResidual` ignores it.
  - Scale animation (squash-and-stretch, UE curve-driven scale) is therefore silently frozen at the last full sync whenever the plugin uses residual or quantized mode.
  - `ToPoseSample` still carries scales, so the predictors do useless work on them.
- Recommendation:
  - Restore scale deltas behind the same threshold. The schema already has `scale:[ScaleUpdate]`, so no schema change is needed.
  - Add scale to the residual codec, or document it as unsupported and have the sender force a full sync on scale change.
- Effort: S
- Owner: coding
- Status: closed in #346

### CORE-12: Rotation quantization picks precision from delta size, but encodes the absolute value
- Category: bug
- Severity: medium
- Location: src/o3ds/model.cpp:544-566, src/o3ds/quant/channel_quant.cpp:216-242, src/o3ds/quant/channel_quant.cpp:284-301
- Evidence:
  - **The mismatch.** The tier is chosen from `rotation.delta()` (change since last send). But smallest-three encodes the *absolute* quaternion. So the Byte tier (±0.707/127 per component, about 0.3–0.5° error) is used exactly when motion is small.
  - **Effect on output.** Slow or idle bones show stepping and jitter.
  - **Error is never refined.** `sent()` stores the exact unquantized value, so a bone that stops leaves the receiver on a stale quantized value.
  - **Non-unit inputs.** `PrepareSmallestThree` does not normalize its input, so a slightly non-unit quaternion reconstructs to a different rotation.
- Recommendation:
  - Choose the rotation tier by required angular precision (a config value in degrees), not by delta.
  - Normalize before quantizing.
  - Set `lastSentValue` to the *dequantized* value so the next delta measures the receiver-visible error.
- Effort: S
- Owner: design

### CORE-13: Quaternion handling in the residual and matrix paths is additive and never re-normalized
- Category: bug
- Severity: medium
- Location: src/o3ds/model.cpp:717-730, src/o3ds/model.cpp:1464-1476, src/o3ds/math.h:53-62, src/o3ds/math.h:209-229
- Evidence:
  - Rotation residuals are component-wise R⁴ differences (`value - refRot`) with float truncation. The reconstruction `refRot + residual` is never normalized.
  - PoC after 200 frames: |q| = 1.00017.
  - `Matrixd::Quaternion` assumes a unit quaternion, so non-unit input produces scale and shear in world matrices.
  - `dist(Vector4)` threshold gating treats q and −q as far apart, which causes needless full resends on hemisphere flips.
- Recommendation:
  - Encode rotation residuals as a delta rotation, `q_actual * conj(q_pred)` in the same hemisphere, quantized.
  - Normalize on reconstruct, on both encoder commit and decoder, identically.
  - Use `1 - |dot(q0,q1)|` for thresholds.
  - Normalize inside `Matrix::Quaternion`, or assert unit length.
- Effort: M
- Owner: design
- Status: closed in #350

### CORE-14: Lockstep predictors depend on cross-compiler floating-point determinism that is not guaranteed
- Category: architecture
- Severity: medium
- Location: src/o3ds/predict/quat_math.cpp:58-96, src/o3ds/predict/linear_predictor.cpp:59-116, src/o3ds/predict/quadratic_predictor.cpp:38-44, .github/workflows/core-tests.yml:91-100
- Evidence:
  - Encoder (MSVC/UE) and decoder (possibly another compiler or libm) must compute *bit-identical* predictions. They use `acos`, `sin`, `cos` and `sqrt`, which vary by libm and by `/fp:fast`.
  - Linear extrapolation propagates any per-frame difference `e` as `e_{n+1} = 2e_n − e_{n−1}` (marginally stable, growing linearly). Quadratic prediction is worse.
  - The DeterminismProbe is explicitly "not a pass/fail gate".
- Recommendation:
  - Make C2 robust to non-bit-exact decoders by quantizing the reference: round the predicted pose to the wire grid before both sides use it.
  - Alternatively, use predictors built from +, −, × only, with fixed-point time.
  - Make the probe a CI gate comparing MSVC and GCC outputs.
- Effort: M
- Owner: design
- Status: closed in #353

### CORE-15: A single forged or corrupted `tx_seq` blackholes a ReorderGate stream; `PeekMeta` skips the CRC
- Category: security
- Severity: medium
- Location: src/o3ds/reorder_gate.cpp:116-130, src/o3ds/reorder_gate.cpp:177-198, src/o3ds/model.cpp:941-967
- Evidence:
  - A frame with `seq = last + 2^40` (same epoch) is buffered. On timeout it is delivered with `lost += 2^40` and `mLastApplied` jumps, after which every legitimate frame is `stale_dropped` until an epoch bump.
  - PoC (`poc gate`): 0 of 90 legitimate frames delivered.
  - `PeekMeta` intentionally does not check the CRC, so a corrupted-but-verifiable buffer drives gate state before `Parse` would reject it.
  - This is currently dormant in the UE plugin only because the plugin sender never sets `tx_seq` (see CORE-29).
- Recommendation:
  - Treat a forward jump larger than a configured `max_forward_jump` like a restart, or discard it.
  - Check flags and CRC in `PeekMeta`, which is cheap once the CRC is table-based (CORE-7).
- Effort: S
- Owner: coding
- Status: closed in #345

### CORE-16: Schema versioning: new semantics are invisible to old readers, and there is no wire version or file identifier
- Category: architecture
- Severity: medium
- Location: src/o3ds.fbs:150-160, src/o3ds.fbs:188-195, src/o3ds.fbs:213, CMakeLists.txt:13, .github/workflows/windows.yml:91, CHANGELOG.md:1-3
- Evidence:
  - **Old readers misread new data.** `predictor_id` changes the meaning of the existing `translations`/`rotation`/`curves` vectors from absolute values to residuals. A pre-C2 reader ignores the unknown field and applies residuals as absolute poses, producing silent garbage. Quantized `*_q8/_q16` vectors are ignored by old readers, so those channels freeze.
  - **No wire version.** There is no `file_identifier` and no protocol version field. `O3DS_VERSION_TAG` is still "1.0.4" after the tx_seq, residual and quant additions, which violates the project rule to bump it.
  - **Release version is ignored.** windows.yml passes `-DVERSION_TAG=...`, which CMake never reads.
  - **Changelog.** `CHANGELOG.md` says "Nothing yet".
  - Field order itself is correct: VT offsets 4..32 are sequential, and new fields are appended.
- Recommendation:
  - Add `file_identifier "O3DS"` (a new-reader check) and a `protocol_version:ushort` on `SubjectList`.
  - Put residual/quant values in *new* vectors, so old readers see "no update" rather than wrong values.
  - Alternatively, gate residual mode behind receiver capability negotiation.
  - Make `O3DS_VERSION_TAG` overridable (`-DO3DS_VERSION_TAG`) and fix windows.yml.
  - Populate the CHANGELOG "Schema/Protocol" section.
- Effort: M
- Owner: design
- Status: closed in #335

### CORE-17: Ownership and API hazards in the model layer
- Category: architecture
- Severity: medium
- Location: src/o3ds/model.h:119-167, src/o3ds/model.h:303-323, src/o3ds/model.h:393, src/o3ds/model.cpp:356-360, src/o3ds/model.cpp:1025-1031
- Evidence:
  - **Raw owning pointers.** `SubjectList::mItems` (`std::vector<Subject*>`) and `TransformList::mItems` hold raw owning pointers.
  - **Copy semantics.** `SubjectList(const SubjectList&)` silently constructs an *empty* list. The implicitly generated copy-assignment operator shallow-copies `mItems`, so `a = b;` double-deletes. `TransformList` has the same issue.
  - **Serialize mutates.** `Serialize()` is non-const because it captures quantization anchors (model.cpp:356-360). Every plugin transport therefore has to `const_cast<O3DS::SubjectList&>(List).Serialize(...)`.
  - **Parse default.** `Parse(..., clearInactive=true)` deletes *all* subjects whenever any buffer carries `subjects`. For a multiplexed receiver, one subject's full sync destroys the others, along with their residual decoders.
  - **Lookup cost.** `findSubject` is a linear string compare per update.
- Recommendation:
  - Use `std::vector<std::unique_ptr<...>>`, delete copy and allow move.
  - Make `Serialize` const, and capture anchors in an explicit `SetQuantAnchor()` step.
  - Default `clearInactive=false`, or replace it with explicit subject-lifecycle messages.
  - Index subjects in an `unordered_map`.
- Effort: M
- Owner: design

### CORE-18: Per-frame allocation churn on the hot paths
- Category: performance
- Severity: medium
- Location: src/o3ds/model.cpp:318-405, src/o3ds/model.cpp:859-878, src/o3ds/model.cpp:1125-1133, src/o3ds/model.cpp:429-448, src/o3ds/predict/concealment.cpp:186-259, src/o3ds/reorder_gate.cpp:127, ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DReceiver/Private/O3DReceiverSource.cpp:577-579
- Evidence:
  - **Serialize.** Every call builds a fresh `FlatBufferBuilder`, per-transform temporary `std::vector`s, and `CreateString` for every bone name. Then `finalize` copies the buffer byte-by-byte through `back_inserter`.
  - **Parse.** On a full sync (every frame in the plugin's default legacy mode), Parse deletes and `new`s every `Transform`, and builds a `std::map<std::string, ...>` of anchors.
  - **Double verification.** `PeekMeta` and then `Parse` each run the full Verifier.
  - **Residual path.** `ToPoseSample` allocates 4 vectors, twice per frame (encoder and decoder).
  - **Concealment.** `ConcealmentEngine` copies whole `PoseSample`s 2–4 times per call.
  - **ReorderGate.** Allocates map/set nodes per frame and calls through `std::function`.
- Recommendation:
  - Reuse a caller-owned builder (`builder.Clear()`) and write the header in place with `resize` + `memcpy`.
  - Diff topology on resync and reuse `Transform` objects.
  - Add a `Parse` overload that skips re-verification when `PeekMeta` has already verified the buffer.
  - Add `ToPoseSample(PoseSample& out)`.
  - Use move/swap in the concealment engine.
- Effort: M
- Owner: coding

### CORE-19: Test gaps: no adversarial, loss or MSVC coverage
- Category: tests
- Severity: medium
- Location: test/model_tests.cpp:28-108, test/udp_fragment_tests.cpp:29-90, test/model_residual_tests.cpp:66-470, test/CMakeLists.txt:8-28, .github/workflows/windows.yml:88-103
- Evidence:
  - All 174 tests pass under ASan/UBSan, but they are round-trip and happy-path only.
  - There are no tests for malformed or verifier-passing hostile buffers (CORE-1 and CORE-8 would have been caught), mixed-fragSize or many-id fragment floods (CORE-2, CORE-3), residual behaviour under dropped, reordered or late-join frames (CORE-5, CORE-6), NaN/Inf input, or scale updates.
  - The suite is a single ctest entry.
  - windows.yml builds Release with MSVC, which is what the plugin ships, but never runs the tests.
- Recommendation:
  - Add a libFuzzer target (`Parse`, `PeekMeta`, `UdpMapper`) and run it for about 60 s in core-tests.yml.
  - Add loss and reorder property tests for residual mode.
  - Run `o3ds_core_tests` in windows.yml, Release.
  - Register each `O3DS_TEST` as its own ctest entry.
- Effort: M
- Owner: coding
- Status: closed in #266

### CORE-20: Build hygiene: header install restricted to one config, warnings off, legacy targets always built
- Category: build
- Severity: medium
- Location: src/CMakeLists.txt:14, src/CMakeLists.txt:144-145, src/CMakeLists.txt:235-243, CMakeLists.txt:26-35, .gitmodules:16-18, thirdparty/CMakeLists.txt:51
- Evidence:
  - **Header install.** `install(TARGETS ... PUBLIC_HEADER DESTINATION include/o3ds CONFIGURATIONS RelWithDebInfo)` limits header install to RelWithDebInfo. This is the real reason Sync-O3DSCore.ps1 (Release) "gets no headers", not the subdirectory flattening its comment blames. The rule would also flatten `predict/` and `quant/`.
  - **Typos.** `message(FATAL ...)` is not a valid mode. `o3ds/o3ds_version.h` is listed twice.
  - **Legacy targets.** The top level unconditionally builds apps/Repeater, SubscribeTest, Test1 and plugins/mobu, which warns "MotionBuilder not installed".
  - **Warnings.** There are no warning flags and `CMAKE_CXX_STANDARD_REQUIRED` is not set. `-Wall -Wextra` gives 68 warnings, including sign-compare in `model.h:160` and `model.cpp:1253/1309/1356/1367`.
  - **WebRTC defaults.** `O3DS_ENABLE_WEBRTC` defaults ON, although the plugin uses LiveKit. `.gitmodules` lists `thirdparty/libdatachannel`, but there is no gitlink for it, while thirdparty/CMakeLists.txt still does `add_subdirectory(libdatachannel)` by default.
  - **Old FlatBuffers.** The FlatBuffers pin is v2.0.6 (2022).
- Recommendation:
  - Use `install(DIRECTORY o3ds/ DESTINATION include/o3ds FILES_MATCHING PATTERN "*.h")` for all configs.
  - Fix the typos. Set `CMAKE_CXX_STANDARD_REQUIRED ON`.
  - Add `-Wall -Wextra` or `/W4` on the library target, with `-Werror` in CI.
  - Gate apps and plugins behind `O3DS_BUILD_APPS` (default OFF), and default WebRTC OFF.
  - Remove the stale libdatachannel submodule entry.
  - Plan a FlatBuffers upgrade, keeping plugin headers and flatc in lockstep.
- Effort: S
- Owner: coding

### CORE-21: Vendoring script ships `.cpp` files and legacy headers, is Win64-only, and has no version check
- Category: build
- Severity: low
- Location: Build/Scripts/Sync-O3DSCore.ps1:136-142, Build/Scripts/Sync-O3DSCore.ps1:76-83, Build/Scripts/Sync-O3DSCore.ps1:112-127
- Evidence:
  - **Content.** It copies all of `src/o3ds` (sources and connector headers) into the plugin include dir.
  - **Unneeded dependency.** It builds CML, which nothing in `src/o3ds` uses (core-tests.yml:34-36 says so).
  - **Platform.** It only produces `lib/Win64`.
  - **No version check.** Nothing verifies that the plugin's vendored FlatBuffers headers (2.0.6) match the flatc used to generate `o3ds_generated.h`, and the generated header has no version `static_assert` at this flatc version.
- Recommendation:
  - Copy only the public headers of the stable surface (see CORE-30).
  - Drop CML from the dependency list.
  - Write a stamp file (git SHA, flatc version, `O3DS_VERSION_TAG`) and check it in `Open3DSender.Build.cs`.
  - Add Linux/Mac lib outputs when those platforms are needed.
- Effort: S
- Owner: coding
- Status: closed in #284

### CORE-22: Unaligned, type-punned, host-endian header reads and writes
- Category: code-quality
- Severity: low
- Location: src/o3ds/model.cpp:866-874, src/o3ds/model.cpp:984-985, src/o3ds/udp_fragment.cpp:59-67, src/o3ds/udp_fragment.cpp:99, src/o3ds/udp_fragment.cpp:191
- Evidence: `*(std::uint32_t*)data` and `(uint32_t*)data` reads on `char*` buffers are strict-aliasing and potentially alignment UB. Flags, CRC and fragment headers are written in host byte order, which is not portable to big-endian peers.
- Recommendation: Use `memcpy` into a local variable plus explicit little-endian conversion (`flatbuffers::ReadScalar`/`WriteScalar`).
- Effort: S
- Owner: coding
- Status: closed in #335

### CORE-23: Legacy `ParseUpdate` stops at the first out-of-range index and mixes signed and unsigned comparisons
- Category: bug
- Severity: low
- Location: src/o3ds/model.cpp:1253-1256, src/o3ds/model.cpp:1309-1312, src/o3ds/model.cpp:1356-1359, src/o3ds/model.cpp:1367
- Evidence:
  - `if (id < outSubject->mTransforms.size()) ... else break;` means one bad index drops all later valid entries.
  - A negative `int` becomes a huge `size_t`, so the comparison is safe only by accident.
  - The quantized branches in the same function correctly use `id < 0 || ... continue`.
- Recommendation: Use the same `if (id < 0 || (size_t)id >= n) continue;` pattern everywhere.
- Effort: S
- Owner: coding
- Status: closed in #265

### CORE-24: `Context` copy constructor drops `mFormat`
- Category: bug
- Severity: low
- Location: src/o3ds/context.h:47-51, src/o3ds/context.h:94
- Evidence: The user-defined copy constructor copies only `mX/mY/mZ`. `mFormat` (serialized as `Subject.format`) is default-constructed on every copy.
- Recommendation: Delete the user-defined copy constructor (rule of zero).
- Effort: S
- Owner: coding

### CORE-25: Dead and broken code in core headers
- Category: code-quality
- Severity: low
- Location: src/o3ds/math.h:141-148, src/o3ds/model.cpp:461, src/o3ds/model.cpp:1504-1537, src/o3ds/o3ds.cpp:1-20, src/o3ds/getTime.cpp:51, src/o3ds/predict/concealment.cpp:103-107
- Evidence:
  - `Matrix::Transpose()` returns an undefined `CMatrix4x4`; it only compiles because the template is never instantiated.
  - There is an unused, wrongly typed `curveUpdates` vector, a large commented-out `Subject::update`, and an entirely commented-out `o3ds.cpp` that duplicates `getVersion`.
  - `getTime.cpp` multiplies by the float literal `1e-9f`.
  - The `ConcealmentEngine` constructor accepts a null predictor and later dereferences it.
- Recommendation: Delete the dead code, fix `Transpose`, use `1e-9`, and assert or throw on a null predictor.
- Effort: S
- Owner: coding

### CORE-26: `ClockOffsetEstimator` has signed-overflow UB on hostile timestamps
- Category: code-quality
- Severity: low
- Location: src/o3ds/clock_offset.cpp:73, src/o3ds/clock_offset.cpp:86-88, src/o3ds/clock_offset.cpp:103-111
- Evidence:
  - `(int64_t)local_recv_us - (int64_t)tx_wallclock_us` overflows (UB) for wire values above `INT64_MAX`.
  - After a sender clock steps backward, `mLastTxWallclockUs` regresses, and the next forward sample gets a huge slew budget, so the estimate snaps.
- Recommendation: Reject `tx_wallclock_us` more than ±1 day from `local_recv_us`, and keep `mLastTxWallclockUs = max(...)`.
- Effort: S
- Owner: coding
- Status: closed in #265

### CORE-27: Legacy connectors are dead relative to the UE plugin, and have their own defects
- Category: architecture
- Severity: low
- Location: src/CMakeLists.txt:163-201, src/o3ds/tcp.cpp:328-347, src/o3ds/nng_connector.cpp:112-120, src/o3ds/webrtc_connector.cpp:1-472
- Evidence:
  - The plugin uses none of `base_connector`, `nng_connector`, `pair`, `publisher`, `subscriber`, `async_*`, `pipeline`, `request`, `tcp`, `udp`, `websocket`, `webrtc_connector`, `socket_*`, `binary_stream` or `xsens_parser`. Its NNG, Sockets and WebRTC transports are reimplemented in UE modules.
  - `TcpSocket::Receive(std::vector<char>&)` busy-loops for 30 s and grows unbounded.
  - The NNG connectors `malloc` a copy of every message for the caller to free.
- Recommendation:
  - Move these files into a separate `open3dstream_legacy` target (default OFF), excluded from the plugin sync.
  - Keep them building for Repeater and MotionBuilder until those are archived (CORE-28).
- Effort: M
- Owner: design
- Status: closed in #375

### CORE-28: Archive the legacy apps, DCC plugins, python and sphinx trees
- Category: architecture
- Severity: low
- Location: CMakeLists.txt:26-35, apps/, plugins/maya, plugins/mobu, python/, sphinx/, .github/workflows/doc.yml:23
- Evidence:
  - The following have not been touched since 2025-11-24: `apps/FbxStream` (commented out of the build), `apps/Test1`, `apps/SubscribeTest`, `apps/XSensTest`, `plugins/maya` (commented out), `plugins/mobu` (needs the MotionBuilder SDK), `python/` (signaling server for the legacy webrtc_connector) and `sphinx/`, which documents the legacy connectors and is still published by doc.yml.
  - `apps/DeterminismProbe` and `apps/PredictorEval` are active (2026-07) roadmap tooling. `apps/Repeater` has its own image workflow.
- Recommendation:
  - **Keep:** DeterminismProbe, PredictorEval, and Repeater (if the image is still deployed; move it to the legacy target).
  - **Archive** (move to an `archive/` branch or tag, and remove from the default build): FbxStream, Test1, SubscribeTest, XSensTest, plugins/maya, plugins/mobu, python/.
  - Replace sphinx with docs for the stable surface.
- Effort: S
- Owner: design
- Status: closed in #375

### CORE-29: Sequencing (A1) is dormant in production because the UE sender never sets `tx_seq`/`frame_epoch`
- Category: architecture
- Severity: medium
- Location: src/o3ds/model.h:377-390, src/o3ds/sequencing.h:40-61, ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DSender/Private/O3DSenderSerializer.cpp:340, ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DSender/Private/O3DSenderSerializer.cpp:468
- Evidence:
  - The core API makes sequencing an optional trailing argument that defaults to 0 = legacy.
  - The plugin calls `Serialize(Buffer, Now)` and `SerializeUpdateResidual(Buffer, Count, DeltaThreshold, Now)` with no seq, and nothing in the plugin uses `SequenceCounter`.
  - As a result, every UE stream takes the receiver's legacy path, and the ReorderGate, loss stats and clock-offset estimation never engage.
- Recommendation:
  - Introduce a per-stream `O3DS::StreamWriter` that owns a `SequenceCounter`, epoch and builder, and always stamps `tx_seq`, `tx_wallclock_us` and `frame_epoch`.
  - Deprecate the raw default-zero overloads.
- Effort: S
- Owner: design
- Status: closed in #341

### CORE-30: Define a small, stable public surface for the plugin
- Category: architecture
- Severity: low
- Location: src/CMakeLists.txt:104-148, src/o3ds/model.h:173-427
- Evidence:
  - `pub_headers` exports 40+ headers, including socket internals.
  - `model.h` exposes mutable public members (`mItems`, `mTransforms`, `mCurveValues`, `mError`) and wire-schema types (`O3DS::Data::*`, `flatbuffers::Offset`) directly.
  - The plugin needs only a handful of types.
- Recommendation: Provide `o3ds/o3ds_api.h` covering:
  - `StreamWriter` (Serialize and SerializeUpdate with sequencing).
  - `StreamReader` (PeekMeta, Parse with limits, returning a structured `ParseResult` instead of `bool` + `mError`).
  - `ReorderGate`, `ClockOffsetEstimator`, `ConcealmentEngine`, the predictors, `UdpFragmenter`/`UdpReassembler` (bounded), and `NowUtcMicros`.
  - Keep the FlatBuffers types and connectors private, and version the surface with `O3DS_VERSION_TAG`.
- Effort: M
- Owner: design
