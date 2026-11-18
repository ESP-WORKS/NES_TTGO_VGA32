/* One-time sRGB(8-bit) -> RGB222 nearest-level quantization.
 * VGA DAC levels are 0,85,170,255 (nominal).  The original >>6 truncation
 * turned values 192..212 into 255 instead of 170.  No per-pixel overhead. */
#ifndef NES_RGB222_PALETTE_H
#define NES_RGB222_PALETTE_H
#include <stdint.h>
static inline uint8_t nes_rgb8_to_rgb2(uint8_t v) {
    return (uint8_t)(((unsigned)v + 42u) / 85u);
}
static inline uint8_t nes_rgb_to_vga222(uint8_t r, uint8_t g, uint8_t b) {
    return (uint8_t)((nes_rgb8_to_rgb2(b) << 4) |
                     (nes_rgb8_to_rgb2(g) << 2) |
                      nes_rgb8_to_rgb2(r));
}
/* v0.1.16: perceptual correction for two red hues in the canonical
 * 2C02 reference palette.  Rounding each RGB component independently to
 * 0/85/170/255 mapped $16 (red) to brown/orange and $26 (salmon) to orange.
 * DeltaE2000 vs the original Mesen RGB reference (host validation):
 * $16: 15.91 -> 6.08; $26: 21.81 -> 8.43.
 * v0.1.17: two cyan hues are corrected conservatively, after a full
 * 64-color CIEDE2000 assessment. No frame/scanline cost. This runs on palette install,
 * never in the raster/PPU loop.  Never alter alternate/custom palettes. */
#ifndef NES_RED_HUE_RGB222
#define NES_RED_HUE_RGB222 1
#endif
#ifndef NES_CYAN_HUE_RGB222
#define NES_CYAN_HUE_RGB222 1
#endif
static inline uint8_t nes_rgb_to_vga222_2c02(unsigned index,
                                               uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t normal = nes_rgb_to_vga222(r, g, b);
#if NES_RED_HUE_RGB222
    if (index == 0x16u && r == 0xB5u && g == 0x31u && b == 0x20u)
        return 0x02u; /* RGB (170,0,0) vs previous (170,85,0) */
    if (index == 0x26u && r == 0xFEu && g == 0x81u && b == 0x70u)
        return 0x17u; /* RGB (255,85,85) vs previous (255,170,85) */
#endif
#if NES_CYAN_HUE_RGB222
    // Avoid pushing NES teal/light cyan into bright blue on RGB222.
    // Exact RGB guards ensure alternate palettes are not affected.
    if (index == 0x1Cu && r == 0x00u && g == 0x7Cu && b == 0x8Du)
        return 0x14u; /* RGB 0,85,85 vs previous 0,85,170 */
    if (index == 0x2Cu && r == 0x48u && g == 0xCDu && b == 0xDEu)
        return 0x29u; /* RGB 85,170,170 vs previous 85,170,255 */
#endif
    (void)index;
    return normal;
}
#endif
