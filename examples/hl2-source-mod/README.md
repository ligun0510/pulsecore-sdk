# Half-Life 2 — PulseCore Enhanced (Source SDK 2013 reference integration)

A Half-Life 2 mod that reports semantic game events (weapon changed, fired, entered a vehicle, took
damage) to PulseCore over the local Bridge pipe. PulseCore turns them into DualSense adaptive-trigger /
haptic effects. This is Phase 3 — the first **reference integration** proving the whole chain with a real
game. The mod reports WHAT happens; it never touches HID or Half-Life 2's own files.

> ⚠️ **Not built or tested in the PulseCore repo** — there is no Source SDK in the PulseCore build
> environment, so this code is written against the well-known Source SDK 2013 client API but is
> **unverified**. Expect to fix a few include paths / accessor names on your first compile; the spots
> most likely to need a touch are flagged `// [VERIFY]` in `src/pulsecore_hl2_integration.cpp`.

## What's here

- `vendor/pulsecore_client.h` — self-contained MIT client (named-pipe, Bridge Protocol v1). No other
  PulseCore SDK files are needed.
- `src/pulsecore_hl2_integration.cpp` — a `CAutoGameSystemPerFrame` that polls the local player and emits
  `weapon.current`, `weapon.primary_fire`, `vehicle.current` + `vehicle.entered/exited`, `player.health` +
  `player.damaged/died`. Self-registers; no other mod file needs editing.
- `mod/gameinfo.txt` — the Source mod skeleton (Source SDK Base 2013 Singleplayer + HL2 content).

The event names match PulseCore's built-in HL2 rule pack (`integration/examples/hl2/effects.json`), e.g.
`weapon_shotgun → heavy_shotgun` trigger, `airboat → accelerator/brake`.

## Prerequisites

- Steam with **Half-Life 2** (appid 220) and **Source SDK Base 2013 Singleplayer** (appid 243730) installed.
- Visual Studio. VS 2013 is Valve's official toolset; VS 2019/2022 works with the community fixes to
  `source-sdk-2013` (search "source-sdk-2013 VS2022"). 
- `git`.

## Build

1. Clone the SDK (singleplayer tree):
   ```
   git clone https://github.com/ValveSoftware/source-sdk-2013
   ```
   Work in `source-sdk-2013/sp/src`.
2. Add the integration to the **client** project:
   - copy `src/pulsecore_hl2_integration.cpp` → `sp/src/game/client/`
   - copy `vendor/pulsecore_client.h` → `sp/src/game/client/` (or any include dir the client sees)
   - register the .cpp: add `$File "pulsecore_hl2_integration.cpp"` to `sp/src/game/client/client_hl2.vpc`,
     then regenerate the projects (`sp/src/creategameprojects.bat`), OR just add the file to the generated
     client project in the VS solution.
3. Build the **Release** config of the client project → produces `client.dll` (and `server.dll`).

## Install & run

1. Create `…/Steam/steamapps/sourcemods/PulseCore_HL2/` and copy `mod/gameinfo.txt` into it.
2. Copy the built `client.dll` (+ `server.dll`) into `PulseCore_HL2/bin/`.
3. Restart Steam → **"Half-Life 2 — PulseCore Enhanced"** appears in your library.
4. Start **PulseCore** (so the Bridge pipe is listening) and connect your DualSense — PulseCore hosts the
   Bridge server that receives your mod's events. (To send test messages by hand without launching a game,
   build and run the `pulse-test-client` example in this repo.)
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
HL2). PulseCore's glue here (`pulsecore_hl2_integration.cpp`, `vendor/pulsecore_client.h`) is licensed
under **PolyForm Noncommercial 1.0.0**, the same as the rest of this repository (see the top-level
`LICENSE.md`). Both licenses point the same way for this example: **noncommercial use only**. Keep this
example in its own tree — do **not** merge Source-SDK-derived code into PulseCore's proprietary core.
