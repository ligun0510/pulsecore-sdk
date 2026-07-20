# PulseCore Game Integration SDK

Add **DualSense** adaptive-trigger and haptic effects to your game or mod — without writing a single line of HID code.

[PulseCore](https://store.steampowered.com/app/4980020) makes a Bluetooth DualSense present to Windows and games as a wired-USB DualSense (input, controller audio, mic, haptics, adaptive triggers). This repository is its **open integration layer**: a small, stable protocol and a client SDK that let a game mod report *what is happening in the game* — the current weapon, a shot fired, a vehicle's RPM, damage taken — over a local pipe. PulseCore's effects engine decides what the controller should *feel*.

> **Mods report WHAT happens; PulseCore decides what the DualSense feels.**
> Integrations never touch HID and never load into the PulseCore process — they only speak this protocol over a local named pipe. That keeps your mod simple and keeps both sides safe.

## What's in here

| Path | What |
|---|---|
| `protocol/` | **Bridge Protocol v1** — the wire format (newline-delimited JSON) + a tiny header-only JSON helper. No dependencies. |
| `sdk/` | **Client SDK** — `connect` / `set_state` / `send_event` / `set_value` / `disconnect`. A flat C ABI plus a C++ wrapper. |
| `examples/test-client/` | A minimal program that connects and sends a few messages. |
| `examples/hl2/` | The built-in **Half-Life 2 effect pack** (`effects.json`) — which game events map to which controller effects. |
| `mods/index.json` | The **catalogue** PulseCore's Mods tab reads, so the list of installable packs grows without a new build of the app. |
| `examples/hl2-source-mod/` | A full **Half-Life 2 (Source SDK 2013)** reference mod that reports weapon / vehicle / health events. |

The proprietary parts of PulseCore — the effects engine, the pipe server, the DualSense / USB-IP bridge — are **not** in this repository and are **not needed** to build an integration.

## Quick start (C++)

```cpp
#include "pulse/integration.h"

pulse::integration pc;
if (pc.connect({ "community.mygame", "steam:220", "1.0.0" })) {
    pc.set_state("weapon.current", "weapon_shotgun");  // persists until it changes
    pc.send_event("weapon.primary_fire");              // one-shot
    pc.set_value("vehicle.rpm", 3200.0);               // frequently-updated number
    pc.heartbeat();                                    // liveness, every ~2 s
    pc.disconnect();
}
```

There is also a flat **C ABI** (`Pulse_Connect`, `Pulse_SetState`, `Pulse_SendEvent`, …) so you can drive it from any language or loader — see [`sdk/include/pulse/integration.h`](sdk/include/pulse/integration.h).

An **effect pack** is the other half: a JSON file of rules deciding what those reports should *feel* like. Packs are pure data, installed with one click from PulseCore's Mods tab, and they update themselves when you publish a new version — the format and the versioning rules are in **[docs/effect-packs.md](docs/effect-packs.md)**.

For the full message model (Hello / Welcome / State / Event / Value / Heartbeat / Goodbye), the limits, and the pipe name, read **[docs/protocol.md](docs/protocol.md)**. The protocol is deliberately tiny and forward-compatible: unknown message types and unknown state/event/value names are always tolerated, never a hard error.

## Build

PulseCore is **Windows-only**, and the SDK talks over a Windows named pipe, so build on Windows (MSVC or clang). Requires CMake ≥ 3.20 and a C++20 compiler.

```
cmake -B build -S .
cmake --build build --config Release
```

This builds `pulse-protocol` + `pulse-sdk` (static libraries) and the `pulse-test-client` example. The Half-Life 2 mod is built separately inside a Source SDK 2013 tree — see [examples/hl2-source-mod/README.md](examples/hl2-source-mod/README.md).

## License — PolyForm Noncommercial 1.0.0

This repository is licensed under the **[PolyForm Noncommercial License 1.0.0](LICENSE.md)**. In short: you may use, modify, and share this code **for any noncommercial purpose** — personal projects, free mods, research, education.

- Building and sharing a **free** mod that integrates with PulseCore is noncommercial use — go for it.
- Want to use the SDK in a **commercial** product? A commercial license is available — email **ligun0510@gmail.com**.
- The **protocol itself is an open specification**: you are free to implement it independently, in any language, under any license. What's licensed here is *this repository's code*.

"PulseCore" and the PulseCore logo are trademarks of the PulseCore author. This license grants no trademark rights: you may say your mod is *"compatible with PulseCore,"* but you may not use the name or branding as your own.

## Not affiliated with Sony

PulseCore is an independent project — not affiliated with, endorsed by, or sponsored by Sony Interactive Entertainment. "DualSense" and "PlayStation" are trademarks of Sony, used here only to describe compatibility.

## Contributing

See **[CONTRIBUTING.md](CONTRIBUTING.md)**. In short: contributions are welcome under PolyForm Noncommercial 1.0.0, and — so the project can keep offering commercial licenses — you agree the maintainer may also license your contribution commercially.
