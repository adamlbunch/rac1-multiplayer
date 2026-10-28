//
// See SoundSync.h
//

#include "SoundSync.h"

#include <rc1/Game.h>

#include "EnemySync.h"
#include "Mirror.h"
#include "SpawnedMobys.h"
#include "SyncedMoby.h"
#include "network/GameClient.h"

// The game's sound channels, 0x70 bytes each: whether it's playing, and which moby and which of its sounds it plays
#define SOUND_CHANNELS 30
#define CHANNEL_STATE(handle) (*(u8*)(0x9637d4 + (handle) * 0x70))
#define CHANNEL_INDEX(handle) (*(s16*)(0x9637de + (handle) * 0x70))
#define CHANNEL_MOBY(handle) (*(Moby**)(0x9637e8 + (handle) * 0x70))

// Gadget slots: the game keeps up to 2 mobys per slot on the player (hand, back, head, feet...), 0x50 bytes apart
#define GADGET_SLOTS ((u8*)0x96ad70)
#define GADGET_SLOT_STRIDE 0x50
#define GADGET_MOBYS 14

// Other players' copies and what they fired run this update (see MPMoby)
#define MP_MOBY_UPDATE_FUNC ((void*)0x710ed8)

#define MAX_OUTGOING 64
#define MAX_QUEUED 256

// Sounds of a frame this far off what we show are played right away, and none waits longer than this many ticks
#define SOUND_FAR_FRAMES 30
#define SOUND_MAX_WAIT 60

extern "C" s32 _c_play_sound(u8 function, s32 index, s32 flags, Moby* moby, u16 sound_class);
extern "C" void _c_stop_sound(s32 handle);

struct QueuedSound {
    bool used;
    u16 sender;
    u32 tick;
    MPSoundEvent event;
};

// What one of our channels plays that we told other games about
struct SentChannel {
    u16 event;
    Moby* moby;
    s16 index;
    u8 flags;   // MP_SOUND_FLAG_ENEMY_FRAME if it's an enemy's
};

// What one of our channels plays for another game
struct ReplayedChannel {
    bool used;
    u16 sender;
    u16 event;
    Moby* moby;
    s16 index;
};

static MPSoundEvent outgoing_[MAX_OUTGOING];
static int n_outgoing_ = 0;
static MPSoundEvent outgoing_stops_[MAX_OUTGOING];
static int n_outgoing_stops_ = 0;
static SentChannel sent_[SOUND_CHANNELS];
static u16 next_event_ = 1;

static QueuedSound queue_[MAX_QUEUED];
static ReplayedChannel replayed_[SOUND_CHANNELS];
static u32 ticks_ = 0;

// Whether the channel still plays what we know it for
static bool channel_plays(s32 handle, Moby* moby, s16 index) {
    return CHANNEL_STATE(handle) != 0 && CHANNEL_MOBY(handle) == moby && CHANNEL_INDEX(handle) == index;
}

// How other games know the moby that made a sound, false if it isn't one of ours they have a copy of
static bool describe(Moby* moby, MPSoundEvent* event) {
    event->frame = Mirror::sent_frame();

    if (moby == EnemySync::own_hero()) {
        event->target = MP_SOUND_TARGET_HERO;
        return true;
    }

    for (int i = 0; i < GADGET_MOBYS; i++) {
        if (*(Moby**)(GADGET_SLOTS + (i / 2) * GADGET_SLOT_STRIDE + (i % 2) * 4) == moby) {
            event->target = MP_SOUND_TARGET_ATTACHMENT;
            event->slot = i;
            return true;
        }
    }

    int spawned_slot = Mirror::spawned_slot(moby);
    if (spawned_slot >= 0) {
        event->target = MP_SOUND_TARGET_ATTACHMENT;
        event->slot = spawned_slot;
        return true;
    }

    u16 uuid = SyncedMoby::uuid_of(moby);
    if (uuid != 0) {
        event->target = MP_SOUND_TARGET_SYNCED;
        event->id = uuid;
        return true;
    }

    u16 uid, spawn;
    if (EnemySync::is_owned(moby) && SpawnedMobys::identify(moby, &uid, &spawn)) {
        event->target = MP_SOUND_TARGET_ENEMY;
        event->id = uid;
        event->spawn = spawn;
        event->flags |= MP_SOUND_FLAG_ENEMY_FRAME;
        event->frame = EnemySync::frame();
        return true;
    }

    return false;
}

bool SoundSync::plays_for(u32 channel, Moby* moby) {
    return channel < SOUND_CHANNELS && CHANNEL_STATE(channel) != 0 && CHANNEL_MOBY(channel) == moby;
}

bool SoundSync::should_play(Moby* moby) {
    if (moby == nullptr || game_state != PlayerControl || Game::shared().connected_client() == nullptr) {
        return true;
    }

    // Other players' copies, and enemies another game runs: their sounds come from those games
    return moby->p_update != MP_MOBY_UPDATE_FUNC && !EnemySync::is_remote(moby) && !EnemySync::is_copy(moby);
}

void SoundSync::on_played(u8 function, s32 index, s32 flags, Moby* moby, u16 sound_class, s32 handle) {
    if (handle < 0 || handle >= SOUND_CHANNELS) {
        return;
    }

    sent_[handle].event = 0;

    if (moby == nullptr || game_state != PlayerControl || n_outgoing_ >= MAX_OUTGOING ||
        Game::shared().connected_client() == nullptr) {
        return;
    }

    MPSoundEvent event;
    memset(&event, 0, sizeof(event));
    if (!describe(moby, &event)) {
        return;
    }

    if (next_event_ == 0) {
        next_event_ = 1;
    }

    event.event = next_event_++;
    event.function = function;
    event.o_class = moby->o_class;
    event.sound_class = sound_class;
    event.index = (s16)index;
    event.sound_flags = flags;
    outgoing_[n_outgoing_++] = event;

    sent_[handle].event = event.event;
    sent_[handle].moby = moby;
    sent_[handle].index = (s16)index;
    sent_[handle].flags = event.flags & MP_SOUND_FLAG_ENEMY_FRAME;
}

void SoundSync::on_stopped(s32 handle) {
    if (handle < 0 || handle >= SOUND_CHANNELS || sent_[handle].event == 0) {
        return;
    }

    SentChannel* sent = &sent_[handle];
    if (channel_plays(handle, sent->moby, sent->index) && n_outgoing_stops_ < MAX_OUTGOING) {
        MPSoundEvent* stop = &outgoing_stops_[n_outgoing_stops_++];
        memset(stop, 0, sizeof(MPSoundEvent));
        stop->flags = MP_SOUND_FLAG_STOP | sent->flags;
        stop->event = sent->event;
        stop->frame = (sent->flags & MP_SOUND_FLAG_ENEMY_FRAME) ? EnemySync::frame() : Mirror::sent_frame();
    }

    sent->event = 0;
}

static void send(Client* client, MPSoundEvent* events, int count, bool reliable) {
    // As many as fit in a packet's own buffer, so nothing is allocated for them
    const size_t max_body = sizeof(((Packet*)0)->payload_buffer) - sizeof(MPPacketHeader);
    const int per_packet = (max_body - sizeof(MPPacketSounds)) / sizeof(MPSoundEvent);

    for (int sent = 0; sent < count; sent += per_packet) {
        int n = count - sent > per_packet ? per_packet : count - sent;

        // A stop that gets lost would leave the sound playing in the other game for good, so those are sent until
        //  they arrive. The client keeps those packets until then, and deletes them after.
        Packet* packet = new Packet(sizeof(MPPacketSounds) + n * sizeof(MPSoundEvent));
        packet->header->type = MP_PACKET_SOUNDS;

        MPPacketSounds* body = (MPPacketSounds*)packet->body;
        body->n_sounds = n;
        memcpy((u8*)body + sizeof(MPPacketSounds), events + sent, n * sizeof(MPSoundEvent));

        if (reliable) {
            client->send_ack(packet);
        } else {
            client->send(packet);
        }
    }
}

void SoundSync::end_frame() {
    Client* client = Game::shared().connected_client();
    if (client != nullptr && ratchet_moby != nullptr &&
        Mirror::nearest_player_distance_sq(&ratchet_moby->position) < 1e29f) {
        send(client, outgoing_, n_outgoing_, false);
        send(client, outgoing_stops_, n_outgoing_stops_, true);
    }

    n_outgoing_ = 0;
    n_outgoing_stops_ = 0;
}

static void play(u16 sender, MPSoundEvent* event);

void SoundSync::receive(MPPacketSounds* packet, size_t len) {
    if (packet->sender == 0 || game_state != PlayerControl ||
        len < sizeof(MPPacketSounds) + packet->n_sounds * sizeof(MPSoundEvent)) {
        return;
    }

    MPSoundEvent* events = (MPSoundEvent*)((u8*)packet + sizeof(MPPacketSounds));
    int next = 0;
    for (int i = 0; i < packet->n_sounds; i++) {
        while (next < MAX_QUEUED && queue_[next].used) {
            next += 1;
        }

        if (next >= MAX_QUEUED) {
            return;
        }

        // What they fired isn't buffered like players and enemies (see SyncedMoby), and may be deleted right after
        if (events[i].target == MP_SOUND_TARGET_SYNCED && !(events[i].flags & MP_SOUND_FLAG_STOP)) {
            play(packet->sender, &events[i]);
            continue;
        }

        QueuedSound* queued = &queue_[next];
        queued->used = true;
        queued->sender = packet->sender;
        queued->tick = ticks_;
        queued->event = events[i];
    }
}

// Our copy of the moby that made it in the sender's game, null if we have none or run that moby ourselves
static Moby* resolve(u16 sender, MPSoundEvent* event) {
    Moby* moby = nullptr;
    switch (event->target) {
        case MP_SOUND_TARGET_HERO:
            moby = Mirror::hero_of(sender);
            break;
        case MP_SOUND_TARGET_ATTACHMENT:
            moby = Mirror::attachment_of(sender, event->slot);
            break;
        case MP_SOUND_TARGET_SYNCED: {
            GameClient* client = (GameClient*)Game::shared().connected_client();
            moby = client != nullptr ? client->moby_for_uuid(event->id) : nullptr;
            break;
        }
        case MP_SOUND_TARGET_ENEMY:
            moby = SpawnedMobys::find(event->id, event->spawn, event->o_class);
            if (moby != nullptr && EnemySync::is_owned(moby)) {
                moby = nullptr;
            }
            break;
    }

    if (moby == nullptr || moby->state < 0 || moby->o_class != (short)event->o_class) {
        return nullptr;
    }

    return moby;
}

static void stop_replayed(int handle) {
    ReplayedChannel* replayed = &replayed_[handle];
    if (replayed->used && channel_plays(handle, replayed->moby, replayed->index)) {
        _c_stop_sound(handle);
    }

    replayed->used = false;
}

static void play(u16 sender, MPSoundEvent* event) {
    if (event->flags & MP_SOUND_FLAG_STOP) {
        for (int handle = 0; handle < SOUND_CHANNELS; handle++) {
            if (replayed_[handle].used && replayed_[handle].sender == sender && replayed_[handle].event == event->event) {
                stop_replayed(handle);
            }
        }
        return;
    }

    Moby* moby = resolve(sender, event);
    if (moby == nullptr) {
        return;
    }

    s32 handle = _c_play_sound(event->function, event->index, event->sound_flags, moby, event->sound_class);
    if (handle < 0 || handle >= SOUND_CHANNELS) {
        return;
    }

    // Our own bookkeeping of the channel is about whatever played on it before
    sent_[handle].event = 0;

    ReplayedChannel* replayed = &replayed_[handle];
    replayed->used = true;
    replayed->sender = sender;
    replayed->event = event->event;
    replayed->moby = moby;
    replayed->index = event->index;
}

void SoundSync::on_tick() {
    ticks_ += 1;

    for (int i = 0; i < MAX_QUEUED; i++) {
        QueuedSound* queued = &queue_[i];
        if (!queued->used) {
            continue;
        }

        if (game_state != PlayerControl) {
            queued->used = false;
            continue;
        }

        // In step with that player, or that game's enemies, as we're showing them
        u32 shown;
        bool showing = (queued->event.flags & MP_SOUND_FLAG_ENEMY_FRAME)
                ? EnemySync::shown_frame(queued->sender, &shown)
                : Mirror::shown_frame(queued->sender, &shown);
        bool due = !showing || shown >= queued->event.frame || queued->event.frame > shown + SOUND_FAR_FRAMES ||
                   ticks_ - queued->tick > SOUND_MAX_WAIT;
        if (!due) {
            continue;
        }

        play(queued->sender, &queued->event);
        queued->used = false;
    }

    // A sound we play for another game whose copy went away stops with it
    for (int handle = 0; handle < SOUND_CHANNELS; handle++) {
        ReplayedChannel* replayed = &replayed_[handle];
        if (!replayed->used) {
            continue;
        }

        if (!channel_plays(handle, replayed->moby, replayed->index)) {
            replayed->used = false;
        } else if (replayed->moby->state < 0) {
            stop_replayed(handle);
        }
    }
}

void SoundSync::reset() {
    memset(sent_, 0, sizeof(sent_));
    memset(queue_, 0, sizeof(queue_));
    memset(replayed_, 0, sizeof(replayed_));
    n_outgoing_ = 0;
    n_outgoing_stops_ = 0;
}

extern "C" bool _c_should_play_sound(Moby* moby) {
    return SoundSync::should_play(moby);
}

extern "C" void _c_on_sound_played(u8 function, s32 index, s32 flags, Moby* moby, u16 sound_class, s32 handle) {
    SoundSync::on_played(function, index, flags, moby, sound_class, handle);
}

extern "C" void _c_on_sound_stopped(s32 handle) {
    SoundSync::on_stopped(handle);
}
