// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PulseCore Game Integration — Bridge Protocol v1 (envelope + message model).
//
// A small, stable core. An integration (game mod / plugin / external adapter) reports WHAT is happening
// in the game (state/event/value); the PulseCore Effects Engine decides what the DualSense should feel.
// The wire format is newline-delimited JSON (one message per line). Unknown message types and unknown
// state/event/value names must be tolerated by older builds — never a hard error. Part of the OPEN
// protocol layer (PolyForm Noncommercial 1.0.0). See docs for the full specification.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "pulse/json.h"

namespace pulse::proto {

// Bump BOTH this and the pipe-name suffix on any breaking change; add fields compatibly otherwise.
inline constexpr int kProtocolVersion = 1;

// Local named pipe. Local-only (no port / no firewall), lets the server check the client PID and
// detect disconnects. Keep in sync with the C# / server copies of the name.
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\PulseCore.GameIntegration.v1";

// Transport limits (also advertised to the client in `welcome`). Guardrails against malformed / hostile
// clients: bounded message size, bounded string length, heartbeat + disconnect timeout.
inline constexpr int kMaxMessageSize = 4096;
inline constexpr int kMaxStringLength = 512;
inline constexpr int kMaxEventsPerSecond = 240;
inline constexpr int kHeartbeatIntervalMs = 2000;
inline constexpr int kDisconnectTimeoutMs = 6000;

enum class MsgType {
    Unknown,   // any type token we don't recognize — carried through, never fatal
    Hello,     // client -> server: open a session, negotiate protocol
    Welcome,   // server -> client: session accepted/rejected + limits
    Ready,     // client -> server: integration finished initializing
    State,     // client -> server: a state that persists until it changes (weapon.current, ...)
    Event,     // client -> server: a one-shot event (weapon.fire, player.damaged, ...)
    Value,     // client -> server: a frequently-updated numeric value (vehicle.rpm, bow.draw, ...)
    Heartbeat, // client -> server: liveness
    Error,     // either direction
    Goodbye,   // client -> server: clean shutdown
};

std::string ToString(MsgType type);
MsgType ParseType(std::string_view token);

// One decoded protocol message. Only the fields relevant to `type` are populated; the rest stay at
// their defaults. `value` holds the state/value payload (string or number or bool).
struct Message {
    int protocol = kProtocolVersion;
    MsgType type = MsgType::Unknown;
    std::string type_raw;   // original token (preserved for MsgType::Unknown)

    // state / event / value
    std::string name;
    json::Value value;

    // hello
    std::string integration_id;
    std::string game_id;
    std::string integration_version;
    int protocol_min = kProtocolVersion;
    int protocol_max = kProtocolVersion;
    unsigned long process_id = 0;
    std::vector<std::string> capabilities;

    // welcome
    bool accepted = false;
    std::string session_id;
    int max_message_size = 0;
    int heartbeat_interval_ms = 0;

    // error
    std::string error_code;
    std::string error_message;
};

struct ParseResult {
    bool ok = false;
    std::string error;   // set when ok == false
    Message message;
};

// Decode one JSON message. A malformed line yields ok=false with an error (never throws). A well-formed
// object with an unrecognized "type" yields ok=true, type=Unknown, type_raw set.
ParseResult ParseMessage(std::string_view line);

// Encode a message to a single JSON line (no trailing newline).
std::string SerializeMessage(const Message& msg);

// Convenience builders.
Message MakeHello(std::string integration_id, std::string game_id, std::string version,
                  std::vector<std::string> capabilities = {}, unsigned long process_id = 0);
Message MakeWelcome(bool accepted, std::string session_id);
Message MakeState(std::string name, json::Value value);
Message MakeEvent(std::string name, json::Value value = {});
Message MakeError(std::string code, std::string message);

}  // namespace pulse::proto
