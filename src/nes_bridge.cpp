/*
 * NES/ESP32 backend: Nofrendo from the user-provided NES.zip.
 * RGB222 VGA and PS/2 come from the SMS v0.1.43 reference.
 * Only the frontend owns the complete iNES ROM buffer; the Nofrendo
 * PRG/CHR pointers are non-owning views into that PSRAM allocation.
 */
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include "nes_bridge.h"
#include "nes_rgb222_palette.h"
#include "nes_scanline_fast.h"
#include "nes_vga_overscan.h"
#include "nes_battery.h"
#include "sms_vga_adapter.h"
#include "vga_6bit.h"
#include "sms_audio_ring.h"
extern "C" {
#include "nes_core/emuapi.h"
#include "nes_core/nofrendo.h"
#include "nes_core/nofconfig.h"
#include "nes_core/nes.h"
#include "nes_core/nes_rom.h"
#include "nes_core/nes_mmc.h"
#include "nes_core/nesinput.h"
#include "nes_core/event.h"
#include "nes_core/vid_drv.h"
#include "nes_core/osd.h"
#include "nes_core/bitmap.h"
}

#ifndef NES_AUDIO_SAMPLE_RATE
#define NES_AUDIO_SAMPLE_RATE 15720
#endif
#define NES_OUTPUT_RATE NES_AUDIO_SAMPLE_RATE

/* v0.1.10: PPU line renderer on core 0 (the emulator/CPU runs on core 1).
 * 0 = every line drawn by the emulator core, exactly as in v0.1.9. */
#ifndef NES_PPU_ASYNC
#define NES_PPU_ASYNC 1
#endif
#ifndef NES_PPU_ASYNC_SLOTS
#define NES_PPU_ASYNC_SLOTS 16      /* power of two */
#endif
/* v0.1.11: 1 KiB CHR-ROM pages cached in internal RAM for the core-0 PPU.
 * 0 disables; the loader falls back to 16 pages when RAM is short. */
#ifndef NES_CHR_CACHE_PAGES
#define NES_CHR_CACHE_PAGES 0       /* v0.1.12: off by default (no gain measured) */
#endif
/* Keep this much internal heap free after allocating the cache. */
#ifndef NES_CHR_CACHE_RESERVE
#define NES_CHR_CACHE_RESERVE 40960u
#endif
/* Below the VGA stage->DMA worker (4) so the frame copy is never stretched. */
#ifndef NES_PPU_TASK_PRIORITY
#define NES_PPU_TASK_PRIORITY 5
#endif
/* v0.1.12: game lines are stored straight into the displayed DMA framebuffer
 * (internal RAM), each one after the monitor beam has passed it.  This removes
 * the PSRAM stage write and the stage->DMA copy for the game area.  Menus and
 * the FPS overlay band still use the staged path.  0 = v0.1.10 staged video. */
#ifndef NES_VGA_DIRECT
#define NES_VGA_DIRECT 1
#endif
#define NES_OSD_BAND_ROWS 16        /* rows kept on the staged path while the overlay is on */
static uint8_t* rom_image = nullptr;
static size_t rom_image_size = 0;
static bool rom_running = false;
static uint8_t palette[64] = {};
/* v0.10: one solid RGB222 color per NES color. No spatial or temporal
 * dithering: a 2x2 beige checkerboard was visibly wrong on VGA monitors.
 * Keep the same 64-color NTSC 2C02 palette and approved red/cyan fixes. */
/* Per-color lookup includes 0x40/0x80 PPU flags and VGA sync bits. */
static uint8_t vga_color_lut[256] = {};
/* v1.03: soft overscan edges.  Nothing is cropped: the 4 outer pixels of
 * every border (left, right, top, bottom) keep their content but are drawn
 * through a darker copy of the palette LUT (NES_VGA_EDGE_PCT of the
 * brightness).  Corners are covered by the same band.  Cost: the row selects
 * its LUT once and, on the other 232 lines, re-packs 2 words (8 pixels). */
static uint8_t vga_edge_lut[256] = {};
static volatile bool edge_soft = NES_VGA_EDGE_SOFT != 0;
static inline __attribute__((always_inline)) uint32_t nes_pack_word4(
    const uint8_t* p, const uint8_t* lut) {
    return (uint32_t)lut[p[2]] | ((uint32_t)lut[p[3]] << 8) |
           ((uint32_t)lut[p[0]] << 16) | ((uint32_t)lut[p[1]] << 24);
}
static inline __attribute__((always_inline)) void nes_pack_video_row(
    uint32_t* dst, const uint8_t* pixels, const uint8_t* lut, int row,
    bool centered, uint8_t blank_pixel) {
    if (!edge_soft) {
        nes_pack_scanline_256(dst, pixels, lut);
    } else if (row < NES_VGA_EDGE_PX || row >= 240 - NES_VGA_EDGE_PX) {
        nes_pack_scanline_256(dst, pixels, vga_edge_lut);
    } else {
        nes_pack_scanline_256(dst, pixels, lut);
        dst[0]  = nes_pack_word4(pixels, vga_edge_lut);
        dst[63] = nes_pack_word4(pixels + 252, vga_edge_lut);
    }
#if NES_VGA_OVERSCAN_SIDE > 0
    // F4 usa crop NO DESCRITOR I2S (sem colunas pretas dentro da imagem).
    // Centrado: tornar a faixa nao-util preta e contigua a margem existente.
    // Como SIDE e multiplo de 4, x^2 preserva os blocos RGB222.
    if (centered) {
        uint8_t* out = (uint8_t*)dst;
        memset(out, blank_pixel, NES_VGA_OVERSCAN_SIDE);
        memset(out + 256 - NES_VGA_OVERSCAN_SIDE, blank_pixel,
               NES_VGA_OVERSCAN_SIDE);
    }
#else
    (void)centered; (void)blank_pixel;
#endif
}
extern "C" void nes_set_edge_soft(bool on) { edge_soft = on; }
extern "C" bool nes_get_edge_soft(void) { return edge_soft; }
static uint8_t** frame_render_rows = nullptr;
static volatile bool direct_active = false;   /* this frame uses direct scanout rows */
static volatile int direct_first_row = 0;
static int frame_render_offset = 32;
static uint8_t frame_black_byte = 0;
static uint32_t perf_emulation_us = 0, perf_present_us = 0, perf_audio_us = 0;
static uint32_t perf_sample_frames = 0;
/* v0.1.10: core-0 drawing time and core-1 time spent waiting for it. */
static volatile uint32_t perf_ppu0_us = 0;
static uint32_t perf_drain_us = 0;

static uint8_t dummy_line[320] = {};

#if NES_PPU_ASYNC
static TaskHandle_t ppu_task = nullptr;
static void* ppu_jobs_mem = nullptr;
static void* chr_cache_mem = nullptr;
static uint32 ppu_clock_cb(void) { return (uint32)esp_timer_get_time(); }
static volatile bool ppu_task_quit = false;
static volatile bool ppu_task_exited = true;
static void ppu_wake_cb(void) {
    TaskHandle_t t = ppu_task;
    if (t) xTaskNotifyGive(t);
}
static void ppu_idle_cb(void) { vTaskDelay(1); }
static void ppu_render_task(void*) {
    TickType_t last_yield = xTaskGetTickCount();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t t0 = esp_timer_get_time();
        ppu_async_consume();
        perf_ppu0_us = perf_ppu0_us + (uint32_t)(esp_timer_get_time() - t0);
        if (ppu_task_quit) break;
        /* If this core never goes idle, let Idle0 feed the task watchdog. */
        if ((TickType_t)(xTaskGetTickCount() - last_yield) > pdMS_TO_TICKS(2000)) {
            vTaskDelay(1);
            last_yield = xTaskGetTickCount();
        }
    }
    ppu_task_exited = true;
    vTaskDelete(nullptr);
}
static void ppu_async_begin() {
    if (ppu_task) return;
    const size_t bytes = (size_t)ppu_async_jobsize() * (size_t)NES_PPU_ASYNC_SLOTS;
    ppu_jobs_mem = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ppu_jobs_mem) {
        Serial.println("[NES] PPU no core 0 indisponivel: sem RAM interna (modo v0.1.9)");
        return;
    }
    ppu_task_quit = false;
    ppu_task_exited = false;
    if (xTaskCreatePinnedToCore(ppu_render_task, "nes_ppu", 4096, nullptr,
                                NES_PPU_TASK_PRIORITY, &ppu_task, 0) != pdPASS) {
        ppu_task = nullptr;
        ppu_task_exited = true;
        free(ppu_jobs_mem); ppu_jobs_mem = nullptr;
        Serial.println("[NES] PPU no core 0 indisponivel: tarefa nao criada (modo v0.1.9)");
        return;
    }
    if (!ppu_async_start(ppu_jobs_mem, NES_PPU_ASYNC_SLOTS, ppu_wake_cb, ppu_idle_cb)) {
        ppu_task_quit = true;
        xTaskNotifyGive(ppu_task);
        for (int i = 0; i < 200 && !ppu_task_exited; ++i) vTaskDelay(1);
        vTaskDelay(2);
        ppu_task = nullptr;
        free(ppu_jobs_mem); ppu_jobs_mem = nullptr;
        return;
    }
    ppu_async_set_clock(ppu_clock_cb);
    Serial.printf("[NES] PPU no core 0: fila %u x %u B\n",
                  (unsigned)NES_PPU_ASYNC_SLOTS, (unsigned)ppu_async_jobsize());
    /* CHR-ROM page cache: only while enough internal heap stays free. */
    int pages = NES_CHR_CACHE_PAGES;
    while (pages >= 16) {
        const size_t need = (size_t)pages * 1024u;
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= need + NES_CHR_CACHE_RESERVE) {
            chr_cache_mem = heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (chr_cache_mem) break;
        }
        pages = (pages > 16) ? 16 : 0;
    }
    if (chr_cache_mem) {
        ppu_chrcache_init(chr_cache_mem, pages);
        Serial.printf("[NES] Cache CHR em RAM interna: %d paginas de 1 KiB\n", pages);
    } else {
        Serial.println("[NES] Cache CHR desligado (sem RAM interna ou NES_CHR_CACHE_PAGES=0)");
    }
}
static void ppu_async_end() {
    if (!ppu_task) return;
    ppu_chrcache_init(nullptr, 0);  /* drains, then forgets every cached page */
    if (chr_cache_mem) { free(chr_cache_mem); chr_cache_mem = nullptr; }
    ppu_async_set_clock(nullptr);
    ppu_async_stop();               /* draws what is queued, then turns the queue off */
    ppu_task_quit = true;
    xTaskNotifyGive(ppu_task);
    for (int i = 0; i < 200 && !ppu_task_exited; ++i) vTaskDelay(1);
    vTaskDelay(2);                  /* Idle0 frees the task's TCB/stack */
    ppu_task = nullptr;
    free(ppu_jobs_mem); ppu_jobs_mem = nullptr;
}
#else
static void ppu_async_begin() {}
static void ppu_async_end() {}
#endif

#if SMS_VGA_LINE_RING
/* sms_vga_adapter.cpp (shared SMS code) references the SMS viewport end when
 * the line ring is enabled; the NES always draws 240 lines. */
extern "C" { int vp_vend = 240; }
#endif
static bitmap_t* bitmap_hw = nullptr;
static uint8_t next_pad = 0;
static uint8_t prev_pad = 0;
static std::string current_path;
static File open_file;
static bool sound_ready = false;
static void (*audio_callback)(void*, int) = nullptr;
static uint32_t frame_samples_acc = 0;
static SMSAudioRing ring;
static bool overlay = false;
static uint16_t shown_fps=0, shown_eps=0;
static bool shown_skip=false;
static uint8_t shown_percent=0;
// v0.1.17: cached OSD in the PSRAM UI layer. The protected top-band
// samples this rectangle each frame; redraw the string only if it changed.
// Reset on ROM load/unload and F4/menu transitions (stage cleared).
static bool osd_text_valid = false;
static char osd_last_text[48] = {};
static uint8_t osd_last_offset = 0xff;
static uint8_t** osd_last_rows = nullptr;
extern "C" void nes_set_overlay(bool on,uint16_t fps,uint16_t eps,bool autoskip,uint8_t percent) {
    overlay=on; shown_fps=fps; shown_eps=eps; shown_skip=autoskip; shown_percent=percent;
}

extern "C" void *emu_Malloc(int size) {
    if (size <= 0) return nullptr;
    /* v0.1.11: blocks up to 8 KiB (cartridge RAM at $6000, 8 KiB CHR-RAM) stay
     * in internal RAM: the CPU and the PPU read them constantly and PSRAM
     * access stalls.  Larger blocks go to PSRAM as before. */
    if (size <= 8192) {
        void* p = heap_caps_malloc((size_t)size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) return p;
    }
    if (size >= 4096 && psramFound()) {
        void* p = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) return p;
    }
    return malloc((size_t)size);
}
extern "C" void *emu_LineBuffer(int) { return dummy_line; }
extern "C" void emu_printf(const char* msg) { if (msg) Serial.print(msg); }
extern "C" int emu_FileOpen(const char* path) {
    if (open_file) open_file.close();
    open_file = SD.open(path, FILE_READ);
    return open_file ? 1 : 0;
}
extern "C" int emu_FileRead(void* dst, int count) {
    return open_file && count > 0 ? (int)open_file.read((uint8_t*)dst, (size_t)count) : 0;
}
extern "C" void emu_FileClose(void) { if (open_file) open_file.close(); }

/* One packed 32-bit store per 4 pixels. Lookup table is 256 bytes of
 * internal DRAM; scanout staging remains the SMS double-buffered PSRAM.
 * Frame-row pointer/geometry is sampled once, before the PPU renders.
 */
/* All output paths share one inlinable packed converter.  On the top 96
 * lines the 24 KiB DRAM band is NOT the displayed DMA framebuffer; pack
 * directly into that band instead of packing to a 256-byte stack scratch
 * and memcpy'ing it once per line.  Below the band keep the proven behind-
 * beam order: pack first, wait for the beam, and copy only when safe. */
extern "C" void IRAM_ATTR emu_DrawLine(unsigned char* pixels, int width, int, int line) {
    if (!pixels || line < 0 || line >= 240 || width < 256) return;
    uint8_t** fb = frame_render_rows;
    if (!fb || !fb[line]) return;
    const uint8_t* const map = vga_color_lut;
    const bool centered = frame_render_offset == 32;
#if NES_VGA_DIRECT
    if (direct_active && line >= direct_first_row) {
        uint8_t* const shown = vga_direct_row(line);
        if (shown) {
            uint8_t* const top = vga_direct_top_row(line);
            if (top) {
                // Exclusive DRAM stage for VBlank; safe on PPU core 0.
                nes_pack_video_row((uint32_t*)top, pixels, map, line, centered, frame_black_byte);
            } else if (uint8_t* const middle =
                           vga_direct_middle_row(line, frame_render_offset)) {
                // v0.8: central scanlines are staged in EXISTING PSRAM,
                // then committed together with the top during VBlank.
                nes_pack_video_row((uint32_t*)middle, pixels, map, line, centered, frame_black_byte);
            } else {
                // Preserve v0.1.17 direct path for the lower lines and
                // re-check the beam immediately before their DMA memcpy.
                uint32_t packed[64];
                nes_pack_video_row(packed, pixels, map, line, centered, frame_black_byte);
                vga_direct_wait_row(line, line == direct_first_row);
                while (!vga_direct_row_copy_safe(line))
                    vga_direct_wait_row(line, false);
                memcpy(shown + frame_render_offset, packed, 256);
                // No artificial one-pixel vertical bars at either viewport edge.
            }
            return;
        }
    }
#endif
    uint8_t* target = fb[line] + frame_render_offset;
#if SMS_VGA_LINE_RING
    uint32_t* const ring = ppu_async_in_consumer() ? nullptr : vga_line_ring_acquire();
    if (ring) {
        nes_pack_video_row(ring, pixels, map, line, centered, frame_black_byte);
        vga_line_ring_publish(target, 256, line >= 239);
        return;
    }
#endif
    if ((((uintptr_t)target) & 3u) == 0u) {
        nes_pack_video_row((uint32_t*)target, pixels, map, line, centered, frame_black_byte);
    } else {
        for (unsigned x = 0; x < 256; ++x)
            target[x ^ 2u] = map[pixels[x]];
#if NES_VGA_OVERSCAN_SIDE > 0
        if (centered) {
            memset(target, frame_black_byte, NES_VGA_OVERSCAN_SIDE);
            memset(target + 256 - NES_VGA_OVERSCAN_SIDE,
                   frame_black_byte, NES_VGA_OVERSCAN_SIDE);
        }
#endif
    }
}

extern "C" void osd_getinput(void) {
    static const int key_events[8] = {
        event_joypad1_a, event_joypad1_b, event_joypad1_select, event_joypad1_start,
        event_joypad1_up, event_joypad1_down, event_joypad1_left, event_joypad1_right
    };
    const uint8_t changed = next_pad ^ prev_pad;
    for (int i=0;i<8;++i) if (changed & (1u<<i)) {
        event_t fn = event_get(key_events[i]);
        if (fn) fn((next_pad & (1u<<i)) ? INP_STATE_MAKE : INP_STATE_BREAK);
    }
    prev_pad = next_pad;
}
extern "C" void nes_set_pad(uint8_t buttons) { next_pad = buttons; }
extern "C" void osd_getmouse(int* x, int* y, int* button) {
    if(x)*x=0; if(y)*y=0; if(button)*button=0;
}
extern "C" int osd_init(void) { return 0; }
extern "C" void osd_shutdown(void) {}
extern "C" void osd_fullname(char* out, const char* in) {
    if (!out) return;
    snprintf(out, PATH_MAX, "%s", in ? in : "");
}
extern "C" char *osd_newextension(char* in, char*) { return in; }
extern "C" int osd_makesnapname(char*, int) { return -1; }
extern "C" int osd_installtimer(int, void*, int, void*, int) { return 0; } // NOLOOP frames paced by Arduino
extern "C" char* osd_getromdata(void) { return (char*)rom_image; }

extern "C" void osd_getsoundinfo(sndinfo_t* info) {
    if (!info) return;
    info->sample_rate = NES_OUTPUT_RATE;
    info->bps = 16;
}
extern "C" void osd_setsound(void(*play)(void*,int)) { audio_callback = play; }
extern "C" void nes_audio_frame(void) {
    if (!rom_running || !audio_callback || !sound_ready) return;
    // Fixed-point audio samples per NTSC frame; 15720/60=262 samples.
    frame_samples_acc += NES_OUTPUT_RATE;
    int count = frame_samples_acc / 60;
    frame_samples_acc %= 60;
    if (count < 1 || count > 400) return;
    int16_t pcm[400];
    audio_callback(pcm, count);
    ring.pushMono(pcm, (uint32_t)count);
}
extern "C" void nes_sound_enable(bool on) {
    sound_ready = on;
    if (!on) ring.reset();
}
extern "C" IRAM_ATTR bool nes_sound_pop(int16_t* output) {
    return sound_ready && ring.pop(output);
}

static int video_init(int,int) { return 0; }
static void video_shutdown() {}
static int video_setmode(int,int) { return 0; }
static void video_palette(rgb_t* pal) {
    if (!pal) return;
    for (int i=0;i<64;++i) {
        int r = pal[i].r, g=pal[i].g, b=pal[i].b;
        r = r<0?0:(r>255?255:r);
        g = g<0?0:(g>255?255:g);
        b = b<0?0:(b>255?255:b);
        palette[i] = nes_rgb_to_vga222_2c02((unsigned)i,
                 (uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
    const uint8_t sync = SMSVGADisplay::getSyncBits();
    for (unsigned i=0; i<256; ++i) vga_color_lut[i] = (uint8_t)(sync | palette[i & 63u]);
    // v1.03: darker palette for the soft edges (built once per palette).
    {
        uint8_t dim[64];
        for (int i=0;i<64;++i) {
            int r = pal[i].r, g=pal[i].g, b=pal[i].b;
            r = r<0?0:(r>255?255:r);
            g = g<0?0:(g>255?255:g);
            b = b<0?0:(b>255?255:b);
            dim[i] = nes_rgb_to_vga222((uint8_t)((unsigned)r * NES_VGA_EDGE_PCT / 100u),
                                       (uint8_t)((unsigned)g * NES_VGA_EDGE_PCT / 100u),
                                       (uint8_t)((unsigned)b * NES_VGA_EDGE_PCT / 100u));
        }
        for (unsigned i=0; i<256; ++i) vga_edge_lut[i] = (uint8_t)(sync | dim[i & 63u]);
    }

}
static void video_clear(uint8_t) {}
static bitmap_t* video_lock() {
    if (!bitmap_hw) bitmap_hw = bmp_createhw(dummy_line, 256, 240, 256);
    return bitmap_hw;
}
static void video_free(int, rect_t*) {}
static void video_blit(bitmap_t*,int,rect_t*) {}  // PPU feeds emu_DrawLine directly.
static viddriver_t nesVgaDriver = {"NES TTGO VGA32", video_init, video_shutdown,
    video_setmode, video_palette, video_clear, video_lock, video_free, video_blit, false};
extern "C" void osd_getvideoinfo(vidinfo_t* info) {
    info->default_width = 256;
    info->default_height = 240;
    info->driver = &nesVgaDriver;
}

extern "C" int osd_main(int, char**) {
    config.filename = (char*)"nes.cfg";
    return main_loop(current_path.c_str(), system_nes);
}

extern "C" void nes_save_battery(void) { (void)nes_battery_save(true); }
extern "C" void nes_unload_rom(void) {
    nes_sound_enable(false);
    if (rom_running) {
        ppu_async_end();    // No queued line may outlive the ROM/PPU memory.
        nes_save_battery(); // This guard requires rom_running; save before clearing below.
        nes_battery_end();
        main_eject();
        vid_shutdown();
        rom_running = false;
    }
    if (rom_image) { free(rom_image); rom_image=nullptr; }
    nes_battery_end();
    rom_image_size = 0;
    osd_text_valid = false;
    audio_callback = nullptr;
    frame_samples_acc = 0;
    prev_pad = next_pad = 0;
}
extern "C" bool nes_load_rom(const char* path) {
    nes_unload_rom();
    if (!path || !*path) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) { Serial.println("[NES] ROM nao encontrada"); return false; }
    const size_t size = f.size();
    uint8_t h[16];
    if (size < 16 || f.read(h,16) != 16 || memcmp(h,"NES\x1a",4) != 0) {
        Serial.println("[NES] Cabecalho iNES invalido"); f.close(); return false;
    }
    const bool nes2 = (h[7]&0x0c)==0x08;
    if (nes2 && (h[9]!=0 || h[4]==0)) {
        // Support conventional NES 2.0 linear-size ROMs only, max 255 PRG/CHR units.
        Serial.println("[NES] NES2 tamanho estendido/ROM nao representavel"); f.close(); return false;
    }
    if (!h[4] || !psramFound()) { Serial.println("[NES] PRG invalido ou PSRAM ausente"); f.close(); return false; }
    // An iNES header may contain legacy 'DiskDude!' text in bytes 7..15.
    const bool diskDude = h[7] == 'D' && memcmp(h + 8, "iskDude!", 8) == 0;
    const int mapper = (h[6] >> 4) | (diskDude ? 0 : (h[7] & 0xf0)) | (nes2 ? ((h[8]&15)<<8) : 0);
    if (!mmc_peek(mapper)) {
        Serial.printf("[NES][MAPPER] %d nao implementado (iNES 1.0)\n", mapper);
        f.close(); return false;
    }
    const size_t expected = 16u + ((h[6]&4) ? 512u : 0u) + 16384u*h[4] + 8192u*h[5];
    if (size < expected || size > 4u*1024u*1024u) {
        Serial.println("[NES] ROM truncada ou tamanho excessivo"); f.close(); return false;
    }
    rom_image = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rom_image) { Serial.println("[NES] Sem memoria PSRAM"); f.close(); return false; }
    f.seek(0);
    const size_t got = f.read(rom_image, size);
    f.close();
    if (got != size) { Serial.println("[NES] Falha lendo cartucho"); nes_unload_rom(); return false; }
    rom_image_size = size;
    current_path = path;
    Serial.printf("[NES] Carregando %s (%u bytes) mapper %u\n", path, (unsigned)size,
                  (unsigned)mapper);
    // NOLOOP mode: nofrendo_main initializes all subsystems, loads cart and returns.
    if (nofrendo_main(0, nullptr) != 0) {
        Serial.println("[NES] Erro ao iniciar Nofrendo (mapper/ROM)");
        // Free partially constructed context while its PRG/CHR view is valid.
        main_eject();
        vid_shutdown();
        if (rom_image) { free(rom_image); rom_image=nullptr; }
        rom_image_size=0;
        return false;
    }
    rom_running = true;
    ppu_async_begin();
    nes_t* machine=nes_getcontextptr();
    if (machine && machine->rominfo)
        nes_battery_begin(path,machine->rominfo->sram,8192,
              ((machine->rominfo->flags & ROM_FLAG_BATTERY)!=0) ||
              (machine->rominfo->mapper_number==159));
    nes_battery_load();
    ring.reset(); frame_samples_acc=0;
    nes_sound_enable(true);
    return true;
}
extern "C" void nes_reset_game(bool hard) {
    if (rom_running) nes_reset(hard ? HARD_RESET : SOFT_RESET);
}
extern "C" void nes_run_frame(bool skip) {
    if (!rom_running) return;
    /* SMS technique: cache staging row pointers and fullscreen geometry once
     * per frame, not 240 times from the PPU scanline callback. */
    frame_render_rows = skip ? nullptr : SMSVGADisplay::getFramebuffer();
    frame_render_offset = SMSVGADisplay::fullscreenActive() ? 0 : 32;
    frame_black_byte = vga_get_sync_bits();
#if NES_VGA_DIRECT
    // v0.1.15: when the top band exists, even OSD rows 0..15 are rendered
    // there.  Sending those rows to the PSRAM video worker caused the
    // remaining flicker near the top at 60 FPS / 60 EPS.
    direct_first_row = vga_direct_top_enabled() ? 0 :
                       (overlay ? NES_OSD_BAND_ROWS : 0);
    direct_active = !skip && vga_direct_row(0) != nullptr;
    if (direct_active) vga_direct_mark_frame_start();
#endif
    const int64_t begin = esp_timer_get_time();
    nes_step(skip ? 1 : 0);
    const int64_t before_drain = esp_timer_get_time();
    ppu_async_drain();              /* every line of this frame is on the stage */
    // The top band is published only AFTER the FPS overlay is composed.
    // Otherwise the old PSRAM worker could overwrite rows 0..15 while the
    // monitor was already scanning them.
    const int64_t after_core = esp_timer_get_time();
    perf_drain_us += (uint32_t)(after_core - before_drain);
    if (!skip) {
        /* OSD pixels must land after every ring line of this frame. */
        vga_line_ring_drain();
        if (overlay) {
            char status[48];
            snprintf(status,sizeof(status),"FPS %u EPS %u %s %u%%",(unsigned)shown_fps,
                     (unsigned)shown_eps,shown_skip?"AUTO":"SKIP",(unsigned)shown_percent);
            const bool top_cached = direct_active && vga_direct_top_enabled();
            const bool osd_change = !osd_text_valid ||
                 osd_last_rows != frame_render_rows ||
                 osd_last_offset != (uint8_t)frame_render_offset ||
                 strcmp(osd_last_text,status) != 0;
            // The working PSRAM UI rectangle persists in direct-video mode.
            // Avoid rasterizing the same text 60 times per second. The final
            // top-band composition and VBlank presentation still run every frame.
            if (!top_cached || osd_change) {
                // Em F4, a janela DMA inicia no pixel NES SIDE: deslocar
                // OSD igualmente para manter 'FPS' totalmente legivel.
                const int osd_x = frame_render_offset == 0 ?
                    (1 + NES_VGA_OVERSCAN_SIDE) : 1;
                SMSVGADisplay::fillRect(osd_x,1,194,11,UI_COLOR_BLACK);
                SMSVGADisplay::setTextColor(UI_COLOR_YELLOW,UI_COLOR_BLACK);
                SMSVGADisplay::drawString(status,osd_x,1);
                strcpy(osd_last_text,status);
                osd_last_rows = frame_render_rows;
                osd_last_offset = (uint8_t)frame_render_offset;
                osd_text_valid = true;
            }
        } else {
            osd_text_valid = false;
        }
        if (direct_active) {
            if (vga_direct_top_enabled()) {
                // Include the OSD in the *same* 96-line VBlank presentation
                // as the game, without a PSRAM worker updating the top rows
                // one millisecond late.  The centered-mode left border is
                // composited in a separate 512-byte internal-RAM strip.
                vga_direct_top_compose_osd(overlay, frame_render_offset);
                vga_direct_top_ready(0, frame_render_offset);
                // The DMA image already has all game and OSD pixels.  No
                // asynchronous PSRAM copy is permitted during direct video.
                vga_clear_dirty_rows(0, 240);
            } else {
                // Low-heap fallback: retain the tested v0.1.14 behavior.
                vga_clear_dirty_rows(direct_first_row, 240 - direct_first_row);
                if (overlay) vga_mark_rows(0, NES_OSD_BAND_ROWS);
                if (overlay || vga_any_dirty_rows()) SMSVGADisplay::updateDisplay();
            }
        } else {
            vga_mark_rows(0, 240);
            SMSVGADisplay::updateDisplay();
        }
    }
    direct_active = false;
    frame_render_rows = nullptr;
    const int64_t after_video = esp_timer_get_time();
    nes_audio_frame();
    nes_battery_poll();
    const int64_t after_audio = esp_timer_get_time();
    perf_emulation_us += (uint32_t)(after_core - begin);
    perf_present_us += (uint32_t)(after_video - after_core);
    perf_audio_us += (uint32_t)(after_audio - after_video);
    ++perf_sample_frames;
}
extern "C" void nes_set_fullscreen(bool enabled) {
    osd_text_valid = false;
    SMSVGADisplay::setFullscreen(enabled ? 1 : 0);
}

/* v0.1.10: call BEFORE nes_perf_take() (which resets the frame count).
 * ppu0 = average microseconds per frame the core-0 task spent drawing;
 * drain = average microseconds per frame core 1 waited for it at frame end. */
extern "C" void nes_perf_take_ppu(uint32_t* ppu0_avg, uint32_t* drain_avg,
                                  uint32_t* wait_avg, uint32_t* beam_avg) {
    const uint32_t n = perf_sample_frames ? perf_sample_frames : 1u;
    if (ppu0_avg) *ppu0_avg = perf_ppu0_us / n;
    if (drain_avg) *drain_avg = perf_drain_us / n;
    /* wait = microseconds per frame core 1 was held by a full line queue;
     * beam = microseconds per frame the line writer waited for the monitor
     * beam (direct video).  Real drawing time = PPU0 - BEAM. */
    if (wait_avg) *wait_avg = (uint32_t)ppu_async_take_wait_us() / n;
    if (beam_avg) *beam_avg = vga_direct_take_wait_us() / n;
    (void)ppu_chrcache_take_misses();
    perf_ppu0_us = 0u;
    perf_drain_us = 0u;
}

/* Read/reset 10-second diagnostic sample; no SD writes and no per-frame UART. */
extern "C" void nes_perf_take(uint32_t* emu_avg, uint32_t* vga_avg, uint32_t* audio_avg) {
    const uint32_t n = perf_sample_frames ? perf_sample_frames : 1u;
    if (emu_avg) *emu_avg = perf_emulation_us / n;
    if (vga_avg) *vga_avg = perf_present_us / n;
    if (audio_avg) *audio_avg = perf_audio_us / n;
    perf_emulation_us = perf_present_us = perf_audio_us = perf_sample_frames = 0u;
}
