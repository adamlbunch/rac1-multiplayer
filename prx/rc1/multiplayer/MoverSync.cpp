//
// See MoverSync.h
//

#include "MoverSync.h"

#include <rc1/Game.h>

#include "EnemySync.h"
#include "LevelMobys.h"
#include "Mirror.h"
#include "MobyAnimation.h"
#include "SpawnedMobys.h"
#include "network/GameClient.h"

#define MAX_MOVERS 250
#define MAX_LEVEL_MOBYS 4096
#define MAX_SENDERS 4

// The most of a mover's variables we keep in step, and of those that change while it runs
#define VARS_MAX 0x180
#define MAX_WORDS 48

// How often the game whose player is nearest sends a mover: often while another player is near enough to see it
//  well, now and then otherwise so it's in step by the time they get there
#define SEND_INTERVAL_NEAR 15
#define SEND_INTERVAL_FAR 60
#define NEAR_RADIUS 150.0f

// A game that's this much nearer to a mover than the one sending it takes it over
#define TIE_DISTANCE 2.0f

// A game that stopped sending a mover for this many ticks has left it to us
#define SENDER_TIMEOUT 120

// We show another game's movers this many frames behind the newest state we have from it, like its enemies (see
//  EnemySync)
#define MOVER_DELAY 3

#define MAX_OUTGOING 32

// A mover only takes on another game's state once it has run this many frames here, so it has set itself up
#define SETTLE_FRAMES 30

// Of the variables that didn't change since we first saw a mover, this many go with each state, in turn, so ones it
//  changed before that get there too
#define ROTATING_WORDS 8

// Things that change their state take on another game's once they've been in their own this many frames, and not
//  while our player is this near
#define STABLE_FRAMES 30
#define INTERACT_RADIUS 8.0f

// Things that change their state send it right away when it changes, and every so often otherwise
#define STATE_SEND_INTERVAL 60

// Bolts are each player's own
#define BOLT_O_CLASS 13
#define BIG_BOLT_O_CLASS 14

// Our player's memory: a moby whose variables point here (or at our player's moby) is doing something with our
//  player, like a bolt flying at them, and only makes sense in this game
#define PLAYER_DATA_START 0x969ce0
#define PLAYER_DATA_END 0x96d000

// The level's memory, which the game allocates from the bottom up: start, next free byte, end
#define LEVEL_HEAP (*(u32**)0xa1091c)

struct Mover {
    bool used;
    u16 index;
    u16 uid;
    u16 o_class;

    // Its variables: how many we keep in step, what they were when we first saw it, and which of those changed since
    u16 vars_size;
    u8 baseline[VARS_MAX];
    u32 changed[VARS_MAX / 4 / 32];

    // The game sending it, how far its player was from it, and when we last heard
    u16 sender;
    float sender_distance;
    u32 received_tick;

    // Frames it ran here, up to SETTLE_FRAMES, and which of its variables we send in turn next
    u8 settled;
    u16 next_word;

    // The state we last sent, and how long it's been in its state here
    s16 sent_state;
    s8 local_state;
    u8 state_frames;

    // A state of it from that game, to put it in at that game's frame we show
    bool pending;
    u32 frame;
    MPEnemyState state;
    u16 n_words;
    MPMoverWord words[MAX_WORDS];

    MirrorAnimation animation;
};

struct MoverSender {
    bool active;
    u16 uuid;
    u32 newest_frame;
    u32 playback_frame;
    u32 last_packet_tick;
};

struct OutgoingMover {
    MPMoverState head;
    MPMoverWord words[MAX_WORDS];
};

static Mover movers_[MAX_MOVERS];
static u8 slots_[MAX_LEVEL_MOBYS];    // Per moby placed in the level: its movers_ slot plus one
static MoverSender senders_[MAX_SENDERS];
static OutgoingMover outgoing_[MAX_OUTGOING];
static int n_outgoing_ = 0;
static u32 ticks_ = 0;

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

// A value that's an address in this game's memory: it means the same in every game, but its value is different
static bool looks_like_pointer(u32 value) {
    if (value >= 0x10000 && value < 0x1800000) {
        return true;
    }

    u32* heap = LEVEL_HEAP;
    return heap != nullptr && value >= heap[0] && value < heap[2] + 0x100000;
}

// Whether its variables point at our player: what it's doing then is about this game only
static bool involves_player(Moby* moby, u16 vars_size) {
    u8* vars = (u8*)moby->vars;
    for (u16 offset = 0; vars != nullptr && offset + 4 <= vars_size; offset += 4) {
        u32 value;
        memcpy(&value, vars + offset, 4);
        if (value == (u32)ratchet_moby || (value >= PLAYER_DATA_START && value < PLAYER_DATA_END)) {
            return true;
        }
    }

    return false;
}

static bool syncable_class(Moby* moby) {
    return moby->o_class != BOLT_O_CLASS && moby->o_class != BIG_BOLT_O_CLASS;
}

// Drawn, unlike the level's invisible triggers and managers
static bool drawn(Moby* moby) {
    MobyClass* model = moby->p_class;
    return model != nullptr && (model->pakcet_cnt_0 != 0 || model->packet_cnt_1 != 0);
}

// How much of the memory at `vars` is the moby's own: up to the next moby's variables
static u16 vars_size_of(Moby* moby) {
    u32 vars = (u32)moby->vars;
    if (vars == 0 || !looks_like_pointer(vars)) {
        return 0;
    }

    u32 next = vars + VARS_MAX;
    for (Moby* other = LEVEL_MOBYS_START; other < moby_ptr_end; other++) {
        u32 other_vars = (u32)other->vars;
        if (other_vars > vars && other_vars < next) {
            next = other_vars;
        }
    }

    return (u16)((next - vars) & ~3);
}

static Mover* find_mover(int index) {
    if (index < 0 || index >= MAX_LEVEL_MOBYS || slots_[index] == 0) {
        return nullptr;
    }

    return &movers_[slots_[index] - 1];
}

static Mover* add_mover(int index, Moby* moby, u16 uid) {
    if (index < 0 || index >= MAX_LEVEL_MOBYS) {
        return nullptr;
    }

    Mover* mover = find_mover(index);
    if (mover != nullptr) {
        return mover;
    }

    for (int i = 0; i < MAX_MOVERS; i++) {
        if (movers_[i].used) {
            continue;
        }

        mover = &movers_[i];
        memset(mover, 0, sizeof(Mover));
        mover->used = true;
        mover->index = index;
        mover->uid = uid;
        mover->o_class = moby->o_class;
        mover->vars_size = vars_size_of(moby);
        mover->sent_state = -1;
        mover->local_state = moby->state;
        if (mover->vars_size > 0) {
            memcpy(mover->baseline, moby->vars, mover->vars_size);
        }
        slots_[index] = i + 1;
        return mover;
    }

    return nullptr;
}

static MoverSender* find_sender(u16 uuid, bool add) {
    for (int i = 0; i < MAX_SENDERS; i++) {
        if (senders_[i].active && senders_[i].uuid == uuid) {
            return &senders_[i];
        }
    }

    if (!add) {
        return nullptr;
    }

    for (int i = 0; i < MAX_SENDERS; i++) {
        if (!senders_[i].active) {
            memset(&senders_[i], 0, sizeof(MoverSender));
            senders_[i].active = true;
            senders_[i].uuid = uuid;
            return &senders_[i];
        }
    }

    return nullptr;
}

// Another game is sending it and its player is nearer than ours, or as near
static bool sent_by_other(Mover* mover, float distance) {
    return mover->sender != 0 && ticks_ - mover->received_tick < SENDER_TIMEOUT &&
           distance >= mover->sender_distance - TIE_DISTANCE;
}

void MoverSync::before_update(Moby* moby, int index) {
    Mover* mover = find_mover(index);
    if (mover == nullptr || moby->o_class != (short)mover->o_class) {
        return;
    }

    if (mover->settled < SETTLE_FRAMES) {
        mover->settled += 1;
        return;
    }

    if (moby->state != mover->local_state) {
        mover->local_state = moby->state;
        mover->state_frames = 0;
    } else if (mover->state_frames < 255) {
        mover->state_frames += 1;
    }

    if (!mover->pending) {
        return;
    }

    // At the frame of the sending game we're showing, so it carries on in step with it
    MoverSender* sender = find_sender(mover->sender, false);
    if (sender != nullptr && sender->playback_frame < mover->frame && mover->frame - sender->playback_frame < 30) {
        return;
    }

    mover->pending = false;

    if (involves_player(moby, mover->vars_size)) {
        return;
    }

    // Where it is, its animation and its variables. Not its own state: its code may need to have set things up
    //  here on its way into it.
    MPEnemyState* state = &mover->state;
    moby->position.x = state->position[0];
    moby->position.y = state->position[1];
    moby->position.z = state->position[2];
    moby->rotation.x = state->rotation[0];
    moby->rotation.y = state->rotation[1];
    moby->rotation.z = state->rotation[2];
    moby->scale = state->scale;
    moby->alpha = state->alpha;

    if ((state->flags & MP_ENEMY_FLAG_STATE) && state->state != 0 && state->state < 0x80 &&
        state->state != (u8)moby->state && mover->state_frames >= STABLE_FRAMES && ratchet_moby != nullptr && drawn(moby)) {
        float dx = ratchet_moby->position.x - moby->position.x;
        float dy = ratchet_moby->position.y - moby->position.y;
        float dz = ratchet_moby->position.z - moby->position.z;
        if (dx * dx + dy * dy + dz * dz > INTERACT_RADIUS * INTERACT_RADIUS) {
            moby->state = state->state;
            mover->local_state = moby->state;
            mover->state_frames = 0;
        }
    }

    MobyAnimation::apply(moby, &state->anim, &mover->animation, false);

    u8* vars = (u8*)moby->vars;
    if (vars == nullptr) {
        return;
    }

    for (int i = 0; i < mover->n_words; i++) {
        MPMoverWord* word = &mover->words[i];
        if (word->offset + 4 > mover->vars_size) {
            continue;
        }

        u32 ours;
        memcpy(&ours, vars + word->offset, 4);
        if (ours != word->value && !looks_like_pointer(ours) && !looks_like_pointer(word->value)) {
            memcpy(vars + word->offset, &word->value, 4);
        }
    }
}

static void queue_state(Mover* mover, Moby* moby, float distance, bool changes_state) {
    if (n_outgoing_ >= MAX_OUTGOING) {
        return;
    }

    OutgoingMover* out = &outgoing_[n_outgoing_];
    MPEnemyState* state = &out->head.moby;
    memset(state, 0, sizeof(MPEnemyState));
    state->uid = mover->uid;
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
    if (changes_state && drawn(moby)) {
        state->flags |= MP_ENEMY_FLAG_STATE;
    }
    MobyAnimation::capture(moby, &state->anim);
    mover->sent_state = moby->state;

    // The variables that changed since we first saw it, and keep changing: where along its path it is, its timers...
    u16 n_words = 0;
    u8* vars = (u8*)moby->vars;
    for (u16 offset = 0; vars != nullptr && offset + 4 <= mover->vars_size; offset += 4) {
        u32 value, baseline;
        memcpy(&value, vars + offset, 4);
        memcpy(&baseline, mover->baseline + offset, 4);

        u32 bit = 1u << ((offset / 4) % 32);
        u32* changed = &mover->changed[offset / 4 / 32];
        if (value != baseline && !looks_like_pointer(value)) {
            *changed |= bit;
        }

        if ((*changed & bit) && n_words < MAX_WORDS) {
            out->words[n_words].offset = offset;
            out->words[n_words].pad = 0;
            out->words[n_words].value = value;
            n_words += 1;
        }
    }

    // And a few of the others in turn
    u16 n_vars = mover->vars_size / 4;
    for (int i = 0; vars != nullptr && i < ROTATING_WORDS && n_vars > 0 && n_words < MAX_WORDS; i++) {
        u16 word = mover->next_word % n_vars;
        mover->next_word = (mover->next_word + 1) % n_vars;
        if (mover->changed[word / 32] & (1u << (word % 32))) {
            continue;
        }

        u32 value;
        memcpy(&value, vars + word * 4, 4);
        if (looks_like_pointer(value)) {
            continue;
        }

        out->words[n_words].offset = word * 4;
        out->words[n_words].pad = 0;
        out->words[n_words].value = value;
        n_words += 1;
    }

    out->head.n_words = n_words;
    out->head.pad = 0;
    n_outgoing_ += 1;
}

void MoverSync::after_update(Moby* moby, int index, float distance, bool moves, bool changes_state) {
    u16 uid, spawn;
    if (moby->state < 0 || !syncable_class(moby) || !SpawnedMobys::identify(moby, &uid, &spawn) || spawn != 0) {
        return;
    }

    // Invisible things that only change their state are the level's triggers and managers: never touched
    if (!moves && !drawn(moby)) {
        return;
    }

    float others_sq = Mirror::nearest_player_distance_sq(&moby->position);
    if (others_sq >= 1e29f) {
        return;
    }

    Mover* mover = find_mover(index);
    if (mover == nullptr) {
        // The nearest player's game sends it, the others wait for it
        if (distance * distance > others_sq) {
            return;
        }

        mover = add_mover(index, moby, uid);
        if (mover == nullptr) {
            return;
        }
    }

    if (sent_by_other(mover, distance)) {
        return;
    }

    u32 frame = EnemySync::frame();
    int interval = others_sq < NEAR_RADIUS * NEAR_RADIUS ? SEND_INTERVAL_NEAR : SEND_INTERVAL_FAR;
    bool due = (frame + index) % interval == 0 ||
               (changes_state && (moby->state != mover->sent_state || (frame + index) % STATE_SEND_INTERVAL == 0));
    if (due && !involves_player(moby, mover->vars_size)) {
        queue_state(mover, moby, distance, changes_state);
    }
}

void MoverSync::send(u32 frame) {
    Client* client = Game::shared().connected_client();
    if (client == nullptr || n_outgoing_ == 0) {
        n_outgoing_ = 0;
        return;
    }

    const size_t max_body = sizeof(((Packet*)0)->payload_buffer) - sizeof(MPPacketHeader);

    int sent = 0;
    while (sent < n_outgoing_) {
        size_t len = sizeof(MPPacketMovers);
        int count = 0;
        while (sent + count < n_outgoing_) {
            size_t size = sizeof(MPMoverState) + outgoing_[sent + count].head.n_words * sizeof(MPMoverWord);
            if (len + size > max_body) {
                break;
            }

            len += size;
            count += 1;
        }

        // One too big for a packet on its own: as many of its variables as fit
        if (count == 0) {
            OutgoingMover* big = &outgoing_[sent];
            big->head.n_words = (max_body - len - sizeof(MPMoverState)) / sizeof(MPMoverWord);
            len += sizeof(MPMoverState) + big->head.n_words * sizeof(MPMoverWord);
            count = 1;
        }

        Packet packet = Packet(len);
        packet.header->type = MP_PACKET_MOVERS;
        packet.retain_after_send = true;

        MPPacketMovers* body = (MPPacketMovers*)packet.body;
        body->n_movers = count;
        body->frame = frame;

        u8* data = (u8*)body + sizeof(MPPacketMovers);
        for (int i = sent; i < sent + count; i++) {
            size_t size = sizeof(MPMoverState) + outgoing_[i].head.n_words * sizeof(MPMoverWord);
            memcpy(data, &outgoing_[i], size);
            data += size;
        }

        client->send(&packet);
        sent += count;
    }

    n_outgoing_ = 0;
}

void MoverSync::receive(MPPacketMovers* packet, size_t len) {
    if (packet->sender == 0 || game_state != PlayerControl || ratchet_moby == nullptr) {
        return;
    }

    MoverSender* sender = find_sender(packet->sender, true);
    if (sender == nullptr) {
        return;
    }

    if (packet->frame > sender->newest_frame) {
        if (sender->newest_frame == 0) {
            sender->playback_frame = packet->frame - MOVER_DELAY;
        }
        sender->newest_frame = packet->frame;
    }
    sender->last_packet_tick = ticks_;

    u8* data = (u8*)packet + sizeof(MPPacketMovers);
    u8* end = (u8*)packet + len;
    for (int i = 0; i < packet->n_movers; i++) {
        if (data + sizeof(MPMoverState) > end) {
            return;
        }

        MPMoverState* head = (MPMoverState*)data;
        size_t size = sizeof(MPMoverState) + head->n_words * sizeof(MPMoverWord);
        if (data + size > end || head->n_words > MAX_WORDS) {
            return;
        }
        data += size;

        Moby* moby = LevelMobys::find(head->moby.uid, head->moby.o_class);
        if (moby == nullptr || moby->state < 0 || !syncable_class(moby)) {
            continue;
        }

        int index = moby - LEVEL_MOBYS_START;
        Mover* mover = add_mover(index, moby, head->moby.uid);
        if (mover == nullptr) {
            continue;
        }

        // Our player is clearly nearer: ours to send, and theirs to take once they see it
        float dx = ratchet_moby->position.x - moby->position.x;
        float dy = ratchet_moby->position.y - moby->position.y;
        float dz = ratchet_moby->position.z - moby->position.z;
        if (square_root(dx * dx + dy * dy + dz * dz) < head->moby.distance - TIE_DISTANCE) {
            continue;
        }

        if (mover->pending && mover->sender == packet->sender && mover->frame > packet->frame) {
            continue;
        }

        mover->sender = packet->sender;
        mover->sender_distance = head->moby.distance;
        mover->received_tick = ticks_;
        mover->pending = true;
        mover->frame = packet->frame;
        memcpy(&mover->state, &head->moby, sizeof(MPEnemyState));
        mover->n_words = head->n_words;
        memcpy(mover->words, (u8*)head + sizeof(MPMoverState), head->n_words * sizeof(MPMoverWord));
    }
}

void MoverSync::on_tick() {
    ticks_ += 1;

    // Play back one frame of each other game per frame, a few frames behind its newest, catching up if we fall behind
    for (int i = 0; i < MAX_SENDERS; i++) {
        MoverSender* sender = &senders_[i];
        if (!sender->active) {
            continue;
        }

        if (ticks_ - sender->last_packet_tick > SENDER_TIMEOUT) {
            sender->active = false;
            continue;
        }

        sender->playback_frame += 1;
        if (sender->newest_frame > sender->playback_frame + MOVER_DELAY + 4) {
            sender->playback_frame = sender->newest_frame - MOVER_DELAY;
        }
        if (sender->playback_frame > sender->newest_frame) {
            sender->playback_frame = sender->newest_frame;
        }
    }
}

void MoverSync::reset() {
    memset(movers_, 0, sizeof(movers_));
    memset(slots_, 0, sizeof(slots_));
    memset(senders_, 0, sizeof(senders_));
    n_outgoing_ = 0;
}
