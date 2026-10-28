//
// Lets one player skip a cutscene for everyone watching it. Each game plays its own cutscenes (the game's opening, a
//  planet's arrival, a character talking to the player), so players who got there together watch the same one side
//  by side. When one of them skipped it, the others still had to sit through it, or skip it too.
//
// So when our player skips a cutscene, we tell the others, and their game skips the same one the way Start would
//  there: right away if they're watching it, or as soon as it starts if that's within a few seconds (their game was
//  a bit slower to get there). A different cutscene they're watching plays on.
//
// The game has two kinds, both skipped with Start:
//  - Cutscenes played in the level (game state CutScene): the game updates one each frame, and Start ends it.
//  - Full-screen movies (game state Movie: infobots, help, story; and the story movies played while the next planet
//    loads): one function plays the whole movie, reading the pad each frame until Start is held or pressed. Another
//    player's skip of the same movie holds Start for us there.
//

#ifndef RAC1_MULTIPLAYER_CUTSCENESYNC_H
#define RAC1_MULTIPLAYER_CUTSCENESYNC_H

#include <lib/types.h>

#include "network/Packet.h"

struct CutsceneSync {
    // From the hook on the game's update of a cutscene played in the level, before each frame of it. True when
    //  another player skipped it: it's ended now instead, like Start ends it. Our player's Start is told to the others.
    static bool should_end_cutscene();

    // From the hooks on the game's movies: the file one is opened from, and the functions that play one from start to
    //  end. `skip_buttons` is the button word they end the movie on when it has Start (held or pressed), null after.
    static void on_movie_opened(const char* path);
    static void on_movie_playing(u32* skip_buttons);

    // From the hook on the game's reading of the pads, right before a movie checks them
    static void after_pads_read();

    static void receive(MPPacketCutsceneSkipped* packet, size_t len);
};

#endif //RAC1_MULTIPLAYER_CUTSCENESYNC_H
