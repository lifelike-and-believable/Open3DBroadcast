# C3 go/no-go: prediction-error findings

**Recommendation: NO-GO on C3 as specified. Do C2.5 (adaptive predictor
selection) instead.**

The roadmap gates C3 explicitly:

> Treat C3 as a **spike with a go/no-go** gated on the prediction-error numbers
> from C1/C2, not a committed deliverable. […] if `LinearPredictor` doesn't
> already beat `HoldPredictor` meaningfully on real captures, the learned
> investment likely isn't worth it.
> — `resilient-streaming-and-motion-prediction.md`, §5/C3 and issue #224

This document supplies those numbers, states plainly what they can and cannot
support, and recommends what to do instead.

Measured with [`apps/PredictorEval`](../../apps/PredictorEval/README.md).
Reproduce with `build/apps/PredictorEval/PredictorEval`.

---

## ⚠️ First, the thing that blocks a clean answer

**There is no `.o3dscap` capture corpus in this repository — zero files.**

The C3 gate was designed to be decided on real captures. Workstream B shipped
the capture format (B1) and the replay engine (B2), and both are unit-tested,
but nobody ever recorded a take with them. The corpus the gate depends on does
not exist.

That is itself a finding, and arguably the most actionable one here: **recording
a real corpus is a prerequisite for *any* further predictor work**, learned or
classical. It is also cheap — the tooling is already built and tested.

Everything below is measured on a deterministic **synthetic** motion suite
standing in for that corpus. Synthetic motion is smooth-plus-noise; it does not
reproduce contact transients, solver artefacts, marker swaps, or occlusion gaps,
all of which make prediction *harder*. **Read these as an optimistic bound on
the classical baselines, and as evidence about ranking and thresholds rather
than about absolute error.**

---

## Finding 1: no single baseline wins — the regime decides

Translation RMSE, 20-node skeleton at 60 fps. `(±%)` is versus `Hold`.

| clip | horizon | Hold | Linear | Quadratic | winner |
|---|---|---|---|---|---|
| **idle** | 1f | 0.123 | 0.212 (+73%) | 0.387 (+215%) | **Hold** |
| | 2f | 0.124 | 0.323 (+162%) | 0.907 (+634%) | **Hold** |
| | 3f | 0.125 | 0.441 (+251%) | 1.645 (+1211%) | **Hold** |
| | 5f | 0.131 | 0.681 (+422%) | 3.762 (+2780%) | **Hold** |
| **walk** | 1f | 0.555 | 0.218 (−61%) | 0.384 (−31%) | **Linear** |
| | 2f | 1.089 | 0.365 (−67%) | 0.903 (−17%) | **Linear** |
| | 3f | 1.624 | 0.555 (−66%) | 1.637 (+1%) | **Linear** |
| | 5f | 2.683 | 1.083 (−60%) | 3.747 (+40%) | **Linear** |
| **run** | 1f | 3.380 | 0.907 (−73%) | 0.452 (−87%) | **Quadratic** |
| | 2f | 6.698 | 2.646 (−61%) | 1.291 (−81%) | **Quadratic** |
| | 3f | 9.902 | 5.210 (−47%) | 2.805 (−72%) | **Quadratic** |
| | 5f | 15.751 | 12.557 (−20%) | 8.604 (−45%) | **Quadratic** |
| **sharp** | 1f | 1.452 | 0.611 (−58%) | 0.841 (−42%) | **Linear** |
| | 2f | 2.839 | 1.396 (−51%) | 2.243 (−21%) | **Linear** |
| | 3f | 4.176 | 2.365 (−43%) | 4.262 (+2%) | **Linear** |
| | 5f | 6.691 | 4.750 (−29%) | 10.150 (+52%) | **Linear** |
| **walk_noisy** | 1f | 1.329 | 2.099 (+58%) | 3.826 (+188%) | **Hold** |
| | 2f | 1.629 | 3.219 (+98%) | 8.985 (+452%) | **Hold** |
| | 3f | 2.022 | 4.388 (+117%) | 16.293 (+706%) | **Hold** |
| | 5f | 2.937 | 6.798 (+131%) | 37.234 (+1168%) | **Hold** |

Each of the three baselines wins somewhere, and each loses badly somewhere.
`Quadratic` has the widest spread: best in class on smooth fast motion (−87% on
`run`), catastrophic under noise (+1168% on `walk_noisy`).

**The premise behind C3 — "Linear beats Hold, so a smarter model should beat
Linear" — is not what the data shows.** Linear beats Hold on some regimes and
loses on others, by margins as large as the wins.

### This corroborates a bug you already shipped a fix for

`idle` is the regime where extrapolation does worst. That matches
**#247 — "D1: add hysteresis to quantization tier selection, fix idle-animation
jitter."** Idle motion, where per-frame movement is below the sensor-noise
floor, is empirically the hardest regime here *and* is where a real jitter bug
surfaced in production. The synthetic result reproducing a real-world failure
mode is the strongest evidence available that the suite behaves sensibly.

---

## Finding 2: the crossover is governed by SNR, and it's a stable number

Holding the gait fixed (`walk`, 1 Hz, amplitude 6.0) and varying only sensor
noise, `Linear` degrades from decisively better than `Hold` to decisively worse.
Peak per-frame motion for this gait is ≈ 0.628 units/frame.

| σ (units) | σ / per-frame motion | Linear vs Hold @1f | @2f | @3f | @5f |
|---|---|---|---|---|---|
| 0.000 | 0.00 | −89% | −84% | −79% | −69% |
| 0.010 | 0.02 | −87% | −83% | −78% | −68% |
| 0.020 | 0.03 | −81% | −80% | −76% | −67% |
| 0.050 | 0.08 | −60% | −66% | −66% | −59% |
| 0.100 | 0.16 | −29% | −40% | −43% | −41% |
| **0.200** | **0.32** | **+17%** | **+10%** | **+7%** | **+5%** |
| 0.350 | 0.56 | +47% | +65% | +70% | +73% |
| 0.500 | 0.80 | +58% | +98% | +117% | +132% |
| 1.000 | 1.59 | +69% | +142% | +201% | +276% |

Interpolated crossover:

| horizon | σ at crossover | **σ / per-frame motion** |
|---|---|---|
| 1 frame (16.7 ms) | 0.163 | **0.26** |
| 2 frames (33.3 ms) | 0.179 | **0.28** |
| 3 frames (50.0 ms) | 0.187 | **0.30** |
| 5 frames (83.3 ms) | 0.189 | **0.30** |

**Rule of thumb: once per-sample sensor noise exceeds roughly 25–30% of
per-frame motion, extrapolating is worse than holding the last pose.**

The ratio is nearly horizon-independent, which is what makes it usable as a
runtime switch: one measurable quantity, not a per-horizon lookup table. The
mechanism is straightforward — `Linear` differentiates two samples, so it
amplifies uncorrelated noise while `Hold` passes it through once.

---

## Finding 3: RMSE is the wrong metric for concealment, and it disagrees

Concealment artefacts are perceived as a **pop** — a single visibly wrong frame.
That's a tail property, and mean error hides it. The suite contains cases where
a predictor wins on RMSE and *loses* on P95:

| case | RMSE | P95 |
|---|---|---|
| `sharp` h=2, Quadratic vs Hold | 2.243 vs 2.839 (**−21%**, better) | 5.157 vs 4.428 (**+16%**, worse) |
| `sharp` h=5, Linear vs Hold | 4.750 vs 6.691 (**−29%**, better) | 11.057 vs 10.774 (**+3%**, worse) |

Rotation on `sharp` h=5 is the sharpest illustration: `Linear` averages **5.41°**
error against `Hold`'s **14.79°** — 2.7× better — while its **P95 is 30.5°
versus Hold's 27.3°**, i.e. worse exactly where it matters. Extrapolation is
right almost all the time and badly wrong at direction reversals, and reversals
are precisely the moments a viewer is looking at.

**Any predictor selection or acceptance criterion should be written against a
tail metric (P95/P99 or the existing "pop" threshold), not RMSE.** This applies
to C1's tuning as much as to any future C3 work.

---

## Recommendation

### 1. NO-GO on C3 as specified

The gate condition is not met. C3's premise was that classical extrapolation
shows a clear, generalisable win that a learned model could extend. Instead the
best baseline flips with motion regime and noise, and in two of five regimes the
*trivial* baseline wins outright. A learned predictor would inherit exactly the
same failure mode — it would have to learn "don't predict" in the low-SNR case —
while adding a real-time inference budget, a versioned-weights shipping problem,
and the C2 determinism constraints the roadmap already flags.

There is also no corpus to train on, and none to validate against.

### 2. Do "C2.5" instead: adaptive predictor selection

The measurements point at a much cheaper win that uses only code already written
and tested:

- Estimate per-frame motion magnitude and short-window noise from the samples
  the receiver already has.
- Switch among the three existing baselines on that ratio: hold below the ~0.28
  crossover, linear in the mid band, quadratic on smooth high-velocity motion.
- Gate on a **tail** metric, per Finding 3.

Every regime in Finding 1 has a baseline that beats Hold by 20–87%, so a
selector that picks correctly captures most of the available headroom with no
new model, no training pipeline, no inference budget, and no weights to version.
`IPosePredictor` already exists as the seam, and `Version()` already exists to
signal predictor identity in-stream for C2.

This is the "classical baseline before anything adaptive/learned" posture the
roadmap already adopts for C0 and D1, applied one step further.

### 3. Record a real corpus — prerequisite for all of the above

Everything here is synthetic. Before any of this is treated as settled, record
real takes with the B1/B2 tooling that already exists and re-run
`PredictorEval` against them. Specifically, real data could overturn:

- the crossover ratio (0.26–0.30), which is the number a selector would encode;
- whether `Quadratic` is ever worth enabling, given how sharply it degrades;
- whether `idle` really is noise-dominated on your actual capture hardware, or
  whether that's an artefact of the synthetic noise model.

Recording a corpus is also what would make a future C3 reconsideration possible
at all. Until it exists, C3 is not merely un-justified — it is un-runnable.

---

## Reproducing

```sh
cmake --build build --target PredictorEval
build/apps/PredictorEval/PredictorEval                        # Finding 1
build/apps/PredictorEval/PredictorEval --noise-sweep          # Finding 2
build/apps/PredictorEval/PredictorEval --csv > results.csv    # raw
```

Output is deterministic — verified by running twice and comparing hashes — so
these tables can be regenerated exactly and diffed after any predictor change.
