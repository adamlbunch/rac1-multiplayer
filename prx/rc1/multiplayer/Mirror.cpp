//
// See Mirror.h
//

#include "Mirror.h"

#include <rc1/Game.h>

#include "MPMoby.h"
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
static Manipulator manipulators_[MIRROR_MAX_PLAYERS][MP_MIRROR_MAX_MANIPULATORS] __attribute__((aligned(16)));

// Copies whose player is gone, deleted at the next tick: deleting a moby during the update loop could break the
//  loop's walk through the moby list
#define MIRROR_MAX_ORPHANS 16
static Moby* orphans_[MIRROR_MAX_ORPHANS];

static u32 frame_ = 0;
static u32 ticks_ = 0;
static u8 hero_animation_restarts_ = 0;
static u8 hero_blend_frames_ = 0;

static bool valid_moby(Moby* moby) {
    u32 address = (u32)moby;

    return address >= 0x10000 && address < 0xC0000000 && (address & 0xf) == 0 &&
           moby->state >= 0 && moby->p_class != nullptr;
}

static MobySeq* animation_sequence(Moby* moby, u8 seq) {
    u8* p_class = (u8*)moby->p_class;

    // Sequence count at 0xc, sequence pointers from 0x48
    if (p_class == nullptr || seq >= p_class[0xc]) {
        return nullptr;
    }

    return ((MobySeq**)(p_class + 0x48))[seq];
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

    // Bone tweaks on top of the animation, like Ratchet's head being scaled down a little
    MPMirrorManipulator manipulators[MP_MIRROR_MAX_MANIPULATORS];
    u8 n_manipulators = 0;
    for (Manipulator* manipulator = ratchet_moby->manipulator1;
         manipulator != nullptr && n_manipulators < MP_MIRROR_MAX_MANIPULATORS;
         manipulator = manipulator->pNext) {
        if ((u32)manipulator < 0x10000 || (u32)manipulator >= 0xC0000000) {
            break;
        }

        MPMirrorManipulator* out = &manipulators[n_manipulators++];
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
    remote->attachment_animation[slot].valid = false;
}

static void deactivate(RemotePlayer* remote, bool mobys_exist) {
    for (int slot = 0; slot < MP_MIRROR_ATTACHMENT_SLOTS; slot++) {
        remove_attachment(remote, slot, mobys_exist);
    }

    if (mobys_exist && remote->hero_manipulated) {
        GameClient* client = (GameClient*)Game::shared().client();
        Moby* hero = client != nullptr ? client->moby_for_uuid(remote->uuid) : nullptr;
        if (hero != nullptr) {
            hero->manipulator1 = nullptr;
        }
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
    }

    snapshot->attachments = packet->attachments;

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

        if (game_state != PlayerControl || hero == nullptr) {
            continue;
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

            Moby* moby = remote->attachment_mobys[slot];
            if (moby != nullptr && (!valid_moby(moby) || moby->o_class != state->o_class)) {
                remove_attachment(remote, slot, true);
                moby = nullptr;
            }

            if (moby == nullptr) {
                moby = (Moby*)MPMoby::spawn(0, state->o_class, MP_MOBY_FLAG_MIRROR | MP_MOBY_FLAG_NO_COLLISION, 0);
                if (moby == nullptr) {
                    continue;
                }

                moby->collision = nullptr;
                remote->attachment_mobys[slot] = moby;
                remote->attachment_animation[slot].valid = false;
            }
        }
    }
}

static void apply_animation(Moby* moby, MPMirrorMoby* state, MirrorAnimation* applied, bool track_restarts) {
    u8* m = (u8*)moby;

    if (moby->o_class != state->o_class) {
        return;
    }

    MobySeq* seq = animation_sequence(moby, state->anim_seq);
    if (seq == nullptr || state->anim_frame1 >= seq->frame_count) {
        return;
    }

    bool started = !applied->valid || applied->seq != state->anim_seq ||
                   (track_restarts && applied->restarts != state->anim_restarts);

    // The sender is blending into this animation from the pose it was in, do the same from ours
    if (started && state->anim_prev_seq == 0xff) {
        set_moby_animation(moby, state->anim_seq, state->anim_frame1, state->blend_frames);
    }

    applied->valid = true;
    applied->seq = state->anim_seq;
    applied->restarts = state->anim_restarts;

    if (state->anim_prev_seq != 0xff) {
        MobySeq* prev_seq = animation_sequence(moby, state->anim_prev_seq);
        if (prev_seq == nullptr || state->anim_frame0 >= prev_seq->frame_count) {
            return;
        }

        m[0x52] = state->anim_prev_seq;
        m[0x50] = state->anim_frame0;
    } else if (m[0x52] != 0xff) {
        // No blend of our own to continue (it ended, or the game had no room for one), go straight to the target
        m[0x52] = state->anim_seq;
        m[0x50] = state->anim_frame1;
    }

    m[0x53] = state->anim_seq;
    m[0x51] = state->anim_frame1;
    memcpy(m + 0x54, &state->anim_t, sizeof(float));
    memcpy(m + 0x58, &state->anim_speed, sizeof(float));
    memcpy(m + 0x5c, &state->anim_rate, sizeof(float));
    m[0x70] = state->anim_flags;

    // Points the moby at the frame data for the frames we just set
    moby_frame_data_something(moby);
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
        return false;
    }

    // The player turned into something else (Clank, Giant Clank...): the regular moby update swaps the moby first
    if (remote->hero_state.o_class != moby->o_class) {
        return false;
    }

    apply_transform(moby, &remote->hero_state);
    apply_animation(moby, &remote->hero_state, &remote->hero_animation, true);

    Manipulator* manipulators = manipulators_[remote - remote_players_];
    for (u8 i = 0; i < remote->n_manipulators; i++) {
        MPMirrorManipulator* state = &remote->manipulator_state[i];
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
        manipulator->pNext = i + 1 < remote->n_manipulators ? &manipulators[i + 1] : nullptr;
    }

    moby->manipulator1 = remote->n_manipulators > 0 ? manipulators : nullptr;
    remote->hero_manipulated = true;

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

            // Keep it in the update loop (0x2 takes it out) and let the game build its transform from our matrix
            moby->mode_bits = (state->mode_bits & ~(0x2 | 0x4)) | 0x100;

            apply_animation(moby, state, &remote->attachment_animation[slot], false);
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
