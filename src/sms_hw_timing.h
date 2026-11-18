// sms_hw_timing.h - temporizacao do modo de varredura esticado (funcoes puras)
//
// Usado por vga_6bit.cpp e testado no PC (tests/test_sms_hw_timing.cpp), com
// buffers "canario" para provar que nada e escrito alem da capacidade.
#ifndef SMS_HW_TIMING_H
#define SMS_HW_TIMING_H
#include <stdint.h>

// N amostras por linha (400 = 320x240 original a 12,6 MHz). Divide o blanking
// (N-256 amostras) em front porch, sync e back porch, com margem preta
// simetrica em volta da imagem. h_shift > 0 move a imagem para a esquerda.
static inline void sms_hw_timing(int N, int h_shift, int* front, int* sync, int* back) {
    const int window10 = N * 8;                       // janela ativa padrao: N*0,8 (decimos)
    int margin = (window10 - 2560 + 10) / 20;         // (janela - 256) / 2
    if (margin < 0) margin = 0;
    const int s = (N * 12 + 50) / 100;                // sync ~3,81 us
    int f = (N * 2 + 50) / 100 + margin + h_shift;    // front 0,635 us + margem direita
    if (f < 1) f = 1;
    int b = (N - 256) - s - f;                        // back porch + margem esquerda
    if (b < 1) { b = 1; f = (N - 256) - s - b; }
    *front = f; *sync = s; *back = b;
}

// Linha do jogo (0..191) mostrada na linha j (0..V-1) da imagem esticada, por
// amostragem no centro. Quando V e multiplo de 24 (432, 456...) cada 8 linhas do
// jogo (um tile) ocupam sempre o mesmo numero inteiro de linhas de video, com o
// mesmo padrao: tiles repetidos ficam identicos na tela.
static inline int sms_hw_row_for_line(int j, int V) {
    return ((2 * j + 1) * 192) / (2 * V);
}

// Coeficientes do APLL (VCO = APLL x 18 com o_div = 7): clock de pixel = N x 31,5 kHz.
static inline void sms_hw_apll(int N, uint32_t* sdm0, uint32_t* sdm1, uint32_t* sdm2) {
    const uint64_t q16 = ((uint64_t)N * 1134ull * 65536ull + 20000ull) / 40000ull;
    *sdm2 = (uint32_t)(q16 >> 16) - 4u;
    *sdm1 = (uint32_t)((q16 >> 8) & 0xFFu);
    *sdm0 = (uint32_t)(q16 & 0xFFu);
}

// Escreve os padroes de hblank (linha normal e linha de vsync). A FIFO I2S de 16
// bits inverte os bytes em pares: posicao logica i fica no indice i ^ 2.
// Devolve 0 e NAO escreve nada se o blanking nao couber em 'cap' bytes.
static inline int sms_hw_fill_hblank(uint8_t* vnorm, uint8_t* vsync, int cap,
                                     int front, int sync, int back,
                                     uint8_t hsync_on, uint8_t hsync_off,
                                     uint8_t vsync_on, uint8_t vsync_off) {
    const int len = front + sync + back;
    if (len <= 0 || (len & 3) != 0 || len > cap) return 0;
    for (int i = 0; i < len; i++) {
        const int j = i ^ 2;
        if (i < front || i >= front + sync) {
            vnorm[j] = (uint8_t)(hsync_off | vsync_off);
            vsync[j] = (uint8_t)(hsync_off | vsync_on);
        } else {
            vnorm[j] = (uint8_t)(hsync_on | vsync_off);
            vsync[j] = (uint8_t)(hsync_on | vsync_on);
        }
    }
    return len;
}
#endif
