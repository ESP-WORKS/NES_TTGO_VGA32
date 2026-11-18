/* BootRC shared with MC-1000 v0.92; ESP-WORKS bootloader pin configuration. */
#pragma once
struct NESBootRC {
    int kbddat = 32;
    int kbdclk = 33;
    int magicb = -1;
    bool loaded = false;
};
extern NESBootRC g_nes_bootrc;
void nes_bootrc_load();
