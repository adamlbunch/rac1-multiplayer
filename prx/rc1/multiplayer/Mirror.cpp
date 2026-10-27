//
// See Mirror.h
//

#include "Mirror.h"

#include <rc1/Game.h>

#include "MPMoby.h"
#include "GadgetModels.h"
#include "WorldSync.h"
#include "network/GameClient.h"

// Gadget slots: the game keeps up to 2 mobys per slot on the player (hand, back, head, feet...), 0x50 bytes apart
#define GADGET_SLOTS ((u8*)0x96ad70)
#define GADGET_SLOT_STRIDE 0x50

// Frames of snapshots we keep in hand before showing one. Smooths over packets arriving unevenly.
#define MIRROR_DELAY 3

// Without snapshots for this many frames, a player goes back to being drawn from regular moby updates
#define MIRROR_TIMEOUT 60

// MPMoby's update function, which our attachment copies run
#define MP_MOBY_UPDATE_FUNC ((void*)0x710ed8)

static RemotePlayer remote_players_[MIRROR_MAX_PLAYERS];

// The game applies manipulators with vector instructions, so they need 16 byte alignment
static Manipulator manipulators_[MIRROR_MAX_PLAYERS][MP_MIRROR_MAX_MANIPULATORS + MP_MIRROR_MAX_EXTRA_MANIPULATORS]
    __attribute__((aligned(16)));

// Copies whose player is gone, deleted at the next tick: deleting a moby during the update loop could break the
//  loop's walk through the moby list
#define MIRROR_MAX_ORPHANS 16
static Moby* orphans_[MIRROR_MAX_ORPHANS];

// Some things on the player aren't posed from their own animation: every frame the game poses them from the player's
//  joints into a pose buffer of its own and points the moby's frame pointers at it. Their own animation alone shows
//  the bare model (the Bomb Glove as just its bomb), so copies are posed the same way, from our copy of the player.
struct JointPose {
    u32 game_pose;      // The game's pose buffer for this kind
    const s32* joints;  // The player's joints it takes, ending in -1
};

#define JOINT_POSE_KINDS 4
#define JOINT_POSE_SIZE 0x200

static const JointPose joint_poses_[JOINT_POSE_KINDS] = {
    {0xa1b880, (const s32*)0x721afc},   // MP_MIRROR_POSE_GLOVE: Bomb, Mine, Doom and Decoy Glove
    {0xa1ba80, (const s32*)0x721b88},   // MP_MIRROR_POSE_HEAD: Sonic Summoner, O2 Mask, Pilot's Helmet
    {0xa1bc80, (const s32*)0x721b98},   // MP_MIRROR_POSE_BOOT: Magneboots and Grindboots, one buffer per boot
    {0xa1bd80, (const s32*)0x721bb0},   // MP_MIRROR_POSE_OTHER_BOOT
};

// One thing on the head takes more of the head's joints, and 6 left unposed
#define HEAD_WITH_MORE_JOINTS 0x1b1
#define HEAD_MORE_JOINTS ((const s32*)0x721b44)
#define HEAD_MORE_BLANK_JOINTS 6

// While posing boots the game sets these two groups of 3 values of the player's to 1
#define BOOT_POSE_VALUES ((float*)0x721eb0)
#define BOOT_POSE_VALUES2 ((float*)0x721f60)

// The player's joint lists are the hero's, Ratchet's, own
#define HERO_O_CLASS 0

static u8 joint_pose_buffers_[MIRROR_MAX_PLAYERS][JOINT_POSE_KINDS][JOINT_POSE_SIZE] __attribute__((aligned(16)));

// Animation layers: animations the game plays on a set of a moby's joints over its own, like the hero's arm raised
//  holding a weapon. The moby's +0x60 points at the first, each at the next. The game keeps the player's in a pool of
//  6 at 0xa2c440. Copies of other players get theirs from us, played on the frames their game is on.
#define LAYER_SIZE 0x40
#define LAYER_JOINT_SET 0x00            // s16: which of the class's sets of joints, listed at the class's +0x1c
#define LAYER_JOINT_COUNT 0x02          // u16
#define LAYER_IN_USE 0x04
#define LAYER_FRAME0_FROM_SOURCE 0x06
#define LAYER_FRAME1_FROM_SOURCE 0x07
#define LAYER_WEIGHT 0x08               // float
#define LAYER_JOINTS 0x0c               // The set's joint numbers
#define LAYER_RESULT 0x10               // Where its joints are worked out, LAYER_RESULT_SIZE each
#define LAYER_CLASS 0x14
#define LAYER_SOURCE_CLASS 0x18         // Whose sequences the frames flagged above are of
#define LAYER_NEXT 0x1c
#define LAYER_ANIMATION 0x20            // frame0, frame1, prev_seq, seq, like a moby's +0x50
#define LAYER_T 0x24
#define LAYER_SPEED 0x28
#define LAYER_RATE 0x2c
#define LAYER_FRAME0 0x38               // Frame data, or the buffer a source class's frame is built in
#define LAYER_FRAME1 0x3c

#define LAYER_RESULT_SIZE 0x30
#define LAYER_MAX_JOINTS 48
#define LAYER_FRAME_SIZE 0x400

// The game's pool size
#define MIRROR_MAX_HERO_LAYERS 6

struct LayerMemory {
    u8 layer[LAYER_SIZE];
    u8 result[LAYER_MAX_JOINTS * LAYER_RESULT_SIZE];
    u8 frames[2][LAYER_FRAME_SIZE];
};

static LayerMemory layer_memory_[MIRROR_MAX_PLAYERS][MP_MIRROR_MAX_LAYERS] __attribute__((aligned(16)));

static u32 frame_ = 0;
static u32 ticks_ = 0;
static u8 hero_animation_restarts_ = 0;
static u8 hero_blend_frames_ = 0;

static bool valid_moby(Moby* moby) {
    u32 address = (u32)moby;

    return address >= 0x10000 && address < 0xC0000000 && (address & 0xf) == 0 &&
           moby->state >= 0 && moby->p_class != nullptr;
}

static u8 joint_pose_kind(Moby* moby) {
    u32 frame0 = (u32)moby->field41_0x68;
    if (frame0 != (u32)moby->field42_0x6c) {
        return MP_MIRROR_POSE_ANIMATION;
    }

    for (u8 i = 0; i < JOINT_POSE_KINDS; i++) {
        if (joint_poses_[i].game_pose == frame0) {
            return i + 1;
        }
    }

    return MP_MIRROR_POSE_ANIMATION;
}

// Poses a copy from our copy of its player's joints, the way the game poses the real one. False if it can't be.
static bool pose_from_joints(Moby* moby, u8 kind, Moby* hero, u8* pose) {
    if (kind == MP_MIRROR_POSE_ANIMATION || kind > JOINT_POSE_KINDS || hero == nullptr || !valid_moby(hero) ||
        hero->o_class != HERO_O_CLASS || hero->field41_0x68 == nullptr || hero->field42_0x6c == nullptr) {
        return false;
    }

    const s32* joints = joint_poses_[kind - 1].joints;
    s32 blank_joints = 0;
    if (kind == MP_MIRROR_POSE_HEAD && moby->o_class == HEAD_WITH_MORE_JOINTS) {
        joints = HEAD_MORE_JOINTS;
        blank_joints = HEAD_MORE_BLANK_JOINTS;
    }

    bool boot = kind == MP_MIRROR_POSE_BOOT || kind == MP_MIRROR_POSE_OTHER_BOOT;
    float saved[6];
    if (boot) {
        for (int i = 0; i < 3; i++) {
            saved[i] = BOOT_POSE_VALUES[i];
            saved[3 + i] = BOOT_POSE_VALUES2[i];
            BOOT_POSE_VALUES[i] = 1.0f;
            BOOT_POSE_VALUES2[i] = 1.0f;
        }
    }

    pose_from_player_joints(joints, pose, moby->p_class, blank_joints, hero);

    if (boot) {
        for (int i = 0; i < 3; i++) {
            BOOT_POSE_VALUES[i] = saved[i];
            BOOT_POSE_VALUES2[i] = saved[3 + i];
        }
    }

    // Like the game: both frames are the pose, and nothing for the animation step to move along
    u8* m = (u8*)moby;
    moby->field41_0x68 = pose;
    moby->field42_0x6c = pose;
    memset(m + 0x50, 0, 4);
    float zero = 0.0f;
    memcpy(m + 0x54, &zero, sizeof(float));
    memcpy(m + 0x5c, &zero, sizeof(float));

    return true;
}

static bool frame_exists(MobyClass* model, u8 seq, u8 frame) {
    // Sequence count at 0xc, sequence pointers from 0x48
    u8* m = (u8*)model;
    if (seq >= m[0xc]) {
        return false;
    }

    MobySeq* sequence = ((MobySeq**)(m + 0x48))[seq];
    return sequence != nullptr && frame < sequence->frame_count;
}

// Sets one of a player's animation layers up on our copy of their hero, and works out its joints. False if it can't be.
static bool set_up_layer(LayerMemory* memory, Moby* hero, MPMirrorLayer* state) {
    MobyClass* model = hero->p_class;
    u8* m = (u8*)model;

    // The class's sets of joints: a count, then one pointer each. A frame built for a layer has 8 bytes per joint.
    s32* sets = *(s32**)(m + 0x1c);
    if (sets == nullptr || state->joint_set < 0 || state->joint_set >= sets[0] ||
        m[8] * 8 + 0x18 > LAYER_FRAME_SIZE) {
        return false;
    }

    // A set: its joint count at +3, its joints +4 past the offset at +1
    u8* set = (u8*)sets[1 + state->joint_set];
    if (set == nullptr || set[3] > LAYER_MAX_JOINTS) {
        return false;
    }

    MobyClass* source = nullptr;
    if (state->source_o_class != 0xffff) {
        source = GadgetModels::is_gadget(state->source_o_class) ? GadgetModels::load(state->source_o_class)
                                                                 : GadgetModels::loaded_class(state->source_o_class);
    }

    bool frame0_from_source = (state->from_source & MP_MIRROR_LAYER_FRAME0_FROM_SOURCE) != 0;
    bool frame1_from_source = (state->from_source & MP_MIRROR_LAYER_FRAME1_FROM_SOURCE) != 0;
    MobyClass* frame0_model = frame0_from_source ? source : model;
    MobyClass* frame1_model = frame1_from_source ? source : model;
    if (frame0_model == nullptr || frame1_model == nullptr ||
        !frame_exists(frame0_model, state->prev_seq, state->frame0) ||
        !frame_exists(frame1_model, state->seq, state->frame1)) {
        return false;
    }

    u8* layer = memory->layer;
    memset(layer, 0, LAYER_SIZE);
    *(s16*)(layer + LAYER_JOINT_SET) = state->joint_set;
    *(u16*)(layer + LAYER_JOINT_COUNT) = set[3];
    layer[LAYER_IN_USE] = 1;
    layer[LAYER_FRAME0_FROM_SOURCE] = frame0_from_source;
    layer[LAYER_FRAME1_FROM_SOURCE] = frame1_from_source;
    memcpy(layer + LAYER_WEIGHT, &state->weight, sizeof(float));
    *(u8**)(layer + LAYER_JOINTS) = set + set[1] + 4;
    *(u8**)(layer + LAYER_RESULT) = memory->result;
    *(MobyClass**)(layer + LAYER_CLASS) = model;
    *(MobyClass**)(layer + LAYER_SOURCE_CLASS) = source;
    layer[LAYER_ANIMATION] = state->frame0;
    layer[LAYER_ANIMATION + 1] = state->frame1;
    layer[LAYER_ANIMATION + 2] = state->prev_seq;
    layer[LAYER_ANIMATION + 3] = state->seq;
    memcpy(layer + LAYER_T, &state->t, sizeof(float));
    *(u8**)(layer + LAYER_FRAME0) = memory->frames[0];
    *(u8**)(layer + LAYER_FRAME1) = memory->frames[1];

    // With no speed or rate it stays on the frames we set and only works out its joints
    update_animation_layer(hero, layer);

    return true;
}

static bool our_layer(void* layer) {
    return (u8*)layer >= (u8*)layer_memory_ && (u8*)layer < (u8*)layer_memory_ + sizeof(layer_memory_);
}

static void apply_layers(Moby* hero, RemotePlayer* remote) {
    LayerMemory* memory = layer_memory_[remote - remote_players_];
    u8* first = nullptr;
    u8* last = nullptr;

    for (u8 i = 0; i < remote->n_layers; i++) {
        if (!set_up_layer(&memory[i], hero, &remote->layer_state[i])) {
            continue;
        }

        if (last != nullptr) {
            *(u8**)(last + LAYER_NEXT) = memory[i].layer;
        } else {
            first = memory[i].layer;
        }
        last = memory[i].layer;
    }

    hero->field36_0x60 = first;
    remote->hero_layered = first != nullptr;
}

// Our layers are only for a hero we're mirroring: the game would play them on whatever the moby turns into
static void drop_layers(Moby* hero, RemotePlayer* remote) {
    if (hero != nullptr && our_layer(hero->field36_0x60)) {
        hero->field36_0x60 = nullptr;
    }

    if (remote != nullptr) {
        remote->hero_layered = false;
    }
}

static void capture(MPMirrorMoby* out, Moby* moby, u8 slot) {
    u8* m = (u8*)moby;

    memset(out, 0, sizeof(MPMirrorMoby));

    out->slot = slot;
    out->enabled = moby->enabled;
    out->o_class = moby->o_class;
    out->mode_bits = moby->mode_bits;
    out->alpha = moby->alpha;

    out->position[0] = moby->position.x;
    out->position[1] = moby->position.y;
    out->position[2] = moby->position.z;
    out->rotation[0] = moby->rotation.x;
    out->rotation[1] = moby->rotation.y;
    out->rotation[2] = moby->rotation.z;
    memcpy(out->matrix, moby->transform, sizeof(out->matrix));
    out->scale = moby->scale;

    // Lighting the game worked out for where the moby is
    memcpy(out->lights, m + 0x38, sizeof(out->lights));
    memcpy(&out->lights2, m + 0x90, sizeof(out->lights2));

    // Animation: blending from frame0 of prev_seq (or a cached pose when it's 0xff) to frame1 of seq, t of the way
    out->anim_frame0 = m[0x50];
    out->anim_frame1 = m[0x51];
    out->anim_prev_seq = m[0x52];
    out->anim_seq = m[0x53];
    memcpy(&out->anim_t, m + 0x54, sizeof(float));
    memcpy(&out->anim_speed, m + 0x58, sizeof(float));
    memcpy(&out->anim_rate, m + 0x5c, sizeof(float));
    out->anim_flags = m[0x70];

    out->pose = joint_pose_kind(moby);
}

void Mirror::on_hero_animation_started(u32 blend_frames) {
    hero_animation_restarts_ += 1;
    hero_blend_frames_ = blend_frames > 0xff ? 0xff : (u8)blend_frames;
}

static void send_part(Client* client, MPPacketPlayerState* state, MPMirrorMoby* mobys, u8 n_mobys,
                      MPMirrorManipulator* manipulators, u8 n_manipulators) {
    size_t len = sizeof(MPPacketPlayerState) + n_mobys * sizeof(MPMirrorMoby) +
                 n_manipulators * sizeof(MPMirrorManipulator);

    // Small enough for the packet's own buffer, so nothing is allocated for the 60 snapshots a second
    Packet packet = Packet(len);
    packet.header->type = MP_PACKET_PLAYER_STATE;
    packet.retain_after_send = true;

    MPPacketPlayerState* body = (MPPacketPlayerState*)packet.body;
    memcpy(body, state, sizeof(MPPacketPlayerState));
    body->n_mobys = n_mobys;
    body->n_manipulators = n_manipulators;

    char* data = (char*)body + sizeof(MPPacketPlayerState);
    memcpy(data, mobys, n_mobys * sizeof(MPMirrorMoby));
    if (n_manipulators > 0) {
        memcpy(data + n_mobys * sizeof(MPMirrorMoby), manipulators, n_manipulators * sizeof(MPMirrorManipulator));
    }

    client->send(&packet);
}

// The part of a frame with the hero's extra manipulators and its animation layers, which has no mobys
static void send_extras(Client* client, MPPacketPlayerState* state, MPMirrorManipulator* manipulators,
                        MPMirrorLayer* layers) {
    size_t manipulators_len = state->n_extra_manipulators * sizeof(MPMirrorManipulator);
    size_t len = sizeof(MPPacketPlayerState) + manipulators_len + state->n_layers * sizeof(MPMirrorLayer);

    Packet packet = Packet(len);
    packet.header->type = MP_PACKET_PLAYER_STATE;
    packet.retain_after_send = true;

    MPPacketPlayerState* body = (MPPacketPlayerState*)packet.body;
    memcpy(body, state, sizeof(MPPacketPlayerState));
    body->n_mobys = 0;
    body->n_manipulators = 0;

    char* data = (char*)body + sizeof(MPPacketPlayerState);
    memcpy(data, manipulators, manipulators_len);
    memcpy(data + manipulators_len, layers, state->n_layers * sizeof(MPMirrorLayer));

    client->send(&packet);
}

static void capture_manipulator(MPMirrorManipulator* out, Manipulator* manipulator) {
    out->bone = manipulator->animation_bone;
    out->state = manipulator->state;
    out->scale_on = manipulator->scaleOn;
    out->absolute = manipulator->absolute;
    out->bone_id = manipulator->bone_id;
    out->blend_t = manipulator->animation_blend_t;
    memcpy(out->rotation, &manipulator->rotation, sizeof(out->rotation));
    memcpy(out->scale, &manipulator->scale, sizeof(out->scale));
    memcpy(out->translation, &manipulator->translation, sizeof(out->translation));
}

// False if other games couldn't play it: its frames are of a model they can't look up
static bool capture_layer(MPMirrorLayer* out, u8* layer) {
    memset(out, 0, sizeof(MPMirrorLayer));

    MobyClass* source = *(MobyClass**)(layer + LAYER_SOURCE_CLASS);
    out->joint_set = *(s16*)(layer + LAYER_JOINT_SET);
    out->source_o_class = source != nullptr ? GadgetModels::o_class_of(source) : 0xffff;
    out->frame0 = layer[LAYER_ANIMATION];
    out->frame1 = layer[LAYER_ANIMATION + 1];
    out->prev_seq = layer[LAYER_ANIMATION + 2];
    out->seq = layer[LAYER_ANIMATION + 3];
    out->from_source = (layer[LAYER_FRAME0_FROM_SOURCE] ? MP_MIRROR_LAYER_FRAME0_FROM_SOURCE : 0) |
                       (layer[LAYER_FRAME1_FROM_SOURCE] ? MP_MIRROR_LAYER_FRAME1_FROM_SOURCE : 0);
    memcpy(&out->t, layer + LAYER_T, sizeof(float));
    memcpy(&out->weight, layer + LAYER_WEIGHT, sizeof(float));

    return out->from_source == 0 || out->source_o_class != 0xffff;
}

void Mirror::send_snapshot(Client* client) {
    if (client == nullptr || ratchet_moby == nullptr || game_state != PlayerControl || !valid_moby(ratchet_moby)) {
        return;
    }

    frame_ += 1;

    MPPacketPlayerState state;
    memset(&state, 0, sizeof(state));
    state.frame = frame_;

    MPMirrorMoby hero;
    capture(&hero, ratchet_moby, MP_MIRROR_SLOT_HERO);
    hero.anim_restarts = hero_animation_restarts_;
    hero.blend_frames = hero_blend_frames_;

    // Bone tweaks on top of the animation, like Ratchet's head being scaled down a little. Aiming a weapon adds a
    //  chain of them down the arm, more than fit with the hero, so the rest go in a part of their own.
    MPMirrorManipulator manipulators[MP_MIRROR_MAX_MANIPULATORS];
    MPMirrorManipulator extra_manipulators[MP_MIRROR_MAX_EXTRA_MANIPULATORS];
    u8 n_manipulators = 0;
    for (Manipulator* manipulator = ratchet_moby->manipulator1;
         manipulator != nullptr && n_manipulators < MP_MIRROR_MAX_MANIPULATORS + MP_MIRROR_MAX_EXTRA_MANIPULATORS;
         manipulator = manipulator->pNext) {
        if ((u32)manipulator < 0x10000 || (u32)manipulator >= 0xC0000000) {
            break;
        }

        capture_manipulator(n_manipulators < MP_MIRROR_MAX_MANIPULATORS
                            ? &manipulators[n_manipulators]
                            : &extra_manipulators[n_manipulators - MP_MIRROR_MAX_MANIPULATORS], manipulator);
        n_manipulators += 1;
    }

    if (n_manipulators > MP_MIRROR_MAX_MANIPULATORS) {
        state.n_extra_manipulators = n_manipulators - MP_MIRROR_MAX_MANIPULATORS;
        n_manipulators = MP_MIRROR_MAX_MANIPULATORS;
    }

    // Animations on part of the body over the hero's own, like the arm raised holding a weapon
    MPMirrorLayer layers[MP_MIRROR_MAX_LAYERS];
    u8* layer = (u8*)ratchet_moby->field36_0x60;
    for (int i = 0; layer != nullptr && i < MIRROR_MAX_HERO_LAYERS && state.n_layers < MP_MIRROR_MAX_LAYERS; i++) {
        if ((u32)layer < 0x10000 || (u32)layer >= 0xC0000000) {
            break;
        }

        if (capture_layer(&layers[state.n_layers], layer)) {
            state.n_layers += 1;
        }

        layer = *(u8**)(layer + LAYER_NEXT);
    }

    MPMirrorMoby attachments[MP_MIRROR_ATTACHMENT_SLOTS];
    u8 n_attachments = 0;
    for (int slot = 0; slot < MP_MIRROR_ATTACHMENT_SLOTS; slot++) {
        Moby* moby = *(Moby**)(GADGET_SLOTS + (slot / 2) * GADGET_SLOT_STRIDE + (slot % 2) * 4);

        if (moby == nullptr || moby == ratchet_moby || !valid_moby(moby)) {
            continue;
        }

        capture(&attachments[n_attachments++], moby, slot);
        state.attachments |= 1 << slot;
    }

    send_part(client, &state, &hero, 1, manipulators, n_manipulators);

    if (state.n_extra_manipulators > 0 || state.n_layers > 0) {
        send_extras(client, &state, extra_manipulators, layers);
    }

    // Up to 4 attachments per packet keeps each packet in its own buffer
    for (u8 sent = 0; sent < n_attachments; sent += 4) {
        u8 count = n_attachments - sent > 4 ? 4 : n_attachments - sent;
        send_part(client, &state, &attachments[sent], count, nullptr, 0);
    }
}

static RemotePlayer* find_remote(u16 uuid) {
    for (int i = 0; i < MIRROR_MAX_PLAYERS; i++) {
        if (remote_players_[i].active && remote_players_[i].uuid == uuid) {
            return &remote_players_[i];
        }
    }

    return nullptr;
}

static void remove_attachment(RemotePlayer* remote, int slot, bool mobys_exist) {
    Moby* moby = remote->attachment_mobys[slot];

    if (moby != nullptr && mobys_exist && valid_moby(moby) && moby->p_update == MP_MOBY_UPDATE_FUNC) {
        delete_moby(moby);
    }

    remote->attachment_mobys[slot] = nullptr;
    remote->attachment_models[slot] = nullptr;
    remote->attachment_animation[slot].valid = false;
}

// The model to draw a copy of another player's attachment with: our own load of it for weapons and gadgets, which the
//  game only has loaded while our player holds them, otherwise whatever the game has loaded for its class. Null if we
//  can't draw it.
static MobyClass* attachment_model(RemotePlayer* remote, u16 o_class) {
    if (GadgetModels::is_gadget(o_class)) {
        return GadgetModels::load(o_class);
    }

    return GadgetModels::loaded_class(o_class);
}

void Mirror::drop_gadget_models() {
    for (int i = 0; i < MIRROR_MAX_PLAYERS; i++) {
        RemotePlayer* remote = &remote_players_[i];
        for (int slot = 0; slot < MP_MIRROR_ATTACHMENT_SLOTS; slot++) {
            if (remote->attachment_models[slot] != nullptr && GadgetModels::owns(remote->attachment_models[slot])) {
                remove_attachment(remote, slot, true);
            }
        }
    }
}

static void deactivate(RemotePlayer* remote, bool mobys_exist) {
    for (int slot = 0; slot < MP_MIRROR_ATTACHMENT_SLOTS; slot++) {
        remove_attachment(remote, slot, mobys_exist);
    }

    if (mobys_exist && (remote->hero_manipulated || remote->hero_layered)) {
        GameClient* client = (GameClient*)Game::shared().client();
        Moby* hero = client != nullptr ? client->moby_for_uuid(remote->uuid) : nullptr;
        if (hero != nullptr && remote->hero_manipulated) {
            hero->manipulator1 = nullptr;
        }
        drop_layers(hero, remote);
    }

    memset(remote, 0, sizeof(RemotePlayer));
}

void Mirror::receive(MPPacketPlayerState* packet, size_t len) {
    if (packet->sender == 0 || game_state != PlayerControl) {
        return;
    }

    size_t expected = sizeof(MPPacketPlayerState) + packet->n_mobys * sizeof(MPMirrorMoby) +
                      packet->n_manipulators * sizeof(MPMirrorManipulator);
    if (len < expected || packet->n_manipulators > MP_MIRROR_MAX_MANIPULATORS) {
        Logger::error("Mirror: malformed player state (%d bytes, expected %d)", len, expected);
        return;
    }

    RemotePlayer* remote = find_remote(packet->sender);

    // A frame far behind what we have means the sender started counting over, e.g. they reconnected with the same ID
    if (remote != nullptr && packet->frame + 120 < remote->newest_frame) {
        deactivate(remote, true);
        remote = nullptr;
    }

    if (remote == nullptr) {
        for (int i = 0; i < MIRROR_MAX_PLAYERS; i++) {
            if (!remote_players_[i].active) {
                remote = &remote_players_[i];
                memset(remote, 0, sizeof(RemotePlayer));
                remote->active = true;
                remote->uuid = packet->sender;
                remote->newest_frame = packet->frame;
                remote->playback_frame = packet->frame > MIRROR_DELAY ? packet->frame - MIRROR_DELAY : 0;
                break;
            }
        }

        if (remote == nullptr) {
            return;
        }
    }

    remote->last_packet_tick = ticks_;

    // Too late to be shown
    if (packet->frame < remote->playback_frame) {
        return;
    }

    // The snapshot for this frame, or the oldest one to reuse for it
    MirrorSnapshot* snapshot = nullptr;
    for (int i = 0; i < MIRROR_BUFFER_FRAMES; i++) {
        MirrorSnapshot* candidate = &remote->buffer[i];
        if (candidate->used && candidate->frame == packet->frame) {
            snapshot = candidate;
            break;
        }

        if (snapshot == nullptr || (snapshot->used && (!candidate->used || candidate->frame < snapshot->frame))) {
            snapshot = candidate;
        }
    }

    if (!snapshot->used || snapshot->frame != packet->frame) {
        snapshot->used = true;
        snapshot->frame = packet->frame;
        snapshot->has_hero = false;
        snapshot->received = 0;
        snapshot->n_manipulators = 0;
        snapshot->has_extras = false;
    }

    snapshot->attachments = packet->attachments;
    snapshot->n_extra_manipulators = packet->n_extra_manipulators;
    snapshot->n_layers = packet->n_layers;

    // The part with the hero's extra manipulators and animation layers
    if (packet->n_mobys == 0 && (packet->n_extra_manipulators > 0 || packet->n_layers > 0)) {
        size_t manipulators_len = packet->n_extra_manipulators * sizeof(MPMirrorManipulator);
        if (packet->n_extra_manipulators > MP_MIRROR_MAX_EXTRA_MANIPULATORS || packet->n_layers > MP_MIRROR_MAX_LAYERS ||
            len < sizeof(MPPacketPlayerState) + manipulators_len + packet->n_layers * sizeof(MPMirrorLayer)) {
            Logger::error("Mirror: malformed player state extras (%d bytes)", len);
            snapshot->n_extra_manipulators = 0;
            snapshot->n_layers = 0;
            return;
        }

        char* data = (char*)packet + sizeof(MPPacketPlayerState);
        memcpy(snapshot->extra_manipulators, data, manipulators_len);
        memcpy(snapshot->layers, data + manipulators_len, packet->n_layers * sizeof(MPMirrorLayer));
        snapshot->has_extras = true;
    }

    MPMirrorMoby* mobys = (MPMirrorMoby*)((char*)packet + sizeof(MPPacketPlayerState));
    for (u8 i = 0; i < packet->n_mobys; i++) {
        MPMirrorMoby* moby = &mobys[i];

        if (moby->slot == MP_MIRROR_SLOT_HERO) {
            memcpy(&snapshot->hero, moby, sizeof(MPMirrorMoby));
            snapshot->has_hero = true;

            // Manipulators come in the same part as the hero
            snapshot->n_manipulators = packet->n_manipulators;
            memcpy(snapshot->manipulators, (char*)mobys + packet->n_mobys * sizeof(MPMirrorMoby),
                   packet->n_manipulators * sizeof(MPMirrorManipulator));
        } else if (moby->slot < MP_MIRROR_ATTACHMENT_SLOTS) {
            memcpy(&snapshot->attachment[moby->slot], moby, sizeof(MPMirrorMoby));
            snapshot->received |= 1 << moby->slot;
        }
    }

    if (packet->frame > remote->newest_frame) {
        remote->newest_frame = packet->frame;
    }
}

void Mirror::on_tick() {
    ticks_ += 1;
    GadgetModels::on_tick();

    for (int i = 0; i < MIRROR_MAX_ORPHANS; i++) {
        Moby* moby = orphans_[i];
        if (moby != nullptr && valid_moby(moby) && moby->p_update == MP_MOBY_UPDATE_FUNC) {
            delete_moby(moby);
        }

        orphans_[i] = nullptr;
    }

    GameClient* client = (GameClient*)Game::shared().connected_client();

    for (int i = 0; i < MIRROR_MAX_PLAYERS; i++) {
        RemotePlayer* remote = &remote_players_[i];
        if (!remote->active) {
            continue;
        }

        Moby* hero = client != nullptr ? client->moby_for_uuid(remote->uuid) : nullptr;

        if (client == nullptr || ticks_ - remote->last_packet_tick > MIRROR_TIMEOUT) {
            deactivate(remote, true);
            continue;
        }

        if (hero == nullptr) {
            remote->arrived_hero = nullptr;
        }

        if (game_state != PlayerControl || hero == nullptr) {
            continue;
        }

        // They showed up on our level (or it loaded again here)
        if (hero != remote->arrived_hero) {
            remote->arrived_hero = hero;
            WorldSync::on_player_arrived();
        }

        // Play back one sender frame per frame, a few frames behind the newest, catching up if we fall behind
        remote->playback_frame += 1;
        if (remote->newest_frame > remote->playback_frame + MIRROR_DELAY + 4) {
            remote->playback_frame = remote->newest_frame - MIRROR_DELAY;
        }
        if (remote->playback_frame > remote->newest_frame) {
            remote->playback_frame = remote->newest_frame;
        }

        MirrorSnapshot* current = nullptr;
        for (int j = 0; j < MIRROR_BUFFER_FRAMES; j++) {
            MirrorSnapshot* snapshot = &remote->buffer[j];
            if (snapshot->used && snapshot->has_hero && snapshot->frame <= remote->playback_frame &&
                (current == nullptr || snapshot->frame > current->frame)) {
                current = snapshot;
            }
        }

        if (current == nullptr) {
            continue;
        }

        remote->showing = true;
        memcpy(&remote->hero_state, &current->hero, sizeof(MPMirrorMoby));
        remote->n_manipulators = current->n_manipulators;
        memcpy(remote->manipulator_state, current->manipulators, sizeof(remote->manipulator_state));

        // When the part with what else is on top of the hero's animation got lost, keep showing what we had
        bool no_extras = current->n_extra_manipulators == 0 && current->n_layers == 0;
        if (current->has_extras || no_extras) {
            remote->n_extra_manipulators = no_extras ? 0 : current->n_extra_manipulators;
            remote->n_layers = no_extras ? 0 : current->n_layers;
            memcpy(remote->extra_manipulator_state, current->extra_manipulators,
                   sizeof(remote->extra_manipulator_state));
            memcpy(remote->layer_state, current->layers, sizeof(remote->layer_state));
        }

        for (int slot = 0; slot < MP_MIRROR_ATTACHMENT_SLOTS; slot++) {
            u16 bit = 1 << slot;

            if (!(current->attachments & bit)) {
                remove_attachment(remote, slot, true);
                continue;
            }

            // The part of the snapshot with this attachment got lost, keep showing what we had
            if (!(current->received & bit)) {
                continue;
            }

            MPMirrorMoby* state = &current->attachment[slot];
            memcpy(&remote->attachment_state[slot], state, sizeof(MPMirrorMoby));

            // Drawing a copy with a model that isn't loaded, or got swapped for another one, crashes the game
            MobyClass* model = attachment_model(remote, state->o_class);

            Moby* moby = remote->attachment_mobys[slot];
            if (moby != nullptr && (!valid_moby(moby) || moby->o_class != state->o_class || moby->p_class != model)) {
                remove_attachment(remote, slot, true);
                moby = nullptr;
            }

            if (moby == nullptr) {
                if (model == nullptr) {
                    continue;
                }

                moby = GadgetModels::spawn_with_class(state->o_class, model,
                                                      MP_MOBY_FLAG_MIRROR | MP_MOBY_FLAG_NO_COLLISION);
                if (moby == nullptr) {
                    continue;
                }

                moby->collision = nullptr;
                remote->attachment_mobys[slot] = moby;
                remote->attachment_models[slot] = model;
                remote->attachment_animation[slot].valid = false;
            }
        }
    }
}

static void apply_animation(Moby* moby, MPMirrorMoby* state, MirrorAnimation* applied, bool track_restarts) {
    if (moby->o_class != state->o_class) {
        return;
    }

    MPMobyAnimation animation;
    animation.seq = state->anim_seq;
    animation.prev_seq = state->anim_prev_seq;
    animation.frame0 = state->anim_frame0;
    animation.frame1 = state->anim_frame1;
    animation.t = state->anim_t;
    animation.speed = state->anim_speed;
    animation.rate = state->anim_rate;
    animation.flags = state->anim_flags;
    animation.restarts = state->anim_restarts;
    animation.blend_frames = state->blend_frames;

    MobyAnimation::apply(moby, &animation, applied, track_restarts);
}

static void apply_transform(Moby* moby, MPMirrorMoby* state) {
    u8* m = (u8*)moby;

    moby->position.x = state->position[0];
    moby->position.y = state->position[1];
    moby->position.z = state->position[2];
    moby->rotation.x = state->rotation[0];
    moby->rotation.y = state->rotation[1];
    moby->rotation.z = state->rotation[2];
    memcpy(moby->transform, state->matrix, sizeof(state->matrix));
    moby->scale = state->scale;

    memcpy(m + 0x38, state->lights, sizeof(state->lights));
    memcpy(m + 0x90, &state->lights2, sizeof(state->lights2));

    moby->alpha = state->alpha;
    moby->enabled = state->enabled;

    // 0x100 makes the game build the moby's transform from the matrix we set instead of from its rotation
    moby->mode_bits |= 0x100;
}

bool Mirror::apply_hero(Moby* moby, u16 uuid) {
    RemotePlayer* remote = find_remote(uuid);
    if (remote == nullptr || !remote->showing || game_state != PlayerControl) {
        drop_layers(moby, remote);
        return false;
    }

    // The player turned into something else (Clank, Giant Clank...): the regular moby update swaps the moby first
    if (remote->hero_state.o_class != moby->o_class) {
        drop_layers(moby, remote);
        return false;
    }

    apply_transform(moby, &remote->hero_state);
    apply_animation(moby, &remote->hero_state, &remote->hero_animation, true);

    Manipulator* manipulators = manipulators_[remote - remote_players_];
    u8 n_manipulators = remote->n_manipulators + remote->n_extra_manipulators;
    for (u8 i = 0; i < n_manipulators; i++) {
        MPMirrorManipulator* state = i < remote->n_manipulators
                                     ? &remote->manipulator_state[i]
                                     : &remote->extra_manipulator_state[i - remote->n_manipulators];
        Manipulator* manipulator = &manipulators[i];

        manipulator->animation_bone = state->bone;
        manipulator->state = state->state;
        manipulator->scaleOn = state->scale_on;
        manipulator->absolute = state->absolute;
        manipulator->bone_id = state->bone_id;
        manipulator->animation_blend_t = state->blend_t;
        memcpy(&manipulator->rotation, state->rotation, sizeof(state->rotation));
        memcpy(&manipulator->scale, state->scale, sizeof(state->scale));
        manipulator->scale.w = 0;
        memcpy(&manipulator->translation, state->translation, sizeof(state->translation));
        manipulator->translation.w = 0;
        manipulator->pNext = i + 1 < n_manipulators ? &manipulators[i + 1] : nullptr;
    }

    moby->manipulator1 = n_manipulators > 0 ? manipulators : nullptr;
    remote->hero_manipulated = true;

    apply_layers(moby, remote);

    return true;
}

void Mirror::update_attachment(Moby* moby) {
    for (int i = 0; i < MIRROR_MAX_PLAYERS; i++) {
        RemotePlayer* remote = &remote_players_[i];
        if (!remote->active || !remote->showing) {
            continue;
        }

        for (int slot = 0; slot < MP_MIRROR_ATTACHMENT_SLOTS; slot++) {
            if (remote->attachment_mobys[slot] != moby) {
                continue;
            }

            MPMirrorMoby* state = &remote->attachment_state[slot];
            apply_transform(moby, state);
            GadgetModels::touch(moby->p_class);

            // Keep it in the update loop (0x2 takes it out) and let the game build its transform from our matrix
            moby->mode_bits = (state->mode_bits & ~(0x2 | 0x4)) | 0x100;

            u8* pose = state->pose != MP_MIRROR_POSE_ANIMATION && state->pose <= JOINT_POSE_KINDS
                       ? joint_pose_buffers_[i][state->pose - 1] : nullptr;
            if (pose == nullptr || !pose_from_joints(moby, state->pose, hero_of(remote->uuid), pose)) {
                apply_animation(moby, state, &remote->attachment_animation[slot], false);
            }
            return;
        }
    }

    // Its player is gone
    moby->enabled = 0;

    for (int i = 0; i < MIRROR_MAX_ORPHANS; i++) {
        if (orphans_[i] == nullptr || orphans_[i] == moby) {
            orphans_[i] = moby;
            break;
        }
    }
}

float Mirror::nearest_player_distance_sq(const Vec4* position) {
    float nearest = 1e30f;

    for (int i = 0; i < MIRROR_MAX_PLAYERS; i++) {
        RemotePlayer* remote = &remote_players_[i];
        if (!remote->active || !remote->showing) {
            continue;
        }

        float dx = remote->hero_state.position[0] - position->x;
        float dy = remote->hero_state.position[1] - position->y;
        float dz = remote->hero_state.position[2] - position->z;
        float distance = dx * dx + dy * dy + dz * dz;
        if (distance < nearest) {
            nearest = distance;
        }
    }

    return nearest;
}

u32 Mirror::sent_frame() {
    return frame_;
}

bool Mirror::shown_frame(u16 uuid, u32* frame) {
    RemotePlayer* remote = find_remote(uuid);
    if (remote == nullptr || !remote->showing) {
        return false;
    }

    *frame = remote->playback_frame;
    return true;
}

Moby* Mirror::hero_of(u16 uuid) {
    GameClient* client = (GameClient*)Game::shared().connected_client();
    return client != nullptr ? client->moby_for_uuid(uuid) : nullptr;
}

Moby* Mirror::attachment_of(u16 uuid, int slot) {
    RemotePlayer* remote = find_remote(uuid);
    if (remote == nullptr || slot < 0 || slot >= MP_MIRROR_ATTACHMENT_SLOTS) {
        return nullptr;
    }

    Moby* moby = remote->attachment_mobys[slot];
    if (moby == nullptr || !valid_moby(moby) || moby->p_update != MP_MOBY_UPDATE_FUNC) {
        return nullptr;
    }

    return moby;
}

bool Mirror::is_mirrored(u16 uuid) {
    RemotePlayer* remote = find_remote(uuid);
    return remote != nullptr && remote->showing;
}

void Mirror::forget(u16 uuid) {
    RemotePlayer* remote = find_remote(uuid);
    if (remote != nullptr) {
        deactivate(remote, true);
    }
}

void Mirror::reset() {
    for (int i = 0; i < MIRROR_MAX_PLAYERS; i++) {
        if (remote_players_[i].active) {
            deactivate(&remote_players_[i], false);
        }
    }

    memset(orphans_, 0, sizeof(orphans_));
}
