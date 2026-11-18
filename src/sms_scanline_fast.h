#ifndef SMS_SCANLINE_FAST_H
#define SMS_SCANLINE_FAST_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
// Como em TK2000 VGA, os pixels sao escritos em blocos de 4 com XOR 2.
// Empacotar na RAM interna e enviar a linha por UM memcpy para PSRAM.
// width deve ser multiplo de 4; a paleta ja contem os bits de sync VGA.
static inline void sms_pack_vga_scanline(uint8_t* dst, const uint8_t* src,
                                          const uint8_t* palette, size_t width) {
    for (size_t x = 0; x < width; x += 4) {
        dst[x]     = palette[src[x + 2] & 31];
        dst[x + 1] = palette[src[x + 3] & 31];
        dst[x + 2] = palette[src[x] & 31];
        dst[x + 3] = palette[src[x + 1] & 31];
    }
}
/* Two-pixel palette pairs: 2 KiB of internal RAM in exchange for two
 * lookups instead of four per 4-pixel group. Keeps XOR2 byte ordering and
 * all VGA sync bits untouched. Only 64 pairs must be updated when one of
 * the 32 CRAM entries changes (instead of rebuilding the entire table).
 * ESP32 and host test are little-endian. */
static inline void sms_palette_pairs_update_color(uint16_t* pairs,
                                                   const uint8_t* palette,
                                                   unsigned color) {
    const unsigned c = color & 31u;
    for (unsigned i = 0; i < 32u; ++i) {
        pairs[(c << 5) | i] = (uint16_t)(palette[c] | ((uint16_t)palette[i] << 8));
        pairs[(i << 5) | c] = (uint16_t)(palette[i] | ((uint16_t)palette[c] << 8));
    }
}
static inline void sms_palette_pairs_init(uint16_t* pairs, const uint8_t* palette) {
    for (unsigned a = 0; a < 32u; ++a)
        for (unsigned b = 0; b < 32u; ++b)
            pairs[(a << 5) | b] = (uint16_t)(palette[a] | ((uint16_t)palette[b] << 8));
}
static inline void sms_pack_vga_scanline_pairs(uint8_t* dst, const uint8_t* src,
                                               const uint16_t* pairs, size_t width) {
    for (size_t x = 0; x < width; x += 4) {
        const uint16_t right = pairs[((src[x + 2] & 31u) << 5) | (src[x + 3] & 31u)];
        const uint16_t left  = pairs[((src[x] & 31u) << 5) | (src[x + 1] & 31u)];
        /* A two-byte memcpy is inlined to a halfword store by GCC; use
         * memcpy rather than violating uint8_t buffer effective types. */
        memcpy(dst + x, &right, 2);
        memcpy(dst + x + 2, &left, 2);
    }
}

/* v0.1.21: compact 32-entry RGB222 lookup (SMS/GG CRAM has 32
 * simultaneously selectable colors, each can encode any of the 64 RGB222
 * values). Adapted from the 64-entry LUT used by MSX v38.9. Unlike the
 * 1024-entry pair LUT, this reads just 32 bytes of palette while converting.
 * The original index byte's high status bits are ignored exactly as before.
 * The 32-bit output preserves the VGA I2S XOR2 [2,3,0,1] byte order.
 * Output alignment: 4; input may be aligned or unaligned. Width multiple
 * of four; no extra reads, no PSRAM reads, no dynamic palette recompute.
 */
static inline void sms_pack_vga_scanline_compact32(uint32_t* dst_words,
                                                    const uint8_t* src,
                                                    const uint8_t* palette,
                                                    size_t width) {
    for (size_t x = 0; x < width; x += 4u) {
        const uint32_t p2 = palette[src[x + 2u] & 31u];
        const uint32_t p3 = palette[src[x + 3u] & 31u];
        const uint32_t p0 = palette[src[x] & 31u];
        const uint32_t p1 = palette[src[x + 1u] & 31u];
        dst_words[x >> 2] = p2 | (p3 << 8) | (p0 << 16) | (p1 << 24);
    }
}

/* v0.1.8: one 32-bit source read and one 32-bit packed output store
 * per 4 SMS/GG pixels. This retains the v0.1.6 two-pixel palette LUT,
 * XOR2 byte order and both VGA sync bits. All data reads are from the
 * original internal-RAM scanline; the caller then copies a full line
 * to PSRAM, except when the optional exact equality check skips it.
 *
 * PRECONDITION: src is 4-byte aligned, width is a multiple of 4, and
 * dst_words holds at least width/4 uint32_t values. The renderer's
 * linebuf is 4-aligned, and its viewport offsets are multiples of 8.
 * Other callers must use sms_pack_vga_scanline_pairs() instead.
 * Using memcpy for the uint8_t pixel input avoids type punning/alias UB;
 * GCC optimizes the proven-aligned constant-size copy to a word read.
 */
/* Impressao digital APENAS como filtro rapido para evitar ler PSRAM
 * em linhas alteradas. Nunca autoriza omitir gravacao sem memcmp exato.
 * A linha ja esta alinhada em RAM interna, em palavras de 32 bits.
 */
static inline uint32_t sms_vga_packed_line_hash(const uint32_t* words,
                                                 size_t width) {
    uint32_t h = 2166136261u ^ (uint32_t)width;
    for (size_t i = 0; i < width / 4u; ++i) {
        h ^= words[i];
        h *= 16777619u;
    }
    return h;
}

/* Checagem exata de linha ja presente no stage PSRAM. A maioria das
 * linhas modificadas e descartada depois de 3 palavras amostradas;
 * se todas coincidirem, memcmp valida o conteudo INTEIRO para impedir
 * falsos positivos e glitches em paleta/sprites. Nunca compara indices
 * de paleta: compara bytes VGA finais, inclusive bits de sync.
 * Nao modifica memoria; retorno true significa que pode omitir memcpy.
 * width <= 256 e multiplo de quatro no caminho rapido.
 */
static inline int sms_vga_line_is_identical(const uint8_t* stage,
                                            const uint8_t* packed,
                                            size_t width) {
    if (width < 4u || (width & 3u)) return 0;
    uint32_t a, b;
    memcpy(&a, stage, sizeof(a));
    memcpy(&b, packed, sizeof(b));
    if (a != b) return 0;
    const size_t middle = (width / 2u) & ~(size_t)3u;
    memcpy(&a, stage + middle, sizeof(a));
    memcpy(&b, packed + middle, sizeof(b));
    if (a != b) return 0;
    memcpy(&a, stage + width - 4u, sizeof(a));
    memcpy(&b, packed + width - 4u, sizeof(b));
    if (a != b) return 0;
    return memcmp(stage, packed, width) == 0;
}

/* v0.1.14: caminho ALTERNATIVO para gravar uma linha completa na PSRAM.
 * Nenhuma leitura do destino (a regressao de v0.1.11 nao retorna).
 * Em enderecos alinhados podemos testar lojas de 32 bits desenroladas,
 * mantendo memcpy como baseline e fallback. ESP32/host little-endian.
 * Nao faz suposicoes de tamanho >= 16 ou stride entre linhas. */
static inline void sms_vga_psram_write32(uint8_t* dst,
                                          const uint32_t* packed,
                                          size_t width) {
    if (((uintptr_t)dst & 3u) != 0u || (width & 15u) != 0u) {
        memcpy(dst, packed, width);
        return;
    }
    uint32_t* out = (uint32_t*)dst;
    for (size_t i = 0; i < width / 4u; i += 4u) {
        out[i] = packed[i];
        out[i + 1u] = packed[i + 1u];
        out[i + 2u] = packed[i + 2u];
        out[i + 3u] = packed[i + 3u];
    }
}

static inline void sms_pack_vga_scanline_words32(uint32_t* dst_words,
                                                 const uint8_t* src,
                                                 const uint16_t* pairs,
                                                 size_t width) {
#if defined(__GNUC__)
    const uint8_t* aligned_src = (const uint8_t*)__builtin_assume_aligned(src, 4);
#else
    const uint8_t* aligned_src = src;
#endif
#if defined(SMS_VGA_PACK_UNROLL2) && SMS_VGA_PACK_UNROLL2
    // v0.1.12 A/B: duas palavras por volta (8 pixels). Testar apenas
    // com a macro explicitamente ativada. Sem acesso fora do limite
    // para largura multipla de 4 mas nao multipla de 8.
    size_t x = 0;
    for (; x + 8u <= width; x += 8u) {
        uint32_t p0, p1;
        memcpy(&p0, aligned_src + x, sizeof(p0));
        memcpy(&p1, aligned_src + x + 4u, sizeof(p1));
        const unsigned l0 = ((p0 & 31u) << 5) | ((p0 >> 8) & 31u);
        const unsigned r0 = (((p0 >> 16) & 31u) << 5) | ((p0 >> 24) & 31u);
        const unsigned l1 = ((p1 & 31u) << 5) | ((p1 >> 8) & 31u);
        const unsigned r1 = (((p1 >> 16) & 31u) << 5) | ((p1 >> 24) & 31u);
        dst_words[x >> 2] = (uint32_t)pairs[r0] | ((uint32_t)pairs[l0] << 16);
        dst_words[(x >> 2) + 1u] = (uint32_t)pairs[r1] | ((uint32_t)pairs[l1] << 16);
    }
    if (x < width) {
        uint32_t pixels;
        memcpy(&pixels, aligned_src + x, sizeof(pixels));
        const unsigned l = ((pixels & 31u) << 5) | ((pixels >> 8) & 31u);
        const unsigned r = (((pixels >> 16) & 31u) << 5) | ((pixels >> 24) & 31u);
        dst_words[x >> 2] = (uint32_t)pairs[r] | ((uint32_t)pairs[l] << 16);
    }
#else
    for (size_t x = 0; x < width; x += 4) {
        uint32_t pixels;
        memcpy(&pixels, aligned_src + x, sizeof(pixels));
        const unsigned left_index = ((pixels & 31u) << 5) | ((pixels >> 8) & 31u);
        const unsigned right_index = (((pixels >> 16) & 31u) << 5) | ((pixels >> 24) & 31u);
        const uint32_t right = pairs[right_index];
        const uint32_t left = pairs[left_index];
        dst_words[x >> 2] = right | (left << 16);
    }
#endif
}


#endif
