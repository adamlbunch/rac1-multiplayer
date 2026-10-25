//
// Shares what players do to the level's own mobys: enemies, crates, breakables and so on. Every hit a player lands
//  goes to the other players' games, which apply the same damage to their copy of that moby, so health, deaths and
//  broken crates match for everyone. If something a player hit disappears in their game, it's removed from the
//  other games too, in case it didn't come down to damage they could apply.
//

#ifndef RAC1_MULTIPLAYER_WORLDSYNC_H
#define RAC1_MULTIPLAYER_WORLDSYNC_H

#include <lib/types.h>
#include <rc1/rc1.h>

#include "network/Packet.h"

struct Client;

struct WorldSync {
    // Picks up the hits our player made since last frame, and notices mobys they hit disappearing
    static void on_tick(Client* client);

    static void receive_hit(MPPacketMobyHit* hit);
    static void receive_killed(MPPacketMobyKilled* killed);

    // New level or respawn
    static void reset();
};

#endif //RAC1_MULTIPLAYER_WORLDSYNC_H
