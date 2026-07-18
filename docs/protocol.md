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
| `process_id` | Your process id (lets the server detect a dead client). |
| `capabilities` | Optional list of capability tags. |

### `welcome` — server → client

```json
{"protocol":1,"type":"welcome","accepted":true,"session_id":"…","max_message_size":4096,"heartbeat_interval_ms":2000}
```

If `accepted` is `false`, the server is rejecting the session (e.g. protocol mismatch). Otherwise use the
returned limits.

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
{"protocol":1,"type":"error","error_code":"protocol","error_message":"unsupported protocol version"}
```

### `goodbye` — client → server

Clean shutdown. Closing the pipe also ends the session.

```json
{"protocol":1,"type":"goodbye"}
```

## Naming conventions

Use lowercase, dot-namespaced names so rule packs can match them predictably:

- **state** — `weapon.current`, `vehicle.current`, `player.health`
- **event** — `weapon.primary_fire`, `player.damaged`, `player.died`, `vehicle.entered`, `vehicle.exited`
- **value** — `vehicle.rpm`, `bow.draw`

PulseCore ships rule packs keyed to these names (see [`examples/hl2/effects.json`](../examples/hl2/effects.json)).
Unknown names are ignored gracefully, so you can introduce your own and ship a rule pack for them later.
