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
#define MODE_HIDDEN 0x2              // The game leaves it out of its update list
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

// Frames without a state from the game running an enemy before it counts as not running it any more
#define OWNER_TIMEOUT 30

// Frames without a packet from the host before it counts as gone (it left the level, or the game)
#define HOST_TIMEOUT 60

// How long a game that comes to the level listens for a host before it hosts itself: a short while when nobody else
//  is there, longer when someone is (their packets can take a moment to start coming)
#define LISTEN_TICKS 30
#define LISTEN_MAX_TICKS 180

// How long an enemy that came out here waits for the host's states before this game runs it for everyone: the host's
//  game doesn't have it (it's already gone there, or something only our player set off brought it out here). The
//  host only sends one once it has seen it move on its own for a while (MOVING_FRAMES), like we do.
#define HOST_WAIT 120

// How much nearer another player has to be for an enemy to go after them instead of the one it's after
#define TARGET_MARGIN 3.0f

// How far another game's camera can be from its player, see add_remote_enemies
#define CAMERA_MARGIN 24.0f

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
    bool host;              // It said it hosts the level's enemies
    bool away;              // Its player isn't playing right now, see MP_ENEMY_PACKET_AWAY
    s32 player_state;       // Its player's, see MPPacketEnemyState
    s32 player_health;
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
static u8 waits_[MAX_LEVEL_MOBYS];          // Frames it waited for the host's states, up to HOST_WAIT
static u8 from_host_[MAX_LEVEL_MOBYS / 8];  // Bit per moby the host sent us states of since we came to the level
static u16 targets_[MAX_LEVEL_MOBYS];       // The player it goes after: OWNER_SELF, another player's ID, or none

static u32 ticks_ = 0;
static u32 frame_ = 0;
static u32 rank_ = 0;
static u32 loop_tick_ = 0;                  // When our moby update loop last ran

// The game that hosts the level's enemies: OWNER_SELF, another player's ID, or OWNER_NONE while we listen for one
static u16 host_ = OWNER_NONE;
static u32 listen_ticks_ = 0;

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
    u8* traits;             // Per moby placed in the level, by its place in the table
    u16* host;
    u32* rank;
};

EnemySyncDebugInfo enemy_sync_debug_info_ = {
    "ESYNC01", enemies_, sizeof(SyncedEnemy), MAX_ENEMIES, senders_, &ticks_, &sync_off_, unsynced_classes_, traits_,
    &host_, &rank_
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

static bool hosting() {
    return host_ == OWNER_SELF;
}

static bool others_here() {
    return ratchet_moby != nullptr && Mirror::nearest_player_distance_sq(&ratchet_moby->position) < 1e29f;
}

// Another game hosts the enemies, or may: we're still listening for one, and someone else is here
static bool host_elsewhere() {
    return (host_ != OWNER_NONE && host_ != OWNER_SELF) || (host_ == OWNER_NONE && others_here());
}

// The host's player isn't playing right now, so its enemies wait for it here
static bool host_away() {
    EnemySender* host = host_ != OWNER_NONE && host_ != OWNER_SELF ? find_sender(host_) : nullptr;
    return host != nullptr && host->away;
}

static bool playing() {
    return ratchet_moby != nullptr && game_state == PlayerControl;
}

// We stop hosting: the enemies we ran for everyone are the host's now, and wait here for its states. What they spawned
//  stays with us until it's gone, the host's game doesn't have it.
static void leave_enemies_to(u16 host) {
    host_ = host;
    memset(waits_, 0, sizeof(waits_));

    for (int i = 0; i < MAX_ENEMIES; i++) {
        SyncedEnemy* enemy = &enemies_[i];
        if (enemy->used && enemy->owner == OWNER_SELF && !enemy->spawned) {
            remove_enemy(enemy);
        }
    }
}

// Which game hosts the level's enemies. The first one on the level does, for as long as it's there: a game that comes
//  to the level listens for a host first. Should two host at once (they came at the same time, or didn't hear each
//  other for a while), the one with the lower rank keeps hosting.
static void choose_host() {
    EnemySender* host = nullptr;
    for (int i = 0; i < MAX_SENDERS; i++) {
        EnemySender* sender = &senders_[i];
        if (sender->active && sender->host && ticks_ - sender->last_packet_tick <= HOST_TIMEOUT &&
            (host == nullptr || sender->rank < host->rank)) {
            host = sender;
        }
    }

    if (hosting()) {
        if (host != nullptr && host->rank < rank_) {
            Logger::info("EnemySync %u: player %d's game hosts the enemies too, leaving them to it", ticks_,
                         host->uuid);
            leave_enemies_to(host->uuid);
        }
        return;
    }

    if (host != nullptr) {
        if (host_ != host->uuid) {
            Logger::info("EnemySync %u: player %d's game hosts the enemies", ticks_, host->uuid);
            leave_enemies_to(host->uuid);
        }
        listen_ticks_ = 0;
        return;
    }

    if (host_ != OWNER_NONE) {
        Logger::info("EnemySync %u: the host of the enemies is gone, listening for another", ticks_);
        host_ = OWNER_NONE;
        listen_ticks_ = 0;
    }

    listen_ticks_ += 1;
    if (listen_ticks_ >= LISTEN_MAX_TICKS || (listen_ticks_ >= LISTEN_TICKS && !others_here())) {
        Logger::info("EnemySync %u: hosting the enemies", ticks_);
        host_ = OWNER_SELF;
    }
}

// The game's enemies go after the player in the globals the game keeps of them: where they are (and the points a
//  little above that they aim at), which way they face, their moby, their state and health. While the update of an
//  enemy we run for everyone goes after another player, those are that player's.
//
// Those are all in the game's player block, and what an enemy does to the player it goes after lands there too: some
//  push them, knock them into a hurt state or take health off them themselves (the hits WorldSync sends are what
//  reaches the other player). None of that is for our player, so the whole block is as it was after the update.
#define PLAYER_AIM_POINTS ((Vec4*)0x969da0)
#define PLAYER_BLOCK ((u8*)0x969ce0)
#define PLAYER_BLOCK_SIZE 0x2320

struct AimSwap {
    bool active;
    u8 block[PLAYER_BLOCK_SIZE] __attribute__((aligned(16)));
    Moby* hero;
    u16 hero_mode_bits;
};

static AimSwap aim_swap_;

// The player the moby goes after: whoever is nearest, OWNER_SELF for ours. It stays after one until another is
//  clearly nearer.
static u16 choose_target(Moby* moby, u16 current) {
    float dx = ratchet_moby->position.x - moby->position.x;
    float dy = ratchet_moby->position.y - moby->position.y;
    float dz = ratchet_moby->position.z - moby->position.z;
    float ours = square_root(dx * dx + dy * dy + dz * dz);

    u16 best = OWNER_SELF;
    float best_distance = ours;
    float current_distance = current == OWNER_SELF ? ours : 1e30f;

    u16 players[MIRROR_MAX_PLAYERS];
    int n = Mirror::shown_players(players, MIRROR_MAX_PLAYERS);
    for (int i = 0; i < n; i++) {
        Vec4 position, rotation;
        if (Mirror::hero_of(players[i]) == nullptr || !Mirror::player_position(players[i], &position, &rotation)) {
            continue;
        }

        dx = position.x - moby->position.x;
        dy = position.y - moby->position.y;
        dz = position.z - moby->position.z;
        float distance = square_root(dx * dx + dy * dy + dz * dz);

        if (players[i] == current) {
            current_distance = distance;
        }

        if (distance < best_distance) {
            best = players[i];
            best_distance = distance;
        }
    }

    if (current_distance < 1e29f && current_distance < best_distance + TARGET_MARGIN) {
        return current;
    }

    return best;
}

static void aim_at(u16 target) {
    if (target == OWNER_SELF || target == OWNER_NONE) {
        return;
    }

    Moby* hero = Mirror::hero_of(target);
    Vec4 position, rotation;
    if (hero == nullptr || !Mirror::player_position(target, &position, &rotation)) {
        return;
    }

    aim_swap_.active = true;
    memcpy(aim_swap_.block, PLAYER_BLOCK, PLAYER_BLOCK_SIZE);
    aim_swap_.hero = hero;
    aim_swap_.hero_mode_bits = hero->mode_bits;

    // The points it aims at are as far above them as ours are above us
    for (int i = 0; i < 2; i++) {
        PLAYER_AIM_POINTS[i].x = position.x + (PLAYER_AIM_POINTS[i].x - player_pos.x);
        PLAYER_AIM_POINTS[i].y = position.y + (PLAYER_AIM_POINTS[i].y - player_pos.y);
        PLAYER_AIM_POINTS[i].z = position.z + (PLAYER_AIM_POINTS[i].z - player_pos.z);
    }

    player_pos.x = position.x;
    player_pos.y = position.y;
    player_pos.z = position.z;
    player_rot.x = rotation.x;
    player_rot.y = rotation.y;
    player_rot.z = rotation.z;
    ratchet_moby = hero;

    EnemySender* sender = find_sender(target);
    if (sender != nullptr) {
        player_state = sender->player_state;
        player_health = sender->player_health;
    }
}

static void aim_back() {
    if (!aim_swap_.active) {
        return;
    }

    memcpy(PLAYER_BLOCK, aim_swap_.block, PLAYER_BLOCK_SIZE);

    // What it did to them (hid them while it holds them, say) is up to their own game, see WorldSync for its hits
    aim_swap_.hero->mode_bits = aim_swap_.hero_mode_bits;
    aim_swap_.active = false;
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

    // As many enemies per packet as fit in the packet's own buffer, so nothing is allocated for them. One packet even
    //  without any, so the others know we're here, whether we host and what our player is up to.
    const size_t max_body = sizeof(((Packet*)0)->payload_buffer) - sizeof(MPPacketHeader);

    int sent = 0;
    do {
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
        body->sender = 0;
        body->n_enemies = count;
        body->flags = (hosting() ? MP_ENEMY_PACKET_HOST : 0) | (playing() ? 0 : MP_ENEMY_PACKET_AWAY);
        body->frame = frame_;
        body->rank = rank_;
        body->player_state = player_state;
        body->player_health = player_health;

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
    } while (sent < n_outgoing_);

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

// Whether the update of a moby we run goes after whichever player is nearest instead of ours: the enemies we run for
//  everyone and what they spawn, and while we host, the ones that could be enemies we haven't seen do enough yet
//  (so one waiting for a player to come near sees the others coming too)
static bool goes_after_anyone(Moby* moby, SyncedEnemy* enemy, int index) {
    if (enemy != nullptr) {
        return enemy->owner == OWNER_SELF;
    }

    if (hosting() && (traits_[index] & (TRAIT_DAMAGEABLE | TRAIT_ATTACKER)) == (TRAIT_DAMAGEABLE | TRAIT_ATTACKER)) {
        return true;
    }

    Moby* spawner = SpawnedMobys::spawner_of(moby);
    SyncedEnemy* spawner_enemy = spawner != nullptr ? find_enemy(sync_index(spawner)) : nullptr;
    return spawner_enemy != nullptr && spawner_enemy->owner == OWNER_SELF;
}

// The player the update about to run goes after, OWNER_NONE for ours as always
static u16 aim_target_ = OWNER_NONE;

// Before the moby's animation step and update. Returns whether its update should run.
static bool before_update(Moby* moby) {
    updating_ = moby;
    updating_enemy_ = nullptr;
    aim_target_ = OWNER_NONE;
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

    // Another game hosts the enemies: one we know is an enemy waits for its states. If they don't come, the host's
    //  game doesn't have it, and ours runs it for everyone. Not while we don't know the host yet, or its player is
    //  away: then it just waits. So does one the host sent before: the host has it, it's just not running it right
    //  now (it's out of everyone's reach there, or the host's game stalled for a moment).
    if (enemy == nullptr && !hosting() && host_elsewhere() && !(sync_off_ & SYNC_OFF_REMOTE_ENEMIES) &&
        (traits_[updating_index_] & TRAITS_ENEMY) == TRAITS_ENEMY && game_state == PlayerControl) {
        bool sent_before = (from_host_[updating_index_ / 8] & (1 << (updating_index_ % 8))) != 0;
        bool waiting = host_ == OWNER_NONE || host_away() || sent_before;
        if (waits_[updating_index_] < HOST_WAIT || waiting) {
            if (!waiting) {
                waits_[updating_index_] += 1;
            }
            return false;
        }

        enemy = add_enemy(updating_index_, moby);
        if (enemy != nullptr) {
            Logger::debug("EnemySync %u: the host doesn't run moby %d/%d (o_class %d), running it for everyone",
                          ticks_, enemy->uid, enemy->spawn, moby->o_class);
            enemy->owner = OWNER_SELF;
            enemy->animation.valid = false;
        }
    }

    if (others_here() && goes_after_anyone(moby, enemy, updating_index_)) {
        targets_[updating_index_] = choose_target(moby, targets_[updating_index_]);
        aim_target_ = targets_[updating_index_];
    }

    if (enemy == nullptr) {
        if (!(sync_off_ & SYNC_OFF_MOVERS)) {
            MoverSync::before_update(moby, updating_index_);

            // What it does in its own update starts from what another game put it in
            position_before_ = moby->position;
            rotation_before_ = moby->rotation;
            state_before_ = moby->state;
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
                                    (traits_[index] & TRAIT_STATE_CHANGES) != 0, state_before_);
        }
        return;
    }

    float others_sq = Mirror::nearest_player_distance_sq(&moby->position);
    if (others_sq >= 1e29f) {
        return;
    }

    // Another game runs it: the host, or another player's game the host left it to
    if (enemy != nullptr && owner_is_remote(enemy)) {
        return;
    }

    // The host runs every enemy it has for everyone. Other games run only the ones the host doesn't have (see
    //  before_update), which they took on already.
    if (enemy == nullptr || enemy->owner != OWNER_SELF) {
        if (!hosting()) {
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

    send_state(enemy, moby, player_distance(moby), others_sq);
}

// The game only runs the mobys near the camera, and those that always run. The enemies another game runs are kept in
//  step wherever they are: one that wandered off from where it is here, towards our player there, shows up where it
//  is there instead of staying behind out of sight. Their own update doesn't run here anyway. So are the movers
//  another game keeps in step for us (see MoverSync::remote_indexes).
static Moby* add_to_list(Moby* list, Moby** last, u8* listed, int index) {
    if (index < 0 || index >= MAX_LEVEL_MOBYS || (listed[index / 8] & (1 << (index % 8)))) {
        return list;
    }

    Moby* moby = LEVEL_MOBYS_START + index;
    if (moby >= moby_ptr_end || moby->state < 0 || (moby->mode_bits & MODE_HIDDEN)) {
        return list;
    }

    moby->p_chain = nullptr;
    if (*last == nullptr) {
        list = moby;
    } else {
        (*last)->p_chain = moby;
    }
    *last = moby;
    listed[index / 8] |= 1 << (index % 8);
    return list;
}

static Moby* add_remote_enemies(Moby* list) {
    static u8 listed[MAX_LEVEL_MOBYS / 8];
    memset(listed, 0, sizeof(listed));

    Moby* last = nullptr;
    for (Moby* moby = list; moby != nullptr; moby = (Moby*)moby->p_chain) {
        int index = moby - LEVEL_MOBYS_START;
        if (index >= 0 && index < MAX_LEVEL_MOBYS) {
            listed[index / 8] |= 1 << (index % 8);
        }
        last = moby;
    }

    for (int i = 0; i < MAX_ENEMIES; i++) {
        SyncedEnemy* enemy = &enemies_[i];
        if (enemy->used && (owner_is_remote(enemy) || enemy->copy)) {
            list = add_to_list(list, &last, listed, enemy->index);
        }
    }

    if (!(sync_off_ & SYNC_OFF_MOVERS)) {
        static u16 movers[256];
        int n = MoverSync::remote_indexes(movers, 256);
        for (int i = 0; i < n; i++) {
            list = add_to_list(list, &last, listed, movers[i]);
        }
    }

    // While we host, what could be an enemy runs here near the other players too, the way the game runs it near our
    //  camera: within its update distance. Their games wait for our states of it. They go by their camera, which can
    //  be a way off from the player we know the position of, so a bit further.
    if (hosting()) {
        u16 players[MIRROR_MAX_PLAYERS];
        Vec4 positions[MIRROR_MAX_PLAYERS];
        int n = Mirror::shown_players(players, MIRROR_MAX_PLAYERS);
        for (int i = 0; i < n; i++) {
            Vec4 rotation;
            if (!Mirror::player_position(players[i], &positions[i], &rotation)) {
                positions[i].x = 1e15f;
            }
        }

        for (int index = 0; n > 0 && index < MAX_LEVEL_MOBYS && LEVEL_MOBYS_START + index < moby_ptr_end; index++) {
            Moby* moby = LEVEL_MOBYS_START + index;
            if (moby->state < 0 || (listed[index / 8] & (1 << (index % 8))) || !can_attack(moby)) {
                continue;
            }

            float reach = (float)(u8)moby->update_distance + CAMERA_MARGIN;
            for (int i = 0; i < n; i++) {
                float dx = positions[i].x - moby->position.x;
                float dy = positions[i].y - moby->position.y;
                float dz = positions[i].z - moby->position.z;
                if (dx * dx + dy * dy + dz * dz < reach * reach) {
                    list = add_to_list(list, &last, listed, index);
                    break;
                }
            }
        }
    }

    return list;
}

bool EnemySync::update_mobys() {
    if (Game::shared().connected_client() == nullptr || (sync_off_ & SYNC_OFF_UPDATE_LOOP)) {
        return false;
    }

    frame_ += 1;
    loop_tick_ = ticks_;

    if (rank_ == 0) {
        rank_ = (u32)sys_time_get_system_time() | 1;
    }

    EffectSync::begin_frame();

    // Other players' copies take the hits our enemies land on them in a frame (see WorldSync), nothing else does
    u16 players[MIRROR_MAX_PLAYERS];
    int n_players = Mirror::shown_players(players, MIRROR_MAX_PLAYERS);
    for (int i = 0; i < n_players; i++) {
        Moby* hero = Mirror::hero_of(players[i]);
        if (hero != nullptr) {
            hero->damage = -1;
        }
    }

    Moby* moby = add_remote_enemies(build_moby_update_list());
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
            aim_at(aim_target_);
            ((MobyUpdateFunction)moby->p_update)(moby);
            aim_back();
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

Moby* EnemySync::own_hero() {
    return aim_swap_.active ? *(Moby**)(aim_swap_.block + ((u8*)&ratchet_moby - PLAYER_BLOCK)) : ratchet_moby;
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

    Logger::info("EnemySync: %d enemies seen, running %d, %d from other games (%d reacting), other player %s, host %s",
                 seen, ours, theirs, reacting, others_here() ? "here" : "not here",
                 hosting() ? "us" : host_ == OWNER_NONE ? "none yet" : "them");
}

void EnemySync::on_tick(Client* client) {
    ticks_ += 1;

    // Our moby update loop doesn't run in some menus: the others still hear from us, and that we're away
    if (client != nullptr && frame_ != 0 && ticks_ - loop_tick_ > 1) {
        send_states();
    }

    if (client == nullptr || ratchet_moby == nullptr || game_state != PlayerControl) {
        return;
    }

    if (rank_ != 0) {
        choose_host();
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

// Variables for our copies of enemies placed in the level. The game gives a moby it spawns 0x80 bytes of them, but
//  enemies placed in the level can have more (Aridia's sand sharks 0x2e0), so a copy shared the rest with the next
//  mobys spawned, and the game read their variables as the copy's (a sand shark copy's group index pointed at a moby
//  with no variables, and the game crashed).
#define REVIVED_VARS_SIZE 0x400
#define MAX_REVIVED_VARS 16
static u8 revived_vars_[MAX_REVIVED_VARS][REVIVED_VARS_SIZE] __attribute__((aligned(16)));
static Moby* revived_vars_owner_[MAX_REVIVED_VARS];

static u8* take_revived_vars(Moby* moby) {
    for (int i = 0; i < MAX_REVIVED_VARS; i++) {
        Moby* owner = revived_vars_owner_[i];
        if (owner == nullptr || owner->state < 0 || owner->vars != revived_vars_[i]) {
            revived_vars_owner_[i] = moby;
            memset(revived_vars_[i], 0, REVIVED_VARS_SIZE);
            return revived_vars_[i];
        }
    }

    return nullptr;
}

// A copy of a moby another game's enemy spawned, which our copy of that enemy didn't spawn (it doesn't run here). Or
//  of an enemy placed in the level that we destroyed here, which is back in that game because a respawn there set the
//  level up again: we show it from there too, so we both see the same enemies.
static Moby* make_copy(MPEnemyState* state, bool revived) {
    if ((state->spawn == 0) != revived || GadgetModels::loaded_class(state->o_class) == nullptr) {
        return nullptr;
    }

    Moby* moby = spawn_moby(state->o_class);
    if (moby == nullptr) {
        return nullptr;
    }

    EnemySync::on_spawned(moby);
    SpawnedMobys::adopt(moby, state->uid, revived ? SpawnedMobys::REVIVED : state->spawn);

    moby->position.x = state->position[0];
    moby->position.y = state->position[1];
    moby->position.z = state->position[2];
    moby->rotation.x = state->rotation[0];
    moby->rotation.y = state->rotation[1];
    moby->rotation.z = state->rotation[2];
    moby->scale = state->scale;

    // What a spawned enemy fires shouldn't bump into anything here, but an enemy has to be there to be hit
    if (!revived) {
        moby->collision = nullptr;
    }

    // Variables of its own, and no update at all: it never runs here, whatever else happens to it
    if (revived) {
        u8* vars = take_revived_vars(moby);
        if (vars == nullptr) {
            moby->state = -1;
            delete_moby(moby);
            return nullptr;
        }

        moby->vars = vars;
        moby->p_update = nullptr;
    }

    return moby;
}

static void receive_state(EnemySender* sender_info, u32 frame, MPEnemyState* state, MPEnemyMatrix* matrix) {
    u16 sender = sender_info->uuid;

    Moby* moby = SpawnedMobys::find(state->uid, state->spawn, state->o_class);
    int index = sync_index(moby);
    SyncedEnemy* enemy = find_enemy(index);

    // The host's states always count: what we ran ourselves meanwhile (the host's didn't come for a while) is the
    //  host's again. Another game's count only for enemies the host doesn't run, so neither ones we run nor ones
    //  another game (the host) sends us.
    bool from_host = sender_info->host && !hosting() && (host_ == sender || host_ == OWNER_NONE);
    if (!from_host && enemy != nullptr &&
        (enemy->owner == OWNER_SELF || (owner_is_remote(enemy) && enemy->owner != sender))) {
        return;
    }

    bool copy = false;
    if (moby == nullptr) {
        if (state->flags & MP_ENEMY_FLAG_SPAWNED) {
            moby = make_copy(state, false);
        } else if (state->spawn == 0 && LevelMobys::find_destroyed(state->uid, state->o_class) != nullptr) {
            Logger::debug("EnemySync %u: moby %d (o_class %d) is back in player %d's game, showing it here too", ticks_,
                          state->uid, state->o_class, sender);
            moby = make_copy(state, true);
        }
        copy = moby != nullptr;
        index = sync_index(moby);
    }

    if (moby == nullptr || index < 0) {
        return;
    }

    waits_[index] = 0;
    if (from_host) {
        from_host_[index / 8] |= 1 << (index % 8);
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
    }
    enemy->copy = enemy->copy || copy;

    if (enemy->owner != sender) {
        Logger::debug("EnemySync %u: moby %d/%d (o_class %d) is run by player %d", ticks_, state->uid, state->spawn,
                      state->o_class, sender);
        enemy->owner = sender;
        enemy->spawned = (state->flags & MP_ENEMY_FLAG_SPAWNED) != 0;
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
    if (packet->sender == 0 || packet->frame == 0 || len < sizeof(MPPacketEnemyState)) {
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

    // Who's there and who hosts, even while we're in a menu ourselves, so we don't lose track of the host meanwhile
    sender->last_packet_tick = ticks_;
    sender->rank = packet->rank;
    sender->host = (packet->flags & MP_ENEMY_PACKET_HOST) != 0;
    sender->away = (packet->flags & MP_ENEMY_PACKET_AWAY) != 0;
    sender->player_state = packet->player_state;
    sender->player_health = packet->player_health;
    if (packet->frame > sender->newest_frame) {
        sender->newest_frame = packet->frame;
    }

    if (game_state != PlayerControl || ratchet_moby == nullptr) {
        return;
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

bool EnemySync::runs_for_everyone(Moby* moby) {
    if (!plausible_moby(moby)) {
        return false;
    }

    if (is_owned(moby)) {
        return true;
    }

    Moby* spawner = SpawnedMobys::spawner_of(moby);
    return spawner != nullptr && is_owned(spawner);
}

u32 EnemySync::player_rank(Moby* moby) {
    if (!plausible_moby(moby)) {
        return 0;
    }

    for (int i = 0; i < MAX_SENDERS; i++) {
        if (senders_[i].active && Mirror::hero_of(senders_[i].uuid) == moby) {
            return senders_[i].rank;
        }
    }

    return 0;
}

bool EnemySync::is_copy(Moby* moby) {
    SyncedEnemy* enemy = find_enemy(sync_index(moby));
    return enemy != nullptr && enemy->copy;
}

u32 EnemySync::frame() {
    return frame_;
}

u32 EnemySync::rank() {
    return rank_;
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
    waits_[index] = 0;
    targets_[index] = OWNER_NONE;
    from_host_[index / 8] &= ~(1 << (index % 8));
}

void EnemySync::reset() {
    Logger::debug("EnemySync %u: reset", ticks_);

    // The host keeps hosting when its player respawns on the level. Anywhere else, we listen for the host again.
    static int host_planet = -1;
    if (!hosting() || current_planet != host_planet) {
        host_ = OWNER_NONE;
        listen_ticks_ = 0;
    }
    host_planet = current_planet;

    // Our copies are only known while we track them: they'd stay behind as mobys nobody knows, so they go
    for (int i = 0; i < MAX_ENEMIES; i++) {
        SyncedEnemy* enemy = &enemies_[i];
        Moby* moby = LEVEL_MOBYS_START != nullptr ? LEVEL_MOBYS_START + enemy->index : nullptr;
        if (enemy->used && enemy->copy && moby != nullptr && moby < moby_ptr_end && moby->state >= 0 &&
            moby->o_class == (short)enemy->o_class) {
            moby->state = -1;
            delete_moby(moby);
        }
    }
    memset(revived_vars_owner_, 0, sizeof(revived_vars_owner_));

    SpawnedMobys::reset();

    memset(enemies_, 0, sizeof(enemies_));
    memset(senders_, 0, sizeof(senders_));
    memset(slots_, 0, sizeof(slots_));
    memset(traits_, 0, sizeof(traits_));
    memset(restarts_, 0, sizeof(restarts_));
    memset(blend_frames_, 0, sizeof(blend_frames_));
    memset(moving_frames_, 0, sizeof(moving_frames_));
    memset(updates_, 0, sizeof(updates_));
    memset(waits_, 0, sizeof(waits_));
    memset(targets_, 0, sizeof(targets_));
    memset(from_host_, 0, sizeof(from_host_));

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
    Mirror::on_spawned(moby, EffectSync::player_updating(), updating_);

    if (updating_enemy_ != nullptr && updating_enemy_->reacting) {
        EffectSync::mute(moby);
    }
}
