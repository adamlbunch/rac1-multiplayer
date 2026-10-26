//
// See SpawnedMobys.h
//

#include "SpawnedMobys.h"

#include "LevelMobys.h"

#define MAX_SPAWNED 256

// Positions in the moby table we keep track of, for the spawners and the spawned
#define MAX_TABLE 4096

struct SpawnedMoby {
    Moby* moby;
    u16 o_class;
    u16 spawner_uid;
    u16 spawn;      // How many the spawner had spawned before, plus one
    Moby* spawner;
};

static SpawnedMoby spawned_[MAX_SPAWNED];

// By position in our moby table: how many mobys each of the level's mobys spawned so far, and which spawned_ entry
//  (plus one) a spawned moby has
static u16 spawn_counts_[MAX_TABLE];
static u16 entries_[MAX_TABLE];

static int table_index(Moby* moby) {
    int index = moby - LEVEL_MOBYS_START;
    return moby != nullptr && index >= 0 && index < MAX_TABLE ? index : -1;
}

void SpawnedMobys::on_spawned(Moby* moby, Moby* spawner) {
    int index = table_index(moby);
    if (index < 0) {
        return;
    }

    // Its slot may have held a moby we knew before
    if (entries_[index] != 0) {
        spawned_[entries_[index] - 1].moby = nullptr;
        entries_[index] = 0;
    }

    int spawner_index = LevelMobys::index_of(spawner);
    if (spawner_index < 0 || spawner_index >= MAX_TABLE) {
        return;
    }

    spawn_counts_[spawner_index] += 1;

    int slot = -1;
    for (int i = 0; i < MAX_SPAWNED && slot < 0; i++) {
        SpawnedMoby* spawned = &spawned_[i];
        if (spawned->moby == nullptr || spawned->moby->state < 0 || spawned->moby->o_class != (short)spawned->o_class) {
            slot = i;
        }
    }

    if (slot < 0) {
        return;
    }

    SpawnedMoby* spawned = &spawned_[slot];
    if (spawned->moby != nullptr) {
        int old = table_index(spawned->moby);
        if (old >= 0 && entries_[old] == slot + 1) {
            entries_[old] = 0;
        }
    }

    spawned->moby = moby;
    spawned->o_class = moby->o_class;
    spawned->spawner_uid = spawner->uid;
    spawned->spawn = spawn_counts_[spawner_index];
    spawned->spawner = spawner;
    entries_[index] = slot + 1;
}

Moby* SpawnedMobys::spawner_of(Moby* moby) {
    int index = table_index(moby);
    if (index < 0 || entries_[index] == 0) {
        return nullptr;
    }

    SpawnedMoby* spawned = &spawned_[entries_[index] - 1];
    Moby* spawner = spawned->spawner;
    if (spawned->moby != moby || spawner == nullptr || spawner->state < 0 || spawner->uid != spawned->spawner_uid) {
        return nullptr;
    }

    return spawner;
}

void SpawnedMobys::adopt(Moby* moby, u16 spawner_uid, u16 spawn) {
    int index = table_index(moby);
    if (index < 0) {
        return;
    }

    if (entries_[index] != 0) {
        spawned_[entries_[index] - 1].moby = nullptr;
        entries_[index] = 0;
    }

    for (int i = 0; i < MAX_SPAWNED; i++) {
        SpawnedMoby* spawned = &spawned_[i];
        if (spawned->moby == nullptr || spawned->moby->state < 0 || spawned->moby->o_class != (short)spawned->o_class) {
            spawned->moby = moby;
            spawned->o_class = moby->o_class;
            spawned->spawner_uid = spawner_uid;
            spawned->spawn = spawn;
            spawned->spawner = nullptr;
            entries_[index] = i + 1;
            break;
        }
    }

    // Find the spawner by UID: its own spawns count on from here
    for (Moby* spawner = LEVEL_MOBYS_START; spawner < LEVEL_MOBYS_END; spawner++) {
        if (spawner->uid == spawner_uid) {
            int spawner_index = table_index(spawner);
            if (spawner_index >= 0 && spawn_counts_[spawner_index] < spawn) {
                spawn_counts_[spawner_index] = spawn;
            }

            for (int i = 0; i < MAX_SPAWNED; i++) {
                if (entries_[index] == i + 1) {
                    spawned_[i].spawner = spawner;
                }
            }
            break;
        }
    }
}

bool SpawnedMobys::identify(Moby* moby, u16* uid, u16* spawn) {
    if (LevelMobys::index_of(moby) >= 0) {
        *uid = moby->uid;
        *spawn = 0;
        return true;
    }

    int index = table_index(moby);
    if (index < 0 || entries_[index] == 0) {
        return false;
    }

    // Still the moby we saw spawn there, even if it just died
    SpawnedMoby* spawned = &spawned_[entries_[index] - 1];
    if (spawned->moby != moby || moby->o_class != (short)spawned->o_class) {
        return false;
    }

    *uid = spawned->spawner_uid;
    *spawn = spawned->spawn == REVIVED ? 0 : spawned->spawn;
    return true;
}

Moby* SpawnedMobys::find(u16 uid, u16 spawn, u16 o_class) {
    if (spawn == 0) {
        Moby* moby = LevelMobys::find(uid, o_class);
        if (moby != nullptr) {
            return moby;
        }

        // Or our copy of it, if it's gone here and back in another game
        spawn = REVIVED;
    }

    for (int i = 0; i < MAX_SPAWNED; i++) {
        SpawnedMoby* spawned = &spawned_[i];
        if (spawned->moby != nullptr && spawned->spawner_uid == uid && spawned->spawn == spawn &&
            spawned->o_class == o_class && spawned->moby->state >= 0 && spawned->moby->o_class == (short)o_class) {
            return spawned->moby;
        }
    }

    return nullptr;
}

void SpawnedMobys::reset() {
    memset(spawned_, 0, sizeof(spawned_));
    memset(spawn_counts_, 0, sizeof(spawn_counts_));
    memset(entries_, 0, sizeof(entries_));
}
