//
// The mobys placed in the level. The game's moby table starts with them and continues with the ones spawned while
//  playing. Every game has the same ones under the same UIDs, which are unique among them, but not always in the same
//  place in the table: when a player respawns, their game rebuilds the table without the crates they broke.
//

#ifndef RAC1_MULTIPLAYER_LEVELMOBYS_H
#define RAC1_MULTIPLAYER_LEVELMOBYS_H

#include <lib/types.h>
#include <rc1/rc1.h>
#include <rc1/game/Moby.h>

#define LEVEL_MOBYS_START (*(Moby**)0xa390a0)
#define LEVEL_MOBYS_END (*(Moby**)0xa390a4)

// MPMoby's update function: mobys running it are our copies of other players and their things
#define LEVEL_MOBYS_MP_UPDATE_FUNC ((void*)0x710ed8)

struct LevelMobys {
    static inline u32 count() {
        return LEVEL_MOBYS_END - LEVEL_MOBYS_START;
    }

    // Position of the moby in our table, or -1 if it isn't one of the level's own mobys (or is our player, or one of
    //  our copies of other players' things)
    static inline int index_of(Moby* moby) {
        if (moby < LEVEL_MOBYS_START || moby >= LEVEL_MOBYS_END || moby == ratchet_moby ||
            moby->p_update == LEVEL_MOBYS_MP_UPDATE_FUNC) {
            return -1;
        }

        return moby - LEVEL_MOBYS_START;
    }

    // The level's moby with this UID if it's alive and of that class
    static Moby* find(u16 uid, u16 o_class);
};

#endif //RAC1_MULTIPLAYER_LEVELMOBYS_H
