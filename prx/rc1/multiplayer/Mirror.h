//
// Draws other players exactly like their own games do. Every frame each game sends a snapshot of its hero moby
//  (animation frame and blend, bone manipulators, transform, lighting) and of everything attached to the player:
//  the item in their hand, packs, helmets, boots and so on. Other games buffer a few frames of snapshots to smooth
//  out the network and replay them one per frame on their copy of the player.
//

#ifndef RAC1_MULTIPLAYER_MIRROR_H
#define RAC1_MULTIPLAYER_MIRROR_H

#include <lib/types.h>
#include <rc1/rc1.h>
#include <rc1/game/Moby.h>

#include "MobyAnimation.h"
#include "network/Packet.h"

struct Client;

#define MIRROR_MAX_PLAYERS 4
#define MIRROR_BUFFER_FRAMES 8

struct MirrorSnapshot {
    u32 frame;
    bool used;
    bool has_hero;
    u16 attachments;    // Slots the sender has this frame
    u16 received;       // Slots we got the state of
    MPMirrorMoby hero;
    u8 n_manipulators;
    MPMirrorManipulator manipulators[MP_MIRROR_MAX_MANIPULATORS];
    MPMirrorMoby attachment[MP_MIRROR_ATTACHMENT_SLOTS];

    // What the hero has on top of its animation that comes in a part of its own (see MPPacketPlayerState)
    u8 n_extra_manipulators;
    u8 n_layers;
    bool has_extras;    // That part arrived
    MPMirrorManipulator extra_manipulators[MP_MIRROR_MAX_EXTRA_MANIPULATORS];
    MPMirrorLayer layers[MP_MIRROR_MAX_LAYERS];
};

struct RemotePlayer {
    bool active;
    u16 uuid;
    u32 newest_frame;
    u32 playback_frame;
    u32 last_packet_tick;
    MirrorSnapshot buffer[MIRROR_BUFFER_FRAMES];

    // What we show this frame, copied out of the buffer so packets arriving can't change it under us
    bool showing;
    Moby* arrived_hero;     // Their hero moby here when we last noticed them arrive on our level
    MPMirrorMoby hero_state;
    u8 n_manipulators;
    MPMirrorManipulator manipulator_state[MP_MIRROR_MAX_MANIPULATORS];
    u8 n_extra_manipulators;
    MPMirrorManipulator extra_manipulator_state[MP_MIRROR_MAX_EXTRA_MANIPULATORS];
    u8 n_layers;
    MPMirrorLayer layer_state[MP_MIRROR_MAX_LAYERS];

    MirrorAnimation hero_animation;
    bool hero_manipulated;
    bool hero_layered;
    Moby* attachment_mobys[MP_MIRROR_ATTACHMENT_SLOTS];
    MobyClass* attachment_models[MP_MIRROR_ATTACHMENT_SLOTS];  // What each copy must be drawn with, see GadgetModels
    MPMirrorMoby attachment_state[MP_MIRROR_ATTACHMENT_SLOTS];
    MirrorAnimation attachment_animation[MP_MIRROR_ATTACHMENT_SLOTS];
};

struct Mirror {
    // Sends this frame's snapshot of our player
    static void send_snapshot(Client* client);

    static void receive(MPPacketPlayerState* packet, size_t len);

    // Picks the snapshot each copy shows this frame and spawns or removes attachments. Runs before mobys update.
    static void on_tick();

    // From the update of another player's hero moby. Returns true if it's mirrored this frame.
    static bool apply_hero(Moby* moby, u16 uuid);

    // Update of the mobys we spawn as copies of other players' attachments
    static void update_attachment(Moby* moby);

    // Whether snapshots are positioning this player's hero, so regular moby updates should leave it alone
    static bool is_mirrored(u16 uuid);

    // How far the nearest other player we're showing is from `position`, squared. Huge if there's nobody.
    static float nearest_player_distance_sq(const Vec4* position);

    // The number of the snapshot of our player we sent last
    static u32 sent_frame();

    // Which of a player's frames we're showing now, false if we aren't showing them
    static bool shown_frame(u16 uuid, u32* frame);

    // Our copy of a player's hero, or of what they have on them in an attachment slot. Null if there's none.
    static Moby* hero_of(u16 uuid);
    static Moby* attachment_of(u16 uuid, int slot);

    // A player's hero moby was deleted
    static void forget(u16 uuid);

    // Removes the copies of other players' weapons drawn with our own loads of them (see GadgetModels)
    static void drop_gadget_models();

    // The level is gone (respawn, level change, disconnect), and with it every moby we spawned
    static void reset();

    // From the hook on the game's function that starts the player's animations
    static void on_hero_animation_started(u32 blend_frames);
};

#endif //RAC1_MULTIPLAYER_MIRROR_H
