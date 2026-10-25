//
// See MobyAnimation.h
//

#include "MobyAnimation.h"

MobySeq* MobyAnimation::sequence(Moby* moby, u8 seq) {
    u8* p_class = (u8*)moby->p_class;

    // Sequence count at 0xc, sequence pointers from 0x48
    if (p_class == nullptr || seq >= p_class[0xc]) {
        return nullptr;
    }

    return ((MobySeq**)(p_class + 0x48))[seq];
}

// Animation: blending from frame0 of prev_seq (or a cached pose when it's 0xff) to frame1 of seq, t of the way
void MobyAnimation::capture(Moby* moby, MPMobyAnimation* out) {
    u8* m = (u8*)moby;

    out->frame0 = m[0x50];
    out->frame1 = m[0x51];
    out->prev_seq = m[0x52];
    out->seq = m[0x53];
    memcpy(&out->t, m + 0x54, sizeof(float));
    memcpy(&out->speed, m + 0x58, sizeof(float));
    memcpy(&out->rate, m + 0x5c, sizeof(float));
    out->flags = m[0x70];
}

void MobyAnimation::apply(Moby* moby, const MPMobyAnimation* state, MirrorAnimation* applied, bool track_restarts) {
    u8* m = (u8*)moby;

    MobySeq* seq = sequence(moby, state->seq);
    if (seq == nullptr || state->frame1 >= seq->frame_count) {
        return;
    }

    bool started = !applied->valid || applied->seq != state->seq ||
                   (track_restarts && applied->restarts != state->restarts);

    // The sender is blending into this animation from the pose it was in, do the same from ours
    if (started && state->prev_seq == 0xff) {
        set_moby_animation(moby, state->seq, state->frame1, state->blend_frames);
    }

    applied->valid = true;
    applied->seq = state->seq;
    applied->restarts = state->restarts;

    if (state->prev_seq != 0xff) {
        MobySeq* prev_seq = sequence(moby, state->prev_seq);
        if (prev_seq == nullptr || state->frame0 >= prev_seq->frame_count) {
            return;
        }

        m[0x52] = state->prev_seq;
        m[0x50] = state->frame0;
    } else if (m[0x52] != 0xff) {
        // No blend of our own to continue (it ended, or the game had no room for one), go straight to the target
        m[0x52] = state->seq;
        m[0x50] = state->frame1;
    }

    m[0x53] = state->seq;
    m[0x51] = state->frame1;
    memcpy(m + 0x54, &state->t, sizeof(float));
    memcpy(m + 0x58, &state->speed, sizeof(float));
    memcpy(m + 0x5c, &state->rate, sizeof(float));
    m[0x70] = state->flags;

    // While the sender blends into an animation, its own code moves the blend along bit by bit. The game's
    //  animation step would finish our blend in one go, so keep it off the copy until the sender's blend is done.
    if (state->prev_seq == 0xff && m[0x52] == 0xff) {
        float stopped = 0.0f;
        memcpy(m + 0x5c, &stopped, sizeof(float));
    }

    // Points the moby at the frame data for the frames we just set
    moby_frame_data_something(moby);
}
