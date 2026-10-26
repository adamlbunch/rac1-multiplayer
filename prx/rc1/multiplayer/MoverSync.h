//
// Keeps the level's moving things that aren't enemies the same in every game: platforms, elevators, trains, the
//  traffic flying by, critters. They run on their own in each game from when that game loaded the level, so without
//  this they're wherever their path has taken them since, which is somewhere else in every game.
//
// They keep running in every game the way they always do (players ride them, bump into them, set them off), but the
//  game whose player is nearest to one sends its state now and then: where it is, its animation, and its variables
//  that aren't what the level started them at. The other games put it in that state at the frame they show of that
//  game, so it carries on from exactly there. Their own variables that point somewhere in their memory are left
//  alone: those differ from game to game.
//

#ifndef RAC1_MULTIPLAYER_MOVERSYNC_H
#define RAC1_MULTIPLAYER_MOVERSYNC_H

#include <lib/types.h>
#include <rc1/rc1.h>
#include <rc1/game/Moby.h>

#include "network/Packet.h"

struct MoverSync {
    // From our moby update loop (see EnemySync), for the level's mobys that move on their own and aren't enemies:
    //  before its update, and after it with how far our player is from it
    static void before_update(Moby* moby, int index);
    static void after_update(Moby* moby, int index, float distance);

    // Sends what was queued this frame, after the loop
    static void send(u32 frame);

    static void receive(MPPacketMovers* packet, size_t len);
    static void on_tick();

    // New level or respawn
    static void reset();
};

#endif //RAC1_MULTIPLAYER_MOVERSYNC_H
