// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "pulse/integration.h"

#include <string>

#include "pulse/protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pulse {

namespace {

bool WriteLine(HANDLE pipe, const std::string& text) {
    std::string line = text;
    line.push_back('\n');
    DWORD written = 0;
    if (!WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) return false;
    return written == line.size();
}

// Read one newline-terminated line (bounded). Returns false on error / EOF / oversize.
bool ReadLine(HANDLE pipe, std::string& out) {
    out.clear();
    char c = 0;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(pipe, &c, 1, &read, nullptr) || read == 0) return false;
        if (c == '\n') return true;
        if (c != '\r') out.push_back(c);
        if (out.size() > static_cast<std::size_t>(proto::kMaxMessageSize)) return false;
    }
}

}  // namespace

integration::~integration() { disconnect(); }

bool integration::connect(const IntegrationConfig& config) {
    disconnect();

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        pipe = CreateFileW(proto::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return false;
        if (!WaitNamedPipeW(proto::kPipeName, 2000)) return false;
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;

    proto::Message hello = proto::MakeHello(config.integration_id, config.game_id, config.version,
                                            config.capabilities,
                                            static_cast<unsigned long>(GetCurrentProcessId()));
    if (!WriteLine(pipe, proto::SerializeMessage(hello))) {
        CloseHandle(pipe);
        return false;
    }

    std::string reply;
    if (!ReadLine(pipe, reply)) {
        CloseHandle(pipe);
        return false;
    }
    proto::ParseResult welcome = proto::ParseMessage(reply);
    if (!welcome.ok || welcome.message.type != proto::MsgType::Welcome || !welcome.message.accepted) {
        CloseHandle(pipe);
        return false;
    }

    pipe_ = pipe;
    connected_ = true;
    return true;
}

void integration::disconnect() {
    if (!connected_) {
        if (pipe_) { CloseHandle(static_cast<HANDLE>(pipe_)); pipe_ = nullptr; }
        return;
    }
    HANDLE pipe = static_cast<HANDLE>(pipe_);
    proto::Message bye;
    bye.type = proto::MsgType::Goodbye;
    WriteLine(pipe, proto::SerializeMessage(bye));
    CloseHandle(pipe);
    pipe_ = nullptr;
    connected_ = false;
}

bool integration::connected() const { return connected_; }

bool integration::set_state(const std::string& name, const std::string& value) {
    if (!connected_) return false;
    return WriteLine(static_cast<HANDLE>(pipe_),
                     proto::SerializeMessage(proto::MakeState(name, json::Value(value))));
}

bool integration::set_value(const std::string& name, double value) {
    if (!connected_) return false;
    proto::Message m = proto::MakeState(name, json::Value(value));
    m.type = proto::MsgType::Value;
    return WriteLine(static_cast<HANDLE>(pipe_), proto::SerializeMessage(m));
}

bool integration::set_bool(const std::string& name, bool value) {
    if (!connected_) return false;
    return WriteLine(static_cast<HANDLE>(pipe_),
                     proto::SerializeMessage(proto::MakeState(name, json::Value(value))));
}

bool integration::send_event(const std::string& name) {
    if (!connected_) return false;
    return WriteLine(static_cast<HANDLE>(pipe_), proto::SerializeMessage(proto::MakeEvent(name)));
}

bool integration::heartbeat() {
    if (!connected_) return false;
    proto::Message m;
    m.type = proto::MsgType::Heartbeat;
    return WriteLine(static_cast<HANDLE>(pipe_), proto::SerializeMessage(m));
}

}  // namespace pulse

// ---- C ABI over a single global client (one integration per process) ----
namespace {
pulse::integration g_client;
}

extern "C" {

int Pulse_Connect(const PulseConfig* config) {
    if (!config) return 0;
    pulse::IntegrationConfig cfg;
    if (config->integration_id) cfg.integration_id = config->integration_id;
    if (config->game_id) cfg.game_id = config->game_id;
    if (config->version) cfg.version = config->version;
    return g_client.connect(cfg) ? 1 : 0;
}
void Pulse_Disconnect(void) { g_client.disconnect(); }
int Pulse_IsConnected(void) { return g_client.connected() ? 1 : 0; }

int Pulse_SetState(const char* name, const char* value) {
    return (name && value && g_client.set_state(name, value)) ? 1 : 0;
}
int Pulse_SetString(const char* name, const char* value) { return Pulse_SetState(name, value); }
int Pulse_SetInteger(const char* name, long long value) {
    return (name && g_client.set_value(name, static_cast<double>(value))) ? 1 : 0;
}
int Pulse_SetFloat(const char* name, double value) {
    return (name && g_client.set_value(name, value)) ? 1 : 0;
}
int Pulse_SetBool(const char* name, int value) {
    return (name && g_client.set_bool(name, value != 0)) ? 1 : 0;
}
int Pulse_SendEvent(const char* name) { return (name && g_client.send_event(name)) ? 1 : 0; }
int Pulse_SendHeartbeat(void) { return g_client.heartbeat() ? 1 : 0; }
int Pulse_GetProtocolVersion(void) { return pulse::proto::kProtocolVersion; }

}  // extern "C"
