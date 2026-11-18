// vga_6bit.h
#ifndef VGA_6BIT_H_FILE
#define VGA_6BIT_H_FILE

#include <cstdint>
#include <Arduino.h>

// Available VGA modes
extern const int VgaMode_vga_mode_320x240[12];
extern const int VgaMode_vga_mode_320x200[12];
extern const int VgaMode_vga_mode_256x240[12];

// Main VGA functions
bool vga_init(const unsigned char *pin_map, const int *mode, bool double_buffered);
bool vga_swap_buffers(bool wait_vsync = true);
// Tela cheia SMS por hardware (v0.1.37/38): clock de pixel mais lento, 256
// amostras ativas por linha, mesmo tempo de linha, e o DMA repete linhas do
// framebuffer (ver SMS_HW_H_SAMPLES / SMS_HW_V_LINES em config.h). So vale a
// partir do modo 320x240. Chamar com a emulacao parada.
// profile: 0 = 320x240 normal, 1 = tela cheia esticada.
bool vga_set_hw_stretch(int profile);
bool vga_hw_stretch_active();
int vga_hw_stretch_profile();
bool vga_async_available();
bool vga_async_submit();
void vga_async_wait_idle();
void vga_async_clear_stages(uint8_t value);
uint32_t vga_async_wait_us();
void vga_clear_screen(uint8_t color);
unsigned char **vga_get_framebuffer();
uint8_t* vga_get_raw_framebuffer();
unsigned char vga_get_sync_bits();
int vga_get_xres();
int vga_get_yres();
void vga_set_buffer_needs_update(bool update);
void vga_mark_rows(int first, int count);
uint32_t vga_last_copy_us();
uint32_t vga_last_vsync_us();
// v0.1.39: idade (us) do ultimo fim de quadro VGA; 0xFFFFFFFF se nenhum.
uint32_t vga_us_since_vsync();
// Atualizado somente no EOF do I2S (um incremento por quadro VGA).
uint32_t vga_frame_sequence();

// v0.1.34: anel de linhas VGA -> PSRAM atendido por tarefa no core 0.
// start/stop: durante a emulacao (alocam/liberam). acquire devolve 64
// palavras (256 B) para empacotar a linha ou NULL se o anel nao estiver
// ativo (usar caminho direto). publish enfileira; drain espera esvaziar.
bool vga_line_ring_start();
void vga_line_ring_stop();
bool vga_line_ring_active();
uint32_t vga_line_ring_stalls();   // vezes que o core 1 esperou o core 0
uint32_t* vga_line_ring_acquire();
void vga_line_ring_publish(uint8_t* dst, uint32_t len, bool flush);
void vga_line_ring_drain();

// NES v0.1.12: linha gravada direto no framebuffer DMA exibido, atras do feixe.
// vga_direct_row: ponteiro da linha exibida (320 bytes) ou NULL se indisponivel.
// vga_direct_wait_row: espera o feixe passar pela linha (ver vga_6bit.cpp).
uint8_t* vga_direct_row(int row);
void vga_direct_wait_row(int row, bool first_row_of_frame);
void vga_direct_mark_frame_start();
bool vga_any_dirty_rows();
uint32_t vga_direct_take_wait_us();
// v0.1.14: upper-screen anti-tearing, 24KiB DRAM staged to DMA on VGA EOF.
bool vga_direct_top_enabled();
uint8_t* vga_direct_top_row(int row);
uint8_t* vga_direct_middle_row(int row, int x_offset);
bool vga_direct_row_copy_safe(int row);
void vga_direct_clean_side_guards(int row, int x_offset);
void vga_direct_top_compose_osd(bool enabled, int x_offset);
void vga_direct_top_ready(int first_row, int x_offset);
void vga_direct_top_present();
void vga_direct_top_discard();
void vga_direct_top_stats(uint32_t* presented, uint32_t* missed,
                          uint32_t* max_copy_us);
void vga_clear_dirty_rows(int first, int count);

// 6-bit VGA color definitions for TTGO VGA32
#define VGA_COLOR_RED     0x03
#define VGA_COLOR_GREEN   0x0C
#define VGA_COLOR_BLUE    0x30
#define VGA_COLOR_WHITE   0x3F
#define VGA_COLOR_BLACK   0x00
#define VGA_COLOR_YELLOW  0x0F
#define VGA_COLOR_CYAN    0x3C
#define VGA_COLOR_MAGENTA 0x33

#endif