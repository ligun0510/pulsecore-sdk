# PulseCore Bridge Protocol v1

A tiny, stable protocol for reporting *what is happening in a game* to PulseCore, which turns it into
DualSense adaptive-trigger and haptic effects. An integration reports **state**, **events**, and
**values**; PulseCore's effects engine decides what the controller feels.

## Transport

- **Named pipe:** `\\.\pipe\PulseCore.GameIntegration.v1` (local-only — no TCP port, no firewall). The
  server can check the client's PID and detect disconnects.
- **Framing:** UTF-8, **newline-delimited JSON** — exactly one JSON object per line (`\n`).
- **Direction:** the integration is the client and connects to PulseCore (the server).

### Limits (guardrails; also advertised in `welcome`)

| Limit | Value |
|---|---|
| Max message size | 4096 bytes |
| Max string length | 512 chars |
| Max events / second | 240 |
| Heartbeat interval | 2000 ms |
| Disconnect timeout | 6000 ms (no traffic → server drops the session) |

## Session lifecycle

```
client ── hello ──▶ server
client ◀─ welcome ─ server      (accepted = true/false, session_id, limits)
client ── ready ──▶ server      (optional: "I finished initializing")
client ── state / event / value / heartbeat ──▶ server   (the steady state)
client ── goodbye ─▶ server     (clean shutdown; the pipe closing also ends the session)
```

## Forward compatibility (important)

Older builds **must tolerate** unknown message `type` tokens and unknown state/event/value `name`s —
they are carried through, never a hard error. This is how the protocol grows without breaking existing
integrations or existing PulseCore versions. Add fields compatibly; only a genuinely breaking change
bumps the version (and the pipe-name suffix `.v1`).

## Messages

Every message is a JSON object with at least `"protocol"` (integer, currently `1`) and `"type"`.

### `hello` — client → server

Opens a session and negotiates the protocol.

```json
{"protocol":1,"type":"hello","integration_id":"community.hl2.enhanced","game_id":"steam:220","integration_version":"1.0.0","protocol_min":1,"protocol_max":1,"process_id":12345,"capabilities":["weapons","vehicles"]}
```

| Field | Meaning |
|---|---|
| `integration_id` | Reverse-dot id of your integration, e.g. `community.hl2.enhanced`. |
| `game_id` | Game identifier, e.g. `steam:220`. |
| `integration_version` | Your integration's version string. |
| `protocol_min` / `protocol_max` | Protocol range you support (both `1` today). |
| `process_id` | Your process id. Optional, but if you send it, it must be yours: the server compares it with the process actually on the other end of the pipe and refuses a mismatch (`pid_mismatch`). |
| `capabilities` | Optional list of capability tags. |

### `welcome` — server → client

```json
{"protocol":1,"type":"welcome","accepted":true,"session_id":"…","max_message_size":4096,"heartbeat_interval_ms":2000,
 "server_features":["device.query","effect.trigger.preset","effect.trigger.primitive","effect.trigger.raw","effect.led","effect.rumble","effect.haptic","effect.speaker","telemetry.write"],
 "grants":["telemetry.write","device.query","effect.trigger.preset","effect.trigger.primitive","effect.trigger.raw","effect.led","effect.rumble","effect.haptic"],
 "limits":{"message_bytes":4096,"messages_per_second":240,"state_entries":256,"name_length":96,"effect_lease_ms_max":5000}}
```

If `accepted` is `false`, the server is rejecting the session (e.g. protocol mismatch). Otherwise use the
returned limits.

`server_features` says what this PulseCore build **can** do; `grants` says what **your session may**
use. They are separate on purpose: a client never grants itself anything. `effect.speaker` (sound in the
controller's speaker) is the one feature outside the default grants: it is granted only while the player
has turned on *Controller speaker* on PulseCore's Mods page — see [Sound](#sound-in-the-controllers-speaker).

`limits` are enforced, not advisory: names longer than `name_length` and states past `state_entries` are
dropped, and more than `messages_per_second` is throttled.

### `ready` — client → server

Optional. Signals your integration finished initializing.

```json
{"protocol":1,"type":"ready"}
```

### `state` — client → server

A value that **persists until it changes**. Send it on every change (and once on connect).

```json
{"protocol":1,"type":"state","name":"weapon.current","value":"weapon_shotgun"}
```

A state whose `value` is `null` **deletes** it — send that when the thing stops being true (weapon
holstered, vehicle left), or the effect it selected stays selected for the rest of the session.

```json
{"protocol":1,"type":"state","name":"weapon.current","value":null}
```

### `state.batch` — client → server

Several states applied **together**, so an initial load lands as one consistent picture instead of
flickering through half-updated combinations.

```json
{"protocol":1,"type":"state.batch","states":[{"name":"weapon.current","value":"weapon_smg1"},{"name":"vehicle.current","value":null}]}
```

### `event` — client → server

A **one-shot** occurrence. `value` is optional.

```json
{"protocol":1,"type":"event","name":"weapon.primary_fire"}
```

### `value` — client → server

A **frequently-updated number** (send at a sane rate — see the events/sec limit).

```json
{"protocol":1,"type":"value","name":"vehicle.rpm","value":3200}
```

### `heartbeat` — client → server

Liveness, every ~2 s while idle.

```json
{"protocol":1,"type":"heartbeat"}
```

### `error` — either direction

```json
{"protocol":1,"type":"error","error_code":"bad_effect","error_message":"strength must be 1..8","retryable":false}
```

Branch on `error_code`, never on `error_message` (human text that may change). `retryable` says whether
the same request can succeed later. The codes are stable — new ones may be added, none is repurposed:

| `error_code` | Meaning |
|---|---|
| `handshake_required` | the first message was not `hello` |
| `bad_integration_id` | `integration_id` missing or longer than 512 |
| `already_established` | a second `hello` on a live session |
| `pid_mismatch` | `hello.process_id` is not the process on the other end of the pipe |
| `malformed_message` | the line was not a JSON object |
| `message_too_large` | the line exceeded 4096 bytes |
| `bad_target` | unknown effect channel (the mic LED is never a mod's) |
| `bad_effect` | unknown schema, wrong shape or out-of-range value |
| `not_granted` | your session lacks the grant this request needs |
| `bad_clip` | a clip upload was refused (format, bounds, budget, offset, checksum) |
| `stream_busy` | another source holds the continuous audio stream — **retryable** |
| `busy` | another integration holds that trigger — **retryable** |
| `throttled` | the per-second rate cap was hit — **retryable** |

### `goodbye` — client → server

Clean shutdown. Closing the pipe also ends the session.

```json
{"protocol":1,"type":"goodbye"}
```

## Controller info — `device.query` / `device.snapshot`

```json
{"protocol":1,"type":"device.query","request_id":"q1"}
{"protocol":1,"type":"device.snapshot","reply_to":"q1","devices":[{"device_id":"primary","connected":true,"model":"dualsense","physical_transport":"bluetooth","game_transport":"virtual_usb","battery":{"percent":80,"charging":false},"features":{"adaptive_triggers":true,"lightbar_rgb":true,"player_led":true,"mic_led":true}}]}
```

`devices` is always an array. It carries no MAC address, serial or hardware id — a mod never needs
them. An unknown battery level is `null`, never a misleading `0`.

## Direct effects — `effect.apply` / `effect.release`

Rules in an [effect pack](effect-packs.md) are the easy path. When a mod needs exact control it drives a
channel directly, always under a **lease** (default 1000 ms, at most 5000 ms): re-send the same
`effect_id` to keep it, `effect.release` to drop it early. A mod that crashes or hangs therefore can
never leave the controller locked or buzzing.

```json
{"protocol":1,"type":"effect.apply","effect_id":"t1","target":{"device_id":"primary","channel":"right_trigger"},"effect":{"schema":"dualsense.trigger.preset.v1","preset":"heavy_shotgun"},"lease_ms":2000}
{"protocol":1,"type":"effect.release","effect_id":"t1"}
```

| `target.channel` | `effect.schema` | payload |
|---|---|---|
| `right_trigger` / `left_trigger` | `dualsense.trigger.preset.v1` | `{"preset":"heavy_shotgun"}` |
| | `dualsense.trigger.primitive.v1` | `{"kind":"feedback","position":2,"strength":6}` |
| | `dualsense.trigger.raw.v1` | `{"mode":33,"parameters":[10 numbers]}` |
| `lightbar` | `dualsense.lightbar.rgb.v1` | `{"r":0-255,"g":0-255,"b":0-255}` |
| `player_led` | `dualsense.player_led.v1` | `{"mask":0-31}` |
| `rumble` | `dualsense.rumble.v1` | `{"heavy":0-255,"light":0-255}` |
| `haptic` | `dualsense.haptic.pulse.v1` | `{"heavy":0-255,"light":0-255,"duration_ms":1-1000}` |
| `speaker` | `dualsense.speaker.cue.v1` | `{"shape":"click\|beep\|thump"[,"gain":0..1]}` |
| | `dualsense.speaker.clip.v1` | `{"clip":"<id>"[,"gain":0..1]}` |

`rumble` is a **level** held until released: heavy is the low-frequency motor in the left grip, light
the high-frequency one in the right, so an uneven pair expresses **direction**. It combines with the
game's own rumble by taking the stronger of the two, so a mod adds feedback and never silences the
game's. `haptic` is a one-shot whose duration bounds it (no lease). There is deliberately **no mic-LED
channel**: that light is a privacy indicator. Each surface has its own grant, so a host can allow
presets while refusing raw writes.

When two integrations want the same channel, the first one holds it for as long as its lease lasts. A
second request for a held **trigger** is refused with `busy` (retryable); on the other channels it is
currently dropped without an error, so do not treat "no error" as "applied". When yours ends — `goodbye`, a closed pipe, or six seconds of silence — everything it held
is released and PulseCore sends the controller one more report so nothing stays latched.

## Sound in the controller's speaker

Needs the `effect.speaker` grant, which is present only while the player has *Controller speaker*
turned on (off by default); without it every speaker request is refused with `not_granted`. Three
ways, all mixed into the speaker only — a game's own audio and its authored haptics keep priority:

**Built-in cues** — `dualsense.speaker.cue.v1` with `click`, `beep` or `thump`. At most one new cue
starts per 120 ms.

**Uploaded clips** — your own audio, in pieces:

```
clip.begin  {"clip_id":"reload","frames":N,"channels":1|2,"sample_rate":48000,"crc32":U}
clip.chunk  {"clip_id":"reload","offset":BYTES,"data":"<base64>"}      (offsets contiguous, ≤1 KB each)
clip.commit {"clip_id":"reload"}          →   clip.ready {"clip_id":"reload"}
```

48 kHz, signed 16-bit little-endian, mono or stereo; `crc32` is the reflected CRC-32 (0xEDB88320) of
the whole PCM. Bounds: 500 ms per clip, 8 clips and 256 KB per session. Ids starting `builtin:` are
reserved. Play a committed clip with `dualsense.speaker.clip.v1`. Clips are dropped when you disconnect.

**Continuous stream** — for audio that keeps coming. Send `audio.stream.open`; the reply
`audio.stream.ready` carries a single-use `stream_token` (valid 15 s), `channels`, `sample_rate` and
`max_frame_bytes`. Then connect to the **binary** pipe `\\.\pipe\PulseCore.GameAudio.v1` and send
frames `[u32 type][u32 length][payload]`, little-endian: first type 1 (hello: u32 version = 1,
u32 channels, u32 sample_rate, then the 32 hex characters of the token), then type 2 frames of
interleaved int16 PCM, at most 8192 bytes each. One stream at a time (`stream_busy`).

## Naming conventions

Use lowercase, dot-namespaced names so rule packs can match them predictably:

- **state** — `weapon.current`, `vehicle.current`, `player.health`
- **event** — `weapon.primary_fire`, `player.damaged`, `player.died`, `vehicle.entered`, `vehicle.exited`
- **value** — `vehicle.rpm`, `bow.draw`

Names are at most 96 characters. PulseCore ships rule packs keyed to these names (see
[`examples/hl2/effects.json`](../examples/hl2/effects.json)). Unknown names are ignored gracefully, so you
can introduce your own and ship a rule pack for them later — how to write one is in
[effect-packs.md](effect-packs.md).
