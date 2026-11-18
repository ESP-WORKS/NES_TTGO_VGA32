/* Based on the optional bootl.rc parser from the user's MC-1000 v0.92.
 * Accepts /bootl.rc in any letter case; values kbddat, kbdclk, magicb.
 */
#include "nes_bootrc.h"
#include "sms_sd_case.h"
#include <Arduino.h>
#include <SD.h>
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include <string>
NESBootRC g_nes_bootrc;
static bool valid_ps2(int g) {
    switch(g) {
        case 0: case 4: case 15: case 26: case 27: case 32: case 33: return true;
        default: return false;
    }
}
static bool valid_input(int g) {
    if(g<0 || g>39 || (g>=6 && g<=11) || g==20 || g==24 || (g>=28 && g<=31)) return false;
    // Reserve VGA and SD/DAC pins, as does the approved MC-1000 reference.
    if(g==2 || g==12 || g==13 || g==14 || g==25) return false;
    return true;
}
static void trim(std::string& s) {
    size_t a=s.find_first_not_of(" \t\r\n");
    if(a==std::string::npos) {s.clear();return;}
    size_t b=s.find_last_not_of(" \t\r\n");
    s=s.substr(a,b+1-a);
}
void nes_bootrc_load() {
    g_nes_bootrc=NESBootRC();
    std::string path;
    if(!SMSSDCase::resolveFile("/bootl.rc",path)) {
        Serial.println("[NES][BOOTRC] bootl.rc ausente: PS2 padrao 32/33");
        return;
    }
    File f=SD.open(path.c_str(),FILE_READ);
    if(!f) return;
    NESBootRC rc;
    unsigned lines=0;
    while(f.available() && lines++<80) {
        String raw=f.readStringUntil('\n');
        std::string line=raw.c_str(); trim(line);
        if(line.empty() || line[0]=='#' || line[0]==';' || line.compare(0,2,"//")==0) continue;
        size_t sep=line.find_first_of("=:\t ");
        if(sep==std::string::npos) continue;
        std::string key=line.substr(0,sep);
        std::string val=line.substr(sep+1);trim(key);trim(val);
        // Accept all three MC-1000 forms: k=v, k: v and k : v.
        while (!val.empty() && (val[0]=='=' || val[0]==':')) {
            val.erase(0,1); trim(val);
        }
        char *end=nullptr;
        long number=strtol(val.c_str(),&end,10);
        if(end==val.c_str() || number < -1 || number > 39) continue;
        if(!strcasecmp(key.c_str(),"kbddat")) rc.kbddat=(int)number;
        else if(!strcasecmp(key.c_str(),"kbdclk")) rc.kbdclk=(int)number;
        else if(!strcasecmp(key.c_str(),"magicb")) rc.magicb=(int)number;
    }
    f.close();
    if(!valid_ps2(rc.kbddat) || !valid_ps2(rc.kbdclk) || rc.kbddat==rc.kbdclk) {
        Serial.println("[NES][BOOTRC] PS2 invalido: usando 32/33");
        rc.kbddat=32;rc.kbdclk=33;
    }
    if(rc.magicb >= 0 && (!valid_input(rc.magicb) || rc.magicb==rc.kbddat || rc.magicb==rc.kbdclk)) {
        Serial.println("[NES][BOOTRC] magicb invalido: desativado");
        rc.magicb=-1;
    }
    rc.loaded=true;
    g_nes_bootrc=rc;
    Serial.printf("[NES][BOOTRC] kbddat=%d kbdclk=%d magicb=%d\n",rc.kbddat,rc.kbdclk,rc.magicb);
}
