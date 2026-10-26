//
// See EffectSync.h
//

#include "EffectSync.h"

#include <rc1/Game.h>

#include "EnemySync.h"
#include "LevelMobys.h"
#include "Mirror.h"
#include "SoundSync.h"
#include "SpawnedMobys.h"
#include "network/GameClient.h"

// The game's effect pool: 4096 records of 64 bytes, and a bit per record that's in use. The first byte of a record is
//  its type.
#define EFFECT_POOL (*(u8**)0xa51fc0)
#define EFFECT_IN_USE ((u8*)0xa52000)
#define EFFECT_COUNT 0x1000

// Gadget slots: the game keeps up to 2 mobys per slot on the player (hand, back, head, feet...), 0x50 bytes apart
#define GADGET_SLOTS ((u8*)0x96ad70)
#define GADGET_SLOT_STRIDE 0x50
#define GADGET_MOBYS 14

// Mobys our player's weapons spawned (bombs, missiles...) we still watch
#define MAX_SPAWNED 64

#define MAX_CAPTURED 128
#define MAX_QUEUED 512

// Effects one moby makes in an update that we may have to undo
#define MAX_MOBY_EFFECTS 32

#define MAX_MUTED 32

// Bolts dropped by enemies, which are always each game's own
#define BOLT_O_CLASS 13
#define BIG_BOLT_O_CLASS 14

// Effects of a player we aren't showing, or whose frame is this far off what we show, are made right away
#define EFFECT_FAR_FRAMES 30

// And none waits longer than this many ticks
#define EFFECT_MAX_WAIT 60

// A moby's address in an effect record, as other games know that moby: the sender's player or what they have in an
//  attachment slot (see Mirror), or a moby placed in the level by its UID. The second byte is how far into the moby
//  the address points, in words.
#define REFERENCE_HERO 0xf1
#define REFERENCE_ATTACHMENT 0xf2
#define REFERENCE_LEVEL_MOBY 0xf3

extern "C" u8* _c_allocate_effect(u32 type);

SHK_FUNCTION_DEFINE_STATIC_1(0x11e6ec, void, free_effect, u8*, effect);

struct QueuedEffect {
    bool used;
    u16 sender;
    u32 frame;
    u32 tick;
    u8 data[MP_EFFECT_SIZE];
};

// Our player and what they have on them this frame
static Moby* ours_[1 + GADGET_MOBYS];

static Moby* spawned_[MAX_SPAWNED];
static u16 spawned_classes_[MAX_SPAWNED];

static u8* captured_[MAX_CAPTURED];
static int n_captured_ = 0;

// Our player, and with them what they hold, are being updated
static bool player_updating_ = false;

// What the moby being updated made so far
static u8* moby_effects_[MAX_MOBY_EFFECTS];
static int n_moby_effects_ = 0;

static Moby* muted_[MAX_MUTED];
static u16 muted_classes_[MAX_MUTED];

static QueuedEffect queue_[MAX_QUEUED];
static u32 ticks_ = 0;

// Ours to send: our player, what they hold or fired, the enemies we run for everyone and what they fired
static bool is_ours(Moby* moby) {
    if (moby == nullptr) {
        return false;
    }

    if (EnemySync::is_owned(moby)) {
        return true;
    }

    for (int i = 0; i < 1 + GADGET_MOBYS; i++) {
        if (ours_[i] == moby) {
            return true;
        }
    }

    for (int i = 0; i < MAX_SPAWNED; i++) {
        if (spawned_[i] != moby) {
            continue;
        }

        // Its slot may have gone to some other moby since
        if (moby->state >= 0 && moby->o_class == (short)spawned_classes_[i]) {
            return true;
        }

        spawned_[i] = nullptr;
    }

    return false;
}

// Still in use by the time we send it: some are only made for the frame
static bool in_use(u8* effect) {
    u8* pool = EFFECT_POOL;
    if (pool == nullptr || effect < pool || effect >= pool + EFFECT_COUNT * MP_EFFECT_SIZE) {
        return false;
    }

    u32 index = (effect - pool) / MP_EFFECT_SIZE;
    return (EFFECT_IN_USE[index >> 3] & (1 << (index & 7))) != 0;
}

void EffectSync::begin_frame() {
    ours_[0] = ratchet_moby;
    for (int i = 0; i < GADGET_MOBYS; i++) {
        ours_[1 + i] = *(Moby**)(GADGET_SLOTS + (i / 2) * GADGET_SLOT_STRIDE + (i % 2) * 4);
    }
}

void EffectSync::on_allocated(u8* effect) {
    if (effect == nullptr || game_state != PlayerControl || (sync_off_ & SYNC_OFF_EFFECT_CAPTURE)) {
        return;
    }

    if (!player_updating_ && EnemySync::updating() != nullptr && n_moby_effects_ < MAX_MOBY_EFFECTS) {
        moby_effects_[n_moby_effects_++] = effect;
    }

    if (n_captured_ < MAX_CAPTURED && (player_updating_ || is_ours(EnemySync::updating()))) {
        captured_[n_captured_++] = effect;
    }
}

void EffectSync::before_moby_update() {
    n_moby_effects_ = 0;
}

void EffectSync::after_moby_update(Moby* moby) {
    for (int i = 0; i < MAX_MUTED; i++) {
        if (muted_[i] != moby) {
            continue;
        }

        if (moby->o_class == (short)muted_classes_[i]) {
            discard_moby_effects();
        } else {
            muted_[i] = nullptr;
        }
        return;
    }
}

void EffectSync::mute(Moby* moby) {
    if (moby == nullptr || moby->o_class == BOLT_O_CLASS || moby->o_class == BIG_BOLT_O_CLASS) {
        return;
    }

    int slot = 0;
    for (int i = 0; i < MAX_MUTED; i++) {
        if (muted_[i] == nullptr || muted_[i] == moby || muted_[i]->state < 0 ||
            muted_[i]->o_class != (short)muted_classes_[i]) {
            slot = i;
            break;
        }
    }

    muted_[slot] = moby;
    muted_classes_[slot] = moby->o_class;
}

void EffectSync::discard_moby_effects() {
    for (int i = 0; i < n_moby_effects_; i++) {
        if (in_use(moby_effects_[i])) {
            free_effect(moby_effects_[i]);
        }
    }

    n_moby_effects_ = 0;
}

void EffectSync::on_spawned(Moby* moby) {
    if (moby == nullptr || game_state != PlayerControl || !(player_updating_ || is_ours(EnemySync::updating()))) {
        return;
    }

    int slot = -1;
    for (int i = 0; i < MAX_SPAWNED && slot < 0; i++) {
        if (spawned_[i] == nullptr || spawned_[i] == moby) {
            slot = i;
        }
    }

    // All taken: forget ones that are gone
    for (int i = 0; i < MAX_SPAWNED && slot < 0; i++) {
        Moby* other = spawned_[i];
        if (other->state < 0 || other->o_class != (short)spawned_classes_[i]) {
            slot = i;
        }
    }

    if (slot < 0) {
        return;
    }

    spawned_[slot] = moby;
    spawned_classes_[slot] = moby->o_class;
}

void EffectSync::before_player_update() {
    player_updating_ = true;
}

void EffectSync::after_player_update() {
    player_updating_ = false;
    end_frame();
}

// Puts in which moby the record's moby addresses are, as other games know it. False if it holds the address of one
//  they can't know.
static bool encode_references(u8* data) {
    Moby* start = LEVEL_MOBYS_START;
    for (int offset = 4; offset < MP_EFFECT_SIZE; offset += 4) {
        u32 value;
        memcpy(&value, data + offset, 4);
        if (start == nullptr || value < (u32)start || value >= (u32)moby_ptr_end) {
            continue;
        }

        Moby* moby = start + (value - (u32)start) / sizeof(Moby);
        u32 into = (value - (u32)moby) / 4;
        u32 reference = 0;

        if (moby == ratchet_moby) {
            reference = (REFERENCE_HERO << 24) | (into << 16);
        }

        for (int i = 0; i < GADGET_MOBYS && reference == 0; i++) {
            if (ours_[1 + i] == moby && moby != nullptr) {
                reference = (REFERENCE_ATTACHMENT << 24) | (into << 16) | i;
            }
        }

        u16 uid, spawn;
        if (reference == 0 && SpawnedMobys::identify(moby, &uid, &spawn) && spawn == 0) {
            reference = (REFERENCE_LEVEL_MOBY << 24) | (into << 16) | uid;
        }

        if (reference == 0) {
            return false;
        }

        memcpy(data + offset, &reference, 4);
    }

    return true;
}

// Puts our copies of the mobys the record is about in it. False if we don't have one.
static bool decode_references(u16 sender, u8* data) {
    for (int offset = 4; offset < MP_EFFECT_SIZE; offset += 4) {
        u32 value;
        memcpy(&value, data + offset, 4);

        u8 kind = value >> 24;
        if (kind != REFERENCE_HERO && kind != REFERENCE_ATTACHMENT && kind != REFERENCE_LEVEL_MOBY) {
            continue;
        }

        Moby* moby = nullptr;
        if (kind == REFERENCE_HERO) {
            moby = Mirror::hero_of(sender);
        } else if (kind == REFERENCE_ATTACHMENT) {
            moby = Mirror::attachment_of(sender, value & 0xffff);
        } else {
            moby = LevelMobys::find(value & 0xffff);
        }

        if (moby == nullptr) {
            return false;
        }

        u32 address = (u32)moby + ((value >> 16) & 0xff) * 4;
        memcpy(data + offset, &address, 4);
    }

    return true;
}

void EffectSync::end_frame() {
    Client* client = Game::shared().connected_client();
    if (client == nullptr || n_captured_ == 0 || ratchet_moby == nullptr ||
        Mirror::nearest_player_distance_sq(&ratchet_moby->position) >= 1e29f) {
        n_captured_ = 0;
        return;
    }

    int n_effects = 0;
    for (int i = 0; i < n_captured_; i++) {
        if (in_use(captured_[i])) {
            captured_[n_effects++] = captured_[i];
        }
    }

    // What goes out, with the mobys they're about as other games know them
    static u8 records[MAX_CAPTURED][MP_EFFECT_SIZE];
    int n_records = 0;
    for (int i = 0; i < n_effects; i++) {
        memcpy(records[n_records], captured_[i], MP_EFFECT_SIZE);
        if (encode_references(records[n_records])) {
            n_records += 1;
        }
    }
    n_effects = n_records;

    // As many as fit in a packet's own buffer, so nothing is allocated for them
    const size_t max_body = sizeof(((Packet*)0)->payload_buffer) - sizeof(MPPacketHeader);
    const int per_packet = (max_body - sizeof(MPPacketEffects)) / MP_EFFECT_SIZE;

    for (int sent = 0; sent < n_effects; sent += per_packet) {
        int count = n_effects - sent > per_packet ? per_packet : n_effects - sent;

        Packet packet = Packet(sizeof(MPPacketEffects) + count * MP_EFFECT_SIZE);
        packet.header->type = MP_PACKET_EFFECTS;
        packet.retain_after_send = true;

        MPPacketEffects* body = (MPPacketEffects*)packet.body;
        body->n_effects = count;
        body->frame = Mirror::sent_frame();

        u8* data = (u8*)body + sizeof(MPPacketEffects);
        for (int i = 0; i < count; i++) {
            memcpy(data + i * MP_EFFECT_SIZE, records[sent + i], MP_EFFECT_SIZE);
        }

        client->send(&packet);
    }

    n_captured_ = 0;
}

void EffectSync::receive(MPPacketEffects* packet, size_t len) {
    if (packet->sender == 0 || game_state != PlayerControl ||
        len < sizeof(MPPacketEffects) + packet->n_effects * MP_EFFECT_SIZE) {
        return;
    }

    u8* data = (u8*)packet + sizeof(MPPacketEffects);
    int next = 0;
    for (int i = 0; i < packet->n_effects; i++) {
        while (next < MAX_QUEUED && queue_[next].used) {
            next += 1;
        }

        if (next >= MAX_QUEUED) {
            return;
        }

        QueuedEffect* effect = &queue_[next];
        effect->used = true;
        effect->sender = packet->sender;
        effect->frame = packet->frame;
        effect->tick = ticks_;
        memcpy(effect->data, data + i * MP_EFFECT_SIZE, MP_EFFECT_SIZE);
    }
}

void EffectSync::on_tick() {
    ticks_ += 1;

    for (int i = 0; i < MAX_QUEUED; i++) {
        QueuedEffect* effect = &queue_[i];
        if (!effect->used) {
            continue;
        }

        if (game_state != PlayerControl || (sync_off_ & SYNC_OFF_EFFECT_REPLAY)) {
            effect->used = false;
            continue;
        }

        // In step with that player's pose we're showing
        u32 shown;
        bool due = !Mirror::shown_frame(effect->sender, &shown) || shown >= effect->frame ||
                   effect->frame > shown + EFFECT_FAR_FRAMES || ticks_ - effect->tick > EFFECT_MAX_WAIT;
        if (!due) {
            continue;
        }

        effect->used = false;

        // About a moby we have no copy of: not made
        if (!decode_references(effect->sender, effect->data)) {
            continue;
        }

        u8* made = _c_allocate_effect(effect->data[0]);
        if (made != nullptr) {
            memcpy(made + 1, effect->data + 1, MP_EFFECT_SIZE - 1);
        }
    }
}

void EffectSync::reset() {
    memset(spawned_, 0, sizeof(spawned_));
    memset(muted_, 0, sizeof(muted_));
    memset(queue_, 0, sizeof(queue_));
    n_captured_ = 0;
}

extern "C" void _c_on_effect_allocated(u8* effect) {
    EffectSync::on_allocated(effect);
}

extern "C" void _c_before_player_update() {
    EffectSync::before_player_update();
}

extern "C" void _c_after_player_update() {
    EffectSync::after_player_update();
    SoundSync::end_frame();
}
