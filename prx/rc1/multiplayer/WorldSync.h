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

// The game records damage in a ring of 64 records. Whatever hurts a moby fills in the next record and points the
//  moby's `damage` at it, and the moby reads it in its update.
#define DAMAGE_RECORDS ((u8*)0xa14c00)
#define DAMAGE_RECORD_SIZE 0x40
#define DAMAGE_RECORD_COUNT 64
#define DAMAGE_NEXT_RECORD (*(u32*)0xa10954)

struct Client;

struct WorldSync {
    // Picks up the hits our player made since last frame, and notices mobys they hit disappearing
    static void on_tick(Client* client);

    static void receive_hit(MPPacketMobyHit* hit, size_t len);
    static void receive_killed(MPPacketMobyKilled* killed);

    // The server changed a byte of a level's record of the mobys gone for good, as another player's game did. Mobys
    //  it adds that are still here on our level are removed: the game only reads that record as the level loads, so
    //  a gate another player's progress opened would otherwise stay shut until we reload.
    static void on_gone_for_good(int level, int offset, u8 before, u8 after);

    // Another player showed up on our level: they're told what was destroyed here before they came
    static void on_player_arrived();

    // A damage record that isn't a real hit, so it's not passed on
    static void ignore_record(u32 index);

    // New level or respawn
    static void reset();
};

#endif //RAC1_MULTIPLAYER_WORLDSYNC_H
