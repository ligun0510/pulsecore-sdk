#!/usr/bin/env python3
"""Validate the effect-pack catalogue before it reaches players.

PulseCore updates installed packs by itself: whatever is listed in mods/index.json reaches everyone
who has that mod, without anybody clicking anything. That convenience is the reason this script
exists -- a mistake here is not "my controller feels wrong", it is everyone's controller feeling
wrong, quietly. So the catalogue is checked before merge, not after it ships.

The rule checks deliberately mirror how PulseCore's effects engine parses a pack (effects.cpp), and
mirror its most dangerous habit: an effect it cannot make sense of is DROPPED, not rejected. The pack
still installs, the rule still matches, and nothing happens -- which the author experiences as "the
haptics are broken" with no error anywhere. Those silent drops are the main thing worth catching
here.

Run: python tools/check_packs.py
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INDEX = os.path.join(ROOT, 'mods', 'index.json')

problems = []
notes = []


def load(path, what):
    try:
        with open(path, encoding='utf-8-sig') as f:
            return json.load(f)
    except FileNotFoundError:
        problems.append('%s: file not found (%s)' % (what, path))
    except ValueError as e:
        # A pack that will not parse is the worst case: the app refuses it AFTER downloading, so the
        # player sees an install fail for no visible reason.
        problems.append('%s: not valid JSON -- %s' % (what, e))
    return None


def num(obj, key, lo, hi):
    """A number present and inside the range the engine accepts."""
    v = obj.get(key)
    return isinstance(v, (int, float)) and not isinstance(v, bool) and lo <= v <= hi


def byte(obj, key):
    return num(obj, key, 0, 255)


def effects_of(apply, where, rule_no):
    """Everything this rule will actually DO, by the engine's rules. Reports near-misses."""
    found = []

    def bad(what, needs):
        problems.append('%s rule %d: "%s" is ignored by the engine -- %s. The rule will match and '
                        'do nothing.' % (where, rule_no, what, needs))

    for side in ('right_trigger', 'left_trigger'):
        t = apply.get(side)
        if not isinstance(t, dict):
            continue
        if isinstance(t.get('preset'), str) and t['preset']:
            found.append(side)
        r = t.get('resistance')
        if isinstance(r, dict):
            if num(r, 'position', 0, 9) and num(r, 'strength', 1, 8):
                found.append(side + '.resistance')
            else:
                bad(side + '.resistance', 'needs position 0..9 and strength 1..8')

    h = apply.get('haptic')
    if isinstance(h, dict):
        if isinstance(h.get('preset'), str) and h['preset']:
            found.append('haptic')
        elif byte(h, 'heavy') and byte(h, 'light') and num(h, 'duration_ms', 1, 1000):
            found.append('haptic.pulse')
        else:
            bad('haptic', 'needs a preset name, or heavy+light in 0..255 with duration_ms 1..1000')

    lb = apply.get('lightbar')
    if isinstance(lb, dict):
        if byte(lb, 'r') and byte(lb, 'g') and byte(lb, 'b'):
            found.append('lightbar')
        else:
            bad('lightbar', 'needs r, g and b in 0..255')

    pt = apply.get('pulse_train')
    if isinstance(pt, dict):
        if (byte(pt, 'heavy') and byte(pt, 'light') and num(pt, 'beat_ms', 1, 1000)
                and num(pt, 'interval_ms', 0, 10000) and pt['interval_ms'] > pt['beat_ms']):
            found.append('pulse_train')
        else:
            bad('pulse_train', 'needs heavy+light in 0..255, beat_ms 1..1000 and a LARGER '
                               'interval_ms up to 10000')

    rb = apply.get('rumble')
    if isinstance(rb, dict):
        if byte(rb, 'heavy') and byte(rb, 'light'):
            found.append('rumble')
        else:
            bad('rumble', 'needs both heavy and light in 0..255')

    return found


def local_pack_path(url):
    """Map the published raw URL back to the file in this checkout.

    Checking the URL players actually download beats checking a path typed separately beside it.
    """
    marker = '/main/'
    if marker not in url:
        return None
    return os.path.join(ROOT, *url.split(marker, 1)[1].split('/'))


index = load(INDEX, 'mods/index.json')
if index is None:
    print('\n'.join('  !!  ' + p for p in problems))
    sys.exit(1)

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

    # The id names the installed file and is how an update is matched to something already on disk.
    # A duplicate means two mods overwriting each other on every player's machine.
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

    rules = pack.get('rules')
    if not isinstance(rules, list) or not rules:
        problems.append('%s: pack has no rules -- the app rejects it, so it would fail to install'
                        % where)
        rules = []

    pack_version = pack.get('pack_version')
    if not isinstance(pack_version, int) or pack_version < 1:
        problems.append('%s: pack is missing "pack_version" (integer >= 1)' % where)
    elif isinstance(mod.get('version'), int) and pack_version != mod['version']:
        # The one mistake this whole script exists for. Both numbers move together, or the update
        # reaches nobody (pack bumped alone) or is downloaded forever (catalogue bumped alone).
        problems.append(
            '%s: version mismatch -- catalogue says %d, pack says pack_version %d. Bump BOTH: the '
            'pack so players get the change, the catalogue so they are offered it.'
            % (where, mod['version'], pack_version))

    live = 0
    for j, rule in enumerate(rules):
        if not isinstance(rule, dict):
            problems.append('%s: rule %d is not an object' % (where, j))
            continue
        has_state = isinstance(rule.get('when'), dict) and rule['when'].get('state')
        has_event = isinstance(rule.get('on_event'), str) and rule['on_event']
        if not has_state and not has_event:
            problems.append('%s: rule %d has neither "when.state" nor "on_event" -- the engine '
                            'skips it entirely' % (where, j))
            continue
        apply = rule.get('apply')
        if not isinstance(apply, dict):
            problems.append('%s: rule %d has no "apply" object -- the engine skips it entirely'
                            % (where, j))
            continue
        if not effects_of(apply, where, j):
            problems.append('%s: rule %d matches but applies no effect the engine understands'
                            % (where, j))
            continue
        live += 1

    notes.append('%s: v%s, %d rules (%d live)'
                 % (mod_id, pack.get('pack_version', '?'), len(rules), live))

for line in notes:
    print('  ok  ' + line)

if problems:
    print('\n%d problem(s):' % len(problems))
    for p in problems:
        print('  !!  ' + p)
    sys.exit(1)

print('\n%d pack(s) validated.' % len(notes))
