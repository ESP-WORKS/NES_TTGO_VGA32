#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool nes_load_rom(const char* path);
void nes_unload_rom(void);
void nes_run_frame(bool skip);
void nes_reset_game(bool hard);
void nes_set_pad(uint8_t buttons);
void nes_audio_frame(void);
void nes_save_battery(void);
void nes_set_fullscreen(bool enabled);
/* v1.03: bordas suavizadas (F5 no jogo). */
void nes_set_edge_soft(bool on);
bool nes_get_edge_soft(void);
#ifdef __cplusplus
}
#endif
