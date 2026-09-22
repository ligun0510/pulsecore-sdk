// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PulseCore Game Integration — vendored single-header client for a Source SDK 2013 mod.
//
// Self-contained (no other PulseCore SDK files needed): drop this one header into your mod, include it
// from ONE .cpp, and report game state/events. It speaks Bridge Protocol v1 over the local named pipe
// \\.\pipe\PulseCore.GameIntegration.v1. The mod reports WHAT happens; PulseCore turns it into DualSense
// effects. Every pipe operation is bounded by kIoTimeoutMs (250 ms) and the server's identity is
// checked before the handshake, so a missing, wedged or impostor PulseCore costs a hitch rather than
// hanging the calling thread. Earlier versions of this header used unbounded synchronous I/O and no
// identity check -- if you have a copy without ServerIsGenuine/WriteBounded, replace it.
//
// NOTE (Source SDK): include this from a .cpp AFTER the Source headers, or in a translation unit that
// tolerates <windows.h>. It pulls in <windows.h> with WIN32_LEAN_AND_MEAN.
#ifndef PULSECORE_CLIENT_H
#define PULSECORE_CLIENT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <clocale>   // localeconv: SetValue writes numbers in JSON's form whatever the game's locale
#include <cstdio>
#include <cstring>
#include <string>
#include <cwchar>

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
        char number[32];
        FormatNumberC(value, number, sizeof(number));
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "{\"type\":\"value\",\"name\":\"%s\",\"value\":%s}", name, number);
        SendRaw(buf);
    }
    // A number in JSON's form whatever the game's locale. "%.4g" honours LC_NUMERIC, so inside a game
    // that had called setlocale() with a comma-decimal locale it printed "0,5" -- invalid JSON, and the
    // server dropped the message. Kept to C and <clocale> on purpose: this header has to build with the
    // older compilers mods use, where <charconv> may not exist.
    static void FormatNumberC(double value, char* out, size_t size) {
        std::snprintf(out, size, "%.4g", value);
        const struct lconv* lc = localeconv();
        const char point = (lc && lc->decimal_point && *lc->decimal_point) ? *lc->decimal_point : '.';
        if (point != '.')
            for (char* p = out; *p; ++p) if (*p == point) *p = '.';
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
    // Every pipe operation is bounded by this. 250 ms is far beyond a healthy local round trip and
    // still under a single frame at 4 fps -- i.e. a wedged or impostor server costs a visible hitch,
    // never a hang.
    static const DWORD kIoTimeoutMs = 250;

    // The escaper is deliberately minimal: game classnames/ids are ASCII with no quotes/backslashes.
    const char* EscOr(const char* v) {
        if (!v) return "";
        for (const char* p = v; *p; ++p) {
            if (*p == '"' || *p == '\\' || static_cast<unsigned char>(*p) < 0x20) return "";
        }
        return v;
    }

    // Is the process on the other end of this pipe really PulseCore?
    //
    // A named pipe is a shared namespace: any process running as this user can create
    // \\.\pipe\PulseCore.GameIntegration.v1 first and receive everything the mod reports. The
    // published SDK client checks this (ServerLooksGenuine in sdk/src/integration.cpp) and explains at
    // length why; the vendored header that mods actually copy did not check at all. Same check here:
    // ask the kernel who owns the server end, then require its image to be pulsecore-core.exe.
    static bool ServerIsGenuine(HANDLE pipe) {
        ULONG serverPid = 0;
        if (!GetNamedPipeServerProcessId(pipe, &serverPid) || serverPid == 0) return false;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, serverPid);
        if (!proc) return false;
        wchar_t image[MAX_PATH] = {0};
        DWORD len = MAX_PATH;
        const BOOL ok = QueryFullProcessImageNameW(proc, 0, image, &len);
        CloseHandle(proc);
        if (!ok || len == 0) return false;
        const wchar_t* leaf = wcsrchr(image, L'\\');
        leaf = leaf ? leaf + 1 : image;
        return _wcsicmp(leaf, L"pulsecore-core.exe") == 0;
    }

    // Read one newline-terminated line with a WALL-CLOCK bound.
    //
    // This replaced a synchronous byte-at-a-time loop whose only guard was a 512-BYTE counter -- which
    // bounds how much can be read, not how long the wait can be. A server that accepts the connection
    // and then sends nothing blocked the caller forever, and that caller is the game's frame thread.
    // Overlapped I/O with a real timeout is the only way to bound it.
    static bool ReadLineBounded(HANDLE h, std::string& out, DWORD timeout_ms) {
        out.clear();
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ev) return false;
        const DWORD deadline = GetTickCount() + timeout_ms;
        bool ok = false;
        for (int guard = 0; guard < 512; ++guard) {
            OVERLAPPED ov;
            std::memset(&ov, 0, sizeof(ov));
            ov.hEvent = ev;
            ResetEvent(ev);
            char c = 0;
            DWORD rd = 0;
            if (!ReadFile(h, &c, 1, &rd, &ov)) {
                if (GetLastError() != ERROR_IO_PENDING) break;
                const DWORD now = GetTickCount();
                const DWORD left = (now >= deadline) ? 0 : (deadline - now);
                if (WaitForSingleObject(ev, left) != WAIT_OBJECT_0) {
                    // Cancel AND wait for the cancellation to land: returning here would leave the
                    // kernel writing into an OVERLAPPED that is about to go out of scope.
                    CancelIoEx(h, &ov);
                    DWORD ignored = 0;
                    GetOverlappedResult(h, &ov, &ignored, TRUE);
                    break;
                }
            }
            if (!GetOverlappedResult(h, &ov, &rd, FALSE) || rd != 1) break;
            if (c == '\n') { ok = true; break; }
            if (c != '\r') out.push_back(c);
        }
        CloseHandle(ev);
        return ok;
    }

    bool TryConnect() {
        // FILE_FLAG_OVERLAPPED so the welcome read below can actually time out. Without it every
        // ReadFile/WriteFile on this handle is synchronous and unbounded, on the game's frame thread.
        HANDLE h = CreateFileW(L"\\\\.\\pipe\\PulseCore.GameIntegration.v1",
                               GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        if (!ServerIsGenuine(h)) { CloseHandle(h); return false; }

        char hello[512];
        std::snprintf(hello, sizeof(hello),
                      "{\"type\":\"hello\",\"integration_id\":\"%s\",\"game_id\":\"%s\","
                      "\"integration_version\":\"%s\",\"protocol_min\":1,\"protocol_max\":1,"
                      "\"process_id\":%lu}",
                      integration_id_.c_str(), game_id_.c_str(), version_.c_str(),
                      static_cast<unsigned long>(GetCurrentProcessId()));
        std::string line = hello;
        line.push_back('\n');
        if (!WriteBounded(h, line, kIoTimeoutMs)) {
            CloseHandle(h);
            return false;
        }
        std::string reply;
        if (!ReadLineBounded(h, reply, kIoTimeoutMs) ||
            reply.find("\"accepted\":true") == std::string::npos) {
            CloseHandle(h);
            return false;
        }
        pipe_ = h;
        return true;
    }

    // Write with a wall-clock bound, for the same reason as ReadLineBounded: this runs per frame.
    // A server that stops reading fills the 4 KB pipe buffer, and an unbounded WriteFile then hangs
    // the game. On timeout the connection is treated as dead and Poll() reconnects.
    static bool WriteBounded(HANDLE h, const std::string& data, DWORD timeout_ms) {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ev) return false;
        OVERLAPPED ov;
        std::memset(&ov, 0, sizeof(ov));
        ov.hEvent = ev;
        DWORD written = 0;
        bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), nullptr, &ov) != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ev, timeout_ms) == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(h, &ov, &written, FALSE) != FALSE;
            } else {
                CancelIoEx(h, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(h, &ov, &ignored, TRUE);
                ok = false;
            }
        } else if (ok) {
            ok = GetOverlappedResult(h, &ov, &written, FALSE) != FALSE;
        }
        CloseHandle(ev);
        return ok && written == data.size();
    }

    void SendRaw(const char* json) {
        if (pipe_ == INVALID_HANDLE_VALUE) return;
        std::string line = json;
        line.push_back('\n');
        if (!WriteBounded(pipe_, line, kIoTimeoutMs)) {
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
