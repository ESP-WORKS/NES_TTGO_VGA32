/* NES TTGO VGA32 v0.1.1 - new mapper adapter for Nofrendo.
 * SPDX-License-Identifier: LGPL-2.0-or-later (compatible with Nofrendo core)
 * Derived from public NESdev hardware documentation; no third-party code copied.
 */
#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "nes.h"
#include "libsnss.h"

/* Namco 118 / DxROM: fixed PRG/CHR mode, no MMC3 IRQ or mirroring writes. */
static uint8 select_reg, banks[8];
static void map206_set(int which) {
    uint8 v=banks[which];
    switch(which) {
    case 0: mmc_bankvrom(2,0,v>>1);break;
    case 1: mmc_bankvrom(2,0x800,v>>1);break;
    case 2:case 3:case 4:case 5:mmc_bankvrom(1,0x1000+(which-2)*0x400,v);break;
    case 6:mmc_bankrom(8,0x8000,v&0x3F);break;
    case 7:mmc_bankrom(8,0xA000,v&0x3F);break;
    }
}
static void map206_init(void) {
    int i;select_reg=0;for(i=0;i<8;++i) banks[i]=0;
    mmc_bankrom(8,0xC000,(mmc_getinfo()->rom_banks*2)-2);
    mmc_bankrom(8,0xE000,MMC_LASTBANK);
    mmc_bankrom(8,0x8000,0);mmc_bankrom(8,0xA000,1);
    for (i=0;i<6;++i) map206_set(i);
}
static void map206_write(uint32 addr,uint8 v) {
    if ((addr&0xE001)==0x8000) select_reg=v&7;
    else if ((addr&0xE001)==0x8001) { banks[select_reg]=v; map206_set(select_reg); }
}
static const map_memwrite wr[]={{0x8000,0x9FFF,map206_write},{(uint32)-1,(uint32)-1,NULL}};
const mapintf_t map206_intf={206,"Namco 118 / DxROM",map206_init,NULL,NULL,NULL,NULL,NULL,wr,NULL};
