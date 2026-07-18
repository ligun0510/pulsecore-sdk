// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "pulse/protocol.h"

#include <array>
#include <utility>

namespace pulse::proto {

namespace {
constexpr std::array<std::pair<MsgType, std::string_view>, 9> kTypeTable{{
    {MsgType::Hello, "hello"},
    {MsgType::Welcome, "welcome"},
    {MsgType::Ready, "ready"},
    {MsgType::State, "state"},
    {MsgType::Event, "event"},
    {MsgType::Value, "value"},
    {MsgType::Heartbeat, "heartbeat"},
    {MsgType::Error, "error"},
    {MsgType::Goodbye, "goodbye"},
}};
}  // namespace

std::string ToString(MsgType type) {
    for (const auto& [t, s] : kTypeTable) {
        if (t == type) return std::string(s);
    }
    return "unknown";
}

MsgType ParseType(std::string_view token) {
    for (const auto& [t, s] : kTypeTable) {
        if (s == token) return t;
    }
    return MsgType::Unknown;
}

ParseResult ParseMessage(std::string_view line) {
    ParseResult result;
    if (line.size() > static_cast<std::size_t>(kMaxMessageSize)) {
        result.error = "message exceeds max size";
        return result;
    }
    std::string error;
    json::Value root = json::Parse(line, error);
    if (!error.empty()) {
        result.error = "malformed JSON: " + error;
        return result;
    }
    if (!root.is_object()) {
        result.error = "message must be a JSON object";
        return result;
    }

    Message& m = result.message;
    if (const json::Value* p = root.find("protocol"); p && p->is_number()) {
        m.protocol = static_cast<int>(p->as_number());
    }
    m.type_raw = root.get_string("type");
    m.type = ParseType(m.type_raw);

    m.name = root.get_string("name");
    if (const json::Value* v = root.find("value")) m.value = *v;

    m.integration_id = root.get_string("integration_id");
    m.game_id = root.get_string("game_id");
    m.integration_version = root.get_string("integration_version");
    if (const json::Value* p = root.find("protocol_min"); p && p->is_number())
        m.protocol_min = static_cast<int>(p->as_number());
    if (const json::Value* p = root.find("protocol_max"); p && p->is_number())
        m.protocol_max = static_cast<int>(p->as_number());
    if (const json::Value* p = root.find("process_id"); p && p->is_number())
        m.process_id = static_cast<unsigned long>(p->as_number());
    if (const json::Value* caps = root.find("capabilities"); caps && caps->is_array()) {
        for (const json::Value& c : caps->items()) {
            if (c.is_string()) m.capabilities.push_back(c.as_string());
        }
    }

    m.accepted = root.get_bool("accepted");
    m.session_id = root.get_string("session_id");
    if (const json::Value* p = root.find("max_message_size"); p && p->is_number())
        m.max_message_size = static_cast<int>(p->as_number());
    if (const json::Value* p = root.find("heartbeat_interval_ms"); p && p->is_number())
        m.heartbeat_interval_ms = static_cast<int>(p->as_number());

    m.error_code = root.get_string("error_code");
    m.error_message = root.get_string("error_message");

    result.ok = true;
    return result;
}

std::string SerializeMessage(const Message& msg) {
    json::Value root = json::Value::MakeObject();
    root.set("protocol", json::Value(static_cast<double>(msg.protocol)));
    root.set("type", json::Value(msg.type == MsgType::Unknown && !msg.type_raw.empty()
                                     ? msg.type_raw
                                     : ToString(msg.type)));

    switch (msg.type) {
        case MsgType::Hello:
            root.set("integration_id", json::Value(msg.integration_id));
            root.set("game_id", json::Value(msg.game_id));
            root.set("integration_version", json::Value(msg.integration_version));
            root.set("protocol_min", json::Value(static_cast<double>(msg.protocol_min)));
            root.set("protocol_max", json::Value(static_cast<double>(msg.protocol_max)));
            root.set("process_id", json::Value(static_cast<double>(msg.process_id)));
            if (!msg.capabilities.empty()) {
                json::Value caps = json::Value::MakeArray();
                for (const std::string& c : msg.capabilities) caps.push_back(json::Value(c));
                root.set("capabilities", std::move(caps));
            }
            break;
        case MsgType::Welcome:
            root.set("accepted", json::Value(msg.accepted));
            root.set("session_id", json::Value(msg.session_id));
            root.set("max_message_size", json::Value(static_cast<double>(msg.max_message_size)));
            root.set("heartbeat_interval_ms", json::Value(static_cast<double>(msg.heartbeat_interval_ms)));
            break;
        case MsgType::State:
        case MsgType::Value:
            root.set("name", json::Value(msg.name));
            root.set("value", msg.value);
            break;
        case MsgType::Event:
            root.set("name", json::Value(msg.name));
            if (!msg.value.is_null()) root.set("value", msg.value);
            break;
        case MsgType::Error:
            root.set("error_code", json::Value(msg.error_code));
            root.set("error_message", json::Value(msg.error_message));
            break;
        case MsgType::Ready:
        case MsgType::Heartbeat:
        case MsgType::Goodbye:
        case MsgType::Unknown:
            break;
    }
    return json::Dump(root);
}

Message MakeHello(std::string integration_id, std::string game_id, std::string version,
                  std::vector<std::string> capabilities, unsigned long process_id) {
    Message m;
    m.type = MsgType::Hello;
    m.integration_id = std::move(integration_id);
    m.game_id = std::move(game_id);
    m.integration_version = std::move(version);
    m.capabilities = std::move(capabilities);
    m.process_id = process_id;
    return m;
}

Message MakeWelcome(bool accepted, std::string session_id) {
    Message m;
    m.type = MsgType::Welcome;
    m.accepted = accepted;
    m.session_id = std::move(session_id);
    m.max_message_size = kMaxMessageSize;
    m.heartbeat_interval_ms = kHeartbeatIntervalMs;
    return m;
}

Message MakeState(std::string name, json::Value value) {
    Message m;
    m.type = MsgType::State;
    m.name = std::move(name);
    m.value = std::move(value);
    return m;
}

Message MakeEvent(std::string name, json::Value value) {
    Message m;
    m.type = MsgType::Event;
    m.name = std::move(name);
    m.value = std::move(value);
    return m;
}

Message MakeError(std::string code, std::string message) {
    Message m;
    m.type = MsgType::Error;
    m.error_code = std::move(code);
    m.error_message = std::move(message);
    return m;
}

}  // namespace pulse::proto
