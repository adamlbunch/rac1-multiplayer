//
// See GadgetModels.h
//

#include "GadgetModels.h"

#include <rc1/rc1.h>

#include "MPMoby.h"
#include "Mirror.h"


// The game's table of gadget models it swaps in and out: their moby class, where the model sits in memory and its size,
//  and the textures for each (16 per model, -1 for none)
#define GADGET_MODEL_COUNT (*(u32*)0xa4dae4)
#define GADGET_MODEL_O_CLASS ((u32*)0xa4d540)
#define GADGET_MODEL_SOURCE ((u32*)0xa4d5a0)
#define GADGET_MODEL_SIZE ((u32*)0xa4d600)
#define GADGET_MODEL_TEXTURES ((s16*)0xa4d7e0)
#define GADGET_TEXTURE_PAGE (*(u32*)0xa4dae0)

// Moby class -> slot in the class table, and the class table itself
#define CLASS_SLOTS ((u8*)0xa354c0)
#define CLASS_SLOT_COUNT 0x800
#define CLASS_TABLE ((MobyClass**)0xa34c00)

// The level's memory, which the game allocates from the bottom up: start, next free byte, end
#define LEVEL_HEAP (*(u32**)0xa1091c)

// Memory for the models has to be in the part of memory the GPU can read, like the level's. We take it from the top
//  of the level's memory, which is left unused, and only if plenty is left for the game. It's split in slots the size
//  of the biggest gadget model, each holding one model, shared by everyone holding that gadget.
#define SLOT_SIZE 0x20000
#define MIN_SLOTS 2
#define MAX_SLOTS 5
#define HEAP_LEFT_FOR_GAME 0x40000

// A model drawn this recently may still be read by the GPU, so its slot isn't loaded over
#define SLOT_IN_USE_TICKS 3

struct ModelSlot {
    bool loaded;
    u16 o_class;
    u32 last_used;
    u32 texture_page;   // GADGET_TEXTURE_PAGE when it was loaded: its textures are only there while that still is
};

static u8* memory_ = nullptr;
static u32* heap_ = nullptr;        // The level memory we took ours from, its end before and after we did
static u32 heap_end_before_ = 0;
static u32 heap_end_ = 0;
static bool gave_back_ = false;     // The game needed ours on this level
static u32 failed_heap_end_ = 0;    // Level memory that didn't have room for ours
static int n_slots_ = 0;
static ModelSlot slots_[MAX_SLOTS];
static u32 ticks_ = 0;

static int gadget_index(u16 o_class) {
    u32 count = GADGET_MODEL_COUNT;
    if (count > 64) {
        return -1;
    }

    for (u32 i = 0; i < count; i++) {
        if (GADGET_MODEL_O_CLASS[i] == o_class) {
            return i;
        }
    }

    return -1;
}

bool GadgetModels::is_gadget(u16 o_class) {
    return gadget_index(o_class) >= 0;
}

static bool prepare_memory() {
    u32* heap = LEVEL_HEAP;
    if (heap == nullptr) {
        return false;
    }

    // Still where we took it. The game keeps the end of its level memory where we moved it from level to level, so this
    //  holds on every level after the first one (what's in our memory is forgotten for each new level, see reset).
    if (memory_ != nullptr && heap == heap_ && heap[2] == heap_end_ && (u32)memory_ == heap_end_) {
        return true;
    }

    memory_ = nullptr;
    n_slots_ = 0;
    memset(slots_, 0, sizeof(slots_));

    if (gave_back_) {
        return false;
    }

    u32 next_free = heap[1];
    u32 end = heap[2];
    u32 spare = end > next_free ? end - next_free : 0;
    int slots = spare > HEAP_LEFT_FOR_GAME ? (spare - HEAP_LEFT_FOR_GAME) / SLOT_SIZE : 0;
    if (slots > MAX_SLOTS) {
        slots = MAX_SLOTS;
    }

    if (end == failed_heap_end_ || slots < MIN_SLOTS) {
        if (end != failed_heap_end_) {
            Logger::error("GadgetModels: not enough level memory for other players' weapons (0x%08x free)", spare);
        }
        failed_heap_end_ = end;
        return false;
    }

    u32 start = (end - slots * SLOT_SIZE) & ~0x7f;
    heap[2] = start;

    heap_ = heap;
    heap_end_before_ = end;
    heap_end_ = start;
    memory_ = (u8*)start;
    n_slots_ = slots;

    Logger::info("GadgetModels: %d model slots at 0x%08x", slots, start);
    return true;
}

void GadgetModels::on_tick() {
    ticks_ += 1;
}

bool GadgetModels::owns(MobyClass* model) {
    return memory_ != nullptr && (u8*)model >= memory_ && (u8*)model < memory_ + n_slots_ * SLOT_SIZE;
}

bool GadgetModels::give_back(u32* heap) {
    // Only while the game hasn't moved the end of that memory itself: then it's below ours, and ours is still ours
    if (memory_ == nullptr || heap != heap_ || heap != LEVEL_HEAP || heap[2] != heap_end_) {
        return false;
    }

    // Nothing may be drawn with our models once the game writes over them
    Mirror::drop_gadget_models();

    heap[2] = heap_end_before_;
    memory_ = nullptr;
    n_slots_ = 0;
    memset(slots_, 0, sizeof(slots_));
    gave_back_ = true;

    Logger::error("GadgetModels: the game needed the level memory other players' weapons were in, they aren't drawn "
                  "until the level loads again");
    return true;
}

void GadgetModels::reset() {
    gave_back_ = false;

    // Each level puts the gadgets' textures somewhere else. A model loaded for the last level was drawn with textures
    //  that aren't there any more, and RPCS3's renderer crashed on it ("rsx::get_address ... Invalid location": Veldin's
    //  copy of another player's weapon on Novalis, found by the co-op bots). Nothing is drawn with ours now, the
    //  mod's mobys went with the old level, so they're loaded again the next time someone holds one.
    memset(slots_, 0, sizeof(slots_));
}

void GadgetModels::touch(MobyClass* model) {
    if (!owns(model)) {
        return;
    }

    slots_[((u8*)model - memory_) / SLOT_SIZE].last_used = ticks_;
}

MobyClass* GadgetModels::load(u16 o_class) {
    int index = gadget_index(o_class);
    if (index < 0 || GADGET_MODEL_SIZE[index] > SLOT_SIZE || GADGET_MODEL_SOURCE[index] == 0) {
        return nullptr;
    }

    if (!prepare_memory()) {
        return nullptr;
    }

    // Loaded already, maybe for someone else holding the same gadget. One loaded for textures the game has moved since
    //  isn't drawn any more (Mirror drops a copy whose model it doesn't get back), and is loaded again once nothing has
    //  drawn from it for a few frames.
    for (int i = 0; i < n_slots_; i++) {
        if (slots_[i].loaded && slots_[i].o_class == o_class) {
            if (slots_[i].texture_page == GADGET_TEXTURE_PAGE) {
                slots_[i].last_used = ticks_;
                return (MobyClass*)(memory_ + i * SLOT_SIZE);
            }

            slots_[i].loaded = false;
        }
    }

    // Into an empty slot, else the one used longest ago, if nothing has drawn from it for a few frames
    int slot = -1;
    for (int i = 0; i < n_slots_; i++) {
        if (ticks_ - slots_[i].last_used < SLOT_IN_USE_TICKS) {
            continue;
        }

        if (!slots_[i].loaded) {
            slot = i;
            break;
        }

        if (slot < 0 || slots_[i].last_used < slots_[slot].last_used) {
            slot = i;
        }
    }

    if (slot < 0) {
        return nullptr;
    }

    u8* model = memory_ + slot * SLOT_SIZE;

    slots_[slot].loaded = false;
    memcpy(model, (void*)GADGET_MODEL_SOURCE[index], GADGET_MODEL_SIZE[index]);

    // Like the game does when the player switches weapons
    load_moby_model((u32**)model, 0, 0, o_class);

    u8* textures = *(u8**)(model + 0x28);
    if (textures != nullptr) {
        for (int i = 0; i < 16; i++) {
            s16 texture = GADGET_MODEL_TEXTURES[index * 16 + i];
            if (texture >= 0) {
                *(u16*)(textures + i * 0x20 + 0x1a) = texture;
                *(u32*)(textures + i * 0x20 + 0x1c) = GADGET_TEXTURE_PAGE;
            }
        }
    }

    slots_[slot].loaded = true;
    slots_[slot].o_class = o_class;
    slots_[slot].last_used = ticks_;
    slots_[slot].texture_page = GADGET_TEXTURE_PAGE;

    return (MobyClass*)model;
}

extern "C" bool _c_on_game_heap_full(u32* heap, s32 size, u32 caller) {
    Logger::error("Game: no room for %d bytes in its memory at 0x%08x (0x%08x to 0x%08x free), asked from 0x%08x", size,
                  heap, heap[1], heap[2], caller);
    return GadgetModels::give_back(heap);
}

MobyClass* GadgetModels::loaded_class(u16 o_class) {
    // Slot 0xff: the level has no such class
    if (o_class >= CLASS_SLOT_COUNT || CLASS_SLOTS[o_class] == 0xff) {
        return nullptr;
    }

    return CLASS_TABLE[CLASS_SLOTS[o_class]];
}

u16 GadgetModels::o_class_of(MobyClass* model) {
    // Asked every frame for the same few models
    static MobyClass* last_model = nullptr;
    static u16 last_o_class = 0xffff;
    if (model == last_model && last_o_class != 0xffff && loaded_class(last_o_class) == model) {
        return last_o_class;
    }

    for (u16 o_class = 0; o_class < CLASS_SLOT_COUNT; o_class++) {
        if (CLASS_SLOTS[o_class] != 0xff && CLASS_TABLE[CLASS_SLOTS[o_class]] == model) {
            last_model = model;
            last_o_class = o_class;
            return o_class;
        }
    }

    return 0xffff;
}

Moby* GadgetModels::spawn_with_class(u16 o_class, MobyClass* model, u16 flags) {
    if (o_class >= CLASS_SLOT_COUNT || CLASS_SLOTS[o_class] == 0xff || model == nullptr) {
        return nullptr;
    }

    // Spawning sets the moby up from the class table, so have it hold our model for a moment
    u8 slot = CLASS_SLOTS[o_class];
    MobyClass* game_model = CLASS_TABLE[slot];
    CLASS_TABLE[slot] = model;

    Moby* moby = (Moby*)MPMoby::spawn(0, o_class, flags, 0);

    CLASS_TABLE[slot] = game_model;

    return moby;
}
