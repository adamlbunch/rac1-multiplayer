//
// Lets everyone hear what other players and the enemies other games run do. The game plays a moby's sounds from its
//  update: footsteps, the wrench, weapons firing, an enemy growling or attacking. Other players' copies (see Mirror)
//  and the enemies another game runs (see EnemySync) don't run their update here, so they were silent.
//
// So when our player, what they hold or fired, or an enemy we run for everyone plays a sound, we send which sound of
//  which moby it was, and other games play the same sound on their copy of that moby, at the frame of it they show.
//  A sound that keeps playing until it's stopped (a flamethrower, an engine) is stopped there when it's stopped here.
//  An enemy another game runs that reacts to a hit here on its own (see EnemySync) is kept quiet meanwhile: what it
//  sounds like comes from that game too.
//

#ifndef RAC1_MULTIPLAYER_SOUNDSYNC_H
#define RAC1_MULTIPLAYER_SOUNDSYNC_H

#include <lib/types.h>
#include <rc1/rc1.h>
#include <rc1/game/Moby.h>

#include "network/Packet.h"

struct SoundSync {
    // From the hooks on the game's sound functions. A sound that shouldn't play here isn't played.
    static bool should_play(Moby* moby);
    static void on_played(u8 function, s32 index, s32 flags, Moby* moby, u16 sound_class, s32 handle);
    static void on_stopped(s32 handle);

    // Sends what was played this frame, once our player has updated. Also right before one of our synced mobys goes
    //  away, so the sounds it made on its way out reach other games while they still have their copy of it.
    static void end_frame();

    static void receive(MPPacketSounds* packet, size_t len);

    // Plays the sounds other games sent that are due, before mobys update
    static void on_tick();

    // New level or respawn
    static void reset();
};

#endif //RAC1_MULTIPLAYER_SOUNDSYNC_H
