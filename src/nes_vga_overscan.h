// NES v1.01: o quadro integral 256x240 e mostrado sem mascara de overscan.
// Valores padrao zero preservam todos os pixels e linhas, inclusive em F4.
// O suporte a recorte fica disponivel apenas para testes opcionais.
#ifndef NES_VGA_OVERSCAN_H
#define NES_VGA_OVERSCAN_H
#ifndef NES_VGA_OVERSCAN_SIDE
#define NES_VGA_OVERSCAN_SIDE 0
#endif
#ifndef NES_VGA_OVERSCAN_BOTTOM
#define NES_VGA_OVERSCAN_BOTTOM 0
#endif
#if NES_VGA_OVERSCAN_SIDE < 0 || NES_VGA_OVERSCAN_SIDE > 16 || (NES_VGA_OVERSCAN_SIDE % 4)
#error "NES_VGA_OVERSCAN_SIDE deve ser 0, 4, 8, 12 ou 16 (alinhamento DMA)"
#endif
#if NES_VGA_OVERSCAN_BOTTOM < 0 || NES_VGA_OVERSCAN_BOTTOM > 16
#error "NES_VGA_OVERSCAN_BOTTOM deve estar entre 0 e 16 linhas"
#endif
// v1.03: bordas suavizadas (nada e recortado). Os 4 pixels/linhas externos de
// cada borda ficam com NES_VGA_EDGE_PCT % do brilho. F5 alterna no jogo.
// Padrao: DESLIGADO ao iniciar (NES_VGA_EDGE_SOFT=0).
#ifndef NES_VGA_EDGE_SOFT
#define NES_VGA_EDGE_SOFT 0
#endif
#ifndef NES_VGA_EDGE_PX
#define NES_VGA_EDGE_PX 4
#endif
#ifndef NES_VGA_EDGE_PCT
#define NES_VGA_EDGE_PCT 30
#endif
#if NES_VGA_EDGE_PX != 4
#error "NES_VGA_EDGE_PX deve ser 4 (uma palavra de 4 pixels por lado)"
#endif
#define NES_VGA_VISIBLE_WIDTH (256 - 2 * NES_VGA_OVERSCAN_SIDE)
#define NES_VGA_LAST_VISIBLE_LINE (240 - NES_VGA_OVERSCAN_BOTTOM)
// Com SIDE=0, N permanece igual ao original (344 amostras no F4).
// Se for habilitado um recorte opcional, ajusta a largura fisica proporcional.
static inline int nes_vga_overscan_samples(int nominal_N) {
    int n = ((nominal_N * NES_VGA_VISIBLE_WIDTH + 512) / 1024) * 4;
    if (n < 312) n = 312;
    if (n > 400) n = 400;
    return n;
}
// Sem recorte, os blankings originais permanecem inalterados.
static inline void nes_vga_overscan_blank(int* front, int* back) {
    *front += NES_VGA_OVERSCAN_SIDE;
    *back  += NES_VGA_OVERSCAN_SIDE;
}
#endif
