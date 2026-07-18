// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PulseCore Game Integration — vendored single-header client for a Source SDK 2013 mod.
//
// Self-contained (no other PulseCore SDK files needed): drop this one header into your mod, include it
// from ONE .cpp, and report game state/events. It speaks Bridge Protocol v1 over the local named pipe
// \\.\pipe\PulseCore.GameIntegration.v1. The mod reports WHAT happens; PulseCore turns it into DualSense
// effects. Non-blocking-friendly: if PulseCore isn't running it silently retries; writes are tiny.
//
// NOTE (Source SDK): include this from a .cpp AFTER the Source headers, or in a translation unit that
// tolerates <windows.h>. It pulls in <windows.h> with WIN32_LEAN_AND_MEAN.
#ifndef PULSECORE_CLIENT_H
#define PULSECORE_CLIENT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace pulsecore_client {

class PulseClient {
public:
    PulseClient() : pipe_(INVALID_HANDLE_VALUE) {}
    ~PulseClient() { Disconnect(); }

    PulseClient(const PulseClient&) = delete;
    PulseClient& operator=(const PulseClient&) = delete;

    // integration_id e.g. "community.hl2.enhanced"; game_id e.g. "steam:220".
    void Configure(const char* integration_id, const char* game_id, const char* version) {
        integration_id_ = integration_id ? integration_id : "";
        game_id_ = game_id ? game_id : "";
        version_ = version ? version : "1.0.0";
    }

    bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }

    // Call each frame (or on a timer). Handles (re)connecting with a cooldown so a missing PulseCore
    // never stalls the game. Returns true when connected.
    bool Poll(unsigned long now_ms) {
        if (connected()) return true;
        if (now_ms - last_attempt_ms_ < kReconnectCooldownMs && last_attempt_ms_ != 0) return false;
        last_attempt_ms_ = now_ms ? now_ms : 1;
        return TryConnect();
    }

    void Disconnect() {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            SendRaw("{\"type\":\"goodbye\"}");
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }

    // A state that persists until it changes (weapon.current, vehicle.current, ...). String value.
    void SetState(const char* name, const char* value) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "{\"type\":\"state\",\"name\":\"%s\",\"value\":\"%s\"}", name, EscOr(value));
        SendRaw(buf);
    }
    // A frequently-updated numeric value (vehicle.rpm, player.health, ...).
    void SetValue(const char* name, double value) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "{\"type\":\"value\",\"name\":\"%s\",\"value\":%.4g}", name, value);
        SendRaw(buf);
    }
    // A one-shot event (weapon.primary_fire, player.damaged, ...).
    void SendEvent(const char* name) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "{\"type\":\"event\",\"name\":\"%s\"}", name);
        SendRaw(buf);
    }
    void Heartbeat() { SendRaw("{\"type\":\"heartbeat\"}"); }

private:
    static const unsigned long kReconnectCooldownMs = 2000;

    // The escaper is deliberately minimal: game classnames/ids are ASCII with no quotes/backslashes.
    const char* EscOr(const char* v) {
        if (!v) return "";
        for (const char* p = v; *p; ++p) {
            if (*p == '"' || *p == '\\' || static_cast<unsigned char>(*p) < 0x20) return "";
        }
        return v;
    }

    bool TryConnect() {
        HANDLE h = CreateFileW(L"\\\\.\\pipe\\PulseCore.GameIntegration.v1",
                               GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;

        char hello[512];
        std::snprintf(hello, sizeof(hello),
                      "{\"type\":\"hello\",\"integration_id\":\"%s\",\"game_id\":\"%s\","
                      "\"integration_version\":\"%s\",\"protocol_min\":1,\"protocol_max\":1,"
                      "\"process_id\":%lu}",
                      integration_id_.c_str(), game_id_.c_str(), version_.c_str(),
                      static_cast<unsigned long>(GetCurrentProcessId()));
        std::string line = hello;
        line.push_back('\n');
        DWORD written = 0;
        if (!WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
            CloseHandle(h);
            return false;
        }
        // Read the welcome (best-effort; a byte-at-a-time read to the newline).
        char c = 0; DWORD rd = 0; std::string reply; int guard = 0;
        while (ReadFile(h, &c, 1, &rd, nullptr) && rd == 1 && c != '\n' && guard++ < 512) {
            if (c != '\r') reply.push_back(c);
        }
        if (reply.find("\"accepted\":true") == std::string::npos) {
            CloseHandle(h);
            return false;
        }
        pipe_ = h;
        return true;
    }

    void SendRaw(const char* json) {
        if (pipe_ == INVALID_HANDLE_VALUE) return;
        std::string line = json;
        line.push_back('\n');
        DWORD written = 0;
        if (!WriteFile(pipe_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;   // drop -> Poll() reconnects
        }
    }

    HANDLE pipe_;
    std::string integration_id_, game_id_, version_;
    unsigned long last_attempt_ms_ = 0;
};

}  // namespace pulsecore_client

#endif  // PULSECORE_CLIENT_H
