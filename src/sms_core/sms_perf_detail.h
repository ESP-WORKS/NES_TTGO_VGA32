/* Optional, sparse instrumentation: 1 of every 64 emulated frames.
 * No ESP timer calls on ordinary frames. Never changes Z80/VDP clocks.
 * Can be disabled by defining SMS_DETAIL_PROFILE=0 at build time.
 */
#ifndef SMS_PERF_DETAIL_H
#define SMS_PERF_DETAIL_H
#include <stdint.h>
#include "../../config.h"
#ifndef SMS_DETAIL_PROFILE
#define SMS_DETAIL_PROFILE 0
#endif
#if SMS_DETAIL_PROFILE && defined(ESP_PLATFORM)
#define SMS_DETAIL_ACTIVE 1
#else
#define SMS_DETAIL_ACTIVE 0
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t sample_frames;
    uint32_t sample_render_frames; /* amostras que realmente desenharam */
    uint32_t rendered_lines;
    uint32_t vdp_us;
    uint32_t z80_us;
    uint32_t z80_quarter_us[4];  /* 262 scanlines, 4 partes (66/66/66/64) */
    // v0.1.14: histograma de opcodes PRIMARIOS por nibble alto (0x0..0xF).
    // Prefixos CB/ED/DD/FD contam na faixa do opcode primario; o byte
    // seguinte e executado pela rotina original e NAO e contado 2x.
    uint32_t z80_opcode_group[16];
    uint32_t z80_opcode_total;
    uint32_t z80_prefix_count[4]; // CB, ED, DD, FD
    uint32_t z80_opcode_profiled_calls; // 262 chamadas por quadro amostrado
    uint32_t render_us;
    uint32_t background_us;
    uint32_t bg_zero_scroll_fast_lines; // linhas SMS no caminho especializado
    uint32_t bg_fractional_fast_lines; // linhas SMS R8 fracionario em word32 alinhado
    uint32_t bg_total_lines; // linhas SMS de BG amostradas
    uint32_t sprites_us;
    uint32_t video_convert_us;
    // Subconjuntos de CONVERT: apenas conversao LUT e memcpy PSRAM.
    // O resto inclui despacho, limites e controle de linhas sujas.
    uint32_t video_pack_us;
    uint32_t video_psram_us;
    uint32_t video_fast_lines;
    uint32_t video_fallback_lines;
    // v0.1.11: tempo de leitura/comparacao PSRAM; linhas identicas
    // podem dispensar COPY, mas NUNCA a marcacao de linha dirty.
    uint32_t video_compare_us;
    uint32_t video_skipped_lines;
} sms_detail_stats_t;
#if SMS_DETAIL_ACTIVE
extern unsigned char sms_detail_sampling;
extern sms_detail_stats_t sms_detail_stats;
void sms_perf_detail_take(sms_detail_stats_t* output);
#endif
#ifdef __cplusplus
}
#endif
#endif
