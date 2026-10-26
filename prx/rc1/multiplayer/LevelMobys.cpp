//
// See LevelMobys.h
//

#include "LevelMobys.h"

// UIDs of mobys placed in a level are small. Bigger ones are searched for.
#define MAX_CACHED_UID 4096

// Where each UID was in the table when we last looked
static Moby* by_uid_[MAX_CACHED_UID];
static Moby* cached_start_ = nullptr;
static Moby* cached_end_ = nullptr;

static void refresh_cache() {
    memset(by_uid_, 0, sizeof(by_uid_));

    for (Moby* moby = LEVEL_MOBYS_START; moby < LEVEL_MOBYS_END; moby++) {
        if (moby->uid < MAX_CACHED_UID) {
            by_uid_[moby->uid] = moby;
        }
    }

    cached_start_ = LEVEL_MOBYS_START;
    cached_end_ = LEVEL_MOBYS_END;
}

static Moby* lookup(u16 uid) {
    if (uid >= MAX_CACHED_UID) {
        for (Moby* moby = LEVEL_MOBYS_START; moby < LEVEL_MOBYS_END; moby++) {
            if (moby->uid == uid) {
                return moby;
            }
        }

        return nullptr;
    }

    // The table moved or was rebuilt (new level, respawn) since we last looked
    Moby* moby = by_uid_[uid];
    if (LEVEL_MOBYS_START != cached_start_ || LEVEL_MOBYS_END != cached_end_ ||
        (moby != nullptr && moby->uid != uid)) {
        refresh_cache();
        moby = by_uid_[uid];
    }

    return moby;
}

Moby* LevelMobys::find(u16 uid, u16 o_class) {
    Moby* moby = lookup(uid);

    if (moby == nullptr || moby->uid != uid || moby->state < 0 || moby->o_class != (short)o_class ||
        moby == ratchet_moby || moby->p_update == LEVEL_MOBYS_MP_UPDATE_FUNC) {
        return nullptr;
    }

    return moby;
}
