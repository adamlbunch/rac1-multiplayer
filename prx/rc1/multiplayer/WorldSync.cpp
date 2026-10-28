//
// See WorldSync.h
//

#include "WorldSync.h"

#include <rc1/Game.h>

#include "EnemySync.h"
#include "LevelMobys.h"
#include "SpawnedMobys.h"
#include "network/GameClient.h"

// How long a moby someone else destroyed gets to go away on its own (play its death) before we remove it
#define KILL_GRACE_TICKS (60 * 3)

// How long we watch a moby our player hit for disappearing
#define HIT_MEMORY_TICKS (60 * 20)

#define MAX_HIT_MOBYS 32
#define MAX_PENDING_KILLS 32
#define MAX_REMOTE_HITS 16

// Bolts that fly out of destroyed crates and enemies
#define BOLT_O_CLASS 13
#define BIG_BOLT_O_CLASS 14
#define BOLT_RADIUS 5.0f

// How long after another player's hit we keep its target's bolts from being picked up, and after it's gone
#define REMOTE_HIT_BOLTS_TICKS (60 * 6)
#define REMOTE_KILL_BOLTS_TICKS (60 * 2)

// What all games know a moby by, see SpawnedMobys
struct MobyKey {
    u16 uid;
    u16 spawn;
    u16 o_class;
};

struct HitMoby {
    Moby* moby;
    MobyKey key;
    u32 last_hit_tick;
};

struct PendingKill {
    bool used;
    MobyKey key;
    u32 deadline;
};

// Mobys another player's hit was the last to land on. When they die of it, every game drops their bolts, but the
//  bolts count is shared, so only the game of the player who made the hit keeps them.
struct RemoteHit {
    Moby* moby;
    MobyKey key;
    Vec4 position;
    u32 hit_tick;
    u32 gone_tick;
    bool gone;
};

static HitMoby hit_mobys_[MAX_HIT_MOBYS];
static PendingKill pending_kills_[MAX_PENDING_KILLS];

// Mobys another player's game recorded as gone for good, which we give a moment to go away on their own here first.
//  Not all of them go away there: Novalis' stalactites record themselves once they've fallen and stay on the floor, so
//  one that changed its state here since (it fell here too, see MoverSync) is doing what it did there, and stays.
#define MAX_PENDING_REMOVALS 64
#define REMOVAL_GRACE_TICKS (60 * 5)

struct PendingRemoval {
    bool used;
    u16 uid;
    u32 deadline;
    bool seen;
    s8 state;   // Its state here when we first looked after the record came
};

static PendingRemoval pending_removals_[MAX_PENDING_REMOVALS];

// Crates placed in the level that were broken since it loaded, here or in another player's game. A respawn sets the
//  level up again in our game only, which brings them back here (and their bolts) while they stay gone in the others,
//  so they're removed again for a while after it. Only crates: other mobys can be tied to each other in ways that
//  only their own code undoes (see safe_to_remove), and after a respawn they come back like in the normal game.
#define FIRST_CRATE_O_CLASS 500
#define LAST_CRATE_O_CLASS 511
#define MAX_DESTROYED 512
#define REDESTROY_TICKS (60 * 5)

static MobyKey destroyed_[MAX_DESTROYED];
static bool kept_[MAX_DESTROYED];          // Left alone since the last respawn: something holds on to it
static int n_destroyed_ = 0;
static int destroyed_planet_ = -1;
static bool redestroy_pending_ = false;     // From when our player is back in control
static u32 redestroy_until_ = 0;

// The next of those to tell a player who just arrived, -1 if there's nothing to tell
#define RESENDS_PER_TICK 4
static int resend_next_ = -1;

// Whether we can take the moby out of the level without its own code: nothing else there holds its address. Mobys that
//  work together keep each other's addresses (a chain of turrets, what one fired), and the game lets them go when one
//  dies of its own; one we just delete would leave the others following an address of nothing, and the game crashes.
static bool safe_to_remove(Moby* target) {
    u32 start = (u32)target;
    u32 end = start + sizeof(Moby);

    for (Moby* moby = LEVEL_MOBYS_START; moby < moby_ptr_end; moby++) {
        if (moby == target || moby->state < 0) {
            continue;
        }

        u32* words = (u32*)moby;
        for (u32 i = 0; i < sizeof(Moby) / 4; i++) {
            // The game's update list goes through every moby
            if (i * 4 != 0x28 && words[i] >= start && words[i] < end) {
                return false;
            }
        }

        u32 vars = (u32)moby->vars;
        if (vars < 0x40000000 || vars >= 0x50000000) {
            continue;
        }

        u32* vars_words = (u32*)vars;
        for (u32 i = 0; i < 0x100 / 4; i++) {
            if (vars_words[i] >= start && vars_words[i] < end) {
                return false;
            }
        }
    }

    return true;
}

static void note_destroyed(const MobyKey* key) {
    if (key->spawn != 0 || key->o_class < FIRST_CRATE_O_CLASS || key->o_class > LAST_CRATE_O_CLASS) {
        return;
    }

    for (int i = 0; i < n_destroyed_; i++) {
        if (destroyed_[i].uid == key->uid && destroyed_[i].o_class == key->o_class) {
            return;
        }
    }

    if (n_destroyed_ < MAX_DESTROYED) {
        destroyed_[n_destroyed_++] = *key;
    }
}
static RemoteHit remote_hits_[MAX_REMOTE_HITS];

static u32 ticks_ = 0;
static u32 next_record_ = 0;

// Records we filled in ourselves for other players' hits, which we must not send back
static u64 injected_ = 0;

static bool plausible_moby(Moby* moby) {
    u32 address = (u32)moby;
    return address >= 0x10000 && address < 0xC0000000 && (address & 0xf) == 0;
}

// What other games know the moby by: placed in the level, or spawned by one of those. False for anything else, like
//  players and what they fired. Also works on a moby that just died.
static bool key_of(Moby* moby, MobyKey* key) {
    if (!plausible_moby(moby) || !SpawnedMobys::identify(moby, &key->uid, &key->spawn)) {
        return false;
    }

    key->o_class = moby->o_class;
    return true;
}

static bool same_key(const MobyKey* a, const MobyKey* b) {
    return a->uid == b->uid && a->spawn == b->spawn && a->o_class == b->o_class;
}

// The moby is alive and still the one we knew under this key (its slot didn't go to another)
static bool still(Moby* moby, const MobyKey* key) {
    MobyKey now;
    return moby->state >= 0 && key_of(moby, &now) && same_key(&now, key);
}

static void remember_hit(Moby* moby, const MobyKey* key) {
    HitMoby* slot = nullptr;

    for (int i = 0; i < MAX_HIT_MOBYS && slot == nullptr; i++) {
        if (hit_mobys_[i].moby == moby && same_key(&hit_mobys_[i].key, key)) {
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
    slot->key = *key;
    slot->last_hit_tick = ticks_;
}

static void remember_remote_hit(Moby* target);

static bool is_remote_hit(Moby* moby) {
    for (int i = 0; i < MAX_REMOTE_HITS; i++) {
        if (remote_hits_[i].moby == moby) {
            return true;
        }
    }

    return false;
}

static void forget_remote_hit(Moby* moby) {
    for (int i = 0; i < MAX_REMOTE_HITS; i++) {
        if (remote_hits_[i].moby == moby) {
            remote_hits_[i].moby = nullptr;
        }
    }
}

static void fill_hit(MPPacketMobyHit* hit, u8* record) {
    memcpy(&hit->vector0, record + 0x00, sizeof(Vec4));
    memcpy(&hit->vector1, record + 0x10, sizeof(Vec4));
    memcpy(&hit->flags, record + 0x24, sizeof(u32));
    hit->field28 = record[0x28];
    hit->field29 = record[0x29];
    memcpy(&hit->field2a, record + 0x2a, sizeof(u16));
    memcpy(&hit->damage, record + 0x2c, sizeof(float));
    memcpy(&hit->field30, record + 0x30, sizeof(u32));
}

// When we last sent a hit on each other player, by their game's rank
#define MAX_HIT_PLAYERS 4
#define PLAYER_HIT_INTERVAL 10
static u32 hit_player_ranks_[MAX_HIT_PLAYERS];
static u32 hit_player_ticks_[MAX_HIT_PLAYERS];

// An enemy we run for everyone hit another player's copy here (see EnemySync): the hit is on them, in their game.
//  Their game takes a hit like it takes any: it decides whether it hurts (they may still be flashing from the last).
//  An enemy can land one every frame on a copy, which nothing makes flash, so they go a few frames apart.
static void send_player_hit(Client* client, u8* record, Moby* source, u32 target_rank) {
    MobyKey source_key;
    if (!key_of(source, &source_key)) {
        return;
    }

    int slot = 0;
    for (int i = 0; i < MAX_HIT_PLAYERS; i++) {
        if (hit_player_ranks_[i] == target_rank) {
            slot = i;
            break;
        }

        if (hit_player_ticks_[i] < hit_player_ticks_[slot]) {
            slot = i;
        }
    }

    if (hit_player_ranks_[slot] == target_rank && ticks_ - hit_player_ticks_[slot] < PLAYER_HIT_INTERVAL) {
        return;
    }

    hit_player_ranks_[slot] = target_rank;
    hit_player_ticks_[slot] = ticks_;

    Packet* packet = new Packet(sizeof(MPPacketMobyHit));
    packet->header->type = MP_PACKET_MOBY_HIT;

    MPPacketMobyHit* hit = (MPPacketMobyHit*)packet->body;
    memset(hit, 0, sizeof(MPPacketMobyHit));
    fill_hit(hit, record);
    hit->source_uid = source_key.uid;
    hit->source_o_class = source_key.o_class;
    hit->source_spawn = source_key.spawn;
    hit->target_rank = target_rank;

    Logger::debug("WorldSync: moby %d/%d (o_class %d) hit another player for %d", source_key.uid, source_key.spawn,
                  source_key.o_class, (int)hit->damage);

    client->send_ack(packet);
}

static void send_record(Client* client, u8* record) {
    Moby* target = *(Moby**)(record + 0x34);
    Moby* source = *(Moby**)(record + 0x20);

    u32 target_rank = EnemySync::player_rank(target);
    if (target_rank != 0) {
        if (EnemySync::runs_for_everyone(source)) {
            send_player_hit(client, record, source, target_rank);
        }
        return;
    }

    // The target can already be gone if the hit destroyed it right away (crates), but it's still where it was
    MobyKey target_key;
    if (!key_of(target, &target_key)) {
        return;
    }

    // The level hurting itself (hazards, a crate's blast) happens in every game on its own, apart from what the
    //  enemies we run do (see EnemySync). When it's set off by another player's hit, like a stack of crates they
    //  knocked over, what it destroys is theirs as well.
    MobyKey source_key;
    bool from_enemy = false;
    if (key_of(source, &source_key)) {
        if (!EnemySync::is_owned(source)) {
            if (is_remote_hit(source)) {
                remember_remote_hit(target);
            }
            return;
        }

        from_enemy = true;
    }

    // Our hit is the latest on it now, so whatever it drops is ours
    forget_remote_hit(target);

    // The server's level scripts already sync these
    GameClient* game_client = (GameClient*)client;
    if (target_key.spawn == 0 && game_client->is_hybrid_moby(target_key.uid)) {
        return;
    }

    Packet* packet = new Packet(sizeof(MPPacketMobyHit));
    packet->header->type = MP_PACKET_MOBY_HIT;

    MPPacketMobyHit* hit = (MPPacketMobyHit*)packet->body;
    hit->uid = target_key.uid;
    hit->o_class = target_key.o_class;
    hit->spawn = target_key.spawn;
    hit->source_uid = from_enemy ? source_key.uid : 0;
    hit->source_o_class = from_enemy ? source_key.o_class : 0xffff;
    hit->source_spawn = from_enemy ? source_key.spawn : 0;
    hit->target_rank = 0;
    fill_hit(hit, record);

    client->send_ack(packet);

    if (target->state < 0) {
        client->send_ack(Packet::make_moby_killed_packet(target_key.uid, target_key.o_class, target_key.spawn));
        note_destroyed(&target_key);
    } else {
        remember_hit(target, &target_key);
    }
}

static void redestroy() {
    if (redestroy_pending_) {
        redestroy_pending_ = false;
        redestroy_until_ = ticks_ + REDESTROY_TICKS;
    }

    if ((s32)(ticks_ - redestroy_until_) >= 0 || ticks_ % 10 != 0) {
        return;
    }

    for (int i = 0; i < n_destroyed_; i++) {
        Moby* moby = kept_[i] ? nullptr : LevelMobys::find(destroyed_[i].uid, destroyed_[i].o_class);
        if (moby == nullptr) {
            continue;
        }

        if (!safe_to_remove(moby)) {
            Logger::debug("WorldSync: leaving crate %d (o_class %d) here, others hold on to it", destroyed_[i].uid,
                          destroyed_[i].o_class);
            kept_[i] = true;
            continue;
        }

        Logger::debug("WorldSync: removing crate %d (o_class %d) again, it was broken before we respawned",
                      destroyed_[i].uid, destroyed_[i].o_class);
        moby->state = -1;
        delete_moby(moby);
    }
}

void WorldSync::on_player_arrived() {
    if (n_destroyed_ > 0) {
        resend_next_ = 0;
    }
}

static void resend_destroyed(Client* client) {
    for (int sent = 0; resend_next_ >= 0 && sent < RESENDS_PER_TICK; sent++) {
        if (resend_next_ >= n_destroyed_) {
            resend_next_ = -1;
            break;
        }

        MobyKey* key = &destroyed_[resend_next_++];
        client->send_ack(Packet::make_moby_killed_packet(key->uid, key->o_class, key->spawn));
    }
}

static void check_hit_mobys(Client* client) {
    for (int i = 0; i < MAX_HIT_MOBYS; i++) {
        HitMoby* hit = &hit_mobys_[i];
        if (hit->moby == nullptr) {
            continue;
        }

        if (!still(hit->moby, &hit->key)) {
            client->send_ack(Packet::make_moby_killed_packet(hit->key.uid, hit->key.o_class, hit->key.spawn));
            note_destroyed(&hit->key);
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
        if (!kill->used) {
            continue;
        }

        Moby* moby = SpawnedMobys::find(kill->key.uid, kill->key.spawn, kill->key.o_class);
        if (moby == nullptr) {
            kill->used = false;
            continue;
        }

        if ((s32)(ticks_ - kill->deadline) >= 0 || EnemySync::is_copy(moby)) {
            kill->used = false;
            if (!EnemySync::is_copy(moby) && !safe_to_remove(moby)) {
                Logger::debug("WorldSync: leaving moby %d/%d (o_class %d) another player destroyed, others hold on "
                              "to it", kill->key.uid, kill->key.spawn, kill->key.o_class);
                continue;
            }

            Logger::debug("WorldSync: removing moby %d/%d (o_class %d) another player destroyed", kill->key.uid,
                          kill->key.spawn, kill->key.o_class);
            moby->state = -1;
            delete_moby(moby);
        }
    }
}

static void remove_remote_hit_bolts() {
    bool any = false;

    for (int i = 0; i < MAX_REMOTE_HITS; i++) {
        RemoteHit* hit = &remote_hits_[i];
        if (hit->moby == nullptr) {
            continue;
        }

        if (!hit->gone && !still(hit->moby, &hit->key)) {
            hit->gone = true;
            hit->gone_tick = ticks_;
        }

        if (!hit->gone) {
            hit->position = hit->moby->position;
        }

        if ((hit->gone && ticks_ - hit->gone_tick > REMOTE_KILL_BOLTS_TICKS) ||
            (!hit->gone && ticks_ - hit->hit_tick > REMOTE_HIT_BOLTS_TICKS)) {
            hit->moby = nullptr;
            continue;
        }

        any = true;
    }

    if (!any) {
        return;
    }

    for (Moby* moby = LEVEL_MOBYS_END; moby < moby_ptr_end; moby++) {
        if (moby->state < 0 || (moby->o_class != BOLT_O_CLASS && moby->o_class != BIG_BOLT_O_CLASS)) {
            continue;
        }

        for (int i = 0; i < MAX_REMOTE_HITS; i++) {
            RemoteHit* hit = &remote_hits_[i];
            if (hit->moby == nullptr) {
                continue;
            }

            float dx = moby->position.x - hit->position.x;
            float dy = moby->position.y - hit->position.y;
            float dz = moby->position.z - hit->position.z;
            if (dx * dx + dy * dy + dz * dz < BOLT_RADIUS * BOLT_RADIUS) {
                moby->state = -1;
                delete_moby(moby);
                break;
            }
        }
    }
}

void WorldSync::on_gone_for_good(int level, int offset, u8 before, u8 after) {
    u8 added = after & ~before;
    if (level != current_planet || added == 0) {
        return;
    }

    for (int bit = 0; bit < 8; bit++) {
        if (!(added & (1 << bit))) {
            continue;
        }

        // The game keeps it as 32-bit words with a bit per UID
        u16 uid = (offset / 4) * 32 + (3 - offset % 4) * 8 + bit;

        for (int i = 0; i < MAX_PENDING_REMOVALS; i++) {
            if (!pending_removals_[i].used) {
                pending_removals_[i].used = true;
                pending_removals_[i].uid = uid;
                pending_removals_[i].deadline = ticks_ + REMOVAL_GRACE_TICKS;
                pending_removals_[i].seen = false;
                break;
            }
        }
    }
}

static void apply_pending_removals() {
    for (int i = 0; i < MAX_PENDING_REMOVALS; i++) {
        PendingRemoval* removal = &pending_removals_[i];
        if (!removal->used) {
            continue;
        }

        Moby* moby = LevelMobys::find(removal->uid);
        if (moby == nullptr) {
            removal->used = false;
            continue;
        }

        if (!removal->seen) {
            removal->seen = true;
            removal->state = moby->state;
        }

        if ((s32)(ticks_ - removal->deadline) >= 0) {
            removal->used = false;
            if (moby->state != removal->state) {
                Logger::debug("WorldSync: leaving moby %d (o_class %d) another player's game recorded as gone for "
                              "good, it went from state %d to %d here", removal->uid, moby->o_class, removal->state,
                              moby->state);
                continue;
            }

            if (!safe_to_remove(moby)) {
                Logger::debug("WorldSync: leaving moby %d (o_class %d) another player's game recorded as gone for "
                              "good, others hold on to it", removal->uid, moby->o_class);
                continue;
            }

            Logger::debug("WorldSync: removing moby %d (o_class %d) another player's game recorded as gone for good",
                          removal->uid, moby->o_class);
            moby->state = -1;
            delete_moby(moby);
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
    apply_pending_removals();
    redestroy();
    resend_destroyed(client);
    remove_remote_hit_bolts();
}

void WorldSync::receive_hit(MPPacketMobyHit* hit, size_t len) {
    // Older games only send hits on mobys placed in the level, made by their player
    bool full = len >= MP_MOBY_HIT_V15_SIZE;

    // An enemy another game runs hit a player there: ours, or someone else's
    Moby* target = nullptr;
    bool on_player = len >= sizeof(MPPacketMobyHit) && hit->target_rank != 0;
    if (on_player) {
        if (hit->target_rank != EnemySync::rank()) {
            return;
        }

        target = ratchet_moby;
    } else {
        target = SpawnedMobys::find(hit->uid, full ? hit->spawn : 0, hit->o_class);
    }

    if (target == nullptr) {
        return;
    }

    // Whoever made the hit: the enemy that did, or the player's copy in our game if we have it
    Moby* source = nullptr;
    if (full && hit->source_o_class != 0xffff) {
        source = SpawnedMobys::find(hit->source_uid, hit->source_spawn, hit->source_o_class);
    } else {
        GameClient* client = (GameClient*)Game::shared().client();
        source = client != nullptr ? client->moby_for_uuid(hit->sender) : nullptr;
    }

    if (source == nullptr) {
        // Our player hit by itself would be knocked back from where it stands
        if (on_player) {
            return;
        }

        source = ratchet_moby;
    }

    if (on_player) {
        Logger::debug("WorldSync: moby %d/%d (o_class %d) in player %d's game hit us for %d", hit->source_uid,
                      hit->source_spawn, hit->source_o_class, hit->sender, (int)hit->damage);
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

    remember_remote_hit(target);
}

// Another player's hit is the latest on this moby now
static void remember_remote_hit(Moby* target) {
    MobyKey key;
    if (!key_of(target, &key)) {
        return;
    }

    RemoteHit* slot = &remote_hits_[0];
    for (int i = 0; i < MAX_REMOTE_HITS; i++) {
        if (remote_hits_[i].moby == target) {
            slot = &remote_hits_[i];
            break;
        }

        if (remote_hits_[i].moby == nullptr || remote_hits_[i].hit_tick < slot->hit_tick) {
            slot = &remote_hits_[i];
        }
    }

    slot->moby = target;
    slot->key = key;
    slot->position = target->position;
    slot->hit_tick = ticks_;
    slot->gone = false;
}

void WorldSync::receive_killed(MPPacketMobyKilled* killed) {
    MobyKey key;
    key.uid = killed->uid;
    key.spawn = killed->spawn;
    key.o_class = killed->o_class;

    // One of our level's: it may come back here when we respawn
    if (key.spawn == 0 && LevelMobys::find(key.uid, key.o_class) != nullptr) {
        note_destroyed(&key);
    }

    PendingKill* free_slot = nullptr;
    for (int i = 0; i < MAX_PENDING_KILLS; i++) {
        PendingKill* kill = &pending_kills_[i];
        if (kill->used && same_key(&kill->key, &key)) {
            return;
        }

        if (!kill->used && free_slot == nullptr) {
            free_slot = kill;
        }
    }

    if (free_slot != nullptr) {
        free_slot->used = true;
        free_slot->key = key;
        free_slot->deadline = ticks_ + KILL_GRACE_TICKS;
        return;
    }

    // A lot at once (what was destroyed before we came): no time to play their deaths
    Moby* moby = SpawnedMobys::find(key.uid, key.spawn, key.o_class);
    if (moby != nullptr && safe_to_remove(moby)) {
        moby->state = -1;
        delete_moby(moby);
    }
}

void WorldSync::ignore_record(u32 index) {
    injected_ |= 1ULL << (index % DAMAGE_RECORD_COUNT);
}

void WorldSync::reset() {
    // A respawn on the same level: what was destroyed is gone again. A new level starts with everything there.
    memset(kept_, 0, sizeof(kept_));
    if (current_planet == destroyed_planet_) {
        redestroy_pending_ = n_destroyed_ > 0;
    } else {
        n_destroyed_ = 0;
        destroyed_planet_ = current_planet;
        redestroy_pending_ = false;
    }
    redestroy_until_ = ticks_;
    resend_next_ = -1;

    memset(hit_mobys_, 0, sizeof(hit_mobys_));
    memset(remote_hits_, 0, sizeof(remote_hits_));
    memset(pending_kills_, 0, sizeof(pending_kills_));
    memset(pending_removals_, 0, sizeof(pending_removals_));
    injected_ = 0;
    next_record_ = DAMAGE_NEXT_RECORD % DAMAGE_RECORD_COUNT;
}
