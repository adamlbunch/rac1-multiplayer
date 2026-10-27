//
// Shows other players' weapon effects: the Blaster's bolts, the Pyrocitor's flames, the Tesla Claw's lightning, the
//  sparks and smoke of everything they fire, and so on. None of those are mobys. The game keeps them as small records
//  in an effect pool, each holding all there is to the effect in world coordinates, and animates them from there.
//
// So when our player, the weapons they hold or the things they fired make effects, we send the effect records. The
//  other games make the same records, at the frame of that player's replayed pose they belong with (see Mirror), and
//  the game animates them just like ours. Some effects stay with a moby (on one of its bones, say) and hold its
//  address, which means something else in other games: those are sent as which moby it is (the player, what they
//  hold, a moby placed in the level), and other games put in their own copy of it. Same for the enemies our game runs for everyone (see EnemySync) and what
//  they fire: other games only show what those make here, not what their own copy makes reacting to a hit.
//

#ifndef RAC1_MULTIPLAYER_EFFECTSYNC_H
#define RAC1_MULTIPLAYER_EFFECTSYNC_H

#include <lib/types.h>
#include <rc1/rc1.h>
#include <rc1/game/Moby.h>

#include "network/Packet.h"

struct EffectSync {
    // Around our copy of the game's moby update loop (see EnemySync) and the update of our player, which the game
    //  runs on its own after the loop, with what they hold
    static void begin_frame();
    static void end_frame();
    static void before_player_update();
    static void after_player_update();
    static bool player_updating();

    // From the hooks on the game's functions that make effect records and spawn mobys
    static void on_allocated(u8* effect);
    static void on_spawned(Moby* moby);

    // Around the update of each moby in the update loop. A moby can have the effects it made in its update undone.
    static void before_moby_update();
    static void after_moby_update(Moby* moby);
    static void discard_moby_effects();

    // An enemy another game runs spawned this reacting to a hit here (an explosion, debris...). Its effects come from
    //  that game too, so the ones it makes here are undone.
    static void mute(Moby* moby);

    static void receive(MPPacketEffects* packet, size_t len);

    // Makes the effects other players' games sent that are due, before mobys update
    static void on_tick();

    // New level or respawn
    static void reset();
};

#endif //RAC1_MULTIPLAYER_EFFECTSYNC_H
