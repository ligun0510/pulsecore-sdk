// SPDX-License-Identifier: MIT
//========= PulseCore Game Integration — Half-Life 2 rumble-bus bridge =========//
//
// Half-Life 2 already tells the controller what it should FEEL. The game sends a `Rumble` user message
// carrying a semantic waveform id -- RUMBLE_SHOTGUN_DOUBLE, RUMBLE_CROWBAR_SWING, RUMBLE_FALL_LONG,
// RUMBLE_JEEP_ENGINE_LOOP, RUMBLE_DMG_HIGH ... -- and the stock client turns it into XInput rumble.
// (Enumerated in src/game/shared/rumble_shared.h; registered in hl2_usermessages.cpp; received in
// clientmode_shared.cpp -> c_rumble.cpp.)
//
// So the single highest-value thing this mod can do is LISTEN to that bus. One hook gives per-weapon
// fire, the crowbar swing, falls by height, damage by severity, the airboat gun and the jeep engine --
// all of it already tuned by Valve, with no server-side code and no guessing at game internals.
//
// This file forwards each waveform to PulseCore as an event named `hl2.rumble.<name>` plus the scale
// the game asked for, and then hands the message to the stock RumbleEffect() so ordinary XInput rumble
// keeps working exactly as before. PulseCore's effects.json decides what a DualSense should do with it.
//
// Licence: MIT (PulseCore glue). Compiled into a Source mod, the resulting binary is also governed by
// Valve's Source 1 SDK License.
//
// NOT COMPILED HERE (no Source SDK in the PulseCore build environment). Lines marked // [VERIFY] are
// the ones most likely to need a small adjustment on your first build.

#include "cbase.h"
#include "usermessages.h"        // the `usermessages` registry itself -- hud.h does NOT pull it in
#include "rumble_shared.h"       // RUMBLE_* waveform ids            // [VERIFY] path in your SDK
#include "c_rumble.h"            // RumbleEffect()                    // [VERIFY] path in your SDK

#include "pulsecore_client.h"

#include "tier0/memdbgon.h"

namespace pulsecore_hl2 {

// The client that pulsecore_hl2_integration.cpp owns. Declared there, shared here so both files talk
// over ONE session (two connections would fight for effect ownership).
pulsecore_client::PulseClient& SharedClient();

}  // namespace pulsecore_hl2

namespace {

// Names are the STABLE contract with effects.json, so they are spelled out rather than derived from the
// enum: a future SDK that renumbers or inserts a waveform must not silently re-point existing rules.
const char* RumbleName(int index) {
    switch (index) {
        case RUMBLE_PISTOL:           return "pistol";
        case RUMBLE_357:              return "357";
        case RUMBLE_SMG1:             return "smg1";
        case RUMBLE_AR2:              return "ar2";
        case RUMBLE_AR2_ALT_FIRE:     return "ar2_alt";
        case RUMBLE_SHOTGUN_SINGLE:   return "shotgun_single";
        case RUMBLE_SHOTGUN_DOUBLE:   return "shotgun_double";
        case RUMBLE_RPG_MISSILE:      return "rpg_missile";
        case RUMBLE_CROWBAR_SWING:    return "crowbar_swing";
        case RUMBLE_AIRBOAT_GUN:      return "airboat_gun";
        case RUMBLE_JEEP_ENGINE_LOOP: return "jeep_engine";
        case RUMBLE_FLAT_LEFT:        return "flat_left";
        case RUMBLE_FLAT_RIGHT:       return "flat_right";
        case RUMBLE_FLAT_BOTH:        return "flat_both";
        case RUMBLE_DMG_LOW:          return "damage_low";
        case RUMBLE_DMG_MED:          return "damage_medium";
        case RUMBLE_DMG_HIGH:         return "damage_high";
        case RUMBLE_FALL_LONG:        return "fall_long";
        case RUMBLE_FALL_SHORT:       return "fall_short";
        case RUMBLE_PHYSCANNON_OPEN:  return "physcannon_open";
        case RUMBLE_PHYSCANNON_PUNT:  return "physcannon_punt";
        case RUMBLE_STOP_ALL:         return "stop_all";
        default:                      return NULL;   // unknown/unused id: report nothing, guess nothing
    }
}

// Hook for the game's `Rumble` user message. Registered as "Rumble" in hl2_usermessages.cpp:34 with
// three bytes: waveform index, data (scale 0..100), flags (RUMBLE_FLAG_*).
void MsgFunc_PulseCoreRumble(bf_read& msg) {
    const unsigned char waveform = msg.ReadByte();
    const unsigned char data = msg.ReadByte();
    const int flags = msg.ReadByte();

    if (const char* name = RumbleName(waveform)) {
        char event[64];
        Q_snprintf(event, sizeof(event), "hl2.rumble.%s", name);
        // `data` is the game's own 0..100 intensity; pass it through as a value so a rule can scale
        // with it instead of every waveform collapsing to a single fixed strength.
        pulsecore_hl2::SharedClient().SetValue("hl2.rumble.scale", static_cast<double>(data));
        // A looping waveform is a STATE (the jeep engine, held fire), a one-shot is an EVENT. Telling
        // them apart here means effects.json can hold a sustained level for the former without the
        // rule author having to know which HL2 waveforms happen to loop.
        if (flags & RUMBLE_FLAG_LOOP) {
            pulsecore_hl2::SharedClient().SetState("hl2.rumble.loop", name);
        } else if (waveform == RUMBLE_STOP_ALL) {
            pulsecore_hl2::SharedClient().SetState("hl2.rumble.loop", "");
        }
        pulsecore_hl2::SharedClient().SendEvent(event);
    }

    // Hand the message to the stock implementation so normal XInput rumble is untouched. Without this
    // the mod would SILENTLY disable the game's own controller vibration -- a regression a player would
    // blame on PulseCore.                                                                    // [VERIFY]
    RumbleEffect(waveform, data, flags);
}

// Receiver for the facts only server.dll can see (gravity gun mass, per-shell reload, grenades). The
// server sends ONE message type carrying a name and a number; everything it observes arrives here and
// goes out over the SAME PulseCore session the rest of the mod uses.
//
// A name is forwarded as an event and the number as a value beside it, rather than being baked into the
// event name, because the number is the interesting part: the MASS of the held object is what makes the
// trigger resist more for a filing cabinet than for a can.
void MsgFunc_PulseFx(bf_read& msg) {
    char name[64];
    msg.ReadString(name, sizeof(name));
    const float value = msg.ReadFloat();
    if (name[0] == 0) return;   // empty name: nothing to report

    char valueName[96];
    Q_snprintf(valueName, sizeof(valueName), "%s.value", name);
    pulsecore_hl2::SharedClient().SetValue(valueName, static_cast<double>(value));
    pulsecore_hl2::SharedClient().SendEvent(name);

    // The held object is a STATE as well as an event: the trigger should stay resisting for as long as
    // the object is held, not for one frame at the moment it was grabbed.
    if (Q_strcmp(name, "weapon.reload_start") == 0) {
        pulsecore_hl2::SharedClient().SetState("weapon.reloading", "true");
    } else if (Q_strcmp(name, "weapon.reload_end") == 0) {
        pulsecore_hl2::SharedClient().SetState("weapon.reloading", "false");
    } else if (Q_strcmp(name, "physcannon.grab") == 0) {
        pulsecore_hl2::SharedClient().SetValue("physcannon.mass", static_cast<double>(value));
        pulsecore_hl2::SharedClient().SetState("physcannon.holding", "true");
    } else if (Q_strcmp(name, "physcannon.release") == 0) {
        pulsecore_hl2::SharedClient().SetState("physcannon.holding", "false");
    }
}

// Registering the hook REPLACES the stock one installed by ClientModeShared::Init (clientmode_shared.cpp
// :186). That is why the forward above is mandatory. Done from a game system so it runs after the
// stock registration rather than racing it.
class CPulseCoreRumbleBridge : public CAutoGameSystem {
public:
    CPulseCoreRumbleBridge() : CAutoGameSystem("CPulseCoreRumbleBridge") {}

    void PostInit() override {                                                              // [VERIFY]
        usermessages->HookMessage("Rumble", MsgFunc_PulseCoreRumble);
        // "PulseFx" is the mod's OWN message, so this hook replaces nothing.
        usermessages->HookMessage("PulseFx", MsgFunc_PulseFx);                          // [VERIFY]
    }
};

CPulseCoreRumbleBridge g_PulseCoreRumbleBridge;

}  // namespace
