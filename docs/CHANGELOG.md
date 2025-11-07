```markdown
## Unreleased

### Schema/Protocol
- Add SubjectList fields:
  - tx_seq (uint64): monotonic transmit sequence number (0 == unset).
  - tx_wallclock_us (uint64): transmit wall-clock timestamp in UTC microseconds (0 == unset).
- New receivers use tx_seq for deterministic ordering/dedup and tx_wallclock_us for stale-frame drops.
- Backwards compatible: old clients ignore these fields; new receivers fall back cleanly when fields are unset.
```