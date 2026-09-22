// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "pulse/protocol.h"

#include <array>
#include <utility>

namespace pulse::proto {

namespace {
constexpr std::array<std::pair<MsgType, std::string_view>, 20> kTypeTable{{
    {MsgType::Hello, "hello"},
    {MsgType::Welcome, "welcome"},
    {MsgType::Ready, "ready"},
    {MsgType::State, "state"},
    {MsgType::Event, "event"},
    {MsgType::Value, "value"},
    {MsgType::Heartbeat, "heartbeat"},
    {MsgType::Error, "error"},
    {MsgType::Goodbye, "goodbye"},
    {MsgType::DeviceQuery, "device.query"},
    {MsgType::DeviceSnapshot, "device.snapshot"},
    {MsgType::EffectApply, "effect.apply"},
    {MsgType::EffectRelease, "effect.release"},
    {MsgType::StateBatch, "state.batch"},
    {MsgType::ClipBegin, "clip.begin"},
    {MsgType::ClipChunk, "clip.chunk"},
    {MsgType::ClipCommit, "clip.commit"},
    {MsgType::ClipReady, "clip.ready"},
    {MsgType::StreamOpen, "audio.stream.open"},
    {MsgType::StreamReady, "audio.stream.ready"},
}};

// Base64 alphabet position, or -1. A table rather than arithmetic on character ranges: '+' and '/' are
// nowhere near the letters, and the range version of this is a classic place to accept a character that
// is not in the alphabet at all.
int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}  // namespace

std::uint32_t Crc32(const std::uint8_t* data, std::size_t length) {
    if (!data) return 0;
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

bool DecodeBase64(std::string_view text, std::vector<std::uint8_t>& out) {
    out.clear();
    // Trailing '=' is optional; anything else outside the alphabet is a malformed upload, not something
    // to skip past. Silently ignoring stray characters is how a corrupted chunk becomes audio.
    std::size_t length = text.size();
    while (length > 0 && text[length - 1] == '=') --length;
    if (length % 4 == 1) return false;   // no valid encoding has this length

    out.reserve(length * 3 / 4);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (std::size_t index = 0; index < length; ++index) {
        const int value = Base64Value(text[index]);
        if (value < 0) {
            // NEVER a partial result: a caller that checked only the return value and then used what
            // it found in `out` would be assembling audio out of the prefix of a corrupted chunk.
            out.clear();
            return false;
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFFu));
        }
    }
    return true;
}

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
    if (const json::Value* f = root.find("server_features"); f && f->is_array()) {
        for (const json::Value& item : f->items()) {
            if (item.is_string()) m.server_features.push_back(item.as_string());
        }
    }
    if (const json::Value* g = root.find("grants"); g && g->is_array()) {
        for (const json::Value& item : g->items()) {
            if (item.is_string()) m.grants.push_back(item.as_string());
        }
    }
    m.session_id = root.get_string("session_id");
    if (const json::Value* p = root.find("max_message_size"); p && p->is_number())
        m.max_message_size = static_cast<int>(p->as_number());
    if (const json::Value* p = root.find("heartbeat_interval_ms"); p && p->is_number())
        m.heartbeat_interval_ms = static_cast<int>(p->as_number());

    m.error_code = root.get_string("error_code");
    m.error_message = root.get_string("error_message");
    m.retryable = root.get_bool("retryable");

    // Request/reply correlation (device.query -> device.snapshot). Bounded like every other string so a
    // hostile client can't push an unbounded id through.
    m.request_id = root.get_string("request_id");
    m.reply_to = root.get_string("reply_to");
    if (m.request_id.size() > static_cast<std::size_t>(kMaxStringLength)) m.request_id.clear();
    if (m.reply_to.size() > static_cast<std::size_t>(kMaxStringLength)) m.reply_to.clear();

    // effect.apply / effect.release. The effect payload stays raw JSON: the effects layer validates it
    // per schema, so adding a schema later needs no protocol change.
    m.effect_id = root.get_string("effect_id");
    if (m.effect_id.size() > static_cast<std::size_t>(kMaxStringLength)) m.effect_id.clear();
    if (const json::Value* target = root.find("target"); target && target->is_object()) {
        m.target_device_id = target->get_string("device_id");
        m.target_channel = target->get_string("channel");
    }
    if (const json::Value* states = root.find("states"); states && states->is_array()) {
        for (const json::Value& item : states->items()) {
            if (!item.is_object()) continue;
            const json::Value* value = item.find("value");
            m.batch.emplace_back(item.get_string("name"), value ? *value : json::Value());
        }
    }
    if (const json::Value* effect = root.find("effect")) m.effect = *effect;
    if (const json::Value* p = root.find("lease_ms"); p && p->is_number())
        m.lease_ms = static_cast<int>(p->as_number());

    // Clip upload. Only the shape is decoded here; whether the numbers are ADMISSIBLE (the format, the
    // bounds, the session's remaining budget) is the server's decision, exactly as with effect payloads.
    m.clip_id = root.get_string("clip_id");
    if (m.clip_id.size() > static_cast<std::size_t>(kMaxNameLength)) m.clip_id.clear();
    if (const json::Value* p = root.find("frames"); p && p->is_number())
        m.clip_frames = static_cast<int>(p->as_number());
    if (const json::Value* p = root.find("channels"); p && p->is_number())
        m.clip_channels = static_cast<int>(p->as_number());
    if (const json::Value* p = root.find("sample_rate"); p && p->is_number())
        m.clip_sample_rate = static_cast<int>(p->as_number());
    if (const json::Value* p = root.find("crc32"); p && p->is_number())
        m.clip_crc32 = static_cast<unsigned int>(p->as_number());
    if (const json::Value* p = root.find("offset"); p && p->is_number())
        m.clip_offset = static_cast<int>(p->as_number());
    m.stream_token = root.get_string("stream_token");
    if (m.stream_token.size() > static_cast<std::size_t>(kMaxStringLength)) m.stream_token.clear();
    if (const json::Value* p = root.find("data"); p && p->is_string()) {
        // A chunk that does not decode leaves clip_data EMPTY, and the server refuses an empty chunk.
        // Deliberately not an early return: a bad "data" field must fail as a bad clip chunk with a
        // clip-specific error, not as a malformed message that kills the whole connection.
        if (!DecodeBase64(p->as_string(), m.clip_data)) m.clip_data.clear();
    }

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
        case MsgType::Welcome: {
            root.set("accepted", json::Value(msg.accepted));
            root.set("session_id", json::Value(msg.session_id));
            root.set("max_message_size", json::Value(static_cast<double>(msg.max_message_size)));
            root.set("heartbeat_interval_ms", json::Value(static_cast<double>(msg.heartbeat_interval_ms)));
            json::Value features = json::Value::MakeArray();
            for (const std::string& f : msg.server_features) features.push_back(json::Value(f));
            root.set("server_features", std::move(features));
            json::Value grants = json::Value::MakeArray();
            for (const std::string& g : msg.grants) grants.push_back(json::Value(g));
            root.set("grants", std::move(grants));
            // Real, enforced limits — not documentation. A client can size its own buffers from these.
            json::Value limits = json::Value::MakeObject();
            limits.set("message_bytes", json::Value(static_cast<double>(msg.max_message_size)));
            limits.set("messages_per_second", json::Value(static_cast<double>(msg.max_events_per_second)));
            limits.set("state_entries", json::Value(static_cast<double>(msg.max_state_entries)));
            limits.set("name_length", json::Value(static_cast<double>(msg.max_name_length)));
            limits.set("effect_lease_ms_max", json::Value(static_cast<double>(msg.max_lease_ms)));
            root.set("limits", std::move(limits));
            break;
        }
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
            root.set("retryable", json::Value(msg.retryable));
            break;
        case MsgType::DeviceQuery:
            if (!msg.request_id.empty()) root.set("request_id", json::Value(msg.request_id));
            break;
        case MsgType::DeviceSnapshot: {
            if (!msg.reply_to.empty()) root.set("reply_to", json::Value(msg.reply_to));
            json::Value devices = json::Value::MakeArray();
            for (const DeviceInfo& device : msg.devices) {
                json::Value entry = json::Value::MakeObject();
                entry.set("device_id", json::Value(device.device_id));
                entry.set("connected", json::Value(device.connected));
                entry.set("model", json::Value(device.model));
                entry.set("physical_transport", json::Value(device.physical_transport));
                entry.set("game_transport", json::Value(device.game_transport));

                json::Value battery = json::Value::MakeObject();
                // Unknown charge is null, NOT 0 -- a mod must be able to tell "no reading" from "empty".
                battery.set("percent", device.battery_known
                                           ? json::Value(static_cast<double>(device.battery_percent))
                                           : json::Value());
                battery.set("charging", json::Value(device.charging));
                entry.set("battery", std::move(battery));

                json::Value features = json::Value::MakeObject();
                features.set("adaptive_triggers", json::Value(device.adaptive_triggers));
                features.set("lightbar_rgb", json::Value(device.lightbar_rgb));
                features.set("player_led", json::Value(device.player_led));
                features.set("mic_led", json::Value(device.mic_led));
                entry.set("features", std::move(features));

                devices.push_back(std::move(entry));
            }
            root.set("devices", std::move(devices));
            break;
        }
        case MsgType::EffectApply: {
            if (!msg.request_id.empty()) root.set("request_id", json::Value(msg.request_id));
            root.set("effect_id", json::Value(msg.effect_id));
            json::Value target = json::Value::MakeObject();
            target.set("device_id", json::Value(msg.target_device_id.empty() ? std::string("primary")
                                                                             : msg.target_device_id));
            target.set("channel", json::Value(msg.target_channel));
            root.set("target", std::move(target));
            root.set("effect", msg.effect);
            root.set("lease_ms", json::Value(static_cast<double>(msg.lease_ms)));
            break;
        }
        case MsgType::StateBatch: {
            json::Value states = json::Value::MakeArray();
            for (const auto& [name, value] : msg.batch) {
                json::Value entry = json::Value::MakeObject();
                entry.set("name", json::Value(name));
                entry.set("value", value);
                states.push_back(std::move(entry));
            }
            root.set("states", std::move(states));
            break;
        }
        case MsgType::EffectRelease:
            if (!msg.request_id.empty()) root.set("request_id", json::Value(msg.request_id));
            root.set("effect_id", json::Value(msg.effect_id));
            break;
        case MsgType::ClipBegin:
            root.set("clip_id", json::Value(msg.clip_id));
            root.set("frames", json::Value(static_cast<double>(msg.clip_frames)));
            root.set("channels", json::Value(static_cast<double>(msg.clip_channels)));
            root.set("sample_rate", json::Value(static_cast<double>(msg.clip_sample_rate)));
            root.set("crc32", json::Value(static_cast<double>(msg.clip_crc32)));
            break;
        case MsgType::ClipChunk:
            // Serializing a chunk is for tests and for a client library; the server only ever receives
            // them. The payload is left to the caller rather than base64-encoded here, because a message
            // struct carrying decoded bytes has no business re-encoding them on a path nobody uses in
            // production -- and a half-implemented encoder is worse than none.
            root.set("clip_id", json::Value(msg.clip_id));
            root.set("offset", json::Value(static_cast<double>(msg.clip_offset)));
            break;
        case MsgType::ClipCommit:
        case MsgType::ClipReady:
            root.set("clip_id", json::Value(msg.clip_id));
            break;
        case MsgType::StreamOpen:
            root.set("channels", json::Value(static_cast<double>(msg.clip_channels)));
            root.set("sample_rate", json::Value(static_cast<double>(msg.clip_sample_rate)));
            break;
        case MsgType::StreamReady:
            // The client is told the pipe name and the format explicitly rather than being expected to
            // hardcode them from the spec: a mod built against an older SDK then fails with a clear
            // mismatch instead of writing 44.1 kHz into a 48 kHz channel and sounding wrong.
            root.set("stream_token", json::Value(msg.stream_token));
            root.set("channels", json::Value(static_cast<double>(msg.clip_channels)));
            root.set("sample_rate", json::Value(static_cast<double>(msg.clip_sample_rate)));
            root.set("max_frame_bytes", json::Value(static_cast<double>(kMaxAudioFrameBytes)));
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

Message MakeWelcome(bool accepted, std::string session_id, std::vector<std::string> grants) {
    Message m;
    m.type = MsgType::Welcome;
    m.accepted = accepted;
    m.session_id = std::move(session_id);
    m.max_message_size = kMaxMessageSize;
    m.heartbeat_interval_ms = kHeartbeatIntervalMs;
    m.max_events_per_second = kMaxEventsPerSecond;
    m.max_state_entries = kMaxStateEntries;
    m.max_name_length = kMaxNameLength;
    m.max_lease_ms = kMaxLeaseMs;
    m.server_features = {kFeatureTelemetry, kFeatureDeviceQuery, kFeatureTriggerPreset,
                         kFeatureTriggerPrimitive, kFeatureTriggerRaw, kFeatureLed, kFeatureRumble,
                         kFeatureHaptic, kFeatureSpeaker};
    m.grants = std::move(grants);
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

Message MakeError(std::string code, std::string message, bool retryable) {
    Message m;
    m.type = MsgType::Error;
    m.error_code = std::move(code);
    m.error_message = std::move(message);
    m.retryable = retryable;
    return m;
}

Message MakeDeviceQuery(std::string request_id) {
    Message m;
    m.type = MsgType::DeviceQuery;
    m.request_id = std::move(request_id);
    return m;
}

Message MakeDeviceSnapshot(std::string reply_to, std::vector<DeviceInfo> devices) {
    Message m;
    m.type = MsgType::DeviceSnapshot;
    m.reply_to = std::move(reply_to);
    m.devices = std::move(devices);
    return m;
}

}  // namespace pulse::proto
