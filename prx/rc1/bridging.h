//
// Created by Vetle Hjelle on 19/12/2022.
//

#ifndef PS3_CKIT_BRIDGING_H
#define PS3_CKIT_BRIDGING_H

#ifdef __cplusplus
extern "C" {
#endif

#include "rc1/game/Moby.h"

void cpp_start();

void _c_game_tick();
void _c_game_render();
void _c_game_reset();

void _c_on_respawn();

void _c_before_init_gameplay();
void _c_after_init_gameplay();

void _c_moby_update(Moby* moby);

bool _c_update_mobys();
void _c_on_moby_animation_started(Moby* moby, u32 blend_frames);
void _c_on_moby_get_damage(Moby* moby);

void _c_on_effect_allocated(u8* effect);
void _c_on_moby_spawned(Moby* moby);
void _c_before_player_update();
void _c_after_player_update();

bool _c_should_play_sound(Moby* moby);
void _c_on_sound_played(u8 function, s32 index, s32 flags, Moby* moby, u16 sound_class, s32 handle);
void _c_on_sound_stopped(s32 handle);

bool _c_on_game_heap_full(u32* heap, s32 size, u32 caller);

void _c_set_ratchet_animation_duration(u32 duration);

void _c_bink_do_frame();
void _c_on_save_operation(int action, void* savedata);

#ifdef __cplusplus
}
#endif

#endif //PS3_CKIT_BRIDGING_H
