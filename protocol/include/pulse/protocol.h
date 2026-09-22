// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PulseCore Game Integration — Bridge Protocol v1 (envelope + message model).
//
// A small, stable core. An integration (game mod / plugin / external adapter) reports WHAT is happening
// in the game (state/event/value); the PulseCore Effects Engine decides what the DualSense should feel.
// The wire format is newline-delimited JSON (one message per line). Unknown message types and unknown
// state/event/value names must be tolerated by older builds — never a hard error. Part of the OPEN
// protocol layer. See docs for the full specification.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
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
// Per-session data-plane caps (enforced by the SessionRouter, not just advertised): the longest a
// state/event/value NAME may be, and the most DISTINCT state names one session may hold — so a hostile
// mod can neither send megabyte names nor grow the state store without bound. Over-limit data is
// dropped (bounded, never fatal).
inline constexpr int kMaxNameLength = 96;
inline constexpr int kMaxStateEntries = 256;

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
    DeviceQuery,    // client -> server: "what controllers are there, and what can they do?"
    DeviceSnapshot, // server -> client: reply to DeviceQuery (correlated by reply_to)
    EffectApply,    // client -> server: drive one effect DIRECTLY, under a lease
    EffectRelease,  // client -> server: drop a direct effect early
    StateBatch,     // client -> server: several states applied ATOMICALLY (one reconcile at the end)
    ClipBegin,      // client -> server: declare an audio clip about to be uploaded
    ClipChunk,      // client -> server: one bounded piece of that clip's PCM
    ClipCommit,     // client -> server: the upload is complete -- verify it and make it playable
    ClipReady,      // server -> client: the clip passed verification and may now be played
    StreamOpen,     // client -> server: ask for a token to open the continuous-audio pipe
    StreamReady,    // server -> client: the token, plus the format the audio pipe expects
};

// Direct effects are LEASED, never open-ended: a mod that crashes, hangs or forgets can otherwise leave
// a trigger locked. The lease is refreshed by re-applying; on expiry / release / disconnect the channel
// falls back to whatever the semantic rule layer selects.
inline constexpr int kDefaultLeaseMs = 1000;
inline constexpr int kMaxLeaseMs = 5000;

// Effect payload schemas carried in `effect.schema` (one shape per schema; never mixed):
//   dualsense.trigger.preset.v1    {"preset":"heavy_shotgun"}          -- our curated names
//   dualsense.trigger.primitive.v1 {"kind":"feedback","position":2,"strength":6[,"frequency":20]}
//   dualsense.trigger.raw.v1       {"mode":33,"parameters":[10 numbers]} -- full hardware access
inline constexpr char kSchemaTriggerPreset[] = "dualsense.trigger.preset.v1";
inline constexpr char kSchemaTriggerPrimitive[] = "dualsense.trigger.primitive.v1";
inline constexpr char kSchemaTriggerRaw[] = "dualsense.trigger.raw.v1";
// LED schemas (same effect.apply envelope, target.channel = "lightbar" | "player_led"):
//   dualsense.lightbar.rgb.v1 {"r":0-255,"g":0-255,"b":0-255}
//   dualsense.player_led.v1   {"mask":0-31}   -- the 5 player indicator LEDs, as a bitmask
// There is deliberately NO mic-LED schema: the mic light is a PRIVACY INDICATOR, not decoration, and a
// mod must never be able to show "mic off" while it is really on. device.query still REPORTS it.
inline constexpr char kSchemaLightbarRgb[] = "dualsense.lightbar.rgb.v1";
inline constexpr char kSchemaPlayerLed[] = "dualsense.player_led.v1";
// Sustained rumble (target.channel = "rumble"):
//   dualsense.rumble.v1 {"heavy":0-255,"light":0-255}
// A haptic EVENT decays on its own; this is a LEVEL a mod holds and moves -- an engine running, a lift
// humming, water, radiation rising, the tremor of holding something heavy. The two motors are separate
// because they are physically apart: heavy (low frequency) is the LEFT grip, light (high frequency) the
// RIGHT, so an uneven pair is how a mod expresses DIRECTION. Like every direct effect it is leased, and
// it MAXes with the game's own rumble rather than replacing it.
inline constexpr char kSchemaRumble[] = "dualsense.rumble.v1";
// One-shot haptic with an explicit shape (target.channel = "haptic"):
//   dualsense.haptic.pulse.v1 {"heavy":0-255,"light":0-255,"duration_ms":1-1000}
// The curated presets (weapon_recoil, impact_hard, ...) cover the common cases, but a great many
// feelings are a CONTINUOUS quantity rather than a name: an explosion scaled by distance, a crowbar
// landing on metal versus wood, a footstep on grating versus grass. Those want numbers, not a
// dictionary. Bounded in time so an "event" can never quietly become a state -- that is what the
// rumble channel is for, and it is leased precisely because it can.
inline constexpr char kSchemaHapticPulse[] = "dualsense.haptic.pulse.v1";

// A cue in the controller's speaker naming one of the BUILT-IN shapes plus a gain.
//
// This is the convenience path, not the limit of what the speaker can play -- see kSchemaSpeakerClip
// for a mod's own audio. It exists because most cues are a tick, an alert or a thud, and for those,
// naming the sound is less work for a mod author than uploading it, needs no upload state at all, and
// comes out already normalised against every other built-in.
inline constexpr char kSchemaSpeakerCue[] = "dualsense.speaker.cue.v1";

// The shapes the built-in path may name. A short list because it is a shortcut, not a vocabulary: a
// mod that needs a sound not in here uploads it rather than waiting for us to add a name.
inline constexpr char kCueClick[] = "click";   // a mechanical tick: UI confirm, reload clack
inline constexpr char kCueBeep[]  = "beep";    // a clean tone that cuts through a game mix
inline constexpr char kCueThump[] = "thump";   // a low impact, felt as much as heard
inline constexpr int kMaxHapticMs = 1000;

// Play a clip the mod UPLOADED (or a built-in referenced by id):
//   dualsense.speaker.clip.v1 {"clip":"<id>"[,"gain":0..1]}
//
// A new schema rather than a redefinition of speaker.cue.v1. The cue schema keeps meaning exactly what
// it has always meant; changing a published .v1 payload from {"shape"} to {"clip"} underneath the same
// name would be the one thing a version number exists to prevent.
inline constexpr char kSchemaSpeakerClip[] = "dualsense.speaker.clip.v1";

// Ids beginning with this refer to a built-in clip ("builtin:click") and are RESERVED: a session may
// not define one, so a mod can never replace the sound the app's own audition button plays.
inline constexpr char kBuiltinClipPrefix[] = "builtin:";

// ---- clip upload ------------------------------------------------------------------------------
//
// The correction this exists for: the speaker channel first shipped as three synthesised shapes and no
// way to send audio at all, on the reasoning that accepting a file would mean parsing a container
// inside a process with a realtime deadline. The first half of that is right and the second does not
// follow. Nothing here is parsed on the audio thread: a clip is received, bounded, assembled and
// verified while it is still just bytes, and only a finished block of PCM is ever handed to the mixer.
//
// It arrives in pieces because the transport bound is kMaxMessageSize (4096 bytes) and half a second of
// 48 kHz stereo is 96000 -- so "one message carrying the clip" was never possible, and pretending
// otherwise would have failed on the first real clip anybody sent.
//
//   clip.begin  {"clip_id":"reload","frames":N,"channels":1|2,"sample_rate":48000,"crc32":U}
//   clip.chunk  {"clip_id":"reload","offset":BYTES,"data":"<base64>"}
//   clip.commit {"clip_id":"reload"}
//   clip.ready  {"clip_id":"reload"}                    <- server, only after verification passes
//
// The FORMAT IS FIXED rather than negotiated: 48 kHz, signed 16-bit, little-endian, mono or stereo.
// 48 kHz because it is the pipeline's native rate and the pad's; anything else would need a resampler
// that only exists to accommodate a mod that could have resampled once, offline, instead of on every
// machine that runs it. Mono is accepted because most cues are mono and it halves the upload.
//
// Chunks carry an explicit byte OFFSET and must arrive contiguously. An out-of-order or gapped chunk is
// an error rather than a hole silently filled with silence: a clip assembled wrongly does not fail, it
// plays something the author never wrote.
inline constexpr int kClipSampleRate = 48000;
inline constexpr int kMaxClipChunkBytes = 1024;   // decoded; base64 of this is ~1368 chars, well inside
                                                 // kMaxMessageSize once the JSON envelope is counted
inline constexpr int kMaxClipFrames = 24000;      // 500 ms at 48 kHz. A cue, not a soundtrack: the
                                                  // continuous case is what the stream channel is for
inline constexpr int kMaxClipsPerSession = 8;
// Every byte a session has spent on clips, in flight or committed, counted together. This is the single
// bound that makes the whole upload path safe: it caps memory, and because a chunk is bounded too, it
// also caps how many clip messages a session can usefully send.
inline constexpr int kMaxClipBytesPerSession = 256 * 1024;

// CRC-32 (reflected, polynomial 0xEDB88320) of a clip's assembled PCM, checked at commit.
//
// Deliberately a checksum and not SHA-256, and the distinction is worth stating because it looks like a
// weakening: this verifies that the bytes we assembled are the bytes the mod meant to send -- a
// truncated upload, a chunk written at the wrong offset, an endianness mistake. It is NOT a security
// boundary and could not be one. The peer is a local process that already passed the pipe DACL and a
// PID check, and a hostile mod can compute a correct digest for hostile audio under any algorithm, so
// cryptographic strength here would buy exactly nothing while implying a guarantee we do not make.
std::uint32_t Crc32(const std::uint8_t* data, std::size_t length);

// ---- continuous audio stream ------------------------------------------------------------------
//
// A clip is a sound with an end. A STREAM is audio that keeps arriving -- a mod mixing its own
// soundtrack, a companion app, later something off a phone -- and the two need opposite handling. The
// cue rate limit (one accepted start per 120 ms) is right for the first and meaningless for the
// second; a bounded jitter buffer with underrun and overflow counters is right for the second and
// pointless for the first. So they are separate channels rather than one channel with a mode flag.
//
// It gets its OWN PIPE, carrying binary frames, because this one is not a control message. Continuous
// 48 kHz stereo is ~192 KB/s; through newline-delimited JSON it would be base64 (a third larger
// again), re-encoded and re-parsed on both sides, and every frame would allocate. The JSON channel
// stays what it is good at -- describing what happened -- and the audio goes down a pipe shaped like
// audio. The session that owns the stream is still established over the JSON channel: a client asks
// with audio.stream.open, gets a one-time token back, and presents that token on the audio pipe. The
// token is what binds the two connections; without it the audio pipe would have no idea whose
// permission, grant or ownership it is operating under.
inline constexpr wchar_t kAudioPipeName[] = L"\\\\.\\pipe\\PulseCore.GameAudio.v1";
inline constexpr int kAudioProtocolVersion = 1;
inline constexpr int kAudioTokenLength = 32;      // hex characters
inline constexpr int kAudioFrameHello = 1;        // payload: version, channels, sample_rate, token
inline constexpr int kAudioFramePcm = 2;          // payload: interleaved int16 little-endian
// The biggest PCM payload one frame may carry: 8 KB is ~21 ms of 48 kHz stereo. Large enough that a
// producer is not making a syscall per millisecond, small enough that a hostile length field cannot
// make us allocate anything interesting.
inline constexpr int kMaxAudioFrameBytes = 8192;

// Decode standard base64 (with or without padding). Returns false on any character outside the
// alphabet or a length that cannot be a valid encoding -- never a partial result.

// Decode standard base64 (with or without padding). Returns false on any character outside the
// alphabet or a length that cannot be a valid encoding -- never a partial result.
bool DecodeBase64(std::string_view text, std::vector<std::uint8_t>& out);

// Capability names. `server_features` says what THIS BUILD can do (so a client can adapt instead of
// guessing from a version number); `grants` says what THIS SESSION is allowed to use. They are separate
// on purpose: a client never assigns itself permission — the server decides and tells it.
inline constexpr char kFeatureDeviceQuery[] = "device.query";
inline constexpr char kFeatureTriggerPreset[] = "effect.trigger.preset";
inline constexpr char kFeatureTriggerPrimitive[] = "effect.trigger.primitive";
inline constexpr char kFeatureTriggerRaw[] = "effect.trigger.raw";
inline constexpr char kFeatureLed[] = "effect.led";
inline constexpr char kFeatureRumble[] = "effect.rumble";
inline constexpr char kFeatureHaptic[] = "effect.haptic";
// The pad's own speaker. A SEPARATE grant from every other effect, deliberately: haptics and triggers
// are felt by the person holding the controller, and a sound is heard by everyone in the room. A host
// that is happy to let a mod shape triggers may reasonably refuse to let it make noise, and the player
// may be wearing headphones for a reason.
inline constexpr char kFeatureSpeaker[] = "effect.speaker";
inline constexpr char kFeatureTelemetry[] = "telemetry.write";

// One controller as reported to an integration. Deliberately carries NO MAC address, serial number or
// hardware instance id: a mod never needs them, and they are stable cross-app identifiers we should not
// hand out. `battery_known == false` means "unknown" and is serialized as null -- never a misleading 0.
struct DeviceInfo {
    std::string device_id = "primary";           // stable within a session; NOT a hardware id
    bool connected = false;
    std::string model = "dualsense";
    std::string physical_transport = "none";     // how PulseCore reaches it: bluetooth | usb | none
    std::string game_transport = "virtual_usb";  // how the GAME sees it
    bool battery_known = false;
    int battery_percent = 0;
    bool charging = false;
    bool adaptive_triggers = true;
    bool lightbar_rgb = true;
    bool player_led = true;
    bool mic_led = true;
};

std::string ToString(MsgType type);
MsgType ParseType(std::string_view token);

// One decoded protocol message. Only the fields relevant to `type` are populated; the rest stay at
// their defaults. `value` holds the state/value payload (string or number or bool).
struct Message {
    int protocol = kProtocolVersion;
    MsgType type = MsgType::Unknown;
    std::string type_raw;   // original token (preserved for MsgType::Unknown)

    // state / event / value. A `state` whose value is NULL DELETES that entry: without it a state set
    // once would live for the whole session and keep selecting effects forever.
    std::string name;
    json::Value value;

    // state.batch: name/value pairs applied together, so a mod's initial load lands as ONE consistent
    // picture instead of flickering through intermediate combinations as each state arrives.
    std::vector<std::pair<std::string, json::Value>> batch;

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
    std::vector<std::string> server_features;   // what this build supports
    std::vector<std::string> grants;            // what THIS session may use (server decides, not client)
    int max_events_per_second = 0;
    int max_state_entries = 0;
    int max_name_length = 0;
    int max_lease_ms = 0;

    // error. `error_code` is the STABLE machine-readable token a client branches on (error_message is
    // human text and may change freely). `retryable` says whether the same request could succeed later:
    // a throttled request can be resent, a malformed effect never can.
    std::string error_code;
    std::string error_message;
    bool retryable = false;

    // request/reply correlation: the client stamps request_id, the server echoes it as reply_to, so a
    // client with several outstanding queries can match each answer to its question.
    std::string request_id;
    std::string reply_to;

    // device.snapshot payload (always an ARRAY, even with one pad, so multi-controller needs no new
    // message shape later)
    std::vector<DeviceInfo> devices;

    // effect.apply / effect.release. `effect` is the schema-tagged payload (see kSchema* above); it is
    // kept as raw JSON so a new schema needs no new protocol field. lease_ms 0 => server default.
    std::string effect_id;
    std::string target_device_id;   // "" => the primary pad
    std::string target_channel;     // "left_trigger" | "right_trigger"
    json::Value effect;
    int lease_ms = 0;

    // clip.begin / clip.chunk / clip.commit / clip.ready. `clip_data` holds the DECODED bytes of a
    // chunk: base64 is a transport detail of the JSON line, and leaving it encoded any further would
    // just mean every consumer decoding it again.
    // audio.stream.open / audio.stream.ready. The token is single-use and belongs to ONE session --
    // it is the only thing tying a connection on the audio pipe back to a permission that was granted
    // on the JSON one.
    std::string stream_token;

    std::string clip_id;
    int clip_frames = 0;
    int clip_channels = 0;
    int clip_sample_rate = 0;
    unsigned int clip_crc32 = 0;
    int clip_offset = 0;
    std::vector<std::uint8_t> clip_data;
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
Message MakeWelcome(bool accepted, std::string session_id, std::vector<std::string> grants = {});
Message MakeState(std::string name, json::Value value);
Message MakeEvent(std::string name, json::Value value = {});
// Stable error codes (the client-facing contract; new codes may be ADDED, never repurposed):
//   handshake_required   first message was not hello
//   bad_integration_id   missing / oversized integration_id
//   already_established  duplicate hello on a live session
//   pid_mismatch         hello claimed a PID that is not the connected peer
//   malformed_message    the line was not decodable JSON / not an object
//   message_too_large    the line exceeded the transport limit
//   bad_target           unknown effect target channel (includes the mic LED, which is never a mod's)
//   bad_effect           unknown schema / bad shape / out-of-range value
//   not_granted          the session lacks the capability this request needs
//   bad_clip             a clip upload was refused (format, bounds, budget, offset, checksum)
//   stream_busy          another source already holds the continuous audio stream -- RETRYABLE
//   throttled            the per-second rate cap was hit -- RETRYABLE
Message MakeError(std::string code, std::string message, bool retryable = false);
Message MakeDeviceQuery(std::string request_id);
Message MakeDeviceSnapshot(std::string reply_to, std::vector<DeviceInfo> devices);

}  // namespace pulse::proto
