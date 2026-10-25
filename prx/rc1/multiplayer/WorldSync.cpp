//
// See WorldSync.h
//

#include "WorldSync.h"

#include <rc1/Game.h>

#include "network/GameClient.h"

// The game records damage in a ring of 64 records. Whatever hurts a moby fills in the next record and points the
//  moby's `damage` at it, and the moby reads it in its update.
#define DAMAGE_RECORDS ((u8*)0xa14c00)
#define DAMAGE_RECORD_SIZE 0x40
#define DAMAGE_RECORD_COUNT 64
#define DAMAGE_NEXT_RECORD (*(u32*)0xa10954)

// MPMoby's update function: mobys running it are our copies of other players and their things
#define MP_MOBY_UPDATE_FUNC ((void*)0x710ed8)

// How long a moby someone else destroyed gets to go away on its own (play its death) before we remove it
#define KILL_GRACE_TICKS (60 * 3)

// How long we watch a moby our player hit for disappearing
#define HIT_MEMORY_TICKS (60 * 20)

#define MAX_HIT_MOBYS 32
#define MAX_PENDING_KILLS 32

struct HitMoby {
    Moby* moby;
    u16 uid;
    u16 o_class;
    u32 last_hit_tick;
};

struct PendingKill {
    u16 uid;
    u16 o_class;
    u32 deadline;
};

static HitMoby hit_mobys_[MAX_HIT_MOBYS];
static PendingKill pending_kills_[MAX_PENDING_KILLS];

static u32 ticks_ = 0;
static u32 next_record_ = 0;

// Records we filled in ourselves for other players' hits, which we must not send back
static u64 injected_ = 0;

static bool plausible_moby(Moby* moby) {
    u32 address = (u32)moby;
    return address >= 0x10000 && address < 0xC0000000 && (address & 0xf) == 0;
}

// Placed in the level (as opposed to spawned while playing), so every game has it under the same UID
static bool is_level_moby(Moby* moby) {
    return moby->uid != 0 && moby->uid != 0xffff && moby != ratchet_moby && moby->p_update != MP_MOBY_UPDATE_FUNC;
}

static void remember_hit(Moby* moby) {
    HitMoby* slot = nullptr;

    for (int i = 0; i < MAX_HIT_MOBYS && slot == nullptr; i++) {
        if (hit_mobys_[i].moby == moby && hit_mobys_[i].uid == moby->uid) {
            slot = &hit_mobys_[i];
        }
    }

    for (int i = 0; i < MAX_HIT_MOBYS && slot == nullptr; i++) {
        if (hit_mobys_[i].moby == nullptr) {
            slot = &hit_mobys_[i];
        }
    }

    // All taken: forget the one hit longest ago
    if (slot == nullptr) {
        slot = &hit_mobys_[0];
        for (int i = 1; i < MAX_HIT_MOBYS; i++) {
            if (hit_mobys_[i].last_hit_tick < slot->last_hit_tick) {
                slot = &hit_mobys_[i];
            }
        }
    }

    slot->moby = moby;
    slot->uid = moby->uid;
    slot->o_class = moby->o_class;
    slot->last_hit_tick = ticks_;
}

static void send_record(Client* client, u8* record) {
    Moby* target = *(Moby**)(record + 0x34);
    Moby* source = *(Moby**)(record + 0x20);

    // The target can already be gone if the hit destroyed it right away (crates), but its UID is still there
    if (!plausible_moby(target) || !is_level_moby(target)) {
        return;
    }

    // The server's level scripts already sync these
    GameClient* game_client = (GameClient*)client;
    if (game_client->is_hybrid_moby(target->uid)) {
        return;
    }

    // The level hurting itself (enemies, hazards) happens in every game on its own
    if (plausible_moby(source) && source->state >= 0 && is_level_moby(source)) {
        return;
    }

    Packet* packet = new Packet(sizeof(MPPacketMobyHit));
    packet->header->type = MP_PACKET_MOBY_HIT;

    MPPacketMobyHit* hit = (MPPacketMobyHit*)packet->body;
    hit->uid = target->uid;
    hit->o_class = target->o_class;
    memcpy(&hit->vector0, record + 0x00, sizeof(Vec4));
    memcpy(&hit->vector1, record + 0x10, sizeof(Vec4));
    memcpy(&hit->flags, record + 0x24, sizeof(u32));
    hit->field28 = record[0x28];
    hit->field29 = record[0x29];
    memcpy(&hit->field2a, record + 0x2a, sizeof(u16));
    memcpy(&hit->damage, record + 0x2c, sizeof(float));
    memcpy(&hit->field30, record + 0x30, sizeof(u32));

    client->send_ack(packet);

    if (target->state < 0) {
        client->send_ack(Packet::make_moby_killed_packet(target->uid, target->o_class));
    } else {
        remember_hit(target);
    }
}

static void check_hit_mobys(Client* client) {
    for (int i = 0; i < MAX_HIT_MOBYS; i++) {
        HitMoby* hit = &hit_mobys_[i];
        if (hit->moby == nullptr) {
            continue;
        }

        Moby* moby = hit->moby;
        if (moby->state < 0 || moby->uid != hit->uid || moby->o_class != hit->o_class) {
            client->send_ack(Packet::make_moby_killed_packet(hit->uid, hit->o_class));
            hit->moby = nullptr;
            continue;
        }

        if (ticks_ - hit->last_hit_tick > HIT_MEMORY_TICKS) {
            hit->moby = nullptr;
        }
    }
}

static void apply_pending_kills() {
    for (int i = 0; i < MAX_PENDING_KILLS; i++) {
        PendingKill* kill = &pending_kills_[i];
        if (kill->uid == 0) {
            continue;
        }

        Moby* moby = Moby::find_by_uid(kill->uid);
        if (moby == nullptr || moby->o_class != kill->o_class) {
            kill->uid = 0;
            continue;
        }

        if ((s32)(ticks_ - kill->deadline) >= 0) {
            Logger::debug("WorldSync: removing moby %d (o_class %d) another player destroyed", kill->uid, kill->o_class);
            Moby::delete_by_uid(kill->uid);
            kill->uid = 0;
        }
    }
}

void WorldSync::on_tick(Client* client) {
    ticks_ += 1;

    u32 next = DAMAGE_NEXT_RECORD % DAMAGE_RECORD_COUNT;

    if (client == nullptr || ratchet_moby == nullptr || game_state != PlayerControl) {
        next_record_ = next;
        injected_ = 0;
        return;
    }

    while (next_record_ != next) {
        u64 bit = 1ULL << next_record_;

        if (!(injected_ & bit)) {
            send_record(client, DAMAGE_RECORDS + next_record_ * DAMAGE_RECORD_SIZE);
        }

        injected_ &= ~bit;
        next_record_ = (next_record_ + 1) % DAMAGE_RECORD_COUNT;
    }

    check_hit_mobys(client);
    apply_pending_kills();
}

void WorldSync::receive_hit(MPPacketMobyHit* hit) {
    Moby* target = Moby::find_by_uid(hit->uid);
    if (target == nullptr || target->o_class != hit->o_class) {
        return;
    }

    // Whoever made the hit: their copy in our game if we have it
    GameClient* client = (GameClient*)Game::shared().client();
    Moby* source = client != nullptr ? client->moby_for_uuid(hit->sender) : nullptr;
    if (source == nullptr) {
        source = ratchet_moby;
    }

    // Fill in the next damage record exactly like the game does
    u32 index = DAMAGE_NEXT_RECORD % DAMAGE_RECORD_COUNT;
    u8* record = DAMAGE_RECORDS + index * DAMAGE_RECORD_SIZE;

    memset(record, 0, DAMAGE_RECORD_SIZE);
    memcpy(record + 0x00, &hit->vector0, sizeof(Vec4));
    memcpy(record + 0x10, &hit->vector1, sizeof(Vec4));
    memcpy(record + 0x20, &source, sizeof(Moby*));
    memcpy(record + 0x24, &hit->flags, sizeof(u32));
    record[0x28] = hit->field28;
    record[0x29] = hit->field29;
    memcpy(record + 0x2a, &hit->field2a, sizeof(u16));
    memcpy(record + 0x2c, &hit->damage, sizeof(float));
    memcpy(record + 0x30, &hit->field30, sizeof(u32));
    memcpy(record + 0x34, &target, sizeof(Moby*));

    target->damage = (s8)index;
    DAMAGE_NEXT_RECORD = (index + 1) % DAMAGE_RECORD_COUNT;

    injected_ |= 1ULL << index;
}

void WorldSync::receive_killed(MPPacketMobyKilled* killed) {
    if (killed->uid == 0 || killed->uid == 0xffff) {
        return;
    }

    PendingKill* free_slot = nullptr;
    for (int i = 0; i < MAX_PENDING_KILLS; i++) {
        PendingKill* kill = &pending_kills_[i];
        if (kill->uid == killed->uid) {
            return;
        }

        if (kill->uid == 0 && free_slot == nullptr) {
            free_slot = kill;
        }
    }

    if (free_slot != nullptr) {
        free_slot->uid = killed->uid;
        free_slot->o_class = killed->o_class;
        free_slot->deadline = ticks_ + KILL_GRACE_TICKS;
    }
}

void WorldSync::reset() {
    memset(hit_mobys_, 0, sizeof(hit_mobys_));
    memset(pending_kills_, 0, sizeof(pending_kills_));
    injected_ = 0;
    next_record_ = DAMAGE_NEXT_RECORD % DAMAGE_RECORD_COUNT;
}
