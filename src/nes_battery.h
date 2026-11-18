#pragma once
#include <stddef.h>
#include <stdint.h>

// SRAM iNES with battery; 8 KiB PRG RAM for the supported iNES 1.0 boards.
// No cart battery flag => no load/write (avoids saving volatile RAM).
void nes_battery_begin(const char* rom_path, uint8_t* sram, size_t length, bool has_battery);
void nes_battery_load(void);
bool nes_battery_save(bool force);
void nes_battery_poll(void);
void nes_battery_end(void);
