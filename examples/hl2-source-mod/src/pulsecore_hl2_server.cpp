// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//========= PulseCore Game Integration — Half-Life 2 SERVER side =========//
//
// Three of the best feelings in Half-Life 2 are simply not visible to the client, and no client-side
// trick can reach them (verified against the SDK source, 2026-07-20):
//
//   * the gravity gun's held object and its MASS  -- the client is never sent the held entity, and it
//     has no IPhysicsObject for server props at all, so "resistance by weight" is server-only;
//   * the shotgun's per-shell reload             -- m_bInReload is not networked in singleplayer;
//   * the grenade                                -- the fuse lives in a server entity and is not
//     networked (and is a fixed 3.0s: HL2 has no "cooking in hand" at all).
//
// This file runs in server.dll and reports them. It does NOT open its own PulseCore connection: in
// singleplayer the client and server are two modules of ONE process, so a second connection would be a
// second integration competing with the first for ownership of the same effect channels. Instead it
// sends a small user message that client.dll forwards over the single existing session.
//
// SPDX: PolyForm-Noncommercial-1.0.0 (PulseCore glue). Compiled into a Source mod, the binary is governed by Valve's Source 1
// SDK License.
//
// NOT COMPILED HERE (no Source SDK in the PulseCore build environment). // [VERIFY] marks the lines
// most likely to need a small adjustment on your first build.

#include "cbase.h"
#include "player.h"
#include "basecombatweapon_shared.h"
#include "hl2/weapon_physcannon.h"   // PhysCannonGetHeldEntity / ...HeldObjectMass  // [VERIFY]
#include "vphysics_interface.h"      // IPhysicsObject::GetMass

#include "tier0/memdbgon.h"

namespace {

// REQUIRED ONE-LINE EDIT: a user message must be registered before UserMessageBegin can send it. Add
//     usermessages->Register( "PulseFx", -1 );      // -1 = variable length
// to RegisterUserMessages() in src/game/shared/hl2/hl2_usermessages.cpp, next to the stock "Rumble"
// and "Damage" registrations. It cannot be done from this file: registration happens once, in shared
// code, before either module's game systems start.
//
// One user message carries every server-observed fact: a short name plus one number. Keeping it to a
// single message means the mod adds exactly one entry to the game's message table instead of one per
// feature, and client.dll needs only one hook.
void SendFx(CBasePlayer* pPlayer, const char* name, float value) {
    if (!pPlayer) return;
    CSingleUserRecipientFilter filter(pPlayer);
    filter.MakeReliable();
    UserMessageBegin(filter, "PulseFx");                                          // [VERIFY]
    WRITE_STRING(name);
    WRITE_FLOAT(value);
    MessageEnd();
}

class CPulseCoreServerWatch : public CAutoGameSystemPerFrame {
public:
    CPulseCoreServerWatch() : CAutoGameSystemPerFrame("CPulseCoreServerWatch") {}

    void LevelInitPostEntity() override {
        m_heldEntity = NULL;
        m_lastMass = -1.0f;
        m_wasReloading = false;
        m_lastClip = -1;
    }

    void FrameUpdatePostEntityThink() override {                                   // [VERIFY]
        CBasePlayer* pPlayer = UTIL_GetLocalPlayer();
        if (!pPlayer) return;
        PollPhysCannon(pPlayer);
        PollReload(pPlayer);
    }

private:
    // The gravity gun. Reporting the MASS is the whole point: "the trigger resists more the heavier the
    // object" is a continuous quantity, and it is the one thing about the physcannon that no preset and
    // no client-side observation can supply.
    void PollPhysCannon(CBasePlayer* pPlayer) {
        CBaseEntity* pHeld = GetPlayerHeldEntity(pPlayer);                         // [VERIFY]
        if (pHeld != m_heldEntity) {
            m_heldEntity = pHeld;
            if (pHeld) {
                float mass = 0.0f;
                if (IPhysicsObject* pPhys = pHeld->VPhysicsGetObject()) {          // [VERIFY]
                    mass = pPhys->GetMass();
                }
                m_lastMass = mass;
                SendFx(pPlayer, "physcannon.grab", mass);
            } else {
                m_lastMass = -1.0f;
                SendFx(pPlayer, "physcannon.release", 0.0f);
            }
        }
    }

    // Reload, including the shotgun's shell-by-shell pump. The clip count is the honest signal here:
    // it moves once per shell actually loaded, which is exactly the moment that should be felt, and it
    // needs no activity/sequence guessing.
    void PollReload(CBasePlayer* pPlayer) {
        CBaseCombatWeapon* pWeapon = pPlayer->GetActiveWeapon();
        if (!pWeapon) { m_wasReloading = false; m_lastClip = -1; return; }

        const bool reloading = pWeapon->m_bInReload;                               // [VERIFY]
        const int clip = pWeapon->m_iClip1;

        // Start/end are EVENTS, but "is reloading" is also a STATE a rule wants to hold a trigger
        // lock against for the whole duration, so both are reported.
        if (reloading && !m_wasReloading) {
            SendFx(pPlayer, "weapon.reload_start", 1.0f);
        }
        if (!reloading && m_wasReloading) {
            SendFx(pPlayer, "weapon.reload_end", 0.0f);
        }
        // A shell went in: only while reloading, so firing (which also lowers the clip) is not mistaken
        // for a reload step.
        if (reloading && m_lastClip >= 0 && clip > m_lastClip) {
            SendFx(pPlayer, "weapon.reload_step", static_cast<float>(clip));
        }
        m_wasReloading = reloading;
        m_lastClip = clip;
    }

    EHANDLE m_heldEntity;
    float m_lastMass = -1.0f;
    bool m_wasReloading = false;
    int m_lastClip = -1;
};

CPulseCoreServerWatch g_PulseCoreServerWatch;

}  // namespace

// Called from CWeaponFrag when a grenade leaves the player's hand, and from CGrenadeFrag on detonation.
// Hooking those two call sites is a two-line edit in weapon_frag.cpp / grenade_frag.cpp; the fuse is a
// compile-time constant in HL2 (GRENADE_TIMER 3.0f) rather than something to read back, and there is no
// "cooking" to report because HL2 has none.
void PulseCore_GrenadeThrown(CBasePlayer* pPlayer, float fuseSeconds) {
    SendFx(pPlayer, "grenade.thrown", fuseSeconds);
}

void PulseCore_GrenadeDetonated(CBasePlayer* pPlayer, float distance) {
    SendFx(pPlayer, "grenade.detonated", distance);
}
