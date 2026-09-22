#!/usr/bin/env python3
"""Validate effect packs before they reach players.

Two ways to run it:

    python tools/check_packs.py                 # the whole catalogue in mods/index.json (what CI runs)
    python tools/check_packs.py my-pack.json    # one pack of your own, before you import it

PulseCore updates catalogue packs by itself: whatever is listed in mods/index.json reaches everyone who
has that mod, without anybody clicking anything. So the catalogue is checked before merge, not after it
ships. A pack of your own is imported through PulseCore's Mods page ("Import a pack from a file"), and
PulseCore then shows what it did not accept -- this script tells you the same thing before you import.

The checks mirror how PulseCore's effects engine reads a pack. It is strict: an effect it cannot make
sense of is NOT played at some guessed strength, it is left out, and a rule left with nothing to do is
not loaded at all. Each of those is reported here with the field that caused it.

Exit code 0 when everything is usable, 1 otherwise.
"""
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INDEX = os.path.join(ROOT, 'mods', 'index.json')

# What PulseCore can put on a TRIGGER, and what it can play as a one-shot HAPTIC. The two lists do not
# overlap: a haptic name on a trigger (or the other way round) is refused, it does not "sort of work".
TRIGGER_PRESETS = {
    'off', 'aim', 'pistol', 'revolver', 'smg', 'assault_rifle', 'shotgun', 'heavy_shotgun', 'sniper',
    'bow', 'crossbow', 'grenade', 'heavy_weapon', 'gravity_weapon', 'vehicle_accelerator',
    'vehicle_brake', 'motorcycle_throttle', 'magic_charge', 'block', 'empty_magazine', 'reload_locked',
}
HAPTIC_PRESETS = {'weapon_recoil', 'weapon_recoil_heavy', 'player_damaged', 'impact_soft', 'impact_hard'}

MAX_PACK_BYTES = 256 * 1024   # PulseCore does not read a larger file
MAX_TOTAL_RULES = 512         # across ALL installed packs together

problems = []
notes = []


def load(path, what):
    try:
        with open(path, 'rb') as f:
            raw = f.read()
    except FileNotFoundError:
        problems.append('%s: file not found (%s)' % (what, path))
        return None
    if len(raw) > MAX_PACK_BYTES:
        problems.append('%s: the file is %d bytes; PulseCore does not read packs over 256 KB'
                        % (what, len(raw)))
        return None
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
        problems.append('%s: the file is saved as UTF-16 -- save it as UTF-8 (PulseCore\'s importer '
                        'converts it, a hand copy into the packs folder is refused)' % what)
        return None
    try:
        return json.loads(raw.decode('utf-8-sig'))
    except (ValueError, UnicodeDecodeError) as e:
        # Strict JSON, as PulseCore reads it: no comments, no trailing commas.
        problems.append('%s: not valid JSON -- %s' % (what, e))
    return None


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def num(obj, key, lo, hi):
    """A number present and inside the range the engine accepts."""
    v = obj.get(key)
    return is_num(v) and lo <= v <= hi


def byte(obj, key):
    return num(obj, key, 0, 255)


def scale_problem(parent):
    """None when there is no scale_by or it is usable; otherwise why the engine refuses it.

    A refused scale takes its effect with it: the engine will not play the effect at full strength
    in place of the scale it could not read.
    """
    for typo in ('scaleBy', 'scaleby', 'scale-by', 'scale'):
        if typo in parent:
            return 'unknown key "%s" -- did you mean "scale_by"?' % typo
    sc = parent.get('scale_by')
    if sc is None:
        return None
    if not isinstance(sc, dict):
        return '"scale_by" must be an object'
    if not isinstance(sc.get('value'), str) or not sc['value']:
        return '"scale_by.value" must name a state value'
    lo, hi = sc.get('min', 0.0), sc.get('max', 1.0)
    if not is_num(lo) or not is_num(hi):
        return '"scale_by.min" and "scale_by.max" must be numbers'
    if not math.isfinite(lo) or not math.isfinite(hi):
        return '"scale_by.min" and "scale_by.max" must be ordinary numbers'
    if lo == hi:
        return '"scale_by" needs min and max to differ'
    return None


def effects_of(apply, where, rule_no):
    """Everything this rule will actually DO, by the engine's rules. Reports every refusal."""
    found = []

    def bad(what, needs):
        end = '' if needs.endswith(('.', '?', ')')) else '.'
        problems.append('%s rule %d: "%s" is not applied -- %s%s' % (where, rule_no, what, needs, end))

    for side in ('right_trigger', 'left_trigger'):
        t = apply.get(side)
        if not isinstance(t, dict):
            continue
        preset = t.get('preset')
        if isinstance(preset, str) and preset:
            if preset in TRIGGER_PRESETS:
                found.append(side)
            elif preset in HAPTIC_PRESETS:
                bad(side + '.preset', '"%s" is a haptic preset, not a trigger preset' % preset)
            else:
                bad(side + '.preset', '"%s" is not a trigger preset PulseCore knows' % preset)
        r = t.get('resistance')
        if isinstance(r, dict):
            if not (num(r, 'position', 0, 9) and num(r, 'strength', 1, 8)):
                bad(side + '.resistance', 'needs position 0..9 and strength 1..8')
            elif scale_problem(r):
                bad(side + '.resistance', scale_problem(r))
            else:
                found.append(side + '.resistance')

    h = apply.get('haptic')
    if isinstance(h, dict):
        preset = h.get('preset')
        if isinstance(preset, str) and preset:
            if preset in HAPTIC_PRESETS:
                found.append('haptic')
            elif preset in TRIGGER_PRESETS:
                bad('haptic.preset', '"%s" is a trigger preset, not a haptic preset' % preset)
            else:
                bad('haptic.preset', '"%s" is not a haptic preset PulseCore knows' % preset)
        elif byte(h, 'heavy') and byte(h, 'light') and num(h, 'duration_ms', 1, 1000):
            if scale_problem(h):
                bad('haptic', scale_problem(h))
            else:
                found.append('haptic.pulse')
        else:
            bad('haptic', 'needs a preset name, or heavy+light in 0..255 with duration_ms 1..1000')

    lb = apply.get('lightbar')
    if isinstance(lb, dict):
        if byte(lb, 'r') and byte(lb, 'g') and byte(lb, 'b'):
            found.append('lightbar')
            if scale_problem(lb):
                # The base colour still applies; only the gradient toward "to" is lost.
                bad('lightbar.scale_by', scale_problem(lb) + ' (the base colour is kept, the gradient '
                                                              'is not)')
        else:
            bad('lightbar', 'needs r, g and b in 0..255')

    pt = apply.get('pulse_train')
    if isinstance(pt, dict):
        if not (byte(pt, 'heavy') and byte(pt, 'light') and num(pt, 'beat_ms', 1, 1000)
                and num(pt, 'interval_ms', 0, 10000) and pt['interval_ms'] > pt['beat_ms']):
            bad('pulse_train', 'needs heavy+light in 0..255, beat_ms 1..1000 and a LARGER '
                               'interval_ms up to 10000')
        else:
            gap = pt.get('interval_scale_by')
            why = scale_problem(pt) or (scale_problem({'scale_by': gap}) if gap is not None else None)
            if why:
                bad('pulse_train', why)
            else:
                found.append('pulse_train')

    rb = apply.get('rumble')
    if isinstance(rb, dict):
        if not (byte(rb, 'heavy') and byte(rb, 'light')):
            bad('rumble', 'needs both heavy and light in 0..255')
        elif scale_problem(rb):
            bad('rumble', scale_problem(rb))
        else:
            found.append('rumble')

    return found


def check_rules(pack, where):
    """Checks every rule of one pack; returns (total, live)."""
    rules = pack.get('rules') if isinstance(pack, dict) else None
    if not isinstance(rules, list) or not rules:
        problems.append('%s: no "rules" list -- PulseCore refuses it, so it would not install' % where)
        return 0, 0
    live = 0
    for j, rule in enumerate(rules):
        if not isinstance(rule, dict):
            problems.append('%s: rule %d is not an object' % (where, j))
            continue
        when = rule.get('when')
        has_state = isinstance(when, dict) and isinstance(when.get('state'), str) and when['state']
        has_event = isinstance(rule.get('on_event'), str) and rule['on_event']
        if not has_state and not has_event:
            problems.append('%s: rule %d has neither "when.state" nor "on_event" -- not loaded'
                            % (where, j))
            continue
        if has_state and 'equals' in when:
            eq = when['equals']
            # A string, a number or true/false; a number or boolean matches the game's value in the
            # same form ("equals": 1 matches 1, "equals": true matches true).
            if not isinstance(eq, (str, int, float, bool)):
                problems.append('%s: rule %d: "when.equals" must be a string, a number or '
                                'true/false -- not loaded' % (where, j))
                continue
        apply = rule.get('apply')
        if not isinstance(apply, dict):
            problems.append('%s: rule %d has no "apply" object -- not loaded' % (where, j))
            continue
        if not effects_of(apply, where, j):
            problems.append('%s: rule %d applies nothing PulseCore can carry out -- not loaded'
                            % (where, j))
            continue
        live += 1
    if live > MAX_TOTAL_RULES:
        problems.append('%s: %d usable rules; PulseCore keeps at most %d across all installed packs'
                        % (where, live, MAX_TOTAL_RULES))
    return len(rules), live


def local_pack_path(url):
    """Map the published raw URL back to the file in this checkout.

    Checking the URL players actually download beats checking a path typed separately beside it.
    """
    marker = '/main/'
    if marker not in url:
        return None
    return os.path.join(ROOT, *url.split(marker, 1)[1].split('/'))


def check_catalogue():
    index = load(INDEX, 'mods/index.json')
    if index is None:
        return
    mods = index.get('mods')
    if not isinstance(mods, list) or not mods:
        problems.append('mods/index.json: "mods" must be a non-empty array')
        mods = []

    seen_ids = set()
    for i, mod in enumerate(mods):
        mod_id = mod.get('id', '')
        if not mod_id:
            problems.append('mods/index.json[%d]: missing "id"' % i)
            continue
        where = 'mod "%s"' % mod_id

        # The id names the installed file and is how an update is matched to something already on
        # disk. A duplicate means two mods overwriting each other on every player's machine.
        if mod_id in seen_ids:
            problems.append('%s: duplicate id -- the two entries would install over each other' % where)
        seen_ids.add(mod_id)

        if not isinstance(mod.get('version'), int) or mod['version'] < 1:
            problems.append('%s: "version" must be an integer >= 1' % where)
        for field in ('title', 'pack'):
            if not mod.get(field):
                problems.append('%s: missing "%s"' % (where, field))

        url = mod.get('pack', '')
        path = local_pack_path(url)
        if path is None:
            # A pack hosted elsewhere cannot be checked here -- and an unchecked pack still auto-updates.
            problems.append('%s: "pack" must point into this repository so it can be validated (%s)'
                            % (where, url))
            continue
        if not os.path.isfile(path):
            problems.append('%s: "pack" URL points at a file that is not in this repository: %s'
                            % (where, url))
            continue

        pack = load(path, where + ' pack')
        if pack is None:
            continue

        pack_version = pack.get('pack_version')
        if not isinstance(pack_version, int) or pack_version < 1:
            problems.append('%s: pack is missing "pack_version" (integer >= 1)' % where)
        elif isinstance(mod.get('version'), int) and pack_version != mod['version']:
            # Both numbers move together, or the update reaches nobody (pack bumped alone) or is
            # downloaded forever (catalogue bumped alone).
            problems.append(
                '%s: version mismatch -- catalogue says %d, pack says pack_version %d. Bump BOTH: the '
                'pack so players get the change, the catalogue so they are offered it.'
                % (where, mod['version'], pack_version))

        total, live = check_rules(pack, where)
        notes.append('%s: v%s, %d rules (%d live)' % (mod_id, pack.get('pack_version', '?'), total, live))


def check_one(path):
    where = os.path.basename(path)
    pack = load(path, where)
    if pack is None:
        return
    total, live = check_rules(pack, where)
    notes.append('%s: %d rules (%d usable)' % (where, total, live))


if len(sys.argv) > 1:
    for p in sys.argv[1:]:
        check_one(p)
else:
    check_catalogue()

for line in notes:
    print('  ok  ' + line)

if problems:
    print('\n%d problem(s):' % len(problems))
    for p in problems:
        print('  !!  ' + p)
    sys.exit(1)

print('\n%d pack(s) validated.' % len(notes))
