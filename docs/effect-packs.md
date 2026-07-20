# Effect packs

An **effect pack** is a single JSON file of rules: *when the game reports this, the DualSense should feel like that.* It is pure data — never code — so a pack is safe to publish, review in a pull request, and install with one click.

Packs are what PulseCore's **Mods** tab installs. They live in this repository, so the list of available mods grows without shipping a new build of the app.

```
examples/hl2/effects.json     the pack itself
mods/index.json               the catalogue the app reads
```

## The pack file

```jsonc
{
  "schema_version": 1,
  "pack_version": 2,          // ← bump this on every published change
  "rules": [
    {
      "when": { "state": "weapon.current", "equals": "weapon_shotgun" },
      "left_trigger":  { "preset": "rigid",  "start": 30, "force": 200 },
      "right_trigger": { "preset": "weapon", "start": 25, "end": 45, "force": 255 }
    }
  ]
}
```

A rule matches on a **state** (persists until it changes), an **event** (one-shot), or a **value** (a number, which can scale an effect). What a rule can set — triggers, rumble, pulse trains, lightbar — is listed in [protocol.md](protocol.md).

Unknown fields are ignored rather than rejected, so a pack written for a newer PulseCore still works on an older one: the effects it doesn't understand simply don't fire.

## Versioning — how an update reaches players

`pack_version` is a single integer that goes up by one whenever you publish a change. It lives **inside the pack**, not in a file beside it, so it can never drift out of step with the rules it describes — and a pack copied in by hand still reports the truth.

PulseCore updates installed packs by itself when the Mods tab opens, and tells the player what changed. A pack is data we curate, so an outdated one is simply a worse version of the same thing — there is nothing to ask permission about, but silently altering how a game feels would leave someone wondering whether they imagined it.

A pack with **no** `pack_version`, or one PulseCore cannot read, counts as version 1 — the oldest thing there is. So anything already sitting on a player's disk is treated as older than anything you publish.

**To ship a change, both numbers move together:**

1. Edit the rules in `examples/<game>/effects.json`.
2. Increment `pack_version` in that same file.
3. Increment `version` for that mod in `mods/index.json`.
4. Open a pull request.

The catalogue repeats the number so that checking for updates costs one small file instead of downloading every pack just to compare it. If you bump only the pack, nobody is offered the update; if you bump only the catalogue, the app downloads the same pack forever. Move both.

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

`id` identifies the pack forever: it names the installed file and it is how PulseCore knows an update belongs to something already installed. Changing it strands every existing install.

`needs_game_mod` is honest labelling. A pack alone is enough only when the game already reports what it is doing. Half-Life 2 does not, so it also needs the game-side mod from `examples/hl2-source-mod/` — and the app says so, instead of pretending one button did everything.

## Adding a pack for a new game

Send a pull request with the pack under `examples/<game>/effects.json` and an entry in `mods/index.json` starting at `"pack_version": 1` / `"version": 1`. Packs are reviewed for feel as much as for correctness: an effect that fires constantly, or one strong enough to be tiring, makes a controller worse rather than better.
