//
// Models for other players' weapons and gadgets. The game keeps all of them in memory, but only loads the one the
//  player is holding into a buffer it can draw from, so a copy of another player's weapon would be drawn with
//  whatever our own player is holding (and crash when that changes). We load other players' ones into buffers of
//  our own, the same way the game does.
//

#ifndef RAC1_MULTIPLAYER_GADGETMODELS_H
#define RAC1_MULTIPLAYER_GADGETMODELS_H

#include <lib/types.h>
#include <rc1/game/Moby.h>

struct GadgetModels {
    // Whether the game swaps this moby class's model in and out of its gadget buffer
    static bool is_gadget(u16 o_class);

    // Our own loaded copy of a gadget's model, shared by every other player holding it. Returns null if it can't be
    //  loaded, which happens when all of our model memory was drawn from in the last few frames.
    static MobyClass* load(u16 o_class);

    // A copy is drawn with this model this frame, so it isn't loaded over
    static void touch(MobyClass* model);

    static void on_tick();

    // Whether a model is one of our loaded copies
    static bool owns(MobyClass* model);

    // The game ran out of room in one of its memory areas. If it's the one our models are in, they give it back to
    //  the game and aren't loaded again until the level is set up anew. Returns whether there's more room now.
    static bool give_back(u32* heap);

    // The level is set up anew (level change or respawn)
    static void reset();

    // The model a regular (non-gadget) moby class has loaded in our game, or null if it isn't loaded
    static MobyClass* loaded_class(u16 o_class);

    // Spawns a moby using the given model, whatever the game has loaded for its class
    static Moby* spawn_with_class(u16 o_class, MobyClass* model, u16 flags);
};

#endif //RAC1_MULTIPLAYER_GADGETMODELS_H
