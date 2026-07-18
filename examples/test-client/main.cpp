// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PulseCore Game Integration — Phase 1 demo client. Connects, reports a shotgun + a fire event, then
// leaves. Run the developer console first, then this. Uses only the public SDK (no internals).
#include <chrono>
#include <iostream>
#include <thread>

#include "pulse/integration.h"

int main() {
    pulse::integration client;

    pulse::IntegrationConfig cfg;
    cfg.integration_id = "example.hl2";
    cfg.game_id = "steam:220";
    cfg.version = "1.0.0";
    cfg.capabilities = {"weapon", "vehicle"};

    if (!client.connect(cfg)) {
        std::cerr << "could not connect — is the PulseCore integration bridge running?\n";
        return 1;
    }
    std::cout << "connected as " << cfg.integration_id << "\n";

    client.set_state("weapon.current", "weapon_shotgun");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.send_event("weapon.fire");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    client.disconnect();
    std::cout << "sent weapon.current=weapon_shotgun and weapon.fire, disconnected.\n";
    return 0;
}
