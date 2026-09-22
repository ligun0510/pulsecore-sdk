// SPDX-License-Identifier: MIT
// PulseCore Game Integration SDK — client API for game mods / plugins / adapters.
//
// A tiny surface: connect, report state/events/values, disconnect. The integration reports WHAT happens
// in the game; PulseCore decides what the DualSense feels. Mods NEVER touch HID and never load into the
// PulseCore process — they only speak this protocol over the local named pipe. Open source, MIT (see LICENSE).
#pragma once

#include <string>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

// ---- Stable C ABI (for plugins in any language / loader) ----
extern "C" {

typedef struct PulseConfig {
    const char* integration_id;   // e.g. "community.hl2.enhanced"
    const char* game_id;          // e.g. "steam:220"
    const char* version;          // e.g. "1.0.0"
} PulseConfig;

// Forward-extensible config. `struct_size` (set to sizeof(PulseConfigV2)) + `abi_version` let the
// runtime and the client evolve independently: a newer client passing a larger struct to an older
// runtime is read only up to the runtime's known size, and a newer runtime reading an older, smaller
// struct never touches fields the client didn't provide. New fields are appended below this comment in
// future ABI versions and gated on struct_size — so the ABI grows without ever breaking.
#define PULSE_ABI_VERSION 2u
typedef struct PulseConfigV2 {
    unsigned int struct_size;     // = sizeof(PulseConfigV2)
    unsigned int abi_version;     // = PULSE_ABI_VERSION the client compiled against
    const char* integration_id;
    const char* game_id;
    const char* version;
    // (future fields appended here, gated on struct_size)
} PulseConfigV2;

int Pulse_Connect(const PulseConfig* config);       // returns 1 on success, 0 on failure
int Pulse_ConnectEx(const PulseConfigV2* config);   // versioned/sized config; validates before connecting
void Pulse_Disconnect(void);
int Pulse_IsConnected(void);

int Pulse_SetState(const char* name, const char* value);   // string state (weapon.current)
int Pulse_SetString(const char* name, const char* value);
int Pulse_SetInteger(const char* name, long long value);
int Pulse_SetFloat(const char* name, double value);
int Pulse_SetBool(const char* name, int value);
int Pulse_SendEvent(const char* name);                     // one-shot (weapon.fire)
int Pulse_SendHeartbeat(void);
int Pulse_GetProtocolVersion(void);

// ---- Handle-based sessions ----
// The functions above act on one implicit process-wide session, which is enough for a single game mod
// but cannot serve a host that drives several integrations (a launcher, a test harness, a plugin host).
// These give each one its own handle and independent lifetime. Reporting calls are non-blocking for
// every session (bounded queue + worker thread), so no session can stall a caller's frame loop.
typedef struct PulseSession PulseSession;

int Pulse_SessionCreate(const PulseConfigV2* config, PulseSession** out_session);  // 1 = connected
void Pulse_SessionDestroy(PulseSession* session);
int Pulse_SessionIsConnected(PulseSession* session);

int Pulse_SessionSetState(PulseSession* session, const char* name, const char* value);
int Pulse_SessionSetInteger(PulseSession* session, const char* name, long long value);
int Pulse_SessionSetFloat(PulseSession* session, const char* name, double value);
int Pulse_SessionSetBool(PulseSession* session, const char* name, int value);
int Pulse_SessionSendEvent(PulseSession* session, const char* name);
int Pulse_SessionHeartbeat(PulseSession* session);
// Messages dropped because that session's queue was full (0 on a healthy integration).
unsigned long long Pulse_SessionDrops(PulseSession* session);

}  // extern "C"

// ---- Convenience C++ wrapper ----
namespace pulse {

struct IntegrationConfig {
    std::string integration_id;
    std::string game_id;
    std::string version;
    std::vector<std::string> capabilities;
};

class integration {
public:
    integration() = default;
    ~integration();

    integration(const integration&) = delete;
    integration& operator=(const integration&) = delete;

    // Opens the pipe and performs the hello/welcome handshake. This is the ONLY blocking call (it runs
    // once at init, off the hot path). Returns false if PulseCore isn't listening or rejected us.
    bool connect(const IntegrationConfig& config);
    void disconnect();
    bool connected() const;

    // Report game data. These NEVER touch the pipe: the message is appended to a BOUNDED queue that a
    // worker thread drains, so a slow/stalled reader can never stall the caller — critical because a
    // game calls these from its frame loop and a blocking write would show up as a microfreeze.
    // Return false only when not connected or the queue is full (the message is dropped, see drops()).
    bool set_state(const std::string& name, const std::string& value);
    bool set_value(const std::string& name, double value);
    bool set_bool(const std::string& name, bool value);
    bool send_event(const std::string& name);
    bool heartbeat();

    // Messages dropped because the queue was full. A healthy integration never drops; a non-zero value
    // means the game is producing faster than the pipe drains (diagnostic).
    unsigned long long drops() const { return drops_.load(); }

private:
    struct Queued {
        std::string line;
        std::string coalesce_key;   // non-empty => a newer message with this key REPLACES the queued one
    };
    // coalesce_key empty => never merged (events are discrete and must all be delivered). States/values
    // carry a per-name key: only the newest matters, so a burst collapses instead of overflowing.
    bool Enqueue(std::string line, const std::string& coalesce_key);
    void WorkerLoop();

    static constexpr std::size_t kMaxQueued = 256;

    void* pipe_ = nullptr;   // HANDLE
    std::atomic<bool> connected_{false};
    std::atomic<bool> stop_{false};
    std::atomic<unsigned long long> drops_{0};
    std::thread worker_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Queued> queue_;
};

}  // namespace pulse
