//
// Shows other players' weapon effects: the Blaster's bolts, the Pyrocitor's flames, the Tesla Claw's lightning, the
//  sparks and smoke of everything they fire, and so on. None of those are mobys. The game keeps them as small records
//  in an effect pool, each holding all there is to the effect in world coordinates, and animates them from there.
//
// So when our player, the weapons they hold or the things they fired make effects, we send the effect records. The
//  other games make the same records, at the frame of that player's replayed pose they belong with (see Mirror), and
//  the game animates them just like ours.
//

#ifndef RAC1_MULTIPLAYER_EFFECTSYNC_H
#define RAC1_MULTIPLAYER_EFFECTSYNC_H

#include <lib/types.h>
#include <rc1/rc1.h>
#include <rc1/game/Moby.h>

#include "network/Packet.h"

struct EffectSync {
    // Around our copy of the game's moby update loop, see EnemySync
    static void begin_frame();
    static void end_frame();

    // From the hooks on the game's functions that make effect records and spawn mobys
    static void on_allocated(u8* effect);
    static void on_spawned(Moby* moby);

    static void receive(MPPacketEffects* packet, size_t len);

    // Makes the effects other players' games sent that are due, before mobys update
    static void on_tick();

    // New level or respawn
    static void reset();
};

#endif //RAC1_MULTIPLAYER_EFFECTSYNC_H
