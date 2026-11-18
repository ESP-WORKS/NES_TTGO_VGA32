// sms_hw_vblend.h - suavizacao vertical da tela cheia por hardware (v0.1.42)
//
// Na tela cheia o DMA repete linhas do framebuffer: com V=448 cada 3 linhas do
// jogo ocupam 7 linhas de video (2,3,2), e a linha de 3 repeticoes forma uma
// "grade" de linhas grossas. Aqui as linhas de repeticao do meio viram linhas
// INTERMEDIARIAS (media das duas vizinhas do jogo): r0 r0 M01 r1 M12 r2 r2.
// A altura efetiva por linha do jogo passa de (2, 3, 2) para (2,5; 2; 2,5), a
// metade do desvio, e as bordas ficam suaves em vez de degraus.
// So as linhas "M" precisam de buffer proprio (2 a cada 3 linhas do jogo = 128
// linhas de 256 B); o resto continua apontando para o framebuffer.
// Funcoes puras (sem ESP32) para testar no PC.
#ifndef SMS_HW_VBLEND_H
#define SMS_HW_VBLEND_H
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#define SMS_HW_VB_MAX_V     480
#define SMS_HW_VB_MAX_BLEND 192
#define SMS_HW_VB_WIDTH     256   // amostras ativas por linha

typedef struct {
    int V;                                   // linhas de video da imagem
    int nblend;                              // linhas intermediarias distintas
    int16_t line_row[SMS_HW_VB_MAX_V];       // >= 0: linha pura do jogo; < 0: -(k+1) = blend k
    uint8_t blend_a[SMS_HW_VB_MAX_BLEND];    // blend k = media das linhas a e a+1
    int16_t blend_of_a[192];                 // a -> k ou -1
} SMSHwVMap;

// Constroi o mapa. allow_blend = 0 gera so linhas puras (centro do pixel, como
// antes). Com blend: linhas cujo centro cai a 0,36..0,64 entre duas linhas do
// jogo viram media 50/50; as demais ficam puras na linha mais proxima.
static inline void sms_hw_vmap_build(SMSHwVMap* m, int V, int allow_blend) {
    memset(m, 0, sizeof(*m));
    for (int a = 0; a < 192; ++a) m->blend_of_a[a] = -1;
    m->V = V;
    for (int j = 0; j < V; ++j) {
        const long pnum = (long)(2 * j + 1) * 192 - V;     // posicao = pnum / (2V)
        int a; long rem;
        if (pnum < 0) { m->line_row[j] = 0; continue; }
        a = (int)(pnum / (2L * V)); rem = pnum - (long)a * 2L * V;
        if (a >= 191) { m->line_row[j] = 191; continue; }
        if (allow_blend && rem * 100 >= 36L * 2 * V && rem * 100 <= 64L * 2 * V) {
            int k = m->blend_of_a[a];
            if (k < 0 && m->nblend < SMS_HW_VB_MAX_BLEND) {
                k = m->nblend++;
                m->blend_a[k] = (uint8_t)a;
                m->blend_of_a[a] = (int16_t)k;
            }
            if (k >= 0) { m->line_row[j] = (int16_t)(-(k + 1)); continue; }
        }
        m->line_row[j] = (int16_t)(rem * 2 >= 2L * V ? a + 1 : a);
    }
}

// Media por canal RGB222 (2 bits/canal em bits 0..5; bits de sync iguais nos dois
// bytes sao preservados), 4 pixels por palavra. up = arredonda para cima; senao
// para baixo (alternar evita um desvio sistematico de brilho).
static inline void sms_hw_blend_row(uint32_t* dst, const uint32_t* a, const uint32_t* b,
                                    size_t words, int up) {
    if (up) {
        for (size_t i = 0; i < words; ++i) {
            const uint32_t x = a[i], y = b[i];
            dst[i] = (x | y) - (((x ^ y) >> 1) & 0x15151515u);
        }
    } else {
        for (size_t i = 0; i < words; ++i) {
            const uint32_t x = a[i], y = b[i];
            dst[i] = (x & y) + (((x ^ y) >> 1) & 0x15151515u);
        }
    }
}

// Copia as linhas sujas de src para dst (pitch bytes por linha) em blocos de 8
// linhas e recalcula as linhas intermediarias assim que as duas linhas-fonte ja
// estao em dst. Assim cada linha intermediaria fica pronta logo depois das suas
// fontes, sempre antes do feixe de varredura (que passa depois da copia).
// blend_rows[k]: buffer de 256 B (alinhado em 4). blend_dirty[k]: precisa recalcular.
// Retorna bytes copiados.
static inline size_t sms_hw_copy_and_blend(uint8_t* dst, const uint8_t* src, uint8_t* dirty,
                                           int rows, int pitch, const SMSHwVMap* m,
                                           uint8_t* const* blend_rows, uint8_t* blend_dirty) {
    size_t bytes = 0;
    const int game_rows = rows < 192 ? rows : 192;
    for (int c = 0; c < rows; c += 8) {
        const int c_end = c + 8 < rows ? c + 8 : rows;      // exclusivo
        for (int y = c; y < c_end;) {
            if (!dirty[y]) { ++y; continue; }
            const int first = y;
            while (y < c_end && dirty[y]) ++y;
            memcpy(dst + (size_t)first * pitch, src + (size_t)first * pitch, (size_t)(y - first) * pitch);
            memset(dirty + first, 0, (size_t)(y - first));
            bytes += (size_t)(y - first) * pitch;
            for (int r = first; r < y && r < game_rows; ++r) {
                if (m->blend_of_a[r] >= 0) blend_dirty[m->blend_of_a[r]] = 1;
                if (r > 0 && m->blend_of_a[r - 1] >= 0) blend_dirty[m->blend_of_a[r - 1]] = 1;
            }
        }
        // Linhas intermediarias cujas duas fontes ja foram tratadas (a+1 < c_end).
        for (int k = 0; k < m->nblend; ++k) {
            const int a = m->blend_a[k];
            if (!blend_dirty[k] || a + 1 >= c_end) continue;
            sms_hw_blend_row((uint32_t*)blend_rows[k], (const uint32_t*)(dst + (size_t)a * pitch),
                             (const uint32_t*)(dst + (size_t)(a + 1) * pitch),
                             SMS_HW_VB_WIDTH / 4, (k & 1) == 0);
            blend_dirty[k] = 0;
        }
    }
    for (int k = 0; k < m->nblend; ++k) {                   // sobras (a+1 na ultima linha)
        if (!blend_dirty[k]) continue;
        const int a = m->blend_a[k];
        sms_hw_blend_row((uint32_t*)blend_rows[k], (const uint32_t*)(dst + (size_t)a * pitch),
                         (const uint32_t*)(dst + (size_t)(a + 1) * pitch), SMS_HW_VB_WIDTH / 4,
                         (k & 1) == 0);
        blend_dirty[k] = 0;
    }
    return bytes;
}
#endif
