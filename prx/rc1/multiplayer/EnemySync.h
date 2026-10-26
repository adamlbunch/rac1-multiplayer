//
// Every game runs the level's enemies on its own, so left alone each player fights their own copy of every enemy:
//  somewhere else, chasing whoever is in its own game. Instead, each enemy is run by one player's game at a time,
//  normally the game of the player nearest to it. That game runs it like it always does and sends its state every
//  frame: where it is, which way it's facing, its animation and the state its AI is in. The other games don't run its
//  AI and draw it from that state instead, a few frames behind to smooth out the network, like other players (see
//  Mirror).
//
// A game takes over the enemies its player is nearest to. When two games both send an enemy, the one whose player is
//  nearer keeps it. A game that stops sending an enemy (its player went away, or left) leaves it to the others.
//
// Hits still reach every game (see WorldSync), so health and deaths are the same everywhere. A game that doesn't run
//  an enemy lets it react to hits on its own, so it flinches, dies and drops its bolts there too. Whatever damage it
//  does meanwhile is undone, since that enemy is fighting someone in another game.
//
// What counts as an enemy: a moby placed in the level, or spawned by one (see SpawnedMobys), that reads the damage
//  done to it and moves on its own.
//
// To do all this, the mod runs the game's moby update loop itself (see `update_mobys`), the same way the game does,
//  with a look at each moby before and after its update.
//

#ifndef RAC1_MULTIPLAYER_ENEMYSYNC_H
#define RAC1_MULTIPLAYER_ENEMYSYNC_H

#include <lib/types.h>
#include <rc1/rc1.h>
#include <rc1/game/Moby.h>

#include "network/Packet.h"

struct Client;

// Parts of the syncing the test tools can switch off in a running game, to find what causes a problem (see the
//  marker in EnemySync.cpp). All on normally.
#define SYNC_OFF_UPDATE_LOOP 0x1      // The game runs its own moby update loop: no enemy syncing at all
#define SYNC_OFF_REMOTE_ENEMIES 0x2   // Every enemy runs here, none is drawn from another game
#define SYNC_OFF_EFFECT_REPLAY 0x4    // Effects other games send aren't made
#define SYNC_OFF_EFFECT_CAPTURE 0x8   // Effects aren't sent
#define SYNC_OFF_ENEMY_SENDING 0x10   // Enemies aren't sent or taken over
#define SYNC_OFF_STATE_COPY 0x20      // Enemies drawn from another game don't take on its AI state

extern u32 sync_off_;

struct EnemySync {
    // Our copy of the game's moby update loop. Returns false when the game should run its own (its profiling mode).
    static bool update_mobys();

    // The moby whose update is running, or null
    static Moby* updating();

    static void on_tick(Client* client);
    static void receive(MPPacketEnemyState* packet, size_t len);

    // Whether our game runs this moby for everyone
    static bool is_owned(Moby* moby);

    // Whether this moby is drawn from another game's states, or reacting to a hit in its own way meanwhile
    static bool is_remote(Moby* moby);

    // From the hooks on the game's functions that start a moby's animation and read the damage done to a moby
    static void on_animation_started(Moby* moby, u32 blend_frames);
    static void on_get_damage(Moby* moby);
    static void on_spawned(Moby* moby);

    // New level or respawn
    static void reset();
};

#endif //RAC1_MULTIPLAYER_ENEMYSYNC_H
