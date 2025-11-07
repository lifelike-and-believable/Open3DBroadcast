```markdown
# LiveKit / Cloud SFU Guidance (Open3DStream)

This project adds two protocol signals to SubjectList to enable robust “latest-only” live playback over SFUs:

- tx_seq: monotonic per-process transmit sequence number (uint64, 0 == unset).
- tx_wallclock_us: transmit wall-clock timestamp (uint64 microseconds since Unix epoch, 0 == unset).

Behavior summary:
- Senders populate both fields on encode time.
- Receivers with support drop out-of-order frames (tx_seq) and stale frames (tx_wallclock_us -> MaxAgeMs default 300ms).
- Rollout is additive; older clients ignore fields.

Implementation files (in branch feature/tx-seq-wallclock):
- o3ds.fbs (SubjectList additions)
- src/core/subject_serializer_tx.cpp (sender serialization)
- src/receiver/o3ds_receiver_tx.cpp (receiver checks)
- test/tx_seq_tests.cpp (unit tests)

See CHANGELOG.md for versioning and compatibility notes.
```