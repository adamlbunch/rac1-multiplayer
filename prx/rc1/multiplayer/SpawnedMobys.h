//
// Mobys the level's own mobys spawn while playing: enemies coming out of spawners, the pieces an enemy splits into
//  and so on. Unlike the ones placed in the level they have no UID of their own and land anywhere in the moby table,
//  so games know them by which of the level's mobys spawned them, and how many it had spawned before. That's the same
//  in every game as long as the spawner does the same thing in each, which it does when its enemies are in sync.
//

#ifndef RAC1_MULTIPLAYER_SPAWNEDMOBYS_H
#define RAC1_MULTIPLAYER_SPAWNEDMOBYS_H

#include <lib/types.h>
#include <rc1/rc1.h>
#include <rc1/game/Moby.h>

struct SpawnedMobys {
    // From the hook on the game's function that spawns mobys, with the moby whose update spawned it if any
    static void on_spawned(Moby* moby, Moby* spawner);

    // What other games know a moby by: the UID of a moby placed in the level and 0, or for a moby one of those
    //  spawned, the spawner's UID and how many it had spawned before plus one. False if other games can't know it.
    static bool identify(Moby* moby, u16* uid, u16* spawn);

    // The live moby other games know by `uid` and `spawn`, of that class
    static Moby* find(u16 uid, u16 spawn, u16 o_class);

    // New level or respawn
    static void reset();
};

#endif //RAC1_MULTIPLAYER_SPAWNEDMOBYS_H
