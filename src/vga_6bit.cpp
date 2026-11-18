// vga_6bit.cpp
// Ricardo Massaro mod library (Bitluni) mod by ackerman (convert C, resize)
// Adapted for Anemoia NES Emulator

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <soc/rtc.h>
#include <soc/i2s_reg.h>
#include <soc/i2s_struct.h>
#include <soc/io_mux_reg.h>
#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <driver/periph_ctrl.h>

#if ARDUINO_ARCH_ESP32
#include <Arduino.h>
#include <rom/lldesc.h>
#define DELAY(n) delay(n)
#else
#include <esp32/rom/lldesc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#define DELAY(n) vTaskDelay((n) / portTICK_PERIOD_MS)
#endif

#include "vga_6bit.h"
#include "sms_hw_timing.h"
#include "nes_vga_overscan.h"
#include "sms_hw_vblend.h"
#include "sms_vga_dirty_rows.h"
#include "../config.h"
#include "sms_core/sms_iram.h"
#include "sms_line_ring.h"

// VGA mode timing definitions
const int VgaMode_vga_mode_320x240[12] = { 
    8, 48, 24, 320, 11, 2, 31, 480, 2, 12587500, 1, 1
};

const int VgaMode_vga_mode_320x200[12] = {
    8, 48, 24, 320, 12, 2, 35, 400, 2, 12587500, 0, 1
};

const int VgaMode_vga_mode_256x240[12] = {
    8, 48, 24, 256, 11, 2, 31, 480, 2, 12587500, 1, 1
};

// VGA timing variables
static int h_front, h_sync, h_back, h_pixels;   // temporizacao de VARREDURA atual
static int fb_pitch;                              // largura do framebuffer (320)
// v0.1.37: temporizacao original (320x240) para voltar do modo esticado.
static int orig_h_front, orig_h_sync, orig_h_back, orig_h_pixels;
static int v_front, v_sync, v_back, v_pixels;
static int v_div, pixel_clock;
static unsigned char h_polarity, v_polarity;

// DMA buffer descriptors
static lldesc_t* dma_buf_desc = nullptr;
static int dma_buf_desc_count = 0;
static uint8_t* dma_buf_hblank_vnorm = nullptr;
static uint8_t* dma_buf_hblank_vsync = nullptr;
static uint8_t* dma_buf_vblank_vnorm = nullptr;
static uint8_t* dma_buf_vblank_vsync = nullptr;
static int hblank_cap = 0;   // bytes alocados em cada buffer de hblank

// Double buffering
static uint8_t** framebuffer[3] = { nullptr, nullptr, nullptr };
static uint8_t* framebuffer_linear[3] = { nullptr, nullptr, nullptr };
static uint8_t active_fb = 0;
static uint8_t back_fb = 1;
static uint8_t num_fbs = 2; // DMA scanout + PSRAM; terceiro buffer opcional
static bool buffer_needs_update = false;
static uint8_t dirty_rows[240] = {0};
static uint32_t last_copy_us = 0;
static uint32_t last_vsync_us = 0;

// I2S interrupt handler
static intr_handle_t i2s_isr_handle = nullptr;
static volatile int vga_frame_count = 0;
// Inspirado no worker assincrono de video do MSX v38.9.
// DMA (0) exibe, CPU desenha em uma PSRAM (1/2), worker copia a outra
// na vertical. Nunca modificar o buffer que esta sendo copiado.
struct AsyncVideoJob { uint8_t stage; uint8_t dirty[240]; };
static QueueHandle_t video_ready_queue = nullptr;
static QueueHandle_t video_free_queue = nullptr;
static volatile uint32_t async_submitted = 0;
static volatile uint32_t async_completed = 0;
static bool async_enabled = false;
static uint32_t async_block_us = 0;


// Helper functions for sync bit manipulation
inline unsigned char vsync_inv_bit() { return v_polarity << 7; }
inline unsigned char hsync_inv_bit() { return h_polarity << 6; }
inline unsigned char vsync_bit() { return (1 - v_polarity) << 7; }
inline unsigned char hsync_bit() { return (1 - h_polarity) << 6; }
inline unsigned char sync_bits() { return vsync_inv_bit() | hsync_inv_bit(); }
inline int x_res() { return fb_pitch; }
inline int y_res() { return v_pixels / v_div; }

// I2S interrupt handler - increments frame counter
static volatile uint32_t vsync_ccount = 0;   // v0.1.39: ciclos de CPU no fim do quadro
static volatile bool vsync_seen = false;
static uint32_t direct_line_cc = 0;  // refreshed when hardware VGA timing changes
static void IRAM_ATTR i2s_isr(void* arg) {
    REG_WRITE(I2S_INT_CLR_REG(1), (REG_READ(I2S_INT_RAW_REG(1)) & 0xffffffc0) | 0x3f);
    uint32_t cc;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(cc));   // seguro em ISR nivel 3
    vsync_ccount = cc;
    vsync_seen = true;
    vga_frame_count++;
}
// Microssegundos desde o ultimo fim de quadro VGA (EOF do DMA); 0xFFFFFFFF
// se ainda nao houve nenhum. Valido para idades < ~17 s.
uint32_t vga_frame_sequence() { return (uint32_t)vga_frame_count; }
uint32_t vga_us_since_vsync() {
    if (!vsync_seen) return 0xFFFFFFFFu;
    uint32_t cc;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(cc));
    const uint32_t mhz = (uint32_t)getCpuFrequencyMhz();
    return (cc - vsync_ccount) / (mhz ? mhz : 240u);
}

// Set active framebuffer for display
static void set_active_framebuffer(unsigned char** fb) {
    for (int i = 0; i < v_pixels; i++) {
        int desc_idx = 2 * (v_front + v_sync + v_back + i) + 1;
        if (desc_idx < dma_buf_desc_count && fb) {
            dma_buf_desc[desc_idx].buf = fb[i / v_div];
            dma_buf_desc[desc_idx].length = h_pixels;
            dma_buf_desc[desc_idx].size = h_pixels;
        }
    }
}

// Mapeamento vertical do scanout (so ponteiros de descritores; sem copia).
//  - normal:      linha de video i -> linha i / v_div            (1:1)
//  - hw_stretch:  as 480 linhas de video mostram as 192 linhas do jogo em V
//    linhas centradas (V do perfil: NITIDA 432, GRANDE 448; margens pretas em
//    cima/embaixo para nao perder imagem no overscan do monitor). A linha do
//    jogo e escolhida pelo centro do pixel (sms_hw_row_for_line). Com V multiplo
//    de 24 cada tile de 8 linhas ocupa sempre as mesmas linhas de video. As
//    linhas de margem apontam para o buffer de blanking vertical (preto, sem sync).
static int hw_profile = 0;              // 0 = 1:1 (320x240), 1 = tela cheia esticada
#define hw_stretch (hw_profile != 0)
// Suavizacao vertical (sms_hw_vblend.h): mapa das linhas de video e buffers das
// linhas intermediarias (RAM interna DMA, so enquanto a tela cheia esta ligada).
static SMSHwVMap hw_vmap;
static uint8_t* hw_blend_chunk[SMS_HW_VB_MAX_BLEND / 16 + 1];
static int hw_blend_chunks = 0;
static uint8_t* hw_blend_rows[SMS_HW_VB_MAX_BLEND];
static uint8_t hw_blend_dirty[SMS_HW_VB_MAX_BLEND];
static bool hw_blend_active = false;
static void hw_blend_free() {
    for (int c = 0; c < hw_blend_chunks; ++c) { heap_caps_free(hw_blend_chunk[c]); hw_blend_chunk[c] = nullptr; }
    hw_blend_chunks = 0;
    hw_blend_active = false;
}
// Tenta alocar n linhas intermediarias (blocos de 16 linhas = 4 KiB). Falha sem
// efeito colateral se nao sobrar memoria interna alem da reserva.
static bool hw_blend_alloc(int n) {
    hw_blend_free();
    if (n <= 0) return false;
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    if (heap_caps_get_free_size(caps) < (size_t)n * 256u + SMS_HW_VBLEND_FREE_GUARD) return false;
    for (int k0 = 0; k0 < n; k0 += 16) {
        const int cnt = (n - k0 < 16) ? n - k0 : 16;
        uint8_t* p = (uint8_t*)heap_caps_malloc((size_t)cnt * 256u, caps);
        if (!p) { hw_blend_free(); return false; }
        hw_blend_chunk[hw_blend_chunks++] = p;
        for (int i = 0; i < cnt; ++i) hw_blend_rows[k0 + i] = p + (size_t)i * 256u;
    }
    for (int k = 0; k < n; ++k) {
        memset(hw_blend_rows[k], hsync_inv_bit() | vsync_inv_bit(), 256);
        hw_blend_dirty[k] = 1;     // calcular tudo no primeiro quadro
    }
    hw_blend_active = true;
    return true;
}
static inline size_t copy_dirty_rows_for_display(uint8_t* dst, const uint8_t* src, uint8_t* dirty) {
    if (hw_blend_active)
        return sms_hw_copy_and_blend(dst, src, dirty, y_res(), x_res(), &hw_vmap,
                                     hw_blend_rows, hw_blend_dirty);
    return sms_copy_dirty_rows(dst, src, dirty, y_res(), x_res());
}
static void apply_row_map() {
    if (!dma_buf_desc || !framebuffer[0]) return;
    unsigned char** fb = framebuffer[0];
    const int rows = y_res();
#ifndef NES_VIDEO_240
    const int V = hw_stretch ? SMS_HW_V_LINES : v_pixels;
    int top = hw_stretch ? (v_pixels - V) / 2 + SMS_HW_V_SHIFT : 0;
    if (top < 0) top = 0;
    if (top > v_pixels - V) top = v_pixels - V;
#endif
    for (int i = 0; i < v_pixels; i++) {
        const int desc_idx = 2 * (v_front + v_sync + v_back + i) + 1;
        if (desc_idx >= dma_buf_desc_count) break;
        int row;
        if (hw_stretch) {
#ifdef NES_VIDEO_240
            // NES: horizontal pixel clock is stretched, vertical scan remains
            // 480 VGA scanlines -> 240 native NES lines, with NO 192-line crop.
            row = i / v_div;
            // Em F4, ocultar fisicamente a faixa de overscan inferior.
            // Apontar o descritor para blanking: sem alterar PPU/PSRAM/DMA.
            if (row >= NES_VGA_LAST_VISIBLE_LINE) {
                dma_buf_desc[desc_idx].buf = dma_buf_vblank_vnorm;
                continue;
            }
#else
            const int j = i - top;
            if (j < 0 || j >= V) {
                // Margem preta: buffer de blanking vertical (RGB=0, sem sync).
                dma_buf_desc[desc_idx].buf = dma_buf_vblank_vnorm;
                continue;
            }
            const int v = hw_vmap.line_row[j];
            if (v < 0) {                       // linha intermediaria (suavizacao)
                dma_buf_desc[desc_idx].buf = hw_blend_rows[-v - 1];
                continue;
            }
            row = v;
#endif
        } else {
            row = i / v_div;
        }
        if (row >= rows) row = rows - 1;
        // No modo NES F4, a primeira amostra ativa e o pixel original 8
        // (ou valor configurado). O ponteiro continua alinhado em 32 bits.
#ifdef NES_VIDEO_240
        dma_buf_desc[desc_idx].buf = fb[row] + (hw_stretch ? NES_VGA_OVERSCAN_SIDE : 0);
#else
        dma_buf_desc[desc_idx].buf = fb[row];
#endif
    }
}
bool vga_hw_stretch_active() { return hw_stretch; }
int vga_hw_stretch_profile() { return hw_profile; }

// Allocate contiguous DMA scanout framebuffer for I2S.
static unsigned char** alloc_framebuffer(int index) {
    int yres = y_res();
    int pitch = fb_pitch;
    
    // Allocate single contiguous DMA-capable block
    int total_size = yres * pitch;
    uint8_t* linear = (uint8_t*)heap_caps_malloc(total_size, MALLOC_CAP_DMA);
    if (!linear) return nullptr;
    
    framebuffer_linear[index] = linear;
    
    unsigned char** fb = (unsigned char**)malloc(sizeof(unsigned char*) * yres);
    if (!fb) {
        free(linear);
        framebuffer_linear[index] = nullptr;
        return nullptr;
    }
    
    unsigned char sync = sync_bits();
    for (int i = 0; i < yres; i++) {
        fb[i] = linear + (i * pitch);
        memset(fb[i], sync, pitch);
    }
    
    return fb;
}

// Rendering must never touch the framebuffer while I2S is scanning it.
// TTGO VGA32 WROVER PSRAM holds the staging image; only the scanout image
// needs internal DMA-capable memory. This avoids two costly internal frames.
static unsigned char** alloc_psram_render_framebuffer(int index) {
    const int pitch = x_res(), height = y_res();
    const size_t total = (size_t)pitch * height;
    uint8_t* linear = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!linear) return nullptr;
    unsigned char** rows = (unsigned char**)malloc(sizeof(unsigned char*) * height);
    if (!rows) { free(linear); return nullptr; }
    framebuffer_linear[index] = linear;
    memset(linear, sync_bits(), total);
    for (int y = 0; y < height; ++y) rows[y] = linear + y * pitch;
    return rows;
}

// Initialize DMA buffers for VGA output
static void allocate_vga_i2s_buffers() {
    int hblank_len = h_front + h_sync + h_back;
    int pixel_len = h_pixels;
    // O modo de varredura esticado (vga_set_hw_stretch) usa um blanking
    // horizontal MAIOR que o do 320x240 (N-256 amostras, p.ex. 88 contra 80):
    // os buffers precisam ter a capacidade do maior dos dois, senao a escrita
    // dos padroes e a leitura do DMA passam do fim do bloco (corrupcao de heap).
    hblank_cap = hblank_len;
#if SMS_FULLSCREEN
#ifdef NES_VIDEO_240
    // NES F4: o DAC envia so 256-2*overscan pixels ativos; blanking maior.
    if (SMS_HW_H_SAMPLES - NES_VGA_VISIBLE_WIDTH > hblank_cap)
        hblank_cap = SMS_HW_H_SAMPLES - NES_VGA_VISIBLE_WIDTH;
#else
    if (SMS_HW_H_SAMPLES - 256 > hblank_cap) hblank_cap = SMS_HW_H_SAMPLES - 256;
#endif
#endif
    hblank_cap = (hblank_cap + 3) & ~3;
    
    dma_buf_hblank_vnorm = (uint8_t*)heap_caps_malloc(hblank_cap, MALLOC_CAP_DMA);
    dma_buf_hblank_vsync = (uint8_t*)heap_caps_malloc(hblank_cap, MALLOC_CAP_DMA);
    dma_buf_vblank_vnorm = (uint8_t*)heap_caps_malloc(pixel_len, MALLOC_CAP_DMA);
    dma_buf_vblank_vsync = (uint8_t*)heap_caps_malloc(pixel_len, MALLOC_CAP_DMA);
    
    if (!dma_buf_hblank_vnorm || !dma_buf_hblank_vsync ||
        !dma_buf_vblank_vnorm || !dma_buf_vblank_vsync) return;

    // Initialize blanking buffers with sync patterns
    for (int i = 0; i < hblank_len; i++) {
        if (i < h_front || i >= h_front + h_sync) {
            dma_buf_hblank_vnorm[i] = hsync_inv_bit() | vsync_inv_bit();
            dma_buf_hblank_vsync[i] = hsync_inv_bit() | vsync_bit();
        } else {
            dma_buf_hblank_vnorm[i] = hsync_bit() | vsync_inv_bit();
            dma_buf_hblank_vsync[i] = hsync_bit() | vsync_bit();
        }
    }
    for (int i = 0; i < h_pixels; i++) {
        dma_buf_vblank_vnorm[i] = hsync_inv_bit() | vsync_inv_bit();
        dma_buf_vblank_vsync[i] = hsync_inv_bit() | vsync_bit();
    }
    
    dma_buf_desc_count = 2 * (v_front + v_sync + v_back + v_pixels);
    dma_buf_desc = (lldesc_t*)heap_caps_calloc(dma_buf_desc_count, sizeof(lldesc_t), MALLOC_CAP_DMA);
    if (!dma_buf_desc) return;
    // ESP32 DMA needs owner, size and length on EVERY descriptor. Leaving
    // these uninitialized caused sporadic missing/corrupt horizontal lines.
    for (int i = 0; i < dma_buf_desc_count; ++i) {
        dma_buf_desc[i].owner = 1;
        dma_buf_desc[i].eof = 0;
        dma_buf_desc[i].sosf = 0;
    }
    
    // Link DMA descriptors in circular chain
    int d = 0;
    for (int i = 0; i < dma_buf_desc_count; i++) {
        dma_buf_desc[i].qe.stqe_next = &dma_buf_desc[(i+1) % dma_buf_desc_count];
    }
    dma_buf_desc[dma_buf_desc_count-1].eof = 1;
    
    // Configure vertical timing sequence
    d = 0;
    // Front porch
    for (int i = 0; i < v_front; i++) {
        dma_buf_desc[d].size = hblank_len;
        dma_buf_desc[d].length = hblank_len;
        dma_buf_desc[d].buf = dma_buf_hblank_vnorm;
        d++;
        dma_buf_desc[d].size = pixel_len;
        dma_buf_desc[d].length = pixel_len;
        dma_buf_desc[d].buf = dma_buf_vblank_vnorm;
        d++;
    }
    // Sync pulse
    for (int i = 0; i < v_sync; i++) {
        dma_buf_desc[d].size = hblank_len;
        dma_buf_desc[d].length = hblank_len;
        dma_buf_desc[d].buf = dma_buf_hblank_vsync;
        d++;
        dma_buf_desc[d].size = pixel_len;
        dma_buf_desc[d].length = pixel_len;
        dma_buf_desc[d].buf = dma_buf_vblank_vsync;
        d++;
    }
    // Back porch
    for (int i = 0; i < v_back; i++) {
        dma_buf_desc[d].size = hblank_len;
        dma_buf_desc[d].length = hblank_len;
        dma_buf_desc[d].buf = dma_buf_hblank_vnorm;
        d++;
        dma_buf_desc[d].size = pixel_len;
        dma_buf_desc[d].length = pixel_len;
        dma_buf_desc[d].buf = dma_buf_vblank_vnorm;
        d++;
    }
    // Active video area (to be filled later)
    for (int i = 0; i < v_pixels; i++) {
        dma_buf_desc[d].size = hblank_len;
        dma_buf_desc[d].length = hblank_len;
        dma_buf_desc[d].buf = dma_buf_hblank_vnorm;
        d++;
        dma_buf_desc[d].size = pixel_len;
        dma_buf_desc[d].length = pixel_len;
        dma_buf_desc[d].buf = (uint8_t*)0;
        d++;
    }
}

// Configure I2S peripheral for VGA output
// Divisores de clock, FIFO e canal do I2S1: o estado exato do modo 320x240.
// Usado na inicializacao e, de novo, ao voltar do modo esticado, para que o
// 1:1 seja identico ao original mesmo que algum registrador tenha sido alterado.
static void i2s_apply_clock_and_fifo_regs() {
    I2S1.clkm_conf.val = 0;
    I2S1.clkm_conf.clka_en = 1;
    I2S1.clkm_conf.clkm_div_num = 2;
    I2S1.sample_rate_conf.tx_bck_div_num = 1;
    
    // Configure FIFO for DMA
    I2S1.fifo_conf.val = 0;
    I2S1.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S1.fifo_conf.tx_fifo_mod = 1;
    I2S1.fifo_conf.dscr_en = 1;
    
    I2S1.conf_chan.tx_chan_mod = 1;
    I2S1.conf.tx_right_first = 1;
}

static void setup_i2s_output(const unsigned char* pin_map) {
    periph_module_enable(PERIPH_I2S1_MODULE);
    for (int i = 0; i < 8; i++) {
        int pin = pin_map[i];
        if (pin != 255) {
            PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[pin], PIN_FUNC_GPIO);
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
            gpio_matrix_out(pin, I2S1O_DATA_OUT0_IDX + i, false, false);
        }
    }
    
    // Reset I2S
    I2S1.conf.tx_reset = 1;
    I2S1.conf.tx_reset = 0;
    I2S1.conf.rx_reset = 1;
    I2S1.conf.rx_reset = 0;
    
    // Configure for LCD/VGA mode
    I2S1.conf2.val = 0;
    I2S1.conf2.lcd_en = 1;
    I2S1.conf2.lcd_tx_wrx2_en = 1;
    I2S1.sample_rate_conf.tx_bits_mod = 8;
    
    // Enable APLL for precise clock
    rtc_clk_apll_enable(true, 0x0A, 0x57, 0x07, 0x07);
    
    i2s_apply_clock_and_fifo_regs();
    
    // Install interrupt handler
    esp_intr_alloc(ETS_I2S1_INTR_SOURCE, 
                   ESP_INTR_FLAG_INTRDISABLED | ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM,
                   i2s_isr, NULL, &i2s_isr_handle);
}

// Start I2S output
static void start_i2s_output() {
    esp_intr_disable(i2s_isr_handle);
    
    I2S1.lc_conf.val |= I2S_IN_RST_M | I2S_OUT_RST_M;
    I2S1.lc_conf.val &= ~(I2S_IN_RST_M | I2S_OUT_RST_M);
    I2S1.lc_conf.val |= I2S_OUT_DATA_BURST_EN | I2S_OUTDSCR_BURST_EN;
    I2S1.out_link.addr = (uint32_t)&dma_buf_desc[0];
    I2S1.out_link.start = 1;
    
    I2S1.int_clr.val = I2S1.int_raw.val;
    I2S1.int_ena.out_eof = 1;
    esp_intr_enable(i2s_isr_handle);
    I2S1.conf.tx_start = 1;
}

// Modo de varredura "esticado" (tela cheia SMS por hardware).
// O tempo total de linha NAO muda (31,746 us = 31,5 kHz, o mesmo 640x480@60 de
// antes; o monitor nao troca de modo). Muda o clock de pixel: com N amostras
// por linha, clock = N x 31,5 kHz (N=400 -> 12,6 MHz, o modo 320x240 original;
// N=344 -> 10,836 MHz). Os 256 pixels do jogo ocupam 256 amostras e o resto da
// linha (N-256) e blanking: sync de ~3,81 us, front/back porch padrao e uma
// margem preta simetrica em volta da imagem, para o overscan do monitor nao
// cortar as bordas. APLL = 2 x clock; VCO = APLL x 18 (o_div=7), 350..500 MHz:
//   APLL = 40 MHz x (4 + sdm2 + sdm1/256 + sdm0/65536) / 18
static void write_hblank_patterns() {
    // Nunca escreve alem de hblank_cap (ver sms_hw_timing.h).
    sms_hw_fill_hblank(dma_buf_hblank_vnorm, dma_buf_hblank_vsync, hblank_cap,
                       h_front, h_sync, h_back,
                       hsync_bit(), hsync_inv_bit(), vsync_bit(), vsync_inv_bit());
}
// profile: 0 = volta ao 320x240; 1 = tela cheia esticada (ver config.h).
bool vga_set_hw_stretch(int profile) {
    if (!dma_buf_desc || !framebuffer[0] || !i2s_isr_handle) return false;
    if (profile < 0 || profile > 1) return false;
    if (profile == hw_profile) return true;
    const bool on = profile != 0;
    if (on && !(fb_pitch == 320 && orig_h_front == 8 && orig_h_sync == 48 &&
                orig_h_back == 24 && orig_h_pixels == 320)) return false;
    const int N = on ? nes_vga_overscan_samples(SMS_HW_H_SAMPLES) : 400;
    const int active_w = on ? NES_VGA_VISIBLE_WIDTH : 320;
    if (on && (N - active_w > hblank_cap || N < 312 || N > 400)) return false;
    // Validar a nova geometria ANTES de interromper o VGA; em caso de
    // definicao invalida, conservar intacta a varredura anterior.
    if (on) {
        int f, s, b;
        sms_hw_timing(N, SMS_HW_H_SHIFT, &f, &s, &b);
        nes_vga_overscan_blank(&f, &b);
        if ((f + s + b + active_w) != N || (active_w & 3) ||
            ((f + s + b) & 3)) return false;
    }
    // 1) parar a varredura (descritores e FIFO), sem tocar nos framebuffers
    esp_intr_disable(i2s_isr_handle);
    I2S1.out_link.stop = 1;
    I2S1.conf.tx_start = 0;
    I2S1.int_ena.val = 0;
    I2S1.int_clr.val = I2S1.int_raw.val;
    delayMicroseconds(100);
    I2S1.conf.tx_reset = 1;
    I2S1.conf.tx_reset = 0;
    I2S1.conf.tx_fifo_reset = 1;
    I2S1.conf.tx_fifo_reset = 0;
    // 2) nova temporizacao horizontal (mesmo tempo total de linha)
    if (on) {
        sms_hw_timing(N, SMS_HW_H_SHIFT, &h_front, &h_sync, &h_back);
        // Antes: 256 NES px ativos. Agora: viewport util menor, sem escalar
        // pixels nem deixar trilhas laterais; manter N e HSync identicos.
        nes_vga_overscan_blank(&h_front, &h_back);
        h_pixels = NES_VGA_VISIBLE_WIDTH;
    }
    else { h_front = orig_h_front; h_sync = orig_h_sync; h_back = orig_h_back;
           h_pixels = orig_h_pixels; }
    write_hblank_patterns();
    const int hblank_len = h_front + h_sync + h_back;
    // Invariantes de largura N e DMA alinhado validadas antes da mudanca,
    // e novamente em host (tests/test_v100_overscan.py). Nao ha retirada
    // parcial de DMA durante o jogo.
    for (int d = 0; d < dma_buf_desc_count; ++d) {
        const int len = (d & 1) ? h_pixels : hblank_len;   // par: blank, impar: ativo
        dma_buf_desc[d].size = len;
        dma_buf_desc[d].length = len;
    }
    if (on) {
#ifdef NES_VIDEO_240
        // NES 256x240: do not use the SMS 256x192 vertical interpolator.
        // The physical 480 lines continue to show 240 unique source rows.
        hw_blend_free();
#else
        // Mapa vertical (com linhas intermediarias se houver memoria).
        bool blend = SMS_HW_VBLEND != 0;
        if (blend) {
            sms_hw_vmap_build(&hw_vmap, SMS_HW_V_LINES, 1);
            blend = hw_blend_alloc(hw_vmap.nblend);
        }
        if (!blend) { hw_blend_free(); sms_hw_vmap_build(&hw_vmap, SMS_HW_V_LINES, 0); }
#endif
    }
    hw_profile = profile;
    apply_row_map();                 // descritores passam a apontar para os buffers certos
    if (!on) hw_blend_free();        // so depois de remapear: a varredura esta parada
    // 3) novo clock de pixel
    if (on) {
        uint32_t s0, s1, s2;
        sms_hw_apll(N, &s0, &s1, &s2);
        rtc_clk_apll_enable(true, s0, s1, s2, 7);
    } else {
        rtc_clk_apll_enable(true, 0x0A, 0x57, 0x07, 0x07);  // 25,2 MHz (12,6 MHz de pixel)
    }
    delayMicroseconds(1000);           // APLL estabiliza antes de reativar o I2S
    // Reconfigurar explicitamente LCD 8-bit/FIFO tanto ao entrar em F4
    // quanto ao sair. O reset FIFO acima nao deve deixar um modo de bytes
    // residual durante a nova sequencia de descritores (240/320 ativos).
    I2S1.conf2.val = 0;
    I2S1.conf2.lcd_en = 1;
    I2S1.conf2.lcd_tx_wrx2_en = 1;
    I2S1.sample_rate_conf.tx_bits_mod = 8;
    i2s_apply_clock_and_fifo_regs();
    // Os descritores reprogramados precisam ficar visiveis para o DMA antes
    // de reativar a saida I2S; so executado ao alternar F4, nao por frame.
    __sync_synchronize();
    // 4) reiniciar a varredura
    I2S1.out_link.stop = 0;
    start_i2s_output();
    return true;
}

// ==================== ASYNC VGA WORKER ====================
// Worker no core 0; o setup/loopTask do Arduino ESP32 2.0.17 usa core 1.
static void vga_video_worker(void*) {
    AsyncVideoJob job;
    for (;;) {
        if (xQueueReceive(video_ready_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        const int64_t wait_begin = esp_timer_get_time();
        const int fc = vga_frame_count;
        while (fc == vga_frame_count && esp_timer_get_time() - wait_begin < 20000LL) {
            // Com 35 FPS ou menor, uma espera ativa de alta prioridade
            // (taskYIELD) pode impedir Idle0 de alimentar o Task WDT.
            // Usar espera bloqueante em TODA a janela de VSync.
            vTaskDelay(1);
        }
        const int64_t copy_begin = esp_timer_get_time();
        last_vsync_us = (uint32_t)(copy_begin - wait_begin);
        copy_dirty_rows_for_display(framebuffer_linear[0], framebuffer_linear[job.stage], job.dirty);
        last_copy_us = (uint32_t)(esp_timer_get_time() - copy_begin);
        uint8_t free_stage = job.stage;
        ++async_completed;
        xQueueSend(video_free_queue, &free_stage, portMAX_DELAY);
    }
}

static void vga_async_try_init() {
    framebuffer[2] = alloc_psram_render_framebuffer(2);
    if (!framebuffer[2]) {
        Serial.println("[VGA] Modo sincronizado: PSRAM adicional indisponivel");
        return;
    }
    video_ready_queue = xQueueCreate(1, sizeof(AsyncVideoJob));
    video_free_queue = xQueueCreate(1, sizeof(uint8_t));
    if (!video_ready_queue || !video_free_queue) {
        if (video_ready_queue) vQueueDelete(video_ready_queue);
        if (video_free_queue) vQueueDelete(video_free_queue);
        video_ready_queue = video_free_queue = nullptr;
        free(framebuffer[2]); framebuffer[2] = nullptr;
        free(framebuffer_linear[2]); framebuffer_linear[2] = nullptr;
        return;
    }
    uint8_t initially_free = 2;
    xQueueSend(video_free_queue, &initially_free, 0);
    if (xTaskCreatePinnedToCore(vga_video_worker, "sms_vga", 4096, nullptr, 4,
                                nullptr, 0) != pdPASS) {
        vQueueDelete(video_ready_queue); vQueueDelete(video_free_queue);
        video_ready_queue = video_free_queue = nullptr;
        free(framebuffer[2]); framebuffer[2] = nullptr;
        free(framebuffer_linear[2]); framebuffer_linear[2] = nullptr;
        Serial.println("[VGA] Worker indisponivel; sincronizacao original ativa");
        return;
    }
    num_fbs = 3;
    async_enabled = true;
    Serial.println("[VGA] Worker PSRAM no core 0, emulador Arduino no core 1");
}

bool vga_async_available() { return async_enabled; }

void vga_async_wait_idle() {
    vga_line_ring_drain();
    if (!async_enabled) return;
    while (async_completed != async_submitted) vTaskDelay(1);
}

void vga_async_clear_stages(uint8_t value) {
    vga_async_wait_idle();
    if (!async_enabled) return;
    for (int i=1; i<=2; ++i) memset(framebuffer_linear[i], value,
                                        (size_t)x_res() * y_res());
    vga_mark_rows(0, y_res());
}

bool vga_async_submit() {
    if (!async_enabled) return false;
    vga_line_ring_drain();   // stage completo antes de entrega-lo ao worker
    uint8_t next_stage;
    const int64_t begin = esp_timer_get_time();
    // O stage corrente so e entregue quando ha outro stage totalmente livre.
    // Nao ha corrida com worker/DMA, nem perda de um frame ainda em copia.
    if (xQueueReceive(video_free_queue, &next_stage, portMAX_DELAY) != pdTRUE)
        return false;
    async_block_us = (uint32_t)(esp_timer_get_time() - begin);
    AsyncVideoJob job;
    job.stage = back_fb;
    memcpy(job.dirty, dirty_rows, sizeof(dirty_rows));
    memset(dirty_rows, 0, sizeof(dirty_rows));
    // Alterar render stage somente apos obter um buffer livre.
    back_fb = next_stage;
    ++async_submitted;
    xQueueSend(video_ready_queue, &job, portMAX_DELAY);
    return true;
}

uint32_t vga_async_wait_us() { return async_block_us; }

// ==================== LINE RING (v0.1.34) ====================
// Logica do anel em sms_line_ring.h (testada no host com threads).
// Aqui: memoria, tarefa no core 0 e callbacks FreeRTOS.
#if SMS_VGA_LINE_RING
#define LR_SLOTS ((uint32_t)SMS_VGA_LINE_RING_SLOTS)
static SMSLineRing lr_ring;
static TaskHandle_t lr_task = nullptr;
static volatile bool lr_active = false;
static volatile bool lr_quit = false;
static volatile bool lr_exited = true;

static void lr_wake(void*) { xTaskNotifyGive(lr_task); }
static void lr_idle(void*) { vTaskDelay(1); }

static void vga_line_ring_task(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        lr_ring.consume();
        if (lr_quit) break;
    }
    lr_exited = true;
    vTaskDelete(nullptr);
}

bool vga_line_ring_start() {
    if (lr_active) return true;
    if (!async_enabled) return false;   // so com o worker de video ativo
    SMSLineSlot* mem = (SMSLineSlot*)heap_caps_malloc(sizeof(SMSLineSlot) * LR_SLOTS,
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mem) {
        Serial.println("[VGA] Anel de linhas indisponivel: sem RAM interna");
        return false;
    }
    lr_ring.attach(mem, LR_SLOTS, (uint32_t)SMS_VGA_LINE_RING_BATCH);
    lr_quit = false;
    lr_exited = false;
    if (xTaskCreatePinnedToCore(vga_line_ring_task, "sms_vgaw", 3072, nullptr, 5,
                                &lr_task, 0) != pdPASS) {
        free(mem);
        lr_ring.attach(nullptr, 1, 1);
        lr_exited = true;
        Serial.println("[VGA] Anel de linhas indisponivel: tarefa nao criada");
        return false;
    }
    lr_active = true;
    Serial.printf("[VGA] Linhas PSRAM no core 0: anel %u x %u B, lote %u\n",
                  (unsigned)LR_SLOTS, (unsigned)sizeof(SMSLineSlot),
                  (unsigned)SMS_VGA_LINE_RING_BATCH);
    return true;
}

bool vga_line_ring_active() { return lr_active; }
uint32_t vga_line_ring_stalls() { return lr_ring.stalls(); }

SMS_HOT uint32_t* vga_line_ring_acquire() {
    if (!lr_active) return nullptr;
    return lr_ring.acquire(lr_wake, nullptr, lr_idle);
}

SMS_HOT void vga_line_ring_publish(uint8_t* dst, uint32_t len, bool flush) {
    lr_ring.publish(dst, len, flush, lr_wake, nullptr);
}

void vga_line_ring_drain() {
    if (!lr_active) return;
    lr_ring.drain(lr_wake, nullptr, lr_idle);
}

void vga_line_ring_stop() {
    if (!lr_active) return;
    vga_line_ring_drain();
    lr_active = false;           // novas linhas usam o caminho direto
    lr_quit = true;
    xTaskNotifyGive(lr_task);
    uint32_t guard = 0;
    while (!lr_exited && ++guard < 200u) vTaskDelay(1);
    vTaskDelay(2);               // IDLE0 libera TCB/pilha da tarefa
    SMSLineSlot* mem = lr_ring.slots();
    lr_ring.attach(nullptr, 1, 1);
    lr_task = nullptr;
    free(mem);
}
#else
bool vga_line_ring_start() { return false; }
void vga_line_ring_stop() {}
bool vga_line_ring_active() { return false; }
uint32_t vga_line_ring_stalls() { return 0; }
uint32_t* vga_line_ring_acquire() { return nullptr; }
void vga_line_ring_publish(uint8_t*, uint32_t, bool) {}
void vga_line_ring_drain() {}
#endif

// ==================== TOP-BAND + OSD VBLANK PRESENT (NES v0.1.15) ====================
// On NTSC scrolling, a 60 Hz CPU/PPU producer sometimes finishes the first
// 1/3 of the picture close to the DMA beam.  Write those rows into a small
// internal-RAM staging band and replace them only during vertical blank.
// The rest of the picture keeps the tested v0.1.12 direct-scanout fast path.
// This is intentionally NOT a second 320x240 framebuffer (only 24 KiB).
#ifndef NES_VGA_DIRECT_TOP_ROWS
#define NES_VGA_DIRECT_TOP_ROWS 96
#endif
// v0.8: protect the central hand-off between the top VBlank band and the
// behind-beam direct-video writer. Reuse the EXISTING PSRAM UI framebuffer:
// no additional DMA/internal framebuffer, no mapper- or ROM-specific patches.
// Production PlatformIO builds select 48 more rows (96..143). A zero value
// reverts exactly to the approved v0.1.17 presentation path.
#ifndef NES_VGA_DIRECT_MIDDLE_ROWS
#define NES_VGA_DIRECT_MIDDLE_ROWS 0
#endif
#ifndef NES_VGA_COHERENT_COMMIT
#define NES_VGA_COHERENT_COMMIT 0
#endif
#if NES_VGA_COHERENT_COMMIT
static_assert(NES_VGA_DIRECT_TOP_ROWS + NES_VGA_DIRECT_MIDDLE_ROWS == 240,
              "Coherent mode must stage ALL 240 lines before VGA display");
#endif
static_assert(NES_VGA_DIRECT_TOP_ROWS >= 0 && NES_VGA_DIRECT_TOP_ROWS <= 240,
              "NES top-band row count must be within the 240p image");
static_assert(NES_VGA_DIRECT_MIDDLE_ROWS >= 0 &&
              NES_VGA_DIRECT_MIDDLE_ROWS <= 240-NES_VGA_DIRECT_TOP_ROWS,
              "NES central VBlank stage must fit the 240p image");
static uint8_t* direct_top_stage = nullptr;
static volatile bool direct_top_ready = false;
static int direct_top_first_row = 0;
static int direct_top_x_offset = 32;
static volatile uint32_t direct_top_missed = 0;
static volatile uint32_t direct_top_presented = 0;
static uint32_t direct_top_max_copy_us = 0;
#if NES_VGA_DIRECT_MIDDLE_ROWS > 0
// A slow PSRAM or an unusual monitor timing must never force a VBlank
// overrun. If the extended commit becomes unsafe, use the approved 0.1.17
// direct writer instead. The DRAM top band always remains operational.
static bool direct_middle_enabled = true;
static uint8_t direct_middle_slow_count = 0;
static uint32_t direct_middle_copy_budget_us = 700;
#endif
// NES v0.1.15: the FPS overlay formerly used the asynchronous PSRAM
// worker for the FIRST 16 rows.  That worker wakes one FreeRTOS tick after
// EOF, sometimes during active scanout; this was the remaining top 10% tear.
// Composite the OSD into the existing 24KiB top band instead.  Only the
// 32-pixel LEFT border of the centered (F4 off) image needs extra storage.
static constexpr int NES_DIRECT_OSD_ROWS = 16;
static constexpr int NES_DIRECT_LEFT_BORDER = 32;
static uint8_t direct_top_left[NES_DIRECT_OSD_ROWS * NES_DIRECT_LEFT_BORDER];
// The x=31/x=288 single-pixel guards in v0.9 caused visible vertical rails.
// Clear the COMPLETE 32-pixel margins on the first frame after menu/F4 and
// preserve the NES 256-pixel viewport unmodified. This is NOT per-frame work.
static bool direct_margin_needs_clear = true;
static int direct_previous_x_offset = -1;
static volatile uint32_t direct_coherent_late = 0;


bool vga_direct_top_enabled() { return direct_top_stage != nullptr; }

// Returns a write-only staging row.  Its storage is never exposed to DMA,
// so PPU task 0 may render into it without waiting for the beam.
uint8_t* vga_direct_top_row(int row) {
    if (!direct_top_stage || row < 0 || row >= NES_VGA_DIRECT_TOP_ROWS)
        return nullptr;
    return direct_top_stage + (size_t)row * 256u;
}

// Middle rows render to the existing PSRAM stage; only the VBlank presenter
// copies them to the visible DMA framebuffer. Keep this separate from the
// top DRAM stage to retain its fast internal-RAM conversion and OSD handling.
uint8_t* vga_direct_middle_row(int row, int x_offset) {
#if NES_VGA_DIRECT_MIDDLE_ROWS > 0
    if (!direct_middle_enabled || !direct_top_stage || !framebuffer[back_fb] ||
        row < NES_VGA_DIRECT_TOP_ROWS ||
        row >= NES_VGA_DIRECT_TOP_ROWS + NES_VGA_DIRECT_MIDDLE_ROWS ||
        x_offset < 0 || x_offset + 256 > fb_pitch)
        return nullptr;
    return framebuffer[back_fb][row] + x_offset;
#else
    (void)row; (void)x_offset;
    return nullptr;
#endif
}

// Called after ppu_async_drain(), NEVER while producer task is writing.
void vga_direct_top_ready(int first_row, int x_offset) {
    if (!direct_top_stage) return;
    direct_top_first_row = first_row < 0 ? 0 : first_row;
    direct_top_x_offset = x_offset;
    if (x_offset != direct_previous_x_offset) {
        direct_margin_needs_clear = true;
        direct_previous_x_offset = x_offset;
    }
    direct_top_ready = true;
}

void vga_direct_top_discard() {
    direct_top_ready = false;
    direct_margin_needs_clear = true; // menu -> jogo limpa 32px completos
}

// Called only AFTER the PPU worker has completed and the FPS overlay has
// been drawn into the current PSRAM stage, BEFORE vga_direct_top_ready().
// The game in rows 0..95 is already staged: copy just the OSD rectangle
// into that image, preserving the game in all other pixels.  The RGB222
// bytes are already in the physical VGA byte order (x XOR 2); use an aligned
// [0,196) span to include all bytes of fillRect(1,1,194,11).
void vga_direct_top_compose_osd(bool enabled, int x_offset) {
    if (!direct_top_stage) return;
    const uint8_t blank = sync_bits();
    memset(direct_top_left, blank, sizeof(direct_top_left));
    if (!enabled || (x_offset != 0 && x_offset != 32) || !framebuffer[back_fb])
        return;
    // The overlay uses y=1..11 and x=1..194.  Copying a 4-byte-aligned
    // span handles the I2S byte-swizzle without a pixel conversion loop.
    const int left = x_offset;
    const int right = x_offset + 256;
    const int copy_from = left > 0 ? left : 0;
    // O OSD em F4 e deslocado SIDE pixels para nao ser recortado.
    const int osd_right = 196 + ((x_offset == 0) ? NES_VGA_OVERSCAN_SIDE : 0);
    const int copy_to = right < osd_right ? right : osd_right;
    for (int y = 1; y < 12 && y < NES_VGA_DIRECT_TOP_ROWS; ++y) {
        const uint8_t* const src = framebuffer[back_fb][y];
        if (!src) continue;
        if (copy_to > copy_from)
            memcpy(direct_top_stage + (size_t)y * 256u + (copy_from - left),
                   src + copy_from, (size_t)(copy_to - copy_from));
        if (x_offset == NES_DIRECT_LEFT_BORDER)
            memcpy(direct_top_left + (size_t)y * NES_DIRECT_LEFT_BORDER,
                   src, NES_DIRECT_LEFT_BORDER);
    }
}

// Called by loop() exactly once per new I2S EOF, before starting another
// emulated frame.  96 * 256 = 24,576 bytes in internal RAM; a short copy
// during VBlank now includes the FPS overlay and left border.  NO worker
// is allowed to write to any scanout row while a direct NES frame is active.
void vga_direct_top_present() {
    if (!direct_top_ready || !direct_top_stage || !framebuffer[0]) return;
    const uint32_t age_us = vga_us_since_vsync();
    const uint32_t vblank_lines = (uint32_t)(v_front + v_sync + v_back);
    const uint32_t vblank_us = (uint32_t)(((uint64_t)vblank_lines *
        (uint64_t)(orig_h_front + orig_h_sync + orig_h_back + orig_h_pixels) *
        1000000ULL) / (uint64_t)(pixel_clock > 0 ? pixel_clock : 12587500));
    // v0.8: the middle stage adds at most 16 KiB of PSRAM->DMA copies.
    // Reserve enough blanking time for the complete transaction BEFORE
    // touching a single displayed line. This avoids presenting the upper
    // half from two different frames if an EOF arrives late.
    // The previous copy duration supplies an adaptive budget; the initial
    // estimate is conservative to avoid overrunning a short VBlank.
#if NES_VGA_COHERENT_COMMIT
    // Only first 96 rows must be transferred during VBlank. Remaining rows
    // may be copied ahead of the beam (deadline grows with scanline index).
    const uint32_t copy_guard_us = 500u;
#elif NES_VGA_DIRECT_MIDDLE_ROWS > 0
    const uint32_t copy_guard_us = direct_middle_enabled ?
        (direct_middle_copy_budget_us + 180u) : 350u;
#else
    const uint32_t copy_guard_us = 350u;
#endif
    if (age_us == 0xffffffffu || age_us + copy_guard_us >= vblank_us) {
        ++direct_top_missed;
        direct_top_ready = false;
        return;
    }
    const int start = direct_top_first_row < NES_VGA_DIRECT_TOP_ROWS
                        ? direct_top_first_row : NES_VGA_DIRECT_TOP_ROWS;
    const int x = direct_top_x_offset;
    if (x < 0 || x + 256 > fb_pitch) { direct_top_ready = false; return; }
    const uint32_t begin_us = (uint32_t)esp_timer_get_time();
#if NES_VGA_COHERENT_COMMIT
    // Q8 microsecond physical line duration: avoid accumulating truncation
    // (31us * 478 scanlines would otherwise lose ~360us at frame bottom).
    const uint32_t physical_line_us_q8 = (uint32_t)(
        ((uint64_t)(orig_h_front + orig_h_sync + orig_h_back + orig_h_pixels) *
          256000000ULL) / (uint64_t)(pixel_clock > 0 ? pixel_clock : 12587500));
    const uint32_t commit_epoch = vga_frame_sequence();
    bool coherent_complete = true;
#endif
    for (int row = start; row < NES_VGA_DIRECT_TOP_ROWS; ++row) {
        // Restore the black/OSD left margin as well, but only in the
        // centered 320x240 mode.  Avoid any extra copy on F4 fullscreen.
        // The 32-pixel side margins are blacked once after F1/F4 changes.
        // Their physical ranges are aligned to x^2, so memset is correct.
        if (direct_margin_needs_clear && x == NES_DIRECT_LEFT_BORDER) {
            memset(framebuffer[0][row], sync_bits(), NES_DIRECT_LEFT_BORDER);
            memset(framebuffer[0][row] + x + 256, sync_bits(), NES_DIRECT_LEFT_BORDER);
        }
        if (x == NES_DIRECT_LEFT_BORDER && row < NES_DIRECT_OSD_ROWS)
            memcpy(framebuffer[0][row],
                   direct_top_left + (size_t)row * NES_DIRECT_LEFT_BORDER,
                   NES_DIRECT_LEFT_BORDER);
        memcpy(framebuffer[0][row] + x,
               direct_top_stage + (size_t)row * 256u, 256u);
    }
#if NES_VGA_DIRECT_MIDDLE_ROWS > 0
    if (direct_middle_enabled && framebuffer[back_fb]) {
        // No game-specific banding: every mapper follows the same pipeline.
        // The central rows were finished by ppu_async_drain() before READY.
        for (int row = NES_VGA_DIRECT_TOP_ROWS;
             row < NES_VGA_DIRECT_TOP_ROWS + NES_VGA_DIRECT_MIDDLE_ROWS; ++row) {
#if NES_VGA_COHERENT_COMMIT
            // Coherent commit: all lines are from the SAME completed NES
            // frame, never from the active PPU producer. The entire lower
            // image is copied AHEAD of the beam, once top rows are ready.
            // The first PSRAM row is y=96 (~7.5ms after EOF); the copy
            // normally ends well before the active scan reaches it.
            const uint32_t age = vga_us_since_vsync();
            const uint32_t deadline = (uint32_t)(
                ((uint64_t)(vblank_lines + (uint32_t)row * (uint32_t)v_div) *
                  physical_line_us_q8) >> 8);
            const uint32_t guard_us = (3u * physical_line_us_q8 + 255u) >> 8;
            // If a catastrophic preemption made this row unsafe, do not
            // corrupt the active DMA scan. Count it for diagnosis. Unlike
            // v0.9, no per-scanline post-beam writes are permitted.
            if (age == 0xffffffffu ||
                vga_frame_sequence() != commit_epoch ||
                age + guard_us >= deadline) {
                ++direct_coherent_late;
                ++direct_top_missed;
                coherent_complete = false;
                break;
            }
#endif
            if (direct_margin_needs_clear && x == NES_DIRECT_LEFT_BORDER) {
                memset(framebuffer[0][row], sync_bits(), NES_DIRECT_LEFT_BORDER);
                memset(framebuffer[0][row] + x + 256, sync_bits(), NES_DIRECT_LEFT_BORDER);
            }
            memcpy(framebuffer[0][row] + x,
                   framebuffer[back_fb][row] + x, 256u);
        }
    }
#endif
#if NES_VGA_COHERENT_COMMIT
    if (!coherent_complete) {
        // A late commit is a real missed presentation, not a completed one.
        // Do not clear the margin-dirty flag for lines we have not touched.
        direct_top_ready = false;
        return;
    }
#endif
    if (x == NES_DIRECT_LEFT_BORDER) direct_margin_needs_clear = false;
    const uint32_t duration = (uint32_t)esp_timer_get_time() - begin_us;
    if (duration > direct_top_max_copy_us) direct_top_max_copy_us = duration;
#if NES_VGA_DIRECT_MIDDLE_ROWS > 0 && !NES_VGA_COHERENT_COMMIT
    if (direct_middle_enabled) {
        // Adapt the next frame's VBlank reservation to measured wall time.
        // A transient single-frame stall should not permanently disable it.
        if (duration > direct_middle_copy_budget_us)
            direct_middle_copy_budget_us = duration;
        else
            direct_middle_copy_budget_us = (7u * direct_middle_copy_budget_us + duration + 4u) / 8u;
        if (duration + 250u >= vblank_us) {
            if (++direct_middle_slow_count >= 2) {
                direct_middle_enabled = false; // proven v0.1.17 lower-row path
            }
        } else {
            direct_middle_slow_count = 0;
        }
    }
#endif
    ++direct_top_presented;
    direct_top_ready = false;
}

void vga_direct_top_stats(uint32_t* presented, uint32_t* missed,
                          uint32_t* max_copy_us) {
    if (presented) *presented = direct_top_presented;
    if (missed) *missed = direct_top_missed;
    if (max_copy_us) *max_copy_us = direct_top_max_copy_us;
    direct_top_presented = direct_top_missed = direct_top_max_copy_us = 0;
}

// Direct-scanout rows are NOT dirty PSRAM rows: a pending full-stage
// invalidation from a menu must not overwrite the latest DMA image.
void vga_clear_dirty_rows(int first, int count) {
    if (first < 0) { count += first; first = 0; }
    if (count <= 0 || first >= y_res()) return;
    const int end = first + count < y_res() ? first + count : y_res();
    memset(dirty_rows + first, 0, (size_t)(end - first));
}

// ==================== DIRECT SCANOUT ROWS (NES v0.1.12) ====================
// O emulador pode gravar uma linha pronta direto no framebuffer DMA exibido
// (RAM interna), sem passar pelo stage em PSRAM nem pela copia stage->DMA.
// Para nao rasgar a imagem, a linha N so e gravada depois que o feixe ja
// varreu a linha N na varredura ATUAL ("atras do feixe"). Se um quadro novo
// comecar antes dessa linha ficar pronta, nao gravar a linha a frente do feixe:
// a espera e recalculada contra o novo EOF, sem o bypass perigoso da v0.1.12.
// Modelo do feixe: a interrupcao EOF do DMA ocorre no fim da ultima linha
// ativa; seguem v_front + v_sync + v_back linhas de apagamento e depois as
// linhas ativas (v_div linhas de video por linha do framebuffer).
#ifndef NES_VGA_DIRECT_GUARD_LINES
#define NES_VGA_DIRECT_GUARD_LINES 1     // linhas de video de folga apos a linha
#endif
static volatile uint32_t direct_wait_cc = 0;
static volatile uint32_t direct_frame_epoch = 0; // epoch do inicio deste quadro NES

void vga_direct_mark_frame_start() {
    // The first 96 rows may be handled by the staged band, so no line calls
    // vga_direct_wait_row(first_row=true) until row 96.  Mark the start NOW,
    // or the old epoch would incorrectly enable the ahead-of-beam bypass.
    direct_frame_epoch = (uint32_t)vga_frame_count;
}

uint8_t* vga_direct_row(int row) {
    if (!framebuffer[0] || hw_blend_active || row < 0 || row >= y_res()) return nullptr;
    return framebuffer[0][row];
}

void vga_direct_clean_side_guards(int row, int x_offset) {
    if (x_offset != NES_DIRECT_LEFT_BORDER || !framebuffer[0] ||
        row < 0 || row >= y_res() || x_offset + 256 >= fb_pitch) return;
    // v0.10: intentionally no-op. Narrow dark rails at x=31/288 from
    // the previous side-guard implementation were visible on CRT/VGA.
    // Full margins are cleared atomically on menu/F4 transition instead.
}

bool vga_any_dirty_rows() {
    for (int y = 0; y < y_res(); ++y) if (dirty_rows[y]) return true;
    return false;
}

// Microssegundos gastos esperando o feixe desde a ultima chamada (diagnostico).
uint32_t vga_direct_take_wait_us() {
    const uint32_t mhz = (uint32_t)getCpuFrequencyMhz();
    const uint32_t us = direct_wait_cc / (mhz ? mhz : 240u);
    direct_wait_cc = 0;
    return us;
}

SMS_HOT void vga_direct_wait_row(int row, bool first_row_of_frame) {
    if (!vsync_seen) return;
    if (first_row_of_frame) direct_frame_epoch = (uint32_t)vga_frame_count;
    if (!direct_line_cc) {
        const uint64_t hz = (uint64_t)getCpuFrequencyMhz() * 1000000ULL;
        const uint64_t htotal = (uint64_t)(orig_h_front + orig_h_sync + orig_h_back + orig_h_pixels);
        if (pixel_clock <= 0) return;
        direct_line_cc = (uint32_t)((hz * htotal) / (uint64_t)pixel_clock);
        if (!direct_line_cc) return;
    }
    const uint32_t lines = (uint32_t)(v_front + v_sync + v_back) +
                           (uint32_t)(row + 1) * (uint32_t)v_div +
                           (uint32_t)NES_VGA_DIRECT_GUARD_LINES;
    const uint32_t target = lines * direct_line_cc;
    // A ultima linha de imagem (239) termina junto com o EOF: seu target
    // (+1 guard line) ultrapassa o total do quadro VGA. A espera precisa
    // permitir a escrita da linha do frame NES anterior *antes* de o novo
    // feixe alcanca-la; caso contrario haveria um loop infinito na linha 239.
    const uint32_t ahead_guard = direct_line_cc; // nao escrever junto com DMA
    uint32_t entry_cc;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(entry_cc));
    for (;;) {
        // O ISR atualiza epoch e ccount no fim de cada quadro. Revalidar
        // ambos antes de liberar a escrita: quando ha rollover, esperar a
        // linha do NOVO quadro em vez de gravar adiantado (tearing vertical).
        const uint32_t epoch = (uint32_t)vga_frame_count;
        const uint32_t epoch_cc = vsync_ccount;
        uint32_t cc;
        __asm__ __volatile__("rsr %0, ccount" : "=a"(cc));
        if (epoch != (uint32_t)vga_frame_count) continue;
        const uint32_t elapsed = cc - epoch_cc;
        // v0.9: a permissao generica de escrita ANTES do feixe causava
        // misturar o quadro novo com o topo ainda do quadro anterior.
        // Durante scrolling vertical/diagonal isso aparece como tear na
        // fronteira de um ou varios tiles. Todas as linhas normais devem
        // ser escritas DEPOIS do feixe e exibidas na proxima passagem.
        // Excecao fisica: linha 239 termina exatamente no EOF. Ela pode
        // ser gravada no VBlank seguinte, depois da ultima varredura.
        const uint32_t blank_end = (uint32_t)(v_front + v_sync + v_back) * direct_line_cc;
        const bool last_row_in_new_blank =
            (row == y_res() - 1 && epoch != direct_frame_epoch &&
             blank_end > 2u * ahead_guard && elapsed + 2u * ahead_guard < blank_end);
        if (last_row_in_new_blank || elapsed >= target) {
            if (epoch == (uint32_t)vga_frame_count) break;
            continue;
        }
        if (target - elapsed > 288000u) vTaskDelay(1);
    }
    uint32_t exit_cc;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(exit_cc));
    direct_wait_cc = direct_wait_cc + (exit_cc - entry_cc);
}

// Recheck immediately before the DMA row memcpy (handles task preemption).
// v0.9: 'before the beam' is NOT sufficient: a row from the NEXT NES frame
// would become visible below the VBlank-staged top from the PREVIOUS frame.
// Require post-beam, except at the last line during the new vertical blank.
// This protection is global (all games/mappers) and costs one cycle-counter
// sample only for the unbuffered bottom rows.
bool vga_direct_row_copy_safe(int row) {
    if (!vsync_seen || !direct_line_cc) return true;
    if (row < 0 || row >= y_res()) return false;
    const uint32_t epoch = (uint32_t)vga_frame_count;
    const uint32_t epoch_cc = vsync_ccount;
    uint32_t now_cc;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(now_cc));
    if (epoch != (uint32_t)vga_frame_count) return false;
    const uint32_t elapsed = now_cc - epoch_cc;
    const uint32_t row_end =
        ((uint32_t)(v_front + v_sync + v_back) +
         ((uint32_t)row + 1u) * (uint32_t)v_div) * direct_line_cc;
    const uint32_t guard = (uint32_t)NES_VGA_DIRECT_GUARD_LINES * direct_line_cc;
    // v0.9: NUNCA devolver true antes do feixe em uma linha normal.
    // Isso gerava uma faixa de um frame futuro sob o topo ainda antigo.
    // A ultima linha pode ser entregue apos EOF, dentro do VBlank seguro.
    const uint32_t blank_end = (uint32_t)(v_front + v_sync + v_back) * direct_line_cc;
    const bool tail_in_blank = (row == y_res() - 1 &&
           epoch != direct_frame_epoch && blank_end > 2u * direct_line_cc &&
           elapsed + 2u * direct_line_cc < blank_end);
    return tail_in_blank || (elapsed >= row_end + guard);
}

// ==================== PUBLIC API ====================

// Initialize VGA with specified pin mapping, mode, and double buffering
bool vga_init(const unsigned char* pin_map, const int* mode, bool double_buffered) {
    h_front = mode[0]; h_sync = mode[1]; h_back = mode[2]; h_pixels = mode[3];
    fb_pitch = h_pixels;
    orig_h_front = h_front; orig_h_sync = h_sync; orig_h_back = h_back; orig_h_pixels = h_pixels;
    v_front = mode[4]; v_sync = mode[5]; v_back = mode[6]; v_pixels = mode[7];
    v_div = mode[8]; pixel_clock = mode[9]; v_polarity = mode[10]; h_polarity = mode[11];
    
    (void)double_buffered;  // Always keep an independent render image.
    num_fbs = 2;
    framebuffer[0] = alloc_framebuffer(0); // internal DMA scanout
    framebuffer[1] = alloc_psram_render_framebuffer(1); // PSRAM working image
    if (!framebuffer[0] || !framebuffer[1]) {
        Serial.println("[VGA] FATAL: DMA scanout or PSRAM staging unavailable");
        return false;
    }
    active_fb = 0;
    back_fb = 1;
    allocate_vga_i2s_buffers();
    if (!dma_buf_desc || !dma_buf_hblank_vnorm || !dma_buf_hblank_vsync ||
        !dma_buf_vblank_vnorm || !dma_buf_vblank_vsync) {
        Serial.println("[VGA] FATAL: DMA descriptors/blanking unavailable");
        return false;
    }
    memset(dirty_rows, 1, sizeof dirty_rows);
    set_active_framebuffer(framebuffer[active_fb]);
    setup_i2s_output(pin_map);
    start_i2s_output();
    vga_async_try_init();
    // Only after allocating DMA descriptors and both video workers: do not
    // sacrifice a working VGA init or core-0 stack for this optional band.
    // 24 KiB staging band in internal RAM; does not change the existing
    // 76.8 KiB DMA framebuffer or the PSRAM worker and menu framebuffers.
    if (!direct_top_stage && NES_VGA_DIRECT_TOP_ROWS > 0) {
        const size_t bytes = (size_t)NES_VGA_DIRECT_TOP_ROWS * 256u;
        // Avoid depleting internal heap for PPU tasks, IRQs, ESP32 stacks.
        const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (free_bytes >= bytes + 48u * 1024u && largest >= bytes) {
            direct_top_stage = (uint8_t*)heap_caps_malloc(
                bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (direct_top_stage) {
            memset(direct_top_stage, 0, (size_t)NES_VGA_DIRECT_TOP_ROWS * 256u);
        } else {
            Serial.println("[NES][VGA] Top-band stage indisponivel; usando video direto original");
        }
    }

    if (direct_top_stage) {
        Serial.printf("[NES][VGA] VBlank top-band ativo: %u linhas, %u bytes DRAM\n",
                      (unsigned)NES_VGA_DIRECT_TOP_ROWS,
                      (unsigned)(NES_VGA_DIRECT_TOP_ROWS * 256u));
#if NES_VGA_DIRECT_MIDDLE_ROWS > 0
        Serial.printf("[NES][VGA] VBlank central: linhas %u..%u, %u bytes PSRAM existente\n",
                      (unsigned)NES_VGA_DIRECT_TOP_ROWS,
                      (unsigned)(NES_VGA_DIRECT_TOP_ROWS + NES_VGA_DIRECT_MIDDLE_ROWS - 1),
                      (unsigned)(NES_VGA_DIRECT_MIDDLE_ROWS * 256u));
#endif
    }
    Serial.printf("[VGA] Tear-free staging in PSRAM: %u bytes, DMA scanout in internal RAM\n",
                  (unsigned)(x_res() * y_res()));
    return true;
}

// Swap front and back buffers (optionally wait for vsync)
// A tarefa de video do MC-1000 e o renderer do TK-2000 evitam atualizar
// regioes inalteradas. Mantemos a copia segura em VSync da v0.1.1, mas so
// enviamos as linhas modificadas (jogo 192/240; OSD 7; menus todas).
void vga_mark_rows(int first, int count) {
    if (first < 0) { count += first; first = 0; }
    if (count <= 0 || first >= y_res()) return;
    const int end = first + count < y_res() ? first + count : y_res();
    for (int y = first; y < end; ++y) dirty_rows[y] = 1;
}

uint32_t vga_last_copy_us() { return last_copy_us; }
uint32_t vga_last_vsync_us() { return last_vsync_us; }

bool vga_swap_buffers(bool wait_vsync) {
    vga_line_ring_drain();
    if (!framebuffer_linear[0] || !framebuffer_linear[back_fb]) return false;
    bool any_dirty = false;
    for (int y = 0; y < y_res(); ++y) if (dirty_rows[y]) { any_dirty = true; break; }
    if (!any_dirty) { last_copy_us = last_vsync_us = 0; return true; }
    const int64_t wait_begin = esp_timer_get_time();
    if (wait_vsync) {
        const int current_frame = vga_frame_count;
        const int64_t deadline = wait_begin + 20000LL;
        while (vga_frame_count == current_frame) {
            if (esp_timer_get_time() >= deadline) {
                last_copy_us = 0;
                last_vsync_us = (uint32_t)(esp_timer_get_time() - wait_begin);
                return false;
            }
            taskYIELD();
        }
    }
    const int64_t copy_begin = esp_timer_get_time();
    last_vsync_us = (uint32_t)(copy_begin - wait_begin);
    // Agrupar faixas contiguas reduz chamadas memcpy e leituras lentas da
    // PSRAM. A imagem exibida permanece na RAM DMA interna.
    // O browser pode abrir quando o stage de desenho for o indice 2.
    copy_dirty_rows_for_display(framebuffer_linear[0], framebuffer_linear[back_fb], dirty_rows);
    last_copy_us = (uint32_t)(esp_timer_get_time() - copy_begin);
    return true;
}

// Clear back buffer with specified color
void vga_clear_screen(uint8_t color) {
    if (!framebuffer_linear[back_fb]) return;
    unsigned char clear_byte = sync_bits() | (color & 0x3f);
    memset(framebuffer_linear[back_fb], clear_byte, x_res() * y_res());
    vga_mark_rows(0, y_res());
}

// Get pointer to current back buffer
unsigned char** vga_get_framebuffer() {
    return framebuffer[back_fb];
}

// Get linear framebuffer pointer
uint8_t* vga_get_raw_framebuffer() {
    return framebuffer_linear[back_fb];
}

// Get current sync bits value
unsigned char vga_get_sync_bits() {
    return sync_bits();
}

// Get horizontal resolution
int vga_get_xres() {
    return x_res();
}

// Get vertical resolution
int vga_get_yres() {
    return y_res();
}

// Set buffer update flag
void vga_set_buffer_needs_update(bool update) {
    buffer_needs_update = update;
}