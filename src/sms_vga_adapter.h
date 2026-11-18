// sms_vga_adapter.h
#ifndef SMS_VGA_ADAPTER_H
#define SMS_VGA_ADAPTER_H

#include <Arduino.h>
#include <stdint.h>
#include "vga_6bit.h"
#include "../config.h"

// RGB565 color definitions
#define VGA_COLOR_BLACK_RGB565   0x0000
#define VGA_COLOR_WHITE_RGB565   0xFFFF
#define VGA_COLOR_RED_RGB565     0xF800
#define VGA_COLOR_GREEN_RGB565   0x07E0
#define VGA_COLOR_BLUE_RGB565    0x001F
#define VGA_COLOR_YELLOW_RGB565  0xFFE0
#define VGA_COLOR_CYAN_RGB565    0x07FF
#define VGA_COLOR_MAGENTA_RGB565 0xF81F
#define VGA_COLOR_GRAY_RGB565    0x8410

// UI Colors
#define UI_COLOR_BLACK      0x0000
#define UI_COLOR_WHITE      0xFFFF
#define UI_COLOR_RED        0xF800
#define UI_COLOR_GREEN      0x07E0
#define UI_COLOR_BLUE       0x001F
#define UI_COLOR_YELLOW     0xFFE0
#define UI_COLOR_CYAN       0x07FF
#define UI_COLOR_PURPLE     0x801F
#define UI_COLOR_GRAY       0x8410

// UI Scheme
#define BG_COLOR           UI_COLOR_BLACK
#define BAR_COLOR          UI_COLOR_BLUE
#define BAR_TEXT_COLOR     UI_COLOR_WHITE
#define TEXT_COLOR         UI_COLOR_WHITE
#define SELECTED_TEXT_COLOR UI_COLOR_BLACK
#define SELECTED_BG_COLOR  UI_COLOR_YELLOW
#define BOX_BORDER_COLOR   UI_COLOR_CYAN

class SMSVGADisplay {
public:
    // Initialization
    static void init();
    static bool ready();
    static void fillScreen(uint16_t color);
    static void fillRect(int x, int y, int w, int h, uint16_t color);
    static void drawRect(int x, int y, int w, int h, uint16_t color);
    
    // Text rendering
    static void drawString(const char* text, int x, int y, int font = 0);
    // Fonte TK-2000 5x7 (passo 6px), exclusiva do navegador F2.
    static void drawBrowserSerif(const char* text, int x, int y);
    static int browserSerifWidth(const char* text);
    static void drawChar(char c, int x, int y, uint8_t color, uint8_t backcolor);
    static void setTextColor(uint16_t fg, uint16_t bg = VGA_COLOR_BLACK_RGB565);
    static void setCursor(int x, int y);
    static void print(const char* text);
    static void print(char c);
    static int textWidth(const char* text);
    
    // Display properties
    static int getWidth();
    static int getHeight();
    
    // Emulation mode
    static void beginEmulation();
    static void endEmulation();
    // v0.1.38: tela cheia SMS por hardware. setFullscreen() registra o desejo
    // (vale no proximo beginEmulation). Ativa: varredura esticada (clock de
    // pixel mais lento + DMA repetindo linhas), jogo em x=0,y=0 com o mesmo
    // empacotamento 1:1; sem escala por software. Menus voltam ao 320x240.
    // mode: 0 = 1:1 com bordas, 1 = tela cheia.
    static void setFullscreen(uint8_t mode);
    static bool fullscreenActive();
    static uint8_t fullscreenMode();
    static void updateDisplay();
    
    // SMS-specific drawing
    static void drawScanline(int y, uint8_t* pixels, int width);
    static void updateColorTable(uint8_t* cramd);
    
    // Low-level access
    static void fastDrawPixel(int x, int y, uint8_t color);
    static void setScreenOffset(int offset);
    static int getScreenOffset();
    static uint8_t* getRawFramebuffer();
    static uint8_t** getFramebuffer();
    static uint8_t getSyncBits();
    
    // State flags
    static bool bufferNeedsUpdate;
    static bool emulationMode;
    
private:
    static uint8_t rgb565_to_vga6(uint16_t rgb565);
    static void initColorTable();
    
    static uint8_t** framebuffer_ptr;
    static uint8_t* framebuffer_linear;
    static uint8_t sync_bits_val;
    static int cursorX, cursorY;
    static uint8_t textColor, bgColor;
    static bool initialized;
#if SMS_VGA_BATCH_DIRTY_ROWS
    static int scanline_dirty_first;
    static int scanline_dirty_last;
    static void flushScanlineDirtyRows();
#endif
    static int screen_x_offset;
#if SMS_FULLSCREEN
    static uint8_t fullscreen_want;
    static uint8_t fullscreen_mode;
    static bool fullscreen_active;
    static void applyFullscreen(uint8_t mode);
#endif
#if SMS_VGA_CACHE_GEOMETRY
    static int cached_vga_width;
    static int cached_vga_height;
    static int cached_sms_yoffset;
#endif
    static uint8_t color_table[128];
    static uint8_t sms_color_table[32];
#if !SMS_VGA_COMPACT_LUT
    static uint16_t sms_palette_pairs[32 * 32];
#endif
#if SMS_VGA_SKIP_UNCHANGED_ROWS
    // Duas identidades de stages PSRAM, sem escrever nada no worker VGA.
    static const uint8_t* row_stage_base[2];
    static uint32_t row_stage_hash[2][240];
    static uint8_t row_stage_valid[2][240];
    static unsigned row_stage_next_slot;
#endif
};

// Funciones puente para C
#ifdef __cplusplus
extern "C" {
#endif
void vga_draw_scanline(void* display, int line, uint8_t* pixels, int width);
void vga_update_palette(void* display, uint8_t* cramd);
#ifdef __cplusplus
}
#endif

#endif