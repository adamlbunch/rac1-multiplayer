//
// Keeps the level's moving things that aren't enemies the same in every game: platforms, elevators, trains, the
//  traffic flying by, critters. They run on their own in each game from when that game loaded the level, so without
//  this they're wherever their path has taken them since, which is somewhere else in every game.
//
// They keep running in every game the way they always do (players ride them, bump into them, set them off), but the
//  game whose player is nearest to one sends its state now and then: where it is, its animation, and its variables
//  that aren't what the level started them at. The other games put it in that state at the frame they show of that
//  game, so it carries on from exactly there. Their own variables that point somewhere in their memory are left
//  alone: those differ from game to game. A few keep what they're doing elsewhere too (a byte of the moby, a timer
//  sharing its word with other bytes): those go along for the movers listed by the code of their update. Players
//  about as near to one settle which game sends it by their games' ranks, like enemies (see EnemySync).
//
// The same goes for things that don't move but change their state while the level runs: doors opening, buttons,
//  bridges, hazards going off. Those also take on the state of the game whose player is nearest, once they've been
//  in their own for a moment, unless our player is right next to them (what's going on with them there is up to us).
//  Only things that are drawn: the level's invisible triggers (cutscenes, the camera...) are never touched.
//
// Something busy with the sender's player (a critter reacting to them, something following them) holds their
//  address: other games put in their copy of that player, so it's busy with the same player there.
//
// When one of those changes its state by itself (a door opens because a puzzle was solved, a button goes down), the
//  game it happened in tells the others right away, and they make the same change, wherever the players are: the
//  game whose player is nearest may not be the one where it happened. Things that change state all the time (lights
//  blinking) don't flood the others with every change, they're kept in step by the states now and then.

#ifndef RAC1_MULTIPLAYER_MOVERSYNC_H
#define RAC1_MULTIPLAYER_MOVERSYNC_H

#include <lib/types.h>
#include <rc1/rc1.h>
#include <rc1/game/Moby.h>

#include "network/Packet.h"

struct MoverSync {
    // From our moby update loop (see EnemySync), for the level's mobys that move on their own or change their state,
    //  and aren't enemies: before its update, and after it with how far our player is from it
    static void before_update(Moby* moby, int index);
    static void after_update(Moby* moby, int index, float distance, bool moves, bool changes_state, s8 state_before);

    // Sends what was queued this frame, after the loop
    static void send(u32 frame);

    // Our moby table indexes of the movers another game is keeping in step for us now, up to `max`: they run here
    //  wherever they are, like the enemies other games run, so one that went off towards that game's player (a
    //  critter it woke up) doesn't stay behind out of our reach
    static int remote_indexes(u16* out, int max);

    static void receive(MPPacketMovers* packet, size_t len);
    static void on_tick();

    // New level or respawn
    static void reset();
};

#endif //RAC1_MULTIPLAYER_MOVERSYNC_H
