//
// See CutsceneSync.h
//

#include "CutsceneSync.h"

#include <rc1/Game.h>
#include <rc1/rc1.h>

// Which of the level's cutscenes plays: the number start_in_level_movie (0x167e60) got
#define LEVEL_CUTSCENE (*(s32*)0x95cd80)

// Another player's skip still skips our cutscene if it starts this long after the skip got here
#define LATE_START_MS 10000

// The last skip another player sent, until it's used or too old
static bool skip_pending_;
static MPPacketCutsceneSkipped skip_;
static int64_t skip_time_;

// The movie that plays, the button word it checks for Start (null while none plays), and whether it was skipped
static u32 movie_;
static u32* movie_buttons_;
static bool movie_skipped_;

static void send(u8 kind, u32 id) {
    Client* client = Game::shared().connected_client();
    if (client == nullptr) {
        return;
    }

    Packet* packet = new Packet(sizeof(MPPacketCutsceneSkipped));
    packet->header->type = MP_PACKET_CUTSCENE_SKIPPED;

    MPPacketCutsceneSkipped* body = (MPPacketCutsceneSkipped*)packet->body;
    body->kind = kind;
    body->planet = (u8)current_planet;
    body->id = id;

    Logger::info("CutsceneSync: we skipped %s %08x on planet %d", kind == MP_CUTSCENE_MOVIE ? "movie" : "cutscene",
                 id, current_planet);

    // Right away: during a movie the game doesn't get to the end of its frame, where the client sends what it has
    client->send_ack(packet);
    client->flush();
}

// Whether another player skipped this one: the skip is used up then
static bool skipped_by_other(u8 kind, u32 id) {
    if (!skip_pending_) {
        return false;
    }

    if (get_time() - skip_time_ > LATE_START_MS) {
        skip_pending_ = false;
        return false;
    }

    // The level's cutscenes are numbered per level, movies by their file
    if (skip_.kind != kind || skip_.id != id || (kind == MP_CUTSCENE_LEVEL && skip_.planet != current_planet)) {
        return false;
    }

    Logger::info("CutsceneSync: skipping %s %08x, another player skipped it", kind == MP_CUTSCENE_MOVIE ? "movie" :
                 "cutscene", id);
    skip_pending_ = false;
    return true;
}

bool CutsceneSync::should_end_cutscene() {
    if (game_state != CutScene) {
        return false;
    }

    if (skipped_by_other(MP_CUTSCENE_LEVEL, LEVEL_CUTSCENE)) {
        return true;
    }

    // The update ends it on Start before anything else
    if (pressed_buttons & Start) {
        send(MP_CUTSCENE_LEVEL, LEVEL_CUTSCENE);
    }

    return false;
}

void CutsceneSync::on_movie_opened(const char* path) {
    // By the file's name only: the folder it's in differs between copies of the game
    const char* name = path;
    for (const char* c = path; *c; c++) {
        if (*c == '/') {
            name = c + 1;
        }
    }

    // FNV-1a
    u32 hash = 0x811c9dc5;
    for (const char* c = name; *c; c++) {
        hash = (hash ^ (u8)*c) * 0x01000193;
    }

    movie_ = hash;
}

void CutsceneSync::on_movie_playing(u32* skip_buttons) {
    movie_buttons_ = skip_buttons;
    movie_skipped_ = false;
}

void CutsceneSync::after_pads_read() {
    if (movie_buttons_ == nullptr || movie_skipped_) {
        return;
    }

    if (*movie_buttons_ & Start) {
        movie_skipped_ = true;
        send(MP_CUTSCENE_MOVIE, movie_);
    } else if (skipped_by_other(MP_CUTSCENE_MOVIE, movie_)) {
        movie_skipped_ = true;
        *movie_buttons_ |= Start;
    }
}

void CutsceneSync::receive(MPPacketCutsceneSkipped* packet, size_t len) {
    // Not only from players our game has: a cutscene can start before their player is there
    if (len < sizeof(MPPacketCutsceneSkipped) || packet->kind > MP_CUTSCENE_MOVIE) {
        return;
    }

    skip_ = *packet;
    skip_time_ = get_time();
    skip_pending_ = true;
}

extern "C" bool _c_should_end_cutscene() {
    return CutsceneSync::should_end_cutscene();
}

extern "C" void _c_on_movie_opened(const char* path) {
    CutsceneSync::on_movie_opened(path);
}

extern "C" void _c_on_movie_playing(u32* skip_buttons) {
    CutsceneSync::on_movie_playing(skip_buttons);
}

extern "C" void _c_after_pads_read() {
    CutsceneSync::after_pads_read();
}
