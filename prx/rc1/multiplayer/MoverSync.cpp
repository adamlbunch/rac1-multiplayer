//
// See MoverSync.h
//

#include "MoverSync.h"

#include <rc1/Game.h>

#include "EnemySync.h"
#include "LevelMobys.h"
#include "Mirror.h"
#include "MobyAnimation.h"
#include "SoundSync.h"
#include "SpawnedMobys.h"
#include "network/GameClient.h"

#define MAX_MOVERS 250
#define MAX_LEVEL_MOBYS 4096
#define MAX_SENDERS 4

// The most of a mover's variables we keep in step, and of those that change while it runs
#define VARS_MAX 0x180    // MP_MOVER_POINTER_MASKS has a bit for each of these

// No moby's variables are longer than this: a longer gap to the next ones means other memory comes in between
#define LONGEST_VARS 0x400
#define MAX_WORDS 48

// How often the game whose player is nearest sends a mover: often while another player is near enough to see it
//  well, less the further they are, and now and then otherwise so it's in step by the time they get there. Kerwan has
//  over 200 movers in its traffic; sent 4 times a second each, they took 450 KB/s to the other player, more than their
//  game took in, and the gold bolts, items and unlocks behind them in the server's queue arrived up to a minute late.
#define SEND_INTERVAL_NEAR 15
#define SEND_INTERVAL_MID 30
#define SEND_INTERVAL_FAR 60
#define SEND_INTERVAL_AWAY 120
#define CLOSE_RADIUS 40.0f
#define MID_RADIUS 90.0f
#define NEAR_RADIUS 150.0f

// Players about this near to a mover are as near, and the games' ranks settle which one sends it (see EnemySync).
//  Both games used to leave it to the other then, each took the other's state, and they swapped it back and forth
//  every SENDER_TIMEOUT for as long as the players stood together.
#define TIE_DISTANCE 2.0f

// A game that stopped sending a mover for this many ticks has left it to us
#define SENDER_TIMEOUT 120

// We show another game's movers this many frames behind where that game is, like its enemies (see EnemySync)
#define MOVER_DELAY 3

// A game whose states come this many frames behind where we counted it to be stood still meanwhile: we go by its
//  frames again
#define SENDER_BEHIND 10

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

// Things that change their state send it right away when it changes, and every so often otherwise. Things that cycle
//  through their states by themselves, only while another player is close, and not more often than this.
#define STATE_SEND_INTERVAL 60
#define CYCLER_SEND_GAP 10

// A thing that changes its state by itself more often than this sends no more changes for a while: it's blinking
//  or cycling, and the states sent now and then keep it in step
#define TRANSITION_WINDOW 120
#define MAX_WINDOW_TRANSITIONS 3
#define CYCLER_TICKS 600

// After a change of state from another game, or one of our own, states sent now and then don't change it for this
//  long: they may have been sent before it
#define TRANSITION_HOLD 60

// A change from another game that can't be made yet (it's changing on its own here) waits this many frames at most
#define TRANSITION_WAIT 60

#define MAX_OUTGOING_TRANSITIONS 16

// A change of state goes out this many frames after it happened, unless our game left normal play meanwhile: then it
//  was a character starting to talk, a cutscene starting, a vendor opening, and those are about our player only. The
//  states sent now and then don't carry it before then either: Eudora's Fred, in his talking state in the other game,
//  started talking to the player there too.
#define TRANSITION_DELAY 10

// Bolts are each player's own: classes 13 to 16, worth 1, 5, 20 and 50
#define FIRST_BOLT_O_CLASS 13
#define LAST_BOLT_O_CLASS 16

// Gold bolts are shared by the mod's own messages (see GoldBolt). Their collected state flies them into the camera of
//  the game whose player took them: put in it in another game, they crashed there (Batalia).
#define GOLD_BOLT_O_CLASS 1134

// Our player's memory: a moby whose variables point here (or at our player's moby) is doing something with our
//  player. Where they are and which way they face are known in other games, as our copy of them there.
#define PLAYER_DATA_START 0x969ce0
#define PLAYER_DATA_END 0x96d000
#define PLAYER_POSITION 0x969d60
#define PLAYER_ROTATION 0x969d70

// Other players' copies run this update (see MPMoby)
#define MP_MOBY_UPDATE_FUNC ((void*)0x710ed8)

// The level's memory, which the game allocates from the bottom up: start, next free byte, end. And the game's main
//  memory it's in, with the level's gameplay data the game loads at the top.
#define LEVEL_HEAP (*(u32**)0xa1091c)
#define MAIN_HEAP (*(u32**)0xa15f4c)
#define GAMEPLAY_DATA (*(u32*)0xa108f4)

struct Mover {
    bool used;
    u16 index;
    u16 uid;
    u16 o_class;

    // Its variables: how many we keep in step, what they were when we first saw it, and which of those changed since
    u16 vars_size;
    u8 baseline[VARS_MAX];
    u32 changed[VARS_MAX / 4 / 32];

    // Its variables that hold one of our sound channels (the one of a sound it loops): never sent or taken on
    u32 channels[VARS_MAX / 4 / 32];

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

    // Changes of its state by itself here: how many lately, and until when it's left to the states sent now and then
    u32 window_tick;
    u8 window_transitions;
    u32 cycler_until;
    u32 transition_tick;    // When it last changed state by itself here, or from another game's change

    // Whether we said it isn't set up for a state another game put it in (said once)
    bool told_not_set_up;

    // Our game left normal play right after it changed state (see TRANSITION_DELAY): it's someone who talks to our
    //  player, or something else about them. Never sent.
    bool talks;

    // Its variables when its own update was done, to see which something else changes before it runs again: what
    //  another moby keeps there (a leader its group follows, a list it's on, the traffic it's part of) is about that
    //  moby in this game, and never taken from another game or sent
    u8 after_update[VARS_MAX];
    bool after_update_valid;
    u32 external[VARS_MAX / 4 / 32];

    // A change of its state another game made, waiting to be made here
    bool pending_transition;
    u16 transition_sender;
    u8 transition_wait;
    MPEnemyState transition;
    u32 transition_pointers[MP_MOVER_POINTER_MASKS];
    u16 transition_n_words;
    MPMoverWord transition_words[MAX_WORDS];

    // A state of it from that game, to put it in at that game's frame we show
    bool pending;
    u32 frame;
    MPEnemyState state;
    u32 pointers[MP_MOVER_POINTER_MASKS];
    u16 n_words;
    MPMoverWord words[MAX_WORDS];

    MirrorAnimation animation;

    u32 sent_tick;          // When we last sent its state

    u32 sender_rank;        // Of the game sending it, see TIE_DISTANCE
};

struct MoverSender {
    bool active;
    u16 uuid;
    u32 newest_frame;       // Where its game is now, as far as we can tell: its newest frame, counted on since
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
static OutgoingMover outgoing_transitions_[MAX_OUTGOING_TRANSITIONS];
static u32 transition_ticks_[MAX_OUTGOING_TRANSITIONS];
static int transition_movers_[MAX_OUTGOING_TRANSITIONS];    // Their movers_ slots
static int n_outgoing_transitions_ = 0;
static u32 ticks_ = 0;

// Where the test tools that read a running game's memory find our state (see rac1-coop's tools)
struct MoverSyncDebugInfo {
    char magic[8];
    Mover* movers;
    u32 mover_size;
    u32 max_movers;
    u8* slots;
    u32* ticks;
};

MoverSyncDebugInfo mover_sync_debug_info_ = {
    "MSYNC01", movers_, sizeof(Mover), MAX_MOVERS, slots_, &ticks_
};

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

// A value that's an address in the level's memory, or in its gameplay data the game loads at the top of its main
//  memory, which each game sets up for itself as it goes
static bool points_into_level(u32 value) {
    u32* heap = LEVEL_HEAP;
    if (heap != nullptr && value >= heap[0] && value < heap[2] + 0x100000) {
        return true;
    }

    u32* main_heap = MAIN_HEAP;
    u32 gameplay = GAMEPLAY_DATA;
    return gameplay != 0 && main_heap != nullptr && value >= gameplay && value < main_heap[1];
}

// A value we never copy from another game or over ours: an address in this game's memory, or what might be one. That
//  includes values like 0x10000 that are flags of the game's own bookkeeping (a moby's place in a list of the level's
//  collision: taking another game's flags there made one link itself twice, and the game loop forever).
static bool looks_like_pointer(u32 value) {
    return (value >= 0x10000 && value < 0x1800000) || points_into_level(value);
}

// A value we copy from another game and over ours: a number, the way the game keeps timers, counters, speeds and
//  distances (a whole number below 0x10000 either way, or a float between 2^-40 and 2^40), and not an address. Not
//  bytes or halves packed together: those can be the game's bookkeeping too (Veldin 2's class 1356 keeps whether it's
//  on a list in the second byte of 0xe0 in its variables; set from the other game, it unlinked itself from a list it
//  wasn't on there, and that game crashed).
static bool whole_number(u32 value) {
    s32 whole = (s32)value;
    return whole > -0x10000 && whole < 0x10000;
}

static bool copyable(u32 value) {
    if (looks_like_pointer(value)) {
        return false;
    }

    if (whole_number(value)) {
        return true;
    }

    u32 exponent = (value >> 23) & 0xff;
    return exponent >= 127 - 40 && exponent <= 127 + 40;
}

// Of its variables it hasn't changed since we first saw it (how the level set it up), the ones we send anyway. Not
//  whole numbers: those can be indexes into the game's moby table, and each game's table is laid out differently once
//  a respawn has rebuilt it. Aridia's class 668 keeps the index of its manager; taking the other game's, it counted
//  members on a moby without variables and that game crashed. Those only go once its own code changes them.
static bool sent_unchanged(u32 value) {
    return copyable(value) && !whole_number(value);
}

// Some movers' code keeps what it's doing where the words we send don't carry it: in a byte of the moby itself, or a
//  timer that shares its word with other bytes (see whole_number). A state from another game put one where it is
//  there, and its own code carried on from where it was here: Novalis' bobbing platforms jumped to the other game's
//  height, then rose or sank from there on their own schedule. They're known by the code their update runs, which
//  is what decides where it keeps what.
struct ExtraWord {
    u32 update;
    u16 kind;       // MP_MOVER_WORD_MOBY_BYTE, MP_MOVER_WORD_HALF, or MP_MOVER_WORD_VALUE for a word of its variables
                    //  we send every time, not only once it's no longer what the level set it to
    u16 offset;
};

static const ExtraWord extra_words_[] = {
    // Novalis' bobbing platforms (classes 703 and 715): waiting low, waiting high or moving (0xbc), the frames until
    //  the next of those (0xa6), and how far they go each frame, up or down (0xac)
    {0x429290, MP_MOVER_WORD_MOBY_BYTE, 0xbc},
    {0x429290, MP_MOVER_WORD_HALF, 0xa6},
    {0x429290, MP_MOVER_WORD_VALUE, 0xac},
};

#define N_EXTRA_WORDS (sizeof(extra_words_) / sizeof(extra_words_[0]))

// The code its update runs (p_update is the function's descriptor), or 0
static u32 update_code(Moby* moby) {
    u32 descriptor = (u32)moby->p_update;
    if (descriptor < 0x10000 || descriptor >= 0x1800000) {
        return 0;
    }

    return *(u32*)descriptor;
}

static bool is_extra_word(Moby* moby, u16 kind, u16 offset) {
    u32 update = update_code(moby);
    for (u32 i = 0; i < N_EXTRA_WORDS; i++) {
        if (extra_words_[i].update == update && extra_words_[i].kind == kind && extra_words_[i].offset == offset) {
            return true;
        }
    }

    return false;
}

// The kinds of words that are about the sender's player, see player_reference
static bool about_player(u16 kind) {
    return kind == MP_MOVER_WORD_SENDER_PLAYER || kind == MP_MOVER_WORD_SENDER_POSITION ||
           kind == MP_MOVER_WORD_SENDER_ROTATION;
}

// How other games know an address in its variables that's about our player: our player's moby, where they are or
//  which way they face. MP_MOVER_WORD_VALUE if it's not about our player.
static u16 player_reference(u32 value, u32* into) {
    u32 hero = (u32)ratchet_moby;
    if (hero != 0 && value >= hero && value < hero + sizeof(Moby)) {
        *into = value - hero;
        return MP_MOVER_WORD_SENDER_PLAYER;
    }

    *into = 0;
    if (value == PLAYER_POSITION) {
        return MP_MOVER_WORD_SENDER_POSITION;
    }
    if (value == PLAYER_ROTATION) {
        return MP_MOVER_WORD_SENDER_ROTATION;
    }

    return MP_MOVER_WORD_VALUE;
}

// Whether its variables point at something about a player other games can't know: other parts of our player's
//  memory, or our copy of another player. Then what it's doing only makes sense in this game.
static bool involves_player(Moby* moby, u16 vars_size) {
    u8* vars = (u8*)moby->vars;
    for (u16 offset = 0; vars != nullptr && offset + 4 <= vars_size; offset += 4) {
        u32 value, into;
        memcpy(&value, vars + offset, 4);
        if (player_reference(value, &into) != MP_MOVER_WORD_VALUE) {
            continue;
        }

        if (value >= PLAYER_DATA_START && value < PLAYER_DATA_END) {
            return true;
        }

        Moby* start = LEVEL_MOBYS_START;
        if (start != nullptr && value >= (u32)start && value < (u32)moby_ptr_end) {
            Moby* other = start + (value - (u32)start) / sizeof(Moby);
            if (other->p_update == MP_MOBY_UPDATE_FUNC) {
                return true;
            }
        }
    }

    return false;
}

static bool syncable_class(Moby* moby) {
    return (moby->o_class < FIRST_BOLT_O_CLASS || moby->o_class > LAST_BOLT_O_CLASS) &&
           moby->o_class != GOLD_BOLT_O_CLASS;
}

// Drawn, unlike the level's invisible triggers and managers, which have no model at all
static bool drawn(Moby* moby) {
    return moby->p_class != nullptr;
}

// How much of the memory at `vars` is the moby's own: up to the next moby's variables
// How far it is from the moby's variables to the next moby's, or 0 if that's further than any moby's variables are
//  long: then something else comes after them, and their size isn't known from it
static u32 vars_gap(Moby* moby) {
    u32 vars = (u32)moby->vars;
    if (vars == 0 || !looks_like_pointer(vars)) {
        return 0;
    }

    u32 next = vars + LONGEST_VARS + 1;
    for (Moby* other = LEVEL_MOBYS_START; other < moby_ptr_end; other++) {
        u32 other_vars = (u32)other->vars;
        if (other_vars > vars && other_vars < next) {
            next = other_vars;
        }
    }

    u32 gap = (next - vars) & ~3;
    return gap <= LONGEST_VARS ? gap : 0;
}

// How much of the memory at `vars` is the moby's own, as far as we keep in step: up to the next moby's variables.
//  For the last ones before something else, as long as another moby of its class has them. 0 if we can't tell.
static u16 vars_size_of(Moby* moby) {
    u32 gap = vars_gap(moby);
    for (Moby* other = LEVEL_MOBYS_START; gap == 0 && other < moby_ptr_end; other++) {
        if (other != moby && other->o_class == moby->o_class && other->state >= 0) {
            gap = vars_gap(other);
        }
    }

    return (u16)(gap < VARS_MAX ? gap : VARS_MAX);
}

// Notes which of its variables hold the channel of a sound it plays here
static void note_channels(Mover* mover, Moby* moby) {
    u8* vars = (u8*)moby->vars;
    for (u16 offset = 0; vars != nullptr && offset + 4 <= mover->vars_size; offset += 4) {
        u32 value;
        memcpy(&value, vars + offset, 4);
        if (SoundSync::plays_for(value, moby)) {
            mover->channels[offset / 4 / 32] |= 1u << ((offset / 4) % 32);
        }
    }
}

static bool holds_channel(Mover* mover, u16 offset) {
    return (mover->channels[offset / 4 / 32] & (1u << ((offset / 4) % 32))) != 0;
}

// Which of its variables something else writes, see Mover::external
static bool external(Mover* mover, u16 offset) {
    return (mover->external[offset / 4 / 32] & (1u << ((offset / 4) % 32))) != 0;
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

        // The words its code needs every time count as changed from the start
        u32 update = update_code(moby);
        for (u32 e = 0; e < N_EXTRA_WORDS; e++) {
            u16 offset = extra_words_[e].offset;
            if (extra_words_[e].update == update && extra_words_[e].kind == MP_MOVER_WORD_VALUE &&
                offset + 4 <= mover->vars_size) {
                mover->changed[offset / 4 / 32] |= 1u << ((offset / 4) % 32);
            }
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

// Whether a player this far from a mover is nearer to it than another: clearly nearer, or about as near and their
//  game's rank is lower
static bool nearer(float distance, u32 rank, float other_distance, u32 other_rank) {
    if (distance < other_distance - TIE_DISTANCE) {
        return true;
    }

    if (distance > other_distance + TIE_DISTANCE) {
        return false;
    }

    return rank < other_rank;
}

// Another game is sending it and its player is nearer than ours
static bool sent_by_other(Mover* mover, float distance) {
    return mover->sender != 0 && ticks_ - mover->received_tick < SENDER_TIMEOUT &&
           nearer(mover->sender_distance, mover->sender_rank, distance, EnemySync::rank());
}

// Takes on another game's variables, leaving alone ours that point somewhere in our memory or hold a sound channel.
//  Those about the sender's player point at our copy of them. False if we have no copy of them.
static bool apply_words(Mover* mover, Moby* moby, u16 sender, MPMoverWord* words, u16 n_words) {
    u8* vars = (u8*)moby->vars;
    if (vars == nullptr) {
        return true;
    }

    Moby* copy = nullptr;
    for (int i = 0; i < n_words; i++) {
        if (about_player(words[i].kind)) {
            copy = Mirror::hero_of(sender);
            if (copy == nullptr) {
                return false;
            }
            break;
        }
    }

    for (int i = 0; i < n_words; i++) {
        MPMoverWord* word = &words[i];

        // What its code keeps elsewhere, if ours runs the same code
        if (word->kind == MP_MOVER_WORD_MOBY_BYTE || word->kind == MP_MOVER_WORD_HALF) {
            if (!is_extra_word(moby, word->kind, word->offset)) {
                continue;
            }

            if (word->kind == MP_MOVER_WORD_MOBY_BYTE) {
                ((u8*)moby)[word->offset] = (u8)word->value;
            } else if (word->offset + 2 <= mover->vars_size && !holds_channel(mover, word->offset & ~3) &&
                       !external(mover, word->offset & ~3)) {
                u16 half = (u16)word->value;
                memcpy(vars + word->offset, &half, 2);
            }
            continue;
        }

        if (word->offset + 4 > mover->vars_size || holds_channel(mover, word->offset) ||
            external(mover, word->offset)) {
            continue;
        }

        u32 value = word->value;
        if (word->kind != MP_MOVER_WORD_VALUE) {
            if (word->kind == MP_MOVER_WORD_SENDER_PLAYER && word->value < sizeof(Moby)) {
                value = (u32)copy + word->value;
            } else if (word->kind == MP_MOVER_WORD_SENDER_POSITION) {
                value = (u32)&copy->position;
            } else if (word->kind == MP_MOVER_WORD_SENDER_ROTATION) {
                value = (u32)&copy->rotation;
            } else {
                continue;
            }

            memcpy(vars + word->offset, &value, 4);
            continue;
        }

        u32 ours;
        memcpy(&ours, vars + word->offset, 4);
        if (ours != value && copyable(ours) && copyable(value)) {
            memcpy(vars + word->offset, &value, 4);
        }
    }

    return true;
}

// Whether its variables hold an address in the level's memory wherever the sender's do. Its own code sets those up as
//  it goes through its states, so if ours never went through the ones before (it was out of our player's reach), its
//  code in the state the sender is in would follow an address that isn't there. Those about the sender's player count
//  as there: they come with the state.
static bool set_up_like_sender(Mover* mover, Moby* moby, const u32* pointers, MPMoverWord* words, u16 n_words) {
    u8* vars = (u8*)moby->vars;
    for (u16 offset = 0; offset + 4 <= mover->vars_size; offset += 4) {
        u16 word = offset / 4;
        if (!(pointers[word / 32] & (1u << (word % 32))) || external(mover, offset)) {
            continue;
        }

        u32 ours = 0;
        if (vars != nullptr) {
            memcpy(&ours, vars + offset, 4);
        }
        if (points_into_level(ours)) {
            continue;
        }

        bool comes_with_state = false;
        for (int i = 0; i < n_words && !comes_with_state; i++) {
            comes_with_state = words[i].offset == offset && about_player(words[i].kind);
        }
        if (!comes_with_state) {
            return false;
        }
    }

    return true;
}

static void apply_place(Moby* moby, MPEnemyState* state) {
    moby->position.x = state->position[0];
    moby->position.y = state->position[1];
    moby->position.z = state->position[2];
    moby->rotation.x = state->rotation[0];
    moby->rotation.y = state->rotation[1];
    moby->rotation.z = state->rotation[2];
    moby->scale = state->scale;
    moby->alpha = state->alpha;
}

// Makes the change of state another game's copy made by itself, once ours is in the state it changed from, has been
//  in its own a while, or has kept it waiting long enough
static void apply_transition(Mover* mover, Moby* moby) {
    MPEnemyState* transition = &mover->transition;
    if ((s8)transition->state == moby->state) {
        mover->pending_transition = false;
        return;
    }

    mover->transition_wait += 1;
    if (moby->state != (s8)transition->pad[0] && mover->state_frames < STABLE_FRAMES &&
        mover->transition_wait < TRANSITION_WAIT) {
        return;
    }

    mover->pending_transition = false;

    // Ours isn't set up for it
    if (!set_up_like_sender(mover, moby, mover->transition_pointers, mover->transition_words,
                            mover->transition_n_words)) {
        if (!mover->told_not_set_up) {
            mover->told_not_set_up = true;
            Logger::debug("MoverSync: moby %d (o_class %d) isn't set up for state %d here", mover->uid, mover->o_class,
                          transition->state);
        }
        return;
    }

    // About the sender's player, who we have no copy of: not made
    if (!apply_words(mover, moby, mover->transition_sender, mover->transition_words, mover->transition_n_words)) {
        return;
    }

    moby->state = transition->state;
    apply_place(moby, transition);
    MobyAnimation::apply(moby, &transition->anim, &mover->animation, false);

    mover->local_state = moby->state;
    mover->state_frames = 0;
    mover->transition_tick = ticks_;
}

// Which of its variables something else changed since its own update (the sound system clearing the channel of its
//  looping sound aside)
static void note_external(Mover* mover, Moby* moby) {
    u8* vars = (u8*)moby->vars;
    if (!mover->after_update_valid || vars == nullptr) {
        return;
    }

    for (u16 offset = 0; offset + 4 <= mover->vars_size; offset += 4) {
        if (!holds_channel(mover, offset) && memcmp(vars + offset, mover->after_update + offset, 4) != 0) {
            mover->external[offset / 4 / 32] |= 1u << ((offset / 4) % 32);
        }
    }
}

void MoverSync::before_update(Moby* moby, int index) {
    Mover* mover = find_mover(index);
    if (mover == nullptr || moby->o_class != (short)mover->o_class) {
        return;
    }

    // Umbris' swarms crashed following indexes their leader in the other game had given them
    note_external(mover, moby);

    // Its update runs next, and after_update takes them again only in normal play: one from before a cutscene or a
    //  vendor, compared after it, took what its own update did meanwhile for something else's writes (Novalis'
    //  bobbing platforms stopped taking the other game's speed)
    mover->after_update_valid = false;

    note_channels(mover, moby);

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

    if (mover->pending_transition) {
        apply_transition(mover, moby);
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
    MPEnemyState* state = &mover->state;

    // Ours isn't set up for what it's doing there: only where it is
    if (!set_up_like_sender(mover, moby, mover->pointers, mover->words, mover->n_words)) {
        apply_place(moby, state);
        return;
    }

    // Its own state, if it's in another one here, only once it has been in ours a while and our player isn't right
    //  next to it, and not right after a change of state, which may be newer than this. Something that cycles through
    //  its states by itself (blinking, steaming) never stays in one a while: it takes the state, and its timers with
    //  it, whenever one comes, so it cycles in step.
    bool cycling = (s32)(ticks_ - mover->cycler_until) < 0;
    bool take_state = false;
    if ((state->flags & MP_ENEMY_FLAG_STATE) && state->state != 0 && state->state < 0x80 &&
        state->state != (u8)moby->state &&
        (cycling || (mover->state_frames >= STABLE_FRAMES && ticks_ - mover->transition_tick >= TRANSITION_HOLD)) &&
        ratchet_moby != nullptr && drawn(moby)) {
        float dx = ratchet_moby->position.x - moby->position.x;
        float dy = ratchet_moby->position.y - moby->position.y;
        float dz = ratchet_moby->position.z - moby->position.z;
        take_state = dx * dx + dy * dy + dz * dz > INTERACT_RADIUS * INTERACT_RADIUS;
    }

    // Its variables only along with the state they go with: those of another state would tell its code here what
    //  isn't so (that it's on a list it isn't on, say). Those about the sender's player need our copy of them.
    bool same_state = state->state == (u8)moby->state || !(state->flags & MP_ENEMY_FLAG_STATE);
    if ((same_state || take_state) && !apply_words(mover, moby, mover->sender, mover->words, mover->n_words)) {
        return;
    }

    apply_place(moby, state);

    if (take_state) {
        moby->state = state->state;
        mover->local_state = moby->state;
        mover->state_frames = 0;
    }

    MobyAnimation::apply(moby, &state->anim, &mover->animation, false);
}

// What we send of a mover: what it is, where, its state, its animation, and its variables. Those that changed since
//  we first saw it and a few others in turn, or for a change of its state, all of them that fit.
static void fill_state(Mover* mover, Moby* moby, float distance, OutgoingMover* out, bool all_words) {
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
    MobyAnimation::capture(moby, &state->anim);

    note_channels(mover, moby);
    memset(out->head.pointers, 0, sizeof(out->head.pointers));
    u16 n_words = 0;
    u8* vars = (u8*)moby->vars;

    // What its code keeps elsewhere, first, so it's always there
    u32 update = update_code(moby);
    for (u32 i = 0; i < N_EXTRA_WORDS && n_words < MAX_WORDS; i++) {
        const ExtraWord* extra = &extra_words_[i];
        if (extra->update != update) {
            continue;
        }

        u32 value;
        if (extra->kind == MP_MOVER_WORD_MOBY_BYTE) {
            value = ((u8*)moby)[extra->offset];
        } else if (extra->kind == MP_MOVER_WORD_HALF && vars != nullptr && extra->offset + 2 <= mover->vars_size &&
                   !holds_channel(mover, extra->offset & ~3) && !external(mover, extra->offset & ~3)) {
            u16 half;
            memcpy(&half, vars + extra->offset, 2);
            value = half;
        } else {
            continue;
        }

        out->words[n_words].offset = extra->offset;
        out->words[n_words].kind = extra->kind;
        out->words[n_words].value = value;
        n_words += 1;
    }

    for (u16 offset = 0; vars != nullptr && offset + 4 <= mover->vars_size; offset += 4) {
        if (holds_channel(mover, offset) || external(mover, offset)) {
            continue;
        }

        u32 value, baseline;
        memcpy(&value, vars + offset, 4);
        memcpy(&baseline, mover->baseline + offset, 4);

        if (points_into_level(value)) {
            out->head.pointers[offset / 4 / 32] |= 1u << ((offset / 4) % 32);
        }

        // About our player: always, as our player
        u32 into;
        u16 kind = player_reference(value, &into);
        if (kind != MP_MOVER_WORD_VALUE) {
            if (n_words < MAX_WORDS) {
                out->words[n_words].offset = offset;
                out->words[n_words].kind = kind;
                out->words[n_words].value = into;
                n_words += 1;
            }
            continue;
        }

        u32 bit = 1u << ((offset / 4) % 32);
        u32* changed = &mover->changed[offset / 4 / 32];
        if (value != baseline && copyable(value)) {
            *changed |= bit;
        }

        bool send = (*changed & bit) ? copyable(value) : all_words && sent_unchanged(value);
        if (send && n_words < MAX_WORDS) {
            out->words[n_words].offset = offset;
            out->words[n_words].kind = MP_MOVER_WORD_VALUE;
            out->words[n_words].value = value;
            n_words += 1;
        }
    }

    // And a few of the others in turn
    u16 n_vars = mover->vars_size / 4;
    for (int i = 0; !all_words && vars != nullptr && i < ROTATING_WORDS && n_vars > 0 && n_words < MAX_WORDS; i++) {
        u16 word = mover->next_word % n_vars;
        mover->next_word = (mover->next_word + 1) % n_vars;
        if ((mover->changed[word / 32] & (1u << (word % 32))) || holds_channel(mover, word * 4) ||
            external(mover, word * 4)) {
            continue;
        }

        u32 value, into;
        memcpy(&value, vars + word * 4, 4);
        if (!sent_unchanged(value) || player_reference(value, &into) != MP_MOVER_WORD_VALUE) {
            continue;
        }

        out->words[n_words].offset = word * 4;
        out->words[n_words].kind = MP_MOVER_WORD_VALUE;
        out->words[n_words].value = value;
        n_words += 1;
    }

    out->head.n_words = n_words;
    out->head.pad = 0;
}

static void queue_state(Mover* mover, Moby* moby, float distance, bool changes_state) {
    if (n_outgoing_ >= MAX_OUTGOING) {
        return;
    }

    OutgoingMover* out = &outgoing_[n_outgoing_];
    fill_state(mover, moby, distance, out, false);
    if (changes_state && drawn(moby)) {
        out->head.moby.flags |= MP_ENEMY_FLAG_STATE;
    }
    mover->sent_state = moby->state;
    mover->sent_tick = ticks_;
    n_outgoing_ += 1;
}

// It changed its state by itself: to the other games right away, unless it does that all the time
static void queue_transition(Mover* mover, Moby* moby, float distance, s8 state_before) {
    mover->transition_tick = ticks_;

    if (ticks_ - mover->window_tick > TRANSITION_WINDOW) {
        mover->window_tick = ticks_;
        mover->window_transitions = 0;
    }

    // One that keeps cycling stays a cycler, instead of sending a few changes again every CYCLER_TICKS (Kerwan's
    //  traffic flips its state 10 times a second)
    if (mover->window_transitions < 255) {
        mover->window_transitions += 1;
    }
    if (mover->window_transitions > MAX_WINDOW_TRANSITIONS) {
        mover->cycler_until = ticks_ + CYCLER_TICKS;
    }
    if ((s32)(ticks_ - mover->cycler_until) < 0) {
        return;
    }

    if (n_outgoing_transitions_ >= MAX_OUTGOING_TRANSITIONS || involves_player(moby, mover->vars_size)) {
        return;
    }

    OutgoingMover* out = &outgoing_transitions_[n_outgoing_transitions_];
    fill_state(mover, moby, distance, out, true);
    out->head.moby.flags |= MP_ENEMY_FLAG_STATE | MP_ENEMY_FLAG_TRANSITION;
    out->head.moby.pad[0] = (u8)state_before;
    transition_ticks_[n_outgoing_transitions_] = ticks_;
    transition_movers_[n_outgoing_transitions_] = mover - movers_;
    n_outgoing_transitions_ += 1;
}

void MoverSync::after_update(Moby* moby, int index, float distance, bool moves, bool changes_state, s8 state_before) {
    Mover* known = find_mover(index);
    if (known != nullptr && moby->o_class == (short)known->o_class && moby->vars != nullptr) {
        memcpy(known->after_update, moby->vars, known->vars_size);
        known->after_update_valid = true;
    }

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
    if (mover != nullptr && mover->talks) {
        return;
    }

    // Its own change of state, wherever the players are
    if (changes_state && state_before >= 0 && moby->state != state_before && drawn(moby)) {
        if (mover == nullptr) {
            mover = add_mover(index, moby, uid);
        }
        if (mover != nullptr) {
            queue_transition(mover, moby, distance, state_before);
        }
    }

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

    // Not while a change of its own state waits to go out (see TRANSITION_DELAY), unless it cycles
    bool cycling = (s32)(ticks_ - mover->cycler_until) < 0;
    if (changes_state && !cycling && ticks_ - mover->transition_tick < TRANSITION_DELAY) {
        return;
    }

    u32 frame = EnemySync::frame();
    int interval = others_sq < CLOSE_RADIUS * CLOSE_RADIUS ? SEND_INTERVAL_NEAR :
                   others_sq < MID_RADIUS * MID_RADIUS ? SEND_INTERVAL_MID :
                   others_sq < NEAR_RADIUS * NEAR_RADIUS ? SEND_INTERVAL_FAR : SEND_INTERVAL_AWAY;
    bool close = others_sq < CLOSE_RADIUS * CLOSE_RADIUS;
    bool new_state = moby->state != mover->sent_state &&
                     (!cycling || (close && ticks_ - mover->sent_tick >= CYCLER_SEND_GAP));
    bool due = (frame + index) % interval == 0 ||
               (changes_state && (new_state || (frame + index) % STATE_SEND_INTERVAL == 0));
    if (due && !involves_player(moby, mover->vars_size)) {
        queue_state(mover, moby, distance, changes_state);
    }
}

static void send_movers(Client* client, OutgoingMover* outgoing, int n_outgoing, u32 frame, bool reliable) {
    const size_t max_body = sizeof(((Packet*)0)->payload_buffer) - sizeof(MPPacketHeader);

    int sent = 0;
    while (sent < n_outgoing) {
        size_t len = sizeof(MPPacketMovers);
        int count = 0;
        while (sent + count < n_outgoing) {
            size_t size = sizeof(MPMoverState) + outgoing[sent + count].head.n_words * sizeof(MPMoverWord);
            if (len + size > max_body) {
                break;
            }

            len += size;
            count += 1;
        }

        // One too big for a packet on its own: as many of its variables as fit
        if (count == 0) {
            OutgoingMover* big = &outgoing[sent];
            big->head.n_words = (max_body - len - sizeof(MPMoverState)) / sizeof(MPMoverWord);
            len += sizeof(MPMoverState) + big->head.n_words * sizeof(MPMoverWord);
            count = 1;
        }

        // Changes of state are sent until they arrive, so the client keeps those packets until then
        Packet* packet = new Packet(len);
        packet->header->type = MP_PACKET_MOVERS;

        MPPacketMovers* body = (MPPacketMovers*)packet->body;
        body->n_movers = count;
        body->frame = frame;
        body->rank = EnemySync::rank();

        u8* data = (u8*)body + sizeof(MPPacketMovers);
        for (int i = sent; i < sent + count; i++) {
            size_t size = sizeof(MPMoverState) + outgoing[i].head.n_words * sizeof(MPMoverWord);
            memcpy(data, &outgoing[i], size);
            data += size;
        }

        if (reliable) {
            client->send_ack(packet);
        } else {
            client->send(packet);
        }
        sent += count;
    }
}

int MoverSync::remote_indexes(u16* out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_MOVERS && n < max; i++) {
        Mover* mover = &movers_[i];
        if (mover->used && (mover->pending_transition ||
                            (mover->sender != 0 && ticks_ - mover->received_tick < SENDER_TIMEOUT))) {
            out[n++] = mover->index;
        }
    }

    return n;
}

void MoverSync::send(u32 frame) {
    // Out of normal play, what moves is our player's cutscene, and what changed state just before started it
    if (game_state != PlayerControl) {
        if (game_state == Movie || game_state == CutScene || game_state == Gadgetron || game_state == CinematicMaybe) {
            for (int i = 0; i < n_outgoing_transitions_; i++) {
                Mover* mover = &movers_[transition_movers_[i]];
                if (mover->used && mover->uid == outgoing_transitions_[i].head.moby.uid) {
                    mover->talks = true;
                }
            }
        }
        n_outgoing_ = 0;
        n_outgoing_transitions_ = 0;
        return;
    }

    Client* client = Game::shared().connected_client();
    if (client != nullptr) {
        send_movers(client, outgoing_, n_outgoing_, frame, false);
    }
    n_outgoing_ = 0;

    if (client == nullptr) {
        n_outgoing_transitions_ = 0;
        return;
    }

    // The changes of state old enough, keeping the others for later
    int n_due = 0;
    while (n_due < n_outgoing_transitions_ && ticks_ - transition_ticks_[n_due] >= TRANSITION_DELAY) {
        n_due += 1;
    }

    if (n_due == 0) {
        return;
    }

    send_movers(client, outgoing_transitions_, n_due, frame, true);

    for (int i = n_due; i < n_outgoing_transitions_; i++) {
        outgoing_transitions_[i - n_due] = outgoing_transitions_[i];
        transition_ticks_[i - n_due] = transition_ticks_[i];
        transition_movers_[i - n_due] = transition_movers_[i];
    }
    n_outgoing_transitions_ -= n_due;
}

void MoverSync::receive(MPPacketMovers* packet, size_t len) {
    if (packet->sender == 0 || game_state != PlayerControl || ratchet_moby == nullptr) {
        return;
    }

    MoverSender* sender = find_sender(packet->sender, true);
    if (sender == nullptr) {
        return;
    }

    // Further along than we counted (see on_tick), or well behind: it stood still a while (a menu, a cutscene). A
    //  little behind, its game runs a little slower than ours: we count a frame less.
    if (sender->newest_frame == 0) {
        sender->playback_frame = packet->frame - MOVER_DELAY;
    }
    if (packet->frame > sender->newest_frame || packet->frame + SENDER_BEHIND < sender->newest_frame) {
        sender->newest_frame = packet->frame;
    } else if (packet->frame + 1 < sender->newest_frame) {
        sender->newest_frame -= 1;
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

        MPMoverWord* words = (MPMoverWord*)((u8*)head + sizeof(MPMoverState));

        // A change of state it made by itself there: made here too, wherever our player is
        if (head->moby.flags & MP_ENEMY_FLAG_TRANSITION) {
            mover->pending_transition = true;
            mover->transition_sender = packet->sender;
            mover->transition_wait = 0;
            memcpy(&mover->transition, &head->moby, sizeof(MPEnemyState));
            memcpy(mover->transition_pointers, head->pointers, sizeof(mover->transition_pointers));
            mover->transition_n_words = head->n_words;
            memcpy(mover->transition_words, words, head->n_words * sizeof(MPMoverWord));
            continue;
        }

        // Our player is nearer: ours to send, and theirs to take once they see it
        float dx = ratchet_moby->position.x - moby->position.x;
        float dy = ratchet_moby->position.y - moby->position.y;
        float dz = ratchet_moby->position.z - moby->position.z;
        if (nearer(square_root(dx * dx + dy * dy + dz * dz), EnemySync::rank(), head->moby.distance, packet->rank)) {
            continue;
        }

        // A game whose player is nearer still sends it too: this one stops once it hears from that one
        if (mover->sender != 0 && mover->sender != packet->sender && ticks_ - mover->received_tick < SENDER_TIMEOUT &&
            nearer(mover->sender_distance, mover->sender_rank, head->moby.distance, packet->rank)) {
            continue;
        }

        if (mover->pending && mover->sender == packet->sender && mover->frame > packet->frame) {
            continue;
        }

        mover->sender = packet->sender;
        mover->sender_distance = head->moby.distance;
        mover->sender_rank = packet->rank;
        mover->received_tick = ticks_;
        mover->pending = true;
        mover->frame = packet->frame;
        memcpy(&mover->state, &head->moby, sizeof(MPEnemyState));
        memcpy(mover->pointers, head->pointers, sizeof(mover->pointers));
        mover->n_words = head->n_words;
        memcpy(mover->words, words, head->n_words * sizeof(MPMoverWord));
    }
}

void MoverSync::on_tick() {
    ticks_ += 1;

    // Play back one frame of each other game per frame, a few frames behind where it is, catching up if we fall behind.
    //  It sends only on the frames it has a state due, every few frames, so we count its frames on in between. Held at
    //  its newest frame, the playback stood still until the next one came, a state then came in after its mover here
    //  had moved on, and put it back: Novalis' bobbing platforms hitched every few seconds.
    for (int i = 0; i < MAX_SENDERS; i++) {
        MoverSender* sender = &senders_[i];
        if (!sender->active) {
            continue;
        }

        if (ticks_ - sender->last_packet_tick > SENDER_TIMEOUT) {
            sender->active = false;
            continue;
        }

        sender->newest_frame += 1;
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
    n_outgoing_transitions_ = 0;
}
