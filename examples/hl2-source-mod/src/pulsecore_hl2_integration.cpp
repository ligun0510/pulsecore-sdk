// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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
// SPDX note: this file is PolyForm Noncommercial 1.0.0 (PulseCore glue); once compiled into a Source mod
// the resulting binary is additionally governed by Valve's Source 1 SDK License (non-commercial mod for a
// Source game). Keep it in this separate example, not in PulseCore's proprietary tree.

#include "cbase.h"                 // must be first in every Source translation unit
#include "igamesystem.h"           // CAutoGameSystemPerFrame
#include "c_baseplayer.h"          // C_BasePlayer
#include "c_basecombatweapon.h"    // C_BaseCombatWeapon
#include "iclientvehicle.h"        // IClientVehicle // [VERIFY] header name in your SDK
#include "in_buttons.h"            // IN_ATTACK

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
    int m_lastHealth = -1;
    bool m_lastAttack = false;
    bool m_readySent = false;
    unsigned long m_lastHeartbeat = 0;
};

// Self-registering singleton — no other file needs to reference it.
CPulseCoreIntegration g_PulseCoreIntegration;

}  // namespace
