//
// Created by bordplate on 7/20/2023.
//

#include "GoldBolt.h"
#include <rc1/Game.h>
#include <rc1/multiplayer/network/Packet.h>

#include <lib/shk.h>

extern "C" SHK_HOOK(void, goldBoltUpdate, Moby* moby);
void GoldBolt::update() {
    GoldBoltVars* vars = (GoldBoltVars*)this->vars;
    int number = vars->number;
    if (number < 0 || number > 3) {
        // Not a real one: the game deletes it
        SHK_CALL_HOOK(goldBoltUpdate, this);
        return;
    }

    if (this->state != 2 && blocked_bolts[(current_planet * 4) + number] != 0) {
        delete_moby(this);
        return;
    }

    if (this->state == 0) {
        collected_bolts[number] = 0;
    }

    // Most fly into the camera when taken (state 2), but some the game takes right away, and deletes in the same update
    //  (Kerwan's third): either way, it's marked collected
    bool had = gold_bolts[(current_planet * 4) + number] != 0;
    SHK_CALL_HOOK(goldBoltUpdate, this);
    bool taken = this->state == 2 || (!had && gold_bolts[(current_planet * 4) + number] != 0);

    if (taken && collected_bolts[number] == 0) {
        Logger::debug("Collected gold bolt %d", number);
        collected_bolts[number] = 1;

        Client* client = Game::shared().client();
        if (client != nullptr) {
            Packet* packet = Packet::make_collected_gold_bolt_packet(number);
            client->make_ack(packet, nullptr);
            client->send(packet);
        }
    }
}