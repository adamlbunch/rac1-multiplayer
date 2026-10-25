//
// See GadgetModels.h
//

#include "GadgetModels.h"

#include <sys/syscall.h>

#include <rc1/rc1.h>

#include "MPMoby.h"

// sys/memory.h clashes with the game functions rc1.h declares, so call the system directly
#define SYS_MEMORY_PAGE_SIZE_1M 0x400ULL

static int memory_allocate(uint64_t size, uint64_t flags, uint32_t* address) {
    system_call_3(348, size, flags, (uint32_t)address);
    return_to_user_prog(int);
}


// The game's table of gadget models it swaps in and out: their moby class, where the model sits in memory and its size,
//  and the textures for each (16 per model, -1 for none)
#define GADGET_MODEL_COUNT (*(u32*)0xa4dae4)
#define GADGET_MODEL_O_CLASS ((u32*)0xa4d540)
#define GADGET_MODEL_SOURCE ((u32*)0xa4d5a0)
#define GADGET_MODEL_SIZE ((u32*)0xa4d600)
#define GADGET_MODEL_TEXTURES ((s16*)0xa4d7e0)
#define GADGET_TEXTURE_PAGE (*(u32*)0xa4dae0)

// The size of each of the game's two gadget model buffers
#define GADGET_BUFFER_SIZE 0x30000

// Moby class -> slot in the class table, and the class table itself
#define CLASS_SLOTS ((u8*)0xa354c0)
#define CLASS_SLOT_COUNT 0x800
#define CLASS_TABLE ((MobyClass**)0xa34c00)

#define OWNERS 2
#define MODEL_MEMORY_SIZE 0x100000  // 4 buffers, in a 1MB page

struct ModelBuffer {
    bool loaded;
    u16 o_class;
};

static u8* memory_ = nullptr;
static bool memory_failed_ = false;
static ModelBuffer buffers_[OWNERS * 2];
static int current_[OWNERS];

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

// Memory of our own for models. The SPU jobs that process models find their data from offsets the game works out
//  from the start of its memory, which come out right for any address with 32 bit wraparound.
static bool prepare_memory() {
    if (memory_ != nullptr || memory_failed_) {
        return memory_ != nullptr;
    }

    uint32_t address = 0;
    int err = memory_allocate(MODEL_MEMORY_SIZE, SYS_MEMORY_PAGE_SIZE_1M, &address);
    if (err != 0) {
        Logger::error("GadgetModels: couldn't allocate memory: 0x%08x", err);
        memory_failed_ = true;
        return false;
    }

    memory_ = (u8*)address;
    memset(buffers_, 0, sizeof(buffers_));

    Logger::info("GadgetModels: memory at 0x%08x", address);
    return true;
}

MobyClass* GadgetModels::load(u16 o_class, int owner) {
    if (owner < 0 || owner >= OWNERS) {
        return nullptr;
    }

    int index = gadget_index(o_class);
    if (index < 0 || GADGET_MODEL_SIZE[index] > GADGET_BUFFER_SIZE || GADGET_MODEL_SOURCE[index] == 0) {
        return nullptr;
    }

    if (!prepare_memory()) {
        return nullptr;
    }

    // One of the owner's buffers may have it already
    for (int i = 0; i < 2; i++) {
        int buffer = owner * 2 + i;
        if (buffers_[buffer].loaded && buffers_[buffer].o_class == o_class) {
            current_[owner] = i;
            return (MobyClass*)(memory_ + buffer * GADGET_BUFFER_SIZE);
        }
    }

    // Load into the buffer the owner didn't use last, which the GPU could still be drawing from
    current_[owner] ^= 1;
    int buffer = owner * 2 + current_[owner];
    u8* model = memory_ + buffer * GADGET_BUFFER_SIZE;

    buffers_[buffer].loaded = false;
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

    buffers_[buffer].loaded = true;
    buffers_[buffer].o_class = o_class;

    return (MobyClass*)model;
}

MobyClass* GadgetModels::loaded_class(u16 o_class) {
    // Slot 0xff: the level has no such class
    if (o_class >= CLASS_SLOT_COUNT || CLASS_SLOTS[o_class] == 0xff) {
        return nullptr;
    }

    return CLASS_TABLE[CLASS_SLOTS[o_class]];
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
