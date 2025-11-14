#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include "o3ds_generated.h"

// Configurable values (mirror to engine CVars or config system)
static std::atomic<int32_t> GMaxO3DSTxAgeMs{300}; // default 300ms
static std::atomic<bool> GLogO3DSTxDrops{false};

// Per-connection state keyed by a connection identifier (DataChannel label, peer id, etc.)
struct ConnectionState {
    uint64_t LastSeq = 0; // last accepted tx_seq (0 = none yet)
};

static std::mutex g_conn_states_mutex;
static std::unordered_map<std::string, ConnectionState> g_conn_states;

// Helper: now in microseconds
static uint64_t NowUtcMicroseconds() {
    using namespace std::chrono;
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// Reset called when a connection/datachannel is opened or reconnected. Key must match how sender is identified.
void OnDataChannelOpen(const std::string &conn_id) {
    std::lock_guard<std::mutex> lock(g_conn_states_mutex);
    g_conn_states[conn_id].LastSeq = 0;
}

// Returns true if the frame should be dropped. If dropped, drop_reason is populated (for logging/metrics).
bool ShouldDropFrame(const o3ds::SubjectList *sl, const std::string &conn_id, std::string &drop_reason) {
    uint64_t tx_seq = 0;
    uint64_t tx_wallclock_us = 0;

    if (sl) {
        tx_seq = sl->tx_seq();
        tx_wallclock_us = sl->tx_wallclock_us();
    }

    ConnectionState *state_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_conn_states_mutex);
        state_ptr = &g_conn_states[conn_id];
    }

    // Sequence checking: if present, enforce strict monotonic acceptance
    if (tx_seq != 0) {
        if (tx_seq <= state_ptr->LastSeq) {
            drop_reason = "seq<=LastSeq";
            if (GLogO3DSTxDrops.load()) {
                // Log: sequence drop details (avoid expensive formatting unless enabled)
            }
            return true;
        }
    }

    // Age checking: if present, drop if older than threshold
    if (tx_wallclock_us != 0) {
        uint64_t now_us = NowUtcMicroseconds();
        // Handle potential clock skew: if now_us < tx_wallclock_us, treat as fresh (do not drop)
        if (now_us >= tx_wallclock_us) {
            int64_t age_ms = static_cast<int64_t>((now_us - tx_wallclock_us) / 1000);
            if (age_ms > static_cast<int64_t>(GMaxO3DSTxAgeMs.load())) {
                drop_reason = "stale";
                if (GLogO3DSTxDrops.load()) {
                    // Log stale drop details
                }
                return true;
            }
        }
    }

    // Accept frame: update LastSeq if seq present
    if (tx_seq != 0) {
        state_ptr->LastSeq = tx_seq;
    }

    return false;
}