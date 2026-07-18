// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PulseCore Game Integration SDK — client API for game mods / plugins / adapters.
//
// A tiny surface: connect, report state/events/values, disconnect. The integration reports WHAT happens
// in the game; PulseCore decides what the DualSense feels. Mods NEVER touch HID and never load into the
// PulseCore process — they only speak this protocol over the local named pipe. Open (PolyForm Noncommercial 1.0.0).
#pragma once

#include <string>
#include <vector>

// ---- Stable C ABI (for plugins in any language / loader) ----
extern "C" {

typedef struct PulseConfig {
    const char* integration_id;   // e.g. "community.hl2.enhanced"
    const char* game_id;          // e.g. "steam:220"
    const char* version;          // e.g. "1.0.0"
} PulseConfig;

int Pulse_Connect(const PulseConfig* config);   // returns 1 on success, 0 on failure
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

    // Opens the pipe and performs the hello/welcome handshake. Returns false if PulseCore isn't
    // listening or rejected the session.
    bool connect(const IntegrationConfig& config);
    void disconnect();
    bool connected() const;

    bool set_state(const std::string& name, const std::string& value);
    bool set_value(const std::string& name, double value);
    bool set_bool(const std::string& name, bool value);
    bool send_event(const std::string& name);
    bool heartbeat();

private:
    void* pipe_ = nullptr;   // HANDLE
    bool connected_ = false;
};

}  // namespace pulse
