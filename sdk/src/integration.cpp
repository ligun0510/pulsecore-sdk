// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "pulse/integration.h"

#include <new>
#include <string>

#include "pulse/protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace pulse {

namespace {

// Wall-clock bounds for every pipe operation. A mod runs inside somebody's game; nothing here may
// block its thread indefinitely, whatever PulseCore is doing.
constexpr DWORD kIoTimeoutMs = 250;

// Wait for an overlapped operation with a deadline, cancelling it if the deadline passes.
//
// The cancel is followed by a BLOCKING GetOverlappedResult on purpose: returning straight after
// CancelIoEx would leave the kernel writing into an OVERLAPPED (and a buffer) that is about to go out
// of scope.
bool AwaitOverlapped(HANDLE pipe, OVERLAPPED& ov, DWORD timeout_ms, DWORD& transferred) {
    if (WaitForSingleObject(ov.hEvent, timeout_ms) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        DWORD ignored = 0;
        GetOverlappedResult(pipe, &ov, &ignored, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &ov, &transferred, FALSE) != FALSE;
}

bool WriteLine(HANDLE pipe, const std::string& text) {
    // Bounded, like the read. An unbounded WriteFile blocks once the pipe's buffer fills, which is
    // exactly what a stalled core produces -- and disconnect() then joins the worker sitting in it.
    std::string line = text;
    line.push_back('\n');

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    DWORD written = 0;
    bool ok = WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, &ov) != FALSE;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = AwaitOverlapped(pipe, ov, kIoTimeoutMs, written);
    }
    CloseHandle(ov.hEvent);
    return ok && written == line.size();
}

// Read one newline-terminated line with a WALL-CLOCK bound.
//
// This was a synchronous byte-at-a-time loop whose only guard was a SIZE counter -- which bounds how
// much may be read, not how long the wait may be. A server that accepts the connection and then sends
// nothing blocked the caller forever, and that caller is the game's thread during level load. The
// vendored single-header client that mods copy was fixed for precisely this; the OFFICIAL SDK -- the
// one the platform hands to the community -- still had the original defect.
bool ReadLine(HANDLE pipe, std::string& out, DWORD timeout_ms = kIoTimeoutMs) {
    out.clear();
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;

    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    bool ok = false;
    for (;;) {
        if (out.size() > static_cast<std::size_t>(proto::kMaxMessageSize)) break;

        OVERLAPPED ov{};
        ov.hEvent = ev;
        ResetEvent(ev);

        char c = 0;
        DWORD read = 0;
        if (!ReadFile(pipe, &c, 1, &read, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            const ULONGLONG now = GetTickCount64();
            const DWORD left = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
            if (!AwaitOverlapped(pipe, ov, left, read)) break;
        }
        if (read != 1) break;
        if (c == '\n') { ok = true; break; }
        if (c != '\r') out.push_back(c);
    }
    CloseHandle(ev);
    return ok;
}

// Is the process serving this pipe actually pulsecore-core.exe?
//
// The SDK opened the pipe and went straight into the hello/welcome handshake without ever asking who was
// on the other end -- while the CORE's own control-pipe client (CoreClient) does exactly this check for
// exactly this reason. Any process running as the same user can create PulseCore.GameIntegration.v1
// before the core does; every mod then handshakes with the squatter and streams it the game's semantic
// feed (weapon changes, fire events, vehicle state) instead. The squatter cannot drive the pad, so this
// is telemetry redirection and availability loss rather than an effect-injection hole -- but a mod
// should not be talking to it either way.
//
// Checked by IMAGE NAME rather than full path on purpose: the SDK is compiled into third-party mods that
// have no idea where PulseCore is installed (Steam library, %ProgramFiles%\PulseCore\Service, a dev
// tree), so a path allow-list would reject the genuine server on most machines. The name is what a
// squatter cannot spoof without itself being a process named pulsecore-core.exe.
bool ServerLooksGenuine(HANDLE pipe) {
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &pid) || pid == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t image[MAX_PATH]{};
    DWORD length = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, image, &length);
    CloseHandle(process);
    if (!ok) return false;
    const wchar_t* slash = wcsrchr(image, L'\\');
    const wchar_t* name = slash ? slash + 1 : image;
    return _wcsicmp(name, L"pulsecore-core.exe") == 0;
}

}  // namespace

integration::~integration() { disconnect(); }

bool integration::connect(const IntegrationConfig& config) {
    disconnect();

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // FILE_FLAG_OVERLAPPED is what makes every timeout above possible. Without it each
        // ReadFile/WriteFile on this handle is synchronous and unbounded no matter what the callers
        // do, on a thread belonging to somebody's game.
        pipe = CreateFileW(proto::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return false;
        if (!WaitNamedPipeW(proto::kPipeName, 2000)) return false;
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;

    // Verify the server BEFORE sending anything: the hello carries the game id and the mod's identity,
    // and everything after it is the game's live semantic state.
    if (!ServerLooksGenuine(pipe)) {
        CloseHandle(pipe);
        return false;
    }

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
    connected_.store(true);
    stop_.store(false);
    worker_ = std::thread(&integration::WorkerLoop, this);
    return true;
}

void integration::disconnect() {
    const bool was = connected_.exchange(false);
    stop_.store(true);
    cv_.notify_all();
    // Cancel whatever the worker has in flight BEFORE joining it. Every pipe operation is bounded now,
    // so the join is bounded too -- but cancelling first turns "up to a timeout" into "immediately",
    // and disconnect() is typically called while a game is shutting down.
    if (HANDLE inFlight = static_cast<HANDLE>(pipe_)) CancelIoEx(inFlight, nullptr);
    if (worker_.joinable()) worker_.join();   // always join, even if the worker already died on a write

    HANDLE pipe = static_cast<HANDLE>(pipe_);
    if (pipe) {
        if (was) {   // best-effort clean shutdown so the server releases our effects immediately
            proto::Message bye;
            bye.type = proto::MsgType::Goodbye;
            WriteLine(pipe, proto::SerializeMessage(bye));
        }
        CloseHandle(pipe);
    }
    pipe_ = nullptr;
    std::lock_guard<std::mutex> lock(m_);
    queue_.clear();
}

bool integration::connected() const { return connected_.load(); }

bool integration::Enqueue(std::string line, const std::string& coalesce_key) {
    if (!connected_.load()) return false;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!coalesce_key.empty()) {
            // A newer state/value for the same name supersedes the one still waiting to be written, so a
            // per-frame burst collapses to one message instead of overflowing the queue.
            for (Queued& queued : queue_) {
                if (queued.coalesce_key == coalesce_key) {
                    queued.line = std::move(line);
                    cv_.notify_one();
                    return true;
                }
            }
        }
        if (queue_.size() >= kMaxQueued) {   // full: drop rather than block the caller
            drops_.fetch_add(1);
            return false;
        }
        queue_.push_back(Queued{std::move(line), coalesce_key});
    }
    cv_.notify_one();
    return true;
}

void integration::WorkerLoop() {
    for (;;) {
        Queued item;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });
            if (queue_.empty()) {
                if (stop_.load()) return;   // drained and asked to stop
                continue;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        if (!WriteLine(static_cast<HANDLE>(pipe_), item.line)) {
            connected_.store(false);   // the pipe died; stop writing (disconnect() still joins us)
            return;
        }
    }
}

bool integration::set_state(const std::string& name, const std::string& value) {
    return Enqueue(proto::SerializeMessage(proto::MakeState(name, json::Value(value))), "s:" + name);
}

bool integration::set_value(const std::string& name, double value) {
    proto::Message m = proto::MakeState(name, json::Value(value));
    m.type = proto::MsgType::Value;
    return Enqueue(proto::SerializeMessage(m), "s:" + name);
}

bool integration::set_bool(const std::string& name, bool value) {
    return Enqueue(proto::SerializeMessage(proto::MakeState(name, json::Value(value))), "s:" + name);
}

bool integration::send_event(const std::string& name) {
    return Enqueue(proto::SerializeMessage(proto::MakeEvent(name)), "");   // events never coalesce
}

bool integration::heartbeat() {
    proto::Message m;
    m.type = proto::MsgType::Heartbeat;
    return Enqueue(proto::SerializeMessage(m), "hb");   // only the newest heartbeat matters
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

int Pulse_ConnectEx(const PulseConfigV2* config) {
    // Validate the ABI envelope BEFORE touching the transport: the struct must be at least the base
    // size we require (so all base fields are really present) and carry a non-zero ABI version.
    if (!config || config->struct_size < sizeof(PulseConfigV2) || config->abi_version == 0) return 0;
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

// ---- Handle-based sessions (each owns an independent client + its own async queue) ----
struct PulseSession {
    pulse::integration impl;
};

int Pulse_SessionCreate(const PulseConfigV2* config, PulseSession** out_session) {
    if (!out_session) return 0;
    *out_session = nullptr;
    if (!config || config->struct_size < sizeof(PulseConfigV2) || config->abi_version == 0) return 0;

    PulseSession* session = new (std::nothrow) PulseSession();
    if (!session) return 0;
    pulse::IntegrationConfig cfg;
    if (config->integration_id) cfg.integration_id = config->integration_id;
    if (config->game_id) cfg.game_id = config->game_id;
    if (config->version) cfg.version = config->version;
    if (!session->impl.connect(cfg)) {
        delete session;
        return 0;
    }
    *out_session = session;
    return 1;
}

void Pulse_SessionDestroy(PulseSession* session) {
    if (!session) return;
    session->impl.disconnect();   // joins the worker and sends goodbye
    delete session;
}

int Pulse_SessionIsConnected(PulseSession* session) {
    return (session && session->impl.connected()) ? 1 : 0;
}

int Pulse_SessionSetState(PulseSession* session, const char* name, const char* value) {
    return (session && name && value && session->impl.set_state(name, value)) ? 1 : 0;
}
int Pulse_SessionSetInteger(PulseSession* session, const char* name, long long value) {
    return (session && name && session->impl.set_value(name, static_cast<double>(value))) ? 1 : 0;
}
int Pulse_SessionSetFloat(PulseSession* session, const char* name, double value) {
    return (session && name && session->impl.set_value(name, value)) ? 1 : 0;
}
int Pulse_SessionSetBool(PulseSession* session, const char* name, int value) {
    return (session && name && session->impl.set_bool(name, value != 0)) ? 1 : 0;
}
int Pulse_SessionSendEvent(PulseSession* session, const char* name) {
    return (session && name && session->impl.send_event(name)) ? 1 : 0;
}
int Pulse_SessionHeartbeat(PulseSession* session) {
    return (session && session->impl.heartbeat()) ? 1 : 0;
}
unsigned long long Pulse_SessionDrops(PulseSession* session) {
    return session ? session->impl.drops() : 0ull;
}

}  // extern "C"
