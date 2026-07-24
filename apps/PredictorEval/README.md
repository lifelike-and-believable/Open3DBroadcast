# PredictorEval

Measures how well the C0 classical baselines (`HoldPredictor`, `LinearPredictor`,
`QuadraticPredictor`) predict a pose N frames ahead.

This exists to supply the evidence the roadmap's **C3 go/no-go** depends on:

> Treat C3 as a **spike with a go/no-go** gated on the prediction-error numbers
> from C1/C2, not a committed deliverable.
> — `docs/roadmap/resilient-streaming-and-motion-prediction.md`, §5/C3

The findings are written up in
[`docs/roadmap/c3-go-no-go.md`](../../docs/roadmap/c3-go-no-go.md).

## ⚠️ The corpus is synthetic

**There is no `.o3dscap` capture corpus in this repository** — zero files. The
roadmap assumed C1/C2 would leave one behind for exactly this decision, and that
didn't happen. Rather than block, this tool generates a deterministic synthetic
motion suite as a stand-in.

That changes what the numbers can be used for:

| Use it for | Don't use it for |
| --- | --- |
| **Ranking** predictors against each other | Absolute error magnitudes as real-mocap error |
| How the ranking **changes with motion regime** | Predicting a specific take's error budget |
| The **noise threshold** at which extrapolation stops paying | Tuning production constants without re-running on real data |

The synthetic motion is smooth-plus-noise and does not reproduce real mocap's
contact transients, solver artefacts, marker swaps, or occlusion gaps — all of
which make prediction *harder*, not easier. Treat the results as an optimistic
bound on the classical baselines.

**Re-run this against a real corpus before making an irreversible call.** The
tool already has the shape for it; it needs a `.o3dscap` reader path added and a
recorded take to point at.

## Building

Built as part of the normal core build (`O3DS_BUILD_TESTS` not required):

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/usr" -DO3DS_DISABLE_WEBRTC=ON
cmake --build build --target PredictorEval
```

## Usage

```sh
# Standard suite (idle / walk / run / sharp / walk_noisy) at horizons 1,2,3,5
build/apps/PredictorEval/PredictorEval

# Locate the SNR where extrapolation stops beating "hold the last pose"
build/apps/PredictorEval/PredictorEval --noise-sweep --horizons 2

# Machine-readable
build/apps/PredictorEval/PredictorEval --csv > results.csv
```

## Method

For each clip and each horizon `h`, the harness walks the clip in order. At
frame `i` the predictor has `Observe()`d frames `[0..i]`; it is then asked to
`Predict()` the pose at frame `i+h` and scored against that frame.

Two choices worth knowing about:

- **The predictor observes noisy samples and is scored against a noisy target.**
  That is the operationally correct target for concealment: when a frame is lost,
  the receiver's job is to reproduce the frame that *would have been displayed*,
  sensor noise and all — not some hypothetical clean signal.
- **`Predict()` returning `false` is counted, not scored.** Insufficient history
  is a real behaviour (the caller holds the last pose), not an error, so those
  frames are reported in a `declined` column rather than silently dropped or
  charged as error.

Metrics are translation RMSE and P95 (scene units, ~cm) and rotation mean and
P95 (degrees, geodesic angle with `q ≡ -q` handled).

**P95 is reported alongside RMSE deliberately.** Concealment quality is a tail
phenomenon — a single bad frame is what a viewer perceives as a "pop" — and the
two metrics disagree in at least one regime the suite covers (see the findings
doc's note on `sharp`).

## Determinism

Fully deterministic: same input → byte-identical output, verified by running
twice and comparing hashes. The noise generator is a fixed-seed LCG with
Box-Muller rather than `<random>`, because `std::normal_distribution` is not
specified to produce identical sequences across standard-library
implementations — which would make numbers from a dev machine incomparable with
numbers from CI.

Each clip seeds from its own name, so adding a clip doesn't perturb the others.
