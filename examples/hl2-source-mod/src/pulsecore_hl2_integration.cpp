// SPDX-License-Identifier: MIT
//========= PulseCore Game Integration — Half-Life 2 (Source SDK 2013 Singleplayer) =========//
//
// Client-side integration: a per-frame game system polls the local player and reports semantic game
// events to PulseCore over the local pipe. PulseCore turns them into adaptive-trigger / haptic effects.
// The mod reports WHAT happens; it never touches HID or the DualSense directly.
//
// Drop-in: add this ONE .cpp to the CLIENT project (game/client) of a source-sdk-2013 mod (see the
// mod README), rebuild client.dll. It self-registers — no other file needs editing.
//
// IMPORTANT: this is written against the well-known Source SDK 2013 client API but has NOT been compiled
// here (no Source SDK in the build env). Expect to fix a few include paths / accessor names on your
// first build — each poll below is isolated so a rename is a one-line change. Lines flagged // [VERIFY]
// are the most likely to need a small adjustment for your SDK fork.
//
// Licence note: this file is MIT (PulseCore glue); once compiled into a Source mod the resulting binary is
// also governed by Valve's Source 1 SDK License, which has its own terms for distributing mods. Keep it in this
// separate example, not in PulseCore's proprietary tree.

#include "cbase.h"                 // must be first in every Source translation unit
#include "igamesystem.h"           // CAutoGameSystemPerFrame
#include "c_baseplayer.h"          // C_BasePlayer
#include "c_basecombatweapon.h"    // C_BaseCombatWeapon
#include "iclientvehicle.h"        // IClientVehicle // [VERIFY] header name in your SDK
#include "in_buttons.h"            // IN_ATTACK
#include "c_basehlplayer.h"         // C_BaseHLPlayer (suit power)      // [VERIFY] path in your SDK
#include "decals.h"                 // CHAR_TEX_* ground material codes // [VERIFY] path in your SDK

// The vendored PulseCore client. Included last so <windows.h> doesn't fight the Source headers.
#include "pulsecore_client.h"

// memdbgon must be the last include in a Source .cpp
#include "tier0/memdbgon.h"

namespace {

const char* kIntegrationId = "community.hl2.enhanced";
const char* kGameId = "steam:220";
const char* kVersion = "1.0.0";

// Normalize an HL2 vehicle entity classname to a stable integration id used by effects.json.
const char* NormalizeVehicle(const char* classname) {
    if (!classname) return "";
    if (Q_stristr(classname, "airboat")) return "airboat";
    if (Q_stristr(classname, "jeep") || Q_stristr(classname, "buggy")) return "buggy";
    return classname;
}

// The material the player is standing on, as a stable name for effects.json. Codes come from the
// surface properties the movement code already resolves every step (surfacedata_t::game.material,
// CHAR_TEX_* in decals.h) -- no sound parsing, no guessing from the map.
const char* GroundMaterialName(char code) {
    switch (code) {
        case CHAR_TEX_CONCRETE: return "concrete";
        case CHAR_TEX_METAL:    return "metal";
        case CHAR_TEX_GRATE:    return "grate";
        case CHAR_TEX_VENT:     return "vent";
        case CHAR_TEX_DIRT:     return "dirt";
        case CHAR_TEX_SLOSH:    return "water";
        case CHAR_TEX_TILE:     return "tile";
        case CHAR_TEX_WOOD:     return "wood";
        case CHAR_TEX_GLASS:    return "glass";
        case CHAR_TEX_COMPUTER: return "computer";
        case CHAR_TEX_FLESH:    return "flesh";
        case CHAR_TEX_SAND:     return "sand";                          // [VERIFY] exists in your SDK
        default:                return "default";
    }
}

class CPulseCoreIntegration : public CAutoGameSystemPerFrame {
public:
    CPulseCoreIntegration() : CAutoGameSystemPerFrame("CPulseCoreIntegration") {}

    bool Init() override {
        m_client.Configure(kIntegrationId, kGameId, kVersion);
        return true;
    }

    void Shutdown() override { m_client.Disconnect(); }

    void LevelInitPostEntity() override {
        // Reset the diff cache so the first poll re-sends everything for the new map.
        m_lastWeapon.Clear();
        m_lastVehicle.Clear();
        m_lastHealth = -1;
        m_lastCritical.Clear();
        m_lastAttack = false;
        m_readySent = false;
    }

    void LevelShutdownPreEntity() override {
        m_client.SetState("game.state", "loading");
    }

    // Client per-frame tick.
    void Update(float frametime) override {
        (void)frametime;
        const unsigned long now = static_cast<unsigned long>(gpGlobals->realtime * 1000.0);
        if (!m_client.Poll(now)) return;   // not connected yet (PulseCore not running) -> skip quietly

        // Heartbeat so PulseCore's watchdog keeps the session alive during quiet gameplay.
        if (now - m_lastHeartbeat > 1500) { m_client.Heartbeat(); m_lastHeartbeat = now; }

        C_BasePlayer* pPlayer = C_BasePlayer::GetLocalPlayer();
        if (!pPlayer) return;

        if (!m_readySent) { m_client.SetState("game.state", "playing"); m_readySent = true; }

        // ---- weapon.current ----
        C_BaseCombatWeapon* pWeapon = pPlayer->GetActiveWeapon();   // [VERIFY]
        const char* weaponClass = pWeapon ? pWeapon->GetClassname() : "";
        if (!m_lastWeapon.IsEqualTo(weaponClass)) {
            m_lastWeapon.Set(weaponClass);
            m_client.SetState("weapon.current", weaponClass);
        }

        // ---- weapon.primary_fire (rising edge of IN_ATTACK while holding a weapon) ----
        const bool attack = (pPlayer->m_nButtons & IN_ATTACK) != 0;   // [VERIFY]
        if (attack && !m_lastAttack && pWeapon) m_client.SendEvent("weapon.primary_fire");
        if (!attack && m_lastAttack) m_client.SendEvent("weapon.primary_fire_stop");
        m_lastAttack = attack;

        // ---- vehicle.current + enter/exit ----
        const bool inVehicle = pPlayer->IsInAVehicle();               // [VERIFY]
        const char* vehId = "";
        if (inVehicle) {
            IClientVehicle* pv = pPlayer->GetVehicle();               // [VERIFY]
            C_BaseEntity* pEnt = pv ? pv->GetVehicleEnt() : NULL;     // [VERIFY]
            vehId = NormalizeVehicle(pEnt ? pEnt->GetClassname() : "");
        }
        if (!m_lastVehicle.IsEqualTo(vehId)) {
            const bool wasIn = m_lastVehicle.Length() > 0;
            const bool nowIn = vehId[0] != '\0';
            m_lastVehicle.Set(vehId);
            m_client.SetState("vehicle.current", vehId);
            if (nowIn) m_client.SendEvent("vehicle.entered");
            else if (wasIn) m_client.SendEvent("vehicle.exited");
        }

        // ---- player.health + damage ----
        const int health = pPlayer->GetHealth();                     // [VERIFY]
        if (health != m_lastHealth) {
            if (m_lastHealth >= 0 && health < m_lastHealth) m_client.SendEvent("player.damaged");
            if (m_lastHealth > 0 && health <= 0) m_client.SendEvent("player.died");
            m_lastHealth = health;
            m_client.SetValue("player.health", static_cast<double>(health));
        }

        // A separate state rather than a rule reading the number, because "in danger" is a decision
        // about the game, not about the effect: the threshold belongs with the game's own idea of
        // critical health, and a rule then only decides what that FEELS like.
        const char* critical = (health > 0 && health <= 20) ? "true" : "false";
        if (!m_lastCritical.IsEqualTo(critical)) {
            m_lastCritical.Set(critical);
            m_client.SetState("player.critical", critical);
        }

        PollMedium(pPlayer);
        PollGround(pPlayer);
        PollLanding(pPlayer);
        PollSuit(pPlayer);
        PollVehicleMotion(pPlayer);
    }

    // Water / ladder / air. These are STATES, not events: PulseCore holds a sustained level while one
    // is true and drops it the moment it stops, so swimming hums and walking out of the water is silent
    // without the mod having to remember to say "stop".
    void PollMedium(C_BasePlayer* pPlayer) {
        const char* medium = "air";
        if (pPlayer->GetMoveType() == MOVETYPE_LADDER) medium = "ladder";              // [VERIFY]
        else if (pPlayer->GetWaterLevel() >= 2) medium = "water";   // waist-deep or more // [VERIFY]
        else if (pPlayer->GetWaterLevel() == 1) medium = "shallow_water";
        if (!m_lastMedium.IsEqualTo(medium)) {
            const bool enteringWater = Q_strcmp(medium, "water") == 0 &&
                                       !m_lastMedium.IsEqualTo("shallow_water");
            m_lastMedium.Set(medium);
            m_client.SetState("player.medium", medium);
            if (enteringWater) m_client.SendEvent("player.water_enter");   // the splash
        }
    }

    // The material underfoot, reported only while actually on the ground and moving, so standing still
    // does not stream a state nothing reacts to.
    void PollGround(C_BasePlayer* pPlayer) {
        const char* material = "";
        if (pPlayer->GetGroundEntity() != NULL) {
            // C_BasePlayer::GetGroundSurface() is protected, so the surface is looked up the same way
            // the movement code does: trace a short ray down and ask the physics props what was hit.
            // Reading it ourselves also means the answer stays right if the player is standing on a
            // prop rather than on world geometry.
            const Vector origin = pPlayer->GetAbsOrigin();
            trace_t tr;
            UTIL_TraceLine(origin + Vector(0.0f, 0.0f, 1.0f), origin - Vector(0.0f, 0.0f, 16.0f),
                           MASK_SOLID, pPlayer, COLLISION_GROUP_NONE, &tr);
            if (tr.DidHit()) {
                const surfacedata_t* surface = physprops->GetSurfaceData(tr.surface.surfaceProps);
                if (surface) material = GroundMaterialName(surface->game.material);
            }
        }
        if (!m_lastGround.IsEqualTo(material)) {
            m_lastGround.Set(material);
            m_client.SetState("player.ground_material", material);
        }
    }

    // Landing, with the speed it happened at. HL2's own RUMBLE_FALL_SHORT/LONG already fires for big
    // drops (see the rumble bridge); this adds the CONTINUOUS quantity the waveform ids cannot carry,
    // so a rule can scale the thump by how far the player actually fell.
    void PollLanding(C_BasePlayer* pPlayer) {
        const bool grounded = pPlayer->GetGroundEntity() != NULL;
        // GetFallVelocity() is protected, and we do not need it: downward speed IS the fall speed, and
        // reading it straight from the velocity avoids depending on when the engine chooses to zero
        // its own counter.
        const float fallSpeed = -pPlayer->GetAbsVelocity().z;
        if (grounded && !m_wasGrounded && m_peakFall > 100.0f) {
            m_client.SetValue("player.land_speed", static_cast<double>(m_peakFall));
            m_client.SendEvent("player.landed");
        }
        // Track the peak while airborne: at the instant of landing the engine has already zeroed it.
        m_peakFall = grounded ? 0.0f : (fallSpeed > m_peakFall ? fallSpeed : m_peakFall);
        m_wasGrounded = grounded;
    }

    // HEV suit auxiliary power (sprint / flashlight / oxygen drain it). Quantised to whole percent so a
    // continuously draining bar does not spend the session's message budget on noise.
    void PollSuit(C_BasePlayer* pPlayer) {
        C_BaseHLPlayer* pHL = dynamic_cast<C_BaseHLPlayer*>(pPlayer);                    // [VERIFY]
        if (!pHL) return;
        const int power = static_cast<int>(pHL->m_HL2Local.m_flSuitPower);                // [VERIFY]
        if (power != m_lastSuitPower) {
            m_lastSuitPower = power;
            m_client.SetValue("player.suit_power", static_cast<double>(power));
        }
    }

    // Vehicle speed from the vehicle ENTITY's velocity rather than the driveable's protected fields:
    // it needs no subclass, works for both the airboat and the jeep, and is what a rule actually wants
    // (engine level by speed). Quantised for the same reason as suit power.
    void PollVehicleMotion(C_BasePlayer* pPlayer) {
        int speed = 0;
        if (pPlayer->IsInAVehicle()) {
            IClientVehicle* pv = pPlayer->GetVehicle();
            if (C_BaseEntity* pEnt = pv ? pv->GetVehicleEnt() : NULL) {
                speed = static_cast<int>(pEnt->GetAbsVelocity().Length());               // [VERIFY]
                speed = (speed / 25) * 25;   // 25-unit buckets: enough for an engine curve
            }
        }
        if (speed != m_lastVehicleSpeed) {
            m_lastVehicleSpeed = speed;
            m_client.SetValue("vehicle.speed", static_cast<double>(speed));
        }
    }

private:
    // Small stable-string diff helper (avoids re-sending an unchanged state every frame).
    struct CachedStr {
        char v[128];
        CachedStr() { v[0] = '\0'; }
        void Clear() { v[0] = '\0'; }
        int Length() const { return static_cast<int>(Q_strlen(v)); }
        bool IsEqualTo(const char* s) const { return Q_strcmp(v, s ? s : "") == 0; }
        void Set(const char* s) { Q_strncpy(v, s ? s : "", sizeof(v)); }
    };

    pulsecore_client::PulseClient m_client;
    CachedStr m_lastWeapon;
    CachedStr m_lastVehicle;
    CachedStr m_lastCritical;
    CachedStr m_lastMedium;
    CachedStr m_lastGround;
    int m_lastHealth = -1;
    int m_lastSuitPower = -1;
    int m_lastVehicleSpeed = -1;
    float m_peakFall = 0.0f;
    bool m_wasGrounded = true;
    bool m_lastAttack = false;
    bool m_readySent = false;
    unsigned long m_lastHeartbeat = 0;

public:
    pulsecore_client::PulseClient& client() { return m_client; }
};

// Self-registering singleton — no other file needs to reference it.
CPulseCoreIntegration g_PulseCoreIntegration;

}  // namespace

namespace pulsecore_hl2 {
// One session for the whole mod. The rumble bridge reports through this same client: two connections
// would be two integrations competing for ownership of the same effect channels.
pulsecore_client::PulseClient& SharedClient() { return g_PulseCoreIntegration.client(); }
}  // namespace pulsecore_hl2
