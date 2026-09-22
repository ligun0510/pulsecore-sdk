# Half-Life 2 — PulseCore Enhanced (Source SDK 2013 reference integration)

A Half-Life 2 mod that reports semantic game events (weapon changed, fired, entered a vehicle, took
damage) to PulseCore over the local Bridge pipe. PulseCore turns them into DualSense adaptive-trigger /
haptic effects. This is Phase 3 — the first **reference integration** proving the whole chain with a real
game. The mod reports WHAT happens; it never touches HID or Half-Life 2's own files.

> **Builds clean** against an SDK 2013 singleplayer tree (2026-07-21, Mapbase, VS 2022 v142 toolset,
> Win32/Release) — both `client.dll` and `server.dll`. See [Build](#build) for the exact steps and the
> three toolchain workarounds Valve's own code needs on a modern compiler.
>
> **Not yet play-tested in-game.** Compiling proves the API calls exist; it does not prove the effects
> feel right. Anything still marked `// [VERIFY]` is a spot where the code compiles but the behaviour
> has not been watched happen.

## What's here

- `vendor/pulsecore_client.h` — self-contained MIT client (named-pipe, Bridge Protocol v1). No other
  PulseCore SDK files are needed.
- `src/pulsecore_hl2_integration.cpp` — a `CAutoGameSystemPerFrame` that polls the local player and emits
  `weapon.current`, `weapon.primary_fire`, `vehicle.current` + `vehicle.entered/exited`, `player.health` +
  `player.damaged/died`. Self-registers; no other mod file needs editing.
- `mod/gameinfo.txt` — the Source mod skeleton (Source SDK Base 2013 Singleplayer + HL2 content).

## What it reports (verified against the Source SDK 2013 source, 2026-07-20)

**Half-Life 2's own rumble bus** (`pulsecore_hl2_rumble.cpp`) — the highest-value hook by far. The game
already sends a semantic `Rumble` user message (`shared/rumble_shared.h`) naming what should be felt:
per-weapon fire, the crowbar swing, falls by height, damage by severity, the airboat gun, the jeep
engine loop, the gravity gun. One hook forwards all of it as `hl2.rumble.<name>` events plus the game's
own 0-100 scale, then hands the message to the stock `RumbleEffect()` so ordinary XInput rumble is
unaffected. Valve already tuned these; the mod only relays them.

**Client-side player state** (`pulsecore_hl2_integration.cpp`) — the ground material underfoot (real
surface properties from the movement code, not a guess from the sound), water / shallow water / ladder,
landing with the speed it happened at, HEV auxiliary power, vehicle speed, weapon, health, vehicle
enter/exit.

### Deliberately absent from the rule pack

The reference `effects.json` contains **only rules that can actually fire**. Rules for signals nothing
emits are worse than no rules: they read as working features, and this pack is what mod authors copy.
These are written up here instead, with what each one is waiting for:

| Feeling | Waiting on |
|---|---|
| Damage from the left / right / behind | the two-line `hud_damageindicator.cpp` edit below |
| Crowbar on metal vs wood vs glass | an emitter for impact effects (`CEffectData::m_nSurfaceProp` is client-visible, so this is reachable — it just is not written) |
| Radiation building up | an emitter, and envelopes in the SDK for a smooth ramp |

### Not implemented, and why

- **Damage DIRECTION** — the `Damage` user message carries it (with the world position of the source),
  but the stock `CHudDamageIndicator` already hooks that message and a second hook would replace it,
  silently breaking the on-screen damage indicator. Doing this properly means a two-line call added to
  `hud_damageindicator.cpp::MsgFunc_Damage`, which is a legitimate mod edit but not a drop-in file.
  Until then, severity comes from the rumble bus (`hl2.rumble.damage_low/medium/high`).
- **Naming every nearby entity** — see below.
- **Naming every nearby entity ("director haptics")** — `GetClassname()` on the client returns a
  predicted-classmap name or a C++ type name, not the server classname, so it would yield noise. Only
  the strider, gunship, manhack and rollermine have their own client classes; anything else needs the
  model name or a list replicated from the server.
- **Trigger resistance scaled BY the held object's mass** — the mod now *sends* the mass, but rules match
  on equality, so effects.json cannot yet read it as a continuous quantity. Needs value-driven rules.

### Server side (`pulsecore_hl2_server.cpp`)

Three things the client genuinely cannot see, so they live in `server.dll`: the **gravity gun's held
object and its mass** (the client is never sent the held entity and has no physics object for server
props), the **per-shell shotgun reload** (`m_bInReload` is not networked in singleplayer — the clip
count is used, since it moves once per shell actually loaded), and **grenades**.

It does **not** open its own PulseCore connection. In singleplayer the client and server are two modules
of one process, so a second connection would be a second integration competing for the same effect
channels. Instead it sends one `PulseFx` user message (name + number) that `client.dll` forwards over the
single existing session.

**Required one-line edit:** add `usermessages->Register( "PulseFx", -1 );` to `RegisterUserMessages()` in
`src/game/shared/hl2/hl2_usermessages.cpp`. Registration happens once, in shared code, before either
module's game systems start, so it cannot be done from the mod's own files.

The event names match PulseCore's built-in HL2 rule pack (`integration/examples/hl2/effects.json`), e.g.
`weapon_shotgun → heavy_shotgun` trigger, `airboat → accelerator/brake`.

## Prerequisites

- Steam with **Half-Life 2** (appid 220) and **Source SDK Base 2013 Singleplayer** (appid 243730) installed.
- Visual Studio. VS 2013 is Valve's official toolset; VS 2019/2022 works with the community fixes to
  `source-sdk-2013` (search "source-sdk-2013 VS2022"). 
- `git`.

## Build

**This has been built** (2026-07-21, Mapbase singleplayer tree, VS 2022 v142 toolset, Win32/Release).
The steps below are what actually worked, including the three toolchain workarounds — none of them are
optional, and none are caused by this mod.

1. Clone an SDK 2013 singleplayer tree — either Valve's `source-sdk-2013` or a fork such as Mapbase.
   Layout note: in Valve's current `master` the old `sp/src/...` paths are gone and the code lives in
   `src/game/{client,server,shared}`; Mapbase still uses `sp/src/...`.
2. Copy the files in:
   - `src/pulsecore_hl2_integration.cpp`, `src/pulsecore_hl2_rumble.cpp` → `game/client/`
   - `src/pulsecore_hl2_server.cpp` → `game/server/`
   - `vendor/pulsecore_client.h` → **both** `game/client/` and `game/server/`
3. Register them in the VPC scripts (`$File "…"` next to the neighbouring entries) in
   `game/client/client_hl2.vpc` and `game/server/server_hl2.vpc`, then regenerate:
   ```
   devtools\bin\vpc.exe /hl2 /episodic +game /mksln games.sln
   ```
   **Editing the generated `.vcxproj` alone does not work** — VPC stamps a CRC of the `.vpc` into the
   project and a pre-build step refuses to compile when they disagree. (Writing `games.sln` may fail
   with "Unable to find RegKey" without VS 2013 installed; the `.vcxproj` files are still regenerated,
   and building them directly is enough.)
4. Register the mod's user message. Add one line to `RegisterUserMessages()` in
   `game/shared/hl2/hl2_usermessages.cpp` — **at the very end of the function, after every stock
   registration**:
   ```cpp
   usermessages->Register( "PulseFx", -1 );
   ```
   Without it the server side compiles and then silently sends nothing: registration happens once, in
   shared code, and cannot be done from either module.

   **The position is not cosmetic.** User messages are addressed by INDEX, in registration order.
   Inserting one in the middle renumbers every message after it, while the engine and the game's other
   modules keep counting the old way — so they start reading the wrong message entirely. In the
   anniversary build of Half-Life 2 that showed up as half the controller dying (movement and the
   D-pad gone, camera and triggers fine) and the `ai_disable` cheat switching itself on, with no error
   anywhere. Appending changes nobody's index.
5. Build Release/Win32 for `client_hl2.vcxproj` **and** `server_hl2.vcxproj`:
   ```
   msbuild game\client\client_hl2.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v142
   ```

### The three workarounds a modern toolset needs

Valve's code was written for VS 2013 and its prebuilt libraries ship against that runtime. Building it
with anything newer needs:

| Symptom | Why | Fix |
|---|---|---|
| `error C2220` on `fx.cpp`, `c_te_legacytempents.cpp`, … | `/WX` plus warnings that did not exist in 2013 (C4838 narrowing, C4456/C4458 shadowing) — all in Valve's own code | `set _CL_=/WX-` for the build. Do **not** patch Valve's sources for warnings. |
| `LNK2019: unresolved _sscanf_s` from `dmxloader.lib` | the prebuilt lib expects the old CRT, where `sscanf_s` was a real export; it is inline now | `set LINK=legacy_stdio_definitions.lib` |
| Everything suddenly recompiles and fails after an incremental build worked | regenerating the projects invalidates the object files, so pre-existing errors in Valve's code surface for the first time | expected — apply the two above |

## Install & run

1. Create `…/Steam/steamapps/sourcemods/PulseCore_HL2/` and copy `mod/gameinfo.txt` into it.
2. Copy the built `client.dll` (+ `server.dll`) into `PulseCore_HL2/bin/`.
3. Restart Steam → **"Half-Life 2 — PulseCore Enhanced"** appears in your library.
4. Start **PulseCore** (so the Bridge pipe is listening) and connect your DualSense. For raw debugging you
   can instead run `build/integration/Release/pulse-dev-console.exe` to watch the event stream.
5. Launch the mod from Steam. Switch weapons / drive the airboat / take damage → the adaptive triggers
   change on the pad. In PulseCore's state, `bridgeconn` = 1 and `bridgetrig` climbs.

## First-build checklist (the `// [VERIFY]` spots)

- `GetActiveWeapon()` / `GetClassname()` on the local player + weapon.
- `m_nButtons & IN_ATTACK` for the fire edge (some forks expose buttons differently).
- Vehicle access: `IsInAVehicle()` / `GetVehicle()` / `IClientVehicle::GetVehicleEnt()` and the
  `iclientvehicle.h` include path.
- `GetHealth()` on the client player.

If a name differs, each poll is a self-contained block — fix the one line and rebuild.

## Licensing

The mod is a **Source SDK 2013** derivative: distributing the built binaries is governed by **Valve's
Source 1 SDK License** — a non-commercial mod for a Source game (you may not sell it, and players need
HL2). PulseCore's glue here (`pulsecore_hl2_integration.cpp`, `vendor/pulsecore_client.h`) is MIT. Keep
this example in its own tree — do **not** merge Source-SDK-derived code into PulseCore's proprietary
core. This mirrors the chain-of-title boundary the rest of the project maintains.
