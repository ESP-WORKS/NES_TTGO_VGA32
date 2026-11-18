// sms_vga_adapter.cpp
#include "sms_vga_adapter.h"
#include "vga_6bit.h"
#include "gb_sdl_font8x8.h"
#include "sms_browser_tk2000_font.h"
#include "sms_scanline_fast.h"
#include "sms_core/sms_perf_detail.h"
#include "sms_core/sms_iram.h"
#if SMS_VGA_LINE_RING
extern "C" int vp_vend;   // render.c: fim do viewport (192 SMS / 168 GG)
#endif
#if SMS_DETAIL_ACTIVE
#include "esp_timer.h"
#endif

// Static variable definitions
uint8_t** SMSVGADisplay::framebuffer_ptr = nullptr;
uint8_t* SMSVGADisplay::framebuffer_linear = nullptr;
uint8_t SMSVGADisplay::sync_bits_val = 0;
int SMSVGADisplay::cursorX = 0;
int SMSVGADisplay::cursorY = 0;
uint8_t SMSVGADisplay::textColor = VGA_COLOR_WHITE;
uint8_t SMSVGADisplay::bgColor = VGA_COLOR_BLACK;
bool SMSVGADisplay::initialized = false;
#if SMS_VGA_BATCH_DIRTY_ROWS
int SMSVGADisplay::scanline_dirty_first = -1;
int SMSVGADisplay::scanline_dirty_last = -1;

// Os stages PSRAM so sao apresentados apos sms_frame(). Nenhum worker
// le o stage atual enquanto suas linhas estao sendo desenhadas.
void SMSVGADisplay::flushScanlineDirtyRows() {
    if (scanline_dirty_first >= 0) {
        vga_mark_rows(scanline_dirty_first,
                      scanline_dirty_last - scanline_dirty_first + 1);
        scanline_dirty_first = scanline_dirty_last = -1;
    }
}
#endif
bool SMSVGADisplay::emulationMode = false;
bool SMSVGADisplay::bufferNeedsUpdate = true;
int SMSVGADisplay::screen_x_offset = 0;
#if SMS_FULLSCREEN
uint8_t SMSVGADisplay::fullscreen_want = 0;
uint8_t SMSVGADisplay::fullscreen_mode = 0;
bool SMSVGADisplay::fullscreen_active = false;
#endif
#if SMS_VGA_CACHE_GEOMETRY
int SMSVGADisplay::cached_vga_width = 0;
int SMSVGADisplay::cached_vga_height = 0;
int SMSVGADisplay::cached_sms_yoffset = 0;
#endif
uint8_t SMSVGADisplay::color_table[128];
uint8_t SMSVGADisplay::sms_color_table[32];
#if !SMS_VGA_COMPACT_LUT
uint16_t SMSVGADisplay::sms_palette_pairs[32 * 32];
#endif
#if SMS_VGA_SKIP_UNCHANGED_ROWS
const uint8_t* SMSVGADisplay::row_stage_base[2] = {nullptr, nullptr};
uint32_t SMSVGADisplay::row_stage_hash[2][240] = {};
uint8_t SMSVGADisplay::row_stage_valid[2][240] = {};
unsigned SMSVGADisplay::row_stage_next_slot = 0;
#endif

// Convert RGB565 color to 6-bit VGA format
uint8_t SMSVGADisplay::rgb565_to_vga6(uint16_t rgb565) {
    uint8_t r = (rgb565 >> 8) & 0xF8;
    uint8_t g = (rgb565 >> 3) & 0xFC;
    uint8_t b = (rgb565 << 3) & 0xF8;
    
    uint8_t r2 = r >> 6;
    uint8_t g2 = g >> 6;
    uint8_t b2 = b >> 6;
    
    return (b2 << 4) | (g2 << 2) | r2;
}

// Initialize color lookup table
void SMSVGADisplay::initColorTable() {
    for (int i = 0; i < 128; i++) {
        color_table[i] = sync_bits_val | (i & 0x3F);
    }
    
    // Initial VDP CRAM is black. Real SMS/GG colors come exclusively from
    // palette_sync; a guessed RGB332 palette makes boot colors inconsistent.
    for (int i = 0; i < 32; i++) {
        sms_color_table[i] = sync_bits_val;
    }
#if !SMS_VGA_COMPACT_LUT
    sms_palette_pairs_init(sms_palette_pairs, sms_color_table);
#endif
}

// Update color table from SMS palette
void SMS_HOT SMSVGADisplay::updateColorTable(uint8_t* cramd) {
    if (!cramd) return;
    for (int i = 0; i < 32; i++) {
        const uint8_t value = sync_bits_val | (cramd[i] & 0x3F);
        if (sms_color_table[i] != value) {
            sms_color_table[i] = value;
#if !SMS_VGA_COMPACT_LUT
            sms_palette_pairs_update_color(sms_palette_pairs, sms_color_table, (unsigned)i);
#endif
        }
    }
}

// Fast pixel drawing function
void SMSVGADisplay::fastDrawPixel(int x, int y, uint8_t color) {
    if (!framebuffer_ptr) return;
    if (x < 0 || x >= getWidth() || y < 0 || y >= getHeight()) {
        return;
    }
    int final_x = (x + screen_x_offset) ^ 2;
    if (final_x < 0 || final_x >= getWidth()) {
        return;
    }
    framebuffer_ptr[y][final_x] = color_table[color & 0x7F];
    vga_mark_rows(y, 1);
}

// Initialize VGA display hardware
void SMSVGADisplay::init() {
    if (initialized) return;
    
    #ifdef DEBUG
        Serial.println("SMSVGADisplay: Initializing VGA...");
    #endif
    
    static const unsigned char vga_pins[] = VGA_PINS;
    
    if (!vga_init(vga_pins, VgaMode_vga_mode_320x240, false)) {
        Serial.println("SMSVGADisplay: Falha ao inicializar VGA / PSRAM");
        return;
    }
    
    framebuffer_ptr = vga_get_framebuffer();
    framebuffer_linear = vga_get_raw_framebuffer();
    sync_bits_val = vga_get_sync_bits();
    
    initColorTable();
#if SMS_VGA_CACHE_GEOMETRY
    // A mode VGA nao muda em runtime. O fallback antigo continua
    // selecionavel por config.h; nao cachear ponteiros de stage.
    cached_vga_width = vga_get_xres();
    cached_vga_height = vga_get_yres();
    cached_sms_yoffset = (cached_vga_height - 192) / 2;
#endif

    initialized = true;
    
    #ifdef DEBUG
        Serial.printf("SMSVGADisplay: %dx%d initialized\n", getWidth(), getHeight());
    #endif
}

bool SMSVGADisplay::ready() { return initialized; }

// Fill entire screen with a single color
void SMSVGADisplay::fillScreen(uint16_t color) {
    uint8_t vga_color = rgb565_to_vga6(color);
    if (framebuffer_linear) {
        memset(framebuffer_linear, sync_bits_val | vga_color, getWidth() * getHeight());
        vga_mark_rows(0, getHeight());
    }
}

// Draw a filled rectangle
void SMSVGADisplay::fillRect(int x, int y, int w, int h, uint16_t color) {
    if (!framebuffer_ptr) return;
    
    uint8_t vga_color = rgb565_to_vga6(color);
    uint8_t pixel = sync_bits_val | vga_color;
    int x1 = max(x, 0);
    int y1 = max(y, 0);
    int x2 = min(x + w, getWidth());
    int y2 = min(y + h, getHeight());
    
    for (int row = y1; row < y2; row++) {
        uint8_t* line = framebuffer_ptr[row];
        for (int col = x1; col < x2; col++) {
            int final_col = (col + screen_x_offset) ^ 2;
            line[final_col] = pixel;
        }
    }
    vga_mark_rows(y1, y2 - y1);
}

// Draw hollow rectangle outline
void SMSVGADisplay::drawRect(int x, int y, int w, int h, uint16_t color) {
    fillRect(x, y, w, 1, color);
    fillRect(x, y + h - 1, w, 1, color);
    fillRect(x, y + 1, 1, h - 2, color);
    fillRect(x + w - 1, y + 1, 1, h - 2, color);
}

// Draw a single character using 8x8 font
void SMSVGADisplay::drawChar(char c, int x, int y, uint8_t color, uint8_t backcolor) {
    int auxId = c << 3;
    
    for (int row = 0; row < 8; row++) {
        int y_pos = y + row;
        if (y_pos >= getHeight()) break;
        
        uint8_t bits = gb_sdl_font_8x8[auxId + row];
        
        for (int col = 0; col < 8; col++) {
            int x_pos = x + (6 - col);
            if (x_pos >= getWidth()) continue;
            
            uint8_t auxColor = (bits >> col) & 0x01;
            fastDrawPixel(x_pos, y_pos, auxColor ? color : backcolor);
        }
    }
}

// Draw a string of text
void SMSVGADisplay::drawString(const char* text, int x, int y, int font) {
    (void)font;
    int px = x;
    
    while (*text) {
        drawChar(*text++, px, y, textColor, bgColor);
        px += 7;
    }
}

// Fonte original do navegador de discos TK-2000: colunas de 5x7,
// passo de 6 pixels. API antiga mantida para nao afetar F1 nem OSD.
int SMSVGADisplay::browserSerifWidth(const char* text) {
    return text ? (int)strlen(text) * SMS_BROWSER_TK2000_ADVANCE : 0;
}

void SMSVGADisplay::drawBrowserSerif(const char* text, int x, int y) {
    if (!text || !framebuffer_ptr) return;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        const unsigned char c = (*p >= 32 && *p <= 126) ? *p : (unsigned char)'?';
        const uint8_t* columns = sms_browser_tk2000_glyphs[c - 32];
        if (x >= getWidth()) break;
        for (int col = 0; col < SMS_BROWSER_TK2000_ADVANCE; ++col) {
            const uint8_t bits = col < SMS_BROWSER_TK2000_WIDTH ? columns[col] : 0;
            for (int row = 0; row < SMS_BROWSER_TK2000_HEIGHT; ++row) {
                fastDrawPixel(x + col, y + row,
                              (bits & (1u << row)) ? textColor : bgColor);
            }
        }
        x += SMS_BROWSER_TK2000_ADVANCE;
    }
}

// Set text foreground and background colors
void SMSVGADisplay::setTextColor(uint16_t fg, uint16_t bg) {
    textColor = rgb565_to_vga6(fg);
    bgColor = rgb565_to_vga6(bg);
}

// Set cursor position
void SMSVGADisplay::setCursor(int x, int y) {
    cursorX = x;
    cursorY = y;
}

// Print string
void SMSVGADisplay::print(const char* text) {
    drawString(text, cursorX, cursorY);
    cursorX += strlen(text) * 7;
}

// Print char
void SMSVGADisplay::print(char c) {
    char str[2] = {c, 0};
    print(str);
}

// Calculate text width
int SMSVGADisplay::textWidth(const char* text) {
    return strlen(text) * 7;
}

// Get screen width
int SMSVGADisplay::getWidth() {
    return vga_get_xres();
}

// Get screen height
int SMSVGADisplay::getHeight() {
    return vga_get_yres();
}

#if SMS_FULLSCREEN
// Liga/desliga a varredura esticada (driver) e ajusta o layout do jogo:
// esticada = linhas 0..191, x=0; normal = centralizado (y=24, x=32).
// Chamar com a emulacao parada e os stages limpos logo depois.
void SMSVGADisplay::applyFullscreen(uint8_t mode) {
    if (mode > 1) mode = 1;
    if (mode && !vga_set_hw_stretch(1)) mode = 0;          // sem suporte: 1:1
    if (!mode) vga_set_hw_stretch(0);                       // volta ao 320x240
    fullscreen_mode = mode;
    fullscreen_active = mode != 0;
#if SMS_VGA_CACHE_GEOMETRY
    cached_sms_yoffset = mode ? 0 : (cached_vga_height - 192) / 2;
#endif
}
void SMSVGADisplay::setFullscreen(uint8_t mode) { fullscreen_want = mode; }
bool SMSVGADisplay::fullscreenActive() { return fullscreen_active; }
uint8_t SMSVGADisplay::fullscreenMode() { return fullscreen_mode; }
#else
void SMSVGADisplay::setFullscreen(uint8_t) {}
bool SMSVGADisplay::fullscreenActive() { return false; }
uint8_t SMSVGADisplay::fullscreenMode() { return 0; }
#endif

// Enter emulation mode
void SMSVGADisplay::beginEmulation() {
    vga_async_wait_idle();
    vga_direct_top_discard();
#if SMS_VGA_BATCH_DIRTY_ROWS
    scanline_dirty_first = scanline_dirty_last = -1;
#endif
#if SMS_VGA_SKIP_UNCHANGED_ROWS
    // Reset apos troca de ROM e limpeza dos dois stages. Cache de hash
    // nao persiste em menus, cold reset ou novas imagens PSRAM.
    row_stage_base[0] = row_stage_base[1] = nullptr;
    memset(row_stage_valid, 0, sizeof(row_stage_valid));
    row_stage_next_slot = 0;
#endif
    emulationMode = true;
#if SMS_FULLSCREEN
    applyFullscreen(fullscreen_want);
#endif
    // Remover imagens antigas dos dois stages (especialmente ao trocar ROM).
    vga_async_clear_stages(sync_bits_val);
    framebuffer_ptr = vga_get_framebuffer();
    framebuffer_linear = vga_get_raw_framebuffer();
    fillScreen(VGA_COLOR_BLACK_RGB565);
    // NES v0.1.15: a v0.1.14 only cleared the PSRAM stages.  The direct
    // DMA scanout still contained the previous menu beyond the 256-pixel
    // game viewport (F4 off), as seen on physical hardware.  Present ONE
    // complete black frame on menu -> game / ROM change / F4 transition,
    // BEFORE letting the PPU draw directly to DMA.  No full-frame copy is
    // added to the gameplay loop, preserving the 60 FPS / 60 EPS fast path.
    updateDisplay();
    vga_async_wait_idle();
#if SMS_VGA_LINE_RING
    // Depois de loadGame(): o anel nao interfere na alocacao dos caches de ROM.
    vga_line_ring_start();
#endif
}

// Exit emulation mode
void SMSVGADisplay::endEmulation() {
    vga_direct_top_discard();
#if SMS_VGA_LINE_RING
    vga_line_ring_stop();   // drena e libera anel/tarefa antes dos menus
#endif
    // Menus usam apresentacao sincrona. Nao sobrepor a copia em andamento.
    vga_async_wait_idle();
#if SMS_FULLSCREEN
    // Menus e navegador usam o framebuffer 320x240 1:1.
    if (fullscreen_active) applyFullscreen(0);
#endif
#if SMS_VGA_BATCH_DIRTY_ROWS
    // Se o usuario abriu um menu antes da apresentacao, os pixels
    // renderizados ainda serao marcados no stage; o menu pode sobrescrever.
    flushScanlineDirtyRows();
#endif
    emulationMode = false;
}

// Update display
void SMSVGADisplay::updateDisplay() {
    // Apresenta o quadro completo somente durante o blank vertical.
    if (!initialized) return;
#if SMS_VGA_BATCH_DIRTY_ROWS
    // Equivalente as marcacoes por linha; nao altera indices de buffer,
    // a thread VGA, a sincronizacao ou as marcas de OSD/menu.
    flushScanlineDirtyRows();
#endif
    if (emulationMode && vga_async_submit()) {
        // A proxima linha ja pode ser desenhada enquanto o core 1 apresenta
        // o quadro anterior. Atualizar tambem o ponteiro usado pelo OSD.
        framebuffer_ptr = vga_get_framebuffer();
        framebuffer_linear = vga_get_raw_framebuffer();
        bufferNeedsUpdate = false;
    } else if (vga_swap_buffers(true)) bufferNeedsUpdate = false;
}

// Draw scanline for SMS
void SMS_HOT SMSVGADisplay::drawScanline(int y, uint8_t* pixels, int width) {
    if (!emulationMode || !framebuffer_ptr || !pixels) return;

    // Resolucao do VGA nao muda depois de init(). O caminho acelerado
    // evita chamadas externas repetidas por scanline. Sem cache, consulta
    // a geometria do driver a cada scanline (teste A/B).
#if SMS_VGA_CACHE_GEOMETRY
    const int vga_width = cached_vga_width;
    const int vga_height = cached_vga_height;
    const int y_offset = cached_sms_yoffset;
#else
    const int vga_width = getWidth();
    const int vga_height = getHeight();
    const int y_offset = (vga_height - 192) / 2;
#endif
#if SMS_FULLSCREEN
    const int screen_y = y + (fullscreen_active ? 0 : y_offset);
#else
    const int screen_y = y + y_offset;
#endif
    if (screen_y < 0 || screen_y >= vga_height) return;
    int x_offset = (vga_width - width) / 2;
#if SMS_FULLSCREEN
    // A varredura esticada mostra so as 256 primeiras amostras de cada linha.
    if (fullscreen_active) x_offset = 0;
#endif
    if (x_offset < 0) x_offset = 0;

#if SMS_DETAIL_ACTIVE
    // Coletar tempos somente quando o profiler esparso (1/64 frames)
    // estiver ativo. Em quadros normais nao ha chamadas de timer.
    const bool measure_video = sms_detail_sampling != 0;
    int64_t video_t0 = measure_video ? esp_timer_get_time() : 0;
#endif

    // Caminho rapido, seguindo a tecnica do TK-2000 v0.86: preparar os
    // 256/160 pixels (SMS/GG) na RAM interna e copiar PSRAM em bloco.
    // A condicao de alinhamento preserva o swizzle XOR2 para cada grupo.
    if (width > 0 && width <= 256 && (width & 3) == 0 &&
        x_offset + width <= vga_width &&
        ((x_offset + screen_x_offset) & 3) == 0 &&
        x_offset + screen_x_offset >= 0 &&
        x_offset + screen_x_offset + width <= vga_width) {
        // v0.1.8: linebuf e viewport sao alinhados em 4 bytes. Ler
        // indices de 4 em 4 e emitir uma palavra de 32 bits evita as
        // duas escritas de 16 bits por grupo da v0.1.7. A copia para
        // PSRAM continua unica e em bloco. O fallback cobre fontes
        // desalinhadas e permite comparacao A/B por config.h.
        uint32_t packed_stack[256 / 4];
        uint32_t* packed_words = packed_stack;
#if SMS_VGA_LINE_RING && !SMS_VGA_SKIP_UNCHANGED_ROWS
        // v0.1.34: empacotar direto num slot do anel (RAM interna); o core 0
        // copia para o stage PSRAM. NULL = anel inativo, caminho direto.
        uint32_t* const ring_words = vga_line_ring_acquire();
        if (ring_words) packed_words = ring_words;
#endif
#if SMS_VGA_COMPACT_LUT
        // Paleta dinamica de 32 bytes: sem 2 KiB de LUT de pares.
        // Usa o mesmo caminho para linebuf alinhado ou nao alinhado.
        sms_pack_vga_scanline_compact32(packed_words, pixels,
                                        sms_color_table, (size_t)width);
#else
#if SMS_VGA_CONVERT_WORD32
        if (((uintptr_t)pixels & 3u) == 0u) {
            sms_pack_vga_scanline_words32(packed_words, pixels,
                                          sms_palette_pairs, (size_t)width);
        } else
#endif
        {
            sms_pack_vga_scanline_pairs((uint8_t*)packed_words, pixels,
                                        sms_palette_pairs, (size_t)width);
        }
#endif
#if SMS_DETAIL_ACTIVE
        if (measure_video) {
            const int64_t t = esp_timer_get_time();
            sms_detail_stats.video_pack_us += (uint32_t)(t - video_t0);
            video_t0 = t;
            ++sms_detail_stats.video_fast_lines;
        }
#endif
#if SMS_VGA_SKIP_UNCHANGED_ROWS
        uint8_t* const dst = framebuffer_ptr[screen_y] + x_offset + screen_x_offset;
        // Primeira triagem em 3 palavras; comparacao integral evita
        // falsos positivos mesmo com sprites/paleta alterados.
        unsigned slot;
        if (row_stage_base[0] == framebuffer_linear) slot = 0;
        else if (row_stage_base[1] == framebuffer_linear) slot = 1;
        else {
            slot = row_stage_next_slot++ & 1u;
            row_stage_base[slot] = framebuffer_linear;
            memset(row_stage_valid[slot], 0, sizeof(row_stage_valid[slot]));
        }
        const uint32_t hash = sms_vga_packed_line_hash(packed_words, (size_t)width);
        // A impressao digital so dispensa a leitura se a linha mudou.
        // Colisao do hash NAO provoca perda de pixels: memcmp integral
        // contra os bytes VGA reais no stage confirma toda igualdade.
        const bool unchanged = row_stage_valid[slot][screen_y] &&
            row_stage_hash[slot][screen_y] == hash &&
            sms_vga_line_is_identical(dst, (const uint8_t*)packed_words,
                                      (size_t)width) != 0;
        row_stage_hash[slot][screen_y] = hash;
        row_stage_valid[slot][screen_y] = 1;
#if SMS_DETAIL_ACTIVE
        if (measure_video) {
            const int64_t t = esp_timer_get_time();
            sms_detail_stats.video_compare_us += (uint32_t)(t - video_t0);
            video_t0 = t;
            if (unchanged) ++sms_detail_stats.video_skipped_lines;
        }
#endif
        if (!unchanged) {
            memcpy(dst, packed_words, (size_t)width);
#if SMS_DETAIL_ACTIVE
            if (measure_video)
                sms_detail_stats.video_psram_us += (uint32_t)(esp_timer_get_time() - video_t0);
#endif
        }
#else
#if SMS_VGA_LINE_RING
        if (ring_words) {
            // Ultima linha visivel: acordar o core 0 sem esperar o lote.
            vga_line_ring_publish(
                framebuffer_ptr[screen_y] + x_offset + screen_x_offset,
                (uint32_t)width, y >= vp_vend - 1);
        } else
#endif
        {
        // v0.1.14: testar apenas escrita de 32 bits alinhada, SEM jamais
        // ler/comparar PSRAM. Default usa o memcpy da v0.1.10/v0.1.13.
#if SMS_VGA_PSRAM_WRITE32
        sms_vga_psram_write32(
            framebuffer_ptr[screen_y] + x_offset + screen_x_offset,
            packed_words, (size_t)width);
#else
        memcpy(framebuffer_ptr[screen_y] + x_offset + screen_x_offset,
               packed_words, (size_t)width);
#endif
        }
#if SMS_DETAIL_ACTIVE
        if (measure_video)
            sms_detail_stats.video_psram_us += (uint32_t)(esp_timer_get_time() - video_t0);
#endif
#endif
    } else {
#if SMS_DETAIL_ACTIVE
        if (measure_video) ++sms_detail_stats.video_fallback_lines;
#endif
        for (int x = 0; x < width && x + x_offset < vga_width; x++) {
            const int final_x = (x + x_offset + screen_x_offset) ^ 2;
            if (final_x >= 0 && final_x < vga_width)
                framebuffer_ptr[screen_y][final_x] = sms_color_table[pixels[x] & 31];
        }
    }
#if SMS_VGA_BATCH_DIRTY_ROWS
    // Linhas normais do VDP sao consecutivas: uma marcacao por quadro.
    // Se chegarem linhas nao consecutivas, descarregar a faixa anterior
    // para manter exatamente a mesma mascara de linhas do metodo antigo.
    if (scanline_dirty_first < 0) {
        scanline_dirty_first = scanline_dirty_last = screen_y;
    } else if (screen_y == scanline_dirty_last + 1) {
        scanline_dirty_last = screen_y;
    } else if (screen_y < scanline_dirty_first || screen_y > scanline_dirty_last) {
        flushScanlineDirtyRows();
        scanline_dirty_first = scanline_dirty_last = screen_y;
    }
#else
    vga_mark_rows(screen_y, 1);
#endif
    bufferNeedsUpdate = true;
}

// Get framebuffer rows for the non-destructive top-border OSD.
uint8_t** SMSVGADisplay::getFramebuffer() {
    return framebuffer_ptr;
}

// Get raw framebuffer pointer
uint8_t* SMSVGADisplay::getRawFramebuffer() {
    return framebuffer_linear;
}

// Get sync bits
uint8_t SMSVGADisplay::getSyncBits() {
    return sync_bits_val;
}

// Set screen offset
void SMSVGADisplay::setScreenOffset(int offset) {
    screen_x_offset = offset;
}

// Get screen offset
int SMSVGADisplay::getScreenOffset() {
    return screen_x_offset;
}

// ============================================================
// FUNCIONES PUENTE PARA SER LLAMADAS DESDE C (render.c)
// ============================================================

extern "C" SMS_HOT void vga_draw_scanline(void* display, int line, uint8_t* pixels, int width) {
    SMSVGADisplay* disp = (SMSVGADisplay*)display;
    if (disp) {
        disp->drawScanline(line, pixels, width);
    }
}

extern "C" SMS_HOT void vga_update_palette(void* display, uint8_t* cramd) {
    SMSVGADisplay* disp = (SMSVGADisplay*)display;
    if (disp) {
        disp->updateColorTable(cramd);
    }
}