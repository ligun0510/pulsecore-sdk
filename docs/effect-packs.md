# Effect packs

An **effect pack** is a single JSON file of rules: *when the game reports this, the DualSense should feel
like that.* It is pure data — never code — so a pack is safe to publish, review in a pull request and
install with one click.

A pack only **reacts**. Something has to tell PulseCore what is happening in the game — a game-side mod
or plugin speaking the [protocol](protocol.md). Half-Life 2 has one in
[`examples/hl2-source-mod/`](../examples/hl2-source-mod/); a pack for a game with nothing sending events
will install fine and never fire.

## Two ways a pack reaches a player

| | Catalogue pack | Your own pack |
|---|---|---|
| Where it lives | this repository (`examples/<game>/effects.json` + `mods/index.json`) | any `.json` file on your disk |
| How it is installed | PulseCore → **Mods** → pick the game → **Install** | PulseCore → **Mods** → **Import a pack from a file…** |
| Updates | automatic, when the catalogue version goes up | none — import again after removing the old one |
| Removing | **Remove** on its row | **Remove** on its row ("Your own pack") |

An imported pack gets a row of its own on the Mods page. Under it PulseCore says whether it loaded the
pack and lists everything it did not accept — see [Seeing what PulseCore made of your pack](#seeing-what-pulsecore-made-of-your-pack).
A pack cannot be imported under the file name of a catalogue mod (the next catalogue update would
replace it).

## The pack file

```json
{
  "schema_version": 1,
  "pack_version": 1,
  "rules": [
    { "when": { "state": "weapon.current", "equals": "weapon_pistol" },
      "apply": { "right_trigger": { "preset": "pistol" } } },

    { "on_event": "weapon.primary_fire",
      "apply": { "haptic": { "preset": "weapon_recoil", "intensity": 0.8 } } },

    { "when": { "state": "vehicle.current", "equals": "airboat" },
      "apply": { "rumble": { "heavy": 120, "light": 60,
                             "scale_by": { "value": "vehicle.speed", "min": 0, "max": 600 } } } },

    { "when": { "state": "player.health_low", "equals": true },
      "apply": { "pulse_train": { "heavy": 200, "light": 0, "beat_ms": 80, "interval_ms": 900 },
                 "lightbar": { "r": 255, "g": 0, "b": 0 } } }
  ]
}
```

Every rule has **one trigger** and an **`apply`** object:

- `"when": {"state": NAME, "equals": X}` — holds while the game's state `NAME` equals `X`. `X` may be a
  string, a number or `true`/`false`; a number or boolean matches the value the game sends in the same
  form (`"equals": 1` matches `1`, `"equals": true` matches `true`). A state rule's effect is **released**
  the moment nothing selects it any more, so leaving the water cannot leave the pad buzzing.
- `"on_event": NAME` — fires once each time the game sends event `NAME`.

When several state rules select the same channel, the **later** rule in the file wins.

### What `apply` can do

| Key | Shape | Notes |
|---|---|---|
| `right_trigger` / `left_trigger` | `{"preset": NAME}` | trigger presets: `off`, `aim`, `pistol`, `revolver`, `smg`, `assault_rifle`, `shotgun`, `heavy_shotgun`, `sniper`, `bow`, `crossbow`, `grenade`, `heavy_weapon`, `gravity_weapon`, `vehicle_accelerator`, `vehicle_brake`, `motorcycle_throttle`, `magic_charge`, `block`, `empty_magazine`, `reload_locked` |
| | `{"resistance": {"position": 0-9, "strength": 1-8}}` | resistance computed rather than named; takes `scale_by` (the weight of what you hold, the draw of a bow) |
| `haptic` | `{"preset": NAME, "intensity": 0-1}` | haptic presets: `weapon_recoil`, `weapon_recoil_heavy`, `player_damaged`, `impact_soft`, `impact_hard` |
| | `{"heavy": 0-255, "light": 0-255, "duration_ms": 1-1000}` | a one-shot shaped by numbers; takes `scale_by` |
| `rumble` | `{"heavy": 0-255, "light": 0-255}` | a level held while the state holds; heavy = left grip (low), light = right grip (high); takes `scale_by` |
| `pulse_train` | `{"heavy", "light", "beat_ms": 1-1000, "interval_ms": beat_ms…10000, "alternate": bool}` | a rhythm (heartbeat, ladder, fuse); `alternate` swaps grips each beat; takes `scale_by` (strength) and `interval_scale_by` (speed) |
| `lightbar` | `{"r", "g", "b"}` (0-255) | with `"to": {"r","g","b"}` and a `scale_by` it becomes a gradient |

Trigger presets and haptic presets are **separate lists**: `weapon_recoil` on a trigger, or `shotgun` as
a haptic, is refused — it does not "sort of work".

### Scaling by a live value — `scale_by`

```json
"scale_by": { "value": "vehicle.speed", "min": 0, "max": 600 }
```

The game sends numbers with `value` messages; the effect is multiplied by where the number sits between
`min` and `max` (clamped to 0…1). `min` may be greater than `max` to invert it — natural for distance,
where nearer must mean stronger. A value that has not arrived yet counts as **zero**, not full strength.

## How PulseCore reads a pack

PulseCore is strict, on purpose. **An effect it cannot make sense of is left out — never played at a
guessed strength — and a rule left with nothing to do is not loaded at all.** Each case below is reported
(on the Mods page and by `tools/check_packs.py`):

- a `scale_by` that is not an object, names no value, has non-number or equal `min`/`max`, or is
  misspelt (`scaleBy`, `scale`) — the effect it belongs to is not applied;
- a trigger preset that is not a trigger preset, a haptic preset that is not a haptic preset;
- a number outside its range (`strength` 20, `duration_ms` 5000, an `interval_ms` not larger than
  `beat_ms`), or half a pair (`heavy` without `light`);
- `"equals"` that is an object or a list;
- a rule with an empty `apply`, or with neither `when.state` nor `on_event`.

The file itself:

- **strict JSON** — no comments, no trailing commas;
- **UTF-8**; a UTF-8 byte-order mark is fine; UTF-16 is refused when copied in by hand (the importer
  converts it);
- **at most 256 KB**, and at most **512 rules across all installed packs together**;
- packs are applied in the order of their **file names** (case-insensitive). Today every installed pack
  applies to every connected game, so two packs that react to the same names can overlap — the later
  file name wins a channel both select.

Unknown **keys** are ignored, so a pack written for a newer PulseCore still loads on an older one; only
the effects it does not understand stay silent. Unknown state/event **names** are simply never sent.

## Seeing what PulseCore made of your pack

Open **Mods** and select the pack. Under it:

- **"PulseCore has loaded this pack."** or **"PulseCore did not load this pack."**
- **"What PulseCore noticed in this pack:"** — one line per problem: why the file was skipped, or which
  rule was ignored and why. The text can be selected and copied into a bug report.

An import that PulseCore did not fully accept says so straight away. Every load is also written to
PulseCore's `core.log` (`%LOCALAPPDATA%\PulseCore\logs`), one line per decision.

Before importing, you can run the same checks yourself:

```
python tools/check_packs.py path/to/my-pack.json
```

It prints every problem with the rule number and the field, and exits non-zero if anything will not be
applied. A change to the rules takes effect for a game the next time it connects — restart the game after
importing or removing a pack.

## Versioning — how a catalogue update reaches players

`pack_version` is a single integer that goes up by one whenever you publish a change. It lives **inside
the pack**, so it can never drift out of step with the rules it describes.

PulseCore updates installed catalogue packs by itself when the Mods page opens, and tells the player what
changed. It replaces only a file that is **exactly what PulseCore installed**: a pack the player has
edited is kept, and the player is told a newer version exists. A pack with no `pack_version` counts as
version 1.

**To ship a change, both numbers move together:**

1. Edit the rules in `examples/<game>/effects.json`.
2. Increment `pack_version` in that same file.
3. Increment `version` for that mod in `mods/index.json`.
4. Open a pull request.

If you bump only the pack, nobody is offered the update; if you bump only the catalogue, the app
downloads the same pack forever. `tools/check_packs.py` fails the pull request when they disagree.

## The catalogue

`mods/index.json` is the list the app fetches:

```jsonc
{
  "mods": [
    {
      "id": "community.hl2.enhanced",     // also the installed file name — keep it stable
      "title": "Half-Life 2",
      "game": "Half-Life 2",
      "version": 2,                        // must match the pack's pack_version
      "pack": "https://raw.githubusercontent.com/.../examples/hl2/effects.json",
      "install": "…",                      // instructions for the game-side mod, if one is needed
      "issues": "…",                       // where players ask for new effects
      "needs_game_mod": true,
      "features": ["…"]
    }
  ]
}
```

`id` identifies the pack forever: it names the installed file and is how PulseCore matches an update to
what is already installed. Changing it strands every existing install. Packs are downloaded only from
GitHub over https and must be at most 256 KB.

`needs_game_mod` is honest labelling: a pack alone is enough only when the game already reports what it
is doing. Half-Life 2 does not, so it also needs the game-side mod — and the app says so.

## What CI checks

`tools/check_packs.py` gates every pull request, and runs locally the same way:

```
python tools/check_packs.py              # the whole catalogue
python tools/check_packs.py my.json      # one pack of your own
```

It rejects a pack that will not parse, a catalogue entry whose `pack` URL does not resolve to a file in
this repository, duplicate ids and the two version numbers disagreeing — and it reports every effect and
rule PulseCore would not apply, by the same rules PulseCore uses, naming the field.

## Where packs and their issues live

**Every catalogue pack lives in this one repository**, so `tools/check_packs.py` can check each pack
against the catalogue that ships it. Requests and reports go here too, through the issue forms:

| I want to… | Use |
|---|---|
| ask for a feeling in Half-Life 2 / Episode One / Episode Two | the matching **effect request** form |
| ask for a game that has no pack yet | **Another game — support request** |
| report an effect that does not fire or feels wrong | **Something in a pack is broken** |
| just ask a question | the [Discord](https://discord.gg/tYRFz6n8Rw) — faster than an issue |

## Adding a pack for a new game

Send a pull request with the pack under `examples/<game>/effects.json` and an entry in `mods/index.json`
starting at `"pack_version": 1` / `"version": 1`. Packs are reviewed for feel as much as for correctness:
an effect that fires constantly, or one strong enough to be tiring, makes a controller worse rather than
better.
