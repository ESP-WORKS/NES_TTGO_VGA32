/* Battery SRAM persistence on FAT32; behavior informed by Mesen BatteryManager.
 * The implementation is original.  Save data is uncompressed RAW 8KiB .sav.
 * Writes: verified .tmp -> .sav with previous snapshot kept as .bak. */
#include <Arduino.h>
#include <SD.h>
#include <stdint.h>
#include <string>
#include <cstring>
#include "nes_battery.h"
#include "sms_sd_case.h"

namespace {
constexpr size_t SAVE_SIZE = 8192;
constexpr uint32_t SAVE_INTERVAL_MS = 20000;
// Guard against SD latency/contention during active VGA emulation.
// Saves are ALWAYS flushed on menu, ROM change and MultiLoader exit.
#ifndef NES_BATTERY_AUTOSAVE_IN_GAME
#define NES_BATTERY_AUTOSAVE_IN_GAME 1
#endif
uint8_t* active_sram = nullptr;
size_t active_length = 0;
std::string save_path;
std::string legacy_path;
bool supported = false;
uint64_t last_hash = 0;
uint32_t last_check_ms = 0;

uint64_t fingerprint(const uint8_t* p, size_t n) {
    uint64_t h = UINT64_C(14695981039346656037);
    for(size_t i=0; i<n; ++i) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}

bool file_read_exact(const std::string& path, uint8_t* target, size_t len) {
    File file = SD.open(path.c_str(), FILE_READ);
    if(!file) return false;
    const bool correct_size = file.size() == len;
    size_t n = correct_size ? file.read(target, len) : 0;
    file.close();
    return correct_size && n == len;
}

bool file_hash_exact(const std::string& path, size_t len, uint64_t wanted) {
    File file = SD.open(path.c_str(), FILE_READ);
    if(!file) return false;
    if(file.size() != len) {file.close(); return false;}
    uint8_t buf[128];
    uint64_t h = UINT64_C(14695981039346656037);
    size_t remain = len;
    while(remain) {
        const size_t amount = remain > sizeof(buf) ? sizeof(buf) : remain;
        if(file.read(buf, amount) != amount) {file.close();return false;}
        for(size_t i=0;i<amount;i++) { h ^= buf[i]; h *= UINT64_C(1099511628211); }
        remain -= amount;
    }
    file.close();return h == wanted;
}

std::string existing_file(const std::string& desired) {
    std::string resolved;
    return SMSSDCase::resolveFile(desired, resolved) ? resolved : std::string();
}

bool load_one(const std::string& desired) {
    const std::string path=existing_file(desired);
    if(path.empty()) return false;
    return file_read_exact(path, active_sram, active_length);
}

// A basename corresponds to Mesen's per-ROM battery naming; a ROM of the
// same name in two different directories shares save data (documented).
std::string make_name(const char* rom) {
    std::string name = SMSSDCase::basename(rom);
    const size_t dot = name.find_last_of('.');
    if(dot != std::string::npos && dot > 0) name.resize(dot);
    if(name.empty() || name=="." || name=="..") name="game";
    return name + ".sav";
}
}

void nes_battery_end(void) {
    active_sram=nullptr; active_length=0; save_path.clear(); legacy_path.clear();
    supported=false;last_hash=0;last_check_ms=0;
}

void nes_battery_begin(const char* rom_path,uint8_t* sram,size_t len,bool battery) {
    nes_battery_end();
    if(!battery || !sram || len!=SAVE_SIZE) return;
    std::string dir;
    if(!SMSSDCase::ensureDirectory("/NES/SAVEGAMES",dir)) {
        Serial.println("[NES][SAVE] Falha criando /NES/SAVEGAMES");
        return;
    }
    save_path = dir + "/" + make_name(rom_path);
    // Backward-compatible one-time copy from v0.1.1, whose .sav lived
    // next to the ROM. Do not delete the legacy original on migration.
    std::string rom = rom_path ? rom_path : "";
    const size_t cut = rom.find_last_of('.');
    const size_t slash = rom.find_last_of("/\\");
    if(cut != std::string::npos && (slash == std::string::npos || cut > slash)) rom.resize(cut);
    legacy_path = rom + ".sav";
    active_sram=sram;active_length=len;supported=true;
    last_hash=fingerprint(active_sram,active_length);
    last_check_ms=millis();
    Serial.printf("[NES][SAVE] Pasta %s\n",dir.c_str());
}

void nes_battery_load(void) {
    if(!supported) return;
    bool migrated = false;
    if(load_one(save_path)) {
        Serial.printf("[NES][SAVE] SRAM carregada: %s (%u bytes)\n",save_path.c_str(),(unsigned)active_length);
    } else if(load_one(save_path+".bak")) {
        Serial.println("[NES][SAVE] Recuperacao SRAM .bak utilizada");
    } else if(!legacy_path.empty() && load_one(legacy_path)) {
        migrated=true;
        Serial.println("[NES][SAVE] Save antigo encontrado: copiando para /NES/SAVEGAMES");
    } else {
        Serial.println("[NES][SAVE] Sem save valido: mantendo RAM inicial do cartucho");
    }
    last_hash=fingerprint(active_sram,active_length);
    if(migrated) {
        last_hash=~last_hash; // forces first central backup for legacy data
        (void)nes_battery_save(true);
    }
    last_check_ms=millis();
}

bool nes_battery_save(bool force) {
    if(!supported) return true;
    const uint64_t new_hash=fingerprint(active_sram,active_length);
    if(!force && new_hash==last_hash) return true;
    // An unchanged forced save also needs no write: saves flash/SD wear.
    if(new_hash==last_hash) return true;

    const std::string temp=save_path+".tmp", backup=save_path+".bak";
    std::string stale=existing_file(temp);
    if(!stale.empty() && !SD.remove(stale.c_str())) {
        Serial.println("[NES][SAVE] Impossivel remover temporario antigo");return false;
    }
    File file=SD.open(temp.c_str(), "w"); // "w" truncates; FILE_WRITE would append
    if(!file) {Serial.println("[NES][SAVE] Impossivel criar temporario");return false;}
    const size_t written=file.write(active_sram,active_length);
    file.flush();file.close();
    if(written!=active_length || !file_hash_exact(temp,active_length,new_hash)) {
        Serial.println("[NES][SAVE] Escrita nao validada; .sav anterior preservado");
        SD.remove(temp.c_str());return false;
    }

    const std::string current=existing_file(save_path);
    if(!current.empty()) {
        // Rotate only a correctly sized current .sav. Keep any earlier good
        // .bak when the primary was already corrupt/truncated.
        File old=SD.open(current.c_str(),FILE_READ);
        const bool valid=old && old.size()==active_length;
        if(old) old.close();
        if(valid) {
            const std::string older_backup=existing_file(backup);
            if(!older_backup.empty() && !SD.remove(older_backup.c_str())) {
                Serial.println("[NES][SAVE] Falha removendo backup anterior");return false;
            }
            if(!SD.rename(current.c_str(),backup.c_str())) {
                Serial.println("[NES][SAVE] Falha rotacionando save anterior");return false;
            }
        } else if(!SD.remove(current.c_str())) {
            Serial.println("[NES][SAVE] Save antigo invalido nao removivel");return false;
        }
    }
    if(!SD.rename(temp.c_str(),save_path.c_str())) {
        // Keep .bak untouched. On next load it is used if the primary
        // does not exist, even if a power loss occurs at this point.
        Serial.println("[NES][SAVE] Falha aplicando novo save; backup preservado");
        return false;
    }
    last_hash=new_hash;
    Serial.printf("[NES][SAVE] Salvo %s (%u bytes)\n",save_path.c_str(),(unsigned)active_length);
    return true;
}
void nes_battery_poll(void) {
#if !NES_BATTERY_AUTOSAVE_IN_GAME
    return;
#endif
    if(!supported) return;
    const uint32_t now=millis();
    if((uint32_t)(now-last_check_ms)<SAVE_INTERVAL_MS) return;
    last_check_ms=now;
    nes_battery_save(false);
}
