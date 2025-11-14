#include <atomic>
#include <chrono>
#include "flatbuffers/flatbuffers.h"
#include "o3ds_generated.h" // generated from o3ds.fbs

// Global per-process monotonic transmit sequence (start at 1)
static std::atomic<uint64_t> g_tx_seq_counter{1};

// Helper: current UTC time in microseconds since Unix epoch
static uint64_t NowUtcMicroseconds() {
    using namespace std::chrono;
    auto now = std::chrono::system_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
}

// Example BuildSubjectList wrapper: adapt parameters and created call to actual generated signature.
flatbuffers::Offset<o3ds::SubjectList> BuildSubjectListWithTx(
    flatbuffers::FlatBufferBuilder &b,
    /* other SubjectList args here (use exact order from generated header) */,
    bool populate_tx_fields = true)
{
    uint64_t tx_seq = 0;
    uint64_t tx_wallclock_us = 0;

    if (populate_tx_fields) {
        // Use relaxed ordering for atomic increment (cheap and sufficient).
        tx_seq = g_tx_seq_counter.fetch_add(1, std::memory_order_relaxed);
        tx_wallclock_us = NowUtcMicroseconds();
    }

    // Replace the arguments below with the real field arguments in the correct order per the generated CreateSubjectList API.
    // Example:
    // return o3ds::CreateSubjectList(b, fieldAOffset, fieldBOffset, ..., tx_seq, tx_wallclock_us);
    return o3ds::CreateSubjectList(
        b,
        /* field_1 */ 0,
        /* field_2 */ 0
        // ...
        // append tx_seq and tx_wallclock_us as the last arguments:
        /*tx_seq*/ tx_seq,
        /*tx_wallclock_us*/ tx_wallclock_us
    );
}