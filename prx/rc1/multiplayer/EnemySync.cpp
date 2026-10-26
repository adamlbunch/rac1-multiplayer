//
// See EnemySync.h
//

#include "EnemySync.h"

#include <sys/sys_time.h>

#include <rc1/Game.h>

#include "AttackerUpdates.h"
#include "EffectSync.h"
#include "MoverSync.h"
#include "GadgetModels.h"
#include "LevelMobys.h"
#include "SpawnedMobys.h"
#include "Mirror.h"
#include "MobyAnimation.h"
#include "WorldSync.h"
#include "network/GameClient.h"

// The game's moby update loop builds a list of the mobys to update this frame (the ones near the camera and the ones
//  that always update), then for each: steps its animation, runs its update function and builds its transform
SHK_FUNCTION_DEFINE_STATIC_0(0xf959c, Moby*, build_moby_update_list);
SHK_FUNCTION_DEFINE_STATIC_1(0xf98d8, void, animate_moby, Moby*, moby);
SHK_FUNCTION_DEFINE_STATIC_1(0xf31a8, int, build_moby_transform, Moby*, moby);

// Where the game keeps the first moby of this frame's update list
#define MOBY_UPDATE_LIST (*(Moby**)0xa390ac)

// Mode bits: 0x4 skips building the transform, 0x40 skips the animation step, 0x100 makes the game use the
//  transform the moby set itself instead of building one from its position, rotation and scale
#define MODE_NO_TRANSFORM 0x4
#define MODE_NO_ANIMATION 0x40
#define MODE_OWN_TRANSFORM 0x100

typedef void (*MobyUpdateFunction)(Moby*);

#define MAX_ENEMIES 96
#define MAX_LEVEL_MOBYS 4096
#define MAX_SENDERS 4

// States we keep of each enemy, and how many frames behind the newest we show, like Mirror
#define ENEMY_HISTORY 6
#define ENEMY_DELAY 3

// Frames without a state from the game running an enemy before the others may take it over
#define OWNER_TIMEOUT 30

// How much nearer our player has to be to take an enemy over from another game, so it doesn't change hands back and
//  forth while players are about as near
#define CLAIM_MARGIN 4.0f

// Only players this near an enemy take it over from another game. Far off, it makes no difference who runs it, and
//  the distances other games report of it are too old to go by when it moves fast.
#define CLAIM_RADIUS 60.0f

// Players about this near to an enemy are as near, and the games' ranks settle which one runs it
#define TIE_DISTANCE 0.5f

// How often we ask the game running an enemy to hand it over, and how long an ask stands
#define CLAIM_INTERVAL 15
#define CLAIM_TIMEOUT 60

// How many times a game sends an enemy's last state when it hands it over, in case some get lost
#define HANDOFF_SENDS 3

// Enemies no other player is this near get sent only every few frames, and other games move them smoothly from one
//  to the next. With the frames we show them behind, the next state is there by the time it's needed.
#define STREAM_RADIUS 150.0f
#define FAR_SEND_INTERVAL 4

#define MAX_SENT_ENEMIES 64

#define OWNER_NONE 0
#define OWNER_SELF 0xffff

// What we've seen the level's mobys do in their own update, and whether their update can hurt anything (which tells
//  enemies from things like platforms and ships flying by, which other parts of the game can depend on running)
#define TRAIT_DAMAGEABLE 0x1
#define TRAIT_MOVES 0x2
#define TRAIT_ATTACKER 0x4
#define TRAIT_ATTACKER_CHECKED 0x8
#define TRAIT_STATE_CHANGES 0x10    // Its state changes while the level runs, not just as it sets itself up
#define TRAITS_ENEMY (TRAIT_DAMAGEABLE | TRAIT_MOVES | TRAIT_ATTACKER)

// Frames a moby has to move in before it counts as moving on its own. Crates settle for a few after the level loads.
#define MOVING_FRAMES 30

// Updates after which a moby that changes its state does that while the level runs, not as it sets itself up
#define SETTLED_UPDATES 60

// Bolts that fly out of destroyed enemies, which are always each game's own
#define BOLT_O_CLASS 13
#define BIG_BOLT_O_CLASS 14

struct EnemyRecord {
    u32 frame;  // 0 for none
    MPEnemyState state;
    MPEnemyMatrix matrix;
};

struct SyncedEnemy {
    bool used;
    u16 index;              // In our moby table
    u16 uid;                // What other games know it by, see SpawnedMobys
    u16 spawn;
    u16 o_class;

    u16 owner;              // The game running it: OWNER_SELF, or the other player's ID
    float owner_distance;   // How far that game's player is from it
    u32 last_record_tick;

    EnemyRecord history[ENEMY_HISTORY];
    MirrorAnimation animation;

    // While another game runs it, it runs its own update to react to a hit until its AI moves on from the reaction
    bool reacting;
    bool hit_this_frame;
    u8 state_before;
    u8 reaction_state;

    // When we last asked the game running it to hand it over to us
    u32 claim_tick;

    // Another game asked for it, and we hand it over at its next update. Then we send our last state of it a few
    //  times, as the claimant picks up from exactly there.
    u16 handoff_to;
    float handoff_distance;
    u32 handoff_tick;
    u8 handoff_sends;
    EnemyRecord handoff;

    // The state we last put it in, to step its animation on from there between states
    u32 shown_frame;

    // Spawned by an enemy the game that runs it runs: it stays with that game until it's gone. When we made a copy of
    //  it ourselves (we didn't spawn one), the copy never runs and goes when that game stops sending it.
    bool spawned;
    bool copy;
};

struct EnemySender {
    bool active;
    u16 uuid;
    u32 rank;
    u32 newest_frame;
    u32 playback_frame;
    u32 last_packet_tick;
};

static SyncedEnemy enemies_[MAX_ENEMIES];
static EnemySender senders_[MAX_SENDERS];

// Per moby placed in the level: its enemies_ slot plus one, what we've seen it do, and its animation starts
static u8 slots_[MAX_LEVEL_MOBYS];
static u8 traits_[MAX_LEVEL_MOBYS];
static u8 restarts_[MAX_LEVEL_MOBYS];
static u8 blend_frames_[MAX_LEVEL_MOBYS];
static u8 moving_frames_[MAX_LEVEL_MOBYS];
static u8 updates_[MAX_LEVEL_MOBYS];        // How many times it ran, up to SETTLED_UPDATES

static u32 ticks_ = 0;
static u32 frame_ = 0;
static u32 rank_ = 0;

// The moby being updated, and what we noted about it before its update
static Moby* updating_ = nullptr;
static int updating_index_ = -1;
static SyncedEnemy* updating_enemy_ = nullptr;
static Vec4 position_before_;
static s8 state_before_;
static Vec4 rotation_before_;
static u32 damage_next_before_ = 0;

static MPEnemyState outgoing_states_[MAX_SENT_ENEMIES];
static MPEnemyMatrix outgoing_matrices_[MAX_SENT_ENEMIES];
static int n_outgoing_ = 0;

// Where the test tools that read a running game's memory find our state (see rac1-coop's tools)
u32 sync_off_ = 0;

// Moby classes the test tools ask us to leave alone, to find one that can't be run by another game
#define MAX_UNSYNCED_CLASSES 8
static u16 unsynced_classes_[MAX_UNSYNCED_CLASSES];

struct EnemySyncDebugInfo {
    char magic[8];
    SyncedEnemy* enemies;
    u32 enemy_size;
    u32 max_enemies;
    EnemySender* senders;
    u32* ticks;
    u32* off;
    u16* unsynced_classes;
};

EnemySyncDebugInfo enemy_sync_debug_info_ = {
    "ESYNC01", enemies_, sizeof(SyncedEnemy), MAX_ENEMIES, senders_, &ticks_, &sync_off_, unsynced_classes_
};

static bool plausible_moby(Moby* moby) {
    u32 address = (u32)moby;
    return address >= 0x10000 && address < 0xC0000000 && (address & 0xf) == 0;
}

static float square_root(float x) {
    if (x <= 0.0f) {
        return 0.0f;
    }

    // A guess from halving the exponent, then Newton's method
    union {
        float f;
        u32 i;
    } guess;
    guess.f = x;
    guess.i = 0x1fbd1df5 + (guess.i >> 1);

    float root = guess.f;
    for (int i = 0; i < 3; i++) {
        root = 0.5f * (root + x / root);
    }

    return root;
}

static float player_distance(Moby* moby) {
    float dx = ratchet_moby->position.x - moby->position.x;
    float dy = ratchet_moby->position.y - moby->position.y;
    float dz = ratchet_moby->position.z - moby->position.z;
    return square_root(dx * dx + dy * dy + dz * dz);
}

static SyncedEnemy* find_enemy(int index) {
    if (index < 0 || index >= MAX_LEVEL_MOBYS || slots_[index] == 0) {
        return nullptr;
    }

    return &enemies_[slots_[index] - 1];
}

static SyncedEnemy* add_enemy(int index, Moby* moby) {
    if (index < 0 || index >= MAX_LEVEL_MOBYS) {
        return nullptr;
    }

    SyncedEnemy* enemy = find_enemy(index);
    if (enemy != nullptr) {
        return enemy;
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies_[i].used) {
            enemy = &enemies_[i];
            memset(enemy, 0, sizeof(SyncedEnemy));
            enemy->used = true;
            enemy->index = index;
            SpawnedMobys::identify(moby, &enemy->uid, &enemy->spawn);
            enemy->o_class = moby->o_class;
            enemy->owner = OWNER_NONE;
            slots_[index] = i + 1;
            return enemy;
        }
    }

    return nullptr;
}

static void remove_enemy(SyncedEnemy* enemy) {
    slots_[enemy->index] = 0;
    enemy->used = false;
}

// Another game runs it and is still sending it
static bool owner_is_remote(SyncedEnemy* enemy) {
    return enemy->owner != OWNER_NONE && enemy->owner != OWNER_SELF &&
           ticks_ - enemy->last_record_tick <= OWNER_TIMEOUT;
}

static EnemySender* find_sender(u16 uuid) {
    for (int i = 0; i < MAX_SENDERS; i++) {
        if (senders_[i].active && senders_[i].uuid == uuid) {
            return &senders_[i];
        }
    }

    return nullptr;
}

// Whether a game at `distance` from an enemy should run it rather than one at `other_distance`
static bool nearer(float distance, u32 rank, float other_distance, u32 other_rank) {
    if (distance < other_distance - TIE_DISTANCE) {
        return true;
    }

    if (distance > other_distance + TIE_DISTANCE) {
        return false;
    }

    return rank < other_rank;
}

// The state to show this frame: the newest one at or before the owner's playback frame
static EnemyRecord* record_to_show(SyncedEnemy* enemy) {
    EnemySender* sender = find_sender(enemy->owner);
    u32 playback = sender != nullptr ? sender->playback_frame : 0xffffffff;

    EnemyRecord* best = nullptr;
    EnemyRecord* oldest = nullptr;
    for (int i = 0; i < ENEMY_HISTORY; i++) {
        EnemyRecord* record = &enemy->history[i];
        if (record->frame == 0) {
            continue;
        }

        if (record->frame <= playback && (best == nullptr || record->frame > best->frame)) {
            best = record;
        }

        if (oldest == nullptr || record->frame < oldest->frame) {
            oldest = record;
        }
    }

    return best != nullptr ? best : oldest;
}

static void apply_record(SyncedEnemy* enemy, Moby* moby, EnemyRecord* record) {
    MPEnemyState* state = &record->state;

    moby->position.x = state->position[0];
    moby->position.y = state->position[1];
    moby->position.z = state->position[2];
    moby->rotation.x = state->rotation[0];
    moby->rotation.y = state->rotation[1];
    moby->rotation.z = state->rotation[2];
    moby->scale = state->scale;
    moby->alpha = state->alpha;

    if (state->flags & MP_ENEMY_FLAG_MATRIX) {
        memcpy(moby->transform, record->matrix.rows, sizeof(record->matrix.rows));
        moby->mode_bits |= MODE_OWN_TRANSFORM;
    } else {
        moby->mode_bits &= ~MODE_OWN_TRANSFORM;
    }

    MobyAnimation::apply(moby, &state->anim, &enemy->animation, true);
}

// The first state after `frame`, or null
static EnemyRecord* record_after(SyncedEnemy* enemy, u32 frame) {
    EnemyRecord* next = nullptr;
    for (int i = 0; i < ENEMY_HISTORY; i++) {
        EnemyRecord* record = &enemy->history[i];
        if (record->frame > frame && (next == nullptr || record->frame < next->frame)) {
            next = record;
        }
    }

    return next;
}

// The angle `t` of the way from `from` to `to`, turning the short way
static float lerp_angle(float from, float to, float t) {
    const float pi = 3.14159265f;

    float delta = to - from;
    while (delta > pi) {
        delta -= 2 * pi;
    }
    while (delta < -pi) {
        delta += 2 * pi;
    }

    return from + delta * t;
}

static void apply_state(SyncedEnemy* enemy, Moby* moby) {
    EnemyRecord* record = record_to_show(enemy);
    if (record == nullptr) {
        return;
    }

    // A new state: all of it. Between states, the game steps its animation on as usual.
    if (record->frame != enemy->shown_frame) {
        apply_record(enemy, moby, record);
        enemy->shown_frame = record->frame;
    }

    // Enemies far from everyone come a few frames apart: move them along to the next state
    EnemySender* sender = find_sender(enemy->owner);
    if (sender == nullptr || sender->playback_frame <= record->frame || (record->state.flags & MP_ENEMY_FLAG_MATRIX)) {
        return;
    }

    EnemyRecord* next = record_after(enemy, record->frame);
    MPEnemyState* from = &record->state;
    MPEnemyState* to = nullptr;
    float t = 0.0f;

    if (next != nullptr) {
        to = &next->state;
        t = (float)(sender->playback_frame - record->frame) / (float)(next->frame - record->frame);
    } else {
        // The next one is late: keep it going the way it was going, for as long as states come apart
        EnemyRecord* before = nullptr;
        for (int i = 0; i < ENEMY_HISTORY; i++) {
            EnemyRecord* other = &enemy->history[i];
            if (other->frame != 0 && other->frame < record->frame && (before == nullptr || other->frame > before->frame)) {
                before = other;
            }
        }

        if (before == nullptr || record->frame - before->frame > FAR_SEND_INTERVAL) {
            return;
        }

        // From the one before through this one, and on as far again
        to = &record->state;
        from = &before->state;
        t = 1.0f + (float)(sender->playback_frame - record->frame) / (float)(record->frame - before->frame);
    }

    if (t > 2.0f) {
        t = 2.0f;
    }

    moby->position.x = from->position[0] + (to->position[0] - from->position[0]) * t;
    moby->position.y = from->position[1] + (to->position[1] - from->position[1]) * t;
    moby->position.z = from->position[2] + (to->position[2] - from->position[2]) * t;
    moby->rotation.x = lerp_angle(from->rotation[0], to->rotation[0], t);
    moby->rotation.y = lerp_angle(from->rotation[1], to->rotation[1], t);
    moby->rotation.z = lerp_angle(from->rotation[2], to->rotation[2], t);
}

// The enemy is fighting someone in another game, so what it hit in its update here wasn't really hit
static void neutralize_damage(Moby* source) {
    u32 next = DAMAGE_NEXT_RECORD % DAMAGE_RECORD_COUNT;

    for (u32 i = damage_next_before_ % DAMAGE_RECORD_COUNT; i != next; i = (i + 1) % DAMAGE_RECORD_COUNT) {
        u8* record = DAMAGE_RECORDS + i * DAMAGE_RECORD_SIZE;

        u32 none = 0;
        memcpy(record + 0x24, &none, sizeof(u32));  // flags
        memcpy(record + 0x2c, &none, sizeof(u32));  // damage

        Moby* target = *(Moby**)(record + 0x34);
        if (plausible_moby(target) && target->damage == (s8)i) {
            target->damage = -1;
        }

        WorldSync::ignore_record(i);
    }
}

static void capture_state(SyncedEnemy* enemy, Moby* moby, float distance, MPEnemyState* state,
                          MPEnemyMatrix* matrix) {
    memset(state, 0, sizeof(MPEnemyState));

    state->uid = enemy->uid;
    state->spawn = enemy->spawn;
    state->o_class = moby->o_class;
    state->state = moby->state;
    state->alpha = moby->alpha;
    state->distance = distance;
    state->position[0] = moby->position.x;
    state->position[1] = moby->position.y;
    state->position[2] = moby->position.z;
    state->rotation[0] = moby->rotation.x;
    state->rotation[1] = moby->rotation.y;
    state->rotation[2] = moby->rotation.z;
    state->scale = moby->scale;

    MobyAnimation::capture(moby, &state->anim);
    state->anim.restarts = restarts_[enemy->index];
    state->anim.blend_frames = blend_frames_[enemy->index];

    if (moby->mode_bits & MODE_OWN_TRANSFORM) {
        state->flags |= MP_ENEMY_FLAG_MATRIX;
        memcpy(matrix->rows, moby->transform, sizeof(matrix->rows));
    }
}

static void queue_state(MPEnemyState* state, MPEnemyMatrix* matrix) {
    if (n_outgoing_ >= MAX_SENT_ENEMIES) {
        return;
    }

    memcpy(&outgoing_states_[n_outgoing_], state, sizeof(MPEnemyState));
    if (state->flags & MP_ENEMY_FLAG_MATRIX) {
        memcpy(&outgoing_matrices_[n_outgoing_], matrix, sizeof(MPEnemyMatrix));
    }

    n_outgoing_ += 1;
}

static void send_state(SyncedEnemy* enemy, Moby* moby, float distance, float others_sq) {
    // Every frame while another player could see it, otherwise now and then so the others know we run it
    if (others_sq > STREAM_RADIUS * STREAM_RADIUS && (frame_ + enemy->index) % FAR_SEND_INTERVAL != 0) {
        return;
    }

    if (n_outgoing_ >= MAX_SENT_ENEMIES) {
        return;
    }

    capture_state(enemy, moby, distance, &outgoing_states_[n_outgoing_], &outgoing_matrices_[n_outgoing_]);
    if (enemy->spawned) {
        outgoing_states_[n_outgoing_].flags |= MP_ENEMY_FLAG_SPAWNED;
    }
    n_outgoing_ += 1;
}

static void send_states() {
    Client* client = Game::shared().connected_client();
    if (client == nullptr) {
        n_outgoing_ = 0;
        return;
    }

    // As many enemies per packet as fit in the packet's own buffer, so nothing is allocated for them
    const size_t max_body = sizeof(((Packet*)0)->payload_buffer) - sizeof(MPPacketHeader);

    int sent = 0;
    while (sent < n_outgoing_) {
        size_t len = sizeof(MPPacketEnemyState);
        int count = 0;
        while (sent + count < n_outgoing_ && count < 255) {
            size_t size = sizeof(MPEnemyState);
            if (outgoing_states_[sent + count].flags & MP_ENEMY_FLAG_MATRIX) {
                size += sizeof(MPEnemyMatrix);
            }

            if (len + size > max_body) {
                break;
            }

            len += size;
            count += 1;
        }

        Packet packet = Packet(len);
        packet.header->type = MP_PACKET_ENEMY_STATE;
        packet.retain_after_send = true;

        MPPacketEnemyState* body = (MPPacketEnemyState*)packet.body;
        body->n_enemies = count;
        body->frame = frame_;
        body->rank = rank_;

        u8* data = (u8*)body + sizeof(MPPacketEnemyState);
        for (int i = sent; i < sent + count; i++) {
            memcpy(data, &outgoing_states_[i], sizeof(MPEnemyState));
            data += sizeof(MPEnemyState);

            if (outgoing_states_[i].flags & MP_ENEMY_FLAG_MATRIX) {
                memcpy(data, &outgoing_matrices_[i], sizeof(MPEnemyMatrix));
                data += sizeof(MPEnemyMatrix);
            }
        }

        client->send(&packet);
        sent += count;
    }

    n_outgoing_ = 0;
}

static bool can_attack(Moby* moby) {
    u32 update = (u32)moby->p_update;
    int low = 0;
    int high = sizeof(attacker_updates) / sizeof(attacker_updates[0]) - 1;

    while (low <= high) {
        int middle = (low + high) / 2;
        if (attacker_updates[middle] == update) {
            return true;
        }

        if (attacker_updates[middle] < update) {
            low = middle + 1;
        } else {
            high = middle - 1;
        }
    }

    return false;
}

// Position in our moby table of a moby other games know too (placed in the level or spawned by one that is, see
//  SpawnedMobys), or -1
static int sync_index(Moby* moby) {
    u16 uid, spawn;
    if (moby < LEVEL_MOBYS_START || moby >= moby_ptr_end || !SpawnedMobys::identify(moby, &uid, &spawn)) {
        return -1;
    }

    for (int i = 0; i < MAX_UNSYNCED_CLASSES; i++) {
        if (unsynced_classes_[i] != 0 && moby->o_class == (short)unsynced_classes_[i]) {
            return -1;
        }
    }

    int index = moby - LEVEL_MOBYS_START;
    return index < MAX_LEVEL_MOBYS ? index : -1;
}

// Before the moby's animation step and update. Returns whether its update should run.
static bool before_update(Moby* moby) {
    updating_ = moby;
    updating_enemy_ = nullptr;
    EffectSync::before_moby_update();
    updating_index_ = sync_index(moby);

    if (updating_index_ < 0) {
        return true;
    }

    position_before_ = moby->position;
    state_before_ = moby->state;
    rotation_before_ = moby->rotation;

    SyncedEnemy* enemy = find_enemy(updating_index_);

    // A copy of what another game's enemy spawned: never runs here
    if (enemy != nullptr && enemy->copy) {
        updating_enemy_ = enemy;
        return false;
    }

    if (enemy == nullptr) {
        if (!(sync_off_ & SYNC_OFF_MOVERS)) {
            MoverSync::before_update(moby, updating_index_);
        }
        return true;
    }

    if (!(owner_is_remote(enemy) || enemy->reacting) || (sync_off_ & SYNC_OFF_REMOTE_ENEMIES)) {
        return true;
    }

    updating_enemy_ = enemy;
    damage_next_before_ = DAMAGE_NEXT_RECORD;
    enemy->state_before = moby->state;

    if (enemy->reacting) {
        return true;
    }

    // A hit landed on it: it reacts the way it does in the game running it
    if (moby->damage != -1) {
        enemy->reacting = true;
        enemy->hit_this_frame = true;
        return true;
    }

    // Its AI picks up from wherever the game running it is whenever it runs again
    EnemyRecord* record = record_to_show(enemy);
    if (record != nullptr && (s8)record->state.state >= 0 && !(sync_off_ & SYNC_OFF_STATE_COPY)) {
        moby->state = record->state.state;
    }

    return false;
}

// After the moby's update, before the game builds its transform
static void after_update(Moby* moby) {
    if (updating_index_ < 0) {
        return;
    }

    SyncedEnemy* enemy = updating_enemy_;

    if (enemy == nullptr) {
        // Learn which of the level's mobys move on their own
        if (moby->state >= 0 && (moby->position.x != position_before_.x || moby->position.y != position_before_.y ||
                                 moby->position.z != position_before_.z || moby->rotation.x != rotation_before_.x ||
                                 moby->rotation.y != rotation_before_.y || moby->rotation.z != rotation_before_.z)) {
            if (moving_frames_[updating_index_] < MOVING_FRAMES) {
                moving_frames_[updating_index_] += 1;
            } else {
                traits_[updating_index_] |= TRAIT_MOVES;
            }
        }

        // And which change their state while the level runs: doors, buttons, bridges, hazards going off...
        if (updates_[updating_index_] < SETTLED_UPDATES) {
            updates_[updating_index_] += 1;
        } else if (moby->state != state_before_ && moby->state >= 0 && state_before_ >= 0) {
            traits_[updating_index_] |= TRAIT_STATE_CHANGES;
        }

        return;
    }

    if (enemy->reacting) {
        neutralize_damage(moby);

        // What it shows reacting comes from the game running it, see EffectSync
        EffectSync::discard_moby_effects();

        if (enemy->hit_this_frame) {
            enemy->hit_this_frame = false;
            enemy->reaction_state = moby->state;

            // The hit didn't change what it's doing
            if (moby->state == enemy->state_before) {
                enemy->reacting = false;
            }
        } else if (moby->state != enemy->reaction_state) {
            enemy->reacting = false;
        }

        if (moby->state < 0) {
            return;
        }

        // Its own reaction plays out right away, while the one from the game running it is still on its way. Once
        //  that one shows up, it follows it, so it flies off and lands the same way. Its own AI keeps going, so it
        //  still dies (or recovers) here when it lands where the other game's did.
        if (enemy->reacting) {
            EnemyRecord* record = record_to_show(enemy);
            if (record != nullptr && record->state.state == (u8)moby->state) {
                apply_state(enemy, moby);
            }

            return;
        }
    }

    if (owner_is_remote(enemy)) {
        apply_state(enemy, moby);
    }
}

// After the game built the moby's transform: what it looks like this frame
static void after_transform(Moby* moby) {
    int index = updating_index_;
    updating_ = nullptr;
    updating_index_ = -1;

    if (index < 0 || moby->state < 0 || ratchet_moby == nullptr || game_state != PlayerControl ||
        (sync_off_ & SYNC_OFF_ENEMY_SENDING)) {
        return;
    }

    SyncedEnemy* enemy = find_enemy(index);

    // Reacting to a hit on its own while another game runs it
    if (enemy != nullptr && enemy->reacting) {
        return;
    }

    if (!(traits_[index] & TRAIT_ATTACKER_CHECKED)) {
        traits_[index] |= TRAIT_ATTACKER_CHECKED | (can_attack(moby) ? TRAIT_ATTACKER : 0);
    }

    // What an enemy we run spawns (a shot, debris...) is drawn from ours in other games until it's gone
    if (enemy == nullptr && moby->o_class != BOLT_O_CLASS && moby->o_class != BIG_BOLT_O_CLASS) {
        Moby* spawner = SpawnedMobys::spawner_of(moby);
        SyncedEnemy* spawner_enemy = spawner != nullptr ? find_enemy(sync_index(spawner)) : nullptr;
        if (spawner_enemy != nullptr && spawner_enemy->owner == OWNER_SELF) {
            enemy = add_enemy(index, moby);
            if (enemy != nullptr) {
                enemy->owner = OWNER_SELF;
                enemy->spawned = true;
            }
        }
    }

    if (enemy != nullptr && enemy->spawned) {
        if (enemy->owner == OWNER_SELF && Mirror::nearest_player_distance_sq(&moby->position) < 1e29f) {
            send_state(enemy, moby, player_distance(moby), Mirror::nearest_player_distance_sq(&moby->position));
        }
        return;
    }

    if (enemy == nullptr && (traits_[index] & TRAITS_ENEMY) != TRAITS_ENEMY) {
        // Things that move on their own but aren't enemies: in step with the game whose player is nearest
        if ((traits_[index] & (TRAIT_MOVES | TRAIT_STATE_CHANGES)) && !(sync_off_ & SYNC_OFF_MOVERS)) {
            MoverSync::after_update(moby, index, player_distance(moby), (traits_[index] & TRAIT_MOVES) != 0,
                                    (traits_[index] & TRAIT_STATE_CHANGES) != 0);
        }
        return;
    }

    float others_sq = Mirror::nearest_player_distance_sq(&moby->position);
    if (others_sq >= 1e29f) {
        return;
    }

    float distance = player_distance(moby);

    if (enemy != nullptr && owner_is_remote(enemy)) {
        // We just handed it over: our last state of it again, in case it got lost
        if (enemy->handoff_sends > 0) {
            enemy->handoff_sends -= 1;
            queue_state(&enemy->handoff.state, &enemy->handoff.matrix);
        }

        // Another game runs it. Once our player is clearly nearer than theirs, ask that game to hand it over, so its
        //  AI picks up here from exactly where it left off there.
        if (distance < CLAIM_RADIUS && distance + CLAIM_MARGIN < enemy->owner_distance &&
            ticks_ - enemy->claim_tick >= CLAIM_INTERVAL) {
            enemy->claim_tick = ticks_;

            MPEnemyState claim;
            MPEnemyMatrix matrix;
            capture_state(enemy, moby, distance, &claim, &matrix);
            claim.flags = MP_ENEMY_FLAG_CLAIM;
            queue_state(&claim, &matrix);
        }

        return;
    }

    if (enemy == nullptr || enemy->owner != OWNER_SELF) {
        // Nobody runs it, and another player is nearer: theirs to take
        if (distance * distance > others_sq) {
            return;
        }

        if (enemy == nullptr) {
            enemy = add_enemy(index, moby);
            if (enemy == nullptr) {
                return;
            }
        }

        Logger::debug("EnemySync %u: running moby %d/%d (o_class %d) for everyone", ticks_, enemy->uid, enemy->spawn,
                      moby->o_class);
        enemy->owner = OWNER_SELF;
        enemy->animation.valid = false;
    }

    // Another game asked for it: this frame's state is the last one we run, the claimant picks up from there
    if (enemy->handoff_to != 0 && ticks_ - enemy->handoff_tick < CLAIM_TIMEOUT) {
        Logger::debug("EnemySync %u: handing moby %d/%d (o_class %d) over to player %d", ticks_, enemy->uid,
                      enemy->spawn, moby->o_class,
                      enemy->handoff_to);

        enemy->handoff.frame = frame_;
        capture_state(enemy, moby, distance, &enemy->handoff.state, &enemy->handoff.matrix);
        enemy->handoff.state.flags |= MP_ENEMY_FLAG_HANDOFF;
        queue_state(&enemy->handoff.state, &enemy->handoff.matrix);
        enemy->handoff_sends = HANDOFF_SENDS;

        enemy->owner = enemy->handoff_to;
        enemy->owner_distance = enemy->handoff_distance;
        enemy->last_record_tick = ticks_;
        enemy->handoff_to = 0;
        memset(enemy->history, 0, sizeof(enemy->history));
        enemy->animation.valid = false;
        return;
    }

    enemy->handoff_to = 0;

    send_state(enemy, moby, distance, others_sq);
}

bool EnemySync::update_mobys() {
    if (Game::shared().connected_client() == nullptr || (sync_off_ & SYNC_OFF_UPDATE_LOOP)) {
        return false;
    }

    frame_ += 1;

    if (rank_ == 0) {
        rank_ = (u32)sys_time_get_system_time() | 1;
    }

    EffectSync::begin_frame();

    Moby* moby = build_moby_update_list();
    MOBY_UPDATE_LIST = moby;

    for (; moby != nullptr; moby = (Moby*)moby->p_chain) {
        if ((u8)moby->state >= 0x80) {
            continue;
        }

        bool run = before_update(moby);

        if (!(moby->mode_bits & MODE_NO_ANIMATION)) {
            animate_moby(moby);
        }

        if (run && moby->p_update != nullptr) {
            ((MobyUpdateFunction)moby->p_update)(moby);
        }

        EffectSync::after_moby_update(moby);
        after_update(moby);

        if (!(moby->mode_bits & MODE_NO_TRANSFORM)) {
            build_moby_transform(moby);
        }

        after_transform(moby);
    }

    updating_ = nullptr;
    updating_index_ = -1;

    send_states();
    MoverSync::send(frame_);
    EffectSync::end_frame();

    return true;
}

Moby* EnemySync::updating() {
    return updating_;
}

static void log_summary() {
    int seen = 0;
    for (int i = 0; i < MAX_LEVEL_MOBYS; i++) {
        if ((traits_[i] & TRAITS_ENEMY) == TRAITS_ENEMY) {
            seen += 1;
        }
    }

    int ours = 0;
    int theirs = 0;
    int reacting = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies_[i].used) {
            continue;
        }

        if (enemies_[i].owner == OWNER_SELF) {
            ours += 1;
        } else if (owner_is_remote(&enemies_[i])) {
            theirs += 1;
        }

        if (enemies_[i].reacting) {
            reacting += 1;
        }
    }

    float others_sq = ratchet_moby != nullptr ? Mirror::nearest_player_distance_sq(&ratchet_moby->position) : 1e30f;
    Logger::info("EnemySync: %d enemies seen, running %d, %d from other games (%d reacting), other player %s",
                 seen, ours, theirs, reacting, others_sq < 1e29f ? "here" : "not here");
}

void EnemySync::on_tick(Client* client) {
    ticks_ += 1;

    if (client == nullptr || ratchet_moby == nullptr || game_state != PlayerControl) {
        return;
    }

    // Play back one frame of each other game per frame, a few frames behind its newest, catching up if we fall behind
    for (int i = 0; i < MAX_SENDERS; i++) {
        EnemySender* sender = &senders_[i];
        if (!sender->active) {
            continue;
        }

        if (ticks_ - sender->last_packet_tick > OWNER_TIMEOUT * 2) {
            sender->active = false;
            continue;
        }

        sender->playback_frame += 1;
        if (sender->newest_frame > sender->playback_frame + ENEMY_DELAY + 4) {
            sender->playback_frame = sender->newest_frame - ENEMY_DELAY;
        }
        if (sender->playback_frame > sender->newest_frame) {
            sender->playback_frame = sender->newest_frame;
        }
    }

    if (ticks_ % 600 == 0) {
        log_summary();
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        SyncedEnemy* enemy = &enemies_[i];
        if (!enemy->used) {
            continue;
        }

        Moby* moby = LEVEL_MOBYS_START + enemy->index;
        u16 uid, spawn;
        if (moby >= moby_ptr_end || moby->state < 0 || moby->o_class != (short)enemy->o_class ||
            !SpawnedMobys::identify(moby, &uid, &spawn) || uid != enemy->uid || spawn != enemy->spawn) {
            // Gone in our game. If we ran it, it goes in the others too.
            if (enemy->owner == OWNER_SELF) {
                client->send_ack(Packet::make_moby_killed_packet(enemy->uid, enemy->o_class, enemy->spawn));
            }

            remove_enemy(enemy);
            continue;
        }

        // Whoever ran it stopped sending it, so it's back to running on its own until someone takes it. A copy we made
        //  goes with it.
        if (enemy->owner != OWNER_SELF && !owner_is_remote(enemy) && !enemy->reacting) {
            if (enemy->copy) {
                moby->state = -1;
                delete_moby(moby);
            }
            remove_enemy(enemy);
        }
    }
}

// A copy of a moby another game's enemy spawned, which our copy of that enemy didn't spawn (it doesn't run here)
static Moby* make_copy(MPEnemyState* state) {
    if (state->spawn == 0 || GadgetModels::loaded_class(state->o_class) == nullptr) {
        return nullptr;
    }

    Moby* moby = spawn_moby(state->o_class);
    if (moby == nullptr) {
        return nullptr;
    }

    EnemySync::on_spawned(moby);
    SpawnedMobys::adopt(moby, state->uid, state->spawn);

    moby->position.x = state->position[0];
    moby->position.y = state->position[1];
    moby->position.z = state->position[2];
    moby->rotation.x = state->rotation[0];
    moby->rotation.y = state->rotation[1];
    moby->rotation.z = state->rotation[2];
    moby->scale = state->scale;
    moby->collision = nullptr;

    return moby;
}

static void receive_state(EnemySender* sender_info, u32 frame, MPEnemyState* state, MPEnemyMatrix* matrix) {
    u16 sender = sender_info->uuid;

    Moby* moby = SpawnedMobys::find(state->uid, state->spawn, state->o_class);
    bool copy = false;
    if (moby == nullptr && (state->flags & MP_ENEMY_FLAG_SPAWNED) && !(state->flags & MP_ENEMY_FLAG_CLAIM)) {
        moby = make_copy(state);
        copy = moby != nullptr;
    }

    int index = sync_index(moby);
    if (moby == nullptr || index < 0) {
        return;
    }

    SyncedEnemy* enemy = find_enemy(index);

    if (state->flags & MP_ENEMY_FLAG_CLAIM) {
        // Another game asks for an enemy we run
        if (enemy != nullptr && enemy->owner == OWNER_SELF && !enemy->spawned &&
            nearer(state->distance, sender_info->rank, player_distance(moby), rank_)) {
            enemy->handoff_to = sender;
            enemy->handoff_distance = state->distance;
            enemy->handoff_tick = ticks_;
        }

        return;
    }

    if (state->flags & MP_ENEMY_FLAG_HANDOFF) {
        if (enemy != nullptr && enemy->owner == sender && enemy->claim_tick != 0 &&
            ticks_ - enemy->claim_tick < CLAIM_TIMEOUT) {
            // The game running it handed it to us: it picks up from exactly where that game left it
            Logger::debug("EnemySync %u: took over moby %d/%d (o_class %d) from player %d", ticks_, state->uid,
                          state->spawn, state->o_class, sender);

            EnemyRecord record;
            record.frame = frame;
            memcpy(&record.state, state, sizeof(MPEnemyState));
            if (matrix != nullptr) {
                memcpy(&record.matrix, matrix, sizeof(MPEnemyMatrix));
            }

            apply_record(enemy, moby, &record);
            if ((s8)state->state >= 0) {
                moby->state = state->state;
            }

            enemy->owner = OWNER_SELF;
            enemy->claim_tick = 0;
            enemy->reacting = false;
            return;
        }

        // A repeat of one we already took, or meant for someone else
        if (enemy == nullptr || enemy->owner != sender) {
            return;
        }
    }

    if (enemy != nullptr && enemy->owner == OWNER_SELF) {
        // We both run it: the game whose player is nearer keeps it
        if (!nearer(state->distance, sender_info->rank, player_distance(moby), rank_)) {
            return;
        }
    } else if (enemy != nullptr && owner_is_remote(enemy) && enemy->owner != sender) {
        EnemySender* owner = find_sender(enemy->owner);
        if (owner != nullptr && !nearer(state->distance, sender_info->rank, enemy->owner_distance, owner->rank)) {
            return;
        }
    }

    if (enemy == nullptr) {
        enemy = add_enemy(index, moby);
        if (enemy == nullptr) {
            if (copy) {
                delete_moby(moby);
            }
            return;
        }
    }

    if (state->flags & MP_ENEMY_FLAG_SPAWNED) {
        enemy->spawned = true;
        enemy->copy = enemy->copy || copy;
    }

    if (enemy->owner != sender) {
        Logger::debug("EnemySync %u: moby %d/%d (o_class %d) is run by player %d", ticks_, state->uid, state->spawn,
                      state->o_class, sender);
        enemy->owner = sender;
        enemy->handoff_to = 0;
        memset(enemy->history, 0, sizeof(enemy->history));
        enemy->animation.valid = false;
        enemy->shown_frame = 0;
    }

    enemy->owner_distance = state->distance;
    enemy->last_record_tick = ticks_;

    // In place of the oldest state we have
    EnemyRecord* slot = nullptr;
    for (int i = 0; i < ENEMY_HISTORY; i++) {
        EnemyRecord* record = &enemy->history[i];
        if (record->frame == frame) {
            slot = record;
            break;
        }

        if (slot == nullptr || record->frame < slot->frame) {
            slot = record;
        }
    }

    slot->frame = frame;
    memcpy(&slot->state, state, sizeof(MPEnemyState));
    if (matrix != nullptr) {
        memcpy(&slot->matrix, matrix, sizeof(MPEnemyMatrix));
    }
}

void EnemySync::receive(MPPacketEnemyState* packet, size_t len) {
    if (packet->sender == 0 || packet->frame == 0 || game_state != PlayerControl || ratchet_moby == nullptr ||
        len < sizeof(MPPacketEnemyState)) {
        return;
    }

    EnemySender* sender = find_sender(packet->sender);

    // A frame far behind what we have means the sender started counting over, e.g. they reconnected with the same ID
    if (sender != nullptr && packet->frame + 120 < sender->newest_frame) {
        sender->active = false;
        sender = nullptr;
    }

    if (sender == nullptr) {
        for (int i = 0; i < MAX_SENDERS && sender == nullptr; i++) {
            if (!senders_[i].active) {
                sender = &senders_[i];
                sender->active = true;
                sender->uuid = packet->sender;
                sender->newest_frame = packet->frame;
                sender->playback_frame = packet->frame > ENEMY_DELAY ? packet->frame - ENEMY_DELAY : 0;
            }
        }

        if (sender == nullptr) {
            return;
        }
    }

    sender->last_packet_tick = ticks_;
    sender->rank = packet->rank;
    if (packet->frame > sender->newest_frame) {
        sender->newest_frame = packet->frame;
    }

    u8* data = (u8*)packet + sizeof(MPPacketEnemyState);
    u8* end = (u8*)packet + len;
    for (u8 i = 0; i < packet->n_enemies; i++) {
        if (data + sizeof(MPEnemyState) > end) {
            break;
        }

        MPEnemyState* state = (MPEnemyState*)data;
        data += sizeof(MPEnemyState);

        MPEnemyMatrix* matrix = nullptr;
        if (state->flags & MP_ENEMY_FLAG_MATRIX) {
            if (data + sizeof(MPEnemyMatrix) > end) {
                break;
            }

            matrix = (MPEnemyMatrix*)data;
            data += sizeof(MPEnemyMatrix);
        }

        receive_state(sender, packet->frame, state, matrix);
    }
}

bool EnemySync::is_owned(Moby* moby) {
    SyncedEnemy* enemy = find_enemy(sync_index(moby));
    return enemy != nullptr && enemy->owner == OWNER_SELF;
}

bool EnemySync::is_copy(Moby* moby) {
    SyncedEnemy* enemy = find_enemy(sync_index(moby));
    return enemy != nullptr && enemy->copy;
}

u32 EnemySync::frame() {
    return frame_;
}

bool EnemySync::shown_frame(u16 sender, u32* frame) {
    for (int i = 0; i < MAX_SENDERS; i++) {
        if (senders_[i].active && senders_[i].uuid == sender) {
            *frame = senders_[i].playback_frame;
            return true;
        }
    }

    return false;
}

bool EnemySync::is_remote(Moby* moby) {
    SyncedEnemy* enemy = find_enemy(sync_index(moby));
    return enemy != nullptr && (owner_is_remote(enemy) || enemy->reacting);
}

void EnemySync::on_animation_started(Moby* moby, u32 blend_frames) {
    if (moby < LEVEL_MOBYS_START || moby >= moby_ptr_end) {
        return;
    }

    u32 index = moby - LEVEL_MOBYS_START;
    if (index >= MAX_LEVEL_MOBYS) {
        return;
    }

    restarts_[index] += 1;
    blend_frames_[index] = blend_frames > 0xff ? 0xff : (u8)blend_frames;
}

void EnemySync::on_get_damage(Moby* moby) {
    if (moby == updating_ && updating_index_ >= 0) {
        traits_[updating_index_] |= TRAIT_DAMAGEABLE;
    }
}

void EnemySync::on_spawned(Moby* moby) {
    int index = moby - LEVEL_MOBYS_START;
    if (moby == nullptr || index < 0 || index >= MAX_LEVEL_MOBYS) {
        return;
    }

    // A new moby in the slot: forget what we knew about the one before, which died since
    SyncedEnemy* enemy = find_enemy(index);
    if (enemy != nullptr) {
        Client* client = Game::shared().connected_client();
        if (enemy->owner == OWNER_SELF && client != nullptr) {
            client->send_ack(Packet::make_moby_killed_packet(enemy->uid, enemy->o_class, enemy->spawn));
        }

        remove_enemy(enemy);
    }

    traits_[index] = 0;
    moving_frames_[index] = 0;
    updates_[index] = 0;
    restarts_[index] = 0;
    blend_frames_[index] = 0;
}

void EnemySync::reset() {
    Logger::debug("EnemySync %u: reset", ticks_);

    SpawnedMobys::reset();

    memset(enemies_, 0, sizeof(enemies_));
    memset(senders_, 0, sizeof(senders_));
    memset(slots_, 0, sizeof(slots_));
    memset(traits_, 0, sizeof(traits_));
    memset(restarts_, 0, sizeof(restarts_));
    memset(blend_frames_, 0, sizeof(blend_frames_));
    memset(moving_frames_, 0, sizeof(moving_frames_));
    memset(updates_, 0, sizeof(updates_));

    updating_ = nullptr;
    updating_index_ = -1;
    updating_enemy_ = nullptr;
    n_outgoing_ = 0;
}

extern "C" bool _c_update_mobys() {
    return EnemySync::update_mobys();
}

extern "C" void _c_on_moby_animation_started(Moby* moby, u32 blend_frames) {
    EnemySync::on_animation_started(moby, blend_frames);
}

extern "C" void _c_on_moby_get_damage(Moby* moby) {
    EnemySync::on_get_damage(moby);
}

extern "C" void _c_on_moby_spawned(Moby* moby) {
    // Whose update spawned it, before anything else uses its slot
    SpawnedMobys::on_spawned(moby, updating_);
    EnemySync::on_spawned(moby);
    EffectSync::on_spawned(moby);

    if (updating_enemy_ != nullptr && updating_enemy_->reacting) {
        EffectSync::mute(moby);
    }
}
