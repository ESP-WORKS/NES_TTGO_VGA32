/* NES TTGO VGA32 v0.1.1 - new mapper adapter for Nofrendo.
 * SPDX-License-Identifier: LGPL-2.0-or-later (compatible with Nofrendo core)
 * Derived from public NESdev hardware documentation; no third-party code copied.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"

/* Sunsoft FME-7/5A basic banking. IRQ approximate: hblank granularity;
 * Sunsoft 5B expansion audio and cycle-exact IRQ are not implemented. */
static uint8 cmd, irq_control;
static uint16 irq_counter;
static void map69_init(void) {
    cmd=irq_control=0; irq_counter=0;
    mmc_bankrom(8,0xE000,MMC_LASTBANK);
}
static void map69_write(uint32 addr, uint8 v) {
    if (addr<0xA000) { cmd=v&15; return; }
    if (addr>=0xC000) return; // 5B expansion audio registers (unsupported)
    if (cmd<8) { mmc_bankvrom(1,(uint32)cmd<<10,v); return; }
    if (cmd==8) {
        // FME7: $6000 selects PRG-ROM only if RAM is not selected.
        if ((v&0x40)==0) mmc_bankrom(8,0x6000,v&0x3F);
        return;
    }
    if (cmd<=11) { mmc_bankrom(8,0x8000+(cmd-9)*0x2000,v&0x3F); return; }
    if (cmd==12) {
        switch(v&3) {
        case 0:ppu_mirror(0,0,1,1);break;
        case 1:ppu_mirror(0,1,0,1);break;
        case 2:ppu_mirror(0,0,0,0);break;
        case 3:ppu_mirror(1,1,1,1);break;
        }
        return;
    }
    if (cmd==13) {irq_control=v&0x81;return;}
    if (cmd==14) {irq_counter=(irq_counter&0xFF00)|v;return;}
    if (cmd==15) {irq_counter=(irq_counter&0x00FF)|((uint16)v<<8);return;}
}
static void map69_hblank(int vblank) {
    UNUSED(vblank);
    if (!(irq_control & 0x80)) return;
    uint16 old=irq_counter;
    irq_counter=(uint16)(irq_counter-114);
    if ((irq_control&1) && old<114) nes_irq();
}
static const map_memwrite wr[]={{0x8000,0xBFFF,map69_write},{(uint32)-1,(uint32)-1,NULL}};
const mapintf_t map69_intf={69,"FME7 (partial IRQ/audio)",map69_init,NULL,map69_hblank,NULL,NULL,NULL,wr,NULL};
