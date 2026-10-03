# Golden wire frames

Frames written by a known writer, read back by `core.wire_format_tests` so a
reader change cannot silently stop accepting (or start misapplying) them
(D8, [ADR 0009](../../../docs/adr/0009-protocol-versioning.md) item 11).

## `v1/`: the baseline writer (protocol 1)

Written once by `test/compat/compat_tool.cpp` (`compat_tool write <dir>`),
built against the plan's baseline commit `develop@7aea235` (FlatBuffers and
NNG at the same submodule pins as today). Each file is one raw frame: the
8-byte header (frame word 1, CRC-32) and the FlatBuffer, with no file
identifier.

| File | Content | Current reader |
|---|---|---|
| `full.o3ds` | full snapshot, subject `Actor`, bones `Root` and `Spine` | accepted |
| `delta.o3ds` | plain delta (Root translation 0.25, 0, 0) | accepted |
| `quantized.o3ds` | D1 quantized delta (Byte tier), stamped 1 | rejected: undeclared new content |
| `residual.o3ds` | C2 residual update (Linear predictor), stamped 1 | rejected: undeclared new content |

To regenerate, build the tool against the baseline core (a `git worktree` of
`7aea235`, its `open3dstreamstatic` target) and run `compat_tool write`. The
frames must not change unless the baseline does.
