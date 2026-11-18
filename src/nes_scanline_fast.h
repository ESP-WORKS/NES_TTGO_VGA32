#pragma once
#include <stdint.h>
/* NES PPU logical input is 256 8-bit pixels; the VGA DMA buffer stores
 * 4-byte words in physical order x^2 (2,3,0,1).  LUT already includes
 * sync bits and masks the PPU priority/transparent flags.  No new buffers.
 * Always inline: called from IRAM_ATTR emu_DrawLine. */
static inline __attribute__((always_inline)) void nes_pack_scanline_256(
        uint32_t* dst, const uint8_t* pixels, const uint8_t* lut) {
    for (unsigned x=0; x<256; x+=4) {
        dst[x >> 2] = (uint32_t)lut[pixels[x+2]] |
                      ((uint32_t)lut[pixels[x+3]] << 8) |
                      ((uint32_t)lut[pixels[x]] << 16) |
                      ((uint32_t)lut[pixels[x+1]] << 24);
    }
}

