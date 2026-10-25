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
};

// The animation we last started on a copy, to tell a new animation from one already playing
struct MirrorAnimation {
    bool valid;
    u8 seq;
    u8 restarts;
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
    MPMirrorMoby hero_state;
    u8 n_manipulators;
    MPMirrorManipulator manipulator_state[MP_MIRROR_MAX_MANIPULATORS];

    MirrorAnimation hero_animation;
    bool hero_manipulated;
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

    // A player's hero moby was deleted
    static void forget(u16 uuid);

    // The level is gone (respawn, level change, disconnect), and with it every moby we spawned
    static void reset();

    // From the hook on the game's function that starts the player's animations
    static void on_hero_animation_started(u32 blend_frames);
};

#endif //RAC1_MULTIPLAYER_MIRROR_H
