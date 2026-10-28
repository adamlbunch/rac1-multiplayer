//
// Every game runs the level's enemies on its own, so left alone each player fights their own copy of every enemy:
//  somewhere else, chasing whoever is in its own game. Instead, one game runs them all, the host of the level: the
//  first game on the level, for as long as it's there. It runs them like it always does and sends their state every
//  frame: where they are, which way they face, their animation and the state their AI is in. The other games don't
//  run their AI and draw them from those states instead, a few frames behind to smooth out the network, like other
//  players (see Mirror).
//
// An enemy's AI keeps more than its state (timers, where it's headed, what it's about to do) in variables of its own,
//  which can't be copied to another game: they hold addresses and table positions that are different there. So an
//  enemy stays in the game that runs it. It only moves to another when the host leaves the level, or when the host's
//  game doesn't have it (it's already gone there, or something only another player set off brought it out): then the
//  game that has it runs it for everyone, until the host's states of it come.
//
// The game's enemies go after the player in its globals. The host's game runs the enemies near other players too, and
//  while each one's update runs, those globals are the nearest player's, so it goes after them like it would in
//  their own game. What it hits on them there, they take in their game (see WorldSync).
//
// Hits still reach every game (see WorldSync), so health and deaths are the same everywhere. A game that doesn't run
//  an enemy lets it react to hits on its own, so it flinches, dies and drops its bolts there too. Whatever damage it
//  does meanwhile is undone, since that enemy is fighting in another game.
//
// What counts as an enemy: a moby placed in the level, or spawned by one (see SpawnedMobys), that reads the damage
//  done to it, moves on its own and has an update that can hurt something (see AttackerUpdates.h).
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
#define SYNC_OFF_MOVERS 0x40          // Moving things that aren't enemies aren't kept in step (see MoverSync)

extern u32 sync_off_;

struct EnemySync {
    // Our copy of the game's moby update loop. Returns false when the game should run its own (its profiling mode).
    static bool update_mobys();

    // The moby whose update is running, or null
    static Moby* updating();

    // Our player's moby. ratchet_moby is another player's copy while an enemy goes after them (see update_mobys).
    static Moby* own_hero();

    static void on_tick(Client* client);
    static void receive(MPPacketEnemyState* packet, size_t len);

    // Whether our game runs this moby for everyone
    static bool is_owned(Moby* moby);

    // Whether it's an enemy our game runs for everyone, or spawned by one (a shot): what it hits is hit everywhere
    static bool runs_for_everyone(Moby* moby);

    // The random number our game goes by, see MPPacketEnemyState
    static u32 rank();

    // The rank of the game whose player this moby is our copy of, 0 if it's no player's or we don't know theirs
    static u32 player_rank(Moby* moby);

    // Whether this moby is drawn from another game's states, or reacting to a hit in its own way meanwhile
    static bool is_remote(Moby* moby);

    // Whether this is a copy we made of something another game's enemy spawned
    static bool is_copy(Moby* moby);

    // The frame of our moby update loop, which the enemy states we send are numbered by
    static u32 frame();

    // Which of a game's frames we're showing its enemies at, false if it doesn't send us any
    static bool shown_frame(u16 sender, u32* frame);

    // From the hooks on the game's functions that start a moby's animation and read the damage done to a moby
    static void on_animation_started(Moby* moby, u32 blend_frames);
    static void on_get_damage(Moby* moby);
    static void on_spawned(Moby* moby);

    // New level or respawn
    static void reset();
};

#endif //RAC1_MULTIPLAYER_ENEMYSYNC_H
