/*
 * NES v1.00 / TTGO VGA32 / PlatformIO Arduino 2.0.17
 * Cartridge: Nofrendo port in NES.zip (the user-provided source)
 * VGA, PS2 and OSD: adapted from the approved SMS v0.1.43 firmware
 * Partition layout: approved MC-1000 v0.92 (MultiLoader preserved)
 */
#include <Arduino.h>
#include <esp_arduino_version.h>
#if ESP_ARDUINO_VERSION_MAJOR != 2
#error "NES TTGO VGA32 requer Arduino ESP32 2.0.17 / PlatformIO espressif32 6.9.0"
#endif
#include <SPI.h>
#include <SD.h>
#include <driver/dac.h>
#include <driver/timer.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <soc/rtc_wdt.h>  // mesmo mecanismo RESET ESP32 do MC-1000 v0.92
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>
#include "../config.h"
#include "nes_bridge.h"
#include "nes_ui.h"
#include "sms_keyboard.h"
#include "nes_bootrc.h"
#include "sms_vga_adapter.h"
#include "vga_6bit.h"

extern "C" void nes_sound_enable(bool enable);
extern "C" IRAM_ATTR bool nes_sound_pop(int16_t* sample);
extern "C" void nes_set_overlay(bool enable, uint16_t fps, uint16_t eps, bool autoskip, uint8_t percent);
extern "C" void nes_perf_take(uint32_t* emu_avg, uint32_t* vga_avg, uint32_t* audio_avg);
extern "C" void nes_perf_take_ppu(uint32_t* ppu0_avg, uint32_t* drain_avg, uint32_t* wait_avg, uint32_t* beam_avg);
static SPIClass sd_spi(HSPI);
static SMSVGADisplay display;
static NES_UI ui(&display);
static bool sd_ok = false;
static bool initialized = false;
static bool fps_on = false;
static bool frameskip_auto = false;
static bool fullscreen_on = false;
static uint16_t fps = 0, eps = 0;
static uint8_t skipped_percent = 0;
static uint32_t fps_count=0, eps_count=0;
static uint32_t fps_since=0;
static int64_t next_frame_us=0;
static uint32_t last_vga_frame=0;

// v0.1.17: diagnostics only in explicitly enabled developer builds.
// Prior release printed a long UART line every 10 s at 115200 baud,
// potentially blocking the emulator core for part of a 16.7 ms frame.
// Keep reset reason at boot, preserve all diagnostic fields when enabled.
#ifndef NES_YIELD_EVERY
#define NES_YIELD_EVERY 8
#endif
#ifndef NES_DIAG_SERIAL
#define NES_DIAG_SERIAL 0
#endif

#if NES_DIAG_SERIAL
static uint32_t diag_last_ms = 0;
static uint32_t diag_total_frames = 0;
static uint32_t diag_max_frame_us = 0;
static uint32_t diag_max_vga_wait_us = 0;
static void diag_after_frame(uint32_t elapsed_us) {
    ++diag_total_frames;
    if (elapsed_us > diag_max_frame_us) diag_max_frame_us = elapsed_us;
    const uint32_t wait = vga_async_wait_us();
    if (wait > diag_max_vga_wait_us) diag_max_vga_wait_us = wait;
    const uint32_t now = millis();
    if ((uint32_t)(now - diag_last_ms) < 10000u) return;
    diag_last_ms = now;
    const uint32_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t min_int = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t stack_words = uxTaskGetStackHighWaterMark(nullptr);
    uint32_t emu_avg=0, present_avg=0, audio_avg=0;
    uint32_t ppu0_avg=0, drain_avg=0, wait_avg=0, beam_avg=0;
    nes_perf_take_ppu(&ppu0_avg,&drain_avg,&wait_avg,&beam_avg);
    nes_perf_take(&emu_avg,&present_avg,&audio_avg);
    uint32_t top_ok=0, top_late=0, top_max_us=0;
    vga_direct_top_stats(&top_ok, &top_late, &top_max_us);
    Serial.printf("[NES][DIAG] ms=%lu frames=%lu fps=%u eps=%u max_frame_us=%lu vga_wait_us=%lu heap_int=%lu heap_min=%lu heap_largest=%lu stack_words=%lu psram_free=%lu EMU_AVG_US=%lu VGA_AVG_US=%lu AUDIO_AVG_US=%lu PPU0_AVG_US=%lu DRAIN_AVG_US=%lu WAIT_AVG_US=%lu BEAM_AVG_US=%lu TOP_PRESENT=%lu TOP_MISSED=%lu TOP_COPY_MAX_US=%lu\n",
                  (unsigned long)now, (unsigned long)diag_total_frames,
                  (unsigned)fps, (unsigned)eps, (unsigned long)diag_max_frame_us,
                  (unsigned long)diag_max_vga_wait_us, (unsigned long)free_int,
                  (unsigned long)min_int, (unsigned long)largest,
                  (unsigned long)stack_words, (unsigned long)ESP.getFreePsram(),
                  (unsigned long)emu_avg, (unsigned long)present_avg,
                  (unsigned long)audio_avg, (unsigned long)ppu0_avg,
                  (unsigned long)drain_avg, (unsigned long)wait_avg,
                  (unsigned long)beam_avg,
                  (unsigned long)top_ok, (unsigned long)top_late,
                  (unsigned long)top_max_us);
    diag_max_frame_us = diag_max_vga_wait_us = 0;
}
#else
static inline void diag_after_frame(uint32_t) {}
#endif

#ifndef NES_AUDIO_SAMPLE_RATE
#define NES_AUDIO_SAMPLE_RATE 15720
#endif
static void IRAM_ATTR audio_isr(void*) {
    timer_group_clr_intr_status_in_isr(TIMER_GROUP_0, TIMER_0);
    timer_group_enable_alarm_in_isr(TIMER_GROUP_0, TIMER_0);
    int16_t sample = 0;
    if (!nes_sound_pop(&sample)) sample = 0;
    // 16-bit signed PCM -> mono DAC: input scaled to the 8-bit voltage range.
    int32_t v = ((int32_t)sample >> 8) + 128;
    if (v < 0) v=0; else if(v>255) v=255;
    dac_output_voltage(DAC_CHANNEL_1, (uint8_t)v);
}
static volatile esp_err_t audio_isr_result = ESP_FAIL;
static volatile bool audio_isr_done = false;
static void audio_register(void*) {
    audio_isr_result = timer_isr_register(TIMER_GROUP_0, TIMER_0, audio_isr, nullptr,
                                          ESP_INTR_FLAG_IRAM, nullptr);
    audio_isr_done = true;
    vTaskDelete(nullptr);
}
static void init_audio() {
    dac_output_enable(DAC_CHANNEL_1);
    dac_output_voltage(DAC_CHANNEL_1, 128);
    timer_config_t cfg = {};
    cfg.alarm_en = TIMER_ALARM_EN;
    cfg.counter_en = TIMER_PAUSE;
    cfg.intr_type = TIMER_INTR_LEVEL;
    cfg.counter_dir = TIMER_COUNT_UP;
    cfg.auto_reload = TIMER_AUTORELOAD_EN;
    cfg.divider = 2;
    timer_init(TIMER_GROUP_0, TIMER_0, &cfg);
    timer_set_counter_value(TIMER_GROUP_0, TIMER_0, 0);
    timer_set_alarm_value(TIMER_GROUP_0, TIMER_0, 40000000ULL / NES_AUDIO_SAMPLE_RATE);
    timer_enable_intr(TIMER_GROUP_0, TIMER_0);
    // Like SMS: keep the 15.72 kHz DAC ISR off the emulator core when possible.
    audio_isr_done = false;
    if (xTaskCreatePinnedToCore(audio_register,"nes_dac_isr",3072,nullptr,
                                4,nullptr,0) == pdPASS) {
        for (int i=0;i<250 && !audio_isr_done;++i) delay(1);
    } else {
        audio_isr_result = timer_isr_register(TIMER_GROUP_0,TIMER_0,audio_isr,
                                               nullptr,ESP_INTR_FLAG_IRAM,nullptr);
        audio_isr_done = true;
    }
    // A task uses static state: no stack pointer may be accessed after return.
    // If it is delayed, avoid a duplicate ISR registration on the other core.
    if (audio_isr_done && audio_isr_result == ESP_OK) timer_start(TIMER_GROUP_0,TIMER_0);
    else Serial.println("[NES] AVISO: audio ISR nao inicializada; DAC silencioso");
    nes_sound_enable(false);
}
static bool init_sd() {
    pinMode(SD_CS_PIN,OUTPUT); digitalWrite(SD_CS_PIN,HIGH);
    sd_spi.begin(SD_SCLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (SD.begin(SD_CS_PIN, sd_spi, SD_FREQ)) return true;
    Serial.println("[NES] Falha SD a 20 MHz; tentando 8 MHz");
    return SD.begin(SD_CS_PIN, sd_spi, 8000000);
}
// MC-1000 v0.92: the factory MultiLoader runs when otadata is erased.
// Only operate on the DATA/OTA partition (0xe000 / 0x2000): never touch
// factory (0x10000) or the emulator image in ota_0 (0x130000).
static bool prepare_loader() {
    const esp_partition_t* ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (!ota || ota->address != 0xE000 || ota->size != 0x2000) {
        Serial.println("[NES][MULTILOADER] particao otadata ausente/invalida");
        return false;
    }
    uint8_t page[256];
    bool blank = true;
    for (size_t off=0; off<ota->size; off+=sizeof(page)) {
        if (esp_partition_read(ota,off,page,sizeof(page)) != ESP_OK) return false;
        for (uint8_t byte : page) if (byte != 0xff) { blank=false; break; }
    }
    if (!blank && esp_partition_erase_range(ota,0,ota->size) != ESP_OK) return false;
    for (size_t off=0; off<ota->size; off+=sizeof(page)) {
        if (esp_partition_read(ota,off,page,sizeof(page)) != ESP_OK) return false;
        for (uint8_t byte : page) if (byte != 0xff) return false;
    }
    return true;
}
// MC-1000 v0.92: MENU REINICIAR ESP32 reinicia o chip usando RTC WDT.
// O otadata ja foi preparado em setup(); reconfira-o ANTES de desmontar SD
// e audio para deixar o menu utilizavel caso haja falha de flash.
static void clean_restart() {
    if (!prepare_loader()) {
        Serial.println("[NES][MULTILOADER] falha ao preparar otadata: reset cancelado");
        return;
    }
    // Flush da bateria antes de fechar o SD, preservando SAVEGAMES.
    nes_unload_rom();
    nes_sound_enable(false);
    vga_async_wait_idle();
    timer_pause(TIMER_GROUP_0,TIMER_0);
    timer_disable_intr(TIMER_GROUP_0,TIMER_0);
    dac_output_voltage(DAC_CHANNEL_1,128);
    SD.end(); sd_spi.end();
    pinMode(SD_CS_PIN,OUTPUT); digitalWrite(SD_CS_PIN,HIGH);
    pinMode(SD_MOSI_PIN,OUTPUT); digitalWrite(SD_MOSI_PIN,LOW);
    pinMode(SD_SCLK_PIN,OUTPUT); digitalWrite(SD_SCLK_PIN,LOW);
    Serial.println("[NES][MULTILOADER] otadata apagada; reset RTC WDT como MC-1000 v0.92");
    Serial.flush();
    // Exatamente a sequencia de reset forcado usada pelo MC-1000 v0.92.
    rtc_wdt_protect_off();
    rtc_wdt_set_stage(RTC_WDT_STAGE0, RTC_WDT_STAGE_ACTION_RESET_RTC);
    rtc_wdt_set_time(RTC_WDT_STAGE0, 100);
    rtc_wdt_enable();
    rtc_wdt_protect_on();
    while (true) { }
}
static void stats_restart() {
    fps = eps = 0; skipped_percent=0; fps_count=eps_count=0;
    fps_since=millis(); next_frame_us=esp_timer_get_time();
    last_vga_frame=vga_frame_sequence();
}
static bool start_game(const std::string& path) {
    nes_sound_enable(false);
    SMSVGADisplay::endEmulation();
    ui.showLoadingMessage("CARREGANDO JOGO NES...");
    if(!nes_load_rom(path.c_str())) {
        ui.showLoadingMessage("ERRO AO CARREGAR ROM NES");
        delay(900);
        return false;
    }
    nes_set_fullscreen(fullscreen_on);
    SMSVGADisplay::beginEmulation();
    // beginEmulation must precede audio publication; menu/loader don't play stale sound.
    nes_sound_enable(true);
    stats_restart();
    return true;
}
static uint8_t buttons_to_nes(uint8_t p) {
    uint8_t nes=0;
    if(p & SMSKeyboard::SMS_BTN1) nes|=0x01; // Z -> NES A
    if(p & SMSKeyboard::SMS_BTN2) nes|=0x02; // X -> NES B
    if(p & SMSKeyboard::SMS_PAUSE) nes|=0x04;// LEFT CTRL -> SELECT
    if(p & SMSKeyboard::SMS_START) nes|=0x08;// ENTER -> START
    if(p & SMSKeyboard::SMS_UP)    nes|=0x10;
    if(p & SMSKeyboard::SMS_DOWN)  nes|=0x20;
    if(p & SMSKeyboard::SMS_LEFT)  nes|=0x40;
    if(p & SMSKeyboard::SMS_RIGHT) nes|=0x80;
    return nes;
}
static void clear_hotkeys() {
    SMSKeyboard::takeMainMenu(); SMSKeyboard::takeLoadGames();
    SMSKeyboard::takeEscape(); SMSKeyboard::takeFpsToggle();
    SMSKeyboard::takeFrameSkipToggle(); SMSKeyboard::takeFullscreenToggle();
    SMSKeyboard::takeSpriteLimitToggle();
}
static std::string open_menu(bool immediate_browser) {
    // Checkpoint battery-backed progress before entering the menu/browser.
    nes_save_battery();
    nes_sound_enable(false);
    SMSVGADisplay::endEmulation();
    std::string selected;
    if (immediate_browser) {
        selected=ui.selectGame(true);
    } else {
        bool stay=true;
        while(stay) {
            NES_UI::MainMenuAction action=ui.mainMenu(fps_on,frameskip_auto,fullscreen_on,true);
            switch(action) {
                case NES_UI::LOAD_GAMES:
                    selected=ui.selectGame(true); stay=false; break;
                case NES_UI::TOGGLE_FPS: fps_on=!fps_on; break;
                case NES_UI::TOGGLE_SKIP: frameskip_auto=!frameskip_auto; break;
                case NES_UI::TOGGLE_FULLSCREEN: fullscreen_on=!fullscreen_on; stay=false; break;
                case NES_UI::RESET_CONSOLE: nes_reset_game(true); stay=false; break;
                case NES_UI::RESET_ESP32:
                    clean_restart(); stay=true; break;
                default: stay=false; break;
            }
        }
    }
    clear_hotkeys();
    if(selected.empty()) {
        nes_set_fullscreen(fullscreen_on);
        SMSVGADisplay::beginEmulation();
        nes_sound_enable(true);
        stats_restart();
    }
    return selected;
}

void setup() {
    Serial.begin(115200);
    Serial.println("\nNES TTGO VGA32 v1.03 / VIDEO COERENTE + QUADRO 256x240 INTEGRAL + BORDAS SUAVIZADAS (F5, INICIA DESLIGADO) + CORRECOES DE NUCLEO / MULTILOADER MC-1000 v0.92");
    Serial.printf("[NES][BOOT] motivo reset=%d; PSRAM=%s\n",
                  (int)esp_reset_reason(), psramFound()?"OK":"AUSENTE");
    setCpuFrequencyMhz(240);
    // MC-1000 v0.92 (src/main.cpp, inicio de setup): apagar otadata assim
    // que o emulador arranca de ota_0. O proximo boot executa a factory
    // (ESP-WORKS MultiLoader), inclusive apos reset espontaneo. Diferente
    // da politica experimental adotada no NES v0.1.4.
    // Esta implementacao ainda valida tipo/endereco/tamanho e verifica a
    // particao apagada, sem tocar factory, ota_0 ou a tabela de particoes.
    if (!prepare_loader()) {
        Serial.println("[NES][BOOT] ERRO FATAL: otadata nao preparada; NES nao iniciado");
        return;
    }
    Serial.println("[NES][BOOT] otadata preparada; proximo boot usa MultiLoader (factory)");
    SMSVGADisplay::init();
    if (!SMSVGADisplay::ready()) { Serial.println("[NES] ERRO VGA/PSRAM"); return; }
    if (!psramFound()) { Serial.println("[NES] Necessaria PSRAM"); return; }
    sd_ok=init_sd();
    if (!sd_ok) { Serial.println("[NES] SD ausente; insira cartao FAT32 e reinicie"); return; }
    nes_bootrc_load();
    if (g_nes_bootrc.magicb>=0) {
        // GPIO34..39 are input-only; external pull-up is required there.
        pinMode(g_nes_bootrc.magicb, g_nes_bootrc.magicb>=34 ? INPUT : INPUT_PULLUP);
    }
    SMSKeyboard::init();
    init_audio();
    SMSVGADisplay::fillScreen(UI_COLOR_BLACK);
    SMSVGADisplay::updateDisplay();
    initialized=true;
    stats_restart();
#if NES_DIAG_SERIAL
    diag_last_ms = millis();
#endif
    Serial.println("[NES][DIAG] Captura serial a 115200: procurar Watchdog, Guru Meditation ou Brownout antes do reset");
}
void loop() {
    if (!initialized) { delay(1000); return; }
    static bool game_running=false;
    static std::string next_game;
    if (!game_running) {
        if (next_game.empty()) next_game=ui.selectGame(false);
        game_running=start_game(next_game);
        next_game.clear();
        return;
    }
    const uint8_t key=SMSKeyboard::read();
    nes_set_pad(buttons_to_nes(key));
    if (SMSKeyboard::takeFpsToggle()) fps_on=!fps_on;
    if (SMSKeyboard::takeFrameSkipToggle()) frameskip_auto=!frameskip_auto;
    // v1.03: F5 liga/desliga as bordas suavizadas (sem recorte, sem custo).
    if (SMSKeyboard::takeSpriteLimitToggle()) {
        nes_set_edge_soft(!nes_get_edge_soft());
        Serial.printf("[NES] Bordas suavizadas: %s\n", nes_get_edge_soft() ? "ON" : "OFF");
    }
    if (SMSKeyboard::takeFullscreenToggle()) {
        fullscreen_on=!fullscreen_on;
        nes_sound_enable(false);
        SMSVGADisplay::endEmulation();
        nes_set_fullscreen(fullscreen_on);
        SMSVGADisplay::beginEmulation();
        nes_sound_enable(true);
        stats_restart();
    }
    if (SMSKeyboard::takeMainMenu()) {
        next_game=open_menu(false);
        if (!next_game.empty()) { nes_unload_rom(); game_running=false; }
        return;
    }
    if (SMSKeyboard::takeLoadGames() || SMSKeyboard::takeEscape()) {
        next_game=open_menu(true);
        if (!next_game.empty()) { nes_unload_rom(); game_running=false; }
        return;
    }
    const int64_t now = esp_timer_get_time();
    // v0.1.13: o NES e o framebuffer DMA devem usar o MESMO relogio de
    // quadro. A v0.1.12 usava 16667us independente do VGA (~60.05Hz),
    // deslocando lentamente a fase do desenho direto atras do feixe e
    // criando uma fronteira de flicker durante o scroll horizontal.
    // EOF do I2S: um incremento por quadro fisico; nao se perde FPS/EPS.
    const uint32_t scan_frame = vga_frame_sequence();
    bool skipped = false;
    if (scan_frame != 0u) {
        if (scan_frame == last_vga_frame) {
            // Ceder aos outros tasks enquanto aguardamos o proximo EOF.
            taskYIELD();
            delayMicroseconds(150);
            return;
        }
        skipped = frameskip_auto && ((uint32_t)(scan_frame-last_vga_frame) > 1u);
        last_vga_frame = scan_frame;
        // Commit only inside VGA vertical blank, before emulating the next
        // frame.  The 96-row staged band prevents the remaining upper-third
        // flicker without paying for a full framebuffer copy.
        vga_direct_top_present();
        // Manter a metrica de folga/yield abaixo, sem outro relogio livre.
        next_frame_us = now + 16667;
    } else {
        // Fallback temporario antes da primeira IRQ do I2S (menus/boot).
        if (next_frame_us-now > 100) {
            delayMicroseconds((uint32_t)min((int64_t)1000,next_frame_us-now));
            return;
        }
        skipped = frameskip_auto && (now-next_frame_us > 2000);
    }
    const uint32_t now_ms = millis();
    // Keep exact real-frame counting (no forced 60 and no artificial skip).
    // A one-second integer window may occasionally display 59 at 60Hz due
    // to timebase phase; preserve that diagnostic rather than hiding drops.
    if (now_ms - fps_since >= 1000) {
        const uint32_t elapsed_ms = now_ms - fps_since;
        fps=(uint16_t)((fps_count*1000UL + elapsed_ms/2)/elapsed_ms);
        eps=(uint16_t)((eps_count*1000UL + elapsed_ms/2)/elapsed_ms);
        skipped_percent=eps_count ?
           (uint8_t)(((eps_count-fps_count)*100UL + eps_count/2)/eps_count) : 0;
        fps_count=eps_count=0; fps_since=now_ms;
    }
    nes_set_overlay(fps_on,fps,eps,frameskip_auto,skipped_percent);
    const uint32_t frame_start_us = (uint32_t)esp_timer_get_time();
    nes_run_frame(skipped);
    diag_after_frame((uint32_t)esp_timer_get_time() - frame_start_us);
    ++eps_count; if(!skipped) ++fps_count;
    if (scan_frame == 0u) {
        next_frame_us += 16667;
        if (esp_timer_get_time() - next_frame_us > 200000)
            next_frame_us=esp_timer_get_time()+16667;
    }
    // v0.1.9: o tick do FreeRTOS (1 ms, ate 2 ms) era consumido em TODO
    // quadro, inclusive quando o emulador esta atrasado. Agora cede quando
    // ha folga (a espera de quadro ja descartava esse tempo) e, de qualquer
    // forma, a cada oito quadros, para Idle/Task WDT. WDT e particoes do
    // MultiLoader intactos. NES_YIELD_EVERY=1 restaura o comportamento antigo.
    {
        static uint8_t yield_phase = 0;
        const int64_t slack = next_frame_us - esp_timer_get_time();
        if (slack > 2500 || (++yield_phase % NES_YIELD_EVERY) == 0) vTaskDelay(1);
    }
}
