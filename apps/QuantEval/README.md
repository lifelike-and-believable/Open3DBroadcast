# QuantEval

Measures what quantized encoding does to motion as a receiver sees it (CORE-12). It plays a
take through the real encoder and decoder (`Subject::Serialize`, `Subject::SerializeUpdate`
with `QuantRanges`, `SubjectList::Parse`), with a full sync at the sender's interval, and
compares the decoded pose with the input on every frame.

It exists so quantization changes are judged on numbers before anyone looks at them in the
editor: the July 2026 quantization (#243, #244) showed visible jitter on idle animation in a
live test (#247), and the maintainer wants that not to happen again.

## What it reports

Per take and encoding configuration:

| Column | Meaning |
|---|---|
| `B/frame` | mean bytes per frame on the wire |
| `err mean`, `err p95`, `err max` | rotation error per bone per frame, degrees |
| `jit p95`, `jit max` | jitter: the absolute difference between the decoded pose's frame-to-frame rotation and the input's, degrees per frame. Motion the input does not have, which is what reads as shaking |
| `idle p95`, `idle max` | jitter on idle bones only (input step below `--idle-deg`, default 0.05 degrees per frame) |
| `stale` | the largest error on a bone that has been idle for `--stale-frames` frames (default 30): a stopped bone left off by the last quantized value |
| `trans max` | the largest translation error, scene units |

Configurations, every run:

- `full-float`: a full frame every frame, the reference.
- `quant-ue-defaults`: the sender component's defaults (`O3DSenderComponent.h`): byte range 0.01, half range 1.0, hysteresis 0.15, delta threshold 0.0001, full sync every second.
- `quant-no-byte-tier`: the same with the Byte tier disabled (byte range 0), so channels use the 16-bit tier or full floats.
- `custom`, when any of `--byte-range`, `--half-range`, `--hysteresis`, `--delta` or `--sync` is given.

## The corpus

By default it runs PredictorEval's synthetic suite (idle, walk, run, sharp, walk_noisy). It is
deterministic and a proxy, not real mocap; see `apps/PredictorEval/README.md` for what such
numbers support. A recorded take gives the numbers that matter:

```sh
QuantEval --capture take.o3dscap [--subject Name] [--csv]
```

The capture must hold full frames (the sender's legacy encoding), so the input is the exact pose.

To record one in Unreal, with the sender component on its default encoding (residual and
quantization off), run the console commands:

```text
o3d.Sender.Capture.Start idle-take.o3dscap
o3d.Sender.Capture.Stop
```

Relative paths go under `Saved/O3DCaptures` of the project. Every sender in the process is
recorded; pick one subject with `--subject`.

## First results (synthetic suite, 2026-10-03)

| Take | Config | B/frame | idle jitter p95 / max | err max |
|---|---|---|---|---|
| idle | quant-ue-defaults | 643 | 0.395 / 0.858 deg | 0.508 deg |
| idle | quant-no-byte-tier | 718 | 0.0096 / 0.0199 deg | 0.0130 deg |
| walk | quant-ue-defaults | 770 | 0.273 / 0.870 deg | 0.543 deg |
| walk | quant-no-byte-tier | 800 | 0.0090 / 0.0388 deg | 0.0388 deg |

Full floats are 2048 bytes per frame on this 20-bone rig. The Byte tier puts about half a
degree of frame-to-frame jitter on idle bones, which matches the July report; without it,
jitter is about forty times smaller for 5 to 10% more bytes. These are synthetic numbers:
they rank the configurations, and real takes decide the defaults.

The first runs also showed a 1.03 degree spike in every delta encoding on the sharp-turn take,
including full-precision updates. It was a bug, fixed with this tool: a full sync did not mark
its values as sent, so a value returning near what was sent before the full sync was skipped
as unchanged.

## Building

Built by the root CMake build, next to PredictorEval:

```sh
cmake --build build --target QuantEval
```
