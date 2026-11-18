/* NES v0.1.8: RGB indices -> four background pixels per 32-bit word.
 * Based on the same technique used for SMS packed tile/scanline conversion.
 * Internal RAM: 4 KiB; no PSRAM tile lookups. The PPU palette can change
 * mid-game, so we detect every change at scanline start before drawing.
 */
#ifndef NES_BG_LUT_H
#define NES_BG_LUT_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint32_t colors[4][256];
    uint16_t spread[256];   /* bit (7-i) of a pattern byte -> bit 2*i */
    uint8_t last_palette[16];
    int ready;
} nes_bg_lut_t;

/* Not in IRAM: this only runs on a palette change, not on each tile. */
static inline void nes_bg_lut_update(nes_bg_lut_t *cache, const uint8_t *palette) {
    if (cache->ready && memcmp(cache->last_palette, palette, 16) == 0) return;
    if (!cache->ready) {
        /* One-time 512-byte table: it replaces ~30 shift/mask operations
         * per background tile with four table loads. */
        for (unsigned p = 0; p < 256; ++p) {
            unsigned s = 0;
            for (unsigned i = 0; i < 8; ++i)
                s |= ((p >> (7u - i)) & 1u) << (2u * i);
            cache->spread[p] = (uint16_t)s;
        }
    }
    memcpy(cache->last_palette, palette, 16);
    for (unsigned group = 0; group < 4; ++group) {
        const uint8_t *c = palette + 4u * group;
        for (unsigned k = 0; k < 256; ++k) {
            cache->colors[group][k] =
                (uint32_t)c[(k     ) & 3u]       |
                ((uint32_t)c[(k >> 2) & 3u] << 8)  |
                ((uint32_t)c[(k >> 4) & 3u] << 16) |
                ((uint32_t)c[(k >> 6) & 3u] << 24);
        }
    }
    cache->ready = 1;
}

static inline void nes_bg_lut_draw(const nes_bg_lut_t *cache, uint8_t *surface,
                                   uint8_t pat1, uint8_t pat2, unsigned group) {
    /* s holds eight 2-bit pixels, pixel 0 in bits 0-1 ... pixel 7 in 14-15;
     * the low byte indexes the LUT for pixels 0-3, the high byte for 4-7. */
    const unsigned s = (unsigned)cache->spread[pat1] |
                       ((unsigned)cache->spread[pat2] << 1);
    const unsigned k0 = s & 0xffu;
    const unsigned k1 = s >> 8;
    const uint32_t w0 = cache->colors[group & 3u][k0];
    const uint32_t w1 = cache->colors[group & 3u][k1];
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if ((((uintptr_t)surface) & 3u) == 0u) {
        ((uint32_t*)surface)[0] = w0;
        ((uint32_t*)surface)[1] = w1;
    } else
#endif
    {
        /* Valid even for scroll offsets 1,3,5,7: no unaligned stores. */
        memcpy(surface, &w0, 4);
        memcpy(surface + 4, &w1, 4);
    }
}
#endif
