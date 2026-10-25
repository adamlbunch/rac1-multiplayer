//
// Copies a moby's animation exactly: the frames it's between, the blend from its previous animation, and how fast
//  it's moving along. Used to draw other players (Mirror) and the enemies other games run (EnemySync) exactly like
//  their own game does.
//

#ifndef RAC1_MULTIPLAYER_MOBYANIMATION_H
#define RAC1_MULTIPLAYER_MOBYANIMATION_H

#include <lib/types.h>
#include <rc1/rc1.h>
#include <rc1/game/Moby.h>

#include "network/Packet.h"

// The animation we last started on a copy, to tell a new animation from one already playing
struct MirrorAnimation {
    bool valid;
    u8 seq;
    u8 restarts;
};

struct MobyAnimation {
    static void capture(Moby* moby, MPMobyAnimation* out);

    // Puts the moby in the captured animation state. Restarts of the same animation are only told apart when
    //  `track_restarts` is set, for animations started through the hooks that count them.
    static void apply(Moby* moby, const MPMobyAnimation* state, MirrorAnimation* applied, bool track_restarts);

    // The animation sequence `seq` of the moby's class, or null if it doesn't have one
    static MobySeq* sequence(Moby* moby, u8 seq);
};

#endif //RAC1_MULTIPLAYER_MOBYANIMATION_H
